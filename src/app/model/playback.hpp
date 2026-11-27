#pragma once
#include <array>
#include <inplace_vector>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/env.hpp"
#include "audio/player.hpp"
#include "media/lyrics.hpp"
#include "media/timeline.hpp"
#include "app/state.hpp"
#include "sys/mpris.hpp"

namespace lyren {

    // The queue and what plays from it: the transport, the current song's stream and lyrics, the listening history, and
    // what the desktop's media controls and the listener's account hear about it.
    class Library;
    class Pins;

    class Playback {
    public:
        using State = audio::Player::State;
        using Mode = PlayMode;

        // silent: a scripted run, whose stream volume stays 0. Takes the queue and the history out of saved.
        Playback(Env &env, audio::Player &player, bool silent, SavedState &saved);
        ~Playback();
        Playback(const Playback &) = delete;
        Playback &operator=(const Playback &) = delete;

        std::move_only_function<void()> on_change;                        // something worth saving changed
        std::move_only_function<void(Source)> on_radio_skip;              // a radio is asked for its next song
        std::move_only_function<void(const Track &, bool)> on_cloud_play; // a cloud-reported listen started or settled

        // Answers the desktop's media controls from now on. raise brings the window forward.
        void connect(Library &library);
        void connect(Pins &pins);
        void connect(Bus &bus, std::move_only_function<void(std::string_view token)> raise, std::move_only_function<void(std::string_view uri)> open_uri = {});
        void save(SavedStateView &to);
        void config_changed();
        // An account left: its radio and the listen being counted for it end.
        void forget(Source source);

        // The queue.
        const std::vector<Track> &queue() const {
            return queue_;
        }
        int index() const {
            return current_;
        }
        const Track *current() const {
            return current_ >= 0 ? &queue_[static_cast<size_t>(current_)] : nullptr;
        }
        bool is_current(const Track &t) const {
            return current() && same(*current(), t);
        }
        // The song before the last change and when that was, for the cross-fade between them.
        const Track &previous() const {
            return previous_;
        }
        double changed_at() const {
            return changed_at_;
        }
        // Replaces the queue with list and plays its i-th song.
        void play(const std::vector<Track> &list, int i);
        // The whole list: in order from the top, or shuffled from a song picked at random.
        void play_all(const std::vector<Track> &list, bool shuffled);
        // A radio's songs: skipping forward asks on_radio_skip for the next one instead of walking the queue.
        void play_radio(Source source, const std::vector<Track> &songs, int i);
        bool radio(Source source) const {
            return radio_ && radio_source_ == source;
        }
        // The i-th song of the queue.
        void jump(int i);
        // Right away, after the one playing, keeping the rest of the queue.
        void play_now(Track track);
        // After the one playing.
        void play_next(Track track);
        void play_next(std::span<const Track> tracks);
        // At the end of the queue.
        void enqueue(Track track);
        void enqueue(std::span<const Track> tracks);
        // Reorders one entry within the queue without interrupting playback.
        void move(int from, int to);
        // Drops every queued track except the current one.
        void keep_only_current();
        void remove(int i);
        void clear();
        // Queue state captured before a destructive operation so it can be restored on undo.
        struct Snapshot {
            std::vector<Track> queue;
            int current = -1;
            double position = 0;
            bool playing = false;
            bool radio = false;
            Source radio_source = Source::netease;
        };
        Snapshot snapshot();
        void restore(Snapshot snap);
        // seed, then the songs the service mixes with it from the liked playlist.
        Task<void> heartbeat(Track seed, std::string liked_playlist);

        // The transport.
        // Playing, or about to: wanted, and fetching, loading or sounding.
        bool playing() const;
        // Wanted but not heard yet: the stream is being fetched, opened or buffered.
        bool waiting() const;
        void toggle();
        void stop();
        void skip(int delta);
        Mode mode() const {
            return mode_;
        }
        void set_mode(Mode mode);
        // The slider's position, 0..1; 0 while muted.
        float volume() const {
            return muted_ ? 0 : volume_;
        }
        bool muted() const {
            return muted_;
        }
        void set_volume(float v); // unmutes
        void toggle_mute();
        uint64_t random();

        // Where the song stands. A preview is a part of its song: positions are in the file, times in the song.
        Timeline timeline() const {
            return {current() ? static_cast<double>(current()->duration) : 0, player_.duration(), clip_start_};
        }
        double position();
        double song_time() {
            return timeline().to_song(position());
        }
        void seek(double song_seconds);

        // The stream of the current song.
        const audio::Player &player() const {
            return player_;
        }
        double latency_ms() {
            return player_.latency_ms();
        }
        float track_gain() const {
            return track_gain_;
        }
        float track_peak() const {
            return track_peak_;
        }
        float applied_gain() const;
        bool trial() const {
            return trial_;
        }
        // The quality the service granted, in its own words.
        std::string_view level() const {
            return stream_level_;
        }
        // When automatic source fallback supplied the current stream, names the platform providing the audio.
        const std::optional<SourceKey> &fallback_source() const {
            return fallback_source_;
        }
        // The chorus, s in the song; empty unless the end is after the start.
        std::pair<double, double> highlight() const {
            return {highlight_from_, highlight_to_};
        }

