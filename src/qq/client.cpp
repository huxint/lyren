#include "qq/client.hpp"

#include <glaze/glaze.hpp>

#include <format>
#include <iterator>

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "net/fetch.hpp"
#include "qq/crypto.hpp"

namespace lyren::qq {

    const std::string &Client::get(std::string_view key) const {
        static const std::string empty;
        auto it = session.find(key);
        return it == session.end() ? empty : it->second;
    }

    namespace {
        struct Top {
            int code = 0;
            std::optional<std::string> traceid;
        };
    } // namespace

    Task<Reply> Client::call(std::vector<Rpc> parts, Comm comm, net::Retry retry) {
        const uint64_t gen = session_gen;
        const bool authenticated = comm == Comm::app && logged_in() && refresh_session;
        if (authenticated) {
            auto ready = co_await refresh_session(false);
            if (!ready) {
                co_return Reply{.error = ready.error()};
            }
            if (gen != session_gen) {
                co_return Reply{.error = "QQ Music session changed; retry the action"};
            }
        }
        const std::string key = get("musickey");
        auto reply = co_await request(parts, comm, retry);
        if (gen != session_gen) {
            co_return Reply{.error = "QQ Music session changed; retry the action"};
        }
        if (!authenticated || !reply.needs_login) {
            co_return reply;
        }
        // Another request may have renewed this same session while our response was in flight.
        if (key == get("musickey")) {
            auto renewed = co_await refresh_session(true);
            if (!renewed) {
                co_return Reply{.error = renewed.error()};
            }
        }
        if (gen != session_gen) {
            co_return Reply{.error = "QQ Music session changed; retry the action"};
        }
        if (retry == net::Retry::never) {
            co_return reply;
        }
        reply = co_await request(parts, comm, retry);
        if (gen != session_gen) {
            co_return Reply{.error = "QQ Music session changed; retry the action"};
        }
        co_return reply;
    }

    Task<Reply> Client::request(const std::vector<Rpc> &parts, Comm comm, net::Retry retry) {
        // The signature covers these exact bytes, so the body is built once and sent as is.
        std::string body = comm == Comm::app ? R"({"comm":{"ct":20,"cv":1770,"tmeAppID":"qqmusic"})" : R"({"comm":{"ct":19,"cv":1,"tmeAppID":"qqmusic"})";
        if (comm == Comm::app && logged_in()) {
            body.pop_back();
            std::format_to(std::back_inserter(body), R"(,"uin":{},"g_tk":{}}})", get("musicid"), hash33(get("musickey"), 5381));
        }
        std::string what;
        for (size_t i = 0; i < parts.size(); ++i) {
            std::format_to(std::back_inserter(body), R"(,"req_{}":{{"module":"{}","method":"{}","param":{}}})", i, parts[i].module, parts[i].method, parts[i].param);
            std::format_to(std::back_inserter(what), "{}{}.{}", i ? ", " : "", parts[i].module, parts[i].method);
        }
        body += '}';
        std::string url = comm == Comm::linux ? "https://u.y.qq.com/cgi-bin/musicu.fcg" : "https://u.y.qq.com/cgi-bin/musics.fcg?sign=" + sign(body);
        net::Request req{.url = std::move(url), .body = std::move(body), .headers = {"Content-Type: application/json", "Referer: https://y.qq.com/", "Origin: https://y.qq.com"}};
        if (logged_in()) {
            req.headers.push_back(std::format("Cookie: uin={0}; qqmusic_uin={0}; qqmusic_key={1}; qqmusic_version=17; qqmusic_miniversion=70; "
                                              "psrf_qqopenid={2}; psrf_qqaccess_token={3}",
                                              get("musicid"), get("musickey"), get("openid"), get("access_token")));
        }
        auto res = co_await net::fetch_retry(loop_, http_, std::move(req), retry);
        Reply r;
        if (!res.error.empty()) {
            r.error = "Network connection failed";
            log::warn("qq {}: {}", what, res.error);
            co_return r;
        }
        if (res.status < 200 || res.status >= 300) {
            r.error = lyren::trf("QQ Music request failed (HTTP {})", res.status);
            log::warn("qq {}: HTTP {}", what, res.status);
            co_return r;
        }
        Top top;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(top, res.body)) {
            r.error = lyren::trf("Unrecognized QQ Music response (HTTP {})", res.status);
            log::warn("qq {}: HTTP {}, {} bytes not JSON", what, res.status, res.body.size());
            co_return r;
        }
        r.needs_login = top.code == 2001;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (auto code = glz::get_view_json(std::format("/req_{}/code", i), res.body)) {
                int value = 0;
                if (!glz::read_json(value, std::string_view(code->data(), code->size())) && value == 2001) {
                    r.needs_login = true;
                }
            }
        }
        r.body = std::move(res.body);
        if (top.code != 0) {
            r.error = lyren::trf("QQ Music request failed ({})", top.code);
            log::warn("qq {}: code {}, trace {}", what, top.code, top.traceid.value_or(""));
            co_return r;
        }
        log::debug("qq {}: HTTP {}, code {}, trace {}", what, res.status, top.code, top.traceid.value_or(""));
        co_return r;
    }

} // namespace lyren::qq
