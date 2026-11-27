#include "media/provider.hpp"

#include <algorithm>
#include <glaze/file/file_ops.hpp>
#include <stdexcept>
#include "core/encoding.hpp"

namespace lyren {

    StringMap<std::string> &Provider::session() {
        return empty_session_;
    }
    void Provider::on_session_changed(std::move_only_function<void()>) {}
    Task<Result<void>> Provider::initialize() {
        co_return Result<void>{};
    }
    Task<Result<std::vector<std::string>>> Provider::search_suggest(std::string) {
        co_return std::vector<std::string>{};
    }
    bool Provider::logged_in() const {
        return !capabilities.accounts;
    }

    Task<Result<std::vector<LyricLine>>> Provider::playback_lyrics(Track track, std::filesystem::path lyric_dir) {
        {
            const auto path = lyric_dir / source_id(track.source, track.instance) / (encoding::url_encode(track.id) + ".lrc");
            std::string text;
            if (glz::file_to_buffer(text, path.native()) == glz::error_code::none) {
                auto lines = parse_timed_lrc(text);
                if (!lines.empty()) {
                    co_return lines;
                }
            }
        }
        co_return co_await lyrics(std::move(track));
    }

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
    Task<Result<Track>> Provider::track_detail(std::string) {
        return unavailable<Track>();
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
        return edit_playlist(std::move(id), std::move(tracks), add);
    }

    Task<Result<void>> Provider::edit_playlist(std::string, std::vector<Track>, bool) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::reorder_playlist(std::string, std::vector<Track>) {
        return unavailable<void>();
    }
    Task<Result<Profile>> Provider::profile(std::string) {
        return unavailable<Profile>();
    }
    Task<Result<PeoplePage>> Provider::people(std::string, bool, std::string) {
        return unavailable<PeoplePage>();
    }
    Task<Result<void>> Provider::follow_user(std::string, bool) {
        return unavailable<void>();
    }
    Task<Result<PostPage>> Provider::posts(std::string, std::string) {
        return unavailable<PostPage>();
    }
    Task<Result<PostPage>> Provider::feed(std::string) {
        return unavailable<PostPage>();
    }
    Task<Result<void>> Provider::like_post(Post, bool) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::delete_post(std::string) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::share(std::string, std::optional<Track>) {
        return unavailable<void>();
    }
    Task<Result<void>> Provider::forward(Post, std::string) {
        return unavailable<void>();
    }
    Task<Result<Ranking>> Provider::ranking(std::string, bool) {
        return unavailable<Ranking>();
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
    Task<SessionCheck> Provider::check_session(std::string) {
        co_return SessionCheck{.signer = {SessionCheck::State::failed, "This feature is not available on this platform"}};
    }
    Task<Result<Account>> Provider::account() {
        return unavailable<Account>();
    }
    Task<void> Provider::logout() {
        co_return;
    }

    ProviderRegistry::ProviderRegistry(std::initializer_list<std::reference_wrapper<Provider>> builtins) {
        entries_.reserve(builtins.size());
        size_t i = 0;
        for (Provider &p : builtins) {
            if (i >= platforms.size()) {
                break;
            }
            const auto &meta = platforms[i++];
            entries_.push_back({
                .source = meta.source,
                .instance = {},
                .id = std::string(meta.id),
                .name = std::string(meta.name),
                .short_name = std::string(meta.short_name),
                .scan_hint = std::string(meta.scan_hint),
                .provider = &p,
            });
        }
    }

    std::string ProviderRegistry::add(SourceEntry entry) {
        if (entry.owned && !entry.provider) {
            entry.provider = entry.owned.get();
        }
        entry.id = source_id(entry.source, entry.instance);
        const auto &meta = platform(entry.source);
        if (entry.name.empty()) {
            entry.name = entry.instance.empty() ? std::string(meta.name) : entry.instance;
        }
        if (entry.short_name.empty()) {
            entry.short_name = entry.instance.empty() ? std::string(meta.short_name) : entry.name;
        }
        if (entry.scan_hint.empty()) {
            entry.scan_hint = std::string(meta.scan_hint);
        }
        std::string id = entry.id;
        auto it = std::ranges::find_if(entries_, [&](const SourceEntry &e) {
            return e.id == id;
        });
        if (it != entries_.end()) {
            *it = std::move(entry);
        } else {
            entries_.push_back(std::move(entry));
        }
        return id;
    }

    bool ProviderRegistry::remove(std::string_view id) {
        return std::erase_if(entries_, [&](const SourceEntry &e) {
                   return e.id == id;
               }) > 0;
    }

    const SourceEntry *ProviderRegistry::entry(std::string_view id) const {
        auto it = std::ranges::find_if(entries_, [&](const SourceEntry &e) {
            return e.id == id;
        });
        return it == entries_.end() ? nullptr : &*it;
    }

    const SourceEntry *ProviderRegistry::entry(Source source, std::string_view instance) const {
        if (instance.empty()) {
            auto it = std::ranges::find_if(entries_, [&](const SourceEntry &e) {
                return e.source == source && e.instance.empty();
            });
            return it == entries_.end() ? nullptr : &*it;
        }
        return entry(source_id(source, instance));
    }

    Provider *ProviderRegistry::find(std::string_view id) const {
        const auto *e = entry(id);
        return e ? e->provider : nullptr;
    }

    Provider *ProviderRegistry::find(Source source, std::string_view instance) const {
        const auto *e = entry(source, instance);
        return e ? e->provider : nullptr;
    }

    Provider &ProviderRegistry::get(Source source, std::string_view instance) const {
        if (Provider *p = find(source, instance)) {
            return *p;
        }
        if (!entries_.empty() && entries_.front().provider) {
            return *entries_.front().provider;
        }
        throw std::out_of_range("No provider registered for source");
    }

    Provider &ProviderRegistry::get(std::string_view id) const {
        if (Provider *p = find(id)) {
            return *p;
        }
        if (!entries_.empty() && entries_.front().provider) {
            return *entries_.front().provider;
        }
        throw std::out_of_range("No provider registered for source");
    }

} // namespace lyren
