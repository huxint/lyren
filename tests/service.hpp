#pragma once
#include <array>
#include <format>
#include "media/provider.hpp"

namespace lyren::test {
    // In-memory provider for model and view tests. Hooks can suspend a request or return a service error.
    class Service : public Provider {
    public:
        static constexpr std::string_view self = "me", friend_id = "ada";

        bool signed_in = true;
        std::vector<std::string> calls;
        std::move_only_function<Task<Result<std::vector<Track>>>()> on_fm;
        std::move_only_function<Task<Result<Stream>>(Track, Quality)> on_stream;
        std::move_only_function<Task<Result<std::vector<Card>>>()> on_collected_albums;
        std::move_only_function<Task<Result<std::vector<Track>>>(Card)> on_tracks;
        std::move_only_function<Task<Result<AlbumInfo>>(std::string)> on_album;
        std::move_only_function<Task<Result<ArtistInfo>>(std::string)> on_artist;
        std::move_only_function<Task<Result<Batch>>(std::string, int)> on_artist_songs;
        std::move_only_function<Task<Result<Profile>>(std::string)> on_profile;
        std::move_only_function<Task<Result<PostPage>>(std::string, std::string)> on_posts;
        std::move_only_function<Task<Result<Batch>>(std::string, SearchKind, int, int)> on_search;
        std::move_only_function<Task<Result<std::vector<std::string>>>(std::string)> on_search_suggest;
        std::move_only_function<Task<Result<std::vector<LyricLine>>>(Track)> on_lyrics;
        std::move_only_function<Task<Result<CommentPage>>(Subject, std::string, bool, int, std::string)> on_comments;
        std::move_only_function<Task<Result<void>>(std::string)> on_import_session;
        std::move_only_function<Task<SessionCheck>(std::string)> on_check_session;

        Service() {
            capabilities.search[4] = true;
            capabilities.library = capabilities.likes = capabilities.playlist_edit = true;
            capabilities.collect_albums = capabilities.follow_artists = true;
            capabilities.profiles = capabilities.people = capabilities.follow_users = true;
            capabilities.posts = capabilities.post_write = capabilities.ranking = capabilities.profile_playlists = true;
            capabilities.comments = {true, true, true, true};
            capabilities.comment_sort = capabilities.comment_replies = capabilities.comment_likes = capabilities.comment_post = capabilities.comment_delete = true;
            capabilities.radio = capabilities.radio_dislike = true;
            features.daily.songs = []() -> Task<Result<std::vector<Track>>> {
                co_return std::vector<Track>{song(0), song(1), song(2)};
            };
            features.radar = []() -> Task<Result<std::vector<Track>>> {
                co_return std::vector<Track>{song(2), song(3), song(4)};
            };
            features.recent_plays = []() -> Task<Result<std::vector<Track>>> {
                co_return std::vector<Track>{song(4), song(1), song(0)};
            };
        }

