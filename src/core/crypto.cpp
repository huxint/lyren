#include "core/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <memory>

#include "core/encoding.hpp"

namespace lyren::crypto {

    namespace {
        using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

        const EVP_CIPHER *aes_cipher(size_t key_size, bool cbc) {
            if (key_size == 16) {
                return cbc ? EVP_aes_128_cbc() : EVP_aes_128_ecb();
            }
            if (key_size == 32) {
                return cbc ? EVP_aes_256_cbc() : EVP_aes_256_ecb();
            }
            return nullptr;
        }

        std::string aes_encrypt(const EVP_CIPHER *cipher, std::string_view key, std::string_view iv, std::string_view plain) {
            if (!cipher) {
                return {};
            }
            CipherCtx ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
            EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                               iv.empty() ? nullptr : reinterpret_cast<const unsigned char *>(iv.data()));
            std::string out(plain.size() + 16, '\0');
            int n1 = 0, n2 = 0;
            auto *dst = reinterpret_cast<unsigned char *>(out.data());
            EVP_EncryptUpdate(ctx.get(), dst, &n1, reinterpret_cast<const unsigned char *>(plain.data()), static_cast<int>(plain.size()));
            EVP_EncryptFinal_ex(ctx.get(), dst + n1, &n2);
            out.resize(static_cast<size_t>(n1 + n2));
            return out;
        }

        std::optional<std::string> aes_decrypt(const EVP_CIPHER *cipher, std::string_view key, std::string_view iv, std::string_view cipher_text) {
            if (!cipher || cipher_text.empty() || cipher_text.size() % 16 != 0) {
                return std::nullopt;
            }
            CipherCtx ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
            EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                               iv.empty() ? nullptr : reinterpret_cast<const unsigned char *>(iv.data()));
            std::string out(cipher_text.size(), '\0');
            int n1 = 0, n2 = 0;
            auto *dst = reinterpret_cast<unsigned char *>(out.data());
            if (EVP_DecryptUpdate(ctx.get(), dst, &n1, reinterpret_cast<const unsigned char *>(cipher_text.data()), static_cast<int>(cipher_text.size())) != 1 ||
                EVP_DecryptFinal_ex(ctx.get(), dst + n1, &n2) != 1) {
                return std::nullopt;
            }
            out.resize(static_cast<size_t>(n1 + n2));
            return out;
        }
    } // namespace

    std::string md5_hex(std::string_view bytes) {
        std::array<unsigned char, 16> digest;
        unsigned size = 0;
        EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_md5(), nullptr);
        return encoding::hex({reinterpret_cast<const char *>(digest.data()), size});
    }

    std::string random_hex(size_t digits, bool upper) {
        std::string bytes((digits + 1) / 2, '\0');
        RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
        return encoding::hex(bytes, upper).substr(0, digits);
    }

    std::string aes_ecb_encrypt(std::string_view key, std::string_view plain) {
        return aes_encrypt(aes_cipher(key.size(), false), key, {}, plain);
    }

    std::optional<std::string> aes_ecb_decrypt(std::string_view key, std::string_view cipher) {
        return aes_decrypt(aes_cipher(key.size(), false), key, {}, cipher);
    }

    std::string aes_cbc_encrypt(std::string_view key, std::string_view iv, std::string_view plain) {
        return iv.size() == 16 ? aes_encrypt(aes_cipher(key.size(), true), key, iv, plain) : std::string{};
    }

    std::optional<std::string> aes_cbc_decrypt(std::string_view key, std::string_view iv, std::string_view cipher) {
        return iv.size() == 16 ? aes_decrypt(aes_cipher(key.size(), true), key, iv, cipher) : std::nullopt;
    }

} // namespace lyren::crypto
