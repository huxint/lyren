#pragma once
#include <glaze/glaze.hpp>

#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace lyren::json {

    using Value = glz::generic_u64;

    inline const Value &field(const Value &value, std::string_view key) {
        static const Value empty;
        if (const auto *object = value.get_if<Value::object_t>()) {
            if (auto it = object->find(key); it != object->end()) {
                return it->second;
            }
        }
        return empty;
    }

    inline const Value &first(const Value &value, std::initializer_list<std::string_view> names) {
        for (auto name : names) {
            const auto &item = field(value, name);
            if (!item.is_null()) {
                return item;
            }
        }
        static const Value empty;
        return empty;
    }

    inline std::span<const Value> array(const Value &value) {
        if (const auto *items = value.get_if<Value::array_t>()) {
            return *items;
        }
        return {};
    }

    inline std::string_view text_view(const Value &value) {
        const auto *s = value.get_if<std::string>();
        return s ? std::string_view(*s) : std::string_view{};
    }

    inline std::string text(const Value &value) {
        if (const auto *s = value.get_if<std::string>()) {
            return *s;
        }
        if (const auto *n = value.get_if<uint64_t>()) {
            return std::to_string(*n);
        }
        if (const auto *n = value.get_if<int64_t>()) {
            return std::to_string(*n);
        }
        return {};
    }

    inline std::string text(const Value &value, std::string_view key) {
        return text(field(value, key));
    }

    inline double number(const Value &value, double absent = 0) {
        if (const auto *n = value.get_if<uint64_t>()) {
            return static_cast<double>(*n);
        }
        if (const auto *n = value.get_if<int64_t>()) {
            return static_cast<double>(*n);
        }
        if (const auto *n = value.get_if<double>()) {
            return std::isfinite(*n) ? *n : absent;
        }
        if (const auto *s = value.get_if<std::string>()) {
            double result = 0;
            auto [end, ec] = std::from_chars(s->data(), s->data() + s->size(), result);
            if (ec == std::errc{} && end == s->data() + s->size() && std::isfinite(result)) {
                return result;
            }
        }
        return absent;
    }

    inline int count(const Value &value, int absent = 0) {
        const double n = number(value, absent);
        return n >= 0 && n <= std::numeric_limits<int>::max() ? static_cast<int>(n) : absent;
    }

    inline bool boolean(const Value &value) {
        const auto *flag = value.get_if<bool>();
        return flag && *flag;
    }

    inline std::string write(const Value &value) {
        return *glz::write_json(value);
    }

} // namespace lyren::json
