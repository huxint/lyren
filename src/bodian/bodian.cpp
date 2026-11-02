#include "bodian/bodian.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <variant>

#include "core/encoding.hpp"
#include "core/result_task.hpp"

namespace lyren::bodian {

    namespace {
        using Str = std::optional<std::string>;
        using Number = std::variant<int64_t, std::string>;
        template <class T>
        using Vec = std::optional<std::vector<T>>;

        struct ArtistRef {
            uint64_t id = 0;
            Str name;
        };
        struct Audio {
            Str level, format, bitrate;
        };
        struct Pay {
            Number refrain_start{}, refrain_end{};
            Str freeSign;
        };
        struct Volume {
            double gain = 0, peak = 0;
        };
        struct Song {
            uint64_t id = 0, artistId = 0, albumId = 0;
            Str name, songName, subtitle, artist, album, albumPic, albumPic120;
            Vec<ArtistRef> artists;
            int64_t duration = 0;
            int isPay = 0, bodianPayAlbum = 0;
            Vec<Audio> audios;
            std::optional<Pay> payInfo;
            std::optional<Volume> mediaBasicInfo;
        };
        struct Playlist {
            uint64_t id = 0;
            Str name, pic, creatorName, description;
            int sourceType = 0, source = 0, musicCount = 0, musicnum = 0, isPrivate = 0;
            Vec<ArtistRef> categories;
        };
        struct Album {
            uint64_t id = 0, albumId = 0, artistId = 0;
            Str name, pic, artist, showtime, info;
            int musicCount = 0, sourceType = 0;
        };
        struct Artist {
            uint64_t id = 0, artistId = 0;
            Str name, aliasName, pic, desc;
            int songNum = 0, albumNum = 0, musicCnt = 0, albumCnt = 0;
        };
        template <class T, glz::string_literal Key = "resultList">
        struct Page {
            std::optional<int> total;
            Vec<T> items;
            struct glaze {
                static constexpr auto value = glz::object("total", &Page::total, Key.sv(), &Page::items);
            };
        };
        template <class T>
        struct SearchPage {
            std::optional<int> total;
            int rn = 0;
            Vec<T> resultList;
        };
        struct ArtistData {
            std::optional<Artist> artistInfo;
        };
        struct AlbumData {
            std::optional<Album> albumInfo;
        };
        struct Module {
            int id = 0, type = 0;
            Str name;
        };
        struct Home {
            Vec<Module> moduleList;
        };
        struct Group {
            uint64_t id = 0;
            Str title, passRecName;
            Vec<Song> songs;
        };
        template <class T>
        struct ModuleData {
            Vec<T> songList;
            Vec<Song> musicList;
        };
        struct Bang {
            uint64_t id = 0;
            Str name, pic, pubStr;
        };
        struct BangSection {
            Vec<Bang> bangList;
        };
        struct CategoryGroup {
            Str name;
            Vec<ArtistRef> subCategories;
        };
        struct CategoryData {
            Vec<CategoryGroup> categories;
        };
        struct User {
            std::optional<Number> id;
            Str nickname, headImg;
        };
        struct Login {
            std::optional<Number> id, bid;
            Str token;
            std::optional<User> userInfo;
        };
        struct Qr {
            Str qrCode;
            int status = 0;
        };
        struct Audition {
            Str https, url, format;
            int64_t start = 0, end = 0;
            int br = 0;
        };
        struct Rights {
            int status = 0;
            std::optional<Audition> audition;
        };
        struct Url {
            Str audioHttpsUrl, audioUrl, format;
            int bitrate = 0;
        };
        struct Lyric {
            Str content;
        };
        struct CollectState {
            uint64_t id = 0;
            bool collect = false;
        };
        struct CollectStates {
            Vec<CollectState> result;
        };
        struct CommentUser {
            Str nickname, headImg;
        };
        struct SongComment {
            uint64_t id = 0, uid = 0;
            std::optional<uint64_t> parentId;
            Str nickname, headImg, content, imgUrl, createTime, publishTime;
            std::optional<CommentUser> userInfo, replyUserInfo;
            int likeCount = 0, replyCount = 0, like = 0;
        };
        struct Comments {
            Vec<SongComment> comment_list;
            int comment_total = 0;
            bool more = false;
        };
        template <class T>
        struct Envelope {
            std::optional<T> data;
        };
        struct Collected {
            uint64_t id = 0, albumId = 0;
            Str name, pic, artist, creatorName;
            int sourceType = 0, musicCount = 0, isPrivate = 0;
        };
        struct LikeCommentBody {
            uint64_t moduleId;
            int moduleType = 2;
            uint64_t uid, commentId;
            int op;
            std::optional<uint64_t> parentId;
        };
        struct PostCommentBody {
            uint64_t moduleId;
            int moduleType = 2;
            uint64_t uid;
            std::string content;
            std::optional<uint64_t> parentId, replyId;
            int anonymous = 0;
        };
        struct Posted {
            std::optional<Number> id, commentId;
        };

        constexpr glz::opts lax{.error_on_unknown_keys = false};

        template <class T>
        Result<T> parse(const Reply &reply) {
            if (!reply.error.empty()) {
                return std::unexpected(reply.error);
            }
            Envelope<T> value;
            if (glz::read<lax>(value, reply.body) || !value.data) {
                return std::unexpected("Could not parse server response");
            }
            return std::move(*value.data);
        }

        Result<void> write_result(const Reply &reply) {
            if (!reply.error.empty()) {
                return std::unexpected(reply.error);
            }
            return {};
        }

        template <class T, glz::string_literal Key = "resultList">
        Result<Page<T, Key>> parse_page(const Reply &reply) {
            return parse<Page<T, Key>>(reply).and_then([](Page<T, Key> page) -> Result<Page<T, Key>> {
                // Empty account collections and followed artists arrive as data: {}.
                if (!page.total && !page.items) {
                    page.total = 0;
                }
                if (!page.total || *page.total < 0 || (!page.items && *page.total != 0)) {
                    return std::unexpected("Could not parse server response");
                }
                return page;
            });
        }

        std::string take(Str &value) {
            return value ? std::move(*value) : std::string{};
        }

        std::string take(Str &value, Str &alternate) {
            return value && !value->empty() ? std::move(*value) : take(alternate);
        }

        std::string_view view(const Str &value) {
            return value ? std::string_view(*value) : std::string_view{};
        }

        Result<uint64_t> id_number(std::string_view text) {
            uint64_t id = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
            if (error != std::errc{} || end != text.data() + text.size() || id == 0) {
                return std::unexpected("Invalid resource ID");
            }
            return id;
        }

