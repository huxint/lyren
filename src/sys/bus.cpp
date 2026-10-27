#include "sys/bus.hpp"

#include <poll.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>

#include <cstring>

#include "core/log.hpp"

namespace lyren {

    Bus::Bus(Loop &loop)
    : loop_(loop) {
        if (int r = sd_bus_open_user_with_description(&bus_, "Lyren"); r < 0) {
            log::warn("no session bus: {}", std::strerror(-r));
            bus_ = nullptr;
            return;
        }
        arm();
    }

    Bus::~Bus() {
        if (!bus_) {
            return;
        }
        loop_.unwatch(sd_bus_get_fd(bus_));
        loop_.cancel(timer_);
        sd_bus_flush_close_unref(bus_);
    }

    void Bus::dispatch() {
        int r;
        while ((r = sd_bus_process(bus_, nullptr)) > 0) {
        }
        if (r < 0) {
            // The bus went away (the session is ending); nothing more will arrive on this connection.
            log::warn("session bus: {}", std::strerror(-r));
            loop_.unwatch(sd_bus_get_fd(bus_));
            loop_.cancel(timer_);
            return;
        }
        arm();
    }

    void Bus::arm() {
        int ev = sd_bus_get_events(bus_);
        if (ev < 0) {
            return;
        }
        uint32_t events = (ev & POLLIN ? uint32_t(EPOLLIN) : 0) | (ev & POLLOUT ? uint32_t(EPOLLOUT) : 0);
        if (events != events_) {
            events_ = events;
            loop_.watch(sd_bus_get_fd(bus_), events, [this](uint32_t) {
                dispatch();
            });
        }
        uint64_t t = UINT64_MAX;
        sd_bus_get_timeout(bus_, &t);
        if (t != timeout_) {
            // An absolute CLOCK_MONOTONIC time in µs, the clock behind steady_clock.
            timeout_ = t;
            loop_.cancel(timer_);
            timer_ = t == UINT64_MAX ? 0 : loop_.at(Clock::time_point(std::chrono::microseconds(t)), [this] {
                timer_ = 0;
                timeout_ = UINT64_MAX;
                dispatch();
            });
        }
    }

} // namespace lyren
