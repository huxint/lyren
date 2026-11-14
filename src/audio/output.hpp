#pragma once
#include <spa/utils/hook.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/meter.hpp"
#include "core/result.hpp"

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
            std::move_only_function<void()> drained;     // the last frame after finish() has played
            std::move_only_function<void(float)> volume; // linear volume changed elsewhere (desktop mixer)
        };

        // wake is bumped and notified when space frees up while set_waiting(true).
        Output(std::atomic<uint32_t> &wake, Events events);
        ~Output();
        Output(const Output &) = delete;
        Output &operator=(const Output &) = delete;

        // Decoder thread.
        Result<void> start(Format f);     // checks PipeWire's channel limit, then starts or recreates the stream
        void flush(uint64_t first_frame); // seek: drops queued audio, the next written frame is first_frame
        size_t space() const;
        size_t capacity() const {
            return cap_;
        }
        void write(const float *frames, size_t n); // n <= space()
        void finish();                             // end of stream after the last write
        void set_waiting(bool w) {
            waiting_.store(w, std::memory_order_relaxed);
        }

        // Main thread.
        void set_active(bool on);
        void set_volume(float linear);
        void set_gain(float linear) {
            gain_.store(linear, std::memory_order_relaxed);
        } // loudness, from the next cycle
        void set_name(std::string name); // media.name, what mixers show for the stream
        uint64_t position();             // frames played out of the speaker
        size_t queued() const;           // frames in the ring
        // Cumulative audio sent to PipeWire; seeks and silence inserted during underruns do not count.
        double played_seconds() const {
            return static_cast<double>(played_ns_.load(std::memory_order_relaxed)) * 1e-9;
        }
        // The audio last sent to PipeWire, after the loudness gain and before the stream volume. Read once a frame
        // for meters: the four values need not come from the same cycle.
        Levels levels() const {
            return {level_[0].load(std::memory_order_relaxed), level_[1].load(std::memory_order_relaxed), level_[2].load(std::memory_order_relaxed),
                    level_[3].load(std::memory_order_relaxed)};
        }

    private:
        friend struct StreamEvents;
        void process();
        void apply_volume();
        void publish(Levels l);

        std::atomic<uint32_t> &wake_;
        Events ev_;
        pw_thread_loop *loop_ = nullptr;
        pw_context *context_ = nullptr;
        pw_core *core_ = nullptr;
        pw_stream *stream_ = nullptr;
        spa_hook listener_{};
        Format fmt_;
        std::atomic<bool> active_{false}, waiting_{false}, volume_ready_{false};
        float volume_ = 1;
        std::atomic<float> gain_{1};
        std::string name_ = "Lyren";
        std::chrono::steady_clock::time_point volume_set_;

        std::vector<float> ring_;
        size_t cap_ = 0; // frames; equals fmt_.rate once a stream exists
        // Monotonic frame counters: r_ <= w_ <= r_ + cap_. Separate producer and consumer cache lines.
        // Fixed across translation units and compiler tuning; the supported x86-64 targets use 64-byte lines.
        static constexpr size_t cache_line_bytes = 64;
        alignas(cache_line_bytes) std::atomic<uint64_t> w_{0};
        // A flush is published as (drop_to, base) then a new flush_seq_; the reader applies it before its next read.
        std::atomic<uint64_t> drop_to_{0}, base_{0}, eos_{UINT64_MAX};
        std::atomic<uint32_t> flush_seq_{0};

        alignas(cache_line_bytes) std::atomic<uint64_t> r_{0};
        std::atomic<uint64_t> played_{0};
        std::atomic<uint64_t> played_ns_{0};
        std::atomic<uint32_t> flush_applied_{0};
        uint32_t flush_seen_ = 0; // realtime thread
        bool draining_ = false;   // realtime thread
        Meter meter_;             // realtime thread
        std::array<std::atomic<float>, 4> level_{};
    };

} // namespace lyren::audio
