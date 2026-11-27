#include "app/store.hpp"

#include <glaze/glaze.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "core/log.hpp"
#include "core/string_map.hpp"
#include "core/utf8.hpp"

namespace lyren {

    namespace {
        struct Stmt {
            sqlite3_stmt *s = nullptr;

            Stmt() = default;
            Stmt(sqlite3 *db, const char *sql) {
                if (db && sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
                    log::warn("sqlite prepare failed: {}", sqlite3_errmsg(db));
                    s = nullptr;
                }
            }
            ~Stmt() {
                if (s) {
                    sqlite3_finalize(s);
                }
            }
            Stmt(const Stmt &) = delete;
            Stmt &operator=(const Stmt &) = delete;

            explicit operator bool() const {
                return s != nullptr;
            }
            void bind(int idx, std::string_view v) {
                sqlite3_bind_text(s, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
            }
            void bind(int idx, int64_t v) {
                sqlite3_bind_int64(s, idx, v);
            }
            void bind(int idx, int v) {
                sqlite3_bind_int(s, idx, v);
            }
            bool step_row() {
                return sqlite3_step(s) == SQLITE_ROW;
            }
            bool step_done() {
                const int rc = sqlite3_step(s);
                return rc == SQLITE_DONE || rc == SQLITE_ROW;
            }
            void reset() {
                sqlite3_reset(s);
                sqlite3_clear_bindings(s);
            }
            std::string text(int col) const {
                const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(s, col));
                const int n = sqlite3_column_bytes(s, col);
                return p && n > 0 ? std::string(p, static_cast<size_t>(n)) : std::string{};
            }
            int64_t i64(int col) const {
                return sqlite3_column_int64(s, col);
            }
            int i32(int col) const {
                return sqlite3_column_int(s, col);
            }
        };

        int64_t unix_now(int64_t override_now) {
            return override_now > 0 ? override_now : static_cast<int64_t>(std::time(nullptr));
        }

        std::string escape_like(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 4);
            for (char c : s) {
                if (c == '%' || c == '_' || c == '\\') {
                    out.push_back('\\');
                }
                out.push_back(c);
            }
            return out;
        }

        std::string encode_track(const Track &t) {
            std::string json;
            static_cast<void>(glz::write_json(t, json));
            return json;
        }

