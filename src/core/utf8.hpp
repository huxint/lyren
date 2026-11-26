#pragma once
#include <cstddef>
#include <string_view>

namespace lyren::utf8 {

    // Decodes the code point at s[i] and advances i; malformed input yields U+FFFD and advances one byte.
    char32_t decode(std::string_view s, std::size_t &i);
    // Byte offset of the next / previous code point boundary.
    std::size_t next(std::string_view s, std::size_t i);
    std::size_t prev(std::string_view s, std::size_t i);
    // Number of UTF-8 code points in s.
    std::size_t count(std::string_view s);
    // Longest valid UTF-8 prefix of s within max_bytes bytes.
    std::string_view truncate_bytes(std::string_view s, std::size_t max_bytes);

} // namespace lyren::utf8
