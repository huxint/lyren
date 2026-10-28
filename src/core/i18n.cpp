#include "core/i18n.hpp"

#include <glaze/glaze.hpp>
#include <map>

namespace lyren {

    namespace {
        Language selected = Language::zh;

        const unsigned char chinese_json[] = {
#embed "i18n/zh.json" suffix(, 0)
        };

        const auto &chinese() {
            static const auto catalog = [] {
                std::map<std::string, std::string, std::less<>> entries;
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

} // namespace lyren
