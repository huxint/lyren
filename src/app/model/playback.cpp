#include "app/model/playback.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>

#include "app/state.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"

namespace lyren {

    Playback::Playback(Env &env, audio::Player &player, bool silent, SavedState &saved)
    : env_(env),
      player_(player),
      silent_(silent),
      queue_(std::move(saved.queue)),
      current_(saved.current),
      mode_(saved.mode),
      resume_at_(saved.position),
      last_pos_(saved.position),
      volume_(saved.volume),
      muted_(saved.muted),
      recent_(std::move(saved.recent)) {
        rng_ ^= static_cast<uint64_t>(std::time(nullptr));
        sort_history();
        env_.images.prefetch(queue_);
        player_.on_change = [this] {
            on_player();
        };
        apply_volume();
        if (current()) {
            spawn(load_lyrics(*current()));
        }
    }

    Playback::~Playback() {
        env_.loop.cancel(prefetch_timer_);
        player_.on_change = nullptr;
    }

    void Playback::connect(Bus &bus, std::move_only_function<void(std::string_view token)> raise) {
        if (!bus.get()) {
            return;
        }
        Mpris::Handlers h{
            .raise = std::move(raise),
            .quit =
                [this] {
                    env_.loop.quit();
                },
            .play =
                [this] {
                    if (!playing()) {
                        toggle();
                    }
                },
            .pause =
                [this] {
                    if (playing()) {
                        toggle();
                    }
                },
            .toggle =
                [this] {
                    toggle();
                },
            .stop =
                [this] {
                    stop();
                },
            .next =
                [this] {
                    skip(1);
                },
            .previous =
                [this] {
                    skip(-1);
                },
            .seek =
                [this](double at) {
                    seek(at);
                },
            .volume =
                [this](double v) {
                    set_volume(static_cast<float>(v));
                },
            .loop_one =
                [this](bool on) {
                    if (on) {
                        set_mode(Mode::one);
                    } else if (mode_ == Mode::one) {
                        set_mode(Mode::list);
                    }
                },
            .shuffle =
                [this](bool on) {
                    if (on) {
                        set_mode(Mode::shuffle);
                    } else if (mode_ == Mode::shuffle) {
                        set_mode(Mode::list);
                    }
                },
            .position =
                [this] {
                    return song_time();
                },
        };
        mpris_ = Mpris::create(bus, std::move(h));
        publish();
    }

    void Playback::save(SavedState &to) {
        to.volume = volume_;
        to.muted = muted_;
        to.mode = mode_;
        to.queue = queue_;
        to.current = current_;
        to.recent = recent_;
        const auto state = player_.state();
        to.position = state == State::playing || state == State::paused ? player_.position() : resume_at_ > 0 ? resume_at_ : last_pos_;
    }

    void Playback::config_changed() {
        apply_gain();
    }

    void Playback::forget(Source source) {
        if (radio_source_ == source) {
            radio_ = false;
        }
        if (listen_ && listen_->source == source) {
            listen_.reset();
        }
    }

