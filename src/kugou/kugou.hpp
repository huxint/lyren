#pragma once
#include "kugou/client.hpp"
#include "media/provider.hpp"

namespace lyren::kugou {

    Result<Batch> parse_search(std::string_view body, SearchKind kind, int offset, int limit);
    Result<Stream> parse_stream(std::string_view body, bool preview, std::string_view quality);
    Result<std::vector<LyricLine>> parse_lyric_download(std::string_view body);
    Result<std::vector<LyricLine>> parse_krc(std::string_view bytes);
    Result<std::vector<Track>> parse_metadata(std::string_view body, std::span<const std::string> hashes);

    // Tracks use the standard file hash as id, with audio_id and album_audio_id in mid and media.
    // Account listid, public specialid, and rankid live in separate card ID namespaces.
    class Kugou : public Provider {
    public:
        Kugou(Loop &loop, net::Http &http);
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
        Task<Result<Account>> account() override;
        Task<void> logout() override;
        void cancel_login() override;
        Task<Result<QrCode>> qr_start() override;
        Task<Result<QrStatus>> qr_poll() override;

        Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) override;
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
        Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor) override;
        Task<Result<CommentPage>> comment_replies(Subject subject, std::string id, std::string root, int page, std::string cursor) override;
        Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) override;
        Task<Result<std::vector<Card>>> collected_playlists() override;
        Task<Result<std::vector<Track>>> daily_songs();
        Task<Result<Track>> song_detail(std::string hash);

    private:
        Task<Result<std::vector<Track>>> metadata(std::vector<std::string> hashes);
        Task<Result<Batch>> song_page(std::string url, Params query, int offset, int limit, std::string album_id = {});
        Task<Result<std::vector<Card>>> account_playlists();
        std::string qr_key_;
        Clock::time_point qr_expires_{};
        uint64_t qr_gen_ = 0;
    };

} // namespace lyren::kugou
