#include "net/fetch.hpp"

#include <array>

#include "core/log.hpp"

namespace lyren::net {

    Task<Response> fetch_retry(Loop &loop, Http &http, Request req, Retry retry) {
        constexpr std::array delays{300ms, 900ms};
        for (auto delay : delays) {
            auto res = co_await fetch{loop, http, req};
            if (res.error.empty() || retry == Retry::never) {
                co_return res;
            }
            log::warn("http {}: {}, retrying", req.url.substr(0, req.url.find('?')), res.error);
            co_await sleep{loop, delay};
        }
        co_return co_await fetch{loop, http, std::move(req)};
    }

} // namespace lyren::net
