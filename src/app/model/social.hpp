#pragma once
#include <array>
#include <inplace_vector>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/env.hpp"
#include "app/model/fetch.hpp"
#include "app/model/writes.hpp"

namespace lyren {

    class Library;

    // Listeners and what they post: a listener's page with the people around them and the music they play, their
    // timeline, the feed of the people the account follows, and the post being written.
    class Social {
    public:
        using Posts = Paged<Post>;
        using People = Paged<Person>;
        struct Chart : Fetch {
            Ranking ranking;
        };
        // The parts of a listener's page.
        enum class Section : uint8_t {
            overview,
            posts,
            chart,
            playlists,
            collection,
            people
        };
        // A listener's identity, social lists and music, kept while cached or in the history.
        struct Page {
            Card who;
            std::optional<Profile> info;
            bool loading = false;
            std::string error;
            Section section = Section::overview;
            double section_since = -10, since = -10;
            int posts_tab = 0;            // the account's own page: its posts, or the feed of the people it follows
            int chart_tab = 0;            // the week or all time
            int collection_tab = 0;       // 0 = albums, 1 = artists
            int people_tab = 0;           // 0 = follows, 1 = fans
            std::array<People, 2> people; // follows, fans
            std::shared_ptr<Posts> posts = std::make_shared<Posts>();
            std::array<Chart, 2> charts;
            Listing playlists;
            int created = 0; // how many of the playlists they made; the rest they saved
        };
        struct Draft {
            Source source = Source::netease;
            std::string text;
            std::optional<Track> track;
            std::optional<Post> forward;
            bool sending = false, focus = false;
        };

        Social(Env &env, Library &library);
        Social(const Social &) = delete;
        Social &operator=(const Social &) = delete;

        // A page for the listener, reusing a cached visit when one is on hand.
        std::shared_ptr<Page> open(Card who);
        // Replaces any cached page for who with a fresh load, keeping the active section and tabs.
        std::shared_ptr<Page> reload(Card who);
        // The sections the listener's platform can fill, in the order they show.
        std::inplace_vector<Section, 6> sections(const Card &who) const;
        void show(const std::shared_ptr<Page> &page, Section section);
        void load_people(std::shared_ptr<Page> page, bool fans);
        void load_chart(std::shared_ptr<Page> page, int tab);
        // author: whose timeline; empty for the feed of the people the account follows.
        void load_posts(std::shared_ptr<Posts> list, Source source, std::string author);
        // The feed of the people the account on the platform follows.
        std::shared_ptr<Posts> feed(Source source);
        // Starts a timeline over from its newest posts: the listener's own, or the feed of the people their platform's
        // account follows.
        void reload_posts(const std::shared_ptr<Page> &page, bool feed);
        // The fans shown for someone, counting a follow the listener has just made or taken back; negative when unknown.
        int fans(Source source, const Profile &who) const;
        int count(const Page &page, bool fans) const;

        Likes::Mark like(Source source, const Post &post) const {
            return likes_.shown(source, post.id, post.liked, post.likes);
        }
        bool removing(Source source, const Post &post) const {
            return removals_.contains(source, post.id);
        }
        void toggle_like(Source source, const Post &post);
        void remove(Source source, Post post);

        Draft &draft() {
            return draft_;
        }
        // Readies the draft for a post that shares the song or passes the post on; false, with a note, when the
        // platform's account cannot post now.
        bool compose(Source source, std::optional<Track> track, std::optional<Post> forward);
        Task<void> send();

    private:
        bool is_own(const Card &who) const;
        std::shared_ptr<Page> find_page(const Card &who, bool touch);
        std::shared_ptr<Page> create(Card who, const Page *previous = nullptr, bool reloading = false);
        Task<void> load_profile(std::shared_ptr<Page> page, bool reloading = false);
        Task<void> fetch_people(std::shared_ptr<Page> page, bool fans, uint64_t gen);
        Task<void> fetch_chart(std::shared_ptr<Page> page, int tab);
        Task<void> load_playlists(std::shared_ptr<Page> page);
        Task<void> fetch_posts(std::shared_ptr<Posts> list, Source source, std::string author, uint64_t gen);
        // The account's own pages show what it has just posted.
        void refresh_own(Source source);
        template <class F>
        void each_page(F &&visit);

        Env &env_;
        Library &library_;
        std::vector<std::weak_ptr<Page>> pages_;
        std::array<std::shared_ptr<Page>, platforms.size()> own_pages_{};
        LruCache<std::shared_ptr<Page>, 8> other_pages_;
        std::array<std::shared_ptr<Posts>, platforms.size()> feeds_;
        Likes likes_;
        Removals removals_;
        Draft draft_;
    };

} // namespace lyren
