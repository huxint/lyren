#include <algorithm>
#include <ranges>

#include "app/model/library.hpp"
#include "core/i18n.hpp"

// What a platform offers whoever is listening: recommendations, charts, the playlist square, new releases, the radio,
// and the albums and artists the account keeps.
namespace lyren {

    // Keep the service's continuation independent of the number of visible rows.
    void Library::arrived(Listing &listing, Result<Batch> batch) {
        listing.loading = false;
        if (!batch) {
            listing.error = batch.error();
            return;
        }
        if (!listing.loaded) {
            listing.since = env_.now();
        }
        listing.loaded = true;
        env_.images.prefetch(batch->tracks);
        env_.images.prefetch(batch->cards);
        listing.next_offset = batch->next_offset;
        listing.tracks.append_range(std::views::as_rvalue(batch->tracks));
        listing.cards.append_range(std::views::as_rvalue(batch->cards));
        listing.total = listing.more() ? std::max(batch->total, static_cast<int>(listing.size())) : static_cast<int>(listing.size());
    }

    Task<void> Library::load_cards(Source source, Listing Shelf::*listing, Task<Result<std::vector<Card>>> request) {
        Shelf &lib = shelf(source);
        const uint64_t gen = lib.account_gen;
        (lib.*listing).loading = true;
        auto result = co_await std::move(request);
        if (gen != lib.account_gen) {
            co_return;
        }
        Listing fresh;
        arrived(fresh, std::move(result).transform([](std::vector<Card> cards) {
            return Batch{.cards = std::move(cards)};
        }));
        lib.*listing = std::move(fresh);
        env_.redraw();
    }

    Task<void> Library::load_recommended(Source source) {
        return load_cards(source, &Shelf::recommended, env_.provider(source).recommended_playlists());
    }

    Task<void> Library::load_toplists(Source source) {
        return load_cards(source, &Shelf::toplists, env_.provider(source).toplists());
    }

    Task<void> Library::load_daily_lists(Source source) {
        Shelf &lib = shelf(source);
        const uint64_t gen = lib.gen;
        auto r = co_await env_.provider(source).features.daily.playlists();
        if (gen != lib.gen || !r) {
            co_return;
        }
        lib.daily_lists = std::move(*r);
        env_.images.prefetch(lib.daily_lists);
        lib.daily_since = env_.now();
        env_.redraw();
    }

    Task<void> Library::load_categories(Source source) {
        Shelf &lib = shelf(source);
        lib.categories_loading = true;
        auto r = co_await env_.provider(source).categories();
        lib.categories_loading = false;
        if (r) {
            lib.categories = std::move(*r);
        }
        env_.redraw();
    }

    void Library::set_category(Source source, Category category) {
        Shelf &lib = shelf(source);
        if (category.id == lib.category.id) {
            return;
        }
        lib.category = std::move(category);
        ++lib.square_gen;
        lib.square = {};
        lib.square_since = env_.now();
        load_square(source);
    }

    void Library::load_square(Source source) {
        Shelf &lib = shelf(source);
        // A first page that failed is asked for again; later pages wait for their retry.
        if (!lib.square.open(!lib.square.loaded)) {
            return;
        }
        lib.square.loading = true;
        lib.square.error.clear();
        spawn(fetch_square(source, lib.square_gen, lib.category.id, lib.square.next_offset));
    }

    Task<void> Library::fetch_square(Source source, uint64_t gen, std::string category, int offset) {
        auto r = co_await env_.provider(source).square(std::move(category), offset);
        Shelf &lib = shelf(source);
        if (gen != lib.square_gen) {
            co_return;
        }
        arrived(lib.square, std::move(r));
        env_.redraw();
    }

    void Library::set_release_category(Source source, int category) {
        Shelf &lib = shelf(source);
        const int tab = lib.release_tab;
        lib.release_category[tab] = category;
        ++lib.release_gen[tab];
        lib.releases[tab] = {};
        load_releases(source);
    }

    void Library::load_releases(Source source) {
        Shelf &lib = shelf(source);
        const int tab = lib.release_tab;
        Listing &listing = lib.releases[tab];
        const auto &categories = env_.provider(source).release_categories[tab];
        if (!listing.open(!listing.loaded) || categories.empty()) {
            return;
        }
        listing.loading = true;
        listing.error.clear();
        const auto &category = categories[std::min(static_cast<size_t>(lib.release_category[tab]), categories.size() - 1)];
        spawn(fetch_releases(source, tab, lib.release_gen[tab], category.id, listing.next_offset));
    }

