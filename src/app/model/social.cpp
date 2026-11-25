#include "app/model/social.hpp"

#include <algorithm>
#include <ranges>

#include "app/model/library.hpp"
#include "core/i18n.hpp"

namespace lyren {

    Social::Social(Env &env, Library &library)
    : env_(env),
      library_(library) {
        library.subscribe([this](const Library::Event &event) {
            if (event.kind == Library::Event::Kind::left) {
                likes_.forget(event.source);
                removals_.forget(event.source);
                feeds_[static_cast<size_t>(event.source)].reset();
            }
        });
    }

    template <class F>
    void Social::each_page(F &&visit) {
        std::erase_if(pages_, [](const std::weak_ptr<Page> &page) {
            return page.expired();
        });
        for (const auto &weak : pages_) {
            if (auto page = weak.lock()) {
                visit(page);
            }
        }
    }

    std::vector<Social::Section> Social::sections(const Card &who) const {
        const auto &caps = env_.provider(who).capabilities;
        const bool self = library_.own(who) || who.owned;
        std::vector<Section> out;
        if (self) {
            out.push_back(Section::overview);
        }
        if (caps.posts) {
            out.push_back(Section::posts);
        }
        if (caps.ranking) {
            out.push_back(Section::chart);
        }
        if (self ? caps.library : caps.profile_playlists) {
            out.push_back(Section::playlists);
        }
        if (self && (caps.collect_albums || caps.follow_artists)) {
            out.push_back(Section::collection);
        }
        if (caps.people) {
            out.push_back(Section::people);
        }
        return out;
    }

    std::shared_ptr<Social::Page> Social::open(Card who) {
        auto page = std::make_shared<Page>();
        page->who = std::move(who);
        page->since = page->section_since = env_.now();
        each_page([](const auto &) {});
        pages_.push_back(page);
        spawn(load_profile(page));
        return page;
    }

    void Social::show(const std::shared_ptr<Page> &page, Section section) {
        page->section = section;
        page->section_since = env_.now();
        if (section == Section::people) {
            load_people(page, page->people_tab != 0);
        } else if (section == Section::chart) {
            load_chart(page, page->chart_tab);
        } else if (section == Section::collection && (library_.own(page->who) || page->who.owned)) {
            const auto &shelf = library_.shelf(page->who.source);
            const auto &res = shelf.collection[static_cast<size_t>(page->collection_tab)];
            if (!res.loaded && !res.loading && env_.signed_in(page->who.source)) {
                spawn(library_.load_collection(page->who.source));
            }
        }
        env_.redraw();
    }

    Task<void> Social::load_profile(std::shared_ptr<Page> page) {
        const Card &who = page->who;
        const auto &shelf = library_.shelf(who.source);
        auto &provider = env_.provider(who);
        const auto &caps = provider.capabilities;
        const bool self = library_.own(who) || who.owned;
        const uint64_t account_gen = shelf.account_gen;
        page->loading = true;
        page->error.clear();
        if (const auto parts = sections(who); !parts.empty()) {
            page->section = parts.front();
        }
        if (caps.posts) {
            load_posts(page->posts, who.source, who.id);
        }
        if (caps.people) {
            load_people(page, false);
            load_people(page, true);
        }
        if (caps.ranking) {
            load_chart(page, 0);
        }
        if (self && provider.features.recent_plays && env_.signed_in(who.source)) {
            load_recent(page);
        }
        if (self && (caps.collect_albums || caps.follow_artists) && !shelf.collection[0].loaded && !shelf.collection[0].loading && shelf.collection[0].error.empty() &&
            shelf.collection[1].error.empty() && env_.signed_in(who.source)) {
            spawn(library_.load_collection(who.source));
        }
        if (!self && caps.profile_playlists) {
            spawn(load_playlists(page));
        }
        // A platform without listener pages still has the account's name and picture.
        if (!caps.profiles) {
            page->loading = false;
            page->info = Profile{.id = who.id, .name = who.title, .avatar = who.cover};
            co_return;
        }
        auto r = co_await provider.profile(who.id);
        if (account_gen != shelf.account_gen) {
            co_return;
        }
        page->loading = false;
        if (r) {
            env_.images.prefetch(r->avatar, 256);
            page->info = std::move(*r);
        } else {
            page->error = r.error();
        }
        env_.redraw();
    }

