#include "net/fetch.hpp"

#include <array>

#include "core/log.hpp"

namespace lyren::net {

    namespace {
        struct FetchRef {
            Loop &loop;
            Http &http;
            const Request &req;
            std::shared_ptr<detail::FetchState> state;
            Http::Id id = 0;

            ~FetchRef() {
                if (state) {
                    state->cancelled = true;
                    state->h = {};
                    if (id) {
                        http.cancel(id);
                    }
                }
            }
            bool await_ready() const noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<> h) {
                id = detail::start_fetch(loop, http, req, state, h);
            }
            Response await_resume() {
                id = 0;
                return std::move(state->res);
            }
        };
    } // namespace

    Task<Response> fetch_retry(Loop &loop, Http &http, Request req, Retry retry) {
        constexpr std::array delays{300ms, 900ms};
        for (auto delay : delays) {
            auto res = co_await FetchRef{loop, http, req};
            if (res.error.empty() || retry == Retry::never) {
                co_return res;
            }
            log::warn("http {}: {}, retrying", req.url.substr(0, req.url.find('?')), res.error);
            co_await sleep{loop, delay};
        }
        co_return co_await FetchRef{loop, http, req};
    }

} // namespace lyren::net
