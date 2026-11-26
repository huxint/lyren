// Development CLI.
//
//   lyren-probe play <url|file> [step...]
//   lyren-probe [ncm|qq|qishui|kugou|bodian] <command> [args...]
//
// Common provider commands (default provider is ncm):
//   search <query> [tracks|albums|artists|playlists] [offset]
//   url <id> [standard|higher|exhigh|lossless|hires] | lyrics <id>
//   album <id> | artist <id> | playlist <id> | top | recommend | guess
//   new-songs [area] | new-albums [area] [offset] | saved-playlists | categories
//   square [category|all] [offset] | similar <id> | collect-playlist <id> on|off
//   comments [song|album|playlist] <id> [new]
//   replies [song|album|playlist <id>] <comment id>
//   post-comment <song|album|playlist> <id> <text file> [reply id]
//   like-comment [song|album|playlist <id>] <comment id> on|off
//   login | account | library | collection
//
// Provider-specific commands:
//   lyren-probe [ncm] call <uri> [fields] [weapi|clientlog]
//   lyren-probe qq call <module> <method> [param] | radar | qr [png] | renew
//   lyren-probe qq dislikes [songs|artists|styles|all] | dislike <songs|artists|styles> <id> on|off [name]
//   lyren-probe qq reorder <dir:ID> <song id>...
//   lyren-probe qishui check [signer_url|json] [cookie|cookie_file]
//
// Steps run in order: wait:MS  seek:SECONDS  pause  play  vol:0..1  open:URL|FILE  quit. Without steps it is interactive:
// space pauses, ←/→ seek 10 s, ↑/↓ change the volume, q quits. Exits when the track fails, or ends after the last step.
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>

#include <qrencode.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include "audio/player.hpp"
#include "bodian/bodian.hpp"
#include "core/log.hpp"
#include "core/string_map.hpp"
#include "core/utf8.hpp"
#include "kugou/kugou.hpp"
#include "media/lyrics.hpp"
#include "ncm/netease.hpp"
#include "net/http.hpp"
#include "qishui/qishui.hpp"
#include "qq/qq.hpp"
#include "sys/creds.hpp"
#include "sys/paths.hpp"

using namespace lyren;

namespace {

    const char *state_name(audio::Player::State s) {
        static constexpr std::array names = {"idle", "loading", "playing", "paused", "ended", "failed"};
        return names[static_cast<int>(s)];
    }

