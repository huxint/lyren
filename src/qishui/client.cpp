#include "qishui/client.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <format>

#include "core/crypto.hpp"

namespace lyren::qishui {

    const Json &field(const Json &value, std::string_view name) {
        static const Json absent;
        if (const auto *object = value.get_if<Json::object_t>()) {
            if (auto it = object->find(name); it != object->end()) {
                return it->second;
            }
        }
        return absent;
    }

    std::span<const Json> array(const Json &value) {
        if (const auto *items = value.get_if<Json::array_t>()) {
            return *items;
        }
        return {};
    }

    std::string_view text(const Json &value) {
        const auto *value_text = value.get_if<std::string>();
        return value_text ? std::string_view(*value_text) : std::string_view{};
    }

    std::string string(const Json &value) {
        if (const auto *text = value.get_if<std::string>()) {
            return *text;
        }
        if (const auto *n = value.get_if<uint64_t>()) {
            return std::to_string(*n);
        }
        if (const auto *n = value.get_if<int64_t>()) {
            return std::to_string(*n);
        }
        return {};
    }

    double number(const Json &value) {
        double result = 0;
        if (value.is_number()) {
            result = value.as<double>();
        } else if (const auto *text = value.get_if<std::string>()) {
            auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), result);
            if (error != std::errc{} || end != text->data() + text->size()) {
                return 0;
            }
        }
        return std::isfinite(result) ? result : 0;
    }

    bool boolean(const Json &value) {
        const auto *flag = value.get_if<bool>();
        return flag && *flag;
    }

    std::string encode(std::string_view value) {
        constexpr std::string_view hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size());
        for (const unsigned char c : value) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
                out += c;
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        return out;
    }

    std::string query_string(const Query &query) {
        std::string out;
        for (const auto &[name, value] : query) {
            if (!out.empty()) {
                out += '&';
            }
            out += encode(name) + '=' + encode(value);
        }
        return out;
    }

    Result<Json> parse_response(std::string_view body, bool *session_expired) {
        Json value;
        if (glz::read_json(value, body) || !value.is_object() || value.get_object().empty()) {
            return std::unexpected("Qishui Music returned an invalid response");
        }
        const auto &status = field(value, "status_code");
        if (!status.is_null() && !status.is_number()) {
            return std::unexpected("Qishui Music returned an invalid status");
        }
        const auto code = number(status);
        if (code != 0) {
            std::string message = string(field(field(value, "status_info"), "status_msg"));
            if (code == 1000016) {
                if (session_expired) {
                    *session_expired = true;
                }
                return std::unexpected("Sign in to Qishui Music and try again");
            }
            return std::unexpected(message.empty() ? std::format("Qishui Music request failed ({})", code) : std::move(message));
        }
        return value;
    }

    namespace {
        Result<Json> received(net::Response response, bool *session_expired = nullptr) {
            if (!response.error.empty()) {
                return std::unexpected("Network connection failed");
            }
            if (response.status < 200 || response.status >= 300) {
                return std::unexpected(std::format("Qishui Music request failed (HTTP {})", response.status));
            }
            if (response.verification == net::Verification::captcha) {
                return std::unexpected("Complete Qishui Music verification in the official app, then reconnect your session");
            }
            if (response.verification == net::Verification::passport) {
                return std::unexpected("Complete Qishui Music account verification, then reconnect your session");
            }
            return parse_response(response.body, session_expired);
        }
    } // namespace

    const std::string &Client::get(std::string_view name) const {
        static const std::string empty;
        const auto found = session.find(name);
        return found == session.end() ? empty : found->second;
    }

    bool Client::logged_in() const {
        return !get("cookie_jar").empty() && !get("user_id").empty();
    }

    Task<Result<Json>> Client::signer(std::string path, std::optional<std::string> body) {
        std::string url = get("signer_url");
        while (url.ends_with('/')) {
            url.pop_back();
        }
        net::Request request{.url = url + path, .body = std::move(body), .headers = {"Content-Type: application/json"}, .follow_redirects = false};
        if (!get("signer_token").empty()) {
            request.headers.push_back("Authorization: Bearer " + get("signer_token"));
        }
        auto result = received(co_await net::fetch_retry(loop_, http_, std::move(request), net::Retry::network));
        if (result && !boolean(field(*result, "ok"))) {
            const auto error = string(field(*result, "error"));
            co_return std::unexpected(error.empty() ? "Qishui Music signer rejected the request" : error);
        }
        co_return result;
    }

    Task<Result<void>> Client::initialize() {
        cookies_ = net::CookieJar::from_netscape(get("cookie_jar"));
        bool changed = false;
        for (std::string_view name : {"device_id", "iid", "search_device_id", "search_iid"}) {
            if (!get("signer_url").empty() && !name.starts_with("search_")) {
                continue;
            }
            if (get(name).empty()) {
                const auto random = crypto::random_hex(16);
                uint64_t value;
                std::from_chars(random.data(), random.data() + random.size(), value, 16);
                session[name] = std::to_string(1000000000000000ull + value % 9000000000000000ull);
                changed = true;
            }
        }
        if (get("search_cdid").empty()) {
            const auto random = crypto::random_hex(32);
            session["search_cdid"] = std::format("{}-{}-{}-{}-{}", random.substr(0, 8), random.substr(8, 4), random.substr(12, 4), random.substr(16, 4), random.substr(20));
            session["search_first_use"] = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            changed = true;
        }
        if (get("signer_url").empty()) {
            if (get("fp").empty()) {
                session["fp"] = get("device_id");
            }
            if (changed && on_session) {
                on_session();
            }
            co_return Result<void>{};
        }
        const auto gen = generation;
        auto config = co_await signer("/config");
        if (!config) {
            co_return std::unexpected(config.error());
        }
        if (generation != gen) {
            co_return std::unexpected("Qishui Music session changed; retry the action");
        }
        const auto &identity = field(*config, "identity");
        for (auto name : {"device_id", "iid", "fp"}) {
            auto value = string(field(identity, name));
            if (!value.empty()) {
                session.insert_or_assign(name, std::move(value));
            }
        }
        if (get("device_id").empty() || get("iid").empty()) {
            co_return std::unexpected("Qishui Music signer must provide device_id and iid, or include them in the connection settings");
        }
        if (get("fp").empty()) {
            session["fp"] = get("device_id");
        }
        if (on_session) {
            on_session();
        }
        co_return Result<void>{};
    }

    Task<Result<Json>> Client::call(std::string path, Query query, std::optional<std::string> body, Operation operation) {
        if (operation == Operation::write && get("signer_url").empty()) {
            co_return std::unexpected("Configure a Qishui Music signer in the account connection settings");
        }
        const auto gen = generation;
        Query common{
            {"aid", "386088"},
            {"app_name", "luna_pc"},
            {"region", "cn"},
            {"geo_region", "cn"},
            {"os_region", "cn"},
            {"sim_region", ""},
            {"device_id", get("device_id")},
            {"cdid", ""},
            {"iid", get("iid")},
            {"fp", get("fp")},
            {"version_name", "3.3.0"},
            {"version_code", "30030000"},
            {"channel", "official"},
            {"build_mode", "master"},
            {"network_carrier", ""},
            {"ac", "wifi"},
            {"tz_name", "Asia/Shanghai"},
            {"resolution", ""},
            {"device_platform", "windows"},
            {"device_type", "Windows"},
            {"os_version", "Windows 11"},
        };
        common.insert(common.end(), std::make_move_iterator(query.begin()), std::make_move_iterator(query.end()));
        std::string url = api_base + path + '?' + query_string(common);
        Json headers{{"user-agent", "LunaPC/3.3.0(359450208)"},
                     {"x-luna-background-type", "foreground"},
                     {"x-luna-is-background-req", "0"},
                     {"x-luna-is-local-user", logged_in() ? "1" : "0"}};
        const auto authority = std::string_view(api_base).substr(api_base.find("://") + 3);
        const auto host = authority.substr(0, authority.find_first_of(":/"));
        auto cookies = cookies_;
        if (auto cookie = cookies.header(host, path, api_base.starts_with("https://")); !cookie.empty()) {
            headers["cookie"] = std::move(cookie);
        }
        if (body) {
            headers["content-type"] = "application/json; charset=utf-8";
            auto stub = crypto::md5_hex(*body);
            for (auto &c : stub) {
                if (c >= 'a' && c <= 'f') {
                    c += 'A' - 'a';
                }
            }
            headers["x-ss-stub"] = std::move(stub);
        }
        if (!get("signer_url").empty()) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            Json sign_request{{"url", url}, {"method", body ? "POST" : "GET"}, {"body", body.value_or("")}, {"headers", headers}, {"ts_ms", now}};
            auto signed_request = co_await signer("/sign", glz::write_json(sign_request).value());
            if (!signed_request) {
                co_return std::unexpected(signed_request.error());
            }
            if (generation != gen) {
                co_return std::unexpected("Qishui Music session changed; retry the action");
            }
            const auto &signatures = field(*signed_request, "headers");
            bool helios = false, medusa = false;
            if (signatures.is_object()) {
                for (const auto &[name, value] : signatures.get_object()) {
                    std::string lower = name;
                    for (auto &c : lower) {
                        if (c >= 'A' && c <= 'Z') {
                            c += 'a' - 'A';
                        }
                    }
                    if (lower == "x-helios" || lower == "x-medusa") {
                        std::string signature = string(value);
                        if (signature.empty() || signature.find_first_of("\r\n") != signature.npos) {
                            co_return std::unexpected("Qishui Music signer returned an invalid signature");
                        }
                        headers[lower] = signature;
                        helios |= lower == "x-helios";
                        medusa |= lower == "x-medusa";
                    }
                }
            }
            if (!helios || !medusa) {
                co_return std::unexpected("Qishui Music signer did not return both request signatures");
            }
            for (const auto &[input, output] : {std::pair{"ms_token", "msToken"}, {"msToken", "msToken"}, {"a_bogus", "a_bogus"}}) {
                auto value = string(field(*signed_request, input));
                if (!value.empty() && !url.contains(std::string(output) + '=')) {
                    url += '&' + std::string(output) + '=' + encode(value);
                }
            }
        }
        net::Request request{.url = std::move(url), .body = std::move(body), .follow_redirects = false, .cookie_jar = std::move(cookies)};
        request.headers.reserve(headers.get_object().size());
        for (const auto &[name, value] : headers.get_object()) {
            request.headers.push_back(name + ": " + value.get<std::string>());
        }
        co_return co_await send(std::move(request), operation == Operation::write ? net::Retry::never : net::Retry::network);
    }

    Task<Result<Json>> Client::android_search(std::string type, std::string query, std::string cursor, int count) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Query fields{{"device_platform", "android"},
                     {"os", "android"},
                     {"ssmix", "a"},
                     {"cdid", get("search_cdid")},
                     {"channel", "xiaomi_8478_64"},
                     {"aid", "386088"},
                     {"app_name", "luna"},
                     {"package", "com.luna.music"},
                     {"version_code", "100198030"},
                     {"version_name", "19.8.0"},
                     {"manifest_version_code", "100198030"},
                     {"update_version_code", "100198030"},
                     {"hybrid_version_code", "100198030"},
                     {"resolution", "1080*1920"},
                     {"dpi", "480"},
                     {"device_type", "ABR-AL80"},
                     {"device_brand", "HUAWEI"},
                     {"device_model", "ABR-AL80"},
                     {"android_device_type", "default"},
                     {"language", "zh"},
                     {"os_api", "35"},
                     {"os_version", "15"},
                     {"ac", "wifi"},
                     {"save_power", "0"},
                     {"font_size", "1.00"},
                     {"luna_first_launch_apk_type", "normal_apk"},
                     {"luna_apk_type", "normal_apk"},
                     {"diversion_channel_name", "xiaomi_8478_64"},
                     {"diversion_category_level_two", "Xiaomi%E5%95%86%E5%BA%97-%E8%87%AA%E7%84%B6"},
                     {"diversion_category_level_one", "%E5%8E%82%E5%95%86%E5%95%86%E5%BA%97-%E8%87%AA%E7%84%B6"},
                     {"is_car_play", "0"},
                     {"battery", "0.99"},
                     {"network_speed", "10156"},
                     {"tz_name", "Asia/Shanghai"},
                     {"tz_offset", "28800"},
                     {"luna_register_time", get("search_first_use")},
                     {"charge", "0"},
                     {"output_device_type", "Phone"},
                     {"volume", "1.00"},
                     {"brightness", "0.08"},
                     {"need_personal_recommend", "1"},
                     {"is_teen_mode", "0"},
                     {"sim_region", "cn"},
                     {"iid", get("search_iid")},
                     {"device_id", get("search_device_id")},
                     {"_rticket", std::to_string(now)},
                     {"q", std::move(query)},
                     {"cursor", cursor.empty() ? "0" : std::move(cursor)},
                     {"count", std::to_string(count)}};
        net::Request request{.url = api_base + "/luna/search/" + type + '?' + query_string(fields),
                             .headers = {"User-Agent: com.luna.music/100198030 (Linux; U; Android 15; zh_CN_#Hans; ABR-AL80; Build/V417IR;tt-ok/3.12.13.19)",
                                         "Content-Type: application/json; charset=UTF-8"},
                             .follow_redirects = false,
                             .cookie_jar = cookies_};
        co_return co_await send(std::move(request), net::Retry::network);
    }

    Task<Result<Json>> Client::send(net::Request request, net::Retry retry) {
        const auto gen = generation;
        auto response = co_await net::fetch_retry(loop_, http_, std::move(request), retry);
        if (generation != gen) {
            co_return std::unexpected("Qishui Music session changed; retry the action");
        }
        if (!response.cookies.empty()) {
            cookies_ = std::move(response.cookie_jar);
            session["cookie_jar"] = cookies_.netscape();
            if (on_session) {
                on_session();
            }
        }
        bool expired = false;
        auto result = received(std::move(response), &expired);
        if (expired && logged_in()) {
            ++generation;
            session.erase("cookie_jar");
            session.erase("user_id");
            cookies_ = {};
            if (on_session) {
                on_session();
            }
        }
        co_return result;
    }

    Task<Result<Json>> Client::public_get(std::string url) {
        co_return received(co_await net::fetch_retry(loop_, http_, {.url = std::move(url)}, net::Retry::network));
    }

} // namespace lyren::qishui