        std::string number_text(const Number &number) {
            return std::visit(
                [](const auto &value) -> std::string {
                    if constexpr (std::same_as<std::decay_t<decltype(value)>, std::string>) {
                        return value;
                    } else {
                        return std::to_string(value);
                    }
                },
                number);
        }

        int64_t number_value(const Number &number) {
            if (auto p = std::get_if<int64_t>(&number)) {
                return *p;
            }
            const auto &text = std::get<std::string>(number);
            int64_t value = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size() ? value : 0;
        }

        struct PlaylistId {
            uint64_t id;
            int source;
        };
        Result<PlaylistId> playlist_id(std::string_view id) {
            int source = 4;
            auto colon = id.find(':');
            if (colon != id.npos) {
                auto [end, error] = std::from_chars(id.data(), id.data() + colon, source);
                if (error != std::errc{} || end != id.data() + colon || (source != 4 && source != 5 && source != 13)) {
                    return std::unexpected("Invalid playlist identifier");
                }
                id.remove_prefix(colon + 1);
            }
            return id_number(id).transform([source](uint64_t value) {
                return PlaylistId{value, source};
            });
        }

        std::pair<std::string_view, std::string_view> comment_id(std::string_view value) {
            const auto colon = value.find(':');
            return colon == value.npos ? std::pair{std::string_view{}, value} : std::pair{value.substr(0, colon), value.substr(colon + 1)};
        }

        Track track(Song &&song) {
            Track out{.id = std::to_string(song.id),
                      .title = take(song.name, song.songName),
                      .alias = take(song.subtitle),
                      .artist = take(song.artist),
                      .album = take(song.album),
                      .artist_id = song.artistId ? std::to_string(song.artistId) : "",
                      .album_id = song.albumId ? std::to_string(song.albumId) : "",
                      .cover = take(song.albumPic, song.albumPic120),
                      .duration = static_cast<uint32_t>(std::clamp<int64_t>(song.duration, 0, UINT32_MAX)),
                      .source = Source::bodian,
                      .paid = song.isPay != 0 || song.bodianPayAlbum != 0};
            if (song.artists && !song.artists->empty()) {
                out.artist.clear();
                for (const auto &artist : *song.artists) {
                    if (!out.artist.empty()) {
                        out.artist += " / ";
                    }
                    out.artist += view(artist.name);
                }
                if (song.artists->front().id) {
                    out.artist_id = std::to_string(song.artists->front().id);
                }
            }
            return out;
        }

        std::vector<Track> tracks_of(Vec<Song> songs) {
            std::vector<Track> out;
            if (songs) {
                out.reserve(songs->size());
                for (auto &song : *songs) {
                    if (song.id) {
                        out.push_back(track(std::move(song)));
                    }
                }
            }
            return out;
        }

        Card playlist_card(Playlist &&playlist, int source = 4, bool owned = false) {
            if (playlist.sourceType) {
                source = playlist.sourceType;
            } else if (playlist.source) {
                source = playlist.source;
            }
            return {.kind = CardKind::playlist,
                    .source = Source::bodian,
                    .id = std::format("{}:{}", source, playlist.id),
                    .title = take(playlist.name),
                    .subtitle = take(playlist.creatorName),
                    .cover = take(playlist.pic),
                    .count = std::max(0, playlist.musicCount ? playlist.musicCount : playlist.musicnum),
                    .count_kind = playlist.musicCount || playlist.musicnum ? Card::Count::songs : Card::Count::none,
                    .owned = owned,
                    .commentable = false,
                    .collectable = !owned && playlist.isPrivate == 0};
        }

        Card album_card(Album &&album) {
            return {.kind = CardKind::album,
                    .source = Source::bodian,
                    .id = std::to_string(album.albumId ? album.albumId : album.id),
                    .title = take(album.name),
                    .subtitle = take(album.artist),
                    .cover = take(album.pic),
                    .count = std::max(0, album.musicCount),
                    .count_kind = album.musicCount ? Card::Count::songs : Card::Count::none,
                    .commentable = false};
        }

        Card artist_card(Artist &&artist) {
            return {.kind = CardKind::artist,
                    .source = Source::bodian,
                    .id = std::to_string(artist.artistId ? artist.artistId : artist.id),
                    .title = take(artist.name),
                    .cover = take(artist.pic),
                    .count = std::max(0, artist.musicCnt ? artist.musicCnt : artist.songNum),
                    .count_kind = artist.musicCnt || artist.songNum ? Card::Count::songs : Card::Count::none,
                    .commentable = false};
        }

        uint64_t resource_id(const Playlist &playlist) {
            return playlist.id;
        }

        uint64_t resource_id(const Song &song) {
            return song.id;
        }
        uint64_t resource_id(const Album &album) {
            return album.albumId ? album.albumId : album.id;
        }
        uint64_t resource_id(const Artist &artist) {
            return artist.artistId ? artist.artistId : artist.id;
        }

        template <class T, class Convert>
        Result<Batch> search_page(const Reply &reply, int offset, int limit, Convert convert) {
            return parse<SearchPage<T>>(reply).and_then([&](SearchPage<T> page) -> Result<Batch> {
                if (!page.total || *page.total < 0 || (!page.resultList && *page.total != 0)) {
                    return std::unexpected("Could not parse server response");
                }
                Batch out;
                const int size = page.resultList ? static_cast<int>(page.resultList->size()) : 0;
                // Live search replies may report only this page's size as total, even when the next page is full.
                const int page_size = page.rn > 0 ? page.rn : limit;
                const bool more = size > 0 && (size >= page_size || limit < *page.total - offset);
                out.next_offset = more ? offset + limit : -1;
                out.total = std::max(*page.total, offset / limit * page_size + size + more);
                auto &items = [&]() -> auto & {
                    if constexpr (std::same_as<T, Song>) {
                        return out.tracks;
                    } else {
                        return out.cards;
                    }
                }();
                items.reserve(size);
                if (page.resultList) {
                    for (auto &item : *page.resultList) {
                        if (resource_id(item)) {
                            items.push_back(convert(std::move(item)));
                        }
                    }
                }
                return out;
            });
        }

        template <class T, glz::string_literal Key = "resultList", class F>
        Result<Batch> card_page(const Reply &reply, int offset, int limit, F card) {
            return parse_page<T, Key>(reply).transform([&](Page<T, Key> page) {
                Batch out{.total = *page.total};
                if (page.items) {
                    out.cards.reserve(page.items->size());
                    for (auto &item : *page.items) {
                        if (resource_id(item)) {
                            out.cards.push_back(card(std::move(item)));
                        }
                    }
                    if (!page.items->empty() && limit < *page.total - offset) {
                        out.next_offset = offset + limit;
                    }
                }
                return out;
            });
        }

