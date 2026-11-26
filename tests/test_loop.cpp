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
    std::thread::id ran_on;
    auto value = std::make_unique<int>(42);
    std::thread sender([&loop, &result, &ran_on, value = std::move(value)]() mutable {
        loop.post([&loop, &result, &ran_on, value = std::move(value)]() mutable {
            ran_on = std::this_thread::get_id();
            loop.defer([&loop, &result, value = std::move(value)]() mutable {
                loop.after(1ms, [&loop, &result, value = std::move(value)] {
                    result = *value;
                    loop.quit();
                });
            });
        });
    });
    sender.join();
    loop.run();
    CHECK(result == 42);
    CHECK(ran_on == std::this_thread::get_id());
}

TEST_CASE("concurrent posts and posts from a running callback all reach the loop exactly once") {
    Loop loop;
    constexpr int producers = 4, count = 500;
    int completed = 0;
    auto finish = [&] {
        if (++completed == producers * count * 2) {
            loop.quit();
        }
    };
    std::vector<std::jthread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < count; ++i) {
                loop.post([&] {
                    finish();
                    loop.post(finish);
                });
            }
        });
    }
    auto timeout = loop.after(3s, [&] {
        loop.quit();
    });
    loop.run();
    loop.cancel(timeout);
    threads.clear();
    CHECK(completed == producers * count * 2);
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

TEST_CASE("a timer installed by the prepare-read hook wakes the same poll") {
    Loop loop;
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    bool late = false;
    loop.after(2s, [&] {
        late = true;
    });
    loop.set_poll_hook({.fd = fd, .before = [&] {
                            loop.after(0ms, [&] {
                                loop.quit();
                            });
                        }});
    loop.run();
    close(fd);
    CHECK_FALSE(late);
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

static Task<int> immediate(int value) {
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
    {
        std::pair<int, int> out;
        spawn([](std::pair<int, int> *out) -> Task<void> {
            *out = co_await both(immediate(3), immediate(4));
        }(&out));
        CHECK(out == std::pair{3, 4});
    }
}

TEST_CASE("destroying suspended tasks cancels sleep and both branches cleanly") {
    Loop loop;
    bool woke_a = false, woke_b = false;
    int destroyed = 0;
    struct Guard {
        int &n;
        ~Guard() {
            ++n;
        }
    };
    auto make_branch = [&](Clock::duration d, bool &woke) -> Task<int> {
        Guard g{destroyed};
        co_await lyren::sleep{loop, d};
        woke = true;
        co_return 1;
    };
    {
        Task<std::pair<int, int>> active;
        CHECK_FALSE(active);
        active = both(make_branch(50ms, woke_a), make_branch(60ms, woke_b));
        CHECK(active);
        // Start and suspend inside a parent task that is destroyed when replaced.
        Task<void> runner;
        auto run_both = [](Task<std::pair<int, int>> t) -> Task<void> {
            co_await std::move(t);
        };
        runner = run_both(std::move(active));
        // Replace with an empty task before awaiting: destroys unstarted runner cleanly.
        runner = {};
    }
    CHECK(destroyed == 0);
    // Now actually start a sleep + both, then destroy while suspended.
    struct Stepper {
        struct promise_type {
            Stepper get_return_object() {
                return Stepper{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_never initial_suspend() noexcept {
                return {};
            }
            std::suspend_always final_suspend() noexcept {
                return {};
            }
            void return_void() noexcept {}
            void unhandled_exception() {
                std::terminate();
            }
        };
        std::coroutine_handle<promise_type> h;
        ~Stepper() {
            if (h) {
                h.destroy();
            }
        }
    };
    {
        auto s = [](Task<std::pair<int, int>> t) -> Stepper {
            co_await std::move(t);
        }(both(make_branch(15ms, woke_a), make_branch(20ms, woke_b)));
        CHECK(destroyed == 0);
    }
    // Both branches' local guards were destroyed immediately when the suspended Stepper died.
    CHECK(destroyed == 2);
    loop.after(30ms, [&] {
        loop.quit();
    });
    loop.run();
    CHECK_FALSE(woke_a);
    CHECK_FALSE(woke_b);
}
