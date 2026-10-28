#include "core/loop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <array>

namespace lyren {

    Loop::Loop()
    : epoll_(epoll_create1(EPOLL_CLOEXEC)),
      timerfd_(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)),
      eventfd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
        watch(timerfd_, EPOLLIN, [this](uint32_t) {
            uint64_t n;
            read(timerfd_, &n, sizeof n);
            run_timers();
        });
        watch(eventfd_, EPOLLIN, [this](uint32_t) {
            uint64_t n;
            read(eventfd_, &n, sizeof n);
            run_posted();
        });
    }

    Loop::~Loop() {
        close(eventfd_);
        close(timerfd_);
        close(epoll_);
    }

    void Loop::watch(int fd, uint32_t events, FdHandler fn) {
        epoll_event ev{.events = events, .data = {.fd = fd}};
        epoll_ctl(epoll_, watches_.contains(fd) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev);
        watches_.insert_or_assign(fd, std::move(fn));
    }

    void Loop::unwatch(int fd) {
        epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr);
        watches_.erase(fd);
    }

    Loop::TimerId Loop::at(Clock::time_point when, std::move_only_function<void() &> fn) {
        TimerId id = next_timer_++;
        auto at = timer_queue_.emplace(when, id);
        timers_.emplace(id, std::pair{at, std::move(fn)});
        if (at == timer_queue_.begin()) {
            arm_timer();
        }
        return id;
    }

    void Loop::cancel(TimerId id) {
        auto it = timers_.find(id);
        if (it == timers_.end()) {
            return;
        }
        bool earliest = it->second.first == timer_queue_.begin();
        timer_queue_.erase(it->second.first);
        timers_.erase(it);
        if (earliest) {
            arm_timer();
        }
    }

    void Loop::arm_timer() {
        itimerspec spec{};
        if (!timer_queue_.empty()) {
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timer_queue_.begin()->first.time_since_epoch()).count();
            spec.it_value = {.tv_sec = ns / 1'000'000'000, .tv_nsec = ns % 1'000'000'000};
            if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
                spec.it_value.tv_nsec = 1;
            }
        }
        timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &spec, nullptr);
    }

    void Loop::run_timers() {
        auto now = Clock::now();
        while (!timer_queue_.empty() && timer_queue_.begin()->first <= now) {
            auto timer = timers_.extract(timer_queue_.begin()->second);
            timer_queue_.erase(timer.mapped().first);
            timer.mapped().second();
        }
        arm_timer();
    }

    void Loop::post(std::move_only_function<void() &> fn) {
        {
            std::lock_guard lock(post_mutex_);
            posted_.push_back(std::move(fn));
        }
        uint64_t one = 1;
        write(eventfd_, &one, sizeof one);
    }

    void Loop::run_posted() {
        std::vector<std::move_only_function<void() &>> batch;
        {
            std::lock_guard lock(post_mutex_);
            batch.swap(posted_);
        }
        for (auto &fn : batch) {
            fn();
        }
    }

    void Loop::defer(std::move_only_function<void() &> fn) {
        deferred_.push_back(std::move(fn));
    }

    void Loop::set_poll_hook(PollHook hook) {
        hook_ = std::move(hook);
        epoll_event ev{.events = EPOLLIN, .data = {.fd = hook_.fd}};
        epoll_ctl(epoll_, EPOLL_CTL_ADD, hook_.fd, &ev);
    }

    void Loop::run() {
        running_ = true;
        std::array<epoll_event, 16> events;
        while (running_) {
            while (!deferred_.empty()) {
                auto batch = std::move(deferred_);
                deferred_.clear();
                for (auto &fn : batch) {
                    fn();
                }
            }
            if (!running_) {
                break;
            }
            if (hook_.before) {
                hook_.before();
            }
            if (!running_) {
                if (hook_.after) {
                    hook_.after(false);
                }
                break;
            }
            int n = epoll_wait(epoll_, events.data(), static_cast<int>(events.size()), deferred_.empty() ? -1 : 0);
            if (hook_.after) {
                bool readable = false;
                for (int i = 0; i < n; ++i) {
                    readable |= events[i].data.fd == hook_.fd;
                }
                hook_.after(readable);
            }
            for (int i = 0; i < n; ++i) {
                auto it = watches_.find(events[i].data.fd);
                if (it != watches_.end()) {
                    auto fn = it->second;
                    fn(events[i].events);
                }
            }
        }
    }

} // namespace lyren