        static Track song(int n) {
            static constexpr std::array<std::string_view, 5> titles{"Night Train", "夜に駆ける", "Paper Planes Over the Harbour at Dawn", "回声", "Slow Radio"};
            static constexpr std::array<std::string_view, 5> artists{"Mira Chen", "YOASOBI", "The Long Afternoon Ensemble", "陈粒", "Low Tide"};
            const auto k = static_cast<size_t>(n) % titles.size();
            return {.id = std::format("song-{}", n),
                    .title = std::string(titles[k]),
                    .artist = std::string(artists[k]),
                    .album = "Tapes",
                    .duration = 200 + 10 * static_cast<uint32_t>(n)};
        }
        static Person person(int n) {
            static constexpr std::array<std::string_view, 6> names{"Ada Lovelace", "林夕", "Grace", "A Listener With A Remarkably Long Display Name", "小野", "Tom"};
            static constexpr std::array<std::string_view, 6> words{
                "Counting in the dark.", "词在心里，歌在路上。", "", "Collects cassettes, bootlegs and train timetables from everywhere.", "", "Loud."};
            const auto k = static_cast<size_t>(n) % names.size();
            return {.id = n == 0 ? std::string(friend_id) : std::format("user-{}", n),
                    .name = std::string(names[k]),
                    .signature = std::string(words[k]),
                    .fans = n == 2 ? -1 : 120 * (n + 1) * (n + 1),
                    .followed = n % 2 == 1,
                    .follows_me = n % 3 == 0};
        }
        static Post post(std::string id, const Person &author, std::string text, bool owned) {
            return {
                .id = id, .thread = "thread-" + id, .author = author, .text = std::move(text), .time = 1731600000000, .likes = 12, .comments = 3, .forwards = 1, .owned = owned};
        }
        static Person me() {
            return {.id = std::string(self), .name = "Mira Chen", .signature = "Tapes, trains and late radio."};
        }
        // A listener's timeline: words with a topic and a mention, a song shared, another post passed on, and one
        // with nothing counted on it yet.
        static std::vector<Post> timeline(const Person &author, bool owned) {
            std::vector<Post> out;
            out.push_back(post("p1", author,
                               "Rain on the window and #NightTrain# on repeat. @Ada would call this weather for counting sheep, slowly, one at a time, until the tape runs out.",
                               owned));
            out.push_back(post("p2", author, "今晚的歌单：城市、雨和慢下来的鼓点。", owned));
            out.back().track = song(1);
            out.back().liked = true;
            out.push_back(post("p3", author, "This.", owned));
            out.back().forwarded.push_back(post("p0", person(1), "把这首歌循环了一整天。", false));
            out.back().forwarded.back().track = song(3);
            out.push_back(post("p4", author, "First light.", owned));
            out.back().likes = out.back().comments = out.back().forwards = 0;
            return out;
        }

