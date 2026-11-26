#include "qishui/qishui.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
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

        std::string image(const Json &value) {
            const auto urls = json::array(json::field(value, "urls"));
            if (urls.empty()) {
                return {};
            }
            std::string url = json::text(urls.front());
            const auto uri = json::text(value, "uri");
            if (url.ends_with('/') && !uri.empty()) {
                url += uri;
                const auto prefix = json::text(value, "template_prefix");
                if (!prefix.empty()) {
                    url += '~' + prefix + "-resize:{size}:{size}.png";
                }
            }
            return url;
        }

        std::string artists(const Json &value) {
            std::string names;
            for (const auto &artist : json::array(value)) {
                const auto name = json::text(artist, "name");
                if (!names.empty() && !name.empty()) {
                    names += " / ";
                }
                names += name;
            }
            return names;
        }

        Result<Track> track(const Json &value) {
            Track result{.source = Source::qishui};
            result.id = json::text(value, "id");
            result.title = json::text(value, "name");
            if (result.id.empty() || result.title.empty()) {
                return std::unexpected("Qishui Music returned incomplete song details");
            }
            result.duration = static_cast<uint32_t>(std::clamp(json::number(json::field(value, "duration")) / 1000.0, 0.0, double(std::numeric_limits<uint32_t>::max())));
            const auto &album = json::field(value, "album");
            result.album = json::text(album, "name");
            result.album_id = json::text(album, "id");
            result.cover = image(json::field(album, "url_cover"));
            result.artist = artists(json::field(value, "artists"));
            const auto people = json::array(json::field(value, "artists"));
            if (!people.empty()) {
                result.artist_id = json::text(people.front(), "id");
            }
            result.paid = json::boolean(json::field(json::field(value, "label_info"), "only_vip_playable"));
            return result;
        }

        const Json &track_entity(const Json &item) {
            const auto &entity = item.contains("entity") ? json::field(item, "entity") : item;
            if (entity.contains("track_wrapper")) {
                return json::field(json::field(entity, "track_wrapper"), "track");
            }
            return entity.contains("track") ? json::field(entity, "track") : entity;
        }

        Result<std::vector<Track>> track_list(std::span<const Json> items) {
            std::vector<Track> result;
            result.reserve(items.size());
            for (const auto &item : items) {
                const auto type = json::text(item, "type");
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
            result.id = json::text(value, "id");
            result.title = json::text(value, kind == CardKind::playlist ? "title" : "name");
            if (result.id.empty() || result.title.empty()) {
                return std::unexpected("Qishui Music returned incomplete resource details");
            }
            result.cover = image(json::field(value, kind == CardKind::artist ? "url_avatar" : "url_cover"));
            result.count = json::count(json::field(value, "count_tracks"));
            if (kind == CardKind::playlist) {
                const auto &total = json::field(json::field(value, "resource_cnt"), "track_cnt");
                if (!total.is_null()) {
                    result.count = json::count(total);
                }
            }
            result.count_kind = result.count ? Card::Count::songs : Card::Count::none;
            result.owned = owned;
            result.commentable = kind == CardKind::playlist && !json::boolean(json::field(value, "is_private"));
            if (kind == CardKind::playlist) {
                result.subtitle = json::text(json::field(value, "owner"), "nickname");
                result.collectable = !owned && result.commentable && json::number(json::field(value, "type")) != 1;
            } else if (kind == CardKind::album) {
                result.subtitle = artists(json::field(value, "artists"));
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

        Result<Profile> profile_info(const Json &response) {
            const auto &info = json::field(response, "my_info");
            Profile result{.id = json::text(info, "id"),
                           .name = json::text(info, "nickname"),
                           .avatar = image(json::field(info, "larger_avatar_url")),
                           .background = image(json::field(info, "bg_url")),
                           .signature = json::text(info, "signature")};
            if (result.name.empty()) {
                result.name = json::text(info, "public_name");
            }
            if (result.avatar.empty()) {
                result.avatar = image(json::field(info, "medium_avatar_url"));
            }
            if (result.background.empty()) {
                result.background = image(json::field(info, "cover_url"));
            }
            if (const auto &v = json::field(info, "count_following"); !v.is_null()) {
                result.follows = json::count(v);
            }
            if (const auto &v = json::field(info, "count_follower"); !v.is_null()) {
                result.fans = json::count(v);
            }
            result.vip = json::boolean(json::field(info, "is_vip")) || json::number(json::field(json::field(info, "vip_info"), "vip_status")) > 0;
            if (result.id.empty()) {
                return std::unexpected("Sign in to Qishui Music and try again");
            }
            return result;
        }

        Result<Account> account_info(const Json &response) {
            return profile_info(response).transform([](Profile p) {
                return Account{.id = std::move(p.id), .name = std::move(p.name), .avatar = std::move(p.avatar)};
            });
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
            if (const auto &playlist = json::field(json::field(block, "entity"), "playlist"); !playlist.is_null()) {
                auto parsed = card(playlist, CardKind::playlist);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.push_back(std::move(*parsed));
            }
            for (auto name : {"resources", "inner_block"}) {
                for (const auto &resource : json::array(json::field(block, name))) {
                    auto parsed = collect_discovery(resource, out);
                    if (!parsed) {
                        return parsed;
                    }
                }
            }
            return {};
        }

        bool looks_like_netscape_jar(std::string_view s) {
            while (!s.empty()) {
                const size_t nl = std::min(s.find('\n'), s.size());
                std::string_view line = trim(s.substr(0, nl));
                s.remove_prefix(std::min(nl + 1, s.size()));
                if (line.empty()) {
                    continue;
                }
                if (line.starts_with("#HttpOnly_")) {
                    return std::ranges::count(line, '\t') >= 6;
                }
                if (line.starts_with('#')) {
                    continue;
                }
                return std::ranges::count(line, '\t') >= 6;
            }
            return false;
        }

        struct ParsedSettings {
            StringMap<std::string> session;
            Result<std::string> cookie_jar{std::string{}};
        };

        Result<ParsedSettings> parse_settings_json(std::string_view text) {
            StringMap<std::string> raw;
            if (glz::read_json(raw, text)) {
                return std::unexpected("Connection settings must be a JSON object of strings");
            }
            ParsedSettings out;
            std::optional<std::string> raw_cookie;
            for (auto &[name, value] : raw) {
                if (name == "cookie") {
                    raw_cookie = std::move(value);
                    continue;
                }
                if (name == "cookie_jar") {
                    if (!raw_cookie) {
                        raw_cookie = std::move(value);
                    }
                    continue;
                }
                const auto trimmed = trim(value);
                if (trimmed.find_first_of("\r\n\t") != trimmed.npos) {
                    return std::unexpected("Connection settings contain invalid control characters");
                }
                if (!trimmed.empty()) {
                    out.session.insert_or_assign(name, std::string(trimmed));
                    if (name == "device_id" || name == "iid" || name == "fp") {
                        out.session.insert_or_assign("custom_" + name, std::string(trimmed));
                    }
                }
            }
            const auto signer = out.session.find("signer_url");
            if (signer == out.session.end() || (!signer->second.starts_with("http://") && !signer->second.starts_with("https://")) ||
                signer->second.find(' ') != std::string::npos) {
                return std::unexpected("Connection settings require an http:// or https:// signer_url");
            }
            if (raw_cookie) {
                out.cookie_jar = parse_cookie_input(*raw_cookie);
            }
            return out;
        }

        // Materialize domain values page by page; discard the JSON tree before fetching the next page.
        template <class T, class Parse>
        Task<Result<T>> read_pages(Client &client, std::string path, std::string name, Query query, T result, Parse parse) {
            StringHashSet cursors;
            std::string cursor;
            for (;;) {
                auto fields = query;
                fields.emplace_back("cursor", cursor);
                fields.emplace_back("count", "100");
                auto response = co_await ok(co_await client.call(path, std::move(fields)));
                const auto &items = json::field(response, name);
                if (!items.is_null() && !items.is_array()) {
                    co_return std::unexpected("Qishui Music returned an invalid list");
                }
                co_await ok(parse(json::array(items), result));
                if (!json::boolean(json::field(response, "has_more"))) {
                    break;
                }
                cursor = json::text(response, "next_cursor");
                if (cursor.empty() || !cursors.insert(cursor).second) {
                    co_return std::unexpected("Qishui Music returned an invalid list continuation");
                }
            }
            co_return result;
        }
    } // namespace

    Result<SearchPage> parse_search(const Json &response, SearchKind kind) {
        SearchPage result;
        const auto &groups = json::field(response, "result_groups");
        if (!groups.is_array()) {
            return std::unexpected("Qishui Music returned no search results");
        }
        const auto index = static_cast<size_t>(kind);
        for (const auto &group : groups.get_array()) {
            if (json::text_view(json::field(group, "id")) != group_ids[index]) {
                continue;
            }
            const auto items = json::array(json::field(group, "data"));
            if (kind == SearchKind::tracks) {
                result.batch.tracks.reserve(items.size());
            } else {
                result.batch.cards.reserve(items.size());
            }
            for (const auto &item : items) {
                const auto &entity = json::field(item, "entity");
                if (kind == SearchKind::tracks) {
                    auto parsed = track(track_entity(entity));
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    result.batch.tracks.push_back(std::move(*parsed));
                } else {
                    const auto card_kind = kind == SearchKind::albums ? CardKind::album : kind == SearchKind::artists ? CardKind::artist : CardKind::playlist;
                    auto parsed = card(json::field(entity, kinds[index]), card_kind);
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    result.batch.cards.push_back(std::move(*parsed));
                }
            }
            if (json::boolean(json::field(group, "has_more"))) {
                result.cursor = json::text(group, "next_cursor");
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
        result.total = json::count(json::field(response, "count"));
        result.more = json::boolean(json::field(response, "has_more"));
        result.cursor = json::text(response, "cursor");
        const auto items = json::array(json::field(response, root.empty() ? "comments" : "reply_infos"));
        result.comments.reserve(items.size());
        if (result.more && result.cursor.empty()) {
            return std::unexpected("Qishui Music returned no comment continuation");
        }
        for (const auto &item : items) {
            Comment comment;
            comment.id = json::text(item, "id");
            if (comment.id.empty()) {
                return std::unexpected("Qishui Music returned incomplete comment details");
            }
            if (!root.empty()) {
                comment.id = std::string(root) + ':' + comment.id;
            }
            const auto &user = json::field(item, "user");
            comment.user_id = json::text(user, "id");
            comment.user = json::text(user, "nickname");
            comment.avatar = image(json::field(user, "medium_avatar_url"));
            comment.text = json::text(item, "content");
            comment.likes = json::count(json::field(item, "count_digged"));
            comment.replies = json::count(json::field(item, "count_reply"));
            comment.liked = json::boolean(json::field(item, "user_digged"));
            comment.owned = json::boolean(json::field(item, "can_delete")) || (!user_id.empty() && comment.user_id == user_id);
            const auto &reply_to = json::field(item, "reply_to");
            comment.reply = json::text(reply_to, "content");
            comment.reply_user = json::text(json::field(reply_to, "user"), "nickname");
            const auto images = json::array(json::field(item, "image_list"));
            if (!images.empty()) {
                comment.image = image(json::field(images.front(), "medium_url"));
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
                        .session_import = true,
                        .profiles = true,
                        .profile_playlists = true};
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

    Result<std::string> parse_cookie_input(std::string_view raw) {
        raw = trim(raw);
        if (raw.empty()) {
            return std::string{};
        }
        if (looks_like_netscape_jar(raw)) {
            std::string normalized = net::CookieJar::from_netscape(raw).netscape();
            if (normalized.empty()) {
                return std::unexpected("Qishui Music Cookie must contain name=value pairs");
            }
            return normalized;
        }
        if (raw.size() >= 7) {
            bool prefix = true;
            for (size_t i = 0; i < 6; ++i) {
                const auto c = static_cast<unsigned char>(raw[i]);
                if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != static_cast<unsigned char>("cookie"[i])) {
                    prefix = false;
                    break;
                }
            }
            if (prefix && raw[6] == ':') {
                raw = trim(raw.substr(7));
            }
        }
        std::string jar;
        bool any = false;
        std::string_view rest = raw;
        while (!rest.empty()) {
            const auto end = rest.find(';');
            const auto entry = trim(rest.substr(0, end));
            if (!entry.empty()) {
                const auto equal = entry.find('=');
                if (equal == entry.npos || equal == 0) {
                    return std::unexpected("Qishui Music Cookie must contain name=value pairs");
                }
                const auto name = trim(entry.substr(0, equal));
                const auto value = trim(entry.substr(equal + 1));
                if (name.empty() || name.find_first_of(" \t\r\n") != name.npos || value.find_first_of(" \t\r\n") != value.npos) {
                    return std::unexpected("Qishui Music Cookie must contain name=value pairs");
                }
                jar += std::format(".qishui.com\tTRUE\t/\tTRUE\t0\t{}\t{}\n", name, value);
                any = true;
            }
            if (end == rest.npos) {
                break;
            }
            rest.remove_prefix(end + 1);
        }
        if (!any) {
            return std::string{};
        }
        return net::CookieJar::from_netscape(jar).netscape();
    }

    Task<Result<void>> Qishui::initialize() {
        return client.initialize();
    }

    Task<SessionCheck> Qishui::check_session(std::string text) {
        using State = SessionCheck::State;
        SessionCheck check;
        auto parsed = parse_settings_json(text);
        if (!parsed) {
            check.signer = {State::failed, parsed.error()};
            co_return check;
        }
        Client candidate(loop_, http_);
        candidate.api_base = client.api_base;
        candidate.seo_base = client.seo_base;
        candidate.session = std::move(parsed->session);
        auto config = co_await candidate.signer("/config");
        if (!config) {
            check.signer = {State::failed, config.error()};
            co_return check;
        }
        check.signer = {State::ok, candidate.get("signer_url")};
        const auto &identity = json::field(*config, "identity");
        for (std::string_view name : {"device_id", "iid", "fp"}) {
            std::string custom = candidate.get(std::format("custom_{}", name));
            if (!custom.empty()) {
                candidate.session.insert_or_assign(std::string(name), std::move(custom));
                continue;
            }
            auto value = json::text(identity, name);
            if (!value.empty()) {
                candidate.session.insert_or_assign(std::string(name), std::move(value));
            }
        }
        if (candidate.get("device_id").empty() || candidate.get("iid").empty()) {
            check.identity = {State::failed, "Qishui Music signer must provide device_id and iid, or include them in the connection settings"};
            co_return check;
        }
        if (candidate.get("fp").empty()) {
            candidate.session["fp"] = candidate.get("device_id");
        }
        check.identity = {State::ok, std::format("device_id {}", candidate.get("device_id"))};
        if (!parsed->cookie_jar) {
            check.account = {State::failed, parsed->cookie_jar.error()};
            co_return check;
        }
        if (parsed->cookie_jar->empty()) {
            check.account = {State::skipped, "No Cookie provided; signing service ready"};
            co_return check;
        }
        candidate.session["cookie_jar"] = *parsed->cookie_jar;
        candidate.cookies_ = net::CookieJar::from_netscape(*parsed->cookie_jar);
        auto response = co_await candidate.call("/luna/pc/me");
        if (!response) {
            check.account = {State::failed, response.error()};
            co_return check;
        }
        auto account = account_info(*response);
        if (!account) {
            check.account = {State::failed, account.error()};
            co_return check;
        }
        check.account = {State::ok, account->name.empty() ? account->id : std::format("{} ({})", account->name, account->id)};
        co_return check;
    }

    Task<Result<void>> Qishui::import_session(std::string text) {
        const auto generation = client.generation;
        auto parsed = co_await ok(parse_settings_json(text));
        auto cookie_jar = co_await ok(std::move(parsed.cookie_jar));
        StringMap<std::string> settings = std::move(parsed.session);
        if (!cookie_jar.empty()) {
            settings["cookie_jar"] = std::move(cookie_jar);
        }
        Client candidate(loop_, http_);
        candidate.api_base = client.api_base;
        candidate.seo_base = client.seo_base;
        candidate.session = std::move(settings);
        co_await ok(co_await candidate.initialize());
        if (!candidate.get("cookie_jar").empty()) {
            auto response = co_await ok(co_await candidate.call("/luna/pc/me"));
            auto account = co_await ok(account_info(response));
            candidate.session["user_id"] = account.id;
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
        auto response = co_await ok(co_await client.call("/luna/pc/me"));
        auto result = account_info(response);
        if (result && client.get("user_id") != result->id) {
            client.session["user_id"] = result->id;
            if (client.on_session) {
                client.on_session();
            }
        }
        co_return result;
    }

    Task<Result<Profile>> Qishui::profile(std::string uid) {
        if (!uid.empty() && uid != client.get("user_id")) {
            // Another listener's public playlists are available; their name and avatar come from the source item.
            co_return Profile{.id = std::move(uid)};
        }
        auto response = co_await ok(co_await client.call("/luna/pc/me"));
        co_return profile_info(response);
    }

    Task<void> Qishui::logout() {
        const bool was_logged_in = logged_in();
        ++client.generation;
        client.session.erase("cookie_jar");
        client.session.erase("cookie");
        client.session.erase("user_id");
        if (!was_logged_in) {
            for (std::string_view key : {"signer_url", "signer_token", "device_id", "iid", "fp", "custom_device_id", "custom_iid", "custom_fp"}) {
                client.session.erase(std::string(key));
            }
        }
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
        if (!was_logged_in) {
            auto saved_cb = std::move(client.on_session);
            (void)co_await client.initialize();
            client.on_session = std::move(saved_cb);
        }
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
        auto page = co_await ok(begin_page(std::string("search:") + type, query, offset));
        Result<Json> response;
        if (client.get("signer_url").empty()) {
            response = co_await client.android_search(type, std::move(query), page.cursor, limit);
        } else {
            Query fields{{"q", query},         {"cursor", page.cursor}, {"search_id", page.search_id}, {"search_method", "input"},
                         {"debug_params", ""}, {"from_search_id", ""},  {"search_scene", "main"},      {"scene_name", "search"}};
            response = co_await client.call(std::string("/luna/pc/search/") + type, std::move(fields));
        }
        auto parsed = co_await ok(parse_search(co_await ok(std::move(response)), kind));
        co_await ok(finish_page(page, std::move(parsed.cursor), parsed.batch));
        co_return std::move(parsed.batch);
    }

    Task<Result<Json>> Qishui::track_data(std::string id) {
        if (client.get("signer_url").empty()) {
            auto response =
                co_await ok(co_await client.public_get(client.seo_base + "/luna/h5/seo_track?" + encoding::query_string({{"track_id", id}, {"device_platform", "web"}})));
            const auto &seo = json::field(response, "seo_track");
            if (json::text(json::field(seo, "track"), "id").empty()) {
                co_return std::unexpected("Qishui Music returned incomplete song details");
            }
            const bool nested_lyric = !json::text_view(json::field(json::field(seo, "lyric"), "content")).empty();
            Json result;
            result["track"] = std::move(response["seo_track"]["track"]);
            result["lyric"] = nested_lyric ? std::move(response["seo_track"]["lyric"]) : std::move(response["lyric"]);
            result["track_player"] = std::move(response["track_player"]);
            co_return result;
        }
        Json body{{"track_id", id}, {"media_type", "track"}, {"queue_type", "favorite_track_playlist"}, {"scene_name", "library"}};
        auto response = co_await ok(co_await client.call("/luna/pc/track_v2", {}, json::write(body)));
        if (!response.contains("track") && response.contains("track_info")) {
            response["track"] = std::move(response["track_info"]);
        }
        co_return response;
    }

    Task<Result<Stream>> Qishui::stream(Track track, Quality quality) {
        auto response = co_await ok(co_await track_data(track.id));
        lyric_id_ = track.id;
        lyric_text_ = json::text(json::field(response, "lyric"), "content");
        lyric_type_ = json::text(json::field(response, "lyric"), "type");
        uint32_t duration = track.duration;
        if (const auto milliseconds = json::number(json::field(json::field(response, "track"), "duration")); milliseconds > 0) {
            duration = static_cast<uint32_t>(std::min(milliseconds / 1000.0, double(std::numeric_limits<uint32_t>::max())));
        }
        auto direct = parse_stream(response, quality, duration);
        if (direct) {
            co_return direct;
        }
        const auto url = json::text(json::field(response, "track_player"), "url_player_info");
        if (!url.empty()) {
            if (!url.starts_with("https://") && !url.starts_with("http://")) {
                co_return std::unexpected("Qishui Music returned an invalid player URL");
            }
            auto player = co_await ok(co_await client.public_get(url));
            const auto &error = json::field(json::field(player, "ResponseMetadata"), "Error");
            if (!json::text(error, "Code").empty()) {
                co_return std::unexpected("Qishui Music could not authorize this audio stream");
            }
            co_return parse_stream(player, quality, duration);
        }
        co_return direct;
    }

    Task<Result<std::vector<LyricLine>>> Qishui::lyrics(Track track) {
        if (lyric_id_ == track.id) {
            co_return qishui::parse_lyrics(lyric_text_, lyric_type_);
        }
        auto response = co_await ok(co_await track_data(track.id));
        const auto &lyric = json::field(response, "lyric");
        co_return qishui::parse_lyrics(json::text_view(json::field(lyric, "content")), json::text_view(json::field(lyric, "type")));
    }

    Task<Result<std::vector<Track>>> Qishui::tracks(Card resource) {
        if (resource.kind == CardKind::album) {
            co_return (co_await ok(co_await album(resource.id))).tracks;
        }
        if (resource.kind == CardKind::artist) {
            co_return (co_await ok(co_await artist_songs(resource.id, 0))).tracks;
        }
        if (resource.id.starts_with("top:")) {
            const auto encoded = encoding::url_encode(std::string_view(resource.id).substr(4));
            Result<Json> response = std::unexpected("");
            if (client.get("signer_url").empty()) {
                response = co_await client.public_get("https://api5-lf.qishui.com/luna/charts/" + encoded + "?charge=0");
            } else {
                response = co_await client.call("/luna/pc/charts/" + encoded);
            }
            auto body = co_await ok(std::move(response));
            const auto &chart = json::field(body, "chart");
            if (!chart.is_object()) {
                co_return std::unexpected("Qishui Music returned no chart");
            }
            co_return track_list(json::array(json::field(chart, "track_ranks")));
        }
        std::vector<Track> result;
        std::string cursor, session;
        StringHashSet cursors;
        for (;;) {
            Query query{{"playlist_id", resource.id}, {"cursor", cursor}, {"count", "100"}, {"session_id", session}, {"reverse", "false"}, {"ab_param", ""}};
            auto response = co_await ok(co_await client.call("/luna/pc/playlist/detail", std::move(query)));
            const auto &items = response.contains("media_resources") ? json::field(response, "media_resources") : json::field(response, "tracks");
            auto parsed = co_await ok(track_list(json::array(items)));
            result.append_range(std::views::as_rvalue(parsed));
            if (!json::boolean(json::field(response, "has_more"))) {
                break;
            }
            cursor = json::text(response, "next_cursor");
            session = json::text(response, "session_id");
            if (cursor.empty() || !cursors.insert(cursor).second) {
                co_return std::unexpected("Qishui Music returned an invalid playlist continuation");
            }
        }
        co_return result;
    }

    Task<Result<std::vector<Card>>> Qishui::recommended_playlists() {
        if (!logged_in()) {
            auto response = co_await ok(co_await client.call("/luna/pc/discover", {}, "{}"));
            std::vector<Card> result;
            for (const auto &block : json::array(json::field(response, "blocks"))) {
                co_await ok(collect_discovery(block, result));
            }
            co_return result;
        }
        auto response = co_await ok(co_await client.call("/luna/me/playlist/recommend"));
        co_return card_list(json::array(json::field(response, "playlists")), CardKind::playlist);
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
        auto response = co_await ok(co_await client.call("/luna/pc/feed/song-tab", {}, json::write(body)));
        auto parsed = co_await ok(track_list(json::array(json::field(response, "items"))));
        ++feed_count_;
        played_media_.clear();
        for (const auto &track : parsed) {
            played_media_.push_back(Json{{"media_id", track.id}, {"type", "track"}, {"duration", track.duration}, {"played_mills", -1}});
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
        return tracks({.source = Source::qishui, .id = "top:7060812597884869927"});
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
        auto response = co_await ok(co_await client.call("/luna/feed/playlist-square", {}, json::write(Json{{"category_id", category_id}})));
        if (revision != square_revision_) {
            co_return std::unexpected("Qishui Music list changed; refresh the page");
        }
        Batch result;
        for (const auto &item : json::array(json::field(response, "items"))) {
            co_await ok(collect_discovery(item, result.cards));
        }
        // This is a recommendation feed: repeated requests return another batch, without a cursor.
        // Keep overlapping recommendations once, and stop requesting when a batch adds nothing.
        std::erase_if(result.cards, [this](const Card &card) {
            return !square_seen_.insert(card.id).second;
        });
        const bool more = json::boolean(json::field(response, "has_more")) && !result.cards.empty();
        result.next_offset = more ? offset + 1 : -1;
        result.total = static_cast<int>(square_seen_.size()) + more;
        co_return result;
    }

    Task<Result<std::vector<Track>>> Qishui::similar(std::string id) {
        Json body{{"media_list", media(std::move(id))}, {"scene_name", "player"}};
        auto response = co_await ok(co_await client.call("/luna/pc/media/related", {}, json::write(body)));
        co_return track_list(json::array(json::field(response, "items")));
    }

    Task<Result<AlbumInfo>> Qishui::album(std::string id) {
        auto response = co_await ok(co_await client.call("/luna/pc/albums/" + encoding::url_encode(id), {{"ignore_tracks", "false"}}));
        const auto &info = json::field(response, "album_info");
        if (!info.is_object()) {
            co_return std::unexpected("Qishui Music returned no album");
        }
        AlbumInfo result;
        result.artist = artists(json::field(info, "artists"));
        const auto people = json::array(json::field(info, "artists"));
        if (!people.empty()) {
            result.artist_id = json::text(people.front(), "id");
        }
        result.company = json::text(info, "company");
        for (const auto &line : json::array(json::field(info, "pclines"))) {
            if (!result.about.empty()) {
                result.about += '\n';
            }
            result.about += json::text(line);
        }
        if (const auto milliseconds = json::number(json::field(info, "release_date")); milliseconds > 0 && milliseconds < 253402300800000.0) {
            const auto seconds = static_cast<time_t>(milliseconds / 1000);
            std::tm date{};
            gmtime_r(&seconds, &date);
            result.date = std::format("{:04d}-{:02d}-{:02d}", date.tm_year + 1900, date.tm_mon + 1, date.tm_mday);
        }
        result.tracks = co_await ok(track_list(json::array(json::field(response, "tracks"))));
        co_return result;
    }

    Task<Result<ArtistInfo>> Qishui::artist(std::string id) {
        auto response = co_await ok(co_await client.call("/luna/pc/artists/" + encoding::url_encode(id), {{"locate_item_id", ""}}));
        const auto &info = json::field(response, "artist_info");
        if (!info.is_object()) {
            co_return std::unexpected("Qishui Music returned no artist");
        }
        co_return ArtistInfo{.name = json::text(info, "name"), .avatar = image(json::field(info, "url_avatar")), .songs = json::count(json::field(info, "count_tracks"))};
    }

    Task<Result<Batch>> Qishui::artist_page(std::string id, int offset, bool albums) {
        auto page = co_await ok(begin_page(std::string(albums ? "artist_albums:" : "artist_songs:") + id, id, offset));
        Query query{{"cursor", page.cursor}, {"count", albums ? "30" : "100"}};
        if (!albums) {
            query.emplace_back("sort_type", "0");
        }
        auto response = co_await ok(co_await client.call("/luna/pc/artists/" + encoding::url_encode(id) + (albums ? "/albums" : "/tracks"), std::move(query)));
        Batch result;
        if (albums) {
            result.cards = co_await ok(card_list(json::array(json::field(response, "albums")), CardKind::album));
        } else {
            result.tracks = co_await ok(track_list(json::array(json::field(response, "tracks"))));
        }
        const bool more = json::boolean(json::field(response, "has_more"));
        std::string next = more ? json::text(response, "next_cursor") : "";
        if (more && next.empty()) {
            co_return std::unexpected("Qishui Music returned no artist continuation");
        }
        co_await ok(finish_page(page, std::move(next), result));
        co_return result;
    }

    Task<Result<Batch>> Qishui::artist_songs(std::string id, int offset) {
        return artist_page(std::move(id), offset, false);
    }
    Task<Result<Batch>> Qishui::artist_albums(std::string id, int offset) {
        return artist_page(std::move(id), offset, true);
    }

    Task<Result<CommentPage>> Qishui::comments(Subject subject, std::string id, bool, int, std::string cursor) {
        if (subject == Subject::album) {
            co_return std::unexpected("Qishui Music does not provide album comments");
        }
        auto response = co_await ok(co_await client.call(
            "/luna/pc/comments", {{"group_id", id}, {"group_type", subject == Subject::song ? "0" : "4"}, {"cursor", cursor}, {"count", "20"}, {"image_strategy", "3"}}));
        co_return parse_comments(response, client.get("user_id"));
    }

    Task<Result<CommentPage>> Qishui::comment_replies(Subject subject, std::string id, std::string root, int, std::string cursor) {
        auto response = co_await ok(
            co_await client.call("/luna/pc/comments/" + encoding::url_encode(root) + "/replies",
                                 {{"group_id", id}, {"group_type", subject == Subject::song ? "0" : "4"}, {"cursor", cursor}, {"count", "20"}, {"image_strategy", "3"}}));
        co_return parse_comments(response, client.get("user_id"), root);
    }

    Task<Result<void>> Qishui::like_comment(Subject, std::string resource, std::string id, bool on) {
        auto [root, reply] = comment_id(id);
        return write(
            "/luna/pc/comments/action",
            Json{{"comment_id", std::move(root)}, {"reply_id", std::move(reply)}, {"group_id", std::move(resource)}, {"type", on ? 1 : 3}, {"scene_name", "comment_detail"}});
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
        auto response = co_await ok(co_await client.call("/luna/pc/comments/create", {}, json::write(body), Operation::write));
        std::string created = json::text(response, "created_comment");
        if (created.empty()) {
            co_return std::unexpected("Qishui Music did not confirm the posted comment");
        }
        co_return created;
    }

    Task<Result<void>> Qishui::delete_comment(Subject, std::string, std::string id) {
        auto [root, reply] = comment_id(id);
        return write("/luna/pc/comments/delete", Json{{"comment_id", std::move(root)}, {"reply_id", std::move(reply)}});
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Qishui::user_playlists(std::string uid) {
        using Library = std::pair<Card, std::vector<Card>>;
        if (!uid.empty() && uid != client.get("user_id")) {
            co_return co_await read_pages(client, "/luna/pc/user/playlist", "playlists", {{"user_id", std::move(uid)}, {"scene", "aweme_user_profile"}},
                                          Library{Card{.source = Source::qishui, .commentable = false}, {}}, [](std::span<const Json> items, Library &out) -> Result<void> {
                                              for (const auto &item : items) {
                                                  auto parsed = card(item, CardKind::playlist);
                                                  if (!parsed) {
                                                      return std::unexpected(parsed.error());
                                                  }
                                                  if (json::number(json::field(item, "type")) == 1) {
                                                      out.first = std::move(*parsed);
                                                      out.first.owned = out.first.collectable = out.first.commentable = false;
                                                  } else {
                                                      parsed->owned = true;
                                                      out.second.push_back(std::move(*parsed));
                                                  }
                                              }
                                              return {};
                                          });
        }
        auto result = co_await ok(co_await read_pages(client, "/luna/pc/me/playlist", "playlists", {}, Library{Card{.source = Source::qishui, .commentable = false}, {}},
                                                      [](std::span<const Json> items, Library &out) -> Result<void> {
                                                          for (const auto &item : items) {
                                                              auto parsed = card(item, CardKind::playlist, true);
                                                              if (!parsed) {
                                                                  return std::unexpected(parsed.error());
                                                              }
                                                              if (json::number(json::field(item, "type")) == 1) {
                                                                  out.first = std::move(*parsed);
                                                                  out.first.owned = out.first.collectable = out.first.commentable = false;
                                                              } else {
                                                                  out.second.push_back(std::move(*parsed));
                                                              }
                                                          }
                                                          return {};
                                                      }));
        const std::string owner_id = uid.empty() ? client.get("user_id") : uid;
        if (result.first.id.empty() && !owner_id.empty()) {
            // The system likes playlist is exposed by the user catalogue, not always by the created-list catalogue.
            result.first = co_await ok(co_await read_pages(client, "/luna/pc/user/playlist", "playlists", {{"user_id", owner_id}, {"scene", "aweme_user_profile"}},
                                                           Card{.source = Source::qishui, .commentable = false}, [](std::span<const Json> items, Card &out) -> Result<void> {
                                                               for (const auto &item : items) {
                                                                   if (json::number(json::field(item, "type")) != 1) {
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
                                                           }));
        }
        auto saved = co_await ok(co_await collected_playlists());
        auto &playlists = result.second;
        playlists.reserve(playlists.size() + saved.size());
        std::unordered_set<std::string_view> ids;
        ids.reserve(playlists.size() + saved.size());
        for (const auto &playlist : playlists) {
            ids.insert(playlist.id);
        }
        for (auto &playlist : saved) {
            if (ids.contains(playlist.id)) {
                continue;
            }
            playlists.push_back(std::move(playlist));
            ids.insert(playlists.back().id); // reserve above keeps all borrowed IDs stable
        }
        liked_playlist_id_ = result.first.id;
        co_return result;
    }

    Task<Result<std::vector<std::string>>> Qishui::liked_ids(std::string uid) {
        if (liked_playlist_id_.empty()) {
            (void)co_await ok(co_await user_playlists(std::move(uid)));
        }
        std::vector<std::string> result;
        if (liked_playlist_id_.empty()) {
            co_return result;
        }
        auto songs = co_await ok(co_await tracks({.source = Source::qishui, .id = liked_playlist_id_}));
        result.reserve(songs.size());
        for (auto &song : songs) {
            result.push_back(std::move(song.id));
        }
        co_return result;
    }

    Task<Result<void>> Qishui::write(std::string path, Json body) {
        if (!logged_in()) {
            co_return std::unexpected("Sign in to Qishui Music and try again");
        }
        (void)co_await ok(co_await client.call(std::move(path), {}, json::write(body), Operation::write));
        co_return Result<void>{};
    }

    Task<Result<void>> Qishui::like(Track track, bool on) {
        Json body{{"media", media(std::move(track.id))}};
        if (on) {
            body["scene"] = "";
        }
        return write(std::string("/luna/pc/me/collection/media") + (on ? "" : "/delete"), std::move(body));
    }

    Task<Result<void>> Qishui::collection(std::string kind, std::string id, bool on) {
        Json body;
        body[kind + "_ids"] = Json::array_t{Json(std::move(id))};
        return write("/luna/pc/me/collection/" + kind + (on ? "" : "/delete"), std::move(body));
    }

    Task<Result<void>> Qishui::follow(std::string id, bool on) {
        return collection("artist", std::move(id), on);
    }
    Task<Result<void>> Qishui::collect(std::string id, bool on) {
        return collection("album", std::move(id), on);
    }
    Task<Result<void>> Qishui::collect_playlist(std::string id, bool on) {
        return collection("playlist", std::move(id), on);
    }

    Task<Result<std::vector<Card>>> Qishui::collections(CardKind kind) {
        const auto type = kind == CardKind::album ? "album" : "playlist";
        return read_pages(client, "/luna/me/collection/mixed", "mixed_collections", {{"item_types", type}}, std::vector<Card>{},
                          [kind, type](std::span<const Json> items, std::vector<Card> &out) -> Result<void> {
                              for (const auto &item : items) {
                                  if (!item.contains(type)) {
                                      continue;
                                  }
                                  auto parsed = card(json::field(item, type), kind);
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
        Json body{{"name", std::move(name)}, {"is_private", hidden}};
        auto response = co_await ok(co_await client.call("/luna/pc/me/playlist", {}, json::write(body), Operation::write));
        co_return card(json::field(response, "playlist"), CardKind::playlist, true);
    }

    Task<Result<void>> Qishui::rename_playlist(std::string id, std::string name) {
        return write("/luna/pc/me/playlist/update", Json{{"playlist_id", std::move(id)}, {"name", std::move(name)}});
    }

    Task<Result<void>> Qishui::delete_playlist(std::string id) {
        return write("/luna/pc/me/playlist/delete", Json{{"playlist_ids", Json::array_t{Json(std::move(id))}}});
    }

    Task<Result<void>> Qishui::edit_playlist(std::string id, std::vector<Track> tracks, bool add) {
        Json items = Json::array_t{};
        for (auto &track : tracks) {
            items.get_array().push_back(Json{{"id", std::move(track.id)}, {"type", "track"}});
        }
        return write(std::string("/luna/pc/me/playlist/media/") + (add ? "append" : "delete"), Json{{"playlist_id", std::move(id)}, {"media", std::move(items)}});
    }

    Task<Result<void>> Qishui::reorder_playlist(std::string id, std::vector<Track> tracks) {
        Json::array_t order;
        order.reserve(tracks.size());
        for (const auto &track : tracks) {
            order.push_back(Json{{"id", track.id}, {"type", "track"}});
        }
        return write("/luna/me/playlist/media/sort", Json{{"playlist_id", std::move(id)}, {"media", std::move(order)}});
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
            if (json::text_view(json::field(item, "media_id")) == id) {
                item["played_mills"] = static_cast<int64_t>(seconds) * 1000;
                break;
            }
        }
        co_return;
    }

} // namespace lyren::qishui