    void Social::load_people(std::shared_ptr<Page> page, bool fans) {
        People &list = page->people[fans];
        if (!list.open()) {
            return;
        }
        list.loading = true;
        spawn(fetch_people(std::move(page), fans, ++list.gen));
    }

    Task<void> Social::fetch_people(std::shared_ptr<Page> page, bool fans, uint64_t gen) {
        People &list = page->people[fans];
        auto r = co_await env_.provider(page->who).people(page->who.id, fans, list.cursor);
        if (gen != list.gen) {
            co_return;
        }
        list.loading = false;
        if (!r) {
            list.error = r.error();
            co_return env_.redraw();
        }
        if (list.items.empty()) {
            list.since = env_.now();
        }
        for (const auto &person : r->people) {
            env_.images.prefetch(person.avatar, 64);
        }
        list.items.append_range(std::views::as_rvalue(r->people));
        list.total = r->total >= 0 ? r->total : list.total;
        list.more = r->more;
        list.cursor = std::move(r->cursor);
        list.loaded = true;
        ++list.page;
        env_.redraw();
    }

    void Social::load_chart(std::shared_ptr<Page> page, int tab) {
        Chart &chart = page->charts[static_cast<size_t>(tab)];
        if (chart.loading || chart.loaded) {
            return;
        }
        chart.loading = true;
        chart.error.clear();
        spawn(fetch_chart(std::move(page), tab));
    }

    Task<void> Social::fetch_chart(std::shared_ptr<Page> page, int tab) {
        auto r = co_await env_.provider(page->who).ranking(page->who.id, tab == 1);
        Chart &chart = page->charts[static_cast<size_t>(tab)];
        chart.loading = false;
        chart.loaded = true;
        chart.since = env_.now();
        if (r) {
            env_.images.prefetch(r->tracks);
            chart.ranking = std::move(*r);
        } else {
            chart.error = r.error();
        }
        // An empty week leaves the chart on the songs of all time.
        if (tab == 0 && chart.error.empty() && chart.ranking.tracks.empty()) {
            page->chart_tab = 1;
            load_chart(page, 1);
        }
        env_.redraw();
    }

    void Social::load_recent(std::shared_ptr<Page> page) {
        auto &recent = page->recent;
        if (recent.loading || recent.loaded || !env_.provider(page->who).features.recent_plays) {
            return;
        }
        recent.loading = true;
        recent.error.clear();
        spawn(fetch_recent(std::move(page)));
    }

    Task<void> Social::fetch_recent(std::shared_ptr<Page> page) {
        auto &shelf = library_.shelf(page->who.source);
        const uint64_t account_gen = shelf.account_gen;
        Result<std::vector<Track>> r;
        if (shelf.recent_plays_loaded && env_.now() - shelf.recent_plays_at < 60.0) {
            r = shelf.recent_plays;
        } else {
            r = co_await env_.provider(page->who).features.recent_plays();
            if (account_gen != shelf.account_gen) {
                co_return;
            }
            if (r) {
                shelf.recent_plays = *r;
                shelf.recent_plays_at = env_.now();
                shelf.recent_plays_loaded = true;
            }
        }
        auto &recent = page->recent;
        recent.loading = false;
        recent.loaded = true;
        recent.since = env_.now();
        if (r) {
            env_.images.prefetch(*r);
            recent.tracks = std::move(*r);
        } else {
            recent.error = r.error();
        }
        env_.redraw();
    }

