#include "core/loop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <functional>

namespace lyren {

    Loop::Loop()
    : epoll_(epoll_create1(EPOLL_CLOEXEC)),
      timerfd_(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)),
      eventfd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
        watch(timerfd_, EPOLLIN, [this](uint32_t) {
            uint64_t n;
            static_cast<void>(read(timerfd_, &n, sizeof n));
            armed_deadline_ = Clock::time_point::max();
            run_timers();
        });
        watch(eventfd_, EPOLLIN, [this](uint32_t) {
            uint64_t n;
            static_cast<void>(read(eventfd_, &n, sizeof n));
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
        timers_.emplace(id, std::move(fn));
        timer_heap_.push_back({when, id});
        std::ranges::push_heap(timer_heap_, std::greater<>{});
        return id;
    }

    void Loop::cancel(TimerId id) {
        if (id == 0 || timers_.erase(id) == 0) {
            return;
        }
        if (timer_heap_.size() > timers_.size() * 2 + 64) {
            std::erase_if(timer_heap_, [this](const TimerEntry &e) {
                return !timers_.contains(e.id);
            });
            std::ranges::make_heap(timer_heap_, std::greater<>{});
        }
    }

    void Loop::prune_timers() {
        while (!timer_heap_.empty() && !timers_.contains(timer_heap_.front().id)) {
            std::ranges::pop_heap(timer_heap_, std::greater<>{});
            timer_heap_.pop_back();
        }
    }

    void Loop::arm_timer() {
        prune_timers();
        const Clock::time_point next = timer_heap_.empty() ? Clock::time_point::max() : timer_heap_.front().when;
        if (next == armed_deadline_) {
            return;
        }
        armed_deadline_ = next;
        itimerspec spec{};
        if (next != Clock::time_point::max()) {
            const auto ns = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count());
            spec.it_value = {.tv_sec = ns / 1'000'000'000, .tv_nsec = ns % 1'000'000'000};
        }
        timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &spec, nullptr);
    }

    void Loop::run_timers() {
        const auto now = Clock::now();
        for (;;) {
            prune_timers();
            if (timer_heap_.empty() || timer_heap_.front().when > now) {
                break;
            }
            std::ranges::pop_heap(timer_heap_, std::greater<>{});
            const TimerId id = timer_heap_.back().id;
            timer_heap_.pop_back();
            auto node = timers_.extract(id);
            if (node) {
                node.mapped()();
            }
        }
    }

    void Loop::post(std::move_only_function<void() &> fn) {
        bool wake;
        {
            std::lock_guard lock(post_mutex_);
            wake = posted_.empty();
            posted_.push_back(std::move(fn));
        }
        if (wake) {
            uint64_t one = 1;
            write(eventfd_, &one, sizeof one);
        }
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
            arm_timer();
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
