#include "core/i18n.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <glaze/glaze.hpp>
#include "core/string_map.hpp"

namespace lyren {

    namespace {
        Language selected = Language::zh;

        const unsigned char chinese_json[] = {
#embed "i18n/zh.json" suffix(, 0)
        };

        const auto &chinese() {
            static const auto catalog = [] {
                StringHashMap<std::string> entries;
                static_cast<void>(glz::read_json(entries, std::string_view{reinterpret_cast<const char *>(chinese_json), sizeof(chinese_json) - 1}));
                return entries;
            }();
            return catalog;
        }
    } // namespace

    void set_language(Language value) {
        selected = value;
    }

    Language language() {
        return selected;
    }

    std::string_view tr(std::string_view english) {
        if (selected == Language::zh) {
            const auto &catalog = chinese();
            if (auto it = catalog.find(english); it != catalog.end()) {
                return it->second;
            }
        }
        return english;
    }

    std::string_view tr_ctx(std::string_view context, std::string_view english) {
        if (selected == Language::zh) {
            const auto &catalog = chinese();
            const size_t len = english.size() + 1 + context.size();
            std::array<char, 256> buf;
            if (len <= buf.size()) {
                auto out = std::ranges::copy(english, buf.begin()).out;
                *out++ = '|';
                std::ranges::copy(context, out);
                if (auto it = catalog.find(std::string_view{buf.data(), len}); it != catalog.end()) {
                    return it->second;
                }
            } else if (auto it = catalog.find(std::format("{}|{}", english, context)); it != catalog.end()) {
                return it->second;
            }
            if (auto it = catalog.find(english); it != catalog.end()) {
                return it->second;
            }
        }
        return english;
    }

} // namespace lyren
