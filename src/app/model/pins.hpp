#pragma once
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/env.hpp"
#include "audio/spool.hpp"
#include "core/string_map.hpp"

namespace lyren {

    // Manages offline-pinned tracks and playlists/albums: protects their cached audio from LRU eviction,
    // pre-caches missing streams quietly one at a time, and provides cached file hashes for offline fallback.
    class Pins {
    public:
        Pins(Env &env, bool silent);
        ~Pins();
        Pins(const Pins &) = delete;
        Pins &operator=(const Pins &) = delete;

        bool is_pinned(const Track &track) const;
        bool is_cached(const Track &track) const;
        bool is_list_pinned(const Card &list) const;
        std::optional<std::string> cached_hash(const Track &track) const;

        const std::vector<Track> &tracks() const {
            return tracks_;
        }
        size_t count() const {
            return tracks_.size();
        }
        bool caching() const {
            return active_spool_ != nullptr || fetching_;
        }
        size_t pending_count() const {
            return pending_.size() + (caching() ? 1 : 0);
        }

        void toggle_pin(const Track &track);
        void pin_tracks(std::span<const Track> tracks);
        void unpin_tracks(std::span<const Track> tracks);
        void toggle_pin_list(const Card &list, std::span<const Track> tracks);
        // Records a newly computed stream cache key for a track (when played or pre-cached).
        void note_cached(const Track &track, std::string_view cache_key);
        // Prepares a Spool completion callback when track is pinned so UI cached status updates on download finish.
        std::move_only_function<void(bool)> on_stream_download(const Track &track, std::string_view cache_key);

        static std::string stream_cache_key(const Track &t, const Stream &s);

    private:
        void reload();
        void sync_spool();
        void enqueue_missing();
        void pump();
        Task<void> cache_one(uint64_t gen, Track track);

        Env &env_;
        bool silent_ = false;
        std::vector<Track> tracks_;
        StringHashSet pinned_keys_;
        StringHashSet cached_keys_;
        StringHashSet pinned_list_keys_;
        StringMap<std::string> cache_hashes_; // key(track) -> md5 cache filename
        std::deque<Track> pending_;
        StringHashSet queued_keys_;
        std::shared_ptr<audio::Spool> active_spool_;
        bool fetching_ = false;
        uint64_t gen_ = 0;
        std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    };

} // namespace lyren
