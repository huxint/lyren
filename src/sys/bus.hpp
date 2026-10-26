#pragma once
#include <cstdint>

#include "core/loop.hpp"

struct sd_bus;

namespace lyren {

// The session D-Bus connection (sd-bus), served from the loop. get() is null when there is no session bus.
class Bus {
public:
    explicit Bus(Loop& loop);
    ~Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    sd_bus* get() const { return bus_; }
    // After sending outside a bus callback: output that did not fit in the socket waits for it to become writable.
    void arm();

private:
    void dispatch();

    Loop& loop_;
    sd_bus* bus_ = nullptr;
    uint32_t events_ = 0;
    uint64_t timeout_ = UINT64_MAX;
    Loop::TimerId timer_ = 0;
};

}  // namespace lyren