        // The lyrics of the current song. serial changes with the lines, for what is laid out from them.
        const std::vector<LyricLine> &lyrics() const {
            return lyrics_;
        }
        bool lyrics_loading() const {
            return lyrics_loading_;
        }
        double lyrics_since() const {
            return lyrics_since_;
        }
        uint64_t lyrics_serial() const {
            return lyrics_serial_;
        }
        // The line sung at the song time, or nullptr; nothing while the lyrics on hand are another song's.
        const LyricLine *line_at(const Track &track, double song_seconds) const;

        // What played on this device, newest first.
        const std::vector<Track> &history(Source source, std::string_view instance = {}) const {
            if (auto it = history_.find(source_id(source, instance)); it != history_.end()) {
                return it->second;
            }
            static const std::vector<Track> empty_history;
            return empty_history;
        }

    private:
        // A stream fetched ahead of its turn.
        struct Prefetched {
            std::string key;
            Source source = Source::netease;
            uint64_t source_gen = 0;
            bool signed_in = false;
            Quality quality = Quality::exhigh;
            std::shared_ptr<audio::Spool> spool;
            uint64_t gen = 0;
            Stream stream;
        };

        void set_current(int i, bool replacing_queue = false);
        void reset_shuffle(bool clear_failures = true);
        void stop_skip_run(std::string_view reason = {});
        void replace(const std::vector<Track> &list, int i, bool clear_failures = true);
        void start(double at = 0, bool play = true, int direction = 1);
        void step(int delta, bool automatic);
        // Stops the output and forgets the stream; the song stays current.
        void halt();
        void stop_player(); // stops output and reports the actual listening time
        void report_listen(bool ended);
        Task<void> report_start(Track track);
        void seek_file(double at);
        int next_index();
        Task<void> load_track(uint64_t gen, Track t);
        Task<void> load_lyrics(Track t);
        void drop_lyrics();
        Task<void> prefetch(Track t);
        void schedule_prefetch();
        void on_player();
        void apply_volume();
        void apply_gain();
        void sort_history();
        void publish(); // the state the media controls show
        void touch();   // published and saved
        bool skip_trials() const {
            return env_.config.playback.trial == TrialMode::skip;
        }

        Env &env_;
        audio::Player &player_;
        bool silent_;

        std::vector<Track> queue_;
        int current_ = -1;
        Track previous_;
        double changed_at_ = -10;
        bool radio_ = false;
        Source radio_source_ = Source::netease;
        Mode mode_ = Mode::list;
        std::vector<int> shuffle_history_;
        int shuffle_next_ = -1;
        uint64_t rng_ = 0x2545f4914f6cdd1dull;

        uint64_t play_gen_ = 0, queue_gen_ = 0;
        StringMap<uint64_t> source_gen_;
        uint64_t player_event_ = 0;
        State last_state_ = State::idle;
        bool want_play_ = false, fetching_ = false;
        bool retried_ = false, counted_ = false;
        static constexpr size_t max_failures = 5;
        size_t failures_ = 0;                           // consecutive failures, including across radio batches
        std::inplace_vector<int, max_failures> failed_; // failed indices in the current queue
        int skip_direction_ = 1;                        // keep searching in the requested direction until a track starts
        double resume_at_ = 0, last_pos_ = 0;           // nonnegative seconds within the current file
        std::optional<double> pending_song_seek_;       // nonnegative song seconds, only while fetching the stream
        float volume_ = 0.7f;
        bool muted_ = false;

        Prefetched prefetch_;
        Loop::TimerId prefetch_timer_ = 0;
        float track_gain_ = 0, track_peak_ = 0;        // loudness of the current stream
        bool trial_ = false;                           // the current stream is a preview clip
        std::string stream_level_;                     // the quality actually granted by the service
        std::optional<SourceKey> fallback_source_;     // set when audio is streamed from another platform via auto_fallback
        double clip_start_ = 0;                        // where a trial clip sits in the song the lyrics are timed for
        double highlight_from_ = 0, highlight_to_ = 0; // the stream's chorus window, s in song time
        std::optional<Track> listen_;                  // the track whose listen is reported to the account
        double listen_started_ = 0;                    // cumulative output time at the start of this listen
        std::optional<Track> local_listen_;            // local cross-platform stats tracking (independent of sign-in)
        double local_listen_started_ = 0;

        std::vector<LyricLine> lyrics_;
        std::string lyrics_id_;
        bool lyrics_loading_ = false;
        double lyrics_since_ = -10;
        uint64_t lyrics_serial_ = 0;

        std::vector<Track> recent_; // newest first, at most 200 across all platforms
        StringMap<std::vector<Track>> history_;
        Pins *pins_ = nullptr;
        std::unique_ptr<Mpris> mpris_;
    };

} // namespace lyren