        template <glz::string_literal Key = "resultList">
        Result<Batch> song_page(const Reply &reply, int offset, int limit) {
            return parse_page<Song, Key>(reply).transform([&](Page<Song, Key> page) {
                Batch out{.total = *page.total};
                if (page.items && !page.items->empty() && limit < *page.total - offset) {
                    out.next_offset = offset + limit;
                }
                out.tracks = tracks_of(std::move(page.items));
                return out;
            });
        }

        template <glz::string_literal Key = "resultList">
        Task<Result<Batch>> track_page(Client &client, std::string path, Query query, int offset, int limit, bool sign = false) {
            query.emplace_back("pn", std::to_string(offset / limit + 1));
            query.emplace_back("rn", std::to_string(limit));
            co_return song_page<Key>(co_await client.call({.path = std::move(path), .query = std::move(query), .sign = sign}), offset, limit);
        }

        template <glz::string_literal Key = "resultList">
        Task<Result<std::vector<Track>>> all_tracks(Client &client, std::string path, Query query = {}, bool sign = false) {
            std::vector<Track> out;
            for (int offset = 0; offset >= 0;) {
                auto page = co_await track_page<Key>(client, path, query, offset, 100, sign);
                if (!page) {
                    co_return std::unexpected(std::move(page).error());
                }
                out.insert(out.end(), std::make_move_iterator(page->tracks.begin()), std::make_move_iterator(page->tracks.end()));
                offset = page->next_offset;
            }
            co_return out;
        }

        std::string quality_name(std::string_view format, int bitrate) {
            if (format == "flac") {
                return "lossless";
            }
            return bitrate >= 256 ? "exhigh" : bitrate >= 160 ? "higher" : "standard";
        }

        bool audio_url(std::string_view url) {
            return url.starts_with("https://") || url.starts_with("http://");
        }

        Result<std::string> audio_spec(const Song &song, Quality quality) {
            const int wanted = quality >= Quality::lossless ? 2 : quality >= Quality::higher ? 1 : 0;
            const Audio *selected = nullptr;
            int best = -1;
            if (song.audios) {
                for (const auto &audio : *song.audios) {
                    const auto level = view(audio.level);
                    const auto format = view(audio.format);
                    int rank = format == "flac" && level == "ff" ? 2 : format == "mp3" && level == "p" ? 1 : format == "mp3" && (level == "s" || level == "h") ? 0 : -1;
                    if (rank < 0 || rank > wanted || rank <= best || !audio.bitrate || !id_number(*audio.bitrate)) {
                        continue;
                    }
                    selected = &audio;
                    best = rank;
                }
            }
            if (!selected) {
                return std::unexpected("No supported audio format is available");
            }
            return *selected->bitrate + 'k' + *selected->format;
        }

        void add_playback_metadata(Stream &stream, const Song &song) {
            if (song.payInfo) {
                const auto from = std::max<int64_t>(0, number_value(song.payInfo->refrain_start));
                const auto to = number_value(song.payInfo->refrain_end);
                if (to > from) {
                    stream.highlight_from = from / 1000.0;
                    stream.highlight_to = to / 1000.0;
                }
            }
            if (song.mediaBasicInfo) {
                const auto &volume = *song.mediaBasicInfo;
                if (std::isfinite(volume.gain) && std::abs(volume.gain) <= std::numeric_limits<float>::max()) {
                    stream.gain = static_cast<float>(volume.gain);
                }
                if (std::isfinite(volume.peak) && volume.peak >= 0 && volume.peak <= std::numeric_limits<float>::max()) {
                    stream.peak = static_cast<float>(volume.peak);
                }
            }
        }

        Result<Stream> trial_stream(Rights rights) {
            if (rights.status != 3 || !rights.audition) {
                return std::unexpected("This song is unavailable (membership or licensing restriction)");
            }
            auto &trial = *rights.audition;
            std::string url = take(trial.https, trial.url);
            if (!audio_url(url) || trial.start < 0 || trial.end < trial.start) {
                return std::unexpected("Could not parse server response");
            }
            return Stream{.url = std::move(url), .level = quality_name(view(trial.format), trial.br), .trial_start = static_cast<double>(trial.start), .trial = true};
        }

    } // namespace

    Result<Batch> parse_search(const Reply &reply, SearchKind kind, int offset, int limit) {
        switch (kind) {
            case SearchKind::tracks:
                return search_page<Song>(reply, offset, limit, track);
            case SearchKind::albums:
                return search_page<Album>(reply, offset, limit, album_card);
            case SearchKind::artists:
                return search_page<Artist>(reply, offset, limit, artist_card);
            case SearchKind::playlists:
                return search_page<Playlist>(reply, offset, limit, [](Playlist p) {
                    return playlist_card(std::move(p));
                });
        }
        std::unreachable();
    }

    Result<Stream> parse_stream(const Reply &reply) {
        return parse<Url>(reply).and_then([](Url data) -> Result<Stream> {
            std::string url = take(data.audioHttpsUrl, data.audioUrl);
            if (!audio_url(url)) {
                return std::unexpected("This song is unavailable (membership or licensing restriction)");
            }
            return Stream{.url = std::move(url), .level = quality_name(view(data.format), data.bitrate)};
        });
    }

