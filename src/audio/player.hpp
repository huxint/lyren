#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/output.hpp"
#include "audio/spool.hpp"
#include "core/loop.hpp"

namespace lyren::audio {

    // Playback of one track at a time. The public API and on_change belong to the loop thread; decoding runs on its own
    // thread and output on PipeWire's.
    class Player {
    public:
        enum class State : uint8_t {
            idle,
            loading,
            playing,
            paused,
            ended,
            failed
        };

        Player(Loop &loop, net::Http &http);
        ~Player();
        Player(const Player &) = delete;
        Player &operator=(const Player &) = delete;

        std::move_only_function<void()> on_change;       // state, duration, volume or error changed
        std::move_only_function<void(double)> on_seeked; // confirmed file position, in seconds

        void open(std::shared_ptr<Spool> spool, bool play = true, std::optional<crypto::Aes128Key> key = {});
        void open_url(std::string url, bool play = true, std::optional<crypto::Aes128Key> key = {}) {
            open(Spool::url(http_, std::move(url)), play, key);
        }
        void open_file(const std::string &path, bool play = true) {
            open(Spool::file(path), play);
        }
        void play();
        void pause();
        void toggle() {
            state_ == State::playing ? pause() : play();
        }
        void seek(double seconds);
        void stop();
        void set_volume(float v); // slider position 0..1; the stream gets v³
        void set_gain(float linear) {
            out_.set_gain(linear);
        } // loudness correction applied to the samples
        void set_title(std::string title) {
            out_.set_name(std::move(title));
        }

        State state() const {
            return state_;
        }
        double position(); // seconds
        double played_seconds() const {
            return out_.played_seconds();
        }
        double duration() const {
            return duration_;
        }
        float volume() const {
            return volume_;
        }
        bool buffering() const; // playing, but starved by the network
        // What the meters show: the loudness of the audio going out now, silent unless it plays.
        Levels levels() const {
            return state_ == State::playing ? out_.levels() : Levels{};
        }
        const std::string &error() const {
            return error_;
        }
        Format format() const {
            return format_;
        }
        std::string_view codec() const {
            return codec_;
        }
        const Spool *spool() const {
            return spool_.get();
        }
        double latency_ms() {
            return state_ == State::playing || state_ == State::paused ? out_.latency_ms() : -1.0;
        }
        int bitrate_kbps() const {
            if (!spool_ || duration_ <= 0) {
                return 0;
            }
            const uint64_t total = spool_->progress().second;
            return total > 0 ? static_cast<int>(std::llround(static_cast<double>(total) * 8.0 / (duration_ * 1000.0))) : 0;
        }

    private:
        struct Cmd {
            enum Kind {
                open,
                seek,
                stop,
                quit
            } kind;
            uint64_t gen = 0;
            std::shared_ptr<Spool> spool;
            double seconds = 0;
            uint64_t seq = 0;
            std::optional<crypto::Aes128Key> key;
        };
        void push(Cmd c);
        void run();
        void changed();

        Loop &loop_;
        net::Http &http_;
        std::atomic<uint32_t> wake_{0};
        Output out_;

        // Loop thread.
        uint64_t gen_ = 0, seek_seq_ = 0;
        State state_ = State::idle;
        bool want_play_ = false;
        double duration_ = 0, last_pos_ = 0;
        float volume_ = 1;
        Format format_;
        std::string_view codec_;
        std::string error_;
        std::shared_ptr<Spool> spool_;

        std::mutex m_;
        std::vector<Cmd> cmds_;
        std::thread thread_;
    };

} // namespace lyren::audio
