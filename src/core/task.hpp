#pragma once
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "core/loop.hpp"

namespace lyren {

    // Lazy coroutine. Awaiting it starts it; its completion resumes the awaiter.
    template <class T = void>
    class Task;

    namespace detail {
        struct PromiseBase {
            std::coroutine_handle<> continuation;
            struct FinalAwaiter {
                bool await_ready() noexcept {
                    return false;
                }
                template <class P>
                std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
                    auto c = h.promise().continuation;
                    return c ? c : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            std::suspend_always initial_suspend() noexcept {
                return {};
            }
            FinalAwaiter final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                std::terminate();
            }
        };
    } // namespace detail

    template <class T>
    class Task {
    public:
        struct promise_type : detail::PromiseBase {
            std::optional<T> value;
            Task get_return_object() {
                return Task(std::coroutine_handle<promise_type>::from_promise(*this));
            }
            void return_value(T v) {
                value = std::move(v);
            }
        };

        Task(Task &&o) noexcept
        : h_(std::exchange(o.h_, {})) {}
        ~Task() {
            if (h_) {
                h_.destroy();
            }
        }

        bool await_ready() const noexcept {
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            h_.promise().continuation = c;
            return h_;
        }
        T await_resume() {
            return std::move(*h_.promise().value);
        }

    private:
        explicit Task(std::coroutine_handle<promise_type> h)
        : h_(h) {}
        std::coroutine_handle<promise_type> h_;
    };

    template <>
    class Task<void> {
    public:
        struct promise_type : detail::PromiseBase {
            Task get_return_object() {
                return Task(std::coroutine_handle<promise_type>::from_promise(*this));
            }
            void return_void() {}
        };

        Task(Task &&o) noexcept
        : h_(std::exchange(o.h_, {})) {}
        ~Task() {
            if (h_) {
                h_.destroy();
            }
        }

        bool await_ready() const noexcept {
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            h_.promise().continuation = c;
            return h_;
        }
        void await_resume() {}

    private:
        explicit Task(std::coroutine_handle<promise_type> h)
        : h_(h) {}
        std::coroutine_handle<promise_type> h_;
    };

    namespace detail {
        struct Detached {
            struct promise_type {
                Detached get_return_object() {
                    return {};
                }
                std::suspend_never initial_suspend() noexcept {
                    return {};
                }
                std::suspend_never final_suspend() noexcept {
                    return {};
                }
                void return_void() {}
                void unhandled_exception() {
                    std::terminate();
                }
            };
        };
    } // namespace detail

    // Starts a task without awaiting it; its frame is freed when it finishes.
    inline detail::Detached spawn(Task<void> t) {
        co_await std::move(t);
    }

    // Both tasks run on the loop thread; whichever finishes last resumes the caller.
    template <class A, class B>
    Task<std::pair<A, B>> both(Task<A> a, Task<B> b) {
        struct State {
            std::optional<A> a;
            std::optional<B> b;
            int left = 2;
            std::coroutine_handle<> caller;
            void finish() {
                if (--left == 0 && caller) {
                    caller.resume();
                }
            }
        };
        auto state = std::make_shared<State>();
        spawn([](std::shared_ptr<State> s, Task<A> task) -> Task<void> {
            s->a = co_await std::move(task);
            s->finish();
        }(state, std::move(a)));
        spawn([](std::shared_ptr<State> s, Task<B> task) -> Task<void> {
            s->b = co_await std::move(task);
            s->finish();
        }(state, std::move(b)));
        struct Wait {
            State &s;
            bool await_ready() const {
                return s.left == 0;
            }
            void await_suspend(std::coroutine_handle<> h) {
                s.caller = h;
            }
            void await_resume() const {}
        };
        co_await Wait{*state};
        co_return std::pair{std::move(*state->a), std::move(*state->b)};
    }

    // co_await sleep(loop, 200ms)
    struct sleep {
        Loop &loop;
        Clock::duration d;
        bool await_ready() const noexcept {
            return d <= Clock::duration::zero();
        }
        void await_suspend(std::coroutine_handle<> h) {
            loop.after(d, [h] {
                h.resume();
            });
        }
        void await_resume() noexcept {}
    };

} // namespace lyren
