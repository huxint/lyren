#include "ncm/crypto.hpp"

#include <openssl/bn.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <memory>
#include <format>

#include "core/crypto.hpp"
#include "core/encoding.hpp"

namespace lyren::ncm {

    static constexpr std::string_view iv = "0102030405060708";
    static constexpr std::string_view preset_key = "0CoJUm6Qyw8W8jud";
    static constexpr std::string_view eapi_key = "e82ckenh8dichen8";
    static constexpr char modulus[] = "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda9"
                                      "2557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";

    std::string eapi(std::string_view url, std::string_view text) {
        std::string message = std::format("nobody{}use{}md5forencrypt", url, text);
        std::string data = std::format("{}-36cd479b6b5-{}-36cd479b6b5-{}", url, text, md5_hex(message));
        return encoding::hex(crypto::aes_ecb_encrypt(eapi_key, data), true);
    }

    // Unpadded RSA with the web client's public key, as 256 hex digits.
    static std::string rsa_hex(std::string_view data) {
        using Number = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
        std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
        BIGNUM *mod = nullptr;
        BN_hex2bn(&mod, modulus);
        Number n(mod, BN_free), exponent(BN_new(), BN_free), encrypted(BN_new(), BN_free);
        Number message(BN_bin2bn(reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()), nullptr), BN_free);
        BN_set_word(exponent.get(), 65537);
        BN_mod_exp(encrypted.get(), message.get(), exponent.get(), n.get(), ctx.get());
        std::array<unsigned char, 128> bytes{};
        BN_bn2binpad(encrypted.get(), bytes.data(), bytes.size());
        return encoding::hex({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    }

    Weapi weapi(std::string_view text, std::string_view secret) {
        std::string key(secret);
        std::string first = encoding::base64(crypto::aes_cbc_encrypt(preset_key, iv, text));
        std::string params = encoding::base64(crypto::aes_cbc_encrypt(key, iv, first));
        std::ranges::reverse(key);
        return {std::move(params), rsa_hex(key)};
    }

    Weapi weapi(std::string_view text) {
        static constexpr std::string_view base62 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::array<unsigned char, 16> r;
        RAND_bytes(r.data(), static_cast<int>(r.size()));
        std::string key(r.size(), '\0');
        for (size_t i = 0; i < r.size(); ++i) {
            key[i] = base62[r[i] % base62.size()];
        }
        return weapi(text, key);
    }

} // namespace lyren::ncm
