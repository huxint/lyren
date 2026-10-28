#pragma once
#include "app/provider.hpp"
#include "ncm/client.hpp"

namespace lyren::ncm {

    // NetEase Cloud Music. Endpoints and their encodings follow api-enhanced's module/*.js.
    class Netease : public Provider {
    public:
        Netease(Loop &loop, net::Http &http);

        Client client;

        std::map<std::string, std::string> &session() override {
            return client.cookies;
        }
        void on_session_changed(std::move_only_function<void()> changed) override {
            client.on_cookies = std::move(changed);
        }
        Task<Result<void>> initialize() override;
        void cancel_qr() override {
            qr_key_.clear();
        }

        Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) override;
        Task<Result<Stream>> stream(Track track, Quality quality) override;
        Task<Result<std::vector<LyricLine>>> lyrics(Track track) override;
        Task<Result<std::vector<Track>>> tracks(Card card) override;

        bool logged_in() const override {
            return client.logged_in();
        }
        Task<Result<QrCode>> qr_start() override;
        Task<Result<QrStatus>> qr_poll() override;
        Task<Result<Account>> account() override;
        Task<void> logout() override;

        Task<Result<std::vector<Card>>> recommended_playlists() override;
        Task<Result<std::vector<Card>>> toplists() override;
        Task<Result<std::vector<Track>>> daily();
        Task<Result<std::vector<Card>>> daily_playlists();
        // Takes a song out of the daily recommendations; the server may name one to take its place.
        Task<Result<std::optional<Track>>> dislike_daily(std::string id);
        // Heartbeat mode: songs to follow seed, from the liked playlist and others like them.
        Task<Result<std::vector<Track>>> heartbeat(std::string seed, std::string playlist);
        Task<Result<std::vector<Track>>> similar(std::string id);
        // The account's recent plays on every device.
        Task<Result<std::vector<Track>>> recent_plays();
        Task<Result<std::vector<Track>>> fm();
        Task<Result<void>> fm_trash(std::string id);
        Task<Result<std::vector<Track>>> cloud();
        // The "liked songs" playlist, then the created and collected ones.
        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) override;
        Task<Result<std::vector<std::string>>> liked_ids(std::string uid) override;
        Task<Result<void>> like(Track track, bool on) override;
        Task<Result<std::vector<Track>>> song_detail(std::vector<std::string> ids);

        Task<Result<Categories>> categories();
        Task<Result<Batch>> square(std::string category, int offset);

        Task<Result<ArtistInfo>> artist(std::string id) override;
        Task<Result<Batch>> artist_songs(std::string id, int offset) override;
        Task<Result<Batch>> artist_albums(std::string id, int offset) override;
        Task<Result<std::vector<std::pair<std::string, std::string>>>> artist_intro(std::string id) override;
        Task<Result<AlbumInfo>> album(std::string id) override;
        Task<Result<void>> follow(std::string artist, bool on) override;
        Task<Result<void>> collect(std::string album, bool on) override;
        Task<Result<std::vector<Card>>> collected_albums() override;
        Task<Result<std::vector<Card>>> followed_artists() override;

        Task<Result<Card>> create_playlist(std::string name, bool hidden) override;
        Task<Result<void>> rename_playlist(std::string id, std::string name) override;
        Task<Result<void>> delete_playlist(std::string id) override;
        Task<Result<void>> edit_playlist(std::string id, Track track, bool add) override;

        Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor) override;

        // Listening reports: start puts the song in the account's recent plays, play counts it in the listening ranking.
        Task<void> report_start(std::string id, std::string source);
        Task<void> report_play(std::string id, std::string source, int seconds);

    private:
        std::string qr_key_;
    };

} // namespace lyren::ncm
