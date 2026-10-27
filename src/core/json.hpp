#pragma once
#include <format>
#include <string>
#include <string_view>

namespace lyren {

    // Quoted and escaped, for building request bodies.
    inline std::string json_string(std::string_view s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out += std::format("\\u{:04x}", int(c));
                    } else {
                        out += c;
                    }
            }
        }
        return out + '"';
    }

} // namespace lyren
