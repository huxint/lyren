#include "app/model/pins.hpp"

#include <algorithm>
#include <format>

#include "core/encoding.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"

namespace lyren {

    std::string Pins::stream_cache_key(const Track &t, const Stream &s) {
        std::string out = std::format("{}:{}:{}", key(t), s.level, s.trial ? "trial" : "full");
        if (s.decrypt_key) {
            out += ':';
            out += encoding::hex({reinterpret_cast<const char *>(s.decrypt_key->data()), s.decrypt_key->size()});
        }
        return out;
    }

    Pins::Pins(Env &env, bool silent)
    : env_(env),
      silent_(silent) {
        reload();
        if (!silent_) {
            enqueue_missing();
        }
    }

    Pins::~Pins() {
        ++gen_;
        if (active_spool_) {
            active_spool_->close();
            active_spool_.reset();
        }
    }

    void Pins::reload() {
        tracks_.clear();
        pinned_keys_.clear();
        cached_keys_.clear();
        pinned_list_keys_.clear();
        cache_hashes_.clear();
        if (!env_.store) {
            sync_spool();
            return;
        }
        for (auto &pt : env_.store->pinned_tracks()) {
            const std::string k = key(pt.track);
            pinned_keys_.insert(k);
            if (!pt.cache_hash.empty()) {
                if (audio::Spool::is_hash_cached(pt.cache_hash)) {
                    cached_keys_.insert(k);
                }
                cache_hashes_.insert_or_assign(k, std::move(pt.cache_hash));
            }
            tracks_.push_back(std::move(pt.track));
        }
        for (const auto &pl : env_.store->pinned_lists()) {
            pinned_list_keys_.insert(std::format("{}:{}:{}", pl.source_id, pl.list_id, static_cast<int>(pl.kind)));
        }
        sync_spool();
    }

    void Pins::sync_spool() {
        std::vector<std::string> hashes;
        hashes.reserve(cache_hashes_.size());
        for (const auto &[_, h] : cache_hashes_) {
            if (!h.empty()) {
                hashes.push_back(h);
            }
        }
        audio::Spool::set_pinned_hashes(std::move(hashes));
    }

    bool Pins::is_pinned(const Track &track) const {
        return pinned_keys_.contains(key(track));
    }

    bool Pins::is_cached(const Track &track) const {
        return cached_keys_.contains(key(track));
    }

    bool Pins::is_list_pinned(const Card &list) const {
        return pinned_list_keys_.contains(key(list));
    }

    std::optional<std::string> Pins::cached_hash(const Track &track) const {
        const auto it = cache_hashes_.find(key(track));
        if (it != cache_hashes_.end() && audio::Spool::is_hash_cached(it->second)) {
            return it->second;
        }
        return std::nullopt;
    }

    void Pins::toggle_pin(const Track &track) {
        if (!env_.store || track.id.empty()) {
            return;
        }
        const bool next = !is_pinned(track);
        env_.store->set_track_pin(track, next);
        reload();
        if (next && !silent_) {
            enqueue_missing();
        }
        env_.notify(std::string(lyren::tr(next ? "Pinned for offline playback" : "Removed offline pin")), next ? "pin" : "pin-off");
    }

    void Pins::pin_tracks(std::span<const Track> tracks) {
        if (!env_.store || tracks.empty()) {
            return;
        }
        for (const auto &t : tracks) {
            env_.store->set_track_pin(t, true);
        }
        reload();
        if (!silent_) {
            enqueue_missing();
        }
        env_.notify(std::string(lyren::tr("Pinned for offline playback")), "pin");
    }

    void Pins::unpin_tracks(std::span<const Track> tracks) {
        if (!env_.store || tracks.empty()) {
            return;
        }
        for (const auto &t : tracks) {
            env_.store->set_track_pin(t, false);
        }
        reload();
        env_.notify(std::string(lyren::tr("Removed offline pin")), "pin-off");
    }

