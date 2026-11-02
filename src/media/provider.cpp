#include "media/provider.hpp"

namespace lyren {

    template <class T>
    static Task<Result<T>> unavailable() {
        co_return std::unexpected("This feature is not available on this platform");
    }

    Task<Result<std::vector<Card>>> Provider::recommended_playlists() {
        return unavailable<std::vector<Card>>();
    }
    Task<Result<std::vector<Card>>> Provider::toplists() {
        return unavailable<std::vector<Card>>();
    }
    Task<Result<std::vector<Track>>> Provider::fm() {
        return unavailable<std::vector<Track>>();
    }
    Task<Result<void>> Provider::fm_trash(Track) {
        return unavailable<void>();
    }
    Task<Result<std::vector<Track>>> Provider::new_songs(std::string) {
        return unavailable<std::vector<Track>>();
    }
    Task<Result<Batch>> Provider::new_albums(std::string, int) {
        return unavailable<Batch>();
    }
    Task<Result<Categories>> Provider::categories() {
        return unavailable<Categories>();
    }
    Task<Result<Batch>> Provider::square(std::string, int) {
        return unavailable<Batch>();
    }
    Task<Result<std::vector<Track>>> Provider::similar(std::string) {
        return unavailable<std::vector<Track>>();
    }
    Task<Result<AlbumInfo>> Provider::album(std::string) {
        return unavailable<AlbumInfo>();
    }
    Task<Result<ArtistInfo>> Provider::artist(std::string) {
        return unavailable<ArtistInfo>();
    }
    Task<Result<Batch>> Provider::artist_songs(std::string, int) {
        return unavailable<Batch>();
    }
    Task<Result<Batch>> Provider::artist_albums(std::string, int) {
        return unavailable<Batch>();
    }
    Task<Result<std::vector<std::pair<std::string, std::string>>>> Provider::artist_intro(std::string) {
        return unavailable<std::vector<std::pair<std::string, std::string>>>();
    }
    Task<Result<CommentPage>> Provider::comments(Subject, std::string, bool, int, std::string) {
        return unavailable<CommentPage>();
    }
    Task<Result<CommentPage>> Provider::comment_replies(Subject, std::string, std::string, int, std::string) {
        return unavailable<CommentPage>();
    }
    Task<Result<void>> Provider::like_comment(Subject, std::string, std::string, bool) {
        return unavailable<void>();
    }
    Task<Result<std::string>> Provider::post_comment(Subject, std::string, std::string, std::string, std::string) {
        return unavailable<std::string>();
    }
    Task<Result<void>> Provider::delete_comment(Subject, std::string, std::string) {
        return unavailable<void>();
    }
    Task<Result<std::pair<Card, std::vector<Card>>>> Provider::user_playlists(std::string) {
        return unavailable<std::pair<Card, std::vector<Card>>>();
    }
    Task<Result<std::vector<std::string>>> Provider::liked_ids(std::string) {
        return unavailable<std::vector<std::string>>();
    }
    Task<Result<void>> Provider::like(Track, bool) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::follow(std::string, bool) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::collect(std::string, bool) {
        return unavailable<void>();
    }
    Task<Result<std::vector<Card>>> Provider::collected_playlists() {
        return unavailable<std::vector<Card>>();
    }
    Task<Result<void>> Provider::collect_playlist(std::string, bool) {
        return unavailable<void>();
    }
    Task<Result<std::vector<Card>>> Provider::collected_albums() {
        return unavailable<std::vector<Card>>();
    }
    Task<Result<std::vector<Card>>> Provider::followed_artists() {
        return unavailable<std::vector<Card>>();
    }
    Task<Result<Card>> Provider::create_playlist(std::string, bool) {
        return unavailable<Card>();
    }
    Task<Result<void>> Provider::rename_playlist(std::string, std::string) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::delete_playlist(std::string) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::edit_playlist(std::string id, Track track, bool add) {
        std::vector<Track> tracks;
        tracks.push_back(std::move(track));
        co_return co_await edit_playlist(std::move(id), std::move(tracks), add);
    }

    Task<Result<void>> Provider::edit_playlist(std::string, std::vector<Track>, bool) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::reorder_playlist(std::string, std::vector<Track>) {
        return unavailable<void>();
    }
    Task<Result<QrCode>> Provider::qr_start() {
        return unavailable<QrCode>();
    }
    Task<Result<QrStatus>> Provider::qr_poll() {
        return unavailable<QrStatus>();
    }
    Task<Result<void>> Provider::import_session(std::string) {
        return unavailable<void>();
    }

} // namespace lyren
