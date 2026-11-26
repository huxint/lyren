#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace lyren {

    template <class T>
    using StringMap = std::map<std::string, T, std::less<>>;

    inline const std::string &get(const StringMap<std::string> &map, std::string_view key) {
        static const std::string empty;
        auto it = map.find(key);
        return it == map.end() ? empty : it->second;
    }

    inline std::string_view trim(std::string_view text) {
        constexpr std::string_view blank = " \t\r\n";
        const auto first = text.find_first_not_of(blank);
        if (first == text.npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(blank) - first + 1);
    }

    inline std::string trimmed(std::string_view text) {
        return std::string(trim(text));
    }

    inline std::string trimmed(const std::same_as<std::optional<std::string>> auto &text) {
        return text ? std::string(trim(*text)) : std::string{};
    }

    struct StringHash {
        using is_transparent = void;
        // Let libstdc++ cache string hashes in its nodes, as it does for std::hash<std::string>.
        size_t operator()(std::string_view text) const {
            return std::hash<std::string_view>{}(text);
        }
    };

    template <class T>
    using StringHashMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

    using StringHashSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

    // Lookups borrow the text; only stored keys own a copy. GCC 16.2's unordered try_emplace still converts probes
    // to key_type, so these caches use find + emplace and keep the owning conversion explicit.
    struct CacheKey {
        struct View {
            std::string_view text;
            uint32_t variant;
            bool operator==(const View &) const = default;
        };

        std::string text;
        uint32_t variant;

        explicit CacheKey(View key)
        : text(key.text),
          variant(key.variant) {}

        operator View() const noexcept {
            return {text, variant};
        }

        struct Hash {
            using is_transparent = void;
            size_t operator()(View key) const {
                return StringHash{}(key.text) ^ (static_cast<size_t>(key.variant) * 0x9e3779b97f4a7c15ull);
            }
        };
        struct Equal {
            using is_transparent = void;
            bool operator()(View a, View b) const noexcept {
                return a == b;
            }
        };
    };

    template <class T>
    using CacheMap = std::unordered_map<CacheKey, T, CacheKey::Hash, CacheKey::Equal>;

} // namespace lyren