    Task<void> Library::fetch_releases(Source source, int tab, uint64_t gen, std::string category, int offset) {
        // Not a ?: chain: with co_await in its branches GCC 16 also starts the tasks of branches not taken.
        Result<Batch> r;
        if (tab == 0) {
            auto songs = co_await env_.provider(source).new_songs(std::move(category));
            r = std::move(songs).transform([](std::vector<Track> tracks) {
                const int total = static_cast<int>(tracks.size());
                return Batch{.tracks = std::move(tracks), .total = total};
            });
        } else {
            r = co_await env_.provider(source).new_albums(std::move(category), offset);
        }
        Shelf &lib = shelf(source);
        if (gen != lib.release_gen[tab]) {
            co_return;
        }
        arrived(lib.releases[tab], std::move(r));
        env_.redraw();
    }

    Task<void> Library::load_collection(Source source) {
        Shelf &lib = shelf(source);
        const uint64_t gen = ++lib.collection_gen, revision = lib.preference_gen;
        for (auto &listing : lib.collection) {
            listing.loading = true;
        }
        auto collect = [](Provider &provider, bool artists) -> Task<Result<std::vector<Card>>> {
            if (artists && provider.capabilities.follow_artists) {
                co_return co_await provider.followed_artists();
            }
            if (!artists && provider.capabilities.collect_albums) {
                co_return co_await provider.collected_albums();
            }
            co_return std::vector<Card>{};
        };
        Provider &provider = env_.provider(source);
        auto [albums, artists] = co_await both(collect(provider, false), collect(provider, true));
        if (gen != lib.collection_gen) {
            co_return;
        }
        auto batch = [](std::vector<Card> cards) {
            return Batch{.cards = std::move(cards)};
        };
        std::array<Listing, 2> fresh;
        arrived(fresh[0], std::move(albums).transform(batch));
        arrived(fresh[1], std::move(artists).transform(batch));
        // A reload does not replay the cards' entrance.
        for (int i = 0; i < 2; ++i) {
            if (lib.collection[i].loaded) {
                fresh[i].since = lib.collection[i].since;
            }
            lib.collection[i] = std::move(fresh[i]);
            if (lib.collection[i].loaded) {
                reconcile_all(source, i == 0 ? Preference::album : Preference::artist, revision);
            }
        }
        env_.redraw();
    }

    Task<void> Library::load_radio(Source source) {
        Shelf &lib = shelf(source);
        const uint64_t gen = lib.gen;
        lib.radio_loading = true;
        lib.radio_error.clear();
        auto r = co_await env_.provider(source).fm();
        if (gen != lib.gen) {
            co_return;
        }
        lib.radio_loading = false;
        const bool pending = std::exchange(lib.radio_pending, false);
        if (r) {
            const size_t before = lib.radio.size();
            env_.images.prefetch(*r);
            lib.radio.append_range(std::views::as_rvalue(*r));
            if (pending && lib.radio.size() > before) {
                if (before > 0 && lib.radio_pos + 1 < static_cast<int>(lib.radio.size())) {
                    ++lib.radio_pos;
                }
                lib.radio_since = env_.now();
                emit({.kind = Event::Kind::radio_ready, .source = source});
            }
        } else {
            lib.radio_error = r.error();
            if (pending) {
                env_.notify(lyren::trf("Playback failed: {}", r.error()), "triangle-alert", true);
            }
        }
        env_.redraw();
    }

    bool Library::advance_radio(Source source) {
        Shelf &lib = shelf(source);
        if (lib.radio_pos + 1 >= static_cast<int>(lib.radio.size()) && !lib.radio_loading) {
            // Ask for the next batch now; if the provider answers immediately, we advance below.
            spawn(load_radio(source));
        }
        const int count = static_cast<int>(lib.radio.size());
        if (lib.radio_pos + 1 < count) {
            ++lib.radio_pos;
            lib.radio_pending = false;
            if (lib.radio_pos + 2 >= count && !lib.radio_loading) {
                spawn(load_radio(source));
            }
            lib.radio_since = env_.now();
            return true;
        }
        lib.radio_pending = lib.radio_loading;
        return false;
    }

    void Library::dislike_radio(Track track) {
        spawn([](Library *self, Track track, uint64_t gen) -> Task<void> {
            auto r = co_await self->env_.provider(track).fm_trash(track);
            if (gen != self->shelf(track.source).gen) {
                co_return;
            }
            if (!r) {
                co_return self->env_.notify(lyren::trf("Could not dislike this song: {}", r.error()), "triangle-alert", true);
            }
            if (self->env_.provider(track).features.exclusions.list) {
                spawn(self->load_dislikes(track.source, DislikeKind::song));
            }
            self->env_.notify(std::string(lyren::tr("You will hear fewer songs like this")), "thumbs-down");
        }(this, track, shelf(track.source).gen));
    }

} // namespace lyren
