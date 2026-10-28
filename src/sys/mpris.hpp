#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "sys/bus.hpp"

struct sd_bus_slot;

namespace lyren {

    // What the desktop sees of the player.
    struct MprisState {
        enum class Status : uint8_t {
            stopped,
            playing,
            paused
        };
        Status status = Status::stopped;
        std::string track;                     // object path naming the track; empty without one
        std::string title, artist, album, art; // art: cover URL
        double length = 0;                     // s
        double volume = 0;                     // the volume slider, 0..1
        bool loop_one = false, shuffle = false;
        bool can_play = false, can_next = false, can_prev = false, can_seek = false;
    };

    // org.mpris.MediaPlayer2 at /org/mpris/MediaPlayer2: Plasma's media controls, media keys and the lock screen. The
    // same object takes lyren.Instance.Activate(token) from a second launch.
    class Mpris {
    public:
        struct Handlers {
            std::move_only_function<void(std::string_view token)> raise; // empty token: MPRIS Raise, nothing to hand over
            std::move_only_function<void()> quit, play, pause, toggle, stop, next, previous;
            std::move_only_function<void(double)> seek; // absolute, s
            std::move_only_function<void(double)> volume;
            std::move_only_function<void(bool)> loop_one, shuffle;
            std::move_only_function<double()> position;
        };

        // Registers the object, then takes the well-known name; null when another process holds the name.
        static std::unique_ptr<Mpris> create(Bus &bus, Handlers h);
        ~Mpris();
        Mpris(const Mpris &) = delete;
        Mpris &operator=(const Mpris &) = delete;

        // Announces the properties that differ from the last state.
        void update(const MprisState &s);
        void seeked(double position);

    private:
        friend struct MprisCalls;
        Mpris(Bus &bus, Handlers h)
        : bus_(bus),
          h_(std::move(h)) {}

        Bus &bus_;
        Handlers h_;
        MprisState state_;
        sd_bus_slot *slots_[3]{};
    };

    // A second launch hands the launcher's activation token to the running lyren, which comes forward. True when one
    // answered.
    bool hand_over_to_running(Bus &bus);

} // namespace lyren
