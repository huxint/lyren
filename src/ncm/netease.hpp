#pragma once
#include "app/provider.hpp"
#include "ncm/client.hpp"

namespace lyren::ncm {

// NetEase Cloud Music. Endpoints and their encodings follow api-enhanced's module/*.js.
class Netease : public Provider {
public:
    Netease(Loop& loop, net::Http& http) : client(loop, http) {}

    Client client;

    Task<Result<SearchPage>> search(std::string query, SearchKind kind, int offset, int limit) override;
    Task<Result<Stream>> stream(Track track, Quality quality) override;
    Task<Result<Lyrics>> lyrics(Track track) override;
    Task<Result<std::vector<Track>>> tracks(Card card) override;
    std::string cover_url(const std::string& base, int px) const override;

    bool logged_in() const override { return client.logged_in(); }
    Task<Result<std::string>> qr_start() override;
    Task<Result<QrStatus>> qr_poll() override;
    Task<Result<Account>> account() override;
    Task<void> logout() override;

    Task<Result<std::vector<Card>>> recommended_playlists();
    Task<Result<std::vector<Card>>> toplists();
    Task<Result<std::vector<Track>>> daily();
    Task<Result<std::vector<Track>>> fm();
    Task<Result<void>> fm_trash(std::string id);
    Task<Result<std::vector<Track>>> cloud();
    // The "liked songs" playlist, then the created and collected ones.
    Task<Result<std::pair<Card, std::vector<Card>>>> user_playlists(std::string uid);
    Task<Result<std::vector<std::string>>> liked_ids(std::string uid);
    Task<Result<void>> like(std::string id, bool on);
    Task<Result<std::vector<Track>>> song_detail(std::vector<std::string> ids);

private:
    std::string qr_key_;
};

}  // namespace lyren::ncm
