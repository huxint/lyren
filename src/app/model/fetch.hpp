#pragma once
#include <algorithm>
#include <cstdint>
#include <inplace_vector>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "media/types.hpp"

// The shapes of what the services send: something fetched once, a list that comes a page at a time.
namespace lyren {

    // How something asked of a service stands.
    struct Fetch {
        bool loading = false, loaded = false, refreshing = false, stale = false;
        std::string error;
        double since = -10; // when it arrived, for its entrance
        double fetched_at = -1e9, retry_at = -1e9;
        uint64_t gen = 0; // bumped to disown the requests under way

        bool busy() const {
            return loading || refreshing;
        }
        bool due(double now, double ttl, double stale_ttl = 0.0) const {
            return now >= retry_at && (now - fetched_at >= ttl || (stale && now - fetched_at >= stale_ttl));
        }
        void begin(bool quiet = false) {
            if (quiet) {
                refreshing = true;
            } else {
                loading = true;
                refreshing = false;
                error.clear();
            }
        }
        void succeed(double now, bool mark_since) {
            loading = refreshing = false;
            loaded = true;
            stale = false;
            error.clear();
            fetched_at = now;
            retry_at = -1e9;
            if (mark_since) {
                since = now;
            }
        }
        void fail(std::string_view err, double now, bool quiet = false, double backoff = 15.0) {
            loading = refreshing = false;
            if (quiet) {
                retry_at = now + backoff;
            } else {
                loaded = true;
                error = err;
            }
        }
    };

    // Cumulative row bottoms of a list as last laid out, kept beside the items so that they share their lifetime.
    struct Heights {
        std::vector<float> ends;
        float width = 0, size = 0; // what they were laid out for: the column's width and the text size

        void reset(float w, float s) {
            ends.clear();
            width = w, size = s;
        }
        void push(float h) {
            ends.push_back(total() + h);
        }
        size_t size_count() const {
            return ends.size();
        }
        float total() const {
            return ends.empty() ? 0.f : ends.back();
        }
        float top(size_t i) const {
            return i == 0 ? 0.f : ends[i - 1];
        }
        size_t first_at(float y) const {
            return static_cast<size_t>(std::ranges::upper_bound(ends, y) - ends.begin());
        }
    };

    // Items that arrive a page at a time, each page naming the next by a cursor.
    template <class T>
    struct Paged : Fetch {
        std::vector<T> items;
        std::string cursor;
        int page = 0;   // pages arrived
        int total = -1; // as the service counts them; negative while it has not said
        bool more = true;
        uint64_t version = 0; // bumped whenever items are appended or removed
        Heights heights;

        // The next page may be asked for; a failed one waits for its retry (or retries page 0 if retry_first is set).
        bool open(bool retry_first = false) const {
            return !loading && more && ((retry_first && page == 0) || error.empty());
        }
        void append(std::vector<T> &&batch, std::string &&next_cursor, bool has_more, double now, int next_total = -1) {
            if (page == 0 && items.empty()) {
                since = now;
            }
            items.append_range(std::views::as_rvalue(batch));
            if (next_total >= 0 && (page == 0 || next_total > 0)) {
                total = next_total;
            }
            more = has_more;
            cursor = std::move(next_cursor);
            loaded = true;
            ++page;
            ++version;
        }
    };

    // Songs and cards a page at a time, by offset: search results, charts, an artist's albums.
    struct Listing : Fetch {
        std::vector<Track> tracks;
        std::vector<Card> cards;
        int total = 0;
        int next_offset = 0; // negative once everything is here

        size_t size() const {
            return tracks.size() + cards.size();
        }
        bool more() const {
            return next_offset >= 0;
        }
        // The next page may be asked for. retry: also after a failure.
        bool open(bool retry = false) const {
            return !loading && (retry || (more() && error.empty()));
        }
    };

    // Fixed-capacity LRU cache (most recently used entry at back).
    template <class Entry, size_t N>
    struct LruCache {
        std::inplace_vector<Entry, N> items;

        template <class Pred>
        Entry *find(Pred &&match, bool touch = true) {
            auto it = std::ranges::find_if(items, match);
            if (it == items.end()) {
                return nullptr;
            }
            if (touch) {
                std::rotate(it, it + 1, items.end());
                return &items.back();
            }
            return &*it;
        }
        Entry &push(Entry entry) {
            if (items.size() == items.capacity()) {
                items.erase(items.begin());
            }
            items.push_back(std::move(entry));
            return items.back();
        }
        template <class Pred>
        void erase_if(Pred &&match) {
            std::erase_if(items, match);
        }
    };

} // namespace lyren
