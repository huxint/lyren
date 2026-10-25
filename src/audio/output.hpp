#pragma once
#include <spa/utils/hook.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "audio/decoder.hpp"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

namespace lyren::audio {

// A PipeWire playback stream fed from a one-second ring. The decoder thread writes, PipeWire's realtime thread reads.
class Output {
public:
    // Both run on PipeWire's (non-realtime) thread.
    struct Events {
        std::function<void()> drained;          // the last frame after finish() has played
        std::function<void(float)> volume;      // linear volume changed elsewhere (desktop mixer)
    };

    // wake is bumped and notified when space frees up while set_waiting(true).
    Output(std::atomic<uint32_t>& wake, Events events);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // Decoder thread.
    bool start(Format f);  // new track: drops queued audio; a different format recreates the stream
    void flush(uint64_t first_frame);  // seek: drops queued audio, the next written frame is first_frame
    size_t space() const;
    size_t capacity() const { return cap_; }
    void write(const float* frames, size_t n);  // n <= space()
    void finish();  // end of stream after the last write
    void set_waiting(bool w) { waiting_.store(w, std::memory_order_relaxed); }

    // Main thread.
    void set_active(bool on);
    void set_volume(float linear);
    void set_name(std::string name);  // media.name, what mixers show for the stream
    uint64_t position();  // frames played out of the speaker
    size_t queued() const;  // frames in the ring

private:
    friend struct StreamEvents;
    void process();
    void apply_volume();

    std::atomic<uint32_t>& wake_;
    Events ev_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    Format fmt_;
    std::atomic<bool> active_{false}, waiting_{false}, volume_ready_{false};
    float volume_ = 1;
    std::string name_ = "Lyren";
    std::chrono::steady_clock::time_point volume_set_;

    std::vector<float> ring_;
    size_t cap_ = 0;  // frames
    alignas(64) std::atomic<uint64_t> w_{0};
    alignas(64) std::atomic<uint64_t> r_{0};
    // A flush is published as (drop_to, base) then a new flush_seq_; the reader applies it before its next read.
    std::atomic<uint64_t> drop_to_{0}, base_{0}, eos_{UINT64_MAX}, played_{0};
    std::atomic<uint32_t> flush_seq_{0}, flush_applied_{0};
    uint32_t flush_seen_ = 0;  // realtime thread
    bool draining_ = false;    // realtime thread
};

}  // namespace lyren::audio
