#include "qishui/qishui.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <ctime>
#include <format>
#include <limits>
#include <ranges>
#include <unordered_set>

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "qishui/media.hpp"

namespace lyren::qishui {

    namespace {
        constexpr std::array kinds{"track", "album", "artist", "playlist"};
        constexpr std::array group_ids{"tracks", "albums", "artists", "playlists"};

        int count(const Json &value) {
            return static_cast<int>(std::clamp(number(value), 0.0, double(std::numeric_limits<int>::max())));
        }

        std::string image(const Json &value) {
            const auto urls = array(field(value, "urls"));
            if (urls.empty()) {
                return {};
            }
            std::string url = string(urls.front());
            const auto uri = string(field(value, "uri"));
            if (url.ends_with('/') && !uri.empty()) {
                url += uri;
                const auto prefix = string(field(value, "template_prefix"));
                if (!prefix.empty()) {
                    url += '~' + prefix + "-resize:{size}:{size}.png";
                }
            }
            return url;
        }

        std::string artists(const Json &value) {
            std::string names;
            for (const auto &artist : array(value)) {
                const auto name = string(field(artist, "name"));
                if (!names.empty() && !name.empty()) {
                    names += " / ";
                }
                names += name;
            }
            return names;
        }

        Result<Track> track(const Json &value) {
            Track result{.source = Source::qishui};
            result.id = string(field(value, "id"));
            result.title = string(field(value, "name"));
            if (result.id.empty() || result.title.empty()) {
                return std::unexpected("Qishui Music returned incomplete song details");
            }
            result.duration = static_cast<uint32_t>(std::clamp(number(field(value, "duration")) / 1000.0, 0.0, double(std::numeric_limits<uint32_t>::max())));
            const auto &album = field(value, "album");
            result.album = string(field(album, "name"));
            result.album_id = string(field(album, "id"));
            result.cover = image(field(album, "url_cover"));
            result.artist = artists(field(value, "artists"));
            const auto people = array(field(value, "artists"));
            if (!people.empty()) {
                result.artist_id = string(field(people.front(), "id"));
            }
            result.paid = boolean(field(field(value, "label_info"), "only_vip_playable"));
            return result;
        }

        const Json &track_entity(const Json &item) {
            const auto &entity = item.contains("entity") ? field(item, "entity") : item;
            if (entity.contains("track_wrapper")) {
                return field(field(entity, "track_wrapper"), "track");
            }
            return entity.contains("track") ? field(entity, "track") : entity;
        }

        Result<std::vector<Track>> track_list(std::span<const Json> items) {
            std::vector<Track> result;
            result.reserve(items.size());
            for (const auto &item : items) {
                const auto type = string(field(item, "type"));
                if (!type.empty() && type != "track") {
                    continue;
                }
                auto parsed = track(track_entity(item));
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                result.push_back(std::move(*parsed));
            }
            return result;
        }

        Result<Card> card(const Json &value, CardKind kind, bool owned = false) {
            Card result{.kind = kind, .source = Source::qishui};
            result.id = string(field(value, "id"));
            result.title = string(field(value, kind == CardKind::playlist ? "title" : "name"));
            if (result.id.empty() || result.title.empty()) {
                return std::unexpected("Qishui Music returned incomplete resource details");
            }
            result.cover = image(field(value, kind == CardKind::artist ? "url_avatar" : "url_cover"));
            result.count = count(field(value, "count_tracks"));
            if (kind == CardKind::playlist) {
                const auto &total = field(field(value, "resource_cnt"), "track_cnt");
                if (!total.is_null()) {
                    result.count = count(total);
                }
            }
            result.count_kind = result.count ? Card::Count::songs : Card::Count::none;
            result.owned = owned;
            result.commentable = kind == CardKind::playlist && !boolean(field(value, "is_private"));
            if (kind == CardKind::playlist) {
                result.subtitle = string(field(field(value, "owner"), "nickname"));
                result.collectable = !owned && result.commentable && number(field(value, "type")) != 1;
            } else if (kind == CardKind::album) {
                result.subtitle = artists(field(value, "artists"));
            }
            return result;
        }

        Result<std::vector<Card>> card_list(std::span<const Json> items, CardKind kind) {
            std::vector<Card> result;
            result.reserve(items.size());
            for (const auto &item : items) {
                auto parsed = card(item, kind);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                result.push_back(std::move(*parsed));
            }
            return result;
        }

        Result<Account> account_info(const Json &response) {
            const auto &info = field(response, "my_info");
            Account result{.id = string(field(info, "id")), .name = string(field(info, "nickname")), .avatar = image(field(info, "larger_avatar_url"))};
            if (result.name.empty()) {
                result.name = string(field(info, "public_name"));
            }
            if (result.id.empty()) {
                return std::unexpected("Sign in to Qishui Music and try again");
            }
            return result;
        }

        Json media(std::string id) {
            return Json::array_t{Json{{"id", std::move(id)}, {"type", "track"}}};
        }

        std::pair<std::string, std::string> comment_id(std::string_view id) {
            const auto split = id.find(':');
            if (split == id.npos) {
                return {std::string(id), {}};
            }
            return {std::string(id.substr(0, split)), std::string(id.substr(split + 1))};
        }