    void Pins::toggle_pin_list(const Card &list, std::span<const Track> tracks) {
        if (!env_.store || list.id.empty()) {
            return;
        }
        const bool next = !is_list_pinned(list);
        env_.store->set_list_pin(list, tracks, next);
        reload();
        if (next && !silent_) {
            enqueue_missing();
        }
        env_.notify(std::string(lyren::tr(next ? "Pinned for offline playback" : "Removed offline pin")), next ? "pin" : "pin-off");
    }

    void Pins::note_cached(const Track &track, std::string_view cache_key) {
        if (!env_.store || cache_key.empty() || !is_pinned(track)) {
            return;
        }
        const std::string hash = audio::Spool::cache_hash(cache_key);
        if (hash.empty()) {
            return;
        }
        const std::string k = key(track);
        env_.store->update_pinned_cache(track, cache_key, hash);
        cache_hashes_.insert_or_assign(k, hash);
        if (audio::Spool::is_hash_cached(hash)) {
            cached_keys_.insert(k);
        }
        sync_spool();
    }

    std::move_only_function<void(bool)> Pins::on_stream_download(const Track &track, std::string_view cache_key) {
        if (cache_key.empty() || !is_pinned(track)) {
            return {};
        }
        note_cached(track, cache_key);
        const std::string hash = audio::Spool::cache_hash(cache_key);
        if (hash.empty()) {
            return {};
        }
        Loop *loop = &env_.loop;
        std::weak_ptr<int> weak = alive_;
        std::string track_key = key(track);
        return [this, loop, weak, track_key = std::move(track_key), hash](bool ok) mutable {
            if (!ok) {
                return;
            }
            loop->post([this, weak, track_key = std::move(track_key), hash = std::move(hash)] {
                if (weak.expired() || !pinned_keys_.contains(track_key)) {
                    return;
                }
                if (audio::Spool::is_hash_cached(hash) && cached_keys_.insert(track_key).second) {
                    env_.redraw();
                }
            });
        };
    }

    void Pins::enqueue_missing() {
        if (silent_) {
            return;
        }
        for (const auto &t : tracks_) {
            const std::string k = key(t);
            if (!is_cached(t) && !queued_keys_.contains(k) && env_.providers.contains(t.source, t.instance)) {
                queued_keys_.insert(k);
                pending_.push_back(t);
            }
        }
        pump();
    }

    void Pins::pump() {
        if (silent_ || fetching_ || active_spool_ || pending_.empty()) {
            return;
        }
        while (!pending_.empty()) {
            Track next = std::move(pending_.front());
            pending_.pop_front();
            const std::string k = key(next);
            queued_keys_.erase(k);
            if (!is_pinned(next) || is_cached(next) || !env_.providers.contains(next.source, next.instance)) {
                continue;
            }
            fetching_ = true;
            spawn(cache_one(++gen_, std::move(next)));
            return;
        }
    }

    Task<void> Pins::cache_one(uint64_t gen, Track track) {
        const Quality quality = env_.config.playback.quality;
        auto r = co_await env_.provider(track).stream(track, quality);
        if (gen != gen_) {
            co_return;
        }
        fetching_ = false;
        if (!r || r->url.empty() || (r->trial && env_.config.playback.trial == TrialMode::skip) || r->decrypt_key || !is_pinned(track)) {
            pump();
            co_return;
        }
        const std::string ckey = stream_cache_key(track, *r);
        const std::string hash = audio::Spool::cache_hash(ckey);
        if (env_.store && !hash.empty()) {
            env_.store->update_pinned_cache(track, ckey, hash);
            cache_hashes_.insert_or_assign(key(track), hash);
            sync_spool();
        }
        Loop *loop = &env_.loop;
        std::weak_ptr<int> weak = alive_;
        const std::string track_key = key(track);
        active_spool_ = audio::Spool::url(env_.http, r->url, ckey, [this, loop, weak, gen, track_key, hash](bool ok) {
            loop->post([this, weak, gen, track_key, hash, ok] {
                if (weak.expired() || gen != gen_) {
                    return;
                }
                if (ok && !hash.empty() && audio::Spool::is_hash_cached(hash)) {
                    cached_keys_.insert(track_key);
                }
                active_spool_.reset();
                env_.redraw();
                pump();
            });
        });
    }

} // namespace lyren
