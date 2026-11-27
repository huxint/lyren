#include "app/model/playback.hpp"
#include "app/model/library.hpp"
#include "app/model/pins.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>

#include "app/state.hpp"
#include "core/encoding.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "sys/paths.hpp"

namespace lyren {

    namespace {
        std::string stream_cache_key(const Track &t, const Stream &s) {
            return Pins::stream_cache_key(t, s);
        }
    } // namespace

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
        if (!silent_) {
            audio::Spool::set_cache_limit(static_cast<uint64_t>(std::max(0, env_.config.playback.cache_size)) << 20);
        }
        sort_history();
        env_.prefetch(queue_);
        player_.on_change = [this] {
            on_player();
        };
        player_.on_seeked = [this](double at) {
            last_pos_ = at;
            if (mpris_) {
                mpris_->seeked(timeline().to_song(at));
            }
        };
        apply_volume();
        if (current()) {
            spawn(load_lyrics(*current()));
        }
    }

    Playback::~Playback() {
        env_.loop.cancel(prefetch_timer_);
        player_.on_change = nullptr;
        player_.on_seeked = nullptr;
    }

    void Playback::connect(Library &library) {
        // A radio has no queue to walk: its platform says what comes next.
        on_radio_skip = [this, &library](Source source) {
            const auto &shelf = library.shelf(source);
            if (library.advance_radio(source)) {
                replace(shelf.radio, shelf.radio_pos, false);
            } else if (shelf.radio_pending) {
                halt();
                touch();
            } else {
                stop_skip_run();
            }
        };
        on_cloud_play = [&library](const Track &track, bool settled) {
            library.note_cloud_play(track, settled);
        };
        library.subscribe([this, &library](const Library::Event &event) {
            if (event.kind == Library::Event::Kind::left) {
                forget(event.source);
            } else if (event.kind == Library::Event::Kind::radio_ready && radio(event.source)) {
                const auto &shelf = library.shelf(event.source);
                replace(shelf.radio, shelf.radio_pos, false);
            } else if (event.kind == Library::Event::Kind::radio_exhausted && radio(event.source)) {
                stop_skip_run();
            }
        });
    }

    void Playback::connect(Pins &pins) {
        pins_ = &pins;
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

    void Playback::save(SavedStateView &to) {
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
        if (!silent_) {
            audio::Spool::set_cache_limit(static_cast<uint64_t>(std::max(0, env_.config.playback.cache_size)) << 20);
        }
        apply_gain();
    }

    void Playback::forget(Source source) {
        ++source_gen_[source_id(source)];
        if (current() && current()->source == source) {
            ++queue_gen_;
        }
        if (prefetch_.source == source) {
            prefetch_ = {};
        }
        env_.loop.cancel(prefetch_timer_);
        prefetch_timer_ = 0;
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

    float Playback::applied_gain() const {
        if (!env_.config.playback.normalize) {
            return 1.f;
        }
        return std::min(std::pow(10.f, track_gain_ / 20), track_peak_ > 0 ? 1 / track_peak_ : 1.f);
    }

    void Playback::apply_gain() {
        // The track gain is in dB; the peak caps it so nothing clips, and without a peak it only ever lowers.
        const float g = applied_gain();
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

    void Playback::reset_shuffle(bool clear_failures) {
        shuffle_history_.clear();
        shuffle_next_ = -1;
        failed_.clear();
        if (clear_failures) {
            failures_ = 0;
        }
    }

    void Playback::stop_skip_run(std::string_view reason) {
        if (failures_) {
            auto message = failures_ == 1 ? std::string(lyren::tr("Playback stopped")) : lyren::trf("Playback stopped after {} unavailable songs", failures_);
            if (!reason.empty()) {
                message += " · ";
                message += lyren::tr(reason);
            }
            env_.notify(std::move(message), "circle-alert", true);
        }
        lyrics_loading_ = false;
        stop();
    }

    void Playback::replace(const std::vector<Track> &list, int i, bool clear_failures) {
        ++queue_gen_;
        set_current(i, true);
        queue_ = list;
        reset_shuffle(clear_failures);
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
        ++queue_gen_;
        radio_ = false;
        set_current(i);
        shuffle_next_ = -1;
        failures_ = 0;
        failed_.clear();
        start();
    }

    void Playback::play_now(Track track) {
        ++queue_gen_;
        if (is_current(track)) {
            if (!playing()) {
                toggle();
            }
            return;
        }
        const int at = current_ < 0 ? 0 : current_ + 1;
        queue_.insert(queue_.begin() + at, std::move(track));
        reset_shuffle();
        radio_ = false;
        set_current(at);
        start();
    }

    void Playback::play_next(Track track) {
        play_next(std::span{&track, 1});
    }

    void Playback::play_next(std::span<const Track> tracks) {
        if (tracks.empty()) {
            return;
        }
        reset_shuffle(false);
        if (!current()) {
            // Nothing is on: the first song becomes the current one, ready to play.
            queue_.insert(queue_.begin(), tracks.begin(), tracks.end());
            set_current(0);
            halt();
            drop_lyrics();
            spawn(load_lyrics(*current()));
        } else {
            queue_.insert(queue_.begin() + (current_ + 1), tracks.begin(), tracks.end());
            if (mode_ == Mode::shuffle) {
                shuffle_next_ = current_ + 1;
            }
        }
        touch();
    }

    void Playback::enqueue(Track track) {
        enqueue(std::span{&track, 1});
    }

    void Playback::enqueue(std::span<const Track> tracks) {
        if (tracks.empty()) {
            return;
        }
        reset_shuffle(false);
        const bool empty_before = !current();
        queue_.insert(queue_.end(), tracks.begin(), tracks.end());
        if (empty_before) {
            set_current(0);
            halt();
            drop_lyrics();
            spawn(load_lyrics(*current()));
        }
        touch();
    }

    void Playback::move(int from, int to) {
        const int n = static_cast<int>(queue_.size());
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
            return;
        }
        auto first = queue_.begin();
        if (from < to) {
            std::rotate(first + from, first + from + 1, first + to + 1);
        } else {
            std::rotate(first + to, first + from, first + from + 1);
        }
        auto remap = [from, to](int idx) {
            if (idx == from) {
                return to;
            }
            if (from < to && idx > from && idx <= to) {
                return idx - 1;
            }
            if (to < from && idx >= to && idx < from) {
                return idx + 1;
            }
            return idx;
        };
        if (current_ >= 0) {
            current_ = remap(current_);
        }
        if (shuffle_next_ >= 0) {
            shuffle_next_ = remap(shuffle_next_);
        }
        for (int &idx : shuffle_history_) {
            idx = remap(idx);
        }
        for (int &idx : failed_) {
            idx = remap(idx);
        }
        touch();
        env_.redraw();
    }

    void Playback::keep_only_current() {
        if (!current() || queue_.size() <= 1) {
            return;
        }
        Track cur = std::move(queue_[static_cast<size_t>(current_)]);
        queue_.clear();
        queue_.push_back(std::move(cur));
        current_ = 0;
        radio_ = false;
        reset_shuffle(false);
        touch();
        env_.redraw();
    }

    Playback::Snapshot Playback::snapshot() {
        const auto state = player_.state();
        const double pos = state == State::playing || state == State::paused ? player_.position() : resume_at_ > 0 ? resume_at_ : last_pos_;
        return {.queue = queue_, .current = current_, .position = pos, .playing = playing(), .radio = radio_, .radio_source = radio_source_};
    }

    void Playback::restore(Snapshot snap) {
        if (snap.queue.empty()) {
            clear();
            return;
        }
        ++queue_gen_;
        const int idx = snap.current >= 0 && snap.current < static_cast<int>(snap.queue.size()) ? snap.current : -1;
        set_current(idx, true);
        queue_ = std::move(snap.queue);
        radio_ = snap.radio;
        radio_source_ = snap.radio_source;
        reset_shuffle();
        if (current()) {
            if (snap.playing) {
                start(snap.position, true);
            } else {
                halt();
                resume_at_ = last_pos_ = snap.position;
                drop_lyrics();
                spawn(load_lyrics(*current()));
                touch();
            }
        } else {
            halt();
            drop_lyrics();
            touch();
        }
        env_.redraw();
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
        reset_shuffle(false);
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
        ++queue_gen_;
        if (queue_.empty()) {
            return;
        }
        set_current(-1, true);
        queue_.clear();
        radio_ = false;
        reset_shuffle();
        halt();
        drop_lyrics();
        touch();
    }

    Task<void> Playback::heartbeat(Track seed, std::string liked_playlist) {
        const uint64_t qgen = ++queue_gen_, generation = play_gen_;
        const uint64_t sgen = source_gen_[source_id(seed.source, seed.instance)];
        auto r = co_await env_.provider(seed).features.heartbeat(seed.id, std::move(liked_playlist));
        if (qgen != queue_gen_ || generation != play_gen_ || sgen != source_gen_[source_id(seed.source, seed.instance)]) {
            co_return;
        }
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
        ++queue_gen_;
        radio_ = false;
        failures_ = 0;
        failed_.clear();
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
    // Local cross-platform stats record any real non-trial listen (>= 15 s or completed >= 5 s), regardless of sign-in.
    void Playback::report_listen(bool ended) {
        const double played = player_.played_seconds();
        if (listen_) {
            const int seconds = static_cast<int>(played - listen_started_);
            auto &provider = env_.provider(*listen_);
            if (provider.logged_in() && provider.features.reporting.play && (seconds >= 30 || (ended && seconds > 0))) {
                spawn(provider.features.reporting.play(listen_->id, listen_->album_id, seconds));
            }
            listen_.reset();
        }
        if (local_listen_) {
            const int seconds = static_cast<int>(played - local_listen_started_);
            if (env_.store && (seconds >= 15 || (ended && seconds >= 5))) {
                env_.store->record_listen(*local_listen_, seconds, ended);
            }
            local_listen_.reset();
        }
    }

    void Playback::seek_file(double at) {
        const auto state = player_.state();
        if (state == State::idle || state == State::failed) {
            resume_at_ = last_pos_ = at;
            if (mpris_) {
                mpris_->seeked(timeline().to_song(at));
            }
        } else {
            player_.seek(at);
            if (state == State::loading) {
                last_pos_ = at;
            }
        }
        schedule_prefetch();
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
        reset_shuffle(false);
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
        if (shuffle_next_ >= 0 && shuffle_next_ < n && (n == 1 || shuffle_next_ != current_) && !std::ranges::contains(failed_, shuffle_next_)) {
            return shuffle_next_;
        }
        if (failed_.empty()) {
            shuffle_next_ = n == 1 ? 0 : static_cast<int>(random() % static_cast<uint64_t>(n - 1));
            if (shuffle_next_ >= current_ && n > 1) {
                ++shuffle_next_;
            }
            return shuffle_next_;
        }
        std::vector<int> candidates;
        candidates.reserve(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) {
            if (k != current_ && !std::ranges::contains(failed_, k)) {
                candidates.push_back(k);
            }
        }
        if (candidates.empty()) {
            shuffle_next_ = -1;
            return -1;
        }
        shuffle_next_ = candidates[random() % candidates.size()];
        return shuffle_next_;
    }

    void Playback::skip(int delta) {
        step(delta, false);
    }

    void Playback::step(int delta, bool automatic) {
        if (!automatic) {
            failures_ = 0;
            failed_.clear();
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
        auto next_linear = [&] {
            for (int tries = 1; tries <= n; ++tries) {
                const int cand = ((current_ + delta * tries) % n + n) % n;
                if (!std::ranges::contains(failed_, cand)) {
                    return cand;
                }
            }
            return -1;
        };
        int i = -1;
        if (mode_ == Mode::shuffle && delta > 0) {
            i = next_index();
            if (i < 0) {
                return stop_skip_run();
            }
            if (!std::ranges::contains(failed_, current_)) {
                shuffle_history_.push_back(current_);
            }
            shuffle_next_ = -1;
        } else if (mode_ == Mode::shuffle && delta < 0) {
            while (!shuffle_history_.empty()) {
                const int prev = shuffle_history_.back();
                shuffle_history_.pop_back();
                if (prev >= 0 && prev < n && !std::ranges::contains(failed_, prev)) {
                    i = prev;
                    break;
                }
            }
            if (i < 0) {
                i = next_linear();
            }
            if (i < 0) {
                return stop_skip_run();
            }
        } else {
            i = next_linear();
            if (i < 0) {
                return stop_skip_run();
            }
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
        const Quality quality = env_.config.playback.quality;
        const std::string sid = source_id(t.source, t.instance);
        if (prefetch_.spool && prefetch_.key == key(t) && prefetch_.source == t.source && prefetch_.source_gen == source_gen_[sid] &&
            prefetch_.signed_in == env_.signed_in(t.source, t.instance) && prefetch_.quality == quality && !(prefetch_.stream.trial && skip_trials())) {
            spool = std::move(prefetch_.spool);
            s = std::move(prefetch_.stream);
            prefetch_ = {};
        } else {
            prefetch_ = {};
            auto r = co_await env_.provider(t).stream(t, quality);
            if (gen != play_gen_) {
                co_return;
            }
            if (!r || (r->trial && skip_trials())) {
                // Offline fallback: if this track is pinned and already cached on disk, play it directly.
                if (pins_) {
                    if (auto hash = pins_->cached_hash(t)) {
                        spool = audio::Spool::cached_file(*hash);
                    }
                }
                if (!spool) {
                    fetching_ = false;
                    failed_.push_back(current_);
                    ++failures_;
                    if (failures_ >= max_failures || (!radio_ && failed_.size() == queue_.size())) {
                        stop_skip_run(r ? std::string_view{"Only a preview is available"} : std::string_view{r.error()});
                    } else {
                        if (failures_ == 1) {
                            env_.notify(r ? lyren::trf("Skipped {}: preview only", t.title) : std::format("{}: {}", t.title, lyren::tr(r.error())), "circle-alert", !r);
                        }
                        step(skip_direction_, true);
                    }
                    co_return env_.redraw();
                }
            } else {
                s = std::move(*r);
                const std::string cache_key = silent_ ? std::string{} : stream_cache_key(t, s);
                std::move_only_function<void(bool)> on_cached;
                if (pins_ && !cache_key.empty() && !s.trial && !s.decrypt_key) {
                    on_cached = pins_->on_stream_download(t, cache_key);
                }
                spool = audio::Spool::url(env_.http, s.url, cache_key, std::move(on_cached));
            }
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
        listen_started_ = local_listen_started_ = player_.played_seconds();
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
        const std::string sid = source_id(t.source, t.instance);
        const uint64_t sgen = source_gen_[sid];
        const bool signed_in = env_.signed_in(t.source, t.instance);
        const Quality quality = env_.config.playback.quality;
        auto p = co_await env_.provider(t).stream(t, quality);
        if (gen != play_gen_ || sgen != source_gen_[sid] || signed_in != env_.signed_in(t.source, t.instance) || !p || (p->trial && skip_trials())) {
            co_return;
        }
        log::debug("prefetch: {}", t.title);
        const std::string cache_key = silent_ ? std::string{} : stream_cache_key(t, *p);
        std::move_only_function<void(bool)> on_cached;
        if (pins_ && !cache_key.empty() && !p->trial && !p->decrypt_key) {
            on_cached = pins_->on_stream_download(t, cache_key);
        }
        prefetch_ = {
            .key = key(t),
            .source = t.source,
            .source_gen = sgen,
            .signed_in = signed_in,
            .quality = quality,
            .spool = audio::Spool::url(env_.http, p->url, cache_key, std::move(on_cached)),
            .gen = gen,
            .stream = std::move(*p),
        };
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
                prefetch_.source = queue_[static_cast<size_t>(i)].source;
                spawn(prefetch(queue_[static_cast<size_t>(i)]));
            }
        });
    }

    Task<void> Playback::load_lyrics(Track t) {
        const uint64_t request = ++lyrics_serial_;
        lyrics_id_ = key(t);
        lyrics_loading_ = true;
        auto r = co_await env_.provider(t).playback_lyrics(std::move(t), paths::config_dir() / "lyrics");
        if (request != lyrics_serial_) {
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
        for (auto &[_, songs] : history_) {
            songs.clear();
        }
        for (const auto &track : recent_) {
            history_[source_id(track.source, track.instance)].push_back(track);
        }
        for (const auto &[_, songs] : history_) {
            env_.prefetch(songs);
        }
    }

    Task<void> Playback::report_start(Track track) {
        if (on_cloud_play) {
            on_cloud_play(track, false);
        }
        co_await env_.provider(track).features.reporting.start(track.id, track.album_id);
        if (on_cloud_play) {
            on_cloud_play(track, true);
        }
    }

    void Playback::on_player() {
        const auto s = player_.state();
        const uint64_t event_id = ++player_event_;
        if (s != std::exchange(last_state_, s)) {
            if (s == State::playing && !counted_ && current()) {
                counted_ = true;
                if (failures_ > 1) {
                    env_.notify(lyren::trf("Skipped {} unavailable songs", failures_), "skip-forward");
                }
                failures_ = 0;
                failed_.clear();
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
                if (!trial_) {
                    local_listen_ = *current();
                    local_listen_started_ = player_.played_seconds();
                }
                auto &provider = env_.provider(*current());
                if (provider.logged_in() && !trial_ && provider.features.reporting.start) {
                    listen_ = *current();
                    spawn(report_start(*listen_));
                }
            }
            if (s == State::paused && on_change) {
                on_change();
            }
            if (s == State::ended) {
                std::optional<Track> listened = listen_;
                std::optional<Track> local_listened = local_listen_;
                report_listen(true);
                if (mode_ == Mode::one && !radio_) {
                    listen_ = listened; // each time round is another listen
                    local_listen_ = local_listened;
                    listen_started_ = local_listen_started_ = player_.played_seconds();
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
                    env_.notify(lyren::trf("Playback failed: {}", lyren::tr(player_.error())), "circle-alert", true);
                    want_play_ = false;
                }
            }
        }
        if (event_id != player_event_) {
            return;
        }
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