        Result<void> collect_discovery(const Json &block, std::vector<Card> &out) {
            if (const auto &playlist = field(field(block, "entity"), "playlist"); !playlist.is_null()) {
                auto parsed = card(playlist, CardKind::playlist);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.push_back(std::move(*parsed));
            }
            for (auto name : {"resources", "inner_block"}) {
                for (const auto &resource : array(field(block, name))) {
                    auto parsed = collect_discovery(resource, out);
                    if (!parsed) {
                        return parsed;
                    }
                }
            }
            return {};
        }
    } // namespace

    Result<SearchPage> parse_search(const Json &response, SearchKind kind) {
        SearchPage result;
        const auto &groups = field(response, "result_groups");
        if (!groups.is_array()) {
            return std::unexpected("Qishui Music returned no search results");
        }
        const auto index = static_cast<size_t>(kind);
        for (const auto &group : groups.get_array()) {
            if (text(field(group, "id")) != group_ids[index]) {
                continue;
            }
            const auto size = array(field(group, "data")).size();
            if (kind == SearchKind::tracks) {
                result.batch.tracks.reserve(size);
            } else {
                result.batch.cards.reserve(size);
            }
            for (const auto &item : array(field(group, "data"))) {
                const auto &entity = field(item, "entity");
                if (kind == SearchKind::tracks) {
                    auto parsed = track(track_entity(entity));
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    result.batch.tracks.push_back(std::move(*parsed));
                } else {
                    const auto card_kind = kind == SearchKind::albums ? CardKind::album : kind == SearchKind::artists ? CardKind::artist : CardKind::playlist;
                    auto parsed = card(field(entity, kinds[index]), card_kind);
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    result.batch.cards.push_back(std::move(*parsed));
                }
            }
            if (boolean(field(group, "has_more"))) {
                result.cursor = string(field(group, "next_cursor"));
                if (result.cursor.empty()) {
                    return std::unexpected("Qishui Music returned no search continuation");
                }
            }
            break;
        }
        result.batch.total = static_cast<int>(result.batch.cards.size() + result.batch.tracks.size()) + !result.cursor.empty();
        return result;
    }

    Result<CommentPage> parse_comments(const Json &response, std::string_view user_id, std::string_view root) {
        CommentPage result;
        result.total = count(field(response, "count"));
        result.more = boolean(field(response, "has_more"));
        result.cursor = string(field(response, "cursor"));
        result.comments.reserve(array(field(response, root.empty() ? "comments" : "reply_infos")).size());
        if (result.more && result.cursor.empty()) {
            return std::unexpected("Qishui Music returned no comment continuation");
        }
        for (const auto &item : array(field(response, root.empty() ? "comments" : "reply_infos"))) {
            Comment comment;
            comment.id = string(field(item, "id"));
            if (comment.id.empty()) {
                return std::unexpected("Qishui Music returned incomplete comment details");
            }
            if (!root.empty()) {
                comment.id = std::string(root) + ':' + comment.id;
            }
            const auto &user = field(item, "user");
            comment.user = string(field(user, "nickname"));
            comment.avatar = image(field(user, "medium_avatar_url"));
            comment.text = string(field(item, "content"));
            comment.likes = count(field(item, "count_digged"));
            comment.replies = count(field(item, "count_reply"));
            comment.liked = boolean(field(item, "user_digged"));
            comment.owned = boolean(field(item, "can_delete")) || (!user_id.empty() && string(field(user, "id")) == user_id);
            comment.reply = string(field(field(item, "reply_to"), "content"));
            comment.reply_user = string(field(field(field(item, "reply_to"), "user"), "nickname"));
            const auto images = array(field(item, "image_list"));
            if (!images.empty()) {
                comment.image = image(field(images.front(), "medium_url"));
            }
            result.comments.push_back(std::move(comment));
        }
        return result;
    }

    Qishui::Qishui(Loop &loop, net::Http &http)
    : client(loop, http),
      loop_(loop),
      http_(http) {
        capabilities = {.toplists = true,
                        .square = true,
                        .radio = true,
                        .radio_dislike = true,
                        .similar = true,
                        .library = true,
                        .likes = true,
                        .playlist_edit = true,
                        .playlist_order = true,
                        .private_playlists = true,
                        .collect_playlists = true,
                        .collect_albums = true,
                        .follow_artists = true,
                        .comments = {true, false, true},
                        .comment_replies = true,
                        .comment_likes = true,
                        .comment_post = true,
                        .comment_delete = true,
                        .session_import = true};
        radio_title = "For You";
        release_categories[0] = {{"", "All"}};
        reload_after_edit = true;
        first_use_ = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        features.recent_plays = [this] {
            return recent_plays();
        };
        features.reporting.start = [this](std::string id, std::string source) {
            return report_start(std::move(id), std::move(source));
        };
        features.reporting.play = [this](std::string id, std::string source, int seconds) {
            return report_play(std::move(id), std::move(source), seconds);
        };
    }

    Task<Result<void>> Qishui::initialize() {
        co_return co_await client.initialize();
    }

