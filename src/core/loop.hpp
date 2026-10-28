#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lyren {

    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    // Single-threaded epoll loop. Everything except post() must be called from the loop thread.
    class Loop {
    public:
        // Dispatch keeps a copy alive while a handler replaces or removes its own watch.
        using FdHandler = std::function<void(uint32_t events)>;
        using TimerId = uint64_t;

        Loop();
        ~Loop();
        Loop(const Loop &) = delete;
        Loop &operator=(const Loop &) = delete;

        void watch(int fd, uint32_t events, FdHandler fn);
        void unwatch(int fd);

        TimerId at(Clock::time_point when, std::move_only_function<void() &> fn);
        TimerId after(Clock::duration d, std::move_only_function<void() &> fn) {
            return at(Clock::now() + d, std::move(fn));
        }
        void cancel(TimerId id);

        // Runs fn on the loop thread; safe to call from any thread.
        void post(std::move_only_function<void() &> fn);
        // Runs fn once before the loop next sleeps.
        void defer(std::move_only_function<void() &> fn);

        // For wl_display: before() runs right before sleeping (prepare_read + flush); after(readable) runs after
        // waking or cancelling the wait, before any other handler, so a prepared read is never held while handlers run.
        struct PollHook {
            int fd = -1;
            std::move_only_function<void() &> before;
            std::move_only_function<void(bool readable)> after;
        };
        void set_poll_hook(PollHook hook);

        void run();
        void quit() {
            running_ = false;
        }

    private:
        void arm_timer();
        void run_timers();
        void run_posted();

        int epoll_;
        int timerfd_;
        int eventfd_;
        bool running_ = false;
        std::unordered_map<int, FdHandler> watches_;
        using TimerQueue = std::multimap<Clock::time_point, TimerId>;
        TimerQueue timer_queue_;
        std::unordered_map<TimerId, std::pair<TimerQueue::iterator, std::move_only_function<void() &>>> timers_;
        TimerId next_timer_ = 1;
        std::vector<std::move_only_function<void() &>> deferred_;
        std::mutex post_mutex_;
        std::vector<std::move_only_function<void() &>> posted_;
        PollHook hook_;
    };

} // namespace lyren
