#include <doctest/doctest.h>

#include <thread>

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
