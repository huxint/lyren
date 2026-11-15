#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "media/types.hpp"

// The shapes of what the services send: something fetched once, a list that comes a page at a time.
namespace lyren {

    // How something asked of a service stands.
    struct Fetch {
        bool loading = false, loaded = false;
        std::string error;
        double since = -10; // when it arrived, for its entrance
        uint64_t gen = 0;   // bumped to disown the requests under way
    };

    // The heights of a list's rows as last laid out, kept beside the items so that they share their lifetime.
    struct Heights {
        std::vector<float> rows;
        float width = 0, size = 0; // what they were laid out for: the column's width and the text size
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
