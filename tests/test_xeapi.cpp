#include <doctest/doctest.h>
#include <glaze/glaze.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>

#include "core/encoding.hpp"
#include "ncm/xeapi.hpp"

using namespace lyren;

namespace {
    struct Fixture {
        std::string nonce, signature;
        glz::raw_json registration;
        std::string plaintext, response, gzip_response, invalid_gzip, server_private_key, transport_key;
    };
    const Fixture &fixture() {
        static const Fixture f = [] {
            constexpr unsigned char json[]{
#embed "data/ncm-xeapi.json" suffix(, 0)
            };
            Fixture out;
            REQUIRE_FALSE(glz::read_json(out, std::string_view{reinterpret_cast<const char *>(json), sizeof(json) - 1}));
            return out;
        }();
        return f;
    }
    auto context() {
        return std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
    }
    std::string hash(std::string_view key, std::string_view message) {
        std::string out(32, '\0');
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(message.data()), message.size(),
             reinterpret_cast<unsigned char *>(out.data()), nullptr);
        return out;
    }
    std::string ecb(std::string_view key, std::string_view cipher) {
        auto ctx = context();
        EVP_DecryptInit_ex(ctx.get(), key.size() == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb(), nullptr, reinterpret_cast<const unsigned char *>(key.data()), nullptr);
        std::string plain(cipher.size(), '\0');
        int size = 0, tail = 0;
        auto *dst = reinterpret_cast<unsigned char *>(plain.data());
        REQUIRE(EVP_DecryptUpdate(ctx.get(), dst, &size, reinterpret_cast<const unsigned char *>(cipher.data()), static_cast<int>(cipher.size())) == 1);
        REQUIRE(EVP_DecryptFinal_ex(ctx.get(), dst + size, &tail) == 1);
        plain.resize(static_cast<size_t>(size + tail));
        return plain;
    }
} // namespace

TEST_CASE("xeapi authenticates the server key registration before using it") {
    const auto &f = fixture();
    CHECK(ncm::xeapi_signature("1700000000123", f.nonce) == f.signature);
    auto key = ncm::parse_xeapi_key(f.registration.str, f.nonce);
    REQUIRE(key);
    CHECK(key->public_key.size() == 32);
    CHECK(key->version == "1000000000000");
    CHECK(key->tag == "fixture-session");
    CHECK_FALSE(ncm::parse_xeapi_key(f.registration.str, "different nonce"));
    CHECK_FALSE(ncm::parse_xeapi_key(R"({"code":200,"data":{}})", f.nonce));
    CHECK_FALSE(ncm::parse_xeapi_key(R"({"code":503})", f.nonce));
}

TEST_CASE("xeapi reads encrypted JSON and gzip responses and rejects broken envelopes") {
    const auto &f = fixture();
    for (const auto &encoded : {f.response, f.gzip_response}) {
        const std::string cipher = encoding::unbase64(encoded);
        auto response = ncm::xeapi_response(cipher);
        REQUIRE(response);
        CHECK(*response == f.plaintext);
        CHECK_FALSE(ncm::xeapi_response(std::string_view(cipher).substr(1)));
    }
    CHECK_FALSE(ncm::xeapi_response(encoding::unbase64(f.invalid_gzip)));
    CHECK_FALSE(ncm::xeapi_response(""));
}

TEST_CASE("a server can authenticate and decrypt an independently negotiated xeapi request") {
    const auto &f = fixture();
    const auto key = ncm::parse_xeapi_key(f.registration.str, f.nonce);
    REQUIRE(key);
    constexpr std::string_view content = "ids=%5B1%5D&level=lossless&encodeType=flac";
    const auto packet = ncm::xeapi_encrypt(content, *key);
    REQUIRE(packet);

    // RFC 7748's Bob key plays the server. Derive the peer's secret from S, then authenticate its GCM envelope.
    const std::string wrapped = encoding::unbase64(packet->s), private_key = encoding::unbase64(f.server_private_key);
    REQUIRE(wrapped.size() > 60);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> server(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(private_key.data()), private_key.size()), EVP_PKEY_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(wrapped.data()), 32),
                                                             EVP_PKEY_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> agreement(EVP_PKEY_CTX_new(server.get(), nullptr), EVP_PKEY_CTX_free);
    std::string shared(32, '\0');
    size_t size = shared.size();
    EVP_PKEY_derive_init(agreement.get());
    EVP_PKEY_derive_set_peer(agreement.get(), peer.get());
    REQUIRE(EVP_PKEY_derive(agreement.get(), reinterpret_cast<unsigned char *>(shared.data()), &size) == 1);
    const auto wrapping_key = hash(hash(std::string(32, '\0'), shared), wrapped.substr(0, 32) + '\1');
    auto ctx = context();
    EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, reinterpret_cast<const unsigned char *>(wrapping_key.data()),
                       reinterpret_cast<const unsigned char *>(wrapped.data() + 32));
    std::string envelope(wrapped.size(), '\0');
    int n = 0, tail = 0;
    EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(envelope.data()), &n, reinterpret_cast<const unsigned char *>(wrapped.data() + 44),
                      static_cast<int>(wrapped.size() - 60));
    EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<char *>(wrapped.data() + wrapped.size() - 16));
    REQUIRE(EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(envelope.data()) + n, &tail) == 1);
    envelope.resize(static_cast<size_t>(n + tail));
    const auto separator = envelope.find('|');
    REQUIRE(separator != envelope.npos);
    CHECK(envelope.substr(separator) == "|android|fixture-session");
    const auto dynamic_key = encoding::unbase64(envelope.substr(0, separator));
    REQUIRE(dynamic_key.size() == 16);

    std::string c = packet->c;
    std::ranges::replace(c, '-', '+');
    std::ranges::replace(c, '_', '/');
    while (c.size() % 4 != 0) {
        c += '=';
    }
    const auto outer = ecb(dynamic_key, encoding::unbase64(c));
    REQUIRE(outer.size() >= 32);
    std::string inner = outer.substr(16);
    const auto rotation = static_cast<unsigned char>(outer[0]) & 15;
    std::ranges::rotate(inner, inner.end() - rotation);
    for (size_t i = 0; i < inner.size(); ++i) {
        inner[i] ^= outer[i % 16];
    }
    const auto transport_key = encoding::unbase64(f.transport_key);
    const auto plaintext = ecb(transport_key, inner);
    std::map<std::string, std::string> fields;
    REQUIRE_FALSE(glz::read_json(fields, plaintext));
    CHECK(fields.at("content") == content);
    CHECK(fields.at("queryString") == "e_r=true");
    CHECK(ecb(transport_key, encoding::unbase64(packet->r)) == "1000000000000|");
}
