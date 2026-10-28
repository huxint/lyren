#include <doctest/doctest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <thread>
#include <memory>
#include <vector>

#include "core/loop.hpp"
#include "core/task.hpp"

using namespace lyren;

TEST_CASE("posted deferred and timed work can transfer exclusive ownership") {
    Loop loop;
    int result = 0;
    auto value = std::make_unique<int>(42);
    std::thread sender([&loop, &result, value = std::move(value)]() mutable {
        loop.post([&loop, &result, value = std::move(value)]() mutable {
            loop.defer([&loop, &result, value = std::move(value)]() mutable {
                loop.after(1ms, [&loop, &result, value = std::move(value)] {
                    result = *value;
                    loop.quit();
                });
            });
        });
    });
    loop.run();
    sender.join();
    CHECK(result == 42);
}

TEST_CASE("timers fire in deadline order and can be cancelled") {
    Loop loop;
    std::string order;
    loop.after(30ms, [&] {
        order += "c";
        loop.quit();
    });
    loop.after(10ms, [&] {
        order += "a";
    });
    auto id = loop.after(20ms, [&] {
        order += "x";
    });
    loop.after(15ms, [&] {
        order += "b";
    });
    loop.cancel(id);
    loop.run();
    CHECK(order == "abc");
}

TEST_CASE("cancelling the earliest timer does not wake the loop at its old deadline") {
    Loop loop;
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int wakeups = 0;
    loop.set_poll_hook({.fd = fd, .after = [&](bool) {
                            ++wakeups;
                        }});
    auto cancelled = loop.at(Clock::now() - 1s, [] {});
    loop.after(100ms, [&] {
        loop.quit();
    });
    loop.cancel(cancelled);
    loop.run();
    close(fd);
    CHECK(wakeups == 1);
}

TEST_CASE("quitting in the before-poll hook cancels the prepared read without waiting") {
    Loop loop;
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    bool prepared = false, waited = false;
    loop.set_poll_hook({.fd = fd,
                        .before =
                            [&] {
                                prepared = true;
                                loop.quit();
                            },
                        .after =
                            [&](bool readable) {
                                CHECK_FALSE(readable);
                                prepared = false;
                            }});
    loop.after(20ms, [&] {
        waited = true;
    });
    loop.run();
    close(fd);
    CHECK_FALSE(prepared);
    CHECK_FALSE(waited);
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

static Task<int> add_later(Loop &loop, int a, int b) {
    co_await lyren::sleep{loop, 5ms};
    co_return a + b;
}

static Task<void> chain(Loop &loop, int &out) {
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

static Task<int> finish_later(Loop &loop, int value, Clock::duration delay, std::vector<int> &order) {
    co_await lyren::sleep{loop, delay};
    order.push_back(value);
    co_return value;
}

TEST_CASE("both starts its tasks together and preserves result order") {
    Loop loop;
    std::vector<int> order;
    std::pair<int, int> out;
    spawn([](Loop *loop, std::vector<int> *order, std::pair<int, int> *out) -> Task<void> {
        *out = co_await both(finish_later(*loop, 1, 8ms, *order), finish_later(*loop, 2, 2ms, *order));
        loop->quit();
    }(&loop, &order, &out));
    loop.run();
    CHECK(order == (std::vector<int>{2, 1}));
    CHECK(out == std::pair{1, 2});
}

static Task<int> immediate(int value) {
    co_return value;
}

TEST_CASE("both also joins tasks that complete before it suspends") {
    std::pair<int, int> out;
    spawn([](std::pair<int, int> *out) -> Task<void> {
        *out = co_await both(immediate(3), immediate(4));
    }(&out));
    CHECK(out == std::pair{3, 4});
}
