#include "audio/output.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "core/log.hpp"

namespace lyren::audio {

    // PipeWire's stream callbacks; all but process run on PipeWire's main thread with its lock held.
    struct StreamEvents {
        static Output *self(void *ud) {
            return static_cast<Output *>(ud);
        }

        // The volume control only exists once the format is negotiated; setting it earlier is silently dropped.
        static void state_changed(void *ud, pw_stream_state, pw_stream_state state, const char *) {
            if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING) {
                return;
            }
            self(ud)->apply_volume();
            self(ud)->volume_ready_.store(true, std::memory_order_release);
        }

        static void control_info(void *ud, uint32_t id, const pw_stream_control *c) {
            auto *o = self(ud);
            if (id != SPA_PROP_channelVolumes || !c->n_values || !o->ev_.volume || !o->volume_ready_) {
                return;
            }
            // Our own changes echo back, late ones while a slider is dragged; only report the ones made elsewhere.
            float v = *std::max_element(c->values, c->values + c->n_values);
            if (std::abs(v - o->volume_) < 1e-3f || std::chrono::steady_clock::now() - o->volume_set_ < std::chrono::milliseconds(300)) {
                return;
            }
            o->volume_ = v;
            o->ev_.volume(v);
        }

        static void process(void *ud) {
            self(ud)->process();
        }

        static void drained(void *ud) {
            if (self(ud)->ev_.drained) {
                self(ud)->ev_.drained();
            }
        }