    Task<Result<void>> Qishui::import_session(std::string text) {
        const auto generation = client.generation;
        StringMap<std::string> settings;
        if (glz::read_json(settings, text)) {
            co_return std::unexpected("Connection settings must be a JSON object of strings");
        }
        const auto signer = settings.find("signer_url");
        if (signer == settings.end() || (!signer->second.starts_with("http://") && !signer->second.starts_with("https://"))) {
            co_return std::unexpected("Connection settings require an http:// or https:// signer_url");
        }
        for (const auto &[name, value] : settings) {
            if (name != "cookie_jar" && value.find_first_of("\r\n\t") != value.npos) {
                co_return std::unexpected("Connection settings contain invalid control characters");
            }
        }
        if (auto cookie = settings.find("cookie"); cookie != settings.end()) {
            std::string jar;
            std::string_view rest = cookie->second;
            while (!rest.empty()) {
                auto end = rest.find(';');
                auto entry = rest.substr(0, end);
                while (entry.starts_with(' ')) {
                    entry.remove_prefix(1);
                }
                const auto equal = entry.find('=');
                if (equal == entry.npos || equal == 0) {
                    co_return std::unexpected("Qishui Music Cookie must contain name=value pairs");
                }
                jar += std::format("api.qishui.com\tFALSE\t/\tTRUE\t0\t{}\t{}\n", entry.substr(0, equal), entry.substr(equal + 1));
                if (end == rest.npos) {
                    break;
                }
                rest.remove_prefix(end + 1);
            }
            settings["cookie_jar"] = net::CookieJar::from_netscape(jar).netscape();
            settings.erase("cookie");
        }
        Client candidate(loop_, http_);
        candidate.api_base = client.api_base;
        candidate.seo_base = client.seo_base;
        candidate.session = std::move(settings);
        auto ready = co_await candidate.initialize();
        if (!ready) {
            co_return ready;
        }
        if (!candidate.get("cookie_jar").empty()) {
            auto response = co_await candidate.call("/luna/pc/me");
            if (!response) {
                co_return std::unexpected(response.error());
            }
            auto account = account_info(*response);
            if (!account) {
                co_return std::unexpected(account.error());
            }
            candidate.session["user_id"] = account->id;
        }
        if (client.generation != generation) {
            co_return std::unexpected("Qishui Music session changed; retry the action");
        }
        ++client.generation;
        client.session = std::move(candidate.session);
        client.cookies_ = std::move(candidate.cookies_);
        pagination_.clear();
        square_seen_.clear();
        ++square_revision_;
        liked_playlist_id_.clear();
        played_media_.clear();
        feed_count_ = 0;
        lyric_id_.clear();
        lyric_text_.clear();
        lyric_type_.clear();
        if (client.on_session) {
            client.on_session();
        }
        co_return Result<void>{};
    }

    Task<Result<Account>> Qishui::account() {
        auto response = co_await client.call("/luna/pc/me");
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto result = account_info(*response);
        if (result && client.get("user_id") != result->id) {
            client.session["user_id"] = result->id;
            if (client.on_session) {
                client.on_session();
            }
        }
        co_return result;
    }

    Task<void> Qishui::logout() {
        ++client.generation;
        client.session.erase("cookie_jar");
        client.session.erase("cookie");
        client.session.erase("user_id");
        client.cookies_ = {};
        pagination_.clear();
        square_seen_.clear();
        ++square_revision_;
        liked_playlist_id_.clear();
        played_media_.clear();
        feed_count_ = 0;
        lyric_id_.clear();
        lyric_text_.clear();
        lyric_type_.clear();
        if (client.on_session) {
            client.on_session();
        }
        co_return;
    }

    Result<Qishui::Page> Qishui::begin_page(std::string name, std::string identity, int offset) {
        auto &pages = pagination_[name];
        if (offset == 0) {
            pages = {.identity = std::move(identity), .search_id = crypto::random_hex(32), .revision = ++page_revision_};
        }
        if (offset < 0 || static_cast<size_t>(offset) >= pages.cursors.size() || (offset != 0 && pages.identity != identity)) {
            return std::unexpected("Qishui Music pagination expired; refresh the list");
        }
        return Page{std::move(name), pages.cursors[offset], pages.search_id, pages.revision, offset};
    }

    Result<void> Qishui::finish_page(const Page &page, std::string next, Batch &batch) {
        auto found = pagination_.find(page.name);
        if (found == pagination_.end() || found->second.revision != page.revision) {
            return std::unexpected("Qishui Music list changed; retry the action");
        }
        auto &cursors = found->second.cursors;
        if (!next.empty()) {
            if (std::find(cursors.begin(), cursors.begin() + page.index + 1, next) != cursors.begin() + page.index + 1) {
                return std::unexpected("Qishui Music returned a repeated page");
            }
            cursors.resize(page.index + 1);
            cursors.push_back(std::move(next));
            batch.next_offset = page.index + 1;
        } else {
            batch.next_offset = -1;
        }
        batch.total = std::max(batch.total, static_cast<int>(batch.tracks.size() + batch.cards.size()) + (batch.next_offset >= 0));
        return {};
    }

    Task<Result<Batch>> Qishui::search(std::string query, SearchKind kind, int offset, int limit) {
        const auto type = kinds[static_cast<size_t>(kind)];
        auto page = begin_page(std::string("search:") + type, query, offset);
        if (!page) {
            co_return std::unexpected(page.error());
        }
        Result<Json> response;
        if (client.get("signer_url").empty()) {
            response = co_await client.android_search(type, std::move(query), page->cursor, limit);
        } else {
            Query fields{{"q", query},         {"cursor", page->cursor}, {"search_id", page->search_id}, {"search_method", "input"},
                         {"debug_params", ""}, {"from_search_id", ""},   {"search_scene", "main"},       {"scene_name", "search"}};
            response = co_await client.call(std::string("/luna/pc/search/") + type, std::move(fields));
        }
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto parsed = parse_search(*response, kind);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        auto done = finish_page(*page, std::move(parsed->cursor), parsed->batch);
        if (!done) {
            co_return std::unexpected(done.error());
        }
        co_return std::move(parsed->batch);
    }

