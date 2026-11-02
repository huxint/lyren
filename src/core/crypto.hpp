#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace lyren::crypto {

    using Aes128Key = std::array<unsigned char, 16>;

    std::string md5_hex(std::string_view bytes);
    std::string random_hex(size_t digits, bool upper = false);

} // namespace lyren::crypto
