#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lyren {

    template <class T>
    using StringMap = std::map<std::string, T, std::less<>>;

    struct StringHash {
        using is_transparent = void;
        // Let libstdc++ cache string hashes in its nodes, as it does for std::hash<std::string>.
        size_t operator()(std::string_view text) const {
            return std::hash<std::string_view>{}(text);
        }
    };

    template <class T>
    using StringHashMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

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
