#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace lyren::utf8 {

    // Decodes the code point at s[i] and advances i; malformed input yields U+FFFD and advances one byte.
    char32_t decode(std::string_view s, std::size_t &i);
    void append(std::string &out, char32_t cp);
    // Byte offset of the next / previous code point boundary.
    std::size_t next(std::string_view s, std::size_t i);
    std::size_t prev(std::string_view s, std::size_t i);

} // namespace lyren::utf8
