#pragma once
#include <glaze/glaze.hpp>

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <span>

#include "core/result.hpp"

namespace lyren::kugou::json {

    using Value = glz::generic_u64;

    inline const Value &get(const Value &value, std::string_view key) {
        static const Value empty;
        auto object = value.get_if<Value::object_t>();
        if (!object) {
            return empty;
        }
        auto it = object->find(key);
        return it == object->end() ? empty : it->second;
    }

    inline const Value &first(const Value &value, std::initializer_list<std::string_view> names) {
        for (auto name : names) {
            const auto &item = get(value, name);
            if (!item.is_null()) {
                return item;
            }
        }
        static const Value empty;
        return empty;
    }

    inline std::string text(const Value &value) {
        if (auto p = value.get_if<std::string>()) {
            return *p;
        }
        if (auto p = value.get_if<uint64_t>()) {
            return std::to_string(*p);
        }
        if (auto p = value.get_if<int64_t>()) {
            return std::to_string(*p);
        }
        return {};
    }

    inline std::string text(const Value &value, std::string_view key) {
        return text(get(value, key));
    }

    inline double number(const Value &value, double absent = 0) {
        if (auto p = value.get_if<uint64_t>()) {
            return static_cast<double>(*p);
        }
        if (auto p = value.get_if<int64_t>()) {
            return static_cast<double>(*p);
        }
        if (auto p = value.get_if<double>()) {
            return std::isfinite(*p) ? *p : absent;
        }
        if (auto p = value.get_if<std::string>()) {
            double result;
            auto parsed = std::from_chars(p->data(), p->data() + p->size(), result);
            if (parsed.ec == std::errc{} && parsed.ptr == p->data() + p->size() && std::isfinite(result)) {
                return result;
            }
        }
        return absent;
    }

    inline int count(const Value &value, int absent = 0) {
        auto n = number(value, absent);
        return n >= 0 && n <= std::numeric_limits<int>::max() ? static_cast<int>(n) : absent;
    }

    inline std::span<const Value> array(const Value &value) {
        if (auto p = value.get_if<Value::array_t>()) {
            return *p;
        }
        return {};
    }

    inline Result<Value> parse(std::string_view body) {
        Value value;
        if (glz::read_json(value, body) || !value.is_object()) {
            return std::unexpected("Unrecognized KuGou Music response");
        }
        return value;
    }

    inline Result<Value> response(std::string_view body, bool account = false) {
        auto parsed = parse(body);
        if (!parsed) {
            return parsed;
        }
        const auto &code = first(*parsed, {"error_code", "errcode", "err_code"});
        const auto status = number(get(*parsed, "status"), -1);
        if ((!code.is_null() && number(code, -1) != 0) || (account && status != 1) || (code.is_null() && status != 1)) {
            std::string message = text(first(*parsed, {"error_msg", "errmsg", "msg", "message"}));
            if (message.empty()) {
                message = std::format("KuGou Music request failed ({})", code.is_null() ? status : number(code));
            }
            return std::unexpected(std::move(message));
        }
        return parsed;
    }

    inline const Value &data(const Value &root) {
        const auto &value = get(root, "data");
        if (!value.is_null()) {
            return value;
        }
        const auto &info = get(root, "info");
        return info.is_array() ? root : info;
    }

    inline std::string write(const Value &value) {
        return *glz::write_json(value);
    }

} // namespace lyren::kugou::json
