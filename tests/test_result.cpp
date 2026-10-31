#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/result_task.hpp"

using namespace lyren;

namespace {

    template <class T>
    Task<Result<T>> later(Loop &loop, Result<T> result) {
        co_await lyren::sleep{loop, 1ms};
        co_return result;
    }

    template <class T>
    Task<void> collect(Loop &loop, Task<Result<T>> task, Result<T> &out) {
        out = co_await std::move(task);
        loop.quit();
    }

} // namespace

TEST_CASE("async result chains own values and coroutine captures across suspension") {
    Loop loop;
    std::vector<int> order;
    Result<std::unique_ptr<int>> out = std::unexpected("not completed");
    auto task = and_then(later(loop, Result<std::unique_ptr<int>>{std::make_unique<int>(40)}),
                         [increment = std::make_unique<int>(2), &loop, &order](std::unique_ptr<int> value) -> Task<Result<std::unique_ptr<int>>> {
                             order.push_back(1);
                             co_await lyren::sleep{loop, 1ms};
                             *value += *increment;
                             order.push_back(2);
                             co_return value;
                         });
    CHECK(order.empty());
    spawn(collect(loop, std::move(task), out));
    loop.run();
    REQUIRE(out);
    CHECK(**out == 42);
    CHECK(order == std::vector{1, 2});
}

TEST_CASE("an input error skips the async continuation and preserves its message") {
    Loop loop;
    int calls = 0;
    Result<std::string> out = std::unexpected("not completed");
    auto next = [&](int) {
        ++calls;
        return later(loop, Result<std::string>{"must not run"});
    };
    Result<int> input = std::unexpected("request failed: original message");
    SUBCASE("a ready result") {
        loop.defer([&] {
            spawn(collect(loop, and_then(std::move(input), next), out));
        });
    }
    SUBCASE("an awaited result") {
        spawn(collect(loop, and_then(later(loop, std::move(input)), next), out));
    }
    loop.run();
    REQUIRE_FALSE(out);
    CHECK(out.error() == "request failed: original message");
    CHECK(calls == 0);
}

TEST_CASE("a downstream error skips later steps in an async result chain") {
    Loop loop;
    int first_calls = 0, last_calls = 0;
    Result<void> out;
    auto first = and_then(later(loop, Result<int>{42}), [&](int value) {
        ++first_calls;
        CHECK(value == 42);
        return later(loop, Result<std::string>{std::unexpected("parse failed")});
    });
    auto chain = and_then(std::move(first), [&](std::string) {
        ++last_calls;
        return later(loop, Result<void>{});
    });
    spawn(collect(loop, std::move(chain), out));
    loop.run();
    REQUIRE_FALSE(out);
    CHECK(out.error() == "parse failed");
    CHECK(first_calls == 1);
    CHECK(last_calls == 0);
}

TEST_CASE("async result chains can continue after a void result") {
    Loop loop;
    int calls = 0;
    Result<int> out = std::unexpected("not completed");
    auto next = [&] {
        ++calls;
        return later(loop, Result<int>{42});
    };
    Result<void> input;
    bool failed = false;
    SUBCASE("success") {}
    SUBCASE("failure") {
        failed = true;
        input = std::unexpected("write failed");
    }
    spawn(collect(loop, and_then(later(loop, std::move(input)), next), out));
    loop.run();
    if (failed) {
        REQUIRE_FALSE(out);
        CHECK(out.error() == "write failed");
        CHECK(calls == 0);
    } else {
        REQUIRE(out);
        CHECK(*out == 42);
        CHECK(calls == 1);
    }
}

TEST_CASE("discarding an unstarted result chain releases its captures without starting work") {
    struct Capture {
        int &destroyed;
        ~Capture() {
            ++destroyed;
        }
    };
    Loop loop;
    int destroyed = 0, calls = 0;
    {
        auto task = and_then(later(loop, Result<int>{42}), [capture = std::make_unique<Capture>(destroyed), &loop, &calls](int value) {
            ++calls;
            return later(loop, Result<int>{value});
        });
        CHECK(destroyed == 0);
        CHECK(calls == 0);
    }
    CHECK(destroyed == 1);
    CHECK(calls == 0);
}
