#include "qq/client.hpp"

#include <glaze/glaze.hpp>

#include <format>
#include <iterator>

#include "core/log.hpp"
#include "net/fetch.hpp"
#include "qq/crypto.hpp"

namespace lyren::qq {

    const std::string &Client::get(const std::string &key) const {
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
        // The signature covers these exact bytes, so the body is built once and sent as is.
        std::string body = comm == Comm::app ? R"({"comm":{"ct":20,"cv":1770,"tmeAppID":"qqmusic"})" : R"({"comm":{"ct":19,"cv":1,"tmeAppID":"qqmusic"})";
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
            r.error = "QQ Music request failed (HTTP " + std::to_string(res.status) + ')';
            log::warn("qq {}: HTTP {}", what, res.status);
            co_return r;
        }
        Top top;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(top, res.body)) {
            r.error = "Unrecognized QQ Music response (HTTP " + std::to_string(res.status) + ')';
            log::warn("qq {}: HTTP {}, {} bytes not JSON", what, res.status, res.body.size());
            co_return r;
        }
        if (top.code != 0) {
            r.error = "QQ Music request failed (" + std::to_string(top.code) + ')';
            log::warn("qq {}: code {}, trace {}", what, top.code, top.traceid.value_or(""));
            co_return r;
        }
        log::debug("qq {}: HTTP {}, code {}, trace {}", what, res.status, top.code, top.traceid.value_or(""));
        r.body = std::move(res.body);
        co_return r;
    }

} // namespace lyren::qq
