#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"

namespace lyren::encoding {

    // Views contain raw bytes and may include NULs. OpenSSL's block API accepts byte counts fitting in int.
    std::string hex(std::string_view bytes, bool upper = false);
    std::string base64(std::string_view bytes);
    // Standard padded Base64; an invalid input produces an empty string.
    std::string unbase64(std::string_view text);

    enum class UrlStyle : uint8_t {
        rfc3986,       // A-Za-z0-9-_.~
        uri_component, // A-Za-z0-9-_.!~*'() (JS encodeURIComponent)
        form           // A-Za-z0-9-_.* and ' ' -> '+'
    };

    void append_url_encoded(std::string &out, std::string_view value, UrlStyle style = UrlStyle::rfc3986);
    std::string url_encode(std::string_view value, UrlStyle style = UrlStyle::rfc3986);

    template <class Range>
    std::string query_string(const Range &fields, UrlStyle style = UrlStyle::rfc3986) {
        std::string out;
        size_t size = 0;
        for (const auto &[name, value] : fields) {
            size += std::string_view(name).size() + std::string_view(value).size() + 2;
        }
        out.reserve(size);
        for (const auto &[name, value] : fields) {
            if (!out.empty()) {
                out += '&';
            }
            append_url_encoded(out, name, style);
            out += '=';
            append_url_encoded(out, value, style);
        }
        return out;
    }

    inline std::string query_string(std::initializer_list<std::pair<std::string_view, std::string_view>> fields, UrlStyle style = UrlStyle::rfc3986) {
        return query_string<decltype(fields)>(fields, style);
    }

    // window_bits: 15 (MAX_WBITS) for zlib, 16 + 15 for gzip, -15 for raw deflate.
    Result<std::string> inflate(std::string_view input, int window_bits = 15, size_t max_bytes = 32u << 20);

} // namespace lyren::encoding