        static constexpr pw_stream_events events = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = state_changed,
            .control_info = control_info,
            .process = process,
            .drained = drained,
        };
    };

    Output::Output(std::atomic<uint32_t> &wake, Events events)
    : wake_(wake),
      ev_(std::move(events)) {
        pw_init(nullptr, nullptr);
        loop_ = pw_thread_loop_new("lyren-pw", nullptr);
        pw_thread_loop_start(loop_);
    }

    Output::~Output() {
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_destroy(stream_);
        }
        if (core_) {
            pw_core_disconnect(core_);
        }
        if (context_) {
            pw_context_destroy(context_);
        }
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        pw_thread_loop_destroy(loop_);
        pw_deinit();
    }

    Result<void> Output::start(Format f) {
        if (f.channels > SPA_AUDIO_MAX_CHANNELS) {
            return std::unexpected("This audio file has more channels than PipeWire supports");
        }
        pw_thread_loop_lock(loop_);
        if (stream_ && f == fmt_) {
            pw_thread_loop_unlock(loop_);
            flush(0);
            return {};
        }
        if (stream_) {
            // No process callback runs after this returns, so the ring can be rebuilt directly.
            spa_hook_remove(&listener_);
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (!core_) {
            context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
            core_ = pw_context_connect(context_, nullptr, 0);
            if (!core_) {
                log::error("cannot connect to PipeWire");
                pw_thread_loop_unlock(loop_);
                return std::unexpected("PipeWire is not running");
            }
        }
        fmt_ = f;
        volume_ready_ = false;
        cap_ = f.rate;
        ring_.assign(cap_ * f.channels, 0.f);
        w_ = 0, r_ = 0, drop_to_ = 0, base_ = 0, played_ = 0, eos_ = UINT64_MAX;
        flush_seen_ = flush_seq_.load(), flush_applied_ = flush_seen_;
        draining_ = false;
        meter_.reset(f.rate);
        publish({});

        // state.restore-props=false keeps WirePlumber from restoring an old volume over ours.
        pw_properties *props =
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "Lyren", PW_KEY_APP_ICON_NAME, "lyren",
                              PW_KEY_NODE_NAME, "lyren", PW_KEY_NODE_LATENCY, "2048/48000", "state.restore-props", "false", PW_KEY_MEDIA_NAME, name_.c_str(), nullptr);
        stream_ = pw_stream_new(core_, "Lyren", props);
        pw_stream_add_listener(stream_, &listener_, &StreamEvents::events, this);
        alignas(SPA_POD_ALIGN) std::array<uint8_t, 1024> buf;
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf.data(), buf.size());
        spa_audio_info_raw info{.format = SPA_AUDIO_FORMAT_F32, .rate = f.rate, .channels = f.channels};
        if (f.channels == 1) {
            info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        } else if (f.channels == 2) {
            info.position[0] = SPA_AUDIO_CHANNEL_FL, info.position[1] = SPA_AUDIO_CHANNEL_FR;
        } else {
            info.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
        }
        std::array<const spa_pod *, 1> params = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        auto flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
        if (!active_) {
            flags = flags | PW_STREAM_FLAG_INACTIVE;
        }
        pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, pw_stream_flags(flags), params.data(), static_cast<uint32_t>(params.size()));
        pw_thread_loop_unlock(loop_);
        log::debug("pipewire stream: {} Hz, {} ch", f.rate, f.channels);
        return {};
    }

    void Output::flush(uint64_t first_frame) {
        eos_.store(UINT64_MAX, std::memory_order_relaxed);
        drop_to_.store(w_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        base_.store(first_frame, std::memory_order_relaxed);
        flush_seq_.fetch_add(1, std::memory_order_release);
        pw_thread_loop_lock(loop_);
        // Drops what PipeWire already queued; also ends a drain, after which the stream has to be reactivated.
        pw_stream_flush(stream_, false);
        pw_stream_set_active(stream_, active_);
        pw_thread_loop_unlock(loop_);
    }

    size_t Output::space() const {
        return cap_ - static_cast<size_t>(w_.load(std::memory_order_relaxed) - r_.load(std::memory_order_acquire));
    }

    size_t Output::queued() const {
        return static_cast<size_t>(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire));
    }

    void Output::write(const float *frames, size_t n) {
        uint64_t w = w_.load(std::memory_order_relaxed);
        size_t ch = fmt_.channels;
        for (size_t done = 0; done < n;) {
            size_t at = static_cast<size_t>((w + done) % cap_), run = std::min(n - done, cap_ - at);
            std::copy_n(frames + done * ch, run * ch, ring_.data() + at * ch);
            done += run;
        }
        w_.store(w + n, std::memory_order_release);
    }

    void Output::finish() {
        eos_.store(w_.load(std::memory_order_relaxed), std::memory_order_release);
    }

    void Output::process() {
        pw_buffer *b = pw_stream_dequeue_buffer(stream_);
        if (!b) {
            return;
        }
        spa_data &d = b->buffer->datas[0];
        size_t ch = fmt_.channels, stride = ch * sizeof(float);
        uint64_t n = d.maxsize / stride;
        if (b->requested) {
            n = std::min<uint64_t>(n, b->requested);
        }

        if (!volume_ready_.load(std::memory_order_acquire)) {
            // Nothing is audible before our volume is in place.
            std::fill_n(static_cast<float *>(d.data), static_cast<size_t>(n) * ch, 0.f);
            d.chunk->offset = 0, d.chunk->stride = static_cast<int32_t>(stride), d.chunk->size = static_cast<uint32_t>(n * stride);
            b->size = n;
            pw_stream_queue_buffer(stream_, b);
            return;
        }
        if (uint32_t s = flush_seq_.load(std::memory_order_acquire); s != flush_seen_) {
            flush_seen_ = s;
            r_.store(drop_to_.load(std::memory_order_relaxed), std::memory_order_release);
            played_.store(base_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            draining_ = false;
            meter_.reset(fmt_.rate);
            flush_applied_.store(s, std::memory_order_release);
        }
        uint64_t r = r_.load(std::memory_order_relaxed), k = std::min(n, w_.load(std::memory_order_acquire) - r);
        auto *dst = static_cast<float *>(d.data);
        for (uint64_t done = 0; done < k;) {
            size_t at = static_cast<size_t>((r + done) % cap_), run = std::min<size_t>(static_cast<size_t>(k - done), cap_ - at);
            std::copy_n(ring_.data() + at * ch, run * ch, dst + done * ch);
            done += run;
        }
        if (float g = gain_.load(std::memory_order_relaxed); g != 1) {
            for (size_t i = 0; i < static_cast<size_t>(k) * ch; ++i) {
                dst[i] *= g;
            }
        }
        std::fill_n(dst + k * ch, static_cast<size_t>(n - k) * ch, 0.f);
        publish(meter_.measure(dst, static_cast<size_t>(k), ch));
        r_.store(r + k, std::memory_order_release);
        played_.store(played_.load(std::memory_order_relaxed) + k, std::memory_order_release);
        played_ns_.fetch_add(k * 1000000000 / fmt_.rate, std::memory_order_relaxed);

        d.chunk->offset = 0;
        d.chunk->stride = static_cast<int32_t>(stride);
        d.chunk->size = static_cast<uint32_t>(n * stride);
        b->size = n;
        pw_stream_queue_buffer(stream_, b);

        if (k < n && r + k == eos_.load(std::memory_order_acquire) && !draining_) {
            draining_ = true;
            pw_stream_flush(stream_, true);
        }
        if (waiting_.load(std::memory_order_relaxed) && space() >= cap_ / 4) {
            wake_.fetch_add(1, std::memory_order_release);
            wake_.notify_one();
        }
    }

    void Output::publish(Levels l) {
        level_[0].store(l.all, std::memory_order_relaxed), level_[1].store(l.low, std::memory_order_relaxed);
        level_[2].store(l.mid, std::memory_order_relaxed), level_[3].store(l.high, std::memory_order_relaxed);
    }

    void Output::set_active(bool on) {
        active_ = on;
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_set_active(stream_, on);
        }
        pw_thread_loop_unlock(loop_);
        // A paused stream is not processed, so nothing else would clear the meter.
        if (!on) {
            publish({});
        }
    }

    void Output::set_volume(float linear) {
        pw_thread_loop_lock(loop_);
        volume_ = linear;
        if (stream_ && volume_ready_) {
            apply_volume();
        }
        pw_thread_loop_unlock(loop_);
    }

    void Output::set_name(std::string name) {
        pw_thread_loop_lock(loop_);
        name_ = std::move(name);
        if (stream_) {
            spa_dict_item item = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, name_.c_str());
            spa_dict dict = SPA_DICT_INIT(&item, 1);
            pw_stream_update_properties(stream_, &dict);
        }
        pw_thread_loop_unlock(loop_);
    }

    void Output::apply_volume() {
        std::array<float, SPA_AUDIO_MAX_CHANNELS> v;
        std::fill_n(v.begin(), fmt_.channels, volume_);
        volume_set_ = std::chrono::steady_clock::now();
        pw_stream_set_control(stream_, SPA_PROP_channelVolumes, fmt_.channels, v.data(), 0);
        log::debug("stream volume {:.3f}", volume_);
    }

    uint64_t Output::position() {
        pw_thread_loop_lock(loop_);
        pw_time t{};
        bool ok = stream_ && pw_stream_get_time_n(stream_, &t, sizeof t) == 0 && t.rate.denom;
        uint64_t now = ok ? pw_stream_get_nsec(stream_) : 0;
        double rate = fmt_.rate;
        pw_thread_loop_unlock(loop_);
        if (flush_applied_.load(std::memory_order_acquire) != flush_seq_.load(std::memory_order_acquire)) {
            return base_.load();
        }
        double played = static_cast<double>(played_.load(std::memory_order_acquire));
        if (!ok) {
            return static_cast<uint64_t>(played);
        }
        // Frames handed to PipeWire minus what is still queued, in the resampler and on the way to the device, plus
        // what the device played since the last graph cycle.
        double delay = static_cast<double>(std::max<int64_t>(t.delay, 0)) * t.rate.num * rate / t.rate.denom;
        double since =
            active_ && t.now ? std::clamp(static_cast<double>(static_cast<int64_t>(now - static_cast<uint64_t>(t.now))) * 1e-9 * rate, 0.0, static_cast<double>(t.size)) : 0;
        return static_cast<uint64_t>(std::max(0.0, played - static_cast<double>(t.queued) - static_cast<double>(t.buffered) - delay + since));
    }

} // namespace lyren::audio