    Task<Result<Json>> Qishui::track_data(std::string id) {
        if (client.get("signer_url").empty()) {
            auto response = co_await client.public_get(client.seo_base + "/luna/h5/seo_track?" + query_string({{"track_id", id}, {"device_platform", "web"}}));
            if (!response) {
                co_return std::unexpected(response.error());
            }
            const auto &seo = field(*response, "seo_track");
            if (string(field(field(seo, "track"), "id")).empty()) {
                co_return std::unexpected("Qishui Music returned incomplete song details");
            }
            const bool nested_lyric = !text(field(field(seo, "lyric"), "content")).empty();
            Json result;
            result["track"] = std::move((*response)["seo_track"]["track"]);
            result["lyric"] = nested_lyric ? std::move((*response)["seo_track"]["lyric"]) : std::move((*response)["lyric"]);
            result["track_player"] = std::move((*response)["track_player"]);
            co_return result;
        }
        Json body{{"track_id", id}, {"media_type", "track"}, {"queue_type", "favorite_track_playlist"}, {"scene_name", "library"}};
        auto response = co_await client.call("/luna/pc/track_v2", {}, glz::write_json(body).value());
        if (response && !response->contains("track") && response->contains("track_info")) {
            (*response)["track"] = std::move((*response)["track_info"]);
        }
        co_return response;
    }

    Task<Result<Stream>> Qishui::stream(Track track, Quality quality) {
        auto response = co_await track_data(track.id);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        lyric_id_ = track.id;
        lyric_text_ = string(field(field(*response, "lyric"), "content"));
        lyric_type_ = string(field(field(*response, "lyric"), "type"));
        uint32_t duration = track.duration;
        if (const auto milliseconds = number(field(field(*response, "track"), "duration")); milliseconds > 0) {
            duration = static_cast<uint32_t>(std::min(milliseconds / 1000.0, double(std::numeric_limits<uint32_t>::max())));
        }
        auto direct = parse_stream(*response, quality, duration);
        if (direct) {
            co_return direct;
        }
        const auto url = string(field(field(*response, "track_player"), "url_player_info"));
        if (!url.empty()) {
            if (!url.starts_with("https://") && !url.starts_with("http://")) {
                co_return std::unexpected("Qishui Music returned an invalid player URL");
            }
            auto player = co_await client.public_get(url);
            if (!player) {
                co_return std::unexpected(player.error());
            }
            const auto &error = field(field(*player, "ResponseMetadata"), "Error");
            if (!string(field(error, "Code")).empty()) {
                co_return std::unexpected("Qishui Music could not authorize this audio stream");
            }
            co_return parse_stream(*player, quality, duration);
        }
        co_return direct;
    }

    Task<Result<std::vector<LyricLine>>> Qishui::lyrics(Track track) {
        if (lyric_id_ == track.id) {
            co_return qishui::parse_lyrics(lyric_text_, lyric_type_);
        }
        auto response = co_await track_data(track.id);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return qishui::parse_lyrics(text(field(field(*response, "lyric"), "content")), text(field(field(*response, "lyric"), "type")));
    }

    Task<Result<std::vector<Track>>> Qishui::tracks(Card resource) {
        if (resource.kind == CardKind::album) {
            auto result = co_await album(resource.id);
            if (!result) {
                co_return std::unexpected(result.error());
            }
            co_return std::move(result->tracks);
        }
        if (resource.kind == CardKind::artist) {
            auto result = co_await artist_songs(resource.id, 0);
            if (!result) {
                co_return std::unexpected(result.error());
            }
            co_return std::move(result->tracks);
        }
        if (resource.id.starts_with("top:")) {
            Result<Json> response = std::unexpected("");
            if (client.get("signer_url").empty()) {
                response = co_await client.public_get("https://api5-lf.qishui.com/luna/charts/" + encode(resource.id.substr(4)) + "?charge=0");
            } else {
                response = co_await client.call("/luna/pc/charts/" + encode(resource.id.substr(4)));
            }
            if (!response) {
                co_return std::unexpected(response.error());
            }
            const auto &chart = field(*response, "chart");
            if (!chart.is_object()) {
                co_return std::unexpected("Qishui Music returned no chart");
            }
            co_return track_list(array(field(chart, "track_ranks")));
        }
        std::vector<Track> result;
        std::string cursor, session;
        std::unordered_set<std::string> cursors;
        for (;;) {
            Query query{{"playlist_id", resource.id}, {"cursor", cursor}, {"count", "100"}, {"session_id", session}, {"reverse", "false"}, {"ab_param", ""}};
            auto response = co_await client.call("/luna/pc/playlist/detail", std::move(query));
            if (!response) {
                co_return std::unexpected(response.error());
            }
            const auto &items = response->contains("media_resources") ? field(*response, "media_resources") : field(*response, "tracks");
            auto parsed = track_list(array(items));
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            result.insert(result.end(), std::make_move_iterator(parsed->begin()), std::make_move_iterator(parsed->end()));
            if (!boolean(field(*response, "has_more"))) {
                break;
            }
            cursor = string(field(*response, "next_cursor"));
            session = string(field(*response, "session_id"));
            if (cursor.empty() || !cursors.insert(cursor).second) {
                co_return std::unexpected("Qishui Music returned an invalid playlist continuation");
            }
        }
        co_return result;
    }

