#include "app/shell.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/i18n.hpp"
#include "core/log.hpp"

namespace lyren {

    using State = audio::Player::State;

    uint64_t Shell::random() {
        uint64_t z = (rng_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    void Shell::next_fm() {
        Source source = fm_playing_ ? fm_source_ : active_;
        auto &lib = library(source);
        if (lib.fm_pos + 1 < static_cast<int>(lib.fm.size())) {
            ++lib.fm_pos;
        }
        if (lib.fm_pos + 2 >= static_cast<int>(lib.fm.size()) && !lib.fm_loading) {
            spawn(load_fm(source));
        }
        page_time_ = now_(), page_dir_ = 1;
        if (fm_playing_) {
            play(lib.fm, lib.fm_pos);
        }
    }

    bool Shell::playing() const {
        auto s = player_.state();
        return want_play_ && (fetching_ || s == State::loading || s == State::playing);
    }

    void Shell::apply_volume() {
        player_.set_volume(silent_ || muted_ ? 0.f : volume_);
        publish();
    }

    void Shell::apply_gain() {
        // The track gain is in dB; the peak caps it so nothing clips, and without a peak it only ever lowers.
        float g = 1;
        if (config_.playback.normalize) {
            g = std::min(std::pow(10.f, track_gain_ / 20), track_peak_ > 0 ? 1 / track_peak_ : 1.f);
        }
        log::debug("loudness: {:+.2f} dB, peak {:.3f}: x{:.3f}", track_gain_, track_peak_, g);
        player_.set_gain(g);
    }

    void Shell::play(const std::vector<Track> &list, int i) {
        if (i < 0 || i >= static_cast<int>(list.size())) {
            return;
        }
        Source source = list[static_cast<size_t>(i)].source;
        fm_playing_ = &list == &library(source).fm;
        if (fm_playing_) {
            fm_source_ = source;
        }
        set_current(i, &list != &queue_);
        if (&list != &queue_) {
            queue_ = list;
            shuffle_history_.clear();
        }
        shuffle_next_ = -1;
        start();
    }

    void Shell::set_current(int i, bool replacing_queue) {
        if (i == current_ && !replacing_queue) {
            return;
        }
        prev_ = current() ? *current() : Track{};
        cover_time_ = now_();
        current_ = i;
    }

    void Shell::toggle_play() {
        if (!current()) {
            return;
        }
        auto s = player_.state();
        if (s == State::idle && !fetching_) {
            start(std::exchange(resume_at_, 0));
        } else if (s == State::failed) {
            start(last_pos_);
        } else if (playing()) {
            want_play_ = false;
            player_.pause();
            changed();
        } else {
            want_play_ = true;
            player_.play();
        }
    }

    void Shell::stop() {
        if (!current()) {
            return;
        }
        want_play_ = fetching_ = false;
        pending_song_seek_.reset();
        resume_at_ = last_pos_ = 0;
        trial_ = false, clip_start_ = 0, highlight_from_ = highlight_to_ = 0;
        stream_level_.clear();
        ++play_gen_;
        stop_player();
        changed();
    }

    void Shell::stop_player() {
        player_.stop();
        report_listen(false);
    }

    // A listen counts once it played 30 s, or to the end; the server adds up every report, so each listen is sent once.
    void Shell::report_listen(bool ended) {
        if (!listen_) {
            return;
        }
        int seconds = static_cast<int>(player_.played_seconds() - listen_started_);
        if (provider(*listen_).logged_in() && (seconds >= 30 || (ended && seconds > 0))) {
            spawn(provider(*listen_).features.reporting.play(listen_->id, listen_->album_id, seconds));
        }
        listen_.reset();
    }

    void Shell::seek(double at) {
        last_pos_ = at;
        auto state = player_.state();
        if (state == State::idle || state == State::failed) {
            resume_at_ = at;
        } else {
            player_.seek(at);
        }
        schedule_prefetch();
        if (mpris_) {
            mpris_->seeked(timeline().to_song(at));
        }
        changed();
        redraw_();
    }

    void Shell::seek_song(double at) {
        if (fetching_) {
            pending_song_seek_ = std::clamp(at, 0.0, timeline().duration());
            last_pos_ = timeline().to_file(*pending_song_seek_);
            changed();
            redraw_();
        } else {
            seek(timeline().to_file(at));
        }
    }

    void Shell::set_mode(Mode m) {
        mode_ = m;
        shuffle_next_ = -1;
        shuffle_history_.clear();
        changed();
        publish();
    }

    double Shell::position() {
        auto st = player_.state();
        if (st == State::playing || st == State::paused) {
            return player_.position();
        }
        return st == State::idle && !fetching_ ? resume_at_ : last_pos_;
    }

    Timeline Shell::timeline() const {
        return {current() ? static_cast<double>(current()->duration) : 0, player_.duration(), clip_start_};
    }

    void Shell::publish() {
        if (!mpris_) {
            return;
        }
        MprisState s{.volume = muted_ ? 0 : volume_, .loop_one = mode_ == Mode::one, .shuffle = mode_ == Mode::shuffle};
        if (const Track *t = current()) {
            using S = MprisState::Status;
            auto st = player_.state();
            s.status = playing() ? S::playing : st == State::idle && !fetching_ && resume_at_ == 0 ? S::stopped : S::paused;
            s.track = std::format("/lyren/track/{}_{}", platform(t->source).id, t->id);
            s.title = t->title, s.artist = t->artist, s.album = t->album;
            s.art = cover_url(t->cover, 512);
            s.length = timeline().duration();
            s.can_play = true;
            s.can_next = queue_.size() > 1 || fm_playing_;
            s.can_prev = queue_.size() > 1;
            s.can_seek = (st == State::playing || st == State::paused) && s.length > 0;
        }
        mpris_->update(s);
    }

    int Shell::next_index() {
        int n = static_cast<int>(queue_.size());
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

    void Shell::skip(int delta, bool automatic) {
        if (fm_playing_ && delta > 0) {
            return next_fm();
        }
        if (queue_.empty()) {
            return;
        }
        int n = static_cast<int>(queue_.size()), i;
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
        start(0, automatic ? want_play_ || player_.state() == State::ended : true);
    }

    void Shell::start(double at, bool play) {
        if (!current()) {
            return;
        }
        uint64_t gen = ++play_gen_;
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
            lyrics_id_ = key(*current());
            lyrics_.clear();
            lyric_layout_.clear();
            lyrics_loading_ = true;
        }
        spawn(load_track(gen, *current()));
        changed();
    }

    Task<void> Shell::load_track(uint64_t gen, Track t) {
        std::shared_ptr<audio::Spool> spool;
        Stream s;
        if (prefetch_.spool && prefetch_.key == key(t)) {
            spool = std::move(prefetch_.spool);
            s = std::move(prefetch_.stream);
            prefetch_ = {};
        } else {
            auto r = co_await provider(t).stream(t, config_.playback.quality);
            if (gen != play_gen_) {
                co_return;
            }
            if (!r || (r->trial && skip_trials())) {
                fetching_ = false;
                // A run of unplayable tracks is reported once, when one finally plays.
                if (!failures_) {
                    notify(r ? lyren::trf("Skipped {}: preview only", t.title) : t.title + ": " + r.error(), "circle-alert", !r);
                }
                if (++failures_ < queue_.size()) {
                    skip(1, true);
                } else {
                    failures_ = 0;
                    want_play_ = false;
                }
                co_return redraw_();
            }
            s = std::move(*r);
            spool = audio::Spool::url(http_, s.url);
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
            seek(at);
        }
        player_.set_title(t.title + " - " + t.artist);
        if (lyrics_.empty() && lyrics_id_ == key(t)) {
            spawn(load_lyrics(t));
        }
        redraw_();
    }

    Task<void> Shell::prefetch(Track t) {
        uint64_t gen = play_gen_;
        auto p = co_await provider(t).stream(t, config_.playback.quality);
        if (gen != play_gen_ || !p || (p->trial && skip_trials())) {
            co_return;
        }
        log::debug("prefetch: {}", t.title);
        prefetch_.key = key(t);
        prefetch_.spool = audio::Spool::url(http_, p->url);
        prefetch_.stream = std::move(*p);
    }

    // Shortly before the end, fetch the next track's URL and first bytes. On a timer rather than in draw(), so it also
    // happens while the window is hidden.
    void Shell::schedule_prefetch() {
        loop_.cancel(prefetch_timer_);
        prefetch_timer_ = 0;
        double dur = player_.duration();
        if (player_.state() != State::playing || dur <= 0 || prefetch_.gen == play_gen_) {
            return;
        }
        auto wait = std::chrono::duration<double>(std::max(0.0, dur - 15 - player_.position()));
        prefetch_timer_ = loop_.after(std::chrono::duration_cast<Clock::duration>(wait), [this] {
            prefetch_timer_ = 0;
            if (mode_ == Mode::one || fm_playing_) {
                return;
            }
            prefetch_.gen = play_gen_;
            if (int i = next_index(); i >= 0 && i != current_) {
                spawn(prefetch(queue_[static_cast<size_t>(i)]));
            }
        });
    }

    Task<void> Shell::load_lyrics(Track t) {
        std::string id = key(t);
        lyrics_id_ = id;
        lyrics_loading_ = true;
        auto r = co_await provider(t).lyrics(t);
        if (lyrics_id_ != id) {
            co_return;
        }
        lyrics_loading_ = false;
        if (r) {
            lyrics_ = std::move(*r);
        }
        lyric_layout_.clear();
        lyric_y_.clear();
        lyrics_since_ = now_();
        redraw_();
    }

    void Shell::on_player() {
        auto s = player_.state();
        if (s != last_state_) {
            if (s == State::playing && !counted_ && current()) {
                counted_ = true;
                if (failures_ > 1) {
                    notify(lyren::trf("Skipped {} unavailable songs", failures_), "skip-forward");
                }
                failures_ = 0;
                std::erase_if(recent_, [&](const Track &x) {
                    return same(x, *current());
                });
                recent_.insert(recent_.begin(), *current());
                if (recent_.size() > 200) {
                    recent_.pop_back();
                }
                refresh_recent();
                changed();
                if (provider(*current()).logged_in() && !trial_ && provider(*current()).features.reporting.start) {
                    listen_ = *current();
                    spawn(provider(*listen_).features.reporting.start(listen_->id, listen_->album_id));
                }
            }
            if (s == State::paused) {
                changed();
            }
            if (s == State::ended) {
                std::optional<Track> listened = listen_;
                report_listen(true);
                if (mode_ == Mode::one && !fm_playing_) {
                    listen_ = listened; // each time round is another listen
                    listen_started_ = player_.played_seconds();
                    seek(0);
                    player_.play();
                } else {
                    skip(1, true);
                }
            }
            if (s == State::failed) {
                const audio::Spool *sp = player_.spool();
                // Stream URLs expire; fetch a new one and carry on from here.
                if (sp && sp->status() == 403 && !retried_) {
                    start(last_pos_, want_play_);
                    retried_ = true;
                } else {
                    notify(lyren::trf("Playback failed: {}", player_.error()), "circle-alert", true);
                    want_play_ = false;
                }
            }
        }
        last_state_ = s;
        if (silent_ && player_.volume() > 0) {
            apply_volume();
        } else if (!silent_ && !muted_ && std::abs(player_.volume() - volume_) > 1e-3f) {
            volume_ = player_.volume();
            changed();
        }
        schedule_prefetch();
        publish();
        redraw_();
    }

} // namespace lyren
