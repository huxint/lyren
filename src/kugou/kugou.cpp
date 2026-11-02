#include "kugou/kugou.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <random>
#include <unordered_set>

#include "core/crypto.hpp"
#include "core/encoding.hpp"
#include "kugou/json.hpp"

namespace lyren::kugou {

    namespace {
        using json::array;
        using json::count;
        using json::first;
        using json::get;
        using json::text;
        using json::Value;

        constexpr std::string_view square_url = "http://www2.kugou.kugou.com/yueku/v9/special/getSpecial";
        constexpr std::string_view comment_code = "fc4be23b4e972707f36b8a828a93ba8a";

        int64_t milliseconds() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string hash_id(std::string value) {
            if (value.size() != 32 || !std::ranges::all_of(value, [](unsigned char c) {
                    return std::isxdigit(c);
                })) {
                return {};
            }
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return value;
        }

        std::string lower_hash(std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        bool numeric_id(std::string_view value) {
            return !value.empty() && std::ranges::all_of(value, [](char c) {
                return c >= '0' && c <= '9';
            });
        }

        std::string joined_names(const Value &people, std::string_view field) {
            std::string result;
            for (const auto &person : array(people)) {
                auto name = text(person, field);
                if (!name.empty()) {
                    if (!result.empty()) {
                        result += " / ";
                    }
                    result += name;
                }
            }
            return result;
        }

        Track search_track(const Value &row) {
            Track track;
            track.source = Source::kugou;
            track.id = hash_id(text(row, "FileHash"));
            track.mid = text(row, "Audioid");
            track.media = text(row, "MixSongID");
            track.title = text(row, "SongName");
            track.alias = text(row, "OtherName");
            track.artist = joined_names(get(row, "Singers"), "name");
            if (auto singers = array(get(row, "Singers")); !singers.empty()) {
                track.artist_id = text(singers.front(), "id");
            }
            track.album = text(row, "AlbumName");
            track.album_id = text(row, "AlbumID");
            track.duration = static_cast<uint32_t>(count(get(row, "Duration")));
            track.cover = text(row, "Image");
            track.paid = count(get(row, "PayType")) != 0;
            return track;
        }

        Track metadata_track(const Value &row) {
            const auto &audio = get(row, "audio_info");
            const auto &album = get(row, "album_info");
            Track track;
            track.source = Source::kugou;
            track.id = hash_id(text(audio, "hash"));
            track.mid = text(audio, "audio_id");
            track.title = text(row, "songname");
            track.artist = text(row, "author_name");
            track.album_id = text(album, "album_id");
            track.album = text(album, "album_name");
            track.cover = text(album, "sizable_cover");
            track.duration = static_cast<uint32_t>(count(get(audio, "timelength")) / 1000);
            track.paid = count(get(audio, "pay_type")) != 0;
            return track;
        }

        // Index JSON rows, not copies of Track and all of its owned strings for every quality hash.
        using MetadataIndex = StringHashMap<const Value *>;

        Result<MetadataIndex> index_metadata(const Value &rows) {
            if (!rows.is_array()) {
                return std::unexpected("KuGou Music returned incomplete song details");
            }
            MetadataIndex index;
            index.reserve(rows.size() * 4);
            for (const auto &group : array(rows)) {
                const Value *row = &group;
                if (group.is_array()) {
                    if (group.empty()) {
                        continue;
                    }
                    row = &array(group).front();
                }
                const auto &audio = get(*row, "audio_info");
                const auto *title = get(*row, "songname").get_if<std::string>();
                if (!title || title->empty() || hash_id(text(audio, "hash")).empty()) {
                    continue;
                }
                for (auto field : {"hash", "hash_128", "hash_320", "hash_flac", "hash_high"}) {
                    auto hash = hash_id(text(audio, field));
                    if (!hash.empty()) {
                        index.emplace(std::move(hash), row);
                    }
                }
            }
            return index;
        }

        const Value *metadata_row(const MetadataIndex &index, const std::string &hash, bool single) {
            if (auto found = index.find(hash_id(hash)); found != index.end()) {
                return found->second;
            }
            // A retired hash can resolve to a new canonical hash. Only a single-song request can
            // establish that mapping; batch response position cannot identify the requested song.
            if (single && !index.empty()) {
                const auto *row = index.begin()->second;
                if (!text(get(*row, "audio_info"), "audio_id").empty() && std::ranges::all_of(index, [row](const auto &entry) {
                        return entry.second == row;
                    })) {
                    return row;
                }
            }
            return nullptr;
        }

        std::string page_key(std::span<const std::string> hashes) {
            std::string joined;
            joined.reserve(hashes.size() * 32);
            for (const auto &hash : hashes) {
                joined += hash;
            }
            return crypto::md5_hex(joined);
        }

        Card album_card(const Value &row) {
            return {.kind = CardKind::album,
                    .source = Source::kugou,
                    .id = text(row, "albumid"),
                    .title = text(row, "albumname"),
                    .subtitle = text(row, "singername"),
                    .cover = text(row, "imgurl"),
                    .count = count(get(row, "songcount")),
                    .count_kind = Card::Count::songs,
                    .commentable = false};
        }

        Card playlist_card(const Value &row) {
            return {.kind = CardKind::playlist,
                    .source = Source::kugou,
                    .id = text(row, "specialid"),
                    .title = text(row, "specialname"),
                    .subtitle = text(row, "nickname"),
                    .cover = text(first(row, {"img", "imgurl"})),
                    .count = count(get(row, "songcount")),
                    .count_kind = get(row, "songcount").is_null() ? Card::Count::none : Card::Count::songs,
                    .commentable = false};
        }

        Result<void> continue_page(Batch &batch, int offset, int limit, size_t raw_rows) {
            if (raw_rows == 0 && offset < batch.total) {
                return std::unexpected("KuGou Music returned an incomplete page");
            }
            // Page indices advance by the requested size, even when copyright filtering shortens a page.
            batch.next_offset = raw_rows && offset + limit < batch.total ? offset + limit : -1;
            return {};
        }

        Result<std::vector<std::string>> hashes_in(const Value &rows) {
            if (!rows.is_array()) {
                return std::unexpected("KuGou Music returned an incomplete song list");
            }
            std::vector<std::string> hashes;
            hashes.reserve(rows.size());
            for (const auto &row : array(rows)) {
                auto hash = hash_id(text(first(row, {"hash", "filehash", "file_hash", "hash_128"})));
                if (hash.empty()) {
                    return std::unexpected("KuGou Music returned an unavailable song in this list");
                }
                hashes.push_back(std::move(hash));
            }
            return hashes;
        }

        Result<CommentPage> parse_comments(std::string_view body, int page, int page_size, bool replies) {
            auto parsed = json::response(body);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            const auto &rows = get(*parsed, "list");
            if (!rows.is_array()) {
                return std::unexpected("KuGou Music returned an incomplete comment list");
            }
            CommentPage result;
            result.comments.reserve(rows.size());
            for (const auto &row : array(rows)) {
                Comment comment;
                comment.id = text(row, "id");
                comment.user = text(row, "user_name");
                comment.avatar = text(row, "user_pic");
                comment.text = text(row, "content");
                comment.time = text(row, "addtime");
                comment.reply = text(row, "pcontent");
                comment.reply_user = text(row, "puser");
                comment.likes = count(get(get(row, "like"), "likenum"));
                comment.replies = count(get(row, "reply_num"));
                if (auto images = array(get(row, "images")); !images.empty()) {
                    comment.image = text(images.front(), "url");
                }
                for (const auto &mention : array(get(row, "atlist"))) {
                    const auto tag = "[at=" + text(mention, "id") + ']';
                    const auto name = '@' + text(mention, "name");
                    for (size_t at = 0; (at = comment.text.find(tag, at)) != std::string::npos; at += name.size()) {
                        comment.text.replace(at, tag.size(), name);
                    }
                }
                if (comment.id.empty()) {
                    return std::unexpected("KuGou Music returned an incomplete comment");
                }
                result.comments.push_back(std::move(comment));
            }
            result.total = replies ? (page - 1) * page_size + static_cast<int>(result.comments.size()) : count(get(*parsed, "count"));
            result.more = replies ? result.comments.size() == static_cast<size_t>(page_size) : page * page_size < result.total;
            return result;
        }

        std::vector<std::string> lyric_headers() {
            return {"KG-RC: 1", "KG-THash: expand_search_manager.cpp:852736169:451", "User-Agent: KuGou2012-9020-ExpandSearchManager"};
        }
    } // namespace