    Task<Result<std::vector<Card>>> Qishui::recommended_playlists() {
        if (!logged_in()) {
            auto response = co_await client.call("/luna/pc/discover", {}, "{}");
            if (!response) {
                co_return std::unexpected(response.error());
            }
            std::vector<Card> result;
            for (const auto &block : array(field(*response, "blocks"))) {
                auto parsed = collect_discovery(block, result);
                if (!parsed) {
                    co_return std::unexpected(parsed.error());
                }
            }
            co_return result;
        }
        auto response = co_await client.call("/luna/me/playlist/recommend");
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return card_list(array(field(*response, "playlists")), CardKind::playlist);
    }

    Task<Result<std::vector<Card>>> Qishui::toplists() {
        std::vector<Card> result;
        for (const auto &[id, name] :
             {std::pair{"7036274230471712007", "热歌榜"}, {"7060812597884869927", "新歌榜"}, {"7061475546400005410", "欧美榜"}, {"7415959718721494311", "音乐人歌曲榜"}}) {
            result.push_back({.source = Source::qishui, .id = std::string("top:") + id, .title = name, .commentable = false});
        }
        co_return result;
    }

    Task<Result<std::vector<Track>>> Qishui::fm() {
        Json body{{"played_media", played_media_},
                  {"did_first_use_time", first_use_},
                  {"is_first_request", feed_count_ == 0},
                  {"is_did_first_request", feed_count_ == 0},
                  {"feed_counts", Json{{"mix_session_count", feed_count_ + 1}}}};
        auto response = co_await client.call("/luna/pc/feed/song-tab", {}, glz::write_json(body).value());
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto parsed = track_list(array(field(*response, "items")));
        if (parsed) {
            ++feed_count_;
            played_media_.clear();
            for (const auto &track : *parsed) {
                played_media_.push_back(Json{{"media_id", track.id}, {"type", "track"}, {"duration", track.duration}, {"played_mills", -1}});
            }
        }
        co_return parsed;
    }

    Task<Result<void>> Qishui::fm_trash(Track track) {
        Json authors = Json::array_t{};
        if (!track.artist_id.empty()) {
            authors.get_array().push_back(track.artist_id);
        }
        co_return co_await write("/luna/dislike", Json{{"item_id", track.id}, {"author_ids", authors}, {"dislike_item_type", 1}, {"action_type", 0}});
    }

    Task<Result<std::vector<Track>>> Qishui::new_songs(std::string) {
        co_return co_await tracks({.source = Source::qishui, .id = "top:7060812597884869927"});
    }

    Task<Result<Categories>> Qishui::categories() {
        co_return Categories{};
    }

    Task<Result<Batch>> Qishui::square(std::string category, int offset) {
        uint64_t category_id = 0;
        if (!category.empty()) {
            const auto [end, error] = std::from_chars(category.data(), category.data() + category.size(), category_id);
            if (error != std::errc{} || end != category.data() + category.size()) {
                co_return std::unexpected("Invalid Qishui Music playlist category");
            }
        }
        if (offset == 0) {
            square_category_ = category;
            square_seen_.clear();
            ++square_revision_;
        } else if (category != square_category_) {
            co_return std::unexpected("Qishui Music list changed; refresh the page");
        }
        const auto revision = square_revision_;
        auto response = co_await client.call("/luna/feed/playlist-square", {}, glz::write_json(Json{{"category_id", category_id}}).value());
        if (!response) {
            co_return std::unexpected(response.error());
        }
        if (revision != square_revision_) {
            co_return std::unexpected("Qishui Music list changed; refresh the page");
        }
        Batch result;
        for (const auto &item : array(field(*response, "items"))) {
            auto parsed = collect_discovery(item, result.cards);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
        }
        // This is a recommendation feed: repeated requests return another batch, without a cursor.
        // Keep overlapping recommendations once, and stop requesting when a batch adds nothing.
        std::erase_if(result.cards, [this](const Card &card) {
            return !square_seen_.insert(card.id).second;
        });
        const bool more = boolean(field(*response, "has_more")) && !result.cards.empty();
        result.next_offset = more ? offset + 1 : -1;
        result.total = static_cast<int>(square_seen_.size()) + more;
        co_return result;
    }

    Task<Result<std::vector<Track>>> Qishui::similar(std::string id) {
        Json body{{"media_list", media(std::move(id))}, {"scene_name", "player"}};
        auto response = co_await client.call("/luna/pc/media/related", {}, glz::write_json(body).value());
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return track_list(array(field(*response, "items")));
    }

