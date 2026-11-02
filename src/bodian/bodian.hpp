#pragma once
#include "bodian/client.hpp"
#include "media/provider.hpp"

namespace lyren::bodian {

    Result<Batch> parse_search(const Reply &reply, SearchKind kind, int offset, int limit);
    Result<std::vector<LyricLine>> parse_lyric(const Reply &reply);
    Result<Stream> parse_stream(const Reply &reply);
    Result<CommentPage> parse_comments(const Reply &reply, std::string_view uid);

    class Bodian : public Provider {
    public:
        Bodian(Loop &loop, net::Http &http, std::string base_url = "https://bd-api.kuwo.cn/api/");

        Client client;

        StringMap<std::string> &session() override {
            return client.session;
        }
        void on_session_changed(std::move_only_function<void()> changed) override {
            client.on_session = std::move(changed);
        }
        Task<Result<void>> initialize() override;
        bool logged_in() const override {
            return client.logged_in();
        }
        void cancel_login() override {
            ++qr_gen_;
            qr_key_.clear();
        }
        Task<Result<QrCode>> qr_start() override;
        Task<Result<QrStatus>> qr_poll() override;
        Task<Result<Account>> account() override;
        Task<void> logout() override;

        Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) override;
        Task<Result<Track>> song_detail(std::string id);
        Task<Result<Stream>> stream(Track track, Quality quality) override;
        Task<Result<std::vector<LyricLine>>> lyrics(Track track) override;
        Task<Result<std::vector<Track>>> tracks(Card card) override;
        Task<Result<std::vector<Card>>> recommended_playlists() override;
        Task<Result<std::vector<Card>>> toplists() override;
        Task<Result<Categories>> categories() override;
        Task<Result<Batch>> square(std::string category, int offset) override;
        Task<Result<AlbumInfo>> album(std::string id) override;
        Task<Result<ArtistInfo>> artist(std::string id) override;
        Task<Result<Batch>> artist_songs(std::string id, int offset) override;
        Task<Result<Batch>> artist_albums(std::string id, int offset) override;
        Task<Result<std::vector<std::pair<std::string, std::string>>>> artist_intro(std::string id) override;

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

        Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor) override;
        Task<Result<CommentPage>> comment_replies(Subject subject, std::string id, std::string root, int page, std::string cursor) override;
        Task<Result<void>> like_comment(Subject subject, std::string id, std::string comment, bool on) override;
        Task<Result<std::string>> post_comment(Subject subject, std::string id, std::string content, std::string reply = {}, std::string root = {}) override;

    private:
        Task<Result<Card>> liked_playlist(std::string uid);
        Task<Result<std::vector<Card>>> collections(CardKind kind);
        Task<Result<void>> set_collection(std::string id, int source, bool on);
        std::string qr_key_;
        uint64_t qr_gen_ = 0;
    };

} // namespace lyren::bodian