    Task<void> Social::load_playlists(std::shared_ptr<Page> page) {
        Listing &listing = page->playlists;
        listing.loading = true;
        auto r = co_await env_.provider(page->who).user_playlists(page->who.id);
        listing.loading = false;
        listing.loaded = true;
        listing.next_offset = -1;
        listing.since = env_.now();
        if (!r) {
            listing.error = r.error();
            co_return env_.redraw();
        }
        std::vector<Card> cards;
        if (!r->first.id.empty()) {
            // A listener's liked playlist belongs with the ones they created, never with saved ones.
            r->first.owned = true;
            cards.push_back(std::move(r->first));
        }
        cards.append_range(std::views::as_rvalue(r->second));
        // Theirs come first. They are not the listener's to edit, whoever made them.
        std::ranges::stable_partition(cards, &Card::owned);
        page->created = static_cast<int>(std::ranges::count_if(cards, &Card::owned));
        for (auto &card : cards) {
            card.owned = false;
        }
        env_.images.prefetch(cards);
        listing.cards = std::move(cards);
        env_.redraw();
    }

    void Social::load_posts(std::shared_ptr<Posts> list, Source source, std::string author) {
        if (!list->open()) {
            return;
        }
        list->loading = true;
        const uint64_t gen = ++list->gen;
        spawn(fetch_posts(std::move(list), source, std::move(author), gen));
    }

    Task<void> Social::fetch_posts(std::shared_ptr<Posts> list, Source source, std::string author, uint64_t gen) {
        // Not a ?: chain: GCC 16 would start both tasks.
        Result<PostPage> r;
        if (author.empty()) {
            r = co_await env_.provider(source).feed(list->cursor);
        } else {
            r = co_await env_.provider(source).posts(author, list->cursor);
        }
        if (gen != list->gen) {
            co_return;
        }
        list->loading = false;
        if (!r) {
            list->error = r.error();
            co_return env_.redraw();
        }
        if (list->items.empty()) {
            list->since = env_.now();
        }
        for (const auto &post : r->posts) {
            env_.images.prefetch(post.author.avatar, 64);
            if (post.track) {
                env_.images.prefetch(post.track->cover, 64);
            }
        }
        list->items.append_range(std::views::as_rvalue(r->posts));
        list->more = r->more;
        list->cursor = std::move(r->cursor);
        list->loaded = true;
        ++list->page;
        env_.redraw();
    }

    std::shared_ptr<Social::Posts> Social::feed(Source source) {
        auto &feed = feeds_[static_cast<size_t>(source)];
        if (!feed) {
            feed = std::make_shared<Posts>();
        }
        return feed;
    }

    void Social::reload_posts(const std::shared_ptr<Page> &page, bool feed) {
        const Source source = page->who.source;
        // The requests under way keep the list they were made for.
        auto &list = feed ? feeds_[static_cast<size_t>(source)] : page->posts;
        list = std::make_shared<Posts>();
        load_posts(list, source, feed ? std::string() : page->who.id);
        env_.redraw();
    }

    int Social::fans(Source source, const Profile &who) const {
        if (who.fans < 0) {
            return who.fans;
        }
        const bool now = library_.following(source, {.id = who.id, .followed = who.followed});
        return std::max(0, who.fans + (now == who.followed ? 0 : now ? 1 : -1));
    }

    int Social::count(const Page &page, bool of_fans) const {
        if (!page.info) {
            return page.people[of_fans].total;
        }
        const int n = of_fans ? fans(page.who.source, *page.info) : page.info->follows;
        return n >= 0 ? n : page.people[of_fans].total;
    }

    void Social::toggle_like(Source source, const Post &post) {
        if (!env_.signed_in(source)) {
            return env_.notify(lyren::trf("Sign in to {} to like posts", lyren::tr(platform(source).name)), "thumbs-up");
        }
        auto write = likes_.flip(source, post.id, post.liked, post.likes);
        if (!write) {
            return;
        }
        env_.redraw();
        spawn([](Social *self, Source source, Post post, Likes::Write write) -> Task<void> {
            auto r = co_await self->env_.provider(source).like_post(post, write.liked);
            if (!self->likes_.settle(source, post.id, write, r.has_value())) {
                co_return;
            }
            if (!r) {
                self->env_.notify(lyren::trf("Could not update the like: {}", lyren::tr(r.error())), "triangle-alert", true);
            }
            self->env_.redraw();
        }(this, source, post, *write));
    }

