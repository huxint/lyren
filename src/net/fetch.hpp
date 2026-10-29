#pragma once
#include <cstdint>

#include "core/task.hpp"
#include "net/http.hpp"

namespace lyren::net {

    // Reads may retry transport failures; writes must not be replayed after an uncertain result.
    enum class Retry : uint8_t {
        never,
        network
    };

    // co_await fetch{loop, http, req}: runs the request and resumes on the loop thread.
    struct fetch {
        Loop &loop;
        Http &http;
        Request req;
        Response res;

        bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) {
            http.request(std::move(req), [this, h](Response r) {
                loop.post([this, h, r = std::move(r)]() mutable {
                    res = std::move(r);
                    h.resume();
                });
            });
        }
        Response await_resume() {
            return std::move(res);
        }
    };

    // At most three attempts, with 300 ms and 900 ms waits. HTTP responses are never retried.
    Task<Response> fetch_retry(Loop &loop, Http &http, Request req, Retry retry = Retry::network);

} // namespace lyren::net
