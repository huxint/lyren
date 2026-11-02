#include "kugou/client.hpp"

#include <chrono>
#include <format>

#include "core/crypto.hpp"

namespace lyren::kugou {

    namespace {
        void append_escaped(std::string &out, std::string_view value) {
            constexpr std::string_view digits = "0123456789ABCDEF";
            for (unsigned char c : value) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                    out += static_cast<char>(c);
                } else {
                    out += '%';
                    out += digits[c >> 4];
                    out += digits[c & 15];
                }
            }
        }
    } // namespace

    std::string sign(const Params &query, std::string_view body, std::string_view salt) {
        size_t size = salt.size() * 2 + body.size();
        for (const auto &[name, value] : query) {
            size += name.size() + 1 + value.size();
        }
        std::string input;
        input.reserve(size);
        input += salt;
        for (const auto &[name, value] : query) {
            input += name;
            input += '=';
            input += value;
        }
        input += body;
        input += salt;
        return crypto::md5_hex(input);
    }

    net::Request request(std::string url, const Params &query, std::optional<std::string> body, std::string_view salt, std::vector<std::string> headers) {
        size_t size = url.size() + (salt.empty() ? 0 : 43);
        for (const auto &[name, value] : query) {
            size += name.size() + 2 + value.size();
        }
        url.reserve(size);
        char separator = '?';
        for (const auto &[name, value] : query) {
            url += separator;
            append_escaped(url, name);
            url += '=';
            append_escaped(url, value);
            separator = '&';
        }
        if (!salt.empty()) {
            url += separator;
            url += "signature=";
            url += sign(query, body ? std::string_view(*body) : std::string_view{}, salt);
        }
        headers.emplace_back("Content-Type: application/json");
        return {.url = std::move(url), .body = std::move(body), .headers = std::move(headers)};
    }

    const std::string &Client::get(std::string_view name) const {
        static const std::string empty;
        auto it = session.find(name);
        return it == session.end() ? empty : it->second;
    }

    bool Client::logged_in() const {
        return !get("userid").empty() && !get("token").empty();
    }

    Params Client::common(std::string_view device) const {
        using namespace std::chrono;
        return {{"dfid", "-"},          {"mid", std::string(device.empty() ? get("mid") : device)},
                {"uuid", "-"},          {"appid", "1005"},
                {"clientver", "20489"}, {"clienttime", std::to_string(duration_cast<seconds>(system_clock::now().time_since_epoch()).count())}};
    }

    std::vector<std::string> Client::headers(const Params &query, std::string_view router) const {
        std::vector<std::string> out{"dfid: " + query.at("dfid"), "mid: " + query.at("mid"), "clienttime: " + query.at("clienttime"),
                                     "User-Agent: Android712-AndroidPhone-11451-376-0-FeeCacheUpdate-wifi"};
        if (!router.empty()) {
            out.emplace_back("x-router: " + std::string(router));
        }
        return out;
    }

    Task<Result<std::string>> Client::send(net::Request req, net::Retry retry) {
        const auto generation = session_gen;
        auto reply = co_await net::fetch_retry(loop_, http_, std::move(req), retry);
        if (generation != session_gen) {
            co_return std::unexpected("KuGou Music session changed; retry the action");
        }
        if (!reply.error.empty()) {
            co_return std::unexpected("Network connection failed");
        }
        if (reply.status < 200 || reply.status >= 300) {
            co_return std::unexpected(std::format("KuGou Music request failed (HTTP {})", reply.status));
        }
        co_return std::move(reply.body);
    }

} // namespace lyren::kugou