        StringMap<std::string> &session() override {
            return cookies_;
        }
        void on_session_changed(std::move_only_function<void()>) override {}
        Task<Result<void>> initialize() override {
            co_return Result<void>{};
        }
        bool logged_in() const override {
            return signed_in;
        }
        Task<Result<void>> import_session(std::string json) override {
            if (on_import_session) {
                co_return co_await on_import_session(std::move(json));
            }
            co_return co_await Provider::import_session(std::move(json));
        }
        Task<SessionCheck> check_session(std::string json) override {
            if (on_check_session) {
                co_return co_await on_check_session(std::move(json));
            }
            co_return co_await Provider::check_session(std::move(json));
        }
        Task<Result<Account>> account() override {
            if (!signed_in) {
                co_return std::unexpected("Not signed in");
            }
            co_return Account{.id = std::string(self), .name = "Mira Chen"};
        }
        Task<void> logout() override {
            signed_in = false;
            co_return;
        }
        Task<Result<Batch>> search(std::string q, SearchKind kind, int offset, int limit) override {
            if (on_search) {
                co_return co_await on_search(std::move(q), kind, offset, limit);
            }
            if (kind == SearchKind::tracks) {
                Track t = song(0);
                t.id = "search-" + q;
                t.title = "Result for " + q;
                co_return Batch{.tracks = {std::move(t)}, .total = 1};
            }
            co_return Batch{};
        }
        Task<Result<std::vector<std::string>>> search_suggest(std::string q) override {
            if (on_search_suggest) {
                co_return co_await on_search_suggest(std::move(q));
            }
            co_return std::vector<std::string>{};
        }
        Task<Result<std::vector<Track>>> tracks(Card card) override {
            if (on_tracks) {
                co_return co_await on_tracks(std::move(card));
            }
            co_return std::vector<Track>{song(0), song(1), song(2)};
        }
        Task<Result<AlbumInfo>> album(std::string id) override {
            if (on_album) {
                co_return co_await on_album(std::move(id));
            }
            co_return AlbumInfo{.artist = "Mira Chen", .artist_id = "art-1", .date = "2026-01-01", .tracks = {song(0), song(1)}};
        }
        Task<Result<ArtistInfo>> artist(std::string id) override {
            if (on_artist) {
                co_return co_await on_artist(std::move(id));
            }
            co_return ArtistInfo{.name = "Artist " + id, .songs = 20, .albums = 4};
        }
        Task<Result<Batch>> artist_songs(std::string id, int offset) override {
            if (on_artist_songs) {
                co_return co_await on_artist_songs(std::move(id), offset);
            }
            co_return Batch{.tracks = {song(0), song(1)}, .total = 2};
        }
        Task<Result<std::vector<Card>>> recommended_playlists() override {
            std::vector<Card> out;
            for (int i = 0; i < 6; ++i) {
                out.push_back({.id = std::format("rec-{}", i), .title = std::format("Late Night Tape {}", i + 1), .count = 18 + i * 3, .count_kind = Card::Count::songs});
            }
            co_return out;
        }
        Task<Result<std::vector<Track>>> fm() override {
            if (on_fm) {
                co_return co_await on_fm();
            }
            co_return std::vector<Track>{song(0), song(1), song(2)};
        }
        Task<Result<std::vector<LyricLine>>> lyrics(Track track) override {
            if (on_lyrics) {
                co_return co_await on_lyrics(std::move(track));
            }
            co_return std::vector<LyricLine>{};
        }
        Task<Result<Stream>> stream(Track track, Quality quality) override {
            if (on_stream) {
                co_return co_await on_stream(std::move(track), quality);
            }
            co_return std::unexpected("Unavailable");
        }
        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) override {
            std::vector<Card> lists;
            for (int i = 0; i < 4; ++i) {
                lists.push_back({.id = std::format("{}-list-{}", uid, i),
                                 .title = i == 2 ? "A Playlist Whose Name Runs On Past Its Cover" : std::format("Mixtape {}", i + 1),
                                 .count = 12 + i,
                                 .count_kind = Card::Count::songs,
                                 .owned = i < 2});
            }
            co_return std::pair{Card{.id = uid + "-liked", .title = "Liked songs", .count = 40, .count_kind = Card::Count::songs, .owned = true}, std::move(lists)};
        }
        Task<Result<std::vector<std::string>>> liked_ids(std::string) override {
            co_return std::vector<std::string>{"song-1"};
        }
        Task<Result<std::vector<Card>>> collected_albums() override {
            if (on_collected_albums) {
                co_return co_await on_collected_albums();
            }
            co_return std::vector<Card>{
                {.kind = CardKind::album, .id = "alb-1", .title = "Harbour Tapes", .subtitle = "Mira Chen", .count = 10, .count_kind = Card::Count::songs},
                {.kind = CardKind::album, .id = "alb-2", .title = "Midnight Lines", .subtitle = "Low Tide", .count = 8, .count_kind = Card::Count::songs},
            };
        }
        Task<Result<std::vector<Card>>> followed_artists() override {
            co_return std::vector<Card>{
                {.kind = CardKind::artist, .id = "art-1", .title = "YOASOBI", .count = 14, .count_kind = Card::Count::albums},
                {.kind = CardKind::artist, .id = "art-2", .title = "Low Tide", .count = 4, .count_kind = Card::Count::albums},
            };
        }
        Task<Result<void>> like(Track track, bool on) override {
            calls.push_back(std::format("like {} {}", track.id, on));
            co_return Result<void>{};
        }

