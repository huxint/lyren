#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lyren::crypto {

    using Aes128Key = std::array<unsigned char, 16>;

    std::string md5_hex(std::string_view bytes);
    std::string random_hex(size_t digits, bool upper = false);

    // AES-128 (16-byte key) or AES-256 (32-byte key) with PKCS#7 padding.
    std::string aes_ecb_encrypt(std::string_view key, std::string_view plain);
    std::optional<std::string> aes_ecb_decrypt(std::string_view key, std::string_view cipher);
    std::string aes_cbc_encrypt(std::string_view key, std::string_view iv, std::string_view plain);
    std::optional<std::string> aes_cbc_decrypt(std::string_view key, std::string_view iv, std::string_view cipher);

} // namespace lyren::crypto