    Task<Result<AlbumInfo>> Qishui::album(std::string id) {
        auto response = co_await client.call("/luna/pc/albums/" + encode(id), {{"ignore_tracks", "false"}});
        if (!response) {
            co_return std::unexpected(response.error());
        }
        const auto &info = field(*response, "album_info");
        if (!info.is_object()) {
            co_return std::unexpected("Qishui Music returned no album");
        }
        AlbumInfo result;
        result.artist = artists(field(info, "artists"));
        const auto people = array(field(info, "artists"));
        if (!people.empty()) {
            result.artist_id = string(field(people.front(), "id"));
        }
        result.company = string(field(info, "company"));
        for (const auto &line : array(field(info, "pclines"))) {
            if (!result.about.empty()) {
                result.about += '\n';
            }
            result.about += string(line);
        }
        if (const auto milliseconds = number(field(info, "release_date")); milliseconds > 0 && milliseconds < 253402300800000.0) {
            const auto seconds = static_cast<time_t>(milliseconds / 1000);
            std::tm date;
            gmtime_r(&seconds, &date);
            char formatted[16];
            std::strftime(formatted, sizeof formatted, "%Y-%m-%d", &date);
            result.date = formatted;
        }
        auto parsed = track_list(array(field(*response, "tracks")));
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        result.tracks = std::move(*parsed);
        if (logged_in()) {
            auto saved = co_await collected_albums();
            if (!saved) {
                co_return std::unexpected(saved.error());
            }
            result.collected = std::ranges::any_of(*saved, [&](const auto &card) {
                return card.id == id;
            });
        }
        co_return result;
    }

    Task<Result<ArtistInfo>> Qishui::artist(std::string id) {
        auto response = co_await client.call("/luna/pc/artists/" + encode(id), {{"locate_item_id", ""}});
        if (!response) {
            co_return std::unexpected(response.error());
        }
        const auto &info = field(*response, "artist_info");
        if (!info.is_object()) {
            co_return std::unexpected("Qishui Music returned no artist");
        }
        ArtistInfo result{.name = string(field(info, "name")), .avatar = image(field(info, "url_avatar")), .songs = count(field(info, "count_tracks"))};
        if (logged_in()) {
            auto saved = co_await followed_artists();
            if (!saved) {
                co_return std::unexpected(saved.error());
            }
            result.followed = std::ranges::any_of(*saved, [&](const auto &card) {
                return card.id == id;
            });
        }
        co_return result;
    }

    Task<Result<Batch>> Qishui::artist_page(std::string id, int offset, bool albums) {
        auto page = begin_page(std::string(albums ? "artist_albums:" : "artist_songs:") + id, id, offset);
        if (!page) {
            co_return std::unexpected(page.error());
        }
        Query query{{"cursor", page->cursor}, {"count", albums ? "30" : "100"}};
        if (!albums) {
            query.emplace_back("sort_type", "0");
        }
        auto response = co_await client.call("/luna/pc/artists/" + encode(id) + (albums ? "/albums" : "/tracks"), std::move(query));
        if (!response) {
            co_return std::unexpected(response.error());
        }
        Batch result;
        if (albums) {
            auto parsed = card_list(array(field(*response, "albums")), CardKind::album);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            result.cards = std::move(*parsed);
        } else {
            auto parsed = track_list(array(field(*response, "tracks")));
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            result.tracks = std::move(*parsed);
        }
        const bool more = boolean(field(*response, "has_more"));
        std::string next = more ? string(field(*response, "next_cursor")) : "";
        if (more && next.empty()) {
            co_return std::unexpected("Qishui Music returned no artist continuation");
        }
        auto done = finish_page(*page, std::move(next), result);
        if (!done) {
            co_return std::unexpected(done.error());
        }
        co_return result;
    }

    Task<Result<Batch>> Qishui::artist_songs(std::string id, int offset) {
        co_return co_await artist_page(std::move(id), offset, false);
    }
    Task<Result<Batch>> Qishui::artist_albums(std::string id, int offset) {
        co_return co_await artist_page(std::move(id), offset, true);
    }

    Task<Result<CommentPage>> Qishui::comments(Subject subject, std::string id, bool, int, std::string cursor) {
        if (subject == Subject::album) {
            co_return std::unexpected("Qishui Music does not provide album comments");
        }
        auto response = co_await client.call(
            "/luna/pc/comments", {{"group_id", id}, {"group_type", subject == Subject::song ? "0" : "4"}, {"cursor", cursor}, {"count", "20"}, {"image_strategy", "3"}});
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return parse_comments(*response, client.get("user_id"));
    }

    Task<Result<CommentPage>> Qishui::comment_replies(Subject subject, std::string id, std::string root, int, std::string cursor) {
        auto response =
            co_await client.call("/luna/pc/comments/" + encode(root) + "/replies",
                                 {{"group_id", id}, {"group_type", subject == Subject::song ? "0" : "4"}, {"cursor", cursor}, {"count", "20"}, {"image_strategy", "3"}});
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return parse_comments(*response, client.get("user_id"), root);
    }

    Task<Result<void>> Qishui::like_comment(Subject, std::string resource, std::string id, bool on) {
        auto [root, reply] = comment_id(id);
        co_return co_await write("/luna/pc/comments/action",
                                 Json{{"comment_id", root}, {"reply_id", reply}, {"group_id", resource}, {"type", on ? 1 : 3}, {"scene_name", "comment_detail"}});
    }

