#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "core/loop.hpp"
#include "core/task.hpp"

using namespace lyren;

TEST_CASE("timers fire in deadline order and can be cancelled") {
    Loop loop;
    std::string order;
    loop.after(30ms, [&] {
        order += "c";
        loop.quit();
    });
    loop.after(10ms, [&] { order += "a"; });
    auto id = loop.after(20ms, [&] { order += "x"; });
    loop.after(15ms, [&] { order += "b"; });
    loop.cancel(id);
    loop.run();
    CHECK(order == "abc");
}

TEST_CASE("post runs on the loop thread") {
    Loop loop;
    std::thread::id ran_on;
    std::thread t([&] {
        loop.post([&] {
            ran_on = std::this_thread::get_id();
            loop.quit();
        });
    });
    loop.run();
    t.join();
    CHECK(ran_on == std::this_thread::get_id());
}

static Task<int> add_later(Loop& loop, int a, int b) {
    co_await sleep{loop, 5ms};
    co_return a + b;
}

static Task<void> chain(Loop& loop, int& out) {
    int x = co_await add_later(loop, 1, 2);
    int y = co_await add_later(loop, x, 10);
    out = y;
    loop.quit();
}

TEST_CASE("tasks await timers and each other") {
    Loop loop;
    int out = 0;
    spawn(chain(loop, out));
    loop.run();
    CHECK(out == 13);
}

static Task<int> finish_later(Loop& loop, int value, Clock::duration delay, std::vector<int>& order) {
    co_await sleep{loop, delay};
    order.push_back(value);
    co_return value;
}

TEST_CASE("both starts its tasks together and preserves result order") {
    Loop loop;
    std::vector<int> order;
    std::pair<int, int> out;
    spawn([](Loop* loop, std::vector<int>* order, std::pair<int, int>* out) -> Task<void> {
        *out = co_await both(finish_later(*loop, 1, 8ms, *order), finish_later(*loop, 2, 2ms, *order));
        loop->quit();
    }(&loop, &order, &out));
    loop.run();
    CHECK(order == (std::vector<int>{2, 1}));
    CHECK(out == std::pair{1, 2});
}

static Task<int> immediate(int value) { co_return value; }

TEST_CASE("both also joins tasks that complete before it suspends") {
    std::pair<int, int> out;
    spawn([](std::pair<int, int>* out) -> Task<void> { *out = co_await both(immediate(3), immediate(4)); }(&out));
    CHECK(out == std::pair{3, 4});
}
