#pragma once
#include <doctest/doctest.h>

#include "core/task.hpp"

namespace lyren::test {
    template <class T>
    T run(Loop &loop, Task<T> task) {
        std::optional<T> result;
        auto collect = [&]() -> Task<void> {
            result.emplace(co_await std::move(task));
            loop.quit();
        };
        auto pending = collect();
        pending.await_suspend({}).resume();
        if (!result) {
            const auto timeout = loop.after(5s, [&] {
                loop.quit();
            });
            loop.run();
            loop.cancel(timeout);
        }
        REQUIRE(result.has_value());
        return std::move(*result);
    }

    struct Gate {
        std::coroutine_handle<> waiter{};
        bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) {
            REQUIRE_FALSE(waiter);
            waiter = h;
        }
        void await_resume() const noexcept {}
        void release() {
            REQUIRE(waiter);
            std::exchange(waiter, {}).resume();
        }
    };
} // namespace lyren::test
