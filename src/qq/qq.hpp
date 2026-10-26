#pragma once
#include "app/provider.hpp"
#include "qq/client.hpp"

namespace lyren::qq {

// QQ Music, after docs/qqmusic-api.md. Albums and artists go by their mids, playlists by their public ids, songs by
// their numeric ids with the mids alongside (Track::mid, Track::media).
class Qq : public Provider {
public:
    Qq(Loop& loop, net::Http& http);

    Client client;

    Task<Result<Batch>> search(std::string query, SearchKind kind, int offset, int limit) override;
    Task<Result<Track>> song_detail(std::string mid);
    Task<Result<Stream>> stream(Track track, Quality quality) override;
    Task<Result<std::vector<LyricLine>>> lyrics(Track track) override;
    Task<Result<std::vector<Track>>> tracks(Card card) override;

    Task<Result<std::vector<Card>>> recommended_playlists() override;
    Task<Result<std::vector<Card>>> toplists() override;
    Task<Result<AlbumInfo>> album(std::string mid) override;
    Task<Result<ArtistInfo>> artist(std::string mid) override;
    Task<Result<Batch>> artist_songs(std::string mid, int offset) override;
    Task<Result<Batch>> artist_albums(std::string mid, int offset) override;
    Task<Result<std::vector<std::pair<std::string, std::string>>>> artist_intro(std::string mid) override;
    Task<Result<CommentPage>> comments(Subject subject, std::string id, bool hot, int page, std::string cursor) override;

    Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid) override;
    Task<Result<std::vector<std::string>>> liked_ids(std::string uid) override;
    Task<Result<void>> like(Track track, bool on) override;
    Task<Result<void>> follow(std::string artist, bool on) override;
    Task<Result<void>> collect(std::string album, bool on) override;
    Task<Result<std::vector<Card>>> collected_albums() override;
    Task<Result<std::vector<Card>>> followed_artists() override;
    Task<Result<Card>> create_playlist(std::string name, bool hidden) override;
    Task<Result<void>> rename_playlist(std::string id, std::string name) override;
    Task<Result<void>> delete_playlist(std::string id) override;
    Task<Result<void>> edit_playlist(std::string id, Track track, bool add) override;

    bool logged_in() const override { return client.logged_in(); }
    Task<Result<QrCode>> qr_start() override;
    Task<Result<QrStatus>> qr_poll() override;
    Task<Result<Account>> account() override;
    Task<void> logout() override;
    Task<Result<void>> renew();
    void cancel_qr() { ++qr_gen_; }

private:
    Loop& loop_;
    net::Http& http_;
    std::string guid_;  // names this client in stream requests
    uint64_t qr_gen_ = 0;
    std::map<std::string, std::string> qr_cookies_;
    std::string js_version_, oauth_state_;
};

}  // namespace lyren::qq
