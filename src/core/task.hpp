#pragma once
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include "core/loop.hpp"
#include "core/result.hpp"

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

        template <class T>
        struct PromiseValue {
            std::optional<T> value;
            void return_value(T v) {
                value = std::move(v);
            }
            T result() {
                return std::move(*value);
            }
        };

        template <>
        struct PromiseValue<void> {
            void return_void() {}
            void result() {}
        };

        template <class T>
        struct is_result : std::false_type {};
        template <class T>
        struct is_result<Result<T>> : std::true_type {};

        template <class T>
        struct Ok {
            Result<T> &&result;

            bool await_ready() const noexcept {
                return result.has_value();
            }
            template <class P>
                requires requires(P &p) { p.value; } && is_result<typename decltype(std::declval<P &>().value)::value_type>::value
            std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
                h.promise().return_value(std::unexpected(std::exchange(result.error(), {})));
                auto c = h.promise().continuation;
                return c ? c : std::noop_coroutine();
            }
            T await_resume() {
                if constexpr (!std::is_void_v<T>) {
                    return *std::move(result);
                }
            }
        };

        struct JoinBranch {
            struct promise_type {
                int *left = nullptr;
                std::coroutine_handle<> *caller = nullptr;
                JoinBranch get_return_object() {
                    return JoinBranch{std::coroutine_handle<promise_type>::from_promise(*this)};
                }
                std::suspend_always initial_suspend() noexcept {
                    return {};
                }
                struct Final {
                    bool await_ready() noexcept {
                        return false;
                    }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                        auto &p = h.promise();
                        if (--(*p.left) == 0 && *p.caller) {
                            return *p.caller;
                        }
                        return std::noop_coroutine();
                    }
                    void await_resume() noexcept {}
                };
                Final final_suspend() noexcept {
                    return {};
                }
                void return_void() noexcept {}
                void unhandled_exception() {
                    std::terminate();
                }
            };

            std::coroutine_handle<promise_type> h;
            explicit JoinBranch(std::coroutine_handle<promise_type> handle) noexcept
            : h(handle) {}
            JoinBranch(JoinBranch &&o) noexcept
            : h(std::exchange(o.h, {})) {}
            JoinBranch(const JoinBranch &) = delete;
            JoinBranch &operator=(const JoinBranch &) = delete;
            ~JoinBranch() {
                if (h) {
                    h.destroy();
                }
            }
            void start(int &left, std::coroutine_handle<> &caller) {
                h.promise().left = &left;
                h.promise().caller = &caller;
                h.resume();
            }
        };

        template <class T>
        JoinBranch join_branch(Task<T> task, std::optional<T> &out) {
            out.emplace(co_await std::move(task));
        }
    } // namespace detail

    template <class T>
    class Task {
    public:
        struct promise_type : detail::PromiseBase, detail::PromiseValue<T> {
            Task get_return_object() {
                return Task(std::coroutine_handle<promise_type>::from_promise(*this));
            }
        };

        Task() noexcept = default;
        Task(Task &&o) noexcept
        : h_(std::exchange(o.h_, {})) {}
        Task &operator=(Task &&o) noexcept {
            if (this != &o) {
                if (h_) {
                    h_.destroy();
                }
                h_ = std::exchange(o.h_, {});
            }
            return *this;
        }
        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;
        ~Task() {
            if (h_) {
                h_.destroy();
            }
        }

        explicit operator bool() const noexcept {
            return static_cast<bool>(h_);
        }
        bool await_ready() const noexcept {
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) noexcept {
            h_.promise().continuation = c;
            return h_;
        }
        T await_resume() {
            // An ok() error suspends inside the task. Release its locals before the awaiter continues.
            Task finished = std::move(*this);
            return finished.h_.promise().result();
        }

    private:
        explicit Task(std::coroutine_handle<promise_type> h)
        : h_(h) {}
        std::coroutine_handle<promise_type> h_;
    };

    // Unwraps a Result inside a Task<Result<U>> coroutine, returning the error early on failure.
    template <class T>
    detail::Ok<T> ok(Result<T> &&r) noexcept {
        return {std::move(r)};
    }

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
    // Destroying the returned task while suspended cancels both branches together.
    template <class A, class B>
    Task<std::pair<A, B>> both(Task<A> a, Task<B> b) {
        std::optional<A> ra;
        std::optional<B> rb;
        int left = 2;
        std::coroutine_handle<> caller;
        auto ba = detail::join_branch(std::move(a), ra);
        auto bb = detail::join_branch(std::move(b), rb);
        struct Wait {
            int &left;
            std::coroutine_handle<> &caller;
            detail::JoinBranch &ba, &bb;
            bool await_ready() const noexcept {
                return false;
            }
            bool await_suspend(std::coroutine_handle<> h) {
                ba.start(left, caller);
                bb.start(left, caller);
                if (left == 0) {
                    return false;
                }
                caller = h;
                return true;
            }
            void await_resume() const noexcept {}
        };
        co_await Wait{left, caller, ba, bb};
        co_return std::pair{std::move(*ra), std::move(*rb)};
    }

    // co_await sleep{loop, 200ms}; destroying the suspended coroutine cancels its timer.
    struct sleep {
        Loop &loop;
        Clock::duration d;
        Loop::TimerId timer = 0;

        sleep(Loop &loop, Clock::duration d) noexcept
        : loop(loop),
          d(d) {}
        sleep(const sleep &) = delete;
        sleep &operator=(const sleep &) = delete;
        ~sleep() {
            if (timer) {
                loop.cancel(timer);
            }
        }
        bool await_ready() const noexcept {
            return d <= Clock::duration::zero();
        }
        void await_suspend(std::coroutine_handle<> h) {
            timer = loop.after(d, [this, h] {
                timer = 0;
                h.resume();
            });
        }
        void await_resume() noexcept {
            timer = 0;
        }
    };

} // namespace lyren