    void Social::remove(Source source, Post post) {
        const uint64_t ticket = removals_.begin(source, post.id);
        env_.redraw();
        spawn([](Social *self, Source source, Post post, uint64_t ticket) -> Task<void> {
            auto r = co_await self->env_.provider(source).delete_post(post.id);
            if (!self->removals_.end(source, post.id, ticket)) {
                co_return;
            }
            if (!r) {
                co_return self->env_.notify(lyren::trf("Could not delete the post: {}", lyren::tr(r.error())), "triangle-alert", true);
            }
            self->env_.notify(std::string(lyren::tr("Post deleted")), "trash");
            // Out of every list that shows it.
            auto drop = [&](const std::shared_ptr<Posts> &list) {
                if (list && std::erase_if(list->items, [&](const Post &p) {
                        return p.id == post.id;
                    })) {
                    list->heights.ends.clear();
                }
            };
            self->each_page([&](const std::shared_ptr<Page> &page) {
                if (page->who.source != source) {
                    return;
                }
                drop(page->posts);
                if (page->info && page->info->id == post.author.id && page->info->posts > 0) {
                    --page->info->posts;
                }
            });
            drop(self->feeds_[static_cast<size_t>(source)]);
            self->env_.redraw();
        }(this, source, std::move(post), ticket));
    }

    bool Social::compose(Source source, std::optional<Track> track, std::optional<Post> forward) {
        if (!env_.signed_in(source)) {
            env_.notify(lyren::trf("Sign in to {} to post", lyren::tr(platform(source).name)), "user");
            return false;
        }
        if (draft_.sending) {
            env_.notify(std::string(lyren::tr("The last post is still sending")), "send");
            return false;
        }
        // Words already written stay for the next post of the same kind.
        if (draft_.source != source || draft_.forward.has_value() != forward.has_value()) {
            draft_.text.clear();
        }
        draft_.source = source;
        draft_.track = forward ? std::nullopt : std::move(track);
        draft_.forward = std::move(forward);
        draft_.focus = true;
        return true;
    }

    Task<void> Social::send() {
        Draft &draft = draft_;
        const Source source = draft.source;
        const auto &shelf = library_.shelf(source);
        const uint64_t account_gen = shelf.account_gen;
        draft.sending = true;
        Result<void> r;
        if (draft.forward) {
            r = co_await env_.provider(source).forward(*draft.forward, draft.text);
        } else {
            r = co_await env_.provider(source).share(draft.text, draft.track);
        }
        draft.sending = false;
        if (account_gen != shelf.account_gen) {
            co_return;
        }
        if (!r) {
            co_return env_.notify(lyren::trf("Could not post: {}", lyren::tr(r.error())), "triangle-alert", true);
        }
        draft.text.clear();
        draft.track.reset();
        draft.forward.reset();
        env_.notify(std::string(lyren::tr("Posted")), "circle-check");
        refresh_own(source);
    }

    void Social::refresh_own(Source source) {
        each_page([&](const std::shared_ptr<Page> &page) {
            if (page->who.source != source || !library_.own(page->who)) {
                return;
            }
            page->posts = std::make_shared<Posts>();
            load_posts(page->posts, source, page->who.id);
            spawn([](Social *self, std::shared_ptr<Page> page) -> Task<void> {
                auto r = co_await self->env_.provider(page->who).profile(page->who.id);
                if (r) {
                    page->info = std::move(*r);
                    self->env_.redraw();
                }
            }(this, page));
        });
        if (auto &feed = feeds_[static_cast<size_t>(source)]) {
            feed = std::make_shared<Posts>();
        }
        env_.redraw();
    }

} // namespace lyren
