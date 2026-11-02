#include "bodian/client.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>

#include "core/crypto.hpp"
#include "core/encoding.hpp"

namespace lyren::bodian {

    namespace {
        void append_form(std::string &out, std::string_view value) {
            constexpr char hex[] = "0123456789ABCDEF";
            for (unsigned char c : value) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') {
                    out += static_cast<char>(c);
                } else if (c == ' ') {
                    out += '+';
                } else {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
        }

        struct Envelope {
            int code = 0;
            std::optional<std::string> msg;
        };

        Reply response(net::Response response) {
            if (!response.error.empty()) {
                return {.error = std::move(response.error)};
            }
            if (response.status < 200 || response.status >= 300) {
                return {.error = std::format("Bodian Music: HTTP {}", response.status)};
            }
            Envelope value;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(value, response.body)) {
                return {.error = "Could not parse server response"};
            }
            Reply out{.code = value.code, .body = std::move(response.body)};
            if (value.code != 200) {
                out.error = std::format("Bodian Music: {} ({})", value.msg.value_or("Request failed"), value.code);
            }
            return out;
        }
    } // namespace

    std::string form_encode(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        append_form(out, value);
        return out;
    }

    std::string query_string(const Query &query) {
        std::string out;
        size_t size = 0;
        for (const auto &[key, value] : query) {
            size += key.size() + value.size() + 2;
        }
        out.reserve(size);
        for (const auto &[key, value] : query) {
            if (!out.empty()) {
                out += '&';
            }
            append_form(out, key);
            out += '=';
            append_form(out, value);
        }
        return out;
    }

    std::string signature(std::string_view path, std::string_view query, std::string_view body) {
        std::array<size_t, 128> counts{};
        for (unsigned char c : query) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                ++counts[c];
            }
        }
        constexpr std::string_view salt = "kuwotest";
        std::string input;
        input.reserve(std::max(body.size() + salt.size(), salt.size() + query.size() + 32 + path.size()));
        std::string digest;
        if (!body.empty()) {
            input = body;
            input += salt;
            digest = crypto::md5_hex(input);
        }
        input = salt;
        // Counting the fixed ASCII alphabet keeps the signature construction linear.
        for (unsigned char c : std::string_view("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")) {
            input.append(counts[c], static_cast<char>(c));
        }
        input += digest;
        input += path;
        return crypto::md5_hex(input);
    }

    const std::string &Client::get(std::string_view key) const {
        static const std::string empty;
        auto it = session.find(key);
        return it == session.end() ? empty : it->second;
    }

    bool Client::logged_in() const {
        return !get("uid").empty() && !get("token").empty();
    }

    void Client::ensure_device() {
        if (!get("devid").empty()) {
            return;
        }
        const std::string hex = crypto::random_hex(32);
        session["devid"] = std::format("{}-{}-{}-{}-{}", hex.substr(0, 8), hex.substr(8, 4), hex.substr(12, 4), hex.substr(16, 4), hex.substr(20));
        if (on_session) {
            on_session();
        }
    }

    void Client::clear_login() {
        for (std::string_view key : {"uid", "token", "nickname", "avatar"}) {
            session.erase(std::string(key));
        }
        if (on_session) {
            on_session();
        }
    }

    net::Request Client::request(Call call, int64_t timestamp) const {
        call.query.emplace_back("uid", logged_in() ? get("uid") : "-1");
        call.query.emplace_back("token", logged_in() ? get("token") : "");
        if (call.sign) {
            call.query.emplace_back("timestamp", std::to_string(timestamp));
            call.query.emplace_back("sign", "");
        }
        auto query = query_string(call.query);
        if (call.sign) {
            query += signature(call.path, query, call.body ? std::string_view(*call.body) : std::string_view{});
        }
        net::Request request{.url = base_url_ + call.path + '?' + query,
                             .body = std::move(call.body),
                             .headers = {"User-Agent: Dart/3.3 (dart:io)", call.android ? "plat: android" : "plat: win", "channel: W1", "ver: 1.1.7", "svrver: 13",
                                         "api-ver: application/json", "brand: Windows", "net: wifi", "devid: " + get("devid"), "qimei36: " + get("devid")},
                             .method = std::move(call.method)};
        if (request.body) {
            request.headers.emplace_back("Content-Type: application/json");
        }
        if (logged_in()) {
            request.headers.emplace_back("uid: " + get("uid"));
            request.headers.emplace_back("token: " + get("token"));
        }
        return request;
    }

    Task<Reply> Client::call(Call call) {
        ensure_device();
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string token = get("token");
        auto req = request(std::move(call), now);
        const bool read = req.method == "GET" || (req.method.empty() && !req.body);
        auto reply = response(co_await net::fetch_retry(loop_, http_, std::move(req), read ? net::Retry::network : net::Retry::never));
        if (token != get("token")) {
            co_return Reply{.error = "Bodian Music session changed; retry the action"};
        }
        if (reply.code == 11012 && !token.empty() && token == get("token")) {
            clear_login();
        }
        co_return reply;
    }

    Task<Reply> Client::lyric(std::string id, bool words) {
        std::string payload = std::format("type=lyric&req=2&lrcx={}&rid={}&songname=&artist=&corp=kuwo&fromchannel=bodian", words ? 1 : 0, id);
        net::Request req{.url = "https://mlyric.kuwo.cn/mobi.s?f=bodian&q=" + form_encode(encoding::base64(payload))};
        co_return response(co_await net::fetch_retry(loop_, http_, std::move(req)));
    }

} // namespace lyren::bodian
