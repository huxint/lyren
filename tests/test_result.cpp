#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/task.hpp"

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

TEST_CASE("ok() unwraps move-only values and skips downstream steps on error") {
    Loop loop;
    std::vector<int> order;
    auto chain = [&](bool fail_first, bool fail_second) -> Task<Result<std::unique_ptr<int>>> {
        auto v =
            co_await ok(co_await later(loop, fail_first ? Result<std::unique_ptr<int>>{std::unexpected("first error")} : Result<std::unique_ptr<int>>{std::make_unique<int>(40)}));
        order.push_back(1);
        co_await ok(co_await later(loop, fail_second ? Result<void>{std::unexpected("second error")} : Result<void>{}));
        *v += 2;
        order.push_back(2);
        co_return v;
    };

    SUBCASE("success path preserves order and move-only ownership") {
        Result<std::unique_ptr<int>> out = std::unexpected("not completed");
        spawn(collect(loop, chain(false, false), out));
        loop.run();
        REQUIRE(out);
        CHECK(**out == 42);
        CHECK(order == std::vector{1, 2});
    }

    SUBCASE("input error skips downstream steps") {
        Result<std::unique_ptr<int>> out = std::unexpected("not completed");
        spawn(collect(loop, chain(true, false), out));
        loop.run();
        REQUIRE_FALSE(out);
        CHECK(out.error() == "first error");
        CHECK(order.empty());
    }

    SUBCASE("downstream error stops before completion") {
        Result<std::unique_ptr<int>> out = std::unexpected("not completed");
        spawn(collect(loop, chain(false, true), out));
        loop.run();
        REQUIRE_FALSE(out);
        CHECK(out.error() == "second error");
        CHECK(order == std::vector{1});
    }
}

TEST_CASE("ok() unwraps results and early-returns errors while destroying coroutine locals") {
    struct Guard {
        int &destroyed;
        ~Guard() {
            ++destroyed;
        }
    };
    Loop loop;
    int destroyed = 0, after_error = 0;

    auto pipeline = [&](bool fail_first, bool fail_void) -> Task<Result<int>> {
        Guard g{destroyed};
        const int a = co_await ok(co_await later(loop, fail_first ? Result<int>{std::unexpected("first failed")} : Result<int>{20}));
        co_await ok(co_await later(loop, fail_void ? Result<void>{std::unexpected("void failed")} : Result<void>{}));
        ++after_error;
        const int b = co_await ok(Result<int>{22});
        co_return a + b;
    };

    SUBCASE("success path unwraps values and void") {
        Result<int> out = std::unexpected("pending");
        spawn(collect(loop, pipeline(false, false), out));
        loop.run();
        REQUIRE(out);
        CHECK(*out == 42);
        CHECK(after_error == 1);
        CHECK(destroyed == 1);
    }

    SUBCASE("error in value step short-circuits and destroys locals") {
        Result<int> out = std::unexpected("pending");
        spawn(collect(loop, pipeline(true, false), out));
        loop.run();
        REQUIRE_FALSE(out);
        CHECK(out.error() == "first failed");
        CHECK(after_error == 0);
        CHECK(destroyed == 1);
    }

    SUBCASE("error in void step short-circuits and destroys locals") {
        Result<int> out = std::unexpected("pending");
        spawn(collect(loop, pipeline(false, true), out));
        loop.run();
        REQUIRE_FALSE(out);
        CHECK(out.error() == "void failed");
        CHECK(after_error == 0);
        CHECK(destroyed == 1);
    }

    SUBCASE("short-circuit works inside both()") {
        Result<std::pair<Result<int>, Result<int>>> out = std::unexpected("pending");
        auto run_both = [&]() -> Task<Result<std::pair<Result<int>, Result<int>>>> {
            co_return co_await both(pipeline(true, false), pipeline(false, false));
        };
        spawn(collect(loop, run_both(), out));
        loop.run();
        REQUIRE(out);
        CHECK_FALSE(out->first);
        CHECK(out->first.error() == "first failed");
        REQUIRE(out->second);
        CHECK(*out->second == 42);
        CHECK(destroyed == 2);
    }
}
