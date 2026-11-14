#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lyren::audio {

    // How loud a stretch of audio is: RMS of the whole signal and of its low, middle and high bands. All zero while
    // nothing plays.
    struct Levels {
        float all = 0, low = 0, mid = 0, high = 0;
    };

    // Measures the audio on its way out. Two one-pole low-passes split it at about 180 Hz and 2.2 kHz: a few
    // multiplications a frame and no state but its own, so it can run on the realtime thread.
    class Meter {
    public:
        void reset(uint32_t rate) {
            constexpr float tau = 6.2831853f;
            split_low_ = 1 - std::exp(-tau * 180 / static_cast<float>(rate));
            split_high_ = 1 - std::exp(-tau * 2200 / static_cast<float>(rate));
            low_ = high_ = {};
        }

        // Interleaved frames; the front pair of channels is measured.
        Levels measure(const float *frames, size_t n, size_t channels) {
            if (!n) {
                return {};
            }
            std::array<float, 4> sum{};
            const size_t front = std::min<size_t>(channels, 2);
            for (size_t i = 0; i < n; ++i) {
                for (size_t c = 0; c < front; ++c) {
                    const float x = frames[i * channels + c];
                    low_[c] += split_low_ * (x - low_[c]);
                    high_[c] += split_high_ * (x - high_[c]);
                    const float low = low_[c], mid = high_[c] - low_[c], high = x - high_[c];
                    sum[0] += x * x, sum[1] += low * low, sum[2] += mid * mid, sum[3] += high * high;
                }
            }
            // Digital silence would leave the filters decaying through denormals.
            for (size_t c = 0; c < front; ++c) {
                if (std::abs(low_[c]) < 1e-15f) {
                    low_[c] = 0;
                }
                if (std::abs(high_[c]) < 1e-15f) {
                    high_[c] = 0;
                }
            }
            const auto count = static_cast<float>(n * front);
            return {std::sqrt(sum[0] / count), std::sqrt(sum[1] / count), std::sqrt(sum[2] / count), std::sqrt(sum[3] / count)};
        }

    private:
        float split_low_ = 0, split_high_ = 0;
        std::array<float, 2> low_{}, high_{};
    };

    // What a meter draws from Levels: each band against its own recent peak, so quiet and loud recordings both use the
    // whole range. It rises with the sound and falls back slowly; with nothing coming in it settles at zero.
    class Follower {
    public:
        // dt: seconds since the last call.
        void follow(const Levels &in, float dt) {
            const std::array<float, 4> raw{in.all, in.low, in.mid, in.high};
            constexpr std::array<float, 4> floor{0.02f, 0.012f, 0.008f, 0.003f};
            dt = std::clamp(dt, 0.f, 0.25f);
            const float forget = std::exp(-dt / 6), rise = 1 - std::exp(-dt / 0.03f), fall = 1 - std::exp(-dt / 0.24f);
            for (size_t b = 0; b < raw.size(); ++b) {
                peak_[b] = std::max({raw[b], peak_[b] * forget, floor[b]});
                const float target = std::pow(std::min(raw[b] / peak_[b], 1.f), 0.8f);
                value_[b] += (target - value_[b]) * (target > value_[b] ? rise : fall);
                if (value_[b] < 1e-4f) {
                    value_[b] = 0;
                }
            }
        }
        float all() const {
            return value_[0];
        }
        // 0 low, 1 middle, 2 high.
        float band(size_t b) const {
            return value_[1 + b];
        }
        bool moving() const {
            return value_[0] > 0 || value_[1] > 0 || value_[2] > 0 || value_[3] > 0;
        }

    private:
        std::array<float, 4> value_{}, peak_{};
    };

} // namespace lyren::audio
