#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "media/types.hpp"

struct sqlite3;

namespace lyren {

    // Incremental SQLite persistence (WAL mode) under ~/.local/state/lyren/lyren.db.
    // Backs key-value metadata, search history, cross-platform listening statistics, and offline cache pins.
    class Store {
    public:
        struct SearchEntry {
            std::string query;
            int count = 0;
            int64_t last_used = 0;
        };

        enum class StatsRange : uint8_t {
            week,
            month,
            all
        };

        struct TrackStat {
            Track track;
            int plays = 0;
            int64_t seconds = 0;
        };

        struct ArtistStat {
            std::string name;
            std::string artist_id;
            std::string instance;
            Source source = Source::netease;
            int plays = 0;
            int64_t seconds = 0;
            int tracks = 0;
        };

        struct SourceStat {
            std::string source_id;
            int plays = 0;
            int64_t seconds = 0;
        };

        struct ListeningStats {
            int64_t total_seconds = 0;
            int total_plays = 0;
            int unique_tracks = 0;
            int unique_artists = 0;
            std::vector<TrackStat> top_tracks;
            std::vector<ArtistStat> top_artists;
            std::vector<SourceStat> by_source;
        };

        struct PinnedTrack {
            Track track;
            std::string cache_key;
            std::string cache_hash;
            bool manual = false;
            int64_t pinned_at = 0;
        };

        struct PinnedList {
            std::string source_id;
            std::string list_id;
            std::string title;
            CardKind kind = CardKind::playlist;
            int64_t pinned_at = 0;
        };

        // Pass ":memory:" for an ephemeral in-memory database (unit and view tests).
        explicit Store(std::filesystem::path path);
        ~Store();
        Store(const Store &) = delete;
        Store &operator=(const Store &) = delete;

        bool open() const {
            return db_ != nullptr;
        }
        uint64_t stats_serial() const {
            return stats_serial_;
        }
        uint64_t search_serial() const {
            return search_serial_;
        }
        uint64_t pins_serial() const {
            return pins_serial_;
        }

        // Incremental key-value store.
        std::optional<std::string> get_kv(std::string_view key) const;
        void set_kv(std::string_view key, std::string_view value);
        void remove_kv(std::string_view key);

        // Search history.
        void record_search(std::string_view query, int64_t now = 0);
        std::vector<SearchEntry> search_history(std::string_view prefix = {}, int limit = 10) const;
        void remove_search(std::string_view query);
        void clear_search_history();

        // Cross-platform local listening statistics.
        void record_listen(const Track &track, int seconds, bool ended, int64_t played_at = 0);
        ListeningStats listening_stats(StatsRange range, int64_t now = 0) const;
        void clear_listening_stats();

        // Offline cache pinning (tracks and playlists/albums).
        void set_track_pin(const Track &track, bool pinned, int64_t now = 0);
        void set_list_pin(const Card &list, std::span<const Track> tracks, bool pinned, int64_t now = 0);
        void update_pinned_cache(const Track &track, std::string_view cache_key, std::string_view cache_hash);
        std::vector<PinnedTrack> pinned_tracks() const;
        std::vector<PinnedList> pinned_lists() const;
        std::vector<std::string> pinned_cache_hashes() const;
        std::optional<std::string> pinned_cache_hash(const Track &track) const;

    private:
        void migrate();
        bool exec(const char *sql) const;

        sqlite3 *db_ = nullptr;
        int64_t last_search_ts_ = 0;
        uint64_t stats_serial_ = 1;
        uint64_t search_serial_ = 1;
        uint64_t pins_serial_ = 1;
    };

} // namespace lyren