    Result<Batch> parse_search(std::string_view body, SearchKind kind, int offset, int limit) {
        auto parsed = json::response(body);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const auto &data = json::data(*parsed);
        const auto &rows = get(data, kind == SearchKind::tracks ? "lists" : "info");
        if (!rows.is_array()) {
            return std::unexpected("KuGou Music returned incomplete search results");
        }
        Batch result;
        result.total = count(get(data, "total"));
        if (kind == SearchKind::tracks) {
            result.tracks.reserve(rows.size());
        } else {
            result.cards.reserve(rows.size());
        }
        for (const auto &row : array(rows)) {
            if (kind == SearchKind::tracks) {
                auto track = search_track(row);
                if (track.id.empty() || track.title.empty()) {
                    return std::unexpected("KuGou Music returned incomplete song details");
                }
                result.tracks.push_back(std::move(track));
            } else {
                auto card = kind == SearchKind::albums ? album_card(row) : playlist_card(row);
                if (card.id.empty() || card.title.empty()) {
                    return std::unexpected("KuGou Music returned incomplete search results");
                }
                result.cards.push_back(std::move(card));
            }
        }
        if (auto next = continue_page(result, offset, limit, rows.size()); !next) {
            return std::unexpected(next.error());
        }
        return result;
    }

    Result<std::vector<Track>> parse_metadata(std::string_view body, std::span<const std::string> hashes) {
        auto parsed = json::response(body);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto index = index_metadata(json::data(*parsed));
        if (!index) {
            return std::unexpected(index.error());
        }
        std::vector<Track> result;
        result.reserve(hashes.size());
        for (const auto &hash : hashes) {
            const auto *row = metadata_row(*index, hash, hashes.size() == 1);
            if (!row) {
                return std::unexpected("KuGou Music could not match the requested song details");
            }
            result.push_back(metadata_track(*row));
        }
        return result;
    }

    Result<Stream> parse_stream(std::string_view body, bool preview, std::string_view quality) {
        auto parsed = json::response(body, true);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const auto urls = array(get(*parsed, "url"));
        if (urls.empty()) {
            return std::unexpected("This song is unavailable from KuGou Music");
        }
        const auto url = text(urls.front());
        if (!url.starts_with("http://") && !url.starts_with("https://")) {
            return std::unexpected("KuGou Music returned an invalid audio URL");
        }
        const auto extension = text(*parsed, "extName");
        if (extension != "mp3" && extension != "flac" && extension != "ogg" && extension != "m4a") {
            return std::unexpected("KuGou Music returned an unsupported audio format");
        }
        Stream result;
        result.url = url;
        result.level = quality == "high" ? "hires" : quality == "flac" ? "lossless" : quality == "320" ? "exhigh" : "standard";
        result.trial = preview;
        if (preview) {
            result.trial_start = std::max(0.0, json::number(get(get(*parsed, "hash_offset"), "start_ms"))) / 1000.0;
        }
        result.gain = static_cast<float>(json::number(get(*parsed, "volume_gain")));
        result.peak = static_cast<float>(std::max(0.0, json::number(get(*parsed, "volume_peak"))));
        return result;
    }

    Kugou::Kugou(Loop &loop, net::Http &http)
    : client(loop, http) {
        capabilities.search = {true, true, false, true};
        capabilities.toplists = true;
        capabilities.square = true;
        capabilities.artist_intro = true;
        capabilities.library = true;
        capabilities.comments = {true, false, false};
        capabilities.comment_sort = true;
        capabilities.comment_replies = true;
        capabilities.qr_login = true;
        features.daily.songs = [this] {
            return daily_songs();
        };
    }

