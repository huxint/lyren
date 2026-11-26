#pragma once
#include <cstdint>
#include <memory>
#include <utility>

#include "core/task.hpp"
#include "net/http.hpp"

namespace lyren::net {

    // Reads may retry transport failures; writes must not be replayed after an uncertain result.
    enum class Retry : uint8_t {
        never,
        network
    };

    namespace detail {
        struct FetchState {
            std::coroutine_handle<> h;
            Response res;
            bool cancelled = false; // loop thread only
        };

        inline Http::Id start_fetch(Loop &loop, Http &http, const Request &req, std::shared_ptr<FetchState> &state, std::coroutine_handle<> h) {
            state = std::make_shared<FetchState>();
            state->h = h;
            return http.request(req, [&loop, st = state](Response r) {
                loop.post([st = std::move(st), r = std::move(r)]() mutable {
                    if (st->cancelled) {
                        return;
                    }
                    st->res = std::move(r);
                    if (auto resume = std::exchange(st->h, {})) {
                        resume.resume();
                    }
                });
            });
        }
    } // namespace detail

    // co_await fetch{loop, http, req}: runs the request and resumes on the loop thread.
    // Destroying the suspended coroutine cancels the HTTP request and drops any in-flight completion.
    struct fetch {
        Loop &loop;
        Http &http;
        Request req;
        std::shared_ptr<detail::FetchState> state;
        Http::Id id = 0;

        fetch(Loop &loop, Http &http, Request req) noexcept
        : loop(loop),
          http(http),
          req(std::move(req)) {}
        fetch(const fetch &) = delete;
        fetch &operator=(const fetch &) = delete;
        ~fetch() {
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

    // At most three attempts, with 300 ms and 900 ms waits. HTTP responses are never retried.
    Task<Response> fetch_retry(Loop &loop, Http &http, Request req, Retry retry = Retry::network);

} // namespace lyren::net
