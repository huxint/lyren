#pragma once
#include <coroutine>

#include "core/loop.hpp"
#include "net/http.hpp"

namespace lyren::net {

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

} // namespace lyren::net
