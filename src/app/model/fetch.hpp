#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
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
        Heights heights;

        // The next page may be asked for; a failed one waits for its retry.
        bool open() const {
            return !loading && more && error.empty();
        }
    };

    // Songs and cards a page at a time, by offset: search results, charts, an artist's albums.
    struct Listing {
        std::vector<Track> tracks;
        std::vector<Card> cards;
        int total = 0;
        int next_offset = 0; // negative once everything is here
        bool loading = false, loaded = false;
        std::string error;
        double since = -10;

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

} // namespace lyren