    Task<Result<void>> Kugou::initialize() {
        if (client.get("mid").empty()) {
            client.session["mid"] = crypto::random_hex(32);
        }
        if (client.get("stream_mid").empty()) {
            std::mt19937_64 rng{std::random_device{}()};
            client.session["stream_mid"] = std::to_string(rng());
        }
        co_return Result<void>{};
    }

    Task<Result<Account>> Kugou::account() {
        if (!logged_in()) {
            co_return std::unexpected("Sign in to KuGou Music and try again");
        }
        auto lists = co_await account_playlists();
        if (!lists) {
            co_return std::unexpected(lists.error());
        }
        co_return Account{client.get("userid"), client.get("nickname"), client.get("avatar")};
    }

    Task<void> Kugou::logout() {
        cancel_login();
        ++client.session_gen;
        for (auto name : {"userid", "token", "nickname", "avatar"}) {
            client.session.erase(name);
        }
        if (client.on_session) {
            client.on_session();
        }
        co_return;
    }

    void Kugou::cancel_login() {
        ++qr_gen_;
        qr_key_.clear();
    }

    Task<Result<QrCode>> Kugou::qr_start() {
        cancel_login();
        const auto generation = qr_gen_;
        qr_mid_ = crypto::random_hex(32);
        auto query = client.common(qr_mid_);
        query.insert({{"type", "1"}, {"plat", "4"}, {"srcappid", "2919"}, {"qrcode_txt", "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&"}});
        auto reply = co_await client.send(request("https://login-user.kugou.com/v2/qrcode", query, {}, login_salt, client.headers(query)), net::Retry::never);
        if (generation != qr_gen_) {
            co_return std::unexpected("Sign-in cancelled");
        }
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply, true);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &data = get(*parsed, "data");
        qr_key_ = text(data, "qrcode");
        const auto image = text(data, "qrcode_img");
        constexpr std::string_view prefix = "data:image/png;base64,";
        if (qr_key_.empty() || !image.starts_with(prefix)) {
            co_return std::unexpected("KuGou Music returned an invalid sign-in code");
        }
        auto bytes = encoding::unbase64(std::string_view(image).substr(prefix.size()));
        if (bytes.empty()) {
            co_return std::unexpected("KuGou Music returned an invalid sign-in code");
        }
        qr_expires_ = Clock::now() + 120s;
        co_return QrCode{.image = std::move(bytes)};
    }