    Task<Result<std::string>> Qishui::post_comment(Subject subject, std::string id, std::string content, std::string reply, std::string root) {
        if (subject == Subject::album) {
            co_return std::unexpected("Qishui Music does not provide album comments");
        }
        Json body{{"group_id", id}, {"text", content}, {"group_type", subject == Subject::song ? 0 : 4}};
        if (!reply.empty()) {
            auto [parent, child] = comment_id(reply);
            body["reply_id"] = root.empty() ? parent : root;
            if (!child.empty()) {
                body["reply_to_reply_id"] = child;
            }
        }
        auto response = co_await client.call("/luna/pc/comments/create", {}, glz::write_json(body).value(), Operation::write);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        std::string created = string(field(*response, "created_comment"));
        if (created.empty()) {
            co_return std::unexpected("Qishui Music did not confirm the posted comment");
        }
        co_return created;
    }

    Task<Result<void>> Qishui::delete_comment(Subject, std::string, std::string id) {
        auto [root, reply] = comment_id(id);
        co_return co_await write("/luna/pc/comments/delete", Json{{"comment_id", root}, {"reply_id", reply}});
    }

    namespace {
        // Materialize domain values page by page; discard the JSON tree before fetching the next page.
        template <class T, class Parse>
        Task<Result<T>> read_pages(Client &client, std::string path, std::string name, Query query, T result, Parse parse) {
            std::unordered_set<std::string> cursors;
            std::string cursor;
            for (;;) {
                auto fields = query;
                fields.emplace_back("cursor", cursor);
                fields.emplace_back("count", "100");
                auto response = co_await client.call(path, std::move(fields));
                if (!response) {
                    co_return std::unexpected(response.error());
                }
                const auto &items = field(*response, name);
                if (!items.is_null() && !items.is_array()) {
                    co_return std::unexpected("Qishui Music returned an invalid list");
                }
                if (auto parsed = parse(array(items), result); !parsed) {
                    co_return std::unexpected(parsed.error());
                }
                if (!boolean(field(*response, "has_more"))) {
                    break;
                }
                cursor = string(field(*response, "next_cursor"));
                if (cursor.empty() || !cursors.insert(cursor).second) {
                    co_return std::unexpected("Qishui Music returned an invalid list continuation");
                }
            }
            co_return result;
        }
    } // namespace

