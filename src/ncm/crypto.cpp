#include "ncm/crypto.hpp"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>

namespace lyren::ncm {

    static constexpr char iv[] = "0102030405060708";
    static constexpr char preset_key[] = "0CoJUm6Qyw8W8jud";
    static constexpr char eapi_key[] = "e82ckenh8dichen8";
    static constexpr char modulus[] = "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda9"
                                      "2557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";

    std::string hex(std::string_view bytes, bool upper) {
        const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string s;
        s.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) {
            s += digits[c >> 4], s += digits[c & 15];
        }
        return s;
    }

    std::string md5_hex(std::string_view s) {
        unsigned char d[16];
        unsigned n = 0;
        EVP_Digest(s.data(), s.size(), d, &n, EVP_md5(), nullptr);
        return hex({reinterpret_cast<char *>(d), n});
    }

    std::string random_hex(size_t digits, bool upper) {
        std::string b((digits + 1) / 2, '\0');
        RAND_bytes(reinterpret_cast<unsigned char *>(b.data()), int(b.size()));
        return hex(b, upper).substr(0, digits);
    }

    std::string base64(std::string_view bytes) {
        std::string out(4 * ((bytes.size() + 2) / 3), '\0');
        int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), reinterpret_cast<const unsigned char *>(bytes.data()), int(bytes.size()));
        out.resize(size_t(n));
        return out;
    }

    std::string unbase64(std::string_view text) {
        std::string out(3 * (text.size() / 4), '\0');
        int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()), reinterpret_cast<const unsigned char *>(text.data()), int(text.size()));
        if (n < 0) {
            return {};
        }
        // EVP_DecodeBlock keeps the bytes that padding stands for.
        size_t pad = text.ends_with("==") ? 2 : text.ends_with('=') ? 1 : 0;
        out.resize(size_t(n) - pad);
        return out;
    }

    static std::string aes(const EVP_CIPHER *cipher, std::string_view data, const char *key, const char *ivec) {
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        EVP_EncryptInit_ex(c.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key), reinterpret_cast<const unsigned char *>(ivec));
        std::string out(data.size() + 16, '\0');
        int n1 = 0, n2 = 0;
        auto *o = reinterpret_cast<unsigned char *>(out.data());
        EVP_EncryptUpdate(c.get(), o, &n1, reinterpret_cast<const unsigned char *>(data.data()), int(data.size()));
        EVP_EncryptFinal_ex(c.get(), o + n1, &n2);
        out.resize(size_t(n1 + n2));
        return out;
    }

    std::string eapi(std::string_view url, std::string_view text) {
        std::string message = std::format("nobody{}use{}md5forencrypt", url, text);
        std::string data = std::format("{}-36cd479b6b5-{}-36cd479b6b5-{}", url, text, md5_hex(message));
        return hex(aes(EVP_aes_128_ecb(), data, eapi_key, nullptr), true);
    }

    std::string rsa_hex(std::string_view data) {
        BN_CTX *ctx = BN_CTX_new();
        BIGNUM *n = nullptr, *e = BN_new(), *m = BN_bin2bn(reinterpret_cast<const unsigned char *>(data.data()), int(data.size()), nullptr), *c = BN_new();
        BN_hex2bn(&n, modulus);
        BN_set_word(e, 65537);
        BN_mod_exp(c, m, e, n, ctx);
        char *h = BN_bn2hex(c);
        std::string s(h);
        OPENSSL_free(h);
        BN_free(n), BN_free(e), BN_free(m), BN_free(c);
        BN_CTX_free(ctx);
        std::ranges::transform(s, s.begin(), [](unsigned char ch) {
            return char(std::tolower(ch));
        });
        return std::string(256 - std::min<size_t>(256, s.size()), '0') + s;
    }

    Weapi weapi(std::string_view text, std::string_view secret) {
        std::string key(secret);
        std::string first = base64(aes(EVP_aes_128_cbc(), text, preset_key, iv));
        std::string params = base64(aes(EVP_aes_128_cbc(), first, key.c_str(), iv));
        std::ranges::reverse(key);
        return {std::move(params), rsa_hex(key)};
    }

    Weapi weapi(std::string_view text) {
        static constexpr char base62[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        unsigned char r[16];
        RAND_bytes(r, sizeof r);
        std::string key(16, '\0');
        for (size_t i = 0; i < 16; ++i) {
            key[i] = base62[r[i] % 62];
        }
        return weapi(text, key);
    }

} // namespace lyren::ncm