        Task<Result<Profile>> profile(std::string uid) override {
            if (on_profile) {
                co_return co_await on_profile(std::move(uid));
            }
            if (uid == self) {
                co_return Profile{.id = uid,
                                  .name = "Mira Chen",
                                  .signature = "Tapes, trains and late radio.",
                                  .location = "Hangzhou",
                                  .level = 8,
                                  .follows = 128,
                                  .fans = 2304,
                                  .posts = 37,
                                  .listened = 15872,
                                  .days = 2100,
                                  .vip = true};
            }
            co_return Profile{.id = uid,
                              .name = "Ada Lovelace",
                              .signature = "Counting in the dark.\nSecond line of what she says of herself.",
                              .location = "London",
                              .level = 6,
                              .follows = 56,
                              .fans = 1234567,
                              .posts = 4,
                              .listened = 980,
                              .days = 400,
                              .follows_me = true};
        }
        Task<Result<PeoplePage>> people(std::string, bool fans, std::string) override {
            PeoplePage page{.total = fans ? 2304 : 128};
            for (int i = 0; i < 6; ++i) {
                page.people.push_back(person(fans ? 5 - i : i));
            }
            co_return page;
        }
        Task<Result<void>> follow_user(std::string uid, bool on) override {
            calls.push_back(std::format("follow {} {}", uid, on));
            co_return Result<void>{};
        }
        Task<Result<PostPage>> posts(std::string uid, std::string cursor) override {
            if (on_posts) {
                co_return co_await on_posts(std::move(uid), std::move(cursor));
            }
            co_return PostPage{.posts = timeline(uid == self ? me() : person(0), uid == self)};
        }
        Task<Result<PostPage>> feed(std::string) override {
            PostPage page;
            page.posts.push_back(post("f1", person(0), "Found the B-side at last.", false));
            page.posts.back().track = song(4);
            page.posts.push_back(post("f2", person(3), "Nothing to add.", false));
            co_return page;
        }
        Task<Result<void>> like_post(Post post, bool on) override {
            calls.push_back(std::format("like-post {} {}", post.id, on));
            co_return Result<void>{};
        }
        Task<Result<void>> delete_post(std::string id) override {
            calls.push_back("delete-post " + id);
            co_return Result<void>{};
        }
        Task<Result<void>> share(std::string text, std::optional<Track> track) override {
            calls.push_back(std::format("share {} {}", text, track ? track->id : "-"));
            co_return Result<void>{};
        }
        Task<Result<void>> forward(Post post, std::string text) override {
            calls.push_back(std::format("forward {} {}", post.id, text));
            co_return Result<void>{};
        }
        Task<Result<Ranking>> ranking(std::string, bool all_time) override {
            Ranking ranking;
            for (int i = 0; i < 5; ++i) {
                ranking.tracks.push_back(song(i));
                ranking.plays.push_back((all_time ? 400 : 40) - 7 * i);
            }
            co_return ranking;
        }
        Task<Result<CommentPage>> comments(Subject subject, std::string id, bool newest, int page_no, std::string cursor) override {
            if (on_comments) {
                co_return co_await on_comments(subject, std::move(id), newest, page_no, std::move(cursor));
            }
            CommentPage page{.total = 3};
            page.comments.push_back({.id = "c1",
                                     .user_id = std::string(friend_id),
                                     .user = "Ada Lovelace",
                                     .text = "The bridge at two minutes in is the whole song.",
                                     .time = "2 hours ago",
                                     .likes = 1024,
                                     .replies = 2});
            page.comments.push_back({.id = "c2", .user = "林夕", .text = "听了十年。", .time = "yesterday", .likes = 8});
            page.comments.push_back({.id = "c3", .user = "Tom", .text = "Loud.", .time = "2024", .reply_user = "林夕", .reply = "听了十年。", .owned = true});
            co_return page;
        }
        Task<Result<CommentPage>> comment_replies(Subject, std::string, std::string, int, std::string) override {
            CommentPage page{.total = 2};
            page.comments.push_back({.id = "r1", .user = "林夕", .text = "Same here.", .time = "1 hour ago", .likes = 5});
            page.comments.push_back({.id = "r2", .user = "Tom", .text = "Best part of the track.", .time = "30 minutes ago", .likes = 2});
            co_return page;
        }
        Task<Result<void>> like_comment(Subject, std::string id, std::string comment, bool on) override {
            calls.push_back(std::format("like-comment {} {} {}", id, comment, on));
            co_return Result<void>{};
        }
        Task<Result<std::string>> post_comment(Subject, std::string id, std::string content, std::string reply, std::string) override {
            calls.push_back(std::format("post-comment {} {} {}", id, reply.empty() ? "-" : reply, content));
            co_return std::string("c-new");
        }
        Task<Result<void>> delete_comment(Subject, std::string id, std::string comment) override {
            calls.push_back(std::format("delete-comment {} {}", id, comment));
            co_return Result<void>{};
        }

    private:
        StringMap<std::string> cookies_;
    };
} // namespace lyren::test
