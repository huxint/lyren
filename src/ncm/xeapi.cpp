#include "ncm/xeapi.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <zlib.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>

#include "core/encoding.hpp"

namespace lyren::ncm {

    namespace {
        constexpr std::array<unsigned char, 32> transport_key{0xab, 0x1d, 0x5a, 0x43, 0x0f, 0x6b, 0xb0, 0x4a, 0x3f, 0x01, 0xe8, 0x1d, 0xdd, 0x72, 0xbd, 0x91,
                                                              0x6d, 0x5c, 0xe5, 0x91, 0x24, 0x8a, 0xc1, 0x28, 0x71, 0x48, 0x06, 0xd7, 0xf8, 0xfb, 0x1b, 0x84};
        constexpr std::string_view sign_key = "mUHCwVNWJbunMqAHf5MImuirT6plvs6VSFW62MGHstFQxhBGdEoIhLItH3djc4+FB/OKty3+lL2rGeoFBpVe5g==";

        std::string_view bytes(const auto &buffer) {
            return {reinterpret_cast<const char *>(buffer.data()), buffer.size()};
        }
        auto cipher_context() {
            return std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
        }
        auto hmac(std::string_view key, std::string_view input) {
            std::array<unsigned char, 32> out;
            HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(input.data()), input.size(), out.data(), nullptr);
            return out;
        }
        template <size_t N>
        auto random_bytes() {
            std::array<unsigned char, N> out;
            RAND_bytes(out.data(), static_cast<int>(out.size()));
            return out;
        }
        std::string encrypt(std::string_view key, std::string_view input) {
            auto ctx = cipher_context();
            EVP_EncryptInit_ex(ctx.get(), key.size() == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb(), nullptr, reinterpret_cast<const unsigned char *>(key.data()), nullptr);
            std::string out(input.size() + 16, '\0');
            int n = 0, tail = 0;
            auto *dst = reinterpret_cast<unsigned char *>(out.data());
            EVP_EncryptUpdate(ctx.get(), dst, &n, reinterpret_cast<const unsigned char *>(input.data()), static_cast<int>(input.size()));
            EVP_EncryptFinal_ex(ctx.get(), dst + n, &tail);
            out.resize(static_cast<size_t>(n + tail));
            return out;
        }
        Result<std::string> decrypt(std::string_view key, std::string_view input) {
            if (input.empty() || input.size() % 16 != 0) {
                return std::unexpected("NetEase Music returned an invalid encrypted response");
            }
            auto ctx = cipher_context();
            EVP_DecryptInit_ex(ctx.get(), key.size() == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb(), nullptr, reinterpret_cast<const unsigned char *>(key.data()), nullptr);
            std::string out(input.size(), '\0');
            int n = 0, tail = 0;
            auto *dst = reinterpret_cast<unsigned char *>(out.data());
            EVP_DecryptUpdate(ctx.get(), dst, &n, reinterpret_cast<const unsigned char *>(input.data()), static_cast<int>(input.size()));
            if (EVP_DecryptFinal_ex(ctx.get(), dst + n, &tail) != 1) {
                return std::unexpected("NetEase Music returned an invalid encrypted response");
            }
            out.resize(static_cast<size_t>(n + tail));
            return out;
        }
        std::string scalar(const glz::raw_json &raw) {
            if (!raw.str.starts_with('"')) {
                return raw.str;
            }
            std::string value;
            static_cast<void>(glz::read_json(value, raw.str));
            return value;
        }
        struct KeyData {
            std::string encryptedData, signature;
            glz::raw_json timestamp;
        };
        struct KeyReply {
            int code = 0;
            std::optional<KeyData> data;
        };
        struct KeyPlaintext {
            std::string publicKey, sk;
            glz::raw_json version;
        };
        struct RequestPlaintext {
            std::string_view content, queryString;
        };
    } // namespace

    std::string xeapi_signature(std::string_view timestamp, std::string_view nonce) {
        return encoding::base64(bytes(hmac(sign_key, std::string(timestamp) + std::string(nonce))));
    }

    Result<XeapiKey> parse_xeapi_key(std::string_view response, std::string_view nonce) {
        KeyReply reply;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(reply, response) || reply.code != 200 || !reply.data) {
            return std::unexpected("NetEase Music encryption key request failed");
        }
        if (reply.data->signature != xeapi_signature(scalar(reply.data->timestamp), nonce)) {
            return std::unexpected("NetEase Music encryption key signature mismatch");
        }
        auto plain = decrypt(bytes(transport_key), encoding::unbase64(reply.data->encryptedData));
        if (!plain) {
            return std::unexpected(plain.error());
        }
        KeyPlaintext key;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(key, *plain)) {
            return std::unexpected("NetEase Music returned an invalid encryption key");
        }
        XeapiKey out{encoding::unbase64(key.publicKey), scalar(key.version), std::move(key.sk)};
        if (out.public_key.size() != 32 || out.version.empty() || out.tag.empty()) {
            return std::unexpected("NetEase Music returned an invalid encryption key");
        }
        return out;
    }

    Result<XeapiBody> xeapi_encrypt(std::string_view content, const XeapiKey &key) {
        const auto dynamic = random_bytes<16>(), mask = random_bytes<16>();
        const auto secret = random_bytes<32>();
        const auto nonce = random_bytes<12>();
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> local(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, secret.data(), secret.size()), EVP_PKEY_free);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> peer(
            EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char *>(key.public_key.data()), key.public_key.size()), EVP_PKEY_free);
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> agreement(EVP_PKEY_CTX_new(local.get(), nullptr), EVP_PKEY_CTX_free);
        std::array<unsigned char, 32> shared{}, public_key{};
        size_t n = shared.size(), pub_size = public_key.size();
        EVP_PKEY_get_raw_public_key(local.get(), public_key.data(), &pub_size);
        EVP_PKEY_derive_init(agreement.get());
        EVP_PKEY_derive_set_peer(agreement.get(), peer.get());
        if (EVP_PKEY_derive(agreement.get(), shared.data(), &n) != 1) {
            return std::unexpected("NetEase Music returned an invalid encryption key");
        }
        const std::array<unsigned char, 32> salt{};
        const auto prk = hmac(bytes(salt), bytes(shared));
        const auto derived = hmac(bytes(prk), std::string(bytes(public_key)) + '\1');

        // Wrap the request key with X25519/HKDF and AES-GCM.
        auto ctx = cipher_context();
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, derived.data(), nonce.data());
        const std::string envelope = encoding::base64(bytes(dynamic)) + "|android|" + key.tag;
        std::string wrapped(envelope.size() + 16, '\0');
        auto *dst = reinterpret_cast<unsigned char *>(wrapped.data());
        int size = 0, tail = 0;
        EVP_EncryptUpdate(ctx.get(), dst, &size, reinterpret_cast<const unsigned char *>(envelope.data()), static_cast<int>(envelope.size()));
        EVP_EncryptFinal_ex(ctx.get(), dst + size, &tail);
        wrapped.resize(static_cast<size_t>(size + tail));
        std::array<unsigned char, 16> tag;
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data());
        const std::string s = std::string(bytes(public_key)) + std::string(bytes(nonce)) + wrapped + std::string(bytes(tag));

        const std::string plain = glz::write_json(RequestPlaintext{content, "e_r=true"}).value();
        std::string inner = encrypt(bytes(transport_key), plain);
        for (size_t i = 0; i < inner.size(); ++i) {
            inner[i] ^= static_cast<char>(mask[i % mask.size()]);
        }
        std::ranges::rotate(inner, inner.begin() + static_cast<std::ptrdiff_t>(mask[0] & 15));
        std::string c = encoding::base64(encrypt(bytes(dynamic), std::string(bytes(mask)) + inner));
        std::ranges::replace(c, '+', '-');
        std::ranges::replace(c, '/', '_');
        while (c.ends_with('=')) {
            c.pop_back();
        }
        return XeapiBody{std::move(c), encoding::base64(s), encoding::base64(encrypt(bytes(transport_key), key.version + '|'))};
    }

    Result<std::string> xeapi_response(std::string_view ciphertext) {
        auto plain = decrypt("e82ckenh8dichen8", ciphertext);
        if (!plain || !plain->starts_with("\x1f\x8b")) {
            return plain;
        }
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(plain->data());
        stream.avail_in = static_cast<uInt>(plain->size());
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
            return std::unexpected("Could not decompress the NetEase Music response");
        }
        std::string result;
        std::array<char, 16384> buffer;
        int status;
        do {
            stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            status = inflate(&stream, Z_NO_FLUSH);
            result.append(buffer.data(), buffer.size() - stream.avail_out);
        } while (status == Z_OK);
        inflateEnd(&stream);
        if (status != Z_STREAM_END) {
            return std::unexpected("NetEase Music returned an invalid compressed response");
        }
        return result;
    }

} // namespace lyren::ncm