        std::optional<Track> decode_track(std::string_view json) {
            Track t;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(t, json)) {
                return std::nullopt;
            }
            if (!valid_source(t.source)) {
                return std::nullopt;
            }
            return t;
        }

        // Splits joined artist names ("Artist A / Artist B") into individual names for artist distribution stats.
        std::vector<std::string_view> split_artists(std::string_view raw) {
            std::vector<std::string_view> out;
            size_t pos = 0;
            while (pos < raw.size()) {
                const size_t sep = raw.find(" / ", pos);
                std::string_view part = trim(sep == raw.npos ? raw.substr(pos) : raw.substr(pos, sep - pos));
                if (!part.empty()) {
                    out.push_back(part);
                }
                if (sep == raw.npos) {
                    break;
                }
                pos = sep + 3;
            }
            if (out.empty()) {
                if (const auto t = trim(raw); !t.empty()) {
                    out.push_back(t);
                }
            }
            return out;
        }
    } // namespace

    Store::Store(std::filesystem::path path) {
        const std::string target = path.string();
        if (target != ":memory:" && path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(target.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            log::warn("sqlite open {}: {}", target, db_ ? sqlite3_errmsg(db_) : "out of memory");
            if (db_) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            return;
        }
        sqlite3_busy_timeout(db_, 3000);
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("PRAGMA foreign_keys=ON;");
        exec("PRAGMA temp_store=MEMORY;");
        migrate();
    }

    Store::~Store() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool Store::exec(const char *sql) const {
        if (!db_) {
            return false;
        }
        char *err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            log::warn("sqlite exec: {}", err ? err : "unknown error");
            sqlite3_free(err);
            return false;
        }
        return true;
    }

    void Store::migrate() {
        int version = 0;
        if (Stmt st(db_, "PRAGMA user_version;"); st && st.step_row()) {
            version = st.i32(0);
        }
        if (version >= 1) {
            return;
        }
        exec("BEGIN IMMEDIATE;");
        const bool ok = exec(R"SQL(
            CREATE TABLE IF NOT EXISTS kv (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS search_history (
                query TEXT PRIMARY KEY,
                count INTEGER NOT NULL DEFAULT 1,
                last_used INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_search_history_recent ON search_history(last_used DESC);

            CREATE TABLE IF NOT EXISTS listen_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_id TEXT NOT NULL,
                track_id TEXT NOT NULL,
                title TEXT NOT NULL,
                artist TEXT NOT NULL,
                album TEXT NOT NULL,
                artist_id TEXT NOT NULL,
                album_id TEXT NOT NULL,
                cover TEXT NOT NULL,
                duration INTEGER NOT NULL,
                seconds INTEGER NOT NULL,
                ended INTEGER NOT NULL,
                played_at INTEGER NOT NULL,
                track_json TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_listen_events_time ON listen_events(played_at DESC);
            CREATE INDEX IF NOT EXISTS idx_listen_events_track ON listen_events(source_id, track_id);

            CREATE TABLE IF NOT EXISTS pinned_tracks (
                source_id TEXT NOT NULL,
                track_id TEXT NOT NULL,
                cache_key TEXT NOT NULL DEFAULT '',
                cache_hash TEXT NOT NULL DEFAULT '',
                manual INTEGER NOT NULL DEFAULT 0,
                pinned_at INTEGER NOT NULL,
                track_json TEXT NOT NULL,
                PRIMARY KEY (source_id, track_id)
            );
            CREATE TABLE IF NOT EXISTS pinned_lists (
                source_id TEXT NOT NULL,
                kind INTEGER NOT NULL,
                list_id TEXT NOT NULL,
                title TEXT NOT NULL,
                pinned_at INTEGER NOT NULL,
                PRIMARY KEY (source_id, kind, list_id)
            );
            CREATE TABLE IF NOT EXISTS pinned_list_tracks (
                list_source_id TEXT NOT NULL,
                list_kind INTEGER NOT NULL,
                list_id TEXT NOT NULL,
                track_source_id TEXT NOT NULL,
                track_id TEXT NOT NULL,
                PRIMARY KEY (list_source_id, list_kind, list_id, track_source_id, track_id),
                FOREIGN KEY (list_source_id, list_kind, list_id)
                    REFERENCES pinned_lists(source_id, kind, list_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_pinned_list_tracks_track ON pinned_list_tracks(track_source_id, track_id);
            PRAGMA user_version = 1;
        )SQL");
        exec(ok ? "COMMIT;" : "ROLLBACK;");
    }

    std::optional<std::string> Store::get_kv(std::string_view key) const {
        Stmt st(db_, "SELECT value FROM kv WHERE key = ?1;");
        if (!st) {
            return std::nullopt;
        }
        st.bind(1, key);
        if (!st.step_row()) {
            return std::nullopt;
        }
        return st.text(0);
    }

    void Store::set_kv(std::string_view key, std::string_view value) {
        Stmt st(db_, "INSERT INTO kv(key, value, updated_at) VALUES(?1, ?2, ?3) "
                     "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;");
        if (!st) {
            return;
        }
        st.bind(1, key);
        st.bind(2, value);
        st.bind(3, unix_now(0));
        st.step_done();
    }

    void Store::remove_kv(std::string_view key) {
        Stmt st(db_, "DELETE FROM kv WHERE key = ?1;");
        if (!st) {
            return;
        }
        st.bind(1, key);
        st.step_done();
    }

    void Store::record_search(std::string_view raw, int64_t now) {
        const std::string_view q = trim(raw);
        if (q.empty() || !db_) {
            return;
        }
        int64_t ts = now;
        if (ts <= 0) {
            ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (ts <= last_search_ts_) {
                ts = last_search_ts_ + 1;
            }
        }
        last_search_ts_ = std::max(last_search_ts_, ts);
        Stmt up(db_, "INSERT INTO search_history(query, count, last_used) VALUES(?1, 1, ?2) "
                     "ON CONFLICT(query) DO UPDATE SET count = count + 1, last_used = MAX(last_used, excluded.last_used);");
        if (!up) {
            return;
        }
        up.bind(1, q);
        up.bind(2, ts);
        if (up.step_done()) {
            exec("DELETE FROM search_history WHERE query NOT IN "
                 "(SELECT query FROM search_history ORDER BY last_used DESC LIMIT 100);");
            ++search_serial_;
        }
    }

    std::vector<Store::SearchEntry> Store::search_history(std::string_view prefix, int limit) const {
        std::vector<SearchEntry> out;
        if (!db_ || limit <= 0) {
            return out;
        }
        const std::string_view p = trim(prefix);
        if (p.empty()) {
            Stmt st(db_, "SELECT query, count, last_used FROM search_history ORDER BY last_used DESC LIMIT ?1;");
            if (!st) {
                return out;
            }
            st.bind(1, limit);
            while (st.step_row()) {
                out.push_back({st.text(0), st.i32(1), st.i64(2)});
            }
            return out;
        }
        const std::string esc = escape_like(p);
        const std::string starts = esc + "%";
        const std::string contains = "%" + esc + "%";
        Stmt st(db_, "SELECT query, count, last_used FROM search_history "
                     "WHERE query LIKE ?1 ESCAPE '\\' "
                     "ORDER BY CASE WHEN query LIKE ?2 ESCAPE '\\' THEN 0 ELSE 1 END, last_used DESC LIMIT ?3;");
        if (!st) {
            return out;
        }
        st.bind(1, contains);
        st.bind(2, starts);
        st.bind(3, limit);
        while (st.step_row()) {
            out.push_back({st.text(0), st.i32(1), st.i64(2)});
        }
        return out;
    }

    void Store::remove_search(std::string_view query) {
        Stmt st(db_, "DELETE FROM search_history WHERE query = ?1;");
        if (!st) {
            return;
        }
        st.bind(1, query);
        if (st.step_done()) {
            ++search_serial_;
        }
    }

    void Store::clear_search_history() {
        if (exec("DELETE FROM search_history;")) {
            ++search_serial_;
        }
    }

    void Store::record_listen(const Track &track, int seconds, bool ended, int64_t played_at) {
        if (!db_ || track.id.empty() || seconds <= 0) {
            return;
        }
        Stmt st(db_, "INSERT INTO listen_events(source_id, track_id, title, artist, album, artist_id, album_id, cover, duration, seconds, ended, played_at, track_json) "
                     "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13);");
        if (!st) {
            return;
        }
        st.bind(1, source_id(track.source, track.instance));
        st.bind(2, track.id);
        st.bind(3, track.title);
        st.bind(4, track.artist);
        st.bind(5, track.album);
        st.bind(6, track.artist_id);
        st.bind(7, track.album_id);
        st.bind(8, track.cover);
        st.bind(9, static_cast<int64_t>(track.duration));
        st.bind(10, seconds);
        st.bind(11, ended ? 1 : 0);
        st.bind(12, unix_now(played_at));
        st.bind(13, encode_track(track));
        if (st.step_done()) {
            ++stats_serial_;
        }
    }

    Store::ListeningStats Store::listening_stats(StatsRange range, int64_t now) const {
        ListeningStats out;
        if (!db_) {
            return out;
        }
        const int64_t ts = unix_now(now);
        const int64_t since = range == StatsRange::week ? ts - 7 * 86400 : range == StatsRange::month ? ts - 30 * 86400 : 0;

        // 1. Totals & distinct tracks.
        {
            Stmt st(db_, "SELECT COALESCE(SUM(seconds), 0), COUNT(*), COUNT(DISTINCT source_id || ':' || track_id) "
                         "FROM listen_events WHERE played_at >= ?1;");
            if (st) {
                st.bind(1, since);
                if (st.step_row()) {
                    out.total_seconds = st.i64(0);
                    out.total_plays = st.i32(1);
                    out.unique_tracks = st.i32(2);
                }
            }
        }
        if (out.total_plays == 0) {
            return out;
        }

        // 2. Platform distribution.
        {
            Stmt st(db_, "SELECT source_id, COUNT(*), COALESCE(SUM(seconds), 0) "
                         "FROM listen_events WHERE played_at >= ?1 "
                         "GROUP BY source_id ORDER BY SUM(seconds) DESC, COUNT(*) DESC;");
            if (st) {
                st.bind(1, since);
                while (st.step_row()) {
                    out.by_source.push_back({st.text(0), st.i32(1), st.i64(2)});
                }
            }
        }

        // 3. Top tracks (latest metadata via MAX(id)).
        {
            Stmt st(db_, "SELECT e.track_json, agg.plays, agg.secs "
                         "FROM ("
                         "  SELECT source_id, track_id, COUNT(*) AS plays, SUM(seconds) AS secs, MAX(id) AS latest_id "
                         "  FROM listen_events WHERE played_at >= ?1 "
                         "  GROUP BY source_id, track_id "
                         "  ORDER BY plays DESC, secs DESC LIMIT 30"
                         ") agg "
                         "JOIN listen_events e ON e.id = agg.latest_id "
                         "ORDER BY agg.plays DESC, agg.secs DESC;");
            if (st) {
                st.bind(1, since);
                while (st.step_row()) {
                    if (auto track = decode_track(st.text(0))) {
                        out.top_tracks.push_back({std::move(*track), st.i32(1), st.i64(2)});
                    }
                }
            }
        }

        // 4. Artist distribution across all platforms (splitting collaborations cleanly).
        {
            struct ArtistAgg {
                ArtistStat stat;
                int64_t best_id_secs = -1;
                std::unordered_set<std::string> tracks;
            };
            std::unordered_map<std::string, ArtistAgg> by_artist;
            Stmt st(db_, "SELECT source_id, track_id, artist, artist_id, COUNT(*), SUM(seconds) "
                         "FROM listen_events WHERE played_at >= ?1 "
                         "GROUP BY source_id, track_id;");
            if (st) {
                st.bind(1, since);
                while (st.step_row()) {
                    const std::string sid = st.text(0);
                    const std::string tid = st.text(1);
                    const std::string artist_raw = st.text(2);
                    const std::string aid = st.text(3);
                    const int plays = st.i32(4);
                    const int64_t secs = st.i64(5);
                    const auto sk = parse_source_key(sid).value_or(SourceKey{});
                    const auto names = split_artists(artist_raw);
                    for (size_t i = 0; i < names.size(); ++i) {
                        std::string name(names[i]);
                        auto &agg = by_artist[name];
                        if (agg.stat.name.empty()) {
                            agg.stat.name = name;
                            agg.stat.source = sk.source;
                            agg.stat.instance = sk.instance;
                        }
                        agg.stat.plays += plays;
                        agg.stat.seconds += secs;
                        agg.tracks.insert(sid + ":" + tid);
                        // First artist owns Track::artist_id; pick the link from their most-played platform.
                        if (i == 0 && !aid.empty() && secs > agg.best_id_secs) {
                            agg.best_id_secs = secs;
                            agg.stat.artist_id = aid;
                            agg.stat.source = sk.source;
                            agg.stat.instance = sk.instance;
                        }
                    }
                }
            }
            out.unique_artists = static_cast<int>(by_artist.size());
            out.top_artists.reserve(by_artist.size());
            for (auto &[_, agg] : by_artist) {
                agg.stat.tracks = static_cast<int>(agg.tracks.size());
                out.top_artists.push_back(std::move(agg.stat));
            }
            std::ranges::sort(out.top_artists, [](const ArtistStat &a, const ArtistStat &b) {
                return a.seconds != b.seconds ? a.seconds > b.seconds : a.plays > b.plays;
            });
            if (out.top_artists.size() > 12) {
                out.top_artists.resize(12);
            }
        }
        return out;
    }

    void Store::clear_listening_stats() {
        if (exec("DELETE FROM listen_events;")) {
            ++stats_serial_;
        }
    }

    void Store::set_track_pin(const Track &track, bool pinned, int64_t now) {
        if (!db_ || track.id.empty()) {
            return;
        }
        const std::string sid = source_id(track.source, track.instance);
        const int64_t ts = unix_now(now);
        exec("BEGIN IMMEDIATE;");
        if (pinned) {
            Stmt up(db_, "INSERT INTO pinned_tracks(source_id, track_id, manual, pinned_at, track_json) "
                         "VALUES(?1, ?2, 1, ?3, ?4) "
                         "ON CONFLICT(source_id, track_id) DO UPDATE SET manual = 1, pinned_at = excluded.pinned_at, track_json = excluded.track_json;");
            if (up) {
                up.bind(1, sid);
                up.bind(2, track.id);
                up.bind(3, ts);
                up.bind(4, encode_track(track));
                up.step_done();
            }
        } else {
            Stmt clr(db_, "UPDATE pinned_tracks SET manual = 0 WHERE source_id = ?1 AND track_id = ?2;");
            if (clr) {
                clr.bind(1, sid);
                clr.bind(2, track.id);
                clr.step_done();
            }
            Stmt prune(db_, "DELETE FROM pinned_tracks WHERE source_id = ?1 AND track_id = ?2 AND manual = 0 "
                            "AND NOT EXISTS (SELECT 1 FROM pinned_list_tracks WHERE track_source_id = ?1 AND track_id = ?2);");
            if (prune) {
                prune.bind(1, sid);
                prune.bind(2, track.id);
                prune.step_done();
            }
        }
        exec("COMMIT;");
        ++pins_serial_;
    }

    void Store::set_list_pin(const Card &list, std::span<const Track> tracks, bool pinned, int64_t now) {
        if (!db_ || list.id.empty()) {
            return;
        }
        const std::string lsid = source_id(list.source, list.instance);
        const int lkind = static_cast<int>(list.kind);
        const int64_t ts = unix_now(now);
        exec("BEGIN IMMEDIATE;");
        if (pinned) {
            Stmt up_list(db_, "INSERT INTO pinned_lists(source_id, kind, list_id, title, pinned_at) VALUES(?1, ?2, ?3, ?4, ?5) "
                              "ON CONFLICT(source_id, kind, list_id) DO UPDATE SET title = excluded.title, pinned_at = excluded.pinned_at;");
            if (up_list) {
                up_list.bind(1, lsid);
                up_list.bind(2, lkind);
                up_list.bind(3, list.id);
                up_list.bind(4, list.title);
                up_list.bind(5, ts);
                up_list.step_done();
            }
            Stmt del_links(db_, "DELETE FROM pinned_list_tracks WHERE list_source_id = ?1 AND list_kind = ?2 AND list_id = ?3;");
            if (del_links) {
                del_links.bind(1, lsid);
                del_links.bind(2, lkind);
                del_links.bind(3, list.id);
                del_links.step_done();
            }
            Stmt up_track(db_, "INSERT INTO pinned_tracks(source_id, track_id, manual, pinned_at, track_json) VALUES(?1, ?2, 0, ?3, ?4) "
                               "ON CONFLICT(source_id, track_id) DO UPDATE SET track_json = excluded.track_json;");
            Stmt ins_link(db_, "INSERT OR IGNORE INTO pinned_list_tracks(list_source_id, list_kind, list_id, track_source_id, track_id) "
                               "VALUES(?1, ?2, ?3, ?4, ?5);");
            for (const auto &t : tracks) {
                if (t.id.empty()) {
                    continue;
                }
                const std::string tsid = source_id(t.source, t.instance);
                if (up_track) {
                    up_track.reset();
                    up_track.bind(1, tsid);
                    up_track.bind(2, t.id);
                    up_track.bind(3, ts);
                    up_track.bind(4, encode_track(t));
                    up_track.step_done();
                }
                if (ins_link) {
                    ins_link.reset();
                    ins_link.bind(1, lsid);
                    ins_link.bind(2, lkind);
                    ins_link.bind(3, list.id);
                    ins_link.bind(4, tsid);
                    ins_link.bind(5, t.id);
                    ins_link.step_done();
                }
            }
        } else {
            Stmt del_list(db_, "DELETE FROM pinned_lists WHERE source_id = ?1 AND kind = ?2 AND list_id = ?3;");
            if (del_list) {
                del_list.bind(1, lsid);
                del_list.bind(2, lkind);
                del_list.bind(3, list.id);
                del_list.step_done();
            }
            exec("DELETE FROM pinned_tracks WHERE manual = 0 "
                 "AND NOT EXISTS (SELECT 1 FROM pinned_list_tracks p WHERE p.track_source_id = pinned_tracks.source_id AND p.track_id = pinned_tracks.track_id);");
        }
        exec("COMMIT;");
        ++pins_serial_;
    }

    void Store::update_pinned_cache(const Track &track, std::string_view cache_key, std::string_view cache_hash) {
        if (!db_ || track.id.empty() || cache_hash.empty()) {
            return;
        }
        Stmt st(db_, "UPDATE pinned_tracks SET cache_key = ?1, cache_hash = ?2 WHERE source_id = ?3 AND track_id = ?4;");
        if (!st) {
            return;
        }
        st.bind(1, cache_key);
        st.bind(2, cache_hash);
        st.bind(3, source_id(track.source, track.instance));
        st.bind(4, track.id);
        if (st.step_done() && sqlite3_changes(db_) > 0) {
            ++pins_serial_;
        }
    }

    std::vector<Store::PinnedTrack> Store::pinned_tracks() const {
        std::vector<PinnedTrack> out;
        Stmt st(db_, "SELECT track_json, cache_key, cache_hash, manual, pinned_at FROM pinned_tracks ORDER BY pinned_at DESC;");
        if (!st) {
            return out;
        }
        while (st.step_row()) {
            if (auto t = decode_track(st.text(0))) {
                out.push_back({std::move(*t), st.text(1), st.text(2), st.i32(3) != 0, st.i64(4)});
            }
        }
        return out;
    }

    std::vector<Store::PinnedList> Store::pinned_lists() const {
        std::vector<PinnedList> out;
        Stmt st(db_, "SELECT source_id, list_id, title, kind, pinned_at FROM pinned_lists ORDER BY pinned_at DESC;");
        if (!st) {
            return out;
        }
        while (st.step_row()) {
            out.push_back({st.text(0), st.text(1), st.text(2), static_cast<CardKind>(st.i32(3)), st.i64(4)});
        }
        return out;
    }

    std::vector<std::string> Store::pinned_cache_hashes() const {
        std::vector<std::string> out;
        Stmt st(db_, "SELECT DISTINCT cache_hash FROM pinned_tracks WHERE cache_hash != '';");
        if (!st) {
            return out;
        }
        while (st.step_row()) {
            out.push_back(st.text(0));
        }
        return out;
    }

    std::optional<std::string> Store::pinned_cache_hash(const Track &track) const {
        Stmt st(db_, "SELECT cache_hash FROM pinned_tracks WHERE source_id = ?1 AND track_id = ?2 AND cache_hash != '';");
        if (!st) {
            return std::nullopt;
        }
        st.bind(1, source_id(track.source, track.instance));
        st.bind(2, track.id);
        if (!st.step_row()) {
            return std::nullopt;
        }
        return st.text(0);
    }

} // namespace lyren