    int play(std::string src, std::vector<std::string> steps) {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

        Loop loop;
        net::Http http;
        audio::Player player(loop, http);
        bool tty = isatty(STDOUT_FILENO), interactive = steps.empty() && isatty(STDIN_FILENO);

        loop.watch(sfd, EPOLLIN, [&](uint32_t) {
            loop.quit();
        });

        auto status = [&] {
            auto [have, size] = player.spool() ? player.spool()->progress() : std::pair<uint64_t, uint64_t>{};
            auto f = player.format();
            std::string line = std::format("{:8} {:7.2f} / {:6.2f}  {} {} Hz {} ch  vol {:.2f}  cached {:3.0f}%{}", state_name(player.state()), player.position(),
                                           player.duration(), player.codec(), f.rate, f.channels, player.volume(),
                                           size ? 100.0 * static_cast<double>(have) / static_cast<double>(size) : 0.0, player.buffering() ? "  buffering" : "");
            if (tty) {
                std::print("\r{}\x1b[K", line);
            } else {
                std::println("{}", line);
            }
            std::fflush(stdout);
        };
        auto tick = [&](this auto &self) -> void {
            status();
            loop.after(tty ? 100ms : 500ms, self);
        };
        tick();

        auto open = [&](const std::string &src) {
            if (src.starts_with("http://") || src.starts_with("https://")) {
                player.open_url(src);
            } else {
                player.open_file(src);
            }
        };

        size_t step = 0;
        audio::Player::State last = player.state();
        player.on_change = [&] {
            auto s = player.state();
            if (s == last) {
                return;
            }
            last = s;
            if (s == audio::Player::State::failed) {
                std::println("{}failed: {}", tty ? "\n" : "", player.error());
                loop.quit();
            } else if (s == audio::Player::State::ended && step >= steps.size()) {
                loop.quit();
            }
        };

        termios saved{};
        if (interactive) {
            tcgetattr(STDIN_FILENO, &saved);
            termios raw = saved;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
            std::println("space pause · ←/→ seek 10 s · ↑/↓ volume · q quit");
            loop.watch(STDIN_FILENO, EPOLLIN, [&](uint32_t) {
                std::array<char, 16> buf;
                ssize_t n = read(STDIN_FILENO, buf.data(), buf.size());
                std::string_view k(buf.data(), n > 0 ? static_cast<size_t>(n) : 0);
                if (k == " ") {
                    player.toggle();
                } else if (k == "q") {
                    loop.quit();
                } else if (k == "\x1b[C") {
                    player.seek(player.position() + 10);
                } else if (k == "\x1b[D") {
                    player.seek(player.position() - 10);
                } else if (k == "\x1b[A") {
                    player.set_volume(player.volume() + 0.05f);
                } else if (k == "\x1b[B") {
                    player.set_volume(player.volume() - 0.05f);
                }
            });
        }

        // Steps run one after another; wait:MS pauses the sequence. The track may end on its own before the last step.
        auto run_steps = [&](this auto &self) -> void {
            while (step < steps.size()) {
                std::string_view s = steps[step++];
                auto arg = s.substr(s.find(':') + 1);
                if (!tty) {
                    std::println("> {}", s);
                }
                if (s.starts_with("wait:")) {
                    loop.after(std::chrono::milliseconds(std::stoi(std::string(arg))), self);
                    return;
                }
                if (s.starts_with("seek:")) {
                    player.seek(std::stod(std::string(arg)));
                } else if (s.starts_with("vol:")) {
                    player.set_volume(std::stof(std::string(arg)));
                } else if (s.starts_with("open:")) {
                    open(std::string(arg));
                } else if (s == "pause") {
                    player.pause();
                } else if (s == "play") {
                    player.play();
                } else if (s == "quit") {
                    return loop.quit();
                } else {
                    std::println(stderr, "unknown step {}", s);
                }
            }
        };
        open(src);
        run_steps();
        loop.run();

        status();
        if (tty) {
            std::println();
        }
        if (interactive) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        }
        return player.state() == audio::Player::State::failed ? 1 : 0;
    }

    // Two modules per character: the upper and lower half blocks.
    void print_qr(const std::string &text) {
        QRcode *q = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
        int n = q->width;
        auto on = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < n && y < n && (q->data[y * n + x] & 1);
        };
        for (int y = -2; y < n + 2; y += 2) {
            std::string line;
            for (int x = -2; x < n + 2; ++x) {
                bool top = on(x, y), bottom = on(x, y + 1);
                // Light modules are drawn, so the code reads on dark terminals.
                line += !top && !bottom ? "█" : !top ? "▀" : !bottom ? "▄" : " ";
            }
            std::println("{}", line);
        }
        QRcode_free(q);
    }

    std::string_view prefix(std::string_view text, size_t bytes) {
        size_t i = 0, end = 0;
        while (i < text.size() && i < bytes) {
            i = utf8::next(text, i);
            if (i <= bytes) {
                end = i;
            }
        }
        return text.substr(0, end);
    }

    std::string_view arg_at(std::span<const std::string> args, size_t i) {
        return i < args.size() ? std::string_view(args[i]) : std::string_view{};
    }

    std::optional<Subject> parse_subject(std::string_view name) {
        if (name == "song") {
            return Subject::song;
        }
        if (name == "album") {
            return Subject::album;
        }
        if (name == "playlist") {
            return Subject::playlist;
        }
        return std::nullopt;
    }

    Quality parse_quality(std::string_view name) {
        static constexpr std::array names = {"standard", "higher", "exhigh", "lossless", "hires"};
        for (size_t k = 0; k < names.size(); ++k) {
            if (name == names[k]) {
                return static_cast<Quality>(k);
            }
        }
        return Quality::lossless;
    }

    void print_tracks(std::span<const Track> tracks) {
        for (const auto &t : tracks) {
            std::println("{:>12} {:>14} {:>14}  {}{}  {} · {}  {}:{:02}{}", t.id, t.mid, t.media, t.title, t.alias.empty() ? "" : " (" + t.alias + ')', t.artist, t.album,
                         t.duration / 60, t.duration % 60, t.paid ? "  paid" : "");
        }
    }

    void print_cards(std::span<const Card> cards) {
        for (const auto &c : cards) {
            std::println("{:>16}  {}  {}  {}", c.id, c.title, c.subtitle, c.cover);
        }
    }

    Result<int> page_offset(std::string_view value) {
        int offset = 0;
        if (!value.empty()) {
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), offset);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || offset < 0) {
                return std::unexpected("Expected a nonnegative offset");
            }
        }
        return offset;
    }

    Result<uint64_t> positive_u64(std::string_view value, std::string_view error) {
        uint64_t n = 0;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !n) {
            return std::unexpected(std::string(error));
        }
        return n;
    }

    std::string read_file_or_value(std::string_view input) {
        if (!input.empty() && std::filesystem::is_regular_file(input)) {
            std::ifstream in{std::filesystem::path(input)};
            return std::string{std::istreambuf_iterator<char>(in), {}};
        }
        return std::string(input);
    }

    Task<Result<void>> run_common(Provider &p, Source source, std::string_view cmd, std::span<const std::string> args, Loop &loop, creds::Credentials &credentials,
                                  std::move_only_function<Task<Result<Track>>(std::string)> resolve_track) {
        const auto &plat = platform(source);
        auto arg = [&](size_t i) {
            return arg_at(args, i);
        };

        if (cmd == "search") {
            if (args.empty() || args.size() > 3) {
                co_return std::unexpected("Expected search <query> [tracks|albums|artists|playlists] [offset]");
            }
            static constexpr std::array kinds = {"tracks", "albums", "artists", "playlists"};
            auto kind = std::ranges::find(kinds, args.size() > 1 ? args[1] : "tracks");
            if (kind == kinds.end()) {
                co_return std::unexpected("Expected tracks, albums, artists or playlists");
            }
            int offset = co_await ok(page_offset(arg(2)));
            auto type = static_cast<SearchKind>(kind - kinds.begin());
            auto r = co_await ok(co_await p.search(args[0], type, offset, type == SearchKind::tracks ? 30 : 40));
            std::println("{} total, next offset {}", r.total, r.next_offset);
            print_tracks(r.tracks);
            print_cards(r.cards);
            co_return Result<void>{};
        }
        if (cmd == "url") {
            if (args.empty()) {
                co_return std::unexpected("Expected url <id> [standard|higher|exhigh|lossless|hires]");
            }
            Track track = resolve_track ? co_await ok(co_await resolve_track(args[0])) : Track{.id = args[0], .source = source};
            auto r = co_await ok(co_await p.stream(std::move(track), parse_quality(arg(1))));
            std::println("{}\nlevel {}, gain {:+.2f} dB, peak {:.3f}{}", r.url, r.level, r.gain, r.peak,
                         r.trial ? std::format(", trial clip only (from {} s)", r.trial_start) : "");
            co_return Result<void>{};
        }
        if (cmd == "lyrics") {
            if (args.empty()) {
                co_return std::unexpected("Expected lyrics <id>");
            }
            auto r = co_await ok(co_await p.lyrics(Track{.id = args[0], .source = source}));
            for (const auto &l : r) {
                std::println("{:7.3f}  {}{}{}{}", l.time, l.text, l.translation.empty() ? "" : "  /  ", l.translation,
                             l.words.empty() ? "" : std::format("  ({} words)", l.words.size()));
            }
            co_return Result<void>{};
        }
        if (cmd == "album") {
            if (args.empty()) {
                co_return std::unexpected("Expected album <id>");
            }
            auto r = co_await ok(co_await p.album(args[0]));
            std::println("{} ({}) · {} · {} · comments {}\n{}", r.artist, r.artist_id, r.date, r.company, r.thread, prefix(r.about, 300));
            print_tracks(r.tracks);
            co_return Result<void>{};
        }
        if (cmd == "artist") {
            if (args.empty()) {
                co_return std::unexpected("Expected artist <id>");
            }
            auto a = co_await ok(co_await p.artist(args[0]));
            std::println("{} ({}) songs {} albums {} {}", a.name, a.alias, a.songs, a.albums, a.avatar);
            if (auto s = co_await p.artist_songs(args[0], 0)) {
                print_tracks(std::span(s->tracks).first(std::min<size_t>(10, s->tracks.size())));
            }
            if (auto al = co_await p.artist_albums(args[0], 0)) {
                print_cards(al->cards);
            }
            if (auto intro = co_await p.artist_intro(args[0])) {
                for (const auto &[head, text] : *intro) {
                    std::println("## {}\n{}", head, prefix(text, 200));
                }
            }
            co_return Result<void>{};
        }
        if (cmd == "playlist") {
            if (args.empty()) {
                co_return std::unexpected("Expected playlist <id>");
            }
            auto r = co_await ok(co_await p.tracks(Card{.source = source, .id = args[0]}));
            std::println("{} tracks", r.size());
            print_tracks(r);
            co_return Result<void>{};
        }
        if (cmd == "top" || cmd == "recommend") {
            Result<std::vector<Card>> r;
            if (cmd == "top") {
                r = co_await p.toplists();
            } else {
                r = co_await p.recommended_playlists();
            }
            print_cards(co_await ok(std::move(r)));
            co_return Result<void>{};
        }
        if (cmd == "guess") {
            print_tracks(co_await ok(co_await p.fm()));
            co_return Result<void>{};
        }
        if (cmd == "new-songs" || cmd == "new-albums") {
            const bool albums = cmd == "new-albums";
            if (args.size() > (albums ? 2u : 1u)) {
                co_return std::unexpected("Expected new-songs [area] or new-albums [area] [offset]");
            }
            const auto &categories = p.release_categories[albums];
            if (categories.empty()) {
                co_return std::unexpected("Not supported by this service");
            }
            std::string category = args.empty() ? categories.front().id : args[0];
            if (std::ranges::find(categories, category, &Category::id) == categories.end()) {
                co_return std::unexpected("Expected a supported release category ID");
            }
            if (albums) {
                int offset = co_await ok(page_offset(arg(1)));
                auto r = co_await ok(co_await p.new_albums(category, offset));
                std::println("{} total", r.total);
                print_cards(r.cards);
            } else {
                print_tracks(co_await ok(co_await p.new_songs(category)));
            }
            co_return Result<void>{};
        }
        if (cmd == "saved-playlists") {
            print_cards(co_await ok(co_await p.collected_playlists()));
            co_return Result<void>{};
        }
        if (cmd == "categories") {
            auto r = co_await ok(co_await p.categories());
            for (const auto &[heading, categories] : r.groups) {
                std::println("{}", heading);
                for (const auto &category : categories) {
                    std::println("  {}  {}", category.id, category.name);
                }
            }
            co_return Result<void>{};
        }
        if (cmd == "square") {
            int offset = co_await ok(page_offset(arg(1)));
            auto r = co_await ok(co_await p.square(arg(0) == "all" ? "" : std::string(arg(0)), offset));
            std::println("{} total", r.total);
            print_cards(r.cards);
            co_return Result<void>{};
        }
        if (cmd == "similar") {
            if (args.empty()) {
                co_return std::unexpected("Expected similar <id>");
            }
            print_tracks(co_await ok(co_await p.similar(args[0])));
            co_return Result<void>{};
        }
        if (cmd == "collect-playlist") {
            if (args.size() != 2 || (arg(1) != "on" && arg(1) != "off")) {
                co_return std::unexpected("Expected collect-playlist <id> on|off");
            }
            if (!p.logged_in()) {
                co_return std::unexpected("Sign in first");
            }
            co_await ok(co_await p.collect_playlist(args[0], arg(1) == "on"));
            std::println("{}", arg(1) == "on" ? "Playlist saved" : "Playlist removed");
            co_return Result<void>{};
        }
        if (cmd == "comments" || cmd == "replies") {
            const bool replies = cmd == "replies";
            Subject subject = Subject::song;
            std::string target_id, root_id;
            bool hot = true;
            if (!args.empty() && parse_subject(args[0])) {
                subject = *parse_subject(args[0]);
                if (replies ? args.size() != 3 : (args.size() < 2 || args.size() > 3)) {
                    co_return std::unexpected(replies ? "Expected replies <song|album|playlist> <id> <comment id>" : "Expected comments <song|album|playlist> <id> [new]");
                }
                target_id = args[1];
                if (replies) {
                    root_id = args[2];
                } else {
                    hot = arg(2) != "new";
                }
            } else if (replies) {
                if (args.size() != 1 || args[0].empty()) {
                    co_return std::unexpected("Expected replies [song|album|playlist <id>] <comment id>");
                }
                root_id = args[0];
            } else {
                if (args.empty() || args.size() > 2) {
                    co_return std::unexpected("Expected comments [song|album|playlist] <id> [new]");
                }
                target_id = args[0];
                hot = arg(1) != "new";
            }
            std::string cursor;
            for (int page = 1; page <= 2; ++page) {
                Result<CommentPage> res;
                if (replies) {
                    res = co_await p.comment_replies(subject, target_id, root_id, page, cursor);
                } else {
                    res = co_await p.comments(subject, target_id, hot, page, cursor);
                }
                auto r = co_await ok(std::move(res));
                std::println("page {}: {} of {}, more {}, cursor {}", page, r.comments.size(), r.total, r.more, r.cursor);
                for (const auto &c : r.comments) {
                    std::println("  [{}] {} · {} likes{} · {} replies · {}: {}{}", c.time, c.id, c.likes, c.liked ? " (liked)" : "", c.replies, c.user, prefix(c.text, 80),
                                 c.reply.empty() ? "" : " ← " + c.reply_user);
                }
                if (!r.more) {
                    break;
                }
                cursor = std::move(r.cursor);
            }
            co_return Result<void>{};
        }
        if (cmd == "post-comment") {
            if (args.size() < 3 || args.size() > 4) {
                co_return std::unexpected("Expected post-comment <song|album|playlist> <id> <text file> [reply id]");
            }
            auto subject = parse_subject(args[0]);
            if (!subject) {
                co_return std::unexpected("Expected song, album or playlist");
            }
            if (!p.logged_in()) {
                co_return std::unexpected("Sign in before commenting");
            }
            std::ifstream file(args[2]);
            if (!file) {
                co_return std::unexpected("Could not open comment text file");
            }
            std::string content{std::istreambuf_iterator<char>(file), {}};
            if (trim(content).empty()) {
                co_return std::unexpected("Comment cannot be empty");
            }
            auto id = co_await ok(co_await p.post_comment(*subject, args[1], std::move(content), args.size() == 4 ? args[3] : ""));
            std::println("Comment sent: {}", id);
            co_return Result<void>{};
        }
        if (cmd == "like-comment") {
            Subject subject = Subject::song;
            std::string resource, comment, state;
            if (args.size() == 4 && parse_subject(args[0])) {
                subject = *parse_subject(args[0]);
                resource = args[1];
                comment = args[2];
                state = args[3];
            } else if (args.size() == 2) {
                comment = args[0];
                state = args[1];
            }
            if (comment.empty() || (state != "on" && state != "off")) {
                co_return std::unexpected("Expected like-comment [song|album|playlist <id>] <comment id> on|off");
            }
            if (!p.logged_in()) {
                co_return std::unexpected("Sign in first");
            }
            co_await ok(co_await p.like_comment(subject, resource, comment, state == "on"));
            std::println("{}", state == "on" ? "Comment liked" : "Comment like removed");
            co_return Result<void>{};
        }
        if (cmd == "account") {
            auto a = co_await ok(co_await p.account());
            std::println("{} ({}) {}", a.name, a.id, a.avatar);
            co_return Result<void>{};
        }
        if (cmd == "library") {
            std::string uid;
            if (auto a = co_await p.account()) {
                uid = a->id;
            }
            auto r = co_await ok(co_await p.user_playlists(uid));
            print_cards(r.second);
            if (!r.first.id.empty()) {
                auto liked = co_await ok(co_await p.tracks(r.first));
                std::println("{} liked tracks", liked.size());
                print_tracks(liked);
            }
            co_return Result<void>{};
        }
        if (cmd == "collection") {
            print_cards(co_await ok(co_await p.collected_albums()));
            print_cards(co_await ok(co_await p.followed_artists()));
            co_return Result<void>{};
        }
        if (cmd == "login") {
            auto code = co_await ok(co_await p.qr_start());
            std::filesystem::path png_path;
            if (!code.text.empty()) {
                print_qr(code.text);
            } else if (!code.image.empty()) {
                png_path = arg(0).empty() ? paths::state_dir() / std::format("{}-qr.png", plat.id) : std::filesystem::path(arg(0));
                std::ofstream out(png_path, std::ios::binary);
                out.write(code.image.data(), static_cast<std::streamsize>(code.image.size()));
                if (!out) {
                    co_return std::unexpected("Could not save the QR code image");
                }
                std::println("QR code: {}", png_path.string());
            }
            std::println("{}", plat.scan_hint);
            for (QrState last = QrState::waiting;;) {
                co_await lyren::sleep{loop, 2s};
                auto s = co_await ok(co_await p.qr_poll());
                if (s.state != last) {
                    if (s.state == QrState::scanned && !s.nickname.empty()) {
                        std::println("Scanned: {}; confirm on your phone", s.nickname);
                    } else {
                        std::println("state {}", static_cast<int>(s.state));
                    }
                }
                last = s.state;
                if (s.state == QrState::expired) {
                    co_return std::unexpected("QR code expired");
                }
                if (s.state == QrState::confirmed) {
                    break;
                }
            }
            credentials[std::string(plat.id)] = p.session();
            co_await ok(co_await creds::save(loop, creds::to_json(credentials)));
            if (!png_path.empty()) {
                std::filesystem::remove(png_path);
            }
            std::println("Signed in to {}; credentials saved with encryption", plat.name);
            if (auto a = co_await p.account()) {
                std::println("{} ({})", a->name, a->id);
            }
            co_return Result<void>{};
        }
        co_return std::unexpected(std::format("unknown command {}", cmd));
    }

    Task<Result<void>> run_qq_extra(qq::Qq &q, std::string_view cmd, std::span<const std::string> args, Loop &loop, creds::Credentials &credentials) {
        auto arg = [&](size_t i) {
            return arg_at(args, i);
        };
        if (cmd == "radar") {
            print_tracks(co_await ok(co_await q.radar()));
            co_return Result<void>{};
        }
        if (cmd == "reorder") {
            if (args.size() < 2 || !arg(0).starts_with("dir:")) {
                co_return std::unexpected("Expected reorder <dir:ID> <song id>...");
            }
            co_await ok(positive_u64(arg(0).substr(4), "Expected a numeric playlist directory ID"));
            if (!q.logged_in()) {
                co_return std::unexpected("Sign in to QQ Music first");
            }
            auto library = co_await ok(co_await q.user_playlists(q.client.get("musicid")));
            Card playlist;
            if (args[0] == library.first.id) {
                playlist = library.first;
            } else {
                auto it = std::ranges::find(library.second, args[0], &Card::id);
                if (it == library.second.end() || !it->owned) {
                    co_return std::unexpected("Choose a playlist owned by the signed-in QQ account");
                }
                playlist = *it;
            }
            auto songs = co_await ok(co_await q.tracks(playlist));
            if (args.size() - 1 != songs.size()) {
                co_return std::unexpected("Use every song in the playlist exactly once");
            }
            StringHashSet used;
            std::vector<Track> order;
            for (size_t i = 1; i < args.size(); ++i) {
                auto song = std::ranges::find(songs, args[i], &Track::id);
                if (song == songs.end() || !used.insert(args[i]).second) {
                    co_return std::unexpected("Use every song in the playlist exactly once");
                }
                order.push_back(*song);
            }
            auto written = co_await q.reorder_playlist(args[0], order);
            auto fresh = co_await ok(co_await q.tracks(playlist));
            if (!std::ranges::equal(fresh, order, [](const Track &a, const Track &b) {
                    return same(a, b) && a.type == b.type;
                })) {
                co_return std::unexpected(written ? "The server did not keep the requested playlist order" : written.error());
            }
            std::println("Playlist order saved and confirmed");
            print_tracks(fresh);
            co_return Result<void>{};
        }
        if (cmd == "dislikes" || cmd == "dislike") {
            static constexpr std::array<std::string_view, 3> kinds = {"songs", "artists", "styles"};
            if (!q.logged_in()) {
                co_return std::unexpected("Sign in to QQ Music first");
            }
            if (cmd == "dislikes") {
                if (args.size() > 1 || (!arg(0).empty() && arg(0) != "all" && std::ranges::find(kinds, arg(0)) == kinds.end())) {
                    co_return std::unexpected("Expected dislikes [songs|artists|styles|all]");
                }
                for (size_t i = 0; i < kinds.size(); ++i) {
                    if (!arg(0).empty() && arg(0) != "all" && arg(0) != kinds[i]) {
                        continue;
                    }
                    auto r = co_await ok(co_await q.dislikes(static_cast<DislikeKind>(i)));
                    std::println("{}: {} items", kinds[i], r.size());
                    for (const auto &item : r) {
                        std::println("  {}  type {}  {}  {}", item.id, item.id_type, item.name, item.cover);
                    }
                }
                co_return Result<void>{};
            }
            auto category = std::ranges::find(kinds, arg(0));
            if (args.size() < 3 || args.size() > 4 || category == kinds.end() || (arg(2) != "on" && arg(2) != "off") || arg(1).empty()) {
                co_return std::unexpected("Expected dislike <songs|artists|styles> <id> on|off [name]");
            }
            const bool on = arg(2) == "on";
            const auto kind = static_cast<DislikeKind>(category - kinds.begin());
            Dislike item{.id = args[1], .name = std::string(arg(3)), .source = Source::qq, .kind = kind};
            if (on) {
                co_await ok(positive_u64(item.id, "Expected a numeric resource ID"));
            } else {
                auto items = co_await ok(co_await q.dislikes(kind));
                auto found = std::ranges::find(items, item.id, &Dislike::id);
                if (found == items.end()) {
                    co_return std::unexpected("This item is not in the dislike list");
                }
                item = *found;
            }
            co_await ok(co_await q.dislike(std::move(item), on));
            std::println("{}", on ? "Hidden from recommendations" : "Restored to recommendations");
            co_return Result<void>{};
        }
        if (cmd == "qr") {
            auto code = co_await ok(co_await q.qr_start());
            auto path = arg(0).empty() ? paths::state_dir() / "qq-qr.png" : std::filesystem::path(arg(0));
            {
                std::ofstream out(path, std::ios::binary);
                out.write(code.image.data(), static_cast<std::streamsize>(code.image.size()));
                if (!out) {
                    co_return std::unexpected("Could not save the QR code image");
                }
            }
            std::println("QR code: {}\nScan with QQ and confirm QQ Music sign-in", path.string());
            auto s = co_await ok(co_await q.qr_poll());
            std::println("state {}", static_cast<int>(s.state));
            co_return Result<void>{};
        }
        if (cmd == "renew") {
            if (!q.logged_in()) {
                co_return std::unexpected("Sign in to QQ Music first");
            }
            co_await ok(co_await q.renew());
            credentials["qq"] = q.client.session;
            co_await ok(co_await creds::save(loop, creds::to_json(credentials)));
            std::println("QQ Music credentials renewed");
            co_return Result<void>{};
        }
        if (cmd == "call") {
            if (args.size() < 2) {
                co_return std::unexpected("Expected call <module> <method> [param]");
            }
            auto r = co_await q.client.call({{args[0], args[1], args.size() > 2 ? args[2] : "{}"}});
            std::println("{}", r.body);
            if (!r.error.empty()) {
                co_return std::unexpected(r.error);
            }
            co_return Result<void>{};
        }
        co_return std::unexpected(std::format("unknown command {}", cmd));
    }

    Task<Result<void>> run_qishui_check(qishui::Qishui &qs, std::span<const std::string> args) {
        std::string settings_json;
        const std::string first = read_file_or_value(arg_at(args, 0));
        const auto first_non_ws = first.find_first_not_of(" \t\r\n");
        if (first_non_ws != std::string::npos && first[first_non_ws] == '{') {
            settings_json = first;
        } else {
            StringMap<std::string> settings = qs.session();
            settings.erase("user_id");
            if (!first.empty()) {
                settings["signer_url"] = first;
            }
            if (!arg_at(args, 1).empty()) {
                settings.erase("cookie_jar");
                settings["cookie"] = read_file_or_value(arg_at(args, 1));
            }
            settings_json = glz::write_json(settings).value_or("{}");
        }
        const auto result = co_await qs.check_session(std::move(settings_json));
        auto badge = [](SessionCheck::State s) {
            return s == SessionCheck::State::ok ? "[OK]  " : s == SessionCheck::State::skipped ? "[SKIP]" : s == SessionCheck::State::failed ? "[FAIL]" : "[....]";
        };
        std::println("{} signing service : {}", badge(result.signer.state), result.signer.detail);
        std::println("{} device identity : {}", badge(result.identity.state), result.identity.detail.empty() ? "-" : result.identity.detail);
        std::println("{} account session : {}", badge(result.account.state), result.account.detail.empty() ? "-" : result.account.detail);
        if (!result.ok()) {
            co_return std::unexpected("");
        }
        co_return Result<void>{};
    }

    int provider_command(Source source, std::span<const std::string> args) {
        Loop loop;
        net::Http http;
        int rc = 0;
        auto fail = [&](std::string_view e) {
            if (!e.empty()) {
                std::println(stderr, "error: {}", e);
            }
            rc = 1;
        };

        auto run = [&]() -> Task<void> {
            std::unique_ptr<Provider> owned;
            ncm::Netease *ncm_ptr = nullptr;
            qq::Qq *qq_ptr = nullptr;
            qishui::Qishui *qishui_ptr = nullptr;
            kugou::Kugou *kugou_ptr = nullptr;
            bodian::Bodian *bodian_ptr = nullptr;

            switch (source) {
                case Source::netease: {
                    auto p = std::make_unique<ncm::Netease>(loop, http);
                    ncm_ptr = p.get();
                    owned = std::move(p);
                    break;
                }
                case Source::qq: {
                    auto p = std::make_unique<qq::Qq>(loop, http);
                    qq_ptr = p.get();
                    owned = std::move(p);
                    break;
                }
                case Source::qishui: {
                    auto p = std::make_unique<qishui::Qishui>(loop, http);
                    qishui_ptr = p.get();
                    owned = std::move(p);
                    break;
                }
                case Source::kugou: {
                    auto p = std::make_unique<kugou::Kugou>(loop, http);
                    kugou_ptr = p.get();
                    owned = std::move(p);
                    break;
                }
                case Source::bodian: {
                    auto p = std::make_unique<bodian::Bodian>(loop, http);
                    bodian_ptr = p.get();
                    owned = std::move(p);
                    break;
                }
            }

            const std::string_view cmd = args[0];
            const auto rest = args.subspan(1);
            creds::Credentials credentials;
            const bool load_creds = !(qishui_ptr && cmd == "check" && !rest.empty());
            if (load_creds) {
                auto saved = co_await creds::load(loop);
                if (!saved) {
                    if (source == Source::qq) {
                        co_return fail(saved.error());
                    }
                    if (source == Source::netease) {
                        fail(saved.error());
                    }
                } else {
                    credentials = creds::from_json(*saved);
                    owned->session() = credentials[std::string(platform(source).id)];
                }
            }
            if (ncm_ptr) {
                ncm_ptr->client.ensure_fingerprint();
            }
            if (qishui_ptr && cmd == "check") {
                if (auto r = co_await run_qishui_check(*qishui_ptr, rest); !r) {
                    fail(r.error());
                }
                co_return;
            }
            if (auto ready = co_await owned->initialize(); !ready) {
                co_return fail(ready.error());
            }

            if (ncm_ptr && cmd == "call") {
                if (rest.empty()) {
                    co_return fail("Expected call <uri> [fields] [weapi|clientlog]");
                }
                auto api = arg_at(rest, 2);
                auto r = co_await ncm_ptr->client.call(rest[0], std::string(arg_at(rest, 1)),
                                                       api == "weapi"       ? ncm::Api::weapi
                                                       : api == "clientlog" ? ncm::Api::clientlog
                                                                            : ncm::Api::eapi);
                if (!r.error.empty()) {
                    fail(r.error);
                }
                std::println("{}", r.body);
                co_return;
            }
            if (qq_ptr && (cmd == "radar" || cmd == "reorder" || cmd == "dislikes" || cmd == "dislike" || cmd == "qr" || cmd == "renew" || cmd == "call")) {
                if (auto r = co_await run_qq_extra(*qq_ptr, cmd, rest, loop, credentials); !r) {
                    fail(r.error());
                }
                co_return;
            }

            std::move_only_function<Task<Result<Track>>(std::string)> resolve_track;
            if (qq_ptr) {
                resolve_track = [qq_ptr](std::string id) {
                    return qq_ptr->song_detail(std::move(id));
                };
            } else if (kugou_ptr) {
                resolve_track = [kugou_ptr](std::string id) {
                    return kugou_ptr->song_detail(std::move(id));
                };
            } else if (bodian_ptr) {
                resolve_track = [bodian_ptr](std::string id) {
                    return bodian_ptr->song_detail(std::move(id));
                };
            }
            if (auto r = co_await run_common(*owned, source, cmd, rest, loop, credentials, std::move(resolve_track)); !r) {
                fail(r.error());
            }
        };

        auto main_task = [&]() -> Task<void> {
            co_await run();
            loop.quit();
        };
        spawn(main_task());
        loop.run();
        return rc;
    }

    std::optional<Source> parse_provider(std::string_view name) {
        if (name == "ncm" || name == "netease") {
            return Source::netease;
        }
        if (name == "qq") {
            return Source::qq;
        }
        if (name == "qishui") {
            return Source::qishui;
        }
        if (name == "kugou") {
            return Source::kugou;
        }
        if (name == "bodian") {
            return Source::bodian;
        }
        return std::nullopt;
    }

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::erase_if(args, [](const std::string &a) {
        if (a != "--debug") {
            return false;
        }
        log::min_level = log::Level::debug;
        return true;
    });
    if (args.size() >= 2 && args[0] == "play") {
        return play(args[1], {args.begin() + 2, args.end()});
    }
    if (args.size() >= 2) {
        if (auto src = parse_provider(args[0])) {
            return provider_command(*src, std::span(args).subspan(1));
        }
    }
    if (!args.empty()) {
        for (std::string_view cmd :
             {"search",       "url",        "lyrics", "login",    "account", "new-songs", "new-albums", "saved-playlists", "comments", "replies", "post-comment",
              "like-comment", "album",      "artist", "playlist", "top",     "recommend", "guess",      "categories",      "square",   "similar", "collect-playlist",
              "library",      "collection", "call"}) {
            if (args[0] == cmd) {
                return provider_command(Source::netease, args);
            }
        }
    }
    std::println(stderr, "usage: lyren-probe [--debug] play <url|file> [wait:MS seek:S pause play vol:V open:SRC quit ...]\n"
                         "       lyren-probe [ncm|qq|qishui|kugou|bodian] search|url|lyrics|album|artist|playlist|top|recommend|guess ...\n"
                         "       lyren-probe [ncm|qq|qishui|kugou|bodian] new-songs|new-albums|saved-playlists|categories|square|similar|collect-playlist ...\n"
                         "       lyren-probe [ncm|qq|qishui|kugou|bodian] comments|replies|post-comment|like-comment|login|account|library|collection ...\n"
                         "       lyren-probe [ncm] call <uri> [fields] [weapi|clientlog]\n"
                         "       lyren-probe qq call <module> <method> [param] | radar | dislikes | dislike | reorder | qr | renew\n"
                         "       lyren-probe qishui check [signer_url|json] [cookie|cookie_file]");
    return 2;
}