    uint64_t Playback::random() {
        uint64_t z = (rng_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    bool Playback::playing() const {
        const auto s = player_.state();
        return want_play_ && (fetching_ || s == State::loading || s == State::playing);
    }

    bool Playback::waiting() const {
        return want_play_ && (fetching_ || player_.state() == State::loading || player_.buffering());
    }

    void Playback::apply_volume() {
        player_.set_volume(silent_ || muted_ ? 0.f : volume_);
        publish();
    }

    void Playback::apply_gain() {
        // The track gain is in dB; the peak caps it so nothing clips, and without a peak it only ever lowers.
        float g = 1;
        if (env_.config.playback.normalize) {
            g = std::min(std::pow(10.f, track_gain_ / 20), track_peak_ > 0 ? 1 / track_peak_ : 1.f);
        }
        log::debug("loudness: {:+.2f} dB, peak {:.3f}: x{:.3f}", track_gain_, track_peak_, g);
        player_.set_gain(g);
    }

    void Playback::set_volume(float v) {
        volume_ = std::clamp(v, 0.f, 1.f), muted_ = false;
        apply_volume();
        touch();
    }

    void Playback::toggle_mute() {
        muted_ = !muted_;
        apply_volume();
        touch();
    }

    void Playback::touch() {
        publish();
        if (on_change) {
            on_change();
        }
    }

    void Playback::set_current(int i, bool replacing_queue) {
        if (i == current_ && !replacing_queue) {
            return;
        }
        previous_ = current() ? *current() : Track{};
        changed_at_ = env_.now();
        current_ = i;
    }

    void Playback::replace(const std::vector<Track> &list, int i) {
        set_current(i, true);
        queue_ = list;
        shuffle_history_.clear();
        shuffle_next_ = -1;
        start();
    }

    void Playback::play(const std::vector<Track> &list, int i) {
        if (i < 0 || i >= static_cast<int>(list.size())) {
            return;
        }
        radio_ = false;
        replace(list, i);
    }

    void Playback::play_all(const std::vector<Track> &list, bool shuffled) {
        if (list.empty()) {
            return;
        }
        if (shuffled) {
            set_mode(Mode::shuffle);
        } else if (mode_ == Mode::shuffle) {
            set_mode(Mode::list);
        }
        play(list, shuffled ? static_cast<int>(random() % list.size()) : 0);
    }

    void Playback::play_radio(Source source, const std::vector<Track> &songs, int i) {
        if (i < 0 || i >= static_cast<int>(songs.size())) {
            return;
        }
        radio_ = true, radio_source_ = source;
        replace(songs, i);
    }

    void Playback::jump(int i) {
        if (i < 0 || i >= static_cast<int>(queue_.size())) {
            return;
        }
        radio_ = false;
        set_current(i);
        shuffle_next_ = -1;
        start();
    }

    void Playback::play_now(Track track) {
        if (is_current(track)) {
            if (!playing()) {
                toggle();
            }
            return;
        }
        const int at = current_ < 0 ? 0 : current_ + 1;
        queue_.insert(queue_.begin() + at, std::move(track));
        shuffle_history_.clear();
        shuffle_next_ = -1;
        radio_ = false;
        set_current(at);
        start();
    }

    void Playback::play_next(Track track) {
        shuffle_history_.clear();
        shuffle_next_ = -1;
        if (!current()) {
            // Nothing is on: the song becomes the current one, ready to play.
            queue_.insert(queue_.begin(), std::move(track));
            set_current(0);
            halt();
            drop_lyrics();
            spawn(load_lyrics(*current()));
        } else {
            queue_.insert(queue_.begin() + (current_ + 1), std::move(track));
            if (mode_ == Mode::shuffle) {
                shuffle_next_ = current_ + 1;
            }
        }
        touch();
    }

    void Playback::remove(int i) {
        if (i < 0 || i >= static_cast<int>(queue_.size())) {
            return;
        }
        const bool was_playing = playing();
        if (i == current_) {
            previous_ = *current();
            changed_at_ = env_.now();
        }
        queue_.erase(queue_.begin() + i);
        shuffle_history_.clear();
        shuffle_next_ = -1;
        if (i < current_) {
            --current_;
        } else if (i == current_) {
            // The playing song goes: carry on with the one after it.
            current_ = queue_.empty() ? -1 : std::min(current_, static_cast<int>(queue_.size()) - 1);
            if (current() && was_playing) {
                start();
            } else {
                halt();
                drop_lyrics();
                if (current()) {
                    spawn(load_lyrics(*current()));
                }
            }
        }
        touch();
    }

    void Playback::clear() {
        if (queue_.empty()) {
            return;
        }
        set_current(-1, true);
        queue_.clear();
        radio_ = false;
        shuffle_history_.clear();
        shuffle_next_ = -1;
        halt();
        drop_lyrics();
        touch();
    }

    Task<void> Playback::heartbeat(Track seed, std::string liked_playlist) {
        auto r = co_await env_.provider(seed).features.heartbeat(seed.id, std::move(liked_playlist));
        if (!r) {
            co_return env_.notify(lyren::trf("Could not start Heartbeat mode: {}", r.error()), "triangle-alert", true);
        }
        std::vector<Track> list = std::move(*r);
        std::erase_if(list, [&](const Track &x) {
            return same(x, seed);
        });
        list.insert(list.begin(), std::move(seed));
        play_all(list, false);
        env_.notify(std::string(lyren::tr("Heartbeat mode mixes your favorites with similar songs")), "heart-pulse");
    }

    void Playback::toggle() {
        if (!current()) {
            return;
        }
        const auto s = player_.state();
        if (s == State::idle && !fetching_) {
            start(std::exchange(resume_at_, 0));
        } else if (s == State::failed) {
            start(last_pos_);
        } else if (playing()) {
            want_play_ = false;
            player_.pause();
            touch();
        } else {
            want_play_ = true;
            player_.play();
        }
    }

    void Playback::halt() {
        ++play_gen_;
        want_play_ = fetching_ = false;
        pending_song_seek_.reset();
        resume_at_ = last_pos_ = 0;
        trial_ = false, clip_start_ = 0, highlight_from_ = highlight_to_ = 0;
        stream_level_.clear();
        stop_player();
    }

    void Playback::stop() {
        if (!current()) {
            return;
        }
        halt();
        touch();
    }

    void Playback::stop_player() {
        player_.stop();
        report_listen(false);
    }

    // A listen counts once it played 30 s, or to the end; the server adds up every report, so each listen is sent once.
    void Playback::report_listen(bool ended) {
        if (!listen_) {
            return;
        }
        const int seconds = static_cast<int>(player_.played_seconds() - listen_started_);
        auto &provider = env_.provider(*listen_);
        if (provider.logged_in() && provider.features.reporting.play && (seconds >= 30 || (ended && seconds > 0))) {
            spawn(provider.features.reporting.play(listen_->id, listen_->album_id, seconds));
        }
        listen_.reset();
    }

    void Playback::seek_file(double at) {
        last_pos_ = at;
        const auto state = player_.state();
        if (state == State::idle || state == State::failed) {
            resume_at_ = at;
        } else {
            player_.seek(at);
        }
        schedule_prefetch();
        if (mpris_) {
            mpris_->seeked(timeline().to_song(at));
        }
        touch();
        env_.redraw();
    }

    void Playback::seek(double song_seconds) {
        if (fetching_) {
            // The file's own timeline is not known yet; the request waits for the stream.
            pending_song_seek_ = std::clamp(song_seconds, 0.0, timeline().duration());
            last_pos_ = timeline().to_file(*pending_song_seek_);
            touch();
            env_.redraw();
        } else {
            seek_file(timeline().to_file(song_seconds));
        }
    }

    void Playback::set_mode(Mode mode) {
        mode_ = mode;
        shuffle_next_ = -1;
        shuffle_history_.clear();
        touch();
    }

    double Playback::position() {
        const auto state = player_.state();
        if (state == State::playing || state == State::paused) {
            // Remembered for when the stream fails and has to be picked up again from here.
            last_pos_ = player_.position();
            return last_pos_;
        }
        return state == State::idle && !fetching_ ? resume_at_ : last_pos_;
    }

    const LyricLine *Playback::line_at(const Track &track, double song_seconds) const {
        if (lyrics_id_ != key(track)) {
            return nullptr;
        }
        const int at = lyric_at(lyrics_, song_seconds);
        return at >= 0 ? &lyrics_[static_cast<size_t>(at)] : nullptr;
    }

    void Playback::publish() {
        if (!mpris_) {
            return;
        }
        MprisState s{.volume = volume(), .loop_one = mode_ == Mode::one, .shuffle = mode_ == Mode::shuffle};
        if (const Track *t = current()) {
            using S = MprisState::Status;
            const auto state = player_.state();
            s.status = playing() ? S::playing : state == State::idle && !fetching_ && resume_at_ == 0 ? S::stopped : S::paused;
            s.track = std::format("/lyren/track/{}_{}", platform(t->source).id, t->id);
            s.title = t->title, s.artist = t->artist, s.album = t->album;
            s.art = cover_url(t->cover, 512);
            s.length = timeline().duration();
            s.can_play = true;
            s.can_next = queue_.size() > 1 || radio_;
            s.can_prev = queue_.size() > 1;
            s.can_seek = (state == State::playing || state == State::paused) && s.length > 0;
        }
        mpris_->update(s);
    }

    int Playback::next_index() {
        const int n = static_cast<int>(queue_.size());
        if (!n) {
            return -1;
        }
        if (mode_ != Mode::shuffle) {
            return (current_ + 1) % n;
        }
        if (shuffle_next_ < 0 || shuffle_next_ >= n) {
            shuffle_next_ = n == 1 ? 0 : static_cast<int>(random() % static_cast<uint64_t>(n - 1));
            if (shuffle_next_ >= current_ && n > 1) {
                ++shuffle_next_;
            }
        }
        return shuffle_next_;
    }

    void Playback::skip(int delta) {
        step(delta, false);
    }

    void Playback::step(int delta, bool automatic) {
        if (!automatic) {
            failures_ = 0;
        }
        if (radio_ && delta > 0) {
            if (on_radio_skip) {
                on_radio_skip(radio_source_);
            }
            return;
        }
        if (queue_.empty()) {
            return;
        }
        const int n = static_cast<int>(queue_.size());
        int i;
        if (mode_ == Mode::shuffle && delta > 0) {
            i = next_index();
            shuffle_history_.push_back(current_);
            shuffle_next_ = -1;
        } else if (mode_ == Mode::shuffle && !shuffle_history_.empty()) {
            i = shuffle_history_.back();
            shuffle_history_.pop_back();
        } else {
            i = ((current_ + delta) % n + n) % n;
        }
        set_current(i);
        start(0, automatic ? want_play_ || player_.state() == State::ended : true, delta);
    }

    void Playback::drop_lyrics() {
        lyrics_id_ = current() ? key(*current()) : std::string();
        lyrics_.clear();
        lyrics_loading_ = current() != nullptr;
        ++lyrics_serial_;
    }

    void Playback::start(double at, bool play, int direction) {
        if (!current()) {
            return;
        }
        const uint64_t gen = ++play_gen_;
        skip_direction_ = direction;
        retried_ = counted_ = false;
        want_play_ = play;
        fetching_ = true;
        // The old stream's window no longer applies until the new one arrives.
        trial_ = false, clip_start_ = 0, highlight_from_ = highlight_to_ = 0;
        stream_level_.clear();
        last_pos_ = at;
        pending_song_seek_.reset();
        resume_at_ = 0;
        if (prefetch_.key != key(*current())) {
            prefetch_ = {};
        }
        stop_player();
        if (lyrics_id_ != key(*current())) {
            drop_lyrics();
        }
        spawn(load_track(gen, *current()));
        touch();
    }

    Task<void> Playback::load_track(uint64_t gen, Track t) {
        std::shared_ptr<audio::Spool> spool;
        Stream s;
        if (prefetch_.spool && prefetch_.key == key(t)) {
            spool = std::move(prefetch_.spool);
            s = std::move(prefetch_.stream);
            prefetch_ = {};
        } else {
            auto r = co_await env_.provider(t).stream(t, env_.config.playback.quality);
            if (gen != play_gen_) {
                co_return;
            }
            if (!r || (r->trial && skip_trials())) {
                fetching_ = false;
                // A run of unplayable tracks is reported once, when one finally plays.
                if (!failures_) {
                    env_.notify(r ? lyren::trf("Skipped {}: preview only", t.title) : std::format("{}: {}", t.title, lyren::tr(r.error())), "circle-alert", !r);
                }
                if (++failures_ < queue_.size()) {
                    step(skip_direction_, true);
                } else {
                    failures_ = 0;
                    want_play_ = false;
                    publish();
                }
                co_return env_.redraw();
            }
            s = std::move(*r);
            spool = audio::Spool::url(env_.http, s.url);
        }
        fetching_ = false;
        clip_start_ = s.trial_start;
        trial_ = s.trial;
        stream_level_ = std::move(s.level);
        highlight_from_ = s.highlight_from, highlight_to_ = s.highlight_to;
        track_gain_ = s.gain, track_peak_ = s.peak;
        const double at = pending_song_seek_ ? timeline().to_file(*pending_song_seek_) : last_pos_;
        pending_song_seek_.reset();
        last_pos_ = at;
        apply_gain();
        listen_started_ = player_.played_seconds();
        player_.open(std::move(spool), want_play_, s.decrypt_key);
        if (at > 0) {
            seek_file(at);
        }
        player_.set_title(t.title + " - " + t.artist);
        if (lyrics_.empty() && lyrics_id_ == key(t)) {
            spawn(load_lyrics(t));
        }
        env_.redraw();
    }

    Task<void> Playback::prefetch(Track t) {
        const uint64_t gen = play_gen_;
        auto p = co_await env_.provider(t).stream(t, env_.config.playback.quality);
        if (gen != play_gen_ || !p || (p->trial && skip_trials())) {
            co_return;
        }
        log::debug("prefetch: {}", t.title);
        prefetch_.key = key(t);
        prefetch_.spool = audio::Spool::url(env_.http, p->url);
        prefetch_.stream = std::move(*p);
    }

    // Shortly before the end, fetch the next track's URL and first bytes. On a timer rather than while drawing, so it
    // also happens while the window is hidden.
    void Playback::schedule_prefetch() {
        env_.loop.cancel(prefetch_timer_);
        prefetch_timer_ = 0;
        const double duration = player_.duration();
        if (player_.state() != State::playing || duration <= 0 || prefetch_.gen == play_gen_) {
            return;
        }
        const auto wait = std::chrono::duration<double>(std::max(0.0, duration - 15 - player_.position()));
        prefetch_timer_ = env_.loop.after(std::chrono::duration_cast<Clock::duration>(wait), [this] {
            prefetch_timer_ = 0;
            if (mode_ == Mode::one || radio_) {
                return;
            }
            prefetch_.gen = play_gen_;
            if (int i = next_index(); i >= 0 && i != current_) {
                spawn(prefetch(queue_[static_cast<size_t>(i)]));
            }
        });
    }

    Task<void> Playback::load_lyrics(Track t) {
        const std::string id = key(t);
        lyrics_id_ = id;
        lyrics_loading_ = true;
        auto r = co_await env_.provider(t).lyrics(t);
        if (lyrics_id_ != id) {
            co_return;
        }
        lyrics_loading_ = false;
        if (r) {
            lyrics_ = std::move(*r);
        }
        lyrics_since_ = env_.now();
        ++lyrics_serial_;
        env_.redraw();
    }

    void Playback::sort_history() {
        for (auto &songs : history_) {
            songs.clear();
        }
        for (const auto &track : recent_) {
            history_[static_cast<size_t>(track.source)].push_back(track);
        }
        for (const auto &songs : history_) {
            env_.images.prefetch(songs);
        }
    }

    void Playback::on_player() {
        const auto s = player_.state();
        if (s != last_state_) {
            if (s == State::playing && !counted_ && current()) {
                counted_ = true;
                if (failures_ > 1) {
                    env_.notify(lyren::trf("Skipped {} unavailable songs", failures_), "skip-forward");
                }
                failures_ = 0;
                skip_direction_ = 1;
                std::erase_if(recent_, [&](const Track &x) {
                    return same(x, *current());
                });
                recent_.insert(recent_.begin(), *current());
                if (recent_.size() > 200) {
                    recent_.pop_back();
                }
                sort_history();
                if (on_change) {
                    on_change();
                }
                auto &provider = env_.provider(*current());
                if (provider.logged_in() && !trial_ && provider.features.reporting.start) {
                    listen_ = *current();
                    spawn(provider.features.reporting.start(listen_->id, listen_->album_id));
                }
            }
            if (s == State::paused && on_change) {
                on_change();
            }
            if (s == State::ended) {
                std::optional<Track> listened = listen_;
                report_listen(true);
                if (mode_ == Mode::one && !radio_) {
                    listen_ = listened; // each time round is another listen
                    listen_started_ = player_.played_seconds();
                    seek_file(0);
                    player_.play();
                } else {
                    step(1, true);
                }
            }
            if (s == State::failed) {
                const audio::Spool *spool = player_.spool();
                // Stream URLs expire; fetch a new one and carry on from here.
                if (spool && spool->status() == 403 && !retried_) {
                    start(last_pos_, want_play_, skip_direction_);
                    retried_ = true;
                } else {
                    env_.notify(lyren::trf("Playback failed: {}", player_.error()), "circle-alert", true);
                    want_play_ = false;
                }
            }
        }
        last_state_ = s;
        if (s == State::playing || s == State::paused) {
            last_pos_ = player_.position();
        }
        if (silent_ && player_.volume() > 0) {
            apply_volume();
        } else if (!silent_ && !muted_ && std::abs(player_.volume() - volume_) > 1e-3f) {
            volume_ = player_.volume();
            if (on_change) {
                on_change();
            }
        }
        schedule_prefetch();
        publish();
        env_.redraw();
    }

} // namespace lyren
