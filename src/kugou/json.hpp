#pragma once
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

namespace lyren::kugou {

    inline Result<json::Value> parse_json(std::string_view body) {
        json::Value value;
        if (glz::read_json(value, body) || !value.is_object()) {
            return std::unexpected("Unrecognized KuGou Music response");
        }
        return value;
    }

    inline Result<json::Value> parse_response(std::string_view body, bool account = false) {
        auto parsed = parse_json(body);
        if (!parsed) {
            return parsed;
        }
        const auto &code = json::first(*parsed, {"error_code", "errcode", "err_code"});
        const auto status = json::number(json::field(*parsed, "status"), -1);
        const auto error = code.is_null() ? status : json::number(code, -1);
        if ((!code.is_null() && error != 0) || (account && status != 1) || (code.is_null() && status != 1)) {
            std::string message = json::text(json::first(*parsed, {"error_msg", "errmsg", "msg", "message"}));
            if (message.empty()) {
                message = error == 20028 ? std::string(tr("KuGou Music requires security verification (20028)")) : trf("KuGou Music request failed ({})", error);
            }
            return std::unexpected(std::move(message));
        }
        return parsed;
    }

    inline const json::Value &response_data(const json::Value &root) {
        const auto &value = json::field(root, "data");
        if (!value.is_null()) {
            return value;
        }
        const auto &info = json::field(root, "info");
        return info.is_array() ? root : info;
    }

} // namespace lyren::kugou
