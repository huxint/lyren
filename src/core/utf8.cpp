#include "core/utf8.hpp"

namespace lyren::utf8 {

    char32_t decode(std::string_view s, std::size_t &i) {
        auto b = [&](std::size_t k) {
            return static_cast<unsigned char>(s[k]);
        };
        unsigned char c = b(i);
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            ++i;
            return 0xfffd;
        }
        char32_t cp = len == 1 ? c : c & (0x7f >> len);
        for (int k = 1; k < len; ++k) {
            if ((b(i + k) & 0xc0) != 0x80) {
                ++i;
                return 0xfffd;
            }
            cp = (cp << 6) | (b(i + k) & 0x3f);
        }
        static constexpr char32_t min[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min[len] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            ++i;
            return 0xfffd;
        }
        i += len;
        return cp;
    }

    void append(std::string &out, char32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        } else {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }

    std::size_t next(std::string_view s, std::size_t i) {
        if (i >= s.size()) {
            return s.size();
        }
        decode(s, i);
        return i;
    }

    std::size_t prev(std::string_view s, std::size_t i) {
        if (i == 0) {
            return 0;
        }
        --i;
        while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) {
            --i;
        }
        return i;
    }

} // namespace lyren::utf8