    Task<Result<std::pair<Card, std::vector<Card>>>> Qishui::user_playlists(std::string uid) {
        using Library = std::pair<Card, std::vector<Card>>;
        auto result = co_await read_pages(client, "/luna/pc/me/playlist", "playlists", {}, Library{Card{.source = Source::qishui, .commentable = false}, {}},
                                          [](std::span<const Json> items, Library &out) -> Result<void> {
                                              for (const auto &item : items) {
                                                  auto parsed = card(item, CardKind::playlist, true);
                                                  if (!parsed) {
                                                      return std::unexpected(parsed.error());
                                                  }
                                                  if (number(field(item, "type")) == 1) {
                                                      out.first = std::move(*parsed);
                                                      out.first.owned = out.first.collectable = out.first.commentable = false;
                                                  } else {
                                                      out.second.push_back(std::move(*parsed));
                                                  }
                                              }
                                              return {};
                                          });
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->first.id.empty()) {
            // The system likes playlist is exposed by the user catalogue, not always by the created-list catalogue.
            auto liked = co_await read_pages(client, "/luna/pc/user/playlist", "playlists", {{"user_id", uid}, {"scene", "aweme_user_profile"}},
                                             Card{.source = Source::qishui, .commentable = false}, [](std::span<const Json> items, Card &out) -> Result<void> {
                                                 for (const auto &item : items) {
                                                     if (number(field(item, "type")) != 1) {
                                                         continue;
                                                     }
                                                     auto parsed = card(item, CardKind::playlist);
                                                     if (!parsed) {
                                                         return std::unexpected(parsed.error());
                                                     }
                                                     out = std::move(*parsed);
                                                     out.owned = out.collectable = out.commentable = false;
                                                 }
                                                 return {};
                                             });
            if (!liked) {
                co_return std::unexpected(liked.error());
            }
            result->first = std::move(*liked);
        }
        auto saved = co_await collected_playlists();
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
        auto &playlists = result->second;
        playlists.reserve(playlists.size() + saved->size());
        std::unordered_set<std::string_view> ids;
        ids.reserve(playlists.size() + saved->size());
        for (const auto &playlist : playlists) {
            ids.insert(playlist.id);
        }
        for (auto &playlist : *saved) {
            if (ids.contains(playlist.id)) {
                continue;
            }
            playlists.push_back(std::move(playlist));
            ids.insert(playlists.back().id); // reserve above keeps all borrowed IDs stable
        }
        liked_playlist_id_ = result->first.id;
        co_return result;
    }

    Task<Result<std::vector<std::string>>> Qishui::liked_ids(std::string uid) {
        if (liked_playlist_id_.empty()) {
            auto playlists = co_await user_playlists(std::move(uid));
            if (!playlists) {
                co_return std::unexpected(playlists.error());
            }
        }
        std::vector<std::string> result;
        if (liked_playlist_id_.empty()) {
            co_return result;
        }
        auto songs = co_await tracks({.source = Source::qishui, .id = liked_playlist_id_});
        if (!songs) {
            co_return std::unexpected(songs.error());
        }
        result.reserve(songs->size());
        for (auto &song : *songs) {
            result.push_back(std::move(song.id));
        }
        co_return result;
    }

    Task<Result<void>> Qishui::write(std::string path, Json body) {
        if (!logged_in()) {
            co_return std::unexpected("Sign in to Qishui Music and try again");
        }
        auto response = co_await client.call(std::move(path), {}, glz::write_json(body).value(), Operation::write);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return Result<void>{};
    }

    Task<Result<void>> Qishui::like(Track track, bool on) {
        Json body{{"media", media(std::move(track.id))}};
        if (on) {
            body["scene"] = "";
        }
        co_return co_await write(std::string("/luna/pc/me/collection/media") + (on ? "" : "/delete"), std::move(body));
    }

    Task<Result<void>> Qishui::collection(std::string kind, std::string id, bool on) {
        Json body;
        body[kind + "_ids"] = Json::array_t{Json(std::move(id))};
        co_return co_await write("/luna/pc/me/collection/" + kind + (on ? "" : "/delete"), std::move(body));
    }

    Task<Result<void>> Qishui::follow(std::string id, bool on) {
        co_return co_await collection("artist", std::move(id), on);
    }
    Task<Result<void>> Qishui::collect(std::string id, bool on) {
        co_return co_await collection("album", std::move(id), on);
    }
    Task<Result<void>> Qishui::collect_playlist(std::string id, bool on) {
        co_return co_await collection("playlist", std::move(id), on);
    }

    Task<Result<std::vector<Card>>> Qishui::collections(CardKind kind) {
        const auto type = kind == CardKind::album ? "album" : "playlist";
        return read_pages(client, "/luna/me/collection/mixed", "mixed_collections", {{"item_types", type}}, std::vector<Card>{},
                          [kind, type](std::span<const Json> items, std::vector<Card> &out) -> Result<void> {
                              for (const auto &item : items) {
                                  if (!item.contains(type)) {
                                      continue;
                                  }
                                  auto parsed = card(field(item, type), kind);
                                  if (!parsed) {
                                      return std::unexpected(parsed.error());
                                  }
                                  out.push_back(std::move(*parsed));
                              }
                              return {};
                          });
    }

    Task<Result<std::vector<Card>>> Qishui::collected_playlists() {
        return collections(CardKind::playlist);
    }
    Task<Result<std::vector<Card>>> Qishui::collected_albums() {
        return collections(CardKind::album);
    }

    Task<Result<std::vector<Card>>> Qishui::followed_artists() {
        return read_pages(client, "/luna/me/collection/artist", "artists", {}, std::vector<Card>{}, [](std::span<const Json> items, std::vector<Card> &out) -> Result<void> {
            for (const auto &item : items) {
                auto parsed = card(item, CardKind::artist);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.push_back(std::move(*parsed));
            }
            return {};
        });
    }

    Task<Result<Card>> Qishui::create_playlist(std::string name, bool hidden) {
        Json body{{"name", name}, {"is_private", hidden}};
        auto response = co_await client.call("/luna/pc/me/playlist", {}, glz::write_json(body).value(), Operation::write);
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return card(field(*response, "playlist"), CardKind::playlist, true);
    }

    Task<Result<void>> Qishui::rename_playlist(std::string id, std::string name) {
        co_return co_await write("/luna/pc/me/playlist/update", Json{{"playlist_id", id}, {"name", name}});
    }

    Task<Result<void>> Qishui::delete_playlist(std::string id) {
        co_return co_await write("/luna/pc/me/playlist/delete", Json{{"playlist_ids", Json::array_t{Json(std::move(id))}}});
    }

    Task<Result<void>> Qishui::edit_playlist(std::string id, Track track, bool add) {
        co_return co_await write(std::string("/luna/pc/me/playlist/media/") + (add ? "append" : "delete"), Json{{"playlist_id", id}, {"media", media(std::move(track.id))}});
    }

    Task<Result<void>> Qishui::reorder_playlist(std::string id, std::vector<Track> tracks) {
        Json::array_t order;
        order.reserve(tracks.size());
        for (const auto &track : tracks) {
            order.push_back(Json{{"id", track.id}, {"type", "track"}});
        }
        co_return co_await write("/luna/me/playlist/media/sort", Json{{"playlist_id", id}, {"media", order}});
    }

    Task<Result<std::vector<Track>>> Qishui::recent_plays() {
        return read_pages(client, "/luna/pc/me/recently-played-media", "media", {{"media_type", "track"}, {"scene", "library"}}, std::vector<Track>{},
                          [](std::span<const Json> items, std::vector<Track> &out) -> Result<void> {
                              auto parsed = track_list(items);
                              if (!parsed) {
                                  return std::unexpected(parsed.error());
                              }
                              out.append_range(std::views::as_rvalue(*parsed));
                              return {};
                          });
    }

    Task<void> Qishui::report_start(std::string id, std::string) {
        if (!logged_in()) {
            co_return;
        }
        auto result = co_await write("/luna/pc/me/recently-played-media", Json{{"media", media(std::move(id))}});
        if (!result) {
            log::warn("qishui recently played: {}", result.error());
        }
    }

    Task<void> Qishui::report_play(std::string id, std::string, int seconds) {
        for (auto &item : played_media_) {
            if (string(field(item, "media_id")) == id) {
                item["played_mills"] = static_cast<int64_t>(seconds) * 1000;
                break;
            }
        }
        co_return;
    }

} // namespace lyren::qishui