    Result<std::vector<LyricLine>> parse_lyric(const Reply &reply) {
        return parse<Lyric>(reply).and_then([](const Lyric &data) -> Result<std::vector<LyricLine>> {
            if (!data.content || data.content->empty()) {
                return std::vector<LyricLine>{};
            }
            const std::string text = encoding::unbase64(*data.content);
            if (text.empty()) {
                return std::unexpected("Could not parse server response");
            }
            int start_factor = 0, duration_factor = 0;
            if (auto at = text.find("[kuwo:"); at != text.npos) {
                auto end = text.find(']', at);
                if (end != text.npos) {
                    int value = 0;
                    auto parsed = std::from_chars(text.data() + at + 6, text.data() + end, value, 8);
                    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + end && value > 0) {
                        start_factor = value / 10;
                        duration_factor = value % 10;
                    }
                }
            }
            auto lines = lyren::parse_lyrics(text);
            std::vector<LyricLine> out;
            out.reserve(lines.size());
            for (auto &line : lines) {
                std::string_view remaining = line.text;
                std::string plain;
                std::vector<LyricWord> words;
                while (!remaining.empty()) {
                    const auto at = remaining.find('<');
                    plain += remaining.substr(0, at);
                    if (at == remaining.npos) {
                        break;
                    }
                    remaining.remove_prefix(at);
                    const auto end = remaining.find('>');
                    int64_t a = 0, b = 0;
                    bool timed = false;
                    if (end != remaining.npos) {
                        const char *last = remaining.data() + end;
                        const auto first = std::from_chars(remaining.data() + 1, last, a);
                        if (first.ec == std::errc{} && first.ptr != last && *first.ptr == ',') {
                            const auto second = std::from_chars(first.ptr + 1, last, b);
                            timed = second.ec == std::errc{} && second.ptr == last;
                        }
                    }
                    if (!timed) {
                        plain += remaining.front();
                        remaining.remove_prefix(1);
                        continue;
                    }
                    remaining.remove_prefix(end + 1);
                    auto next = remaining.find('<');
                    auto word = remaining.substr(0, next);
                    const auto from = static_cast<uint32_t>(plain.size());
                    plain += word;
                    if (start_factor && duration_factor && (a || b)) {
                        const long double offset = std::floor(std::abs(static_cast<long double>(a) + b) / (2 * start_factor));
                        const long double duration = std::floor(std::abs(static_cast<long double>(a) - b) / (2 * duration_factor));
                        const long double start = std::round(line.time * 1000) + offset;
                        if (start >= 0 && start <= UINT32_MAX && duration > 0 && duration <= UINT32_MAX - start) {
                            words.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(duration), from, static_cast<uint32_t>(plain.size())});
                        }
                    }
                    remaining = next == remaining.npos ? std::string_view{} : remaining.substr(next);
                }
                line.text = std::move(plain);
                line.words = std::move(words);
                if (line.text.empty()) {
                    continue;
                }
                std::ranges::stable_sort(line.words, {}, &LyricWord::start_ms);
                if (!out.empty() && out.back().time == line.time && out.back().words.empty() != line.words.empty()) {
                    if (line.words.empty()) {
                        out.back().translation = std::move(line.text);
                    } else {
                        line.translation = std::move(out.back().text);
                        out.back() = std::move(line);
                    }
                } else {
                    out.push_back(std::move(line));
                }
            }
            return out;
        });
    }

    Bodian::Bodian(Loop &loop, net::Http &http, std::string base_url)
    : client(loop, http, std::move(base_url)) {
        capabilities.toplists = capabilities.square = capabilities.artist_intro = true;
        capabilities.library = capabilities.likes = capabilities.playlist_edit = capabilities.private_playlists = true;
        capabilities.collect_playlists = capabilities.collect_albums = capabilities.follow_artists = true;
        capabilities.comments = {true, false, false};
        capabilities.comment_sort = capabilities.comment_replies = capabilities.comment_likes = capabilities.comment_post = true;
        capabilities.qr_login = true;
        reload_after_edit = true;
    }

    Task<Result<void>> Bodian::initialize() {
        client.ensure_device();
        co_return Result<void>{};
    }

    Task<Result<Batch>> Bodian::search(std::string query, SearchKind kind, int offset, int limit) {
        constexpr std::array names{"music", "album", "artist", "playlist"};
        Call call{.path = std::format("search/{}/list", names[static_cast<size_t>(kind)]),
                  .query = {{"keyword", std::move(query)}, {"pn", std::to_string(offset / limit)}, {"rn", std::to_string(limit)}}};
        if (kind == SearchKind::tracks) {
            call.query.emplace_back("correct", "1");
        }
        co_return parse_search(co_await client.call(std::move(call)), kind, offset, limit);
    }

    Task<Result<Track>> Bodian::song_detail(std::string id) {
        co_return parse<Song>(co_await client.call({.path = "service/music/info", .query = {{"musicId", std::move(id)}}, .sign = true})).and_then([](Song song) -> Result<Track> {
            if (!song.id) {
                return std::unexpected("Could not parse server response");
            }
            return track(std::move(song));
        });
    }

    Task<Result<Stream>> Bodian::stream(Track item, Quality quality) {
        auto song = parse<Song>(co_await client.call({.path = "service/music/info", .query = {{"musicId", item.id}}, .sign = true}));
        co_return co_await and_then(std::move(song), [this, quality](Song song) -> Task<Result<Stream>> {
            if (!song.id) {
                co_return std::unexpected("Could not parse server response");
            }
            const std::string id = std::to_string(song.id);
            const std::string free_sign = song.payInfo ? song.payInfo->freeSign.value_or("") : "";
            auto body = glz::write_json(glz::obj{"musicId", song.id, "freeSign", free_sign}).value();
            auto rights = parse<Rights>(co_await client.call(
                {.path = "play/music/v2/checkRight", .query = {{"musicId", id}, {"freeSign", free_sign}}, .body = std::move(body), .method = "GET", .sign = true}));
            co_return co_await and_then(std::move(rights), [this, quality, song = std::move(song), id, free_sign](Rights rights) -> Task<Result<Stream>> {
                if (rights.status != 4) {
                    co_return trial_stream(std::move(rights)).transform([&](Stream stream) {
                        add_playback_metadata(stream, song);
                        return stream;
                    });
                }
                co_return co_await and_then(audio_spec(song, quality), [this, &song, &id, &free_sign](std::string br) -> Task<Result<Stream>> {
                    auto body = glz::write_json(glz::obj{"devId", client.get("devid"), "musicId", song.id, "br", br, "freeSign", free_sign}).value();
                    auto reply = co_await client.call({.path = "play/music/v2/audioUrl",
                                                       .query = {{"devId", client.get("devid")}, {"musicId", id}, {"br", br}, {"freeSign", free_sign}},
                                                       .body = std::move(body),
                                                       .method = "GET",
                                                       .sign = true});
                    co_return parse_stream(reply).transform([&](Stream stream) {
                        add_playback_metadata(stream, song);
                        return stream;
                    });
                });
            });
        });
    }

    Task<Result<std::vector<LyricLine>>> Bodian::lyrics(Track item) {
        auto lines = parse_lyric(co_await client.lyric(item.id, true));
        if (lines && lines->empty()) {
            lines = parse_lyric(co_await client.lyric(item.id, false));
        }
        co_return lines;
    }

    Task<Result<std::vector<Track>>> Bodian::tracks(Card card) {
        if (card.kind == CardKind::album) {
            co_return co_await all_tracks(client, "service/album/music/" + card.id);
        }
        if (card.kind == CardKind::artist) {
            co_return (co_await artist_songs(card.id, 0)).transform([](Batch page) {
                return std::move(page.tracks);
            });
        }
        if (card.id.starts_with("top:")) {
            co_return co_await all_tracks<"musics">(client, "service/bang/" + card.id.substr(4) + "/musics");
        }
        if (card.id.starts_with("ai:")) {
            const auto colon = card.id.find(':', 3);
            auto reply =
                co_await client.call({.path = "service/home/aiPlaylistDetail", .query = {{"index", card.id.substr(3, colon - 3)}, {"passRecName", card.id.substr(colon + 1)}}});
            co_return parse<ModuleData<Group>>(reply).transform([](auto data) {
                return tracks_of(std::move(data.musicList));
            });
        }
        if (card.id.starts_with("module:")) {
            auto reply = co_await client.call({.path = "service/home/module", .query = {{"moduleId", card.id.substr(7)}}});
            co_return parse<ModuleData<Group>>(reply).transform([](auto data) {
                return tracks_of(std::move(data.musicList));
            });
        }
        co_return co_await and_then(playlist_id(card.id), [this](PlaylistId id) -> Task<Result<std::vector<Track>>> {
            co_return co_await all_tracks<"list">(client, std::format("service/playlist/{}/musicList", id.id), {{"source", std::to_string(id.source)}}, true);
        });
    }

    Task<Result<std::vector<Card>>> Bodian::recommended_playlists() {
        auto home = parse<Home>(co_await client.call({.path = "service/home/index"}));
        co_return co_await and_then(std::move(home), [this](Home home) -> Task<Result<std::vector<Card>>> {
            if (!home.moduleList) {
                co_return std::unexpected("Could not parse server response");
            }
            std::vector<Card> out;
            for (const auto &module : *home.moduleList) {
                if (module.type != 3 && module.type != 4 && module.type != 5 && module.type != 10 && module.type != 11) {
                    continue;
                }
                auto reply = co_await client.call({.path = "service/home/module", .query = {{"moduleId", std::to_string(module.id)}}});
                if (!reply.error.empty()) {
                    co_return std::unexpected(reply.error);
                }
                if (module.type == 5) {
                    auto data = parse<ModuleData<Playlist>>(reply);
                    if (!data) {
                        co_return std::unexpected(std::move(data).error());
                    }
                    if (data->songList) {
                        for (auto &playlist : *data->songList) {
                            if (playlist.id) {
                                out.push_back(playlist_card(std::move(playlist), 13));
                            }
                        }
                    }
                } else {
                    auto data = parse<ModuleData<Group>>(reply);
                    if (!data) {
                        co_return std::unexpected(std::move(data).error());
                    }
                    if (module.type == 3 || module.type == 10) {
                        if (data->musicList && !data->musicList->empty()) {
                            auto &cover = data->musicList->front();
                            out.push_back({.source = Source::bodian,
                                           .id = "module:" + std::to_string(module.id),
                                           .title = module.name.value_or(""),
                                           .cover = take(cover.albumPic, cover.albumPic120),
                                           .count = static_cast<int>(data->musicList->size()),
                                           .count_kind = Card::Count::songs,
                                           .commentable = false});
                        }
                    } else if (data->songList) {
                        for (auto &group : *data->songList) {
                            std::string cover;
                            if (group.songs && !group.songs->empty()) {
                                cover = take(group.songs->front().albumPic, group.songs->front().albumPic120);
                            }
                            out.push_back({.source = Source::bodian,
                                           .id = std::format("ai:{}:{}", group.id, group.passRecName.value_or("")),
                                           .title = group.title.value_or(group.passRecName.value_or(module.name.value_or(""))),
                                           .cover = std::move(cover),
                                           .commentable = false});
                        }
                    }
                }
            }
            co_return out;
        });
    }

    Task<Result<std::vector<Card>>> Bodian::toplists() {
        co_return parse<std::vector<BangSection>>(co_await client.call({.path = "service/home/bangNew"})).transform([](auto sections) {
            std::vector<Card> out;
            for (auto &section : sections) {
                if (!section.bangList) {
                    continue;
                }
                for (auto &bang : *section.bangList) {
                    if (!bang.id) {
                        continue;
                    }
                    out.push_back({.source = Source::bodian,
                                   .id = "top:" + std::to_string(bang.id),
                                   .title = take(bang.name),
                                   .subtitle = take(bang.pubStr),
                                   .cover = take(bang.pic),
                                   .commentable = false});
                }
            }
            return out;
        });
    }

    Task<Result<Categories>> Bodian::categories() {
        co_return parse<CategoryData>(co_await client.call({.path = "service/category/list"})).and_then([](CategoryData data) -> Result<Categories> {
            if (!data.categories) {
                return std::unexpected("Could not parse server response");
            }
            Categories out;
            out.groups.reserve(data.categories->size());
            out.hot.reserve(data.categories->size());
            for (auto &group : *data.categories) {
                std::vector<Category> categories;
                if (group.subCategories) {
                    categories.reserve(group.subCategories->size());
                    for (auto &item : *group.subCategories) {
                        categories.push_back({.id = std::to_string(item.id), .name = take(item.name)});
                    }
                }
                if (!categories.empty()) {
                    out.hot.push_back(categories.front());
                }
                out.groups.emplace_back(take(group.name), std::move(categories));
            }
            return out;
        });
    }

    Task<Result<Batch>> Bodian::square(std::string category, int offset) {
        if (category.empty()) {
            co_return (co_await recommended_playlists()).transform([](std::vector<Card> cards) {
                const int total = static_cast<int>(cards.size());
                return Batch{.cards = std::move(cards), .total = total};
            });
        }
        auto reply = co_await client.call({.path = "service/category/" + category + "/playlist", .query = {{"pn", std::to_string(offset / 50 + 1)}, {"rn", "50"}}});
        co_return card_page<Playlist, "playLists">(reply, offset, 50, [](Playlist p) {
            return playlist_card(std::move(p));
        });
    }

    Task<Result<Batch>> Bodian::artist_songs(std::string id, int offset) {
        co_return co_await track_page(client, "service/artist/music/" + id, {}, offset, 100);
    }

    Task<Result<Batch>> Bodian::artist_albums(std::string id, int offset) {
        auto reply = co_await client.call({.path = "service/artist/album/" + id, .query = {{"pn", std::to_string(offset / 30 + 1)}, {"rn", "30"}}});
        co_return card_page<Album>(reply, offset, 30, album_card);
    }

    Task<Result<std::vector<std::pair<std::string, std::string>>>> Bodian::artist_intro(std::string id) {
        co_return parse<ArtistData>(co_await client.call({.path = "service/artist/" + id}))
            .and_then([](const ArtistData &data) -> Result<std::vector<std::pair<std::string, std::string>>> {
                if (!data.artistInfo) {
                    return std::unexpected("Could not parse server response");
                }
                return std::vector<std::pair<std::string, std::string>>{{"", data.artistInfo->desc.value_or("")}};
            });
    }

    Task<Result<ArtistInfo>> Bodian::artist(std::string id) {
        auto detail = parse<ArtistData>(co_await client.call({.path = "service/artist/" + id}));
        co_return co_await and_then(std::move(detail), [this, id = std::move(id)](ArtistData data) -> Task<Result<ArtistInfo>> {
            if (!data.artistInfo) {
                co_return std::unexpected("Could not parse server response");
            }
            auto &artist = *data.artistInfo;
            ArtistInfo out{.name = take(artist.name),
                           .alias = take(artist.aliasName),
                           .avatar = take(artist.pic),
                           .songs = std::max(0, artist.musicCnt),
                           .albums = std::max(0, artist.albumCnt)};
            if (logged_in()) {
                auto followed = co_await followed_artists();
                if (!followed) {
                    co_return std::unexpected(std::move(followed).error());
                }
                out.followed = std::ranges::any_of(*followed, [&](const Card &card) {
                    return card.id == id;
                });
            }
            co_return out;
        });
    }

    Task<Result<AlbumInfo>> Bodian::album(std::string id) {
        auto detail = parse<AlbumData>(co_await client.call({.path = "service/album/" + id}));
        co_return co_await and_then(std::move(detail), [this, id = std::move(id)](AlbumData data) -> Task<Result<AlbumInfo>> {
            if (!data.albumInfo) {
                co_return std::unexpected("Could not parse server response");
            }
            auto songs = co_await all_tracks(client, "service/album/music/" + id);
            if (!songs) {
                co_return std::unexpected(std::move(songs).error());
            }
            auto &album = *data.albumInfo;
            AlbumInfo out{.artist = take(album.artist),
                          .artist_id = album.artistId ? std::to_string(album.artistId) : "",
                          .date = take(album.showtime),
                          .about = take(album.info),
                          .tracks = std::move(*songs)};
            if (logged_in()) {
                auto state = parse<CollectStates>(co_await client.call({.path = "service/collect/multipleState", .query = {{"source", "6"}, {"sourceIds", id}}, .sign = true}));
                if (!state) {
                    co_return std::unexpected(std::move(state).error());
                }
                if (state->result) {
                    for (const auto &item : *state->result) {
                        if (std::to_string(item.id) == id) {
                            out.collected = item.collect;
                        }
                    }
                }
            }
            co_return out;
        });
    }

    Task<Result<QrCode>> Bodian::qr_start() {
        const uint64_t generation = ++qr_gen_;
        qr_key_.clear();
        auto reply = parse<Qr>(co_await client.call({.path = "ucenter/login/qrCode", .sign = true}));
        co_return std::move(reply).and_then([this, generation](Qr data) -> Result<QrCode> {
            if (generation != qr_gen_) {
                return std::unexpected("Login cancelled");
            }
            if (!data.qrCode || data.qrCode->empty()) {
                return std::unexpected("Could not parse server response");
            }
            qr_key_ = std::move(*data.qrCode);
            return QrCode{.text = "https://bodian-oia.kuwo.cn/bodian/download.html?pageName=login_pc&pt=3&id=" + form_encode(qr_key_)};
        });
    }

    Task<Result<QrStatus>> Bodian::qr_poll() {
        const uint64_t generation = qr_gen_;
        auto status = parse<Qr>(co_await client.call({.path = "ucenter/login/qrCodeStatus", .query = {{"qrCode", qr_key_}}, .sign = true}));
        if (generation != qr_gen_) {
            co_return QrStatus{.state = QrState::expired};
        }
        co_return co_await and_then(std::move(status), [this, generation](Qr data) -> Task<Result<QrStatus>> {
            if (data.status == 1) {
                co_return QrStatus{};
            }
            if (data.status == 2) {
                co_return QrStatus{.state = QrState::expired};
            }
            if (data.status != 3) {
                co_return std::unexpected("Could not parse server response");
            }
            auto reply = co_await client.call({.path = "ucenter/users/login", .body = glz::write_json(glz::obj{"authType", 10, "qrCode", qr_key_}).value(), .sign = true});
            if (generation != qr_gen_) {
                co_return QrStatus{.state = QrState::expired};
            }
            if (reply.code == 11027) {
                co_return QrStatus{.state = QrState::scanned};
            }
            co_return parse<Login>(reply).and_then([this](Login login) -> Result<QrStatus> {
                std::string id;
                for (const auto *value : {&login.id, &login.bid, login.userInfo ? &login.userInfo->id : &login.id}) {
                    if (!*value) {
                        continue;
                    }
                    std::string current = number_text(**value);
                    if (!id_number(current) || (!id.empty() && id != current)) {
                        return std::unexpected("Could not parse server response");
                    }
                    id = std::move(current);
                }
                if (id.empty() || !login.token || login.token->empty()) {
                    return std::unexpected("Could not parse server response");
                }
                client.session["uid"] = std::move(id);
                client.session["token"] = std::move(*login.token);
                client.session["nickname"] = login.userInfo ? take(login.userInfo->nickname) : "";
                client.session["avatar"] = login.userInfo ? take(login.userInfo->headImg) : "";
                if (client.on_session) {
                    client.on_session();
                }
                return QrStatus{.state = QrState::confirmed, .nickname = client.get("nickname"), .avatar = client.get("avatar")};
            });
        });
    }

    Task<Result<Account>> Bodian::account() {
        if (!logged_in()) {
            co_return std::unexpected("Sign in to continue");
        }
        const auto reply = co_await client.call({.path = "ucenter/users/pub/" + client.get("uid"), .sign = true});
        co_return write_result(reply).transform([this] {
            return Account{.id = client.get("uid"), .name = client.get("nickname").empty() ? "Bodian Music" : client.get("nickname"), .avatar = client.get("avatar")};
        });
    }

    Task<void> Bodian::logout() {
        cancel_login();
        client.clear_login();
        co_return;
    }

    Task<Result<Card>> Bodian::liked_playlist(std::string uid) {
        co_return parse<Playlist>(co_await client.call({.path = "service/playlist/fond", .query = {{"userId", std::move(uid)}}, .sign = true})).transform([](Playlist playlist) {
            if (!playlist.id) {
                return Card{.source = Source::bodian};
            }
            auto card = playlist_card(std::move(playlist), 5);
            card.collectable = false;
            return card;
        });
    }

    Task<Result<std::pair<Card, std::vector<Card>>>> Bodian::user_playlists(std::string uid) {
        auto liked = co_await liked_playlist(uid);
        co_return co_await and_then(std::move(liked), [this, uid = std::move(uid)](Card liked) -> Task<Result<std::pair<Card, std::vector<Card>>>> {
            auto created = parse<Page<Playlist, "playLists">>(co_await client.call({.path = "service/playlist/userCreate", .query = {{"userId", uid}}, .sign = true}));
            co_return co_await and_then(std::move(created),
                                        [this, liked = std::move(liked)](Page<Playlist, "playLists"> created) mutable -> Task<Result<std::pair<Card, std::vector<Card>>>> {
                                            auto saved = co_await collected_playlists();
                                            co_return std::move(saved).transform([&](std::vector<Card> saved) {
                                                std::vector<Card> playlists;
                                                playlists.reserve((created.items ? created.items->size() : 0) + saved.size());
                                                if (created.items) {
                                                    for (auto &playlist : *created.items) {
                                                        if (!playlist.id) {
                                                            continue;
                                                        }
                                                        auto card = playlist_card(std::move(playlist), 5, true);
                                                        if (card.id != liked.id) {
                                                            playlists.push_back(std::move(card));
                                                        }
                                                    }
                                                }
                                                playlists.insert(playlists.end(), std::make_move_iterator(saved.begin()), std::make_move_iterator(saved.end()));
                                                return std::pair{std::move(liked), std::move(playlists)};
                                            });
                                        });
        });
    }

    Task<Result<std::vector<std::string>>> Bodian::liked_ids(std::string uid) {
        co_return co_await and_then(co_await liked_playlist(std::move(uid)), [this](Card liked) -> Task<Result<std::vector<std::string>>> {
            if (liked.id.empty()) {
                co_return std::vector<std::string>{};
            }
            co_return (co_await tracks(std::move(liked))).transform([](std::vector<Track> songs) {
                std::vector<std::string> ids;
                ids.reserve(songs.size());
                for (auto &song : songs) {
                    ids.push_back(std::move(song.id));
                }
                return ids;
            });
        });
    }

    Task<Result<void>> Bodian::like(Track track, bool on) {
        co_return co_await and_then(co_await liked_playlist(client.get("uid")), [this, track = std::move(track), on](Card liked) mutable -> Task<Result<void>> {
            if (liked.id.empty()) {
                co_return std::unexpected("The account has no liked-songs playlist");
            }
            co_return co_await edit_playlist(liked.id, std::move(track), on);
        });
    }

    Task<Result<std::vector<Card>>> Bodian::collections(CardKind kind) {
        std::vector<Card> out;
        for (int offset = 0; offset >= 0;) {
            auto page = parse_page<Collected, "playLists">(
                co_await client.call({.path = "service/collect/4/list",
                                      .query = {{"userId", client.get("uid")}, {"fromUid", client.get("uid")}, {"pn", std::to_string(offset / 100 + 1)}, {"rn", "100"}},
                                      .sign = true}));
            if (!page) {
                co_return std::unexpected(std::move(page).error());
            }
            if (!page->items || page->items->empty()) {
                break;
            }
            for (auto &item : *page->items) {
                const bool album = item.sourceType == 6 || item.albumId != 0;
                if ((kind == CardKind::album) != album) {
                    continue;
                }
                if (!item.id && !item.albumId) {
                    continue;
                }
                if (album) {
                    out.push_back(album_card({.id = item.id,
                                              .albumId = item.albumId,
                                              .name = std::move(item.name),
                                              .pic = std::move(item.pic),
                                              .artist = std::move(item.artist),
                                              .musicCount = item.musicCount}));
                } else {
                    out.push_back(playlist_card({.id = item.id,
                                                 .name = std::move(item.name),
                                                 .pic = std::move(item.pic),
                                                 .creatorName = std::move(item.creatorName),
                                                 .sourceType = item.sourceType,
                                                 .musicCount = item.musicCount,
                                                 .isPrivate = item.isPrivate}));
                }
            }
            offset = 100 < *page->total - offset ? offset + 100 : -1;
        }
        co_return out;
    }

    Task<Result<std::vector<Card>>> Bodian::collected_playlists() {
        co_return co_await collections(CardKind::playlist);
    }

    Task<Result<std::vector<Card>>> Bodian::collected_albums() {
        co_return co_await collections(CardKind::album);
    }

    Task<Result<std::vector<Card>>> Bodian::followed_artists() {
        std::vector<Card> out;
        for (int offset = 0; offset >= 0;) {
            auto reply = co_await client.call({.path = "service/collect/7/list",
                                               .query = {{"userId", client.get("uid")}, {"fromUid", client.get("uid")}, {"pn", std::to_string(offset / 100 + 1)}, {"rn", "100"}},
                                               .sign = true});
            auto page = card_page<Artist, "artistList">(reply, offset, 100, artist_card);
            if (!page) {
                co_return std::unexpected(std::move(page).error());
            }
            out.insert(out.end(), std::make_move_iterator(page->cards.begin()), std::make_move_iterator(page->cards.end()));
            offset = page->next_offset;
        }
        co_return out;
    }

    Task<Result<void>> Bodian::set_collection(std::string id, int source, bool on) {
        auto resource = id_number(id);
        auto uid = id_number(client.get("uid"));
        if (!resource) {
            co_return std::unexpected(std::move(resource).error());
        }
        if (!uid) {
            co_return std::unexpected("Sign in to continue");
        }
        std::string body;
        if (source == 7) {
            body = glz::write_json(glz::obj{"source", source, "sourceId", std::array{*resource}, "op", on ? 1 : 2, "uid", *uid, "token", client.get("token")}).value();
        } else {
            body = glz::write_json(glz::obj{"source", source, "sourceId", std::array{*resource}, "op", on ? 1 : 2, "uid", *uid}).value();
        }
        co_return write_result(co_await client.call({.path = "service/collect", .body = std::move(body), .sign = true}));
    }

    Task<Result<void>> Bodian::follow(std::string artist, bool on) {
        co_return co_await set_collection(std::move(artist), 7, on);
    }

    Task<Result<void>> Bodian::collect(std::string album, bool on) {
        co_return co_await set_collection(std::move(album), 6, on);
    }

    Task<Result<void>> Bodian::collect_playlist(std::string id, bool on) {
        co_return co_await and_then(playlist_id(id), [this, on](PlaylistId id) -> Task<Result<void>> {
            co_return co_await set_collection(std::to_string(id.id), 4, on);
        });
    }

    Task<Result<Card>> Bodian::create_playlist(std::string name, bool hidden) {
        auto reply = co_await client.call({.path = "service/playlist", .body = glz::write_json(glz::obj{"name", name, "private", hidden}).value(), .sign = true});
        co_return parse<ArtistRef>(reply).and_then([&](const ArtistRef &data) -> Result<Card> {
            if (!data.id) {
                return std::unexpected("Could not parse server response");
            }
            return Card{.source = Source::bodian, .id = "5:" + std::to_string(data.id), .title = std::move(name), .owned = true, .commentable = false};
        });
    }

    Task<Result<void>> Bodian::rename_playlist(std::string id, std::string name) {
        co_return co_await and_then(playlist_id(id), [this, name = std::move(name)](PlaylistId id) -> Task<Result<void>> {
            auto detail =
                parse<Playlist>(co_await client.call({.path = std::format("service/playlist/info/{}", id.id), .query = {{"source", std::to_string(id.source)}}, .sign = true}));
            co_return co_await and_then(std::move(detail), [this, id, name = std::move(name)](Playlist detail) -> Task<Result<void>> {
                std::vector<uint64_t> categories;
                if (detail.categories) {
                    for (const auto &category : *detail.categories) {
                        categories.push_back(category.id);
                    }
                }
                auto body =
                    glz::write_json(glz::obj{"id", id.id, "name", name, "description", view(detail.description), "pic", view(detail.pic), "categoryList", categories}).value();
                co_return write_result(co_await client.call({.path = "service/playlist", .body = std::move(body), .method = "PUT", .sign = true}));
            });
        });
    }

    Task<Result<void>> Bodian::delete_playlist(std::string id) {
        co_return co_await and_then(playlist_id(id), [this](PlaylistId id) -> Task<Result<void>> {
            co_return write_result(
                co_await client.call({.path = "service/playlist", .body = glz::write_json(glz::obj{"playlistIds", std::array{id.id}}).value(), .method = "DELETE", .sign = true}));
        });
    }

    Task<Result<void>> Bodian::edit_playlist(std::string id, std::vector<Track> tracks, bool add) {
        co_return co_await and_then(playlist_id(id), [this, tracks = std::move(tracks), add](PlaylistId id) -> Task<Result<void>> {
            std::vector<uint64_t> songs;
            songs.reserve(tracks.size());
            for (const auto &track : tracks) {
                auto song = id_number(track.id);
                if (!song) {
                    co_return std::unexpected(song.error());
                }
                songs.push_back(*song);
            }
            auto body = glz::write_json(glz::obj{"playListId", id.id, "musicIdList", songs}).value();
            co_return write_result(co_await client.call({.path = add ? "service/playlist/music" : "service/playlist/music/delete", .body = std::move(body), .sign = true}));
        });
    }

    Result<CommentPage> parse_comments(const Reply &reply, std::string_view uid) {
        return parse<Comments>(reply).and_then([&](Comments data) -> Result<CommentPage> {
            if (!data.comment_list && data.comment_total > 0) {
                return std::unexpected("Could not parse server response");
            }
            CommentPage out{.total = std::max(0, data.comment_total), .more = data.more};
            if (!data.comment_list) {
                return out;
            }
            out.comments.reserve(data.comment_list->size());
            for (auto &comment : *data.comment_list) {
                if (!comment.id) {
                    continue;
                }
                std::string id = std::to_string(comment.id);
                if (comment.parentId && *comment.parentId) {
                    id = std::to_string(*comment.parentId) + ':' + id;
                }
                out.comments.push_back({.id = std::move(id),
                                        .user = comment.userInfo ? take(comment.nickname, comment.userInfo->nickname) : take(comment.nickname),
                                        .avatar = comment.userInfo ? take(comment.headImg, comment.userInfo->headImg) : take(comment.headImg),
                                        .text = take(comment.content),
                                        .time = take(comment.createTime, comment.publishTime),
                                        .reply_user = comment.replyUserInfo ? take(comment.replyUserInfo->nickname) : "",
                                        .image = take(comment.imgUrl),
                                        .likes = std::max(0, comment.likeCount),
                                        .replies = std::max(0, comment.replyCount),
                                        .liked = comment.like != 0,
                                        .owned = !uid.empty() && std::to_string(comment.uid) == uid});
            }
            return out;
        });
    }

    Task<Result<CommentPage>> Bodian::comments(Subject, std::string id, bool hot, int page, std::string) {
        auto reply = co_await client.call({.path = hot ? "comments/v3/hot" : "comments/v3/new",
                                           .query = {{"moduleType", "2"}, {"moduleId", std::move(id)}, {"pn", std::to_string(page)}, {"rn", "30"}},
                                           .android = true});
        co_return parse_comments(reply, client.get("uid"));
    }

    Task<Result<CommentPage>> Bodian::comment_replies(Subject, std::string id, std::string root, int page, std::string) {
        auto reply = co_await client.call(
            {.path = "comments/v3/replies",
             .query = {{"moduleType", "2"}, {"moduleId", std::move(id)}, {"pn", std::to_string(page)}, {"rn", "30"}, {"parentId", std::string(comment_id(root).second)}},
             .android = true});
        co_return parse_comments(reply, client.get("uid"));
    }

    Task<Result<void>> Bodian::like_comment(Subject, std::string id, std::string comment, bool on) {
        const auto [parent, raw_id] = comment_id(comment);
        const auto module = id_number(id), uid = id_number(client.get("uid")), value = id_number(raw_id);
        if (!module || !value) {
            co_return std::unexpected("Invalid resource ID");
        }
        if (!uid) {
            co_return std::unexpected("Sign in to continue");
        }
        LikeCommentBody body{.moduleId = *module, .uid = *uid, .commentId = *value, .op = on ? 1 : 2};
        if (!parent.empty()) {
            auto number = id_number(parent);
            if (!number) {
                co_return std::unexpected(std::move(number).error());
            }
            body.parentId = *number;
        }
        co_return write_result(co_await client.call({.path = "comments/v3/like", .body = glz::write_json(body).value(), .android = true}));
    }

    Task<Result<std::string>> Bodian::post_comment(Subject, std::string id, std::string content, std::string reply, std::string root) {
        const auto module = id_number(id), uid = id_number(client.get("uid"));
        if (!module) {
            co_return std::unexpected(std::move(module).error());
        }
        if (!uid) {
            co_return std::unexpected("Sign in to continue");
        }
        PostCommentBody body{.moduleId = *module, .uid = *uid, .content = std::move(content)};
        if (!reply.empty()) {
            const auto [parent, raw_id] = comment_id(reply);
            if (root.empty()) {
                root = parent.empty() ? std::string(raw_id) : std::string(parent);
            }
            auto parent_number = id_number(comment_id(root).second), reply_number = id_number(raw_id);
            if (!parent_number || !reply_number) {
                co_return std::unexpected("Invalid resource ID");
            }
            body.parentId = *parent_number;
            if (*reply_number != *parent_number) {
                body.replyId = *reply_number;
            }
        }
        auto response = co_await client.call({.path = "comments/v3/publish", .body = glz::write_json(body).value(), .android = true});
        if (!response.error.empty()) {
            co_return std::unexpected(response.error);
        }
        Envelope<std::variant<int64_t, std::string, Posted>> envelope;
        if (glz::read<lax>(envelope, response.body) || !envelope.data) {
            co_return std::string{};
        }
        co_return std::visit(
            [](const auto &value) -> std::string {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, Posted>) {
                    const auto &id = value.id ? value.id : value.commentId;
                    return id ? number_text(*id) : "";
                } else if constexpr (std::same_as<T, std::string>) {
                    return value;
                } else {
                    return std::to_string(value);
                }
            },
            *envelope.data);
    }

} // namespace lyren::bodian
