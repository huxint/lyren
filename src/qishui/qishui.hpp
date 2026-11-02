#pragma once
#include <unordered_set>

#include "media/provider.hpp"
#include "qishui/client.hpp"

namespace lyren::qishui {

    struct SearchPage {
        Batch batch;
        std::string cursor;
    };

    Result<SearchPage> parse_search(const Json &response, SearchKind kind);
    Result<CommentPage> parse_comments(const Json &response, std::string_view user_id, std::string_view root = {});

    class Qishui : public Provider {
    public:
        Qishui(Loop &loop, net::Http &http);

        Client client;
        StringMap<std::string> &session() override {
            return client.session;
        }
        void on_session_changed(std::move_only_function<void()> changed) override {
            client.on_session = std::move(changed);
        }
        Task<Result<void>> initialize() override;
        Task<Result<void>> import_session(std::string json) override;
        void cancel_login() override {
            ++client.generation;
        }
        bool logged_in() const override {
            return client.logged_in();
        }
        Task<Result<Account>> account() override;
        Task<void> logout() override;

        Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) override;
        Task<Result<Stream>> stream(Track track, Quality quality) override;
        Task<Result<std::vector<LyricLine>>> lyrics(Track track) override;
        Task<Result<std::vector<Track>>> tracks(Card card) override;
        Task<Result<std::vector<Card>>> recommended_playlists() override;
        Task<Result<std::vector<Card>>> toplists() override;
        Task<Result<std::vector<Track>>> fm() override;
        Task<Result<void>> fm_trash(Track track) override;
        Task<Result<std::vector<Track>>> new_songs(std::string area) override;
        Task<Result<Categories>> categories() override;
        Task<Result<Batch>> square(std::string category, int offset) override;
        Task<Result<std::vector<Track>>> similar(std::string id) override;
        Task<Result<AlbumInfo>> album(std::string id) override;
        Task<Result<ArtistInfo>> artist(std::string id) override;
        Task<Result<Batch>> artist_songs(std::string id, int offset) override;
        Task<Result<Batch>> artist_albums(std::string id, int offset) override;
        Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor) override;
        Task<Result<CommentPage>> comment_replies(Subject subject, std::string id, std::string root, int page, std::string cursor) override;
        Task<Result<void>> like_comment(Subject subject, std::string resource, std::string id, bool on) override;
        Task<Result<std::string>> post_comment(Subject subject, std::string id, std::string content, std::string reply = {}, std::string root = {}) override;
        Task<Result<void>> delete_comment(Subject subject, std::string resource, std::string id) override;

        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) override;
        Task<Result<std::vector<std::string>>> liked_ids(std::string uid) override;
        Task<Result<void>> like(Track track, bool on) override;
        Task<Result<void>> follow(std::string artist, bool on) override;
        Task<Result<void>> collect(std::string album, bool on) override;
        Task<Result<void>> collect_playlist(std::string id, bool on) override;
        Task<Result<std::vector<Card>>> collected_playlists() override;
        Task<Result<std::vector<Card>>> collected_albums() override;
        Task<Result<std::vector<Card>>> followed_artists() override;
        Task<Result<Card>> create_playlist(std::string name, bool hidden) override;
        Task<Result<void>> rename_playlist(std::string id, std::string name) override;
        Task<Result<void>> delete_playlist(std::string id) override;
        using Provider::edit_playlist;
        Task<Result<void>> edit_playlist(std::string id, std::vector<Track> tracks, bool add) override;
        Task<Result<void>> reorder_playlist(std::string id, std::vector<Track> tracks) override;
        Task<Result<std::vector<Track>>> recent_plays();

    private:
        struct Pagination {
            std::string identity, search_id;
            std::vector<std::string> cursors{std::string{}};
            uint64_t revision = 0;
        };
        struct Page {
            std::string name, cursor, search_id;
            uint64_t revision;
            int index;
        };
        Result<Page> begin_page(std::string name, std::string identity, int offset);
        Result<void> finish_page(const Page &page, std::string next, Batch &batch);
        Task<Result<Json>> track_data(std::string id);
        Task<Result<void>> write(std::string path, Json body);
        Task<Result<void>> collection(std::string kind, std::string id, bool on);
        Task<Result<std::vector<Card>>> collections(CardKind kind);
        Task<Result<Batch>> artist_page(std::string id, int offset, bool albums);
        Task<void> report_start(std::string id, std::string source);
        Task<void> report_play(std::string id, std::string source, int seconds);
        Loop &loop_;
        net::Http &http_;
        StringMap<Pagination> pagination_;
        std::string square_category_;
        std::unordered_set<std::string> square_seen_;
        uint64_t square_revision_ = 0;
        uint64_t page_revision_ = 0;
        uint64_t feed_count_ = 0;
        int64_t first_use_ = 0;
        Json::array_t played_media_;
        std::string lyric_id_, lyric_text_, lyric_type_, liked_playlist_id_;
    };

} // namespace lyren::qishui