    Task<Result<QrStatus>> Kugou::qr_poll() {
        if (qr_key_.empty() || Clock::now() >= qr_expires_) {
            co_return QrStatus{.state = QrState::expired};
        }
        const auto generation = qr_gen_;
        auto query = client.common(qr_mid_);
        query.insert({{"plat", "4"}, {"srcappid", "2919"}, {"qrcode", qr_key_}});
        auto reply = co_await client.send(request("https://login-user.kugou.com/v2/get_userinfo_qrcode", query, {}, login_salt, client.headers(query)));
        if (generation != qr_gen_) {
            co_return std::unexpected("Sign-in cancelled");
        }
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply, true);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &data = get(*parsed, "data");
        QrStatus status;
        status.nickname = text(data, "nickname");
        status.avatar = text(first(data, {"pic", "picurl"}));
        switch (count(get(data, "status"), -1)) {
            case 0:
                status.state = QrState::expired;
                break;
            case 1:
                status.state = QrState::waiting;
                break;
            case 2:
                status.state = QrState::scanned;
                break;
            case 4: {
                auto uid = text(data, "userid"), token = text(data, "token");
                if (!numeric_id(uid) || token.empty()) {
                    co_return std::unexpected("KuGou Music returned an incomplete account session");
                }
                client.session["userid"] = std::move(uid);
                client.session["token"] = std::move(token);
                client.session["mid"] = qr_mid_;
                client.session["nickname"] = status.nickname;
                client.session["avatar"] = status.avatar;
                ++client.session_gen;
                status.state = QrState::confirmed;
                if (client.on_session) {
                    client.on_session();
                }
                cancel_login();
                break;
            }
            default:
                co_return std::unexpected("KuGou Music returned an unknown sign-in state");
        }
        co_return status;
    }

    Task<Result<Batch>> Kugou::search(std::string query, SearchKind kind, int offset, int limit) {
        if (kind == SearchKind::artists) {
            co_return std::unexpected("KuGou Music artist search is unavailable");
        }
        Params params{{"keyword", std::move(query)}, {"page", std::to_string(offset / limit + 1)}, {"pagesize", std::to_string(limit)}};
        std::string url;
        if (kind == SearchKind::tracks) {
            url = "https://songsearch.kugou.com/song_search_v2";
            params.insert({{"userid", "0"}, {"clientver", ""}, {"platform", "WebFilter"}, {"filter", "2"}, {"iscorrection", "1"}, {"privilege_filter", "0"}, {"area_code", "1"}});
        } else if (kind == SearchKind::albums) {
            url = "http://mobilecdn.kugou.com/api/v3/search/album";
        } else {
            url = "http://msearchretry.kugou.com/api/v3/search/special";
            params.insert({{"showtype", "10"}, {"filter", "0"}, {"version", "7910"}, {"sver", "2"}});
        }
        auto reply = co_await client.send(request(std::move(url), std::move(params)));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        co_return parse_search(*reply, kind, offset, limit);
    }

    Task<Result<std::vector<Track>>> Kugou::metadata(std::vector<std::string> hashes) {
        std::vector<Track> result;
        result.reserve(hashes.size());
        for (size_t from = 0; from < hashes.size(); from += 100) {
            auto batch = std::span(hashes).subspan(from, std::min<size_t>(100, hashes.size() - from));
            Value::array_t resources;
            resources.reserve(batch.size());
            for (const auto &hash : batch) {
                resources.emplace_back(Value{{"hash", hash}});
            }
            Value body{{"data", std::move(resources)},
                       {"area_code", "1"},
                       {"show_privilege", 1},
                       {"show_album_info", "1"},
                       {"is_publish", ""},
                       {"appid", 1005},
                       {"clientver", 11451},
                       {"mid", client.get("mid")},
                       {"dfid", "-"},
                       {"clienttime", milliseconds()},
                       {"key", std::string(android_salt)},
                       {"fields", "album_info,author_name,audio_info,ori_audio_name,base,songname,classification"}};
            auto reply = co_await client.send(request("http://gateway.kugou.com/v3/album_audio/audio", {}, json::write(body), {},
                                                      {"x-router: kmr.service.kugou.com", "KG-THash: 13a3164", "KG-RC: 1", "KG-Fake: 0", "KG-RF: 00869891",
                                                       "User-Agent: Android712-AndroidPhone-11451-376-0-FeeCacheUpdate-wifi"}));
            if (!reply) {
                co_return std::unexpected(reply.error());
            }
            auto parsed = json::response(*reply);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            auto index = index_metadata(json::data(*parsed));
            if (!index) {
                co_return std::unexpected(index.error());
            }
            for (const auto &hash : batch) {
                if (const auto *row = metadata_row(*index, hash, batch.size() == 1)) {
                    result.push_back(metadata_track(*row));
                } else {
                    if (batch.size() == 1) {
                        co_return std::unexpected("KuGou Music could not match the requested song details");
                    }
                    auto song = co_await song_detail(hash);
                    if (!song) {
                        co_return std::unexpected(song.error());
                    }
                    result.push_back(std::move(*song));
                }
            }
        }
        co_return result;
    }

    Task<Result<Track>> Kugou::song_detail(std::string hash) {
        hash = hash_id(std::move(hash));
        if (hash.empty()) {
            co_return std::unexpected("Invalid KuGou Music song hash");
        }
        std::vector<std::string> hashes{std::move(hash)};
        auto result = co_await metadata(std::move(hashes));
        if (!result) {
            co_return std::unexpected(result.error());
        }
        co_return std::move(result->front());
    }

    Task<Result<Stream>> Kugou::stream(Track track, Quality quality) {
        auto query = client.common(client.get("stream_mid"));
        if (logged_in()) {
            query.insert({{"userid", client.get("userid")}, {"token", client.get("token")}});
        }
        uint64_t album_id = 0;
        std::from_chars(track.album_id.data(), track.album_id.data() + track.album_id.size(), album_id);
        Value body{{"appid", 1005},
                   {"area_code", "1"},
                   {"behavior", "play"},
                   {"clientver", 20489},
                   {"need_hash_offset", 1},
                   {"relate", 1},
                   {"support_verify", 1},
                   {"resource", Value::array_t{Value{{"type", "audio"}, {"page_id", 0}, {"hash", lower_hash(track.id)}, {"album_id", album_id}}}},
                   {"qualities", Value::array_t{"128", "320", "flac", "high"}}};
        auto resources = co_await client.send(
            request("https://gateway.kugou.com/v2/get_res_privilege/lite", query, json::write(body), android_salt, client.headers(query, "media.store.kugou.com")));
        if (!resources) {
            co_return std::unexpected(resources.error());
        }
        auto parsed = json::response(*resources);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto rows = array(get(*parsed, "data"));
        if (rows.empty()) {
            co_return std::unexpected("This song is unavailable from KuGou Music");
        }
        const auto &resource = rows.front();
        constexpr std::array<std::string_view, 4> qualities{"128", "320", "flac", "high"};
        const size_t target = quality == Quality::hires ? 3 : quality == Quality::lossless ? 2 : quality >= Quality::higher ? 1 : 0;
        std::string hash = lower_hash(track.id);
        size_t selected = 0;
        for (const auto &file : array(get(resource, "relate_goods"))) {
            const auto name = text(file, "quality");
            const auto found = std::ranges::find(qualities, name);
            const auto rank = static_cast<size_t>(found - qualities.begin());
            if (rank <= target && rank >= selected && !hash_id(text(file, "hash")).empty()) {
                hash = lower_hash(text(file, "hash"));
                selected = rank;
            }
        }
        const auto granted = qualities[selected];
        query["clientver"] = "11430";
        query.insert({{"hash", hash},
                      {"album_id", text(resource, "album_id")},
                      {"album_audio_id", text(resource, "album_audio_id")},
                      {"quality", std::string(granted)},
                      {"area_code", "1"},
                      {"ssa_flag", "is_fromtrack"},
                      {"version", "11430"},
                      {"page_id", "151369488"},
                      {"behavior", "play"},
                      {"pid", "2"},
                      {"cmd", "26"},
                      {"pidversion", "3001"},
                      {"IsFreePart", "0"},
                      {"cdnBackup", "1"},
                      {"module", ""}});
        query["key"] = crypto::md5_hex(hash + "57ae12eb6890223e355ccfcb74edf70d" + query.at("appid") + query.at("mid") + (logged_in() ? client.get("userid") : "0"));
        std::string url = "https://gateway.kugou.com/v5/url";
        std::string_view router = "trackercdn.kugou.com";
        if (logged_in()) {
            auto auth_query = client.common(client.get("stream_mid"));
            auth_query.insert({{"userid", client.get("userid")}, {"token", client.get("token")}, {"module_id", "51"}});
            auto user_reply =
                co_await client.send(request("http://gateway.kugou.com/v1/user_verify", auth_query, {}, android_salt, client.headers(auth_query, "trackercdngz.kugou.com")));
            if (!user_reply) {
                co_return std::unexpected(user_reply.error());
            }
            auto user_auth = json::response(*user_reply, true);
            if (!user_auth) {
                co_return std::unexpected(user_auth.error());
            }
            auto authorization = text(get(*user_auth, "data"), "auth");
            if (authorization.empty()) {
                co_return std::unexpected("KuGou Music returned no streaming authorization");
            }
            auth_query.insert({{"authorization", std::move(authorization)}, {"hash", hash}, {"album_audio_id", query.at("album_audio_id")}});
            auth_query["clientver"] = "11561";
            auto track_reply =
                co_await client.send(request("http://gateway.kugou.com/v1/authorization", auth_query, {}, android_salt, client.headers(auth_query, "trackercdngz.kugou.com")));
            if (!track_reply) {
                co_return std::unexpected(track_reply.error());
            }
            auto track_auth = json::response(*track_reply);
            if (!track_auth) {
                co_return std::unexpected(track_auth.error());
            }
            const auto &data = get(*track_auth, "data");
            const auto auth = text(data, "auth"), open_time = text(data, "open_time");
            if (auth.empty() || open_time.empty()) {
                co_return std::unexpected("KuGou Music returned incomplete song authorization");
            }
            query.erase("IsFreePart");
            query["clientver"] = "11561";
            query.insert({{"auth", auth}, {"open_time", open_time}, {"need_m", "0"}, {"ptype", "0"}, {"need_ogg", "1"}, {"mtype", "0"}, {"module_id", "51"}});
            url = "https://gateway.kugou.com/tracker/v5/url";
            router = {};
        }
        auto reply = co_await client.send(request(std::move(url), query, {}, android_salt, client.headers(query, router)));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto full = json::parse(*reply);
        if (!full) {
            co_return std::unexpected(full.error());
        }
        if (json::number(get(*full, "status")) != 2) {
            co_return parse_stream(*reply, false, granted);
        }
        // Observed KG-39 responses use status=2 for a denied full track. Only request a preview
        // when the service actually supplies its playable interval; this does not grant full playback.
        const auto &offset = get(*full, "hash_offset").is_null() ? get(get(resource, "trans_param"), "hash_offset") : get(*full, "hash_offset");
        if (json::number(get(offset, "end_ms")) <= json::number(get(offset, "start_ms"))) {
            co_return std::unexpected("This song is unavailable from KuGou Music");
        }
        for (auto name : {"auth", "open_time", "need_m", "ptype", "need_ogg", "mtype", "module_id"}) {
            query.erase(name);
        }
        query["clientver"] = "11430";
        query["hash"] = lower_hash(track.id);
        query["quality"] = "128";
        query["IsFreePart"] = "1";
        query["key"] = crypto::md5_hex(query.at("hash") + "57ae12eb6890223e355ccfcb74edf70d" + query.at("appid") + query.at("mid") + (logged_in() ? client.get("userid") : "0"));
        auto preview = co_await client.send(request("https://gateway.kugou.com/v5/url", query, {}, android_salt, client.headers(query, "trackercdn.kugou.com")));
        if (!preview) {
            co_return std::unexpected(preview.error());
        }
        co_return parse_stream(*preview, true, "128");
    }

    Task<Result<std::vector<LyricLine>>> Kugou::lyrics(Track track) {
        Params query{{"ver", "1"}, {"man", "yes"}, {"client", "pc"}, {"keyword", track.title}, {"hash", track.id}, {"timelength", std::to_string(track.duration)}, {"lrctxt", "1"}};
        auto reply = co_await client.send(request("http://lyrics.kugou.com/search", std::move(query), {}, {}, lyric_headers()));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::parse(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto candidates = array(get(*parsed, "candidates"));
        if (candidates.empty()) {
            co_return std::vector<LyricLine>{};
        }
        const auto &candidate = candidates.front();
        const auto format = count(get(candidate, "krctype")) == 1 && count(get(candidate, "contenttype")) != 1 ? "krc" : "lrc";
        Params download{{"ver", "1"}, {"client", "pc"}, {"id", text(candidate, "id")}, {"accesskey", text(candidate, "accesskey")}, {"fmt", format}, {"charset", "utf8"}};
        if (download.at("id").empty() || download.at("accesskey").empty()) {
            co_return std::unexpected("KuGou Music returned an invalid lyric candidate");
        }
        auto content = co_await client.send(request("http://lyrics.kugou.com/download", std::move(download), {}, {}, lyric_headers()));
        if (!content) {
            co_return std::unexpected(content.error());
        }
        co_return parse_lyric_download(*content);
    }

    Task<Result<Batch>> Kugou::song_page(std::string url, Params query, int offset, int limit, std::string album_id) {
        query["page"] = std::to_string(offset / limit + 1);
        query["pagesize"] = std::to_string(limit);
        auto reply = co_await client.send(request(std::move(url), std::move(query)));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &data = json::data(*parsed);
        const auto &rows = get(data, "info");
        auto hashes = hashes_in(rows);
        if (!hashes) {
            co_return std::unexpected(hashes.error());
        }
        Batch batch;
        batch.total = count(get(data, "total"));
        if (auto next = continue_page(batch, offset, limit, rows.size()); !next) {
            co_return std::unexpected(next.error());
        }
        auto details = co_await metadata(std::move(*hashes));
        if (!details) {
            co_return std::unexpected(details.error());
        }
        batch.tracks = std::move(*details);
        for (size_t i = 0; i < batch.tracks.size(); ++i) {
            const auto &row = array(rows)[i];
            auto &track = batch.tracks[i];
            track.media = text(row, "album_audio_id");
            if (!album_id.empty()) {
                track.album_id = album_id;
            }
            const auto authors = array(get(row, "authors"));
            if (!authors.empty()) {
                track.artist_id = text(authors.front(), "author_id");
            }
        }
        co_return batch;
    }

    Task<Result<std::vector<Track>>> Kugou::tracks(Card card) {
        if (card.kind == CardKind::artist) {
            auto songs = co_await artist_songs(card.id, 0);
            if (!songs) {
                co_return std::unexpected(songs.error());
            }
            co_return std::move(songs->tracks);
        }
        if (card.kind == CardKind::album || card.id.starts_with("top:")) {
            const bool is_album = card.kind == CardKind::album;
            std::string url = is_album ? "http://mobiles.kugou.com/api/v3/album/song" : "http://mobilecdnbj.kugou.com/api/v3/rank/song";
            Params query{{"version", "9108"}, {"plat", "0"}, {"area_code", is_album ? "0" : "1"}, {"with_res_tag", "0"}};
            if (is_album) {
                query["albumid"] = card.id;
            } else {
                query.insert({{"rankid", card.id.substr(4)}, {"ranktype", "1"}, {"show_portrait_mv", "1"}});
            }
            std::vector<Track> result;
            int offset = 0;
            do {
                auto page = co_await song_page(url, query, offset, 100, is_album ? card.id : "");
                if (!page) {
                    co_return std::unexpected(page.error());
                }
                result.insert(result.end(), std::make_move_iterator(page->tracks.begin()), std::make_move_iterator(page->tracks.end()));
                offset = page->next_offset;
            } while (offset >= 0);
            co_return result;
        }
        if (card.id.starts_with("account:")) {
            if (!logged_in()) {
                co_return std::unexpected("Sign in to KuGou Music and try again");
            }
            std::vector<std::string> hashes;
            std::unordered_set<std::string> seen_pages;
            int received = 0, total = -1;
            for (int page = 1;; ++page) {
                auto query = client.common();
                query.insert({{"userid", client.get("userid")}, {"token", client.get("token")}});
                Value body{{"listid", card.id.substr(8)},
                           {"userid", client.get("userid")},
                           {"token", client.get("token")},
                           {"type", 0},
                           {"page", page},
                           {"pagesize", 300},
                           {"area_code", 1},
                           {"allplatform", 1},
                           {"show_cover", 1}};
                auto reply = co_await client.send(
                    request("https://gateway.kugou.com/v4/get_list_all_file_v3", query, json::write(body), android_salt, client.headers(query, "cloudlist.service.kugou.com")));
                if (!reply) {
                    co_return std::unexpected(reply.error());
                }
                auto parsed = json::response(*reply, true);
                if (!parsed) {
                    co_return std::unexpected(parsed.error());
                }
                const auto &data = get(*parsed, "data");
                const auto &rows = first(data, {"info", "list"});
                auto batch = hashes_in(rows);
                if (!batch) {
                    co_return std::unexpected(batch.error());
                }
                const int current_total = count(first(data, {"count", "total"}), -1);
                if (page == 1) {
                    total = current_total;
                }
                if (total != current_total || (!batch->empty() && !seen_pages.insert(page_key(*batch)).second)) {
                    co_return std::unexpected("KuGou Music playlist changed while loading; refresh it");
                }
                received += static_cast<int>(rows.size());
                if (total >= 0 && (received > total || (rows.empty() && received < total))) {
                    co_return std::unexpected("KuGou Music returned an incomplete playlist");
                }
                hashes.insert(hashes.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
                if ((total >= 0 && received == total) || (total < 0 && rows.size() < 300)) {
                    break;
                }
            }
            auto details = co_await metadata(std::move(hashes));
            co_return details;
        }
        if (!numeric_id(card.id)) {
            co_return std::unexpected("Invalid KuGou Music playlist ID");
        }
        std::vector<std::string> hashes;
        std::unordered_set<std::string> seen_pages;
        int total = -1;
        for (int page = 1;; ++page) {
            Params query{{"srcappid", "2919"},   {"clientver", "20000"},         {"appid", "1058"},   {"type", "0"},
                         {"module", "playlist"}, {"page", std::to_string(page)}, {"pagesize", "100"}, {"specialid", card.id}};
            auto reply = co_await client.send(
                request("https://pubsongscdn.kugou.com/v2/get_other_list_file", std::move(query), {}, login_salt,
                        {"Referer: https://m3ws.kugou.com/share/index.php", "dfid: -",
                         "User-Agent: Mozilla/5.0 (iPhone; CPU iPhone OS 11_0 like Mac OS X) AppleWebKit/604.1.38 (KHTML, like Gecko) Version/11.0 Mobile/15A372 Safari/604.1"}));
            if (!reply) {
                co_return std::unexpected(reply.error());
            }
            auto parsed = json::response(*reply);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            const auto &data = json::data(*parsed), &rows = get(data, "info");
            auto batch = hashes_in(rows);
            if (!batch) {
                co_return std::unexpected(batch.error());
            }
            const int current_total = count(get(data, "count"), -1);
            if (page == 1) {
                total = current_total;
            }
            if (total < 0 || total != current_total || (!batch->empty() && !seen_pages.insert(page_key(*batch)).second)) {
                co_return std::unexpected("KuGou Music returned inconsistent playlist pagination");
            }
            hashes.insert(hashes.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
            if (hashes.size() > static_cast<size_t>(total) || (rows.empty() && hashes.size() < static_cast<size_t>(total))) {
                co_return std::unexpected("KuGou Music returned an incomplete public playlist");
            }
            if (hashes.size() == static_cast<size_t>(total)) {
                break;
            }
        }
        co_return co_await metadata(std::move(hashes));
    }

    Task<Result<std::vector<Card>>> Kugou::recommended_playlists() {
        auto page = co_await square({}, 0);
        if (!page) {
            co_return std::unexpected(page.error());
        }
        co_return std::move(page->cards);
    }

    Task<Result<std::vector<Card>>> Kugou::toplists() {
        Params query{{"version", "9108"}, {"plat", "0"}, {"showtype", "2"}, {"parentid", "0"}, {"apiver", "6"}, {"area_code", "1"}, {"withsong", "1"}};
        auto reply = co_await client.send(request("http://mobilecdnbj.kugou.com/api/v5/rank/list", std::move(query)));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &rows = get(json::data(*parsed), "info");
        if (!rows.is_array()) {
            co_return std::unexpected("KuGou Music returned an incomplete chart list");
        }
        std::vector<Card> cards;
        for (const auto &row : array(rows)) {
            if (count(get(row, "isvol")) != 1) {
                continue;
            }
            auto id = text(row, "rankid");
            if (id.empty()) {
                co_return std::unexpected("KuGou Music returned an incomplete chart list");
            }
            cards.push_back({.source = Source::kugou,
                             .id = "top:" + id,
                             .title = text(row, "rankname"),
                             .subtitle = text(row, "update_frequency"),
                             .cover = text(first(row, {"imgurl", "img_cover"})),
                             .commentable = false});
        }
        co_return cards;
    }

    Task<Result<Categories>> Kugou::categories() {
        auto reply = co_await client.send(request(std::string(square_url), {{"is_smarty", "1"}}));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &data = get(*parsed, "data");
        Categories result;
        const auto &hot = get(data, "hotTag");
        if (count(get(hot, "status")) == 1) {
            if (const auto *tags = get(hot, "data").get_if<Value::object_t>()) {
                for (const auto &[key, tag] : *tags) {
                    result.hot.push_back({text(tag, "special_id"), text(tag, "special_name")});
                }
            }
        }
        const auto *groups = get(data, "tagids").get_if<Value::object_t>();
        if (!groups) {
            co_return std::unexpected("KuGou Music returned incomplete playlist categories");
        }
        for (const auto &[name, group] : *groups) {
            std::vector<Category> tags;
            for (const auto &tag : array(get(group, "data"))) {
                tags.push_back({text(tag, "id"), text(tag, "name")});
            }
            result.groups.emplace_back(name, std::move(tags));
        }
        co_return result;
    }

    Task<Result<Batch>> Kugou::square(std::string category, int offset) {
        Params query{{"is_smarty", "1"}};
        if (!category.empty()) {
            query.insert({{"cdn", "cdn"}, {"t", "5"}, {"c", category}});
        }
        auto info = co_await client.send(request(std::string(square_url), std::move(query)));
        if (!info) {
            co_return std::unexpected(info.error());
        }
        auto parsed_info = json::response(*info);
        if (!parsed_info) {
            co_return std::unexpected(parsed_info.error());
        }
        const auto &params = get(get(*parsed_info, "data"), "params");
        const auto page_size = count(get(params, "pagesize"));
        if (!page_size) {
            co_return std::unexpected("KuGou Music returned invalid playlist pagination");
        }
        auto reply =
            co_await client.send(request(std::string(square_url), {{"is_ajax", "1"}, {"cdn", "cdn"}, {"t", "5"}, {"c", category}, {"p", std::to_string(offset / page_size + 1)}}));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &rows = get(*parsed, "special_db");
        if (!rows.is_array()) {
            co_return std::unexpected("KuGou Music returned an incomplete playlist page");
        }
        Batch result;
        result.total = count(get(params, "total"));
        for (const auto &row : array(rows)) {
            auto card = playlist_card(row);
            if (card.id.empty() || card.title.empty()) {
                co_return std::unexpected("KuGou Music returned incomplete playlist details");
            }
            result.cards.push_back(std::move(card));
        }
        if (auto next = continue_page(result, offset, page_size, rows.size()); !next) {
            co_return std::unexpected(next.error());
        }
        co_return result;
    }

    Task<Result<AlbumInfo>> Kugou::album(std::string id) {
        auto songs = co_await tracks({.kind = CardKind::album, .source = Source::kugou, .id = std::move(id)});
        if (!songs) {
            co_return std::unexpected(songs.error());
        }
        AlbumInfo result;
        if (!songs->empty()) {
            result.artist = songs->front().artist;
            result.artist_id = songs->front().artist_id;
        }
        result.tracks = std::move(*songs);
        co_return result;
    }

    Task<Result<ArtistInfo>> Kugou::artist(std::string id) {
        auto reply = co_await client.send(request("http://mobiles.kugou.com/api/v5/singer/info", {{"singerid", std::move(id)}}));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &data = json::data(*parsed);
        auto name = text(data, "singername");
        if (name.empty()) {
            co_return std::unexpected("KuGou Music returned incomplete artist details");
        }
        co_return ArtistInfo{.name = std::move(name), .avatar = text(data, "imgurl"), .songs = count(get(data, "songcount")), .albums = count(get(data, "albumcount"))};
    }

    Task<Result<Batch>> Kugou::artist_songs(std::string id, int offset) {
        auto page = co_await song_page("http://mobiles.kugou.com/api/v5/singer/song", {{"singerid", id}}, offset, 100);
        if (!page) {
            co_return page;
        }
        for (auto &track : page->tracks) {
            track.artist_id = id;
        }
        co_return page;
    }

    Task<Result<Batch>> Kugou::artist_albums(std::string id, int offset) {
        constexpr int limit = 30;
        auto reply = co_await client.send(request("http://mobiles.kugou.com/api/v5/singer/album",
                                                  {{"singerid", std::move(id)}, {"page", std::to_string(offset / limit + 1)}, {"pagesize", std::to_string(limit)}}));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        const auto &data = json::data(*parsed), &rows = get(data, "info");
        if (!rows.is_array()) {
            co_return std::unexpected("KuGou Music returned an incomplete album list");
        }
        Batch result;
        result.total = count(get(data, "total"));
        for (const auto &row : array(rows)) {
            result.cards.push_back(album_card(row));
        }
        if (auto next = continue_page(result, offset, limit, rows.size()); !next) {
            co_return std::unexpected(next.error());
        }
        co_return result;
    }

    Task<Result<std::vector<std::pair<std::string, std::string>>>> Kugou::artist_intro(std::string id) {
        auto reply = co_await client.send(request("http://mobiles.kugou.com/api/v5/singer/info", {{"singerid", std::move(id)}}));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        co_return std::vector<std::pair<std::string, std::string>>{{"", text(json::data(*parsed), "intro")}};
    }

    Task<Result<CommentPage>> Kugou::comments(Subject subject, std::string id, bool hot, int page, std::string) {
        if (subject != Subject::song) {
            co_return std::unexpected("KuGou Music comments are available for songs only");
        }
        Params query{{"dfid", "0"},
                     {"mid", client.get("mid")},
                     {"clienttime", std::to_string(milliseconds())},
                     {"uuid", "0"},
                     {"extdata", id},
                     {"appid", "1005"},
                     {"code", std::string(comment_code)},
                     {"schash", id},
                     {"clientver", "11409"},
                     {"p", std::to_string(page)},
                     {"clienttoken", ""},
                     {"pagesize", "20"},
                     {"ver", "10"},
                     {"kugouid", "0"}};
        auto reply =
            co_await client.send(request(std::string("http://m.comment.service.kugou.com/r/v1/rank/") + (hot ? "topliked" : "newest"), std::move(query), {}, android_salt));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        co_return parse_comments(*reply, page, 20, false);
    }

    Task<Result<CommentPage>> Kugou::comment_replies(Subject subject, std::string id, std::string root, int page, std::string) {
        if (subject != Subject::song) {
            co_return std::unexpected("KuGou Music comments are available for songs only");
        }
        auto track = co_await song_detail(std::move(id));
        if (!track) {
            co_return std::unexpected(track.error());
        }
        if (track->mid.empty()) {
            co_return std::unexpected("KuGou Music returned no audio ID for this comment thread");
        }
        Params query{{"r", "commentsv2/getReplyWithLike"},
                     {"code", std::string(comment_code)},
                     {"p", std::to_string(page)},
                     {"pagesize", "100"},
                     {"ver", "1.01"},
                     {"clientver", "8373"},
                     {"kugouid", "0"},
                     {"need_show_image", "1"},
                     {"appid", "1001"},
                     {"childrenid", track->mid},
                     {"tid", std::move(root)}};
        auto reply =
            co_await client.send(request("http://comment.service.kugou.com/index.php", std::move(query), {}, {}, {"User-Agent: Android712-AndroidPhone-8983-18-0-COMMENT-wifi"}));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        co_return parse_comments(*reply, page, 100, true);
    }

    Task<Result<std::vector<Card>>> Kugou::account_playlists() {
        if (!logged_in()) {
            co_return std::unexpected("Sign in to KuGou Music and try again");
        }
        std::vector<Card> result;
        std::unordered_set<std::string> ids;
        int total = -1;
        for (int page = 1;; ++page) {
            auto query = client.common();
            query.insert({{"plat", "1"}, {"userid", client.get("userid")}, {"token", client.get("token")}});
            Value body{{"userid", client.get("userid")}, {"token", client.get("token")}, {"total_ver", 979}, {"type", 2}, {"page", page}, {"pagesize", 100}};
            auto reply = co_await client.send(
                request("https://gateway.kugou.com/v7/get_all_list", query, json::write(body), android_salt, client.headers(query, "cloudlist.service.kugou.com")));
            if (!reply) {
                co_return std::unexpected(reply.error());
            }
            auto parsed = json::response(*reply, true);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            const auto &data = get(*parsed, "data"), &rows = first(data, {"info", "list"});
            if (!rows.is_array()) {
                co_return std::unexpected("KuGou Music returned an incomplete account library");
            }
            const int current_total = count(first(data, {"total", "count"}), -1);
            if (page == 1) {
                total = current_total;
            }
            if (total != current_total) {
                co_return std::unexpected("KuGou Music library changed while loading; refresh it");
            }
            for (const auto &row : array(rows)) {
                auto id = text(first(row, {"listid", "list_id"}));
                if (id.empty() || !ids.insert(id).second) {
                    co_return std::unexpected("KuGou Music returned an incomplete account library");
                }
                const auto owner = text(row, "list_create_userid");
                const bool owned = count(get(row, "is_collect")) != 1 && (owner.empty() || owner == client.get("userid"));
                result.push_back({.source = Source::kugou,
                                  .id = "account:" + id,
                                  .title = text(first(row, {"name", "listname"})),
                                  .cover = text(first(row, {"pic", "img", "picurl"})),
                                  .count = count(first(row, {"count", "songcount", "filecount"})),
                                  .count_kind = Card::Count::songs,
                                  .owned = owned,
                                  .commentable = false});
            }
            if (total >= 0 && (result.size() > static_cast<size_t>(total) || (rows.empty() && result.size() < static_cast<size_t>(total)))) {
                co_return std::unexpected("KuGou Music returned an incomplete account library");
            }
            if ((total >= 0 && result.size() == static_cast<size_t>(total)) || (total < 0 && rows.size() < 100)) {
                break;
            }
        }
        co_return result;
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Kugou::user_playlists(std::string) {
        auto playlists = co_await account_playlists();
        if (!playlists) {
            co_return std::unexpected(playlists.error());
        }
        // No documented fixed listid or standalone "liked songs" endpoint exists.
        co_return std::pair<Card, std::vector<Card>>{Card{.source = Source::kugou, .commentable = false}, std::move(*playlists)};
    }

    Task<Result<std::vector<Card>>> Kugou::collected_playlists() {
        auto playlists = co_await account_playlists();
        if (!playlists) {
            co_return std::unexpected(playlists.error());
        }
        std::erase_if(*playlists, [](const Card &card) {
            return card.owned;
        });
        co_return playlists;
    }

    Task<Result<std::vector<Track>>> Kugou::daily_songs() {
        if (!logged_in()) {
            co_return std::unexpected("Sign in to KuGou Music and try again");
        }
        auto query = client.common();
        query.insert({{"userid", client.get("userid")}, {"token", client.get("token")}});
        Value body{{"platform", "android"}, {"userid", client.get("userid")}};
        auto reply = co_await client.send(
            request("https://gateway.kugou.com/everyday_song_recommend", query, json::write(body), android_salt, client.headers(query, "everydayrec.service.kugou.com")));
        if (!reply) {
            co_return std::unexpected(reply.error());
        }
        auto parsed = json::response(*reply, true);
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        auto hashes = hashes_in(get(get(*parsed, "data"), "song_list"));
        if (!hashes) {
            co_return std::unexpected(hashes.error());
        }
        co_return co_await metadata(std::move(*hashes));
    }

} // namespace lyren::kugou
