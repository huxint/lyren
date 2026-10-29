#pragma once
#include <algorithm>
#include <utility>

namespace lyren {

    // Times are finite, nonnegative seconds. A zero duration means it is not known yet.
    // The song contains the entire file; preview files may start partway through it.
    class Timeline {
    public:
        constexpr Timeline(double song_duration, double file_duration, double file_start = 0)
        : duration_(std::max(song_duration, file_start + file_duration)),
          file_duration_(file_duration),
          file_start_(file_start) {}

        constexpr double duration() const {
            return duration_;
        }
        constexpr double file_duration() const {
            return file_duration_;
        }

        // A song-time request outside the file seeks to its nearest playable endpoint.
        constexpr double to_file(double song_seconds) const {
            return std::clamp(song_seconds - file_start_, 0.0, file_duration_ > 0 ? file_duration_ : duration_ - file_start_);
        }
        constexpr double to_song(double file_seconds) const {
            return std::clamp(file_start_ + file_seconds, 0.0, duration_);
        }

        // Normalized positions and endpoints are in [0, 1], including before metadata arrives.
        constexpr float fraction(double song_seconds) const {
            return duration_ > 0 ? static_cast<float>(std::clamp(song_seconds / duration_, 0.0, 1.0)) : 0;
        }
        constexpr float progress(double file_seconds) const {
            return fraction(to_song(file_seconds));
        }
        constexpr std::pair<float, float> playable_span() const {
            return file_duration_ > 0 ? std::pair{fraction(file_start_), fraction(file_start_ + file_duration_)} : std::pair{0.f, 1.f};
        }

    private:
        double duration_;      // >= file_start_ + file_duration_
        double file_duration_; // >= 0; zero until decoding exposes the duration
        double file_start_;    // >= 0; zero for a complete song or an opening preview
    };

} // namespace lyren
