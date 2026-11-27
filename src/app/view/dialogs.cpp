#include "app/view/dialogs.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>

#include "app/model/matcher.hpp"
#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"
#include "gfx/image.hpp"
#include "sys/paths.hpp"

namespace lyren {

    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;

    namespace {
        std::filesystem::path pictures_dir() {
            const char *home = std::getenv("HOME");
            if (const char *env = std::getenv("XDG_PICTURES_DIR"); env && env[0] == '/') {
                return std::filesystem::path(env) / "Lyren";
            }
            std::filesystem::path user_dirs;
            if (const char *cfg = std::getenv("XDG_CONFIG_HOME"); cfg && cfg[0] == '/') {
                user_dirs = std::filesystem::path(cfg) / "user-dirs.dirs";
            } else if (home && home[0]) {
                user_dirs = std::filesystem::path(home) / ".config" / "user-dirs.dirs";
            }
            if (!user_dirs.empty()) {
                std::ifstream in(user_dirs);
                std::string line;
                while (std::getline(in, line)) {
                    const std::string_view sv = trim(line);
                    constexpr std::string_view prefix = "XDG_PICTURES_DIR=";
                    if (!sv.starts_with(prefix)) {
                        continue;
                    }
                    std::string_view val = sv.substr(prefix.size());
                    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                        val = val.substr(1, val.size() - 2);
                    }
                    if (val.starts_with("$HOME/") && home && home[0]) {
                        return std::filesystem::path(home) / std::string(val.substr(6)) / "Lyren";
                    }
                    if (!val.empty() && val.front() == '/') {
                        return std::filesystem::path(val) / "Lyren";
                    }
                }
            }
            if (home && home[0]) {
                return std::filesystem::path(home) / "Pictures" / "Lyren";
            }
            return paths::cache_dir() / "pictures";
        }
    } // namespace

    void Dialogs::edit_playlist(ui::Ui &ui, Card card, std::vector<Track> tracks) {
        edit_name_ = card.title;
        edit_hidden_ = false;
        edit_focus_ = true;
        edit_source_ = card.id.empty() ? tracks.empty() ? browser_.active() : tracks.front().source : card.source;
        const bool with_privacy = card.id.empty() && provider(edit_source_).capabilities.private_playlists;
        ui.open_modal(400, with_privacy ? 290 : 226, [this, card = std::move(card), tracks = std::move(tracks)](ui::Ui &u, Rect r) mutable {
            editor(u, r, card, tracks);
        });
    }

    void Dialogs::editor(ui::Ui &ui, Rect r, const Card &card, std::vector<Track> &tracks) {
        const auto &t = ui.theme();
        const bool create = card.id.empty();
        dialog_title(ui, r, create ? lyren::tr("New playlist") : lyren::tr("Rename playlist"), lyren::tr(platform(edit_source_).name));
        const ui::Id field = ui.id("playlist-name");
        if (std::exchange(edit_focus_, false)) {
            ui.focus = field;
        }
        bool submit = ui.text_box(field, {r.x, r.y + 66, r.w, 38}, edit_name_, lyren::tr("Playlist name"));
        if (create && provider(edit_source_).capabilities.private_playlists) {
            const float y = r.y + 122;
            ui.label({r.x, y, r.w - 60, 20}, lyren::tr("Private playlist"), t.style(type::body, t.text));
            ui.label({r.x, y + 22, r.w - 60, 18}, lyren::tr("Only you can see it"), t.style(type::caption, t.text_dim));
            ui.toggle(ui.id("playlist-hidden"), {r.right() - 40, y + 9, 40, 22}, edit_hidden_);
        }
        const auto name = trim(edit_name_);
        const bool busy = library_.writing(edit_source_, card.id);
        submit |= dialog_actions(ui, r, create ? lyren::tr("Create") : lyren::tr("Save changes"), !busy && !name.empty());
        if (!submit || name.empty() || busy || !ui.modal_open()) {
            return;
        }
        ui.close_modal();
        if (create) {
            spawn(library_.create_playlist(edit_source_, std::string(name), edit_hidden_, std::move(tracks)));
        } else if (name != card.title) {
            spawn(library_.rename_playlist(card, std::string(name)));
        }
    }

    void Dialogs::confirm_delete(ui::Ui &ui, Card card) {
        std::string detail = lyren::trf("{} will be permanently deleted.", card.title);
        ui.confirm(std::string(lyren::tr("Delete playlist")), std::move(detail), std::string(lyren::tr("Delete")), "trash", [this, card = std::move(card)] {
            browser_.delete_playlist(card);
        });
    }

    void Dialogs::pick_playlist(ui::Ui &ui, std::vector<Track> tracks, uint64_t selection_tag) {
        if (tracks.empty()) {
            return;
        }
        const Source source = tracks.front().source;
        if (!env_.signed_in(source)) {
            return ui.toast(lyren::trf("Sign in to {} to add songs to a playlist", lyren::tr(platform(source).name)), "folder-plus");
        }
        std::string sub = tracks.size() == 1 ? tracks.front().title + " · " + tracks.front().artist : lyren::trf("{} songs · {}", tracks.size(), lyren::tr(platform(source).name));
        ui.open_modal(420, 480, [this, source, selection_tag, sub = std::move(sub), tracks = std::move(tracks)](ui::Ui &u, Rect r) mutable {
            picker(u, r, source, sub, tracks, selection_tag);
        });
    }

    void Dialogs::picker(ui::Ui &ui, Rect r, Source source, std::string_view sub, std::vector<Track> &tracks, uint64_t selection_tag) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        dialog_title(ui, r, lyren::tr("Add to playlist"), sub);
        const auto &playlists = library_.shelf(source).playlists;
        std::vector<const Card *> own;
        own.reserve(playlists.size());
        for (const Card &playlist : playlists) {
            if (playlist.owned) {
                own.push_back(&playlist);
            }
        }
        constexpr float row_h = 56, top = 68;
        const size_t total = own.size() + 1;
        const float list_h = static_cast<float>(total) * row_h;
        ui.resize_modal(420, std::min(ui.height - 120, top + list_h + 48));
        const Rect view{r.x - 8, r.y + top, r.w + 24, r.h - top};
        const float off = ui.begin_scroll(ui.id("picker"), view, list_h);
        const size_t first = static_cast<size_t>(std::max(0.f, std::floor(off / row_h)));
        const size_t last = std::min(total, static_cast<size_t>(std::ceil((off + view.h) / row_h)) + 1);
        auto entry = [&](size_t i, std::string_view title, std::string_view detail, const std::string *art, bool busy) {
            const Rect row{view.x, view.y + static_cast<float>(i) * row_h - off, r.w + 16, row_h};
            const ui::Id id = ui.id(static_cast<uint64_t>(i));
            ui::Enabled idle(ui, !busy, 0.45f);
            const auto it = ui.interact(id, row);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const float hot = ui.anim(id, it.hovered ? 1 : 0, pace::quick);
            if (hot > 0) {
                p.rect(row.inset(2), t.fill(hot), t.radius_s());
            }
            const Rect art_box{row.x + 10, row.y + 8, 40, 40};
            if (art) {
                cover(ui, art_box, *art, t.radius_s(), "list-music");
            } else {
                p.rect(art_box, t.fill(1.4f), t.radius_s());
                p.icon("plus", ui::center(art_box, 18, 18), t.text_dim);
            }
            p.text(art_box.right() + 14, row.y + (detail.empty() ? 34 : 25), title, t.style(type::body, t.text), row.w - 80);
            if (!detail.empty()) {
                p.text(art_box.right() + 14, row.y + 43, detail, t.style(type::caption, t.text_dim), row.w - 80);
            }
            return it.clicked;
        };
        std::optional<Card> picked;
        ui.push_id(ui.id("picker-rows"));
        const bool create = first == 0 && last > 0 && entry(0, lyren::tr("New playlist"), "", nullptr, library_.writing(source, ""));
        for (size_t row = std::max<size_t>(first, 1); row < last; ++row) {
            const size_t i = row - 1;
            if (entry(row, own[i]->title, card_subtitle(*own[i]), &own[i]->cover, library_.writing(source, own[i]->id))) {
                picked = *own[i];
            }
        }
        ui.pop_id();
        ui.end_scroll();
        if (!ui.modal_open()) {
            return;
        }
        if (create) {
            edit_playlist(ui, {}, std::move(tracks));
        } else if (picked) {
            ui.close_modal();
            browser_.add_to_playlist(std::move(tracks), std::move(*picked), selection_tag);
        }
    }

    void Dialogs::link_menu(ui::Ui &ui, float x, float y, const Card &card) {
        auto link = share_url(card);
        if (link.empty()) {
            return;
        }
        std::vector<ui::MenuItem> items;
        items.push_back({"link", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                             ui.copy = link;
                             ui.toast(std::string(lyren::tr("Link copied")), "link");
                         }});
        ui.open_menu(x, y, std::move(items));
    }

    void Dialogs::show_picture(ui::Ui &ui, std::string url, float aspect) {
        const float safe_aspect = std::clamp(aspect > 0 ? aspect : 1.f, 0.25f, 4.f);
        float w = ui.width * 0.84f, h = w / safe_aspect;
        if (h > ui.height * 0.84f) {
            h = ui.height * 0.84f, w = h * safe_aspect;
        }
        ui.open_modal(
            std::round(w), std::round(h),
            [this, url = std::move(url)](ui::Ui &u, Rect r) {
                auto &p = u.painter();
                const auto &t = u.theme();
                const auto &e = images_.picture(url, 1024);
                p.shadow({r.x, r.y + 12, r.w, r.h}, t.radius_l(), 48, t.shadow);
                if (e.ready) {
                    p.image(r, e.tex, t.radius_l(), white, cover_uv(static_cast<float>(e.w) / static_cast<float>(std::max(e.h, 1u)), r.w / r.h));
                } else if (e.failed || url.empty()) {
                    p.rect(r, t.elevated, t.radius_l());
                    empty(u, r, "cloud-off", lyren::tr("Could not load this"));
                } else {
                    p.rect(r, t.elevated, t.radius_l());
                    u.spinner(ui::center(r, 24, 24), t.text_dim, 2);
                }
                float rx = 0, ry = 0;
                if (u.context_click(r, &rx, &ry)) {
                    picture_menu(u, rx, ry, url);
                }
                const auto it = u.interact(u.id("picture-view"), r);
                if (it.hovered) {
                    u.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    u.close_modal();
                }
            },
            true);
    }

    void Dialogs::picture_menu(ui::Ui &ui, float x, float y, std::string url) {
        if (url.empty()) {
            return;
        }
        std::vector<ui::MenuItem> items;
        items.push_back({"copy", std::string(lyren::tr("Copy image")), [this, url] {
                             copy_image(url);
                         }});
        items.push_back({"download", std::string(lyren::tr("Save image")), [this, url = std::move(url)] {
                             save_image(std::move(url));
                         }});
        ui.open_menu(x, y, std::move(items));
    }

    void Dialogs::copy_image(std::string url) {
        if (url.empty()) {
            return;
        }
        Loop *loop = &env_.loop;
        std::weak_ptr<int> alive = life_;
        env_.http.request({.url = std::move(url)}, [this, loop, alive = std::move(alive)](net::Response r) {
            std::string png;
            if (r.error.empty() && r.status >= 200 && r.status < 300 && !r.body.empty()) {
                const auto raw = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(r.body.data()), r.body.size());
                if (gfx::image_extension(raw) == "png") {
                    png = std::move(r.body);
                } else {
                    std::string err;
                    if (const gfx::Image img = gfx::decode_fit(raw, 4096, err); img.w > 0 && img.h > 0 && !img.px.empty()) {
                        png = gfx::encode_png(img);
                    }
                }
            }
            loop->post([this, alive, png = std::move(png)]() mutable {
                if (alive.expired()) {
                    return;
                }
                if (png.empty()) {
                    env_.notify(std::string(lyren::tr("Could not copy image")), "triangle-alert", true);
                    return;
                }
                shell_.pending_copy_data = ui::Ui::CopyData{std::move(png), "image/png"};
                env_.notify(std::string(lyren::tr("Image copied")), "copy");
                env_.redraw();
            });
        });
    }

    void Dialogs::save_image(std::string url) {
        if (url.empty()) {
            return;
        }
        Loop *loop = &env_.loop;
        std::weak_ptr<int> alive = life_;
        env_.http.request({.url = std::move(url)}, [this, loop, alive = std::move(alive)](net::Response r) {
            std::string saved_name;
            if (r.error.empty() && r.status >= 200 && r.status < 300 && !r.body.empty()) {
                const auto raw = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(r.body.data()), r.body.size());
                const std::string_view ext = gfx::image_extension(raw);
                const auto dir = pictures_dir();
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                if (!ec) {
                    const std::time_t now = std::time(nullptr);
                    std::tm local{};
                    localtime_r(&now, &local);
                    const std::string stem =
                        std::format("lyren-{:04}{:02}{:02}-{:02}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
                    std::filesystem::path target = dir / std::format("{}.{}", stem, ext);
                    for (int n = 1; std::filesystem::exists(target, ec) && n < 100; ++n) {
                        target = dir / std::format("{}-{}.{}", stem, n, ext);
                    }
                    std::ofstream out(target, std::ios::binary | std::ios::trunc);
                    if (out.write(r.body.data(), static_cast<std::streamsize>(r.body.size())) && out.good()) {
                        saved_name = target.filename().string();
                    }
                }
            }
            loop->post([this, alive, saved_name = std::move(saved_name)]() mutable {
                if (alive.expired()) {
                    return;
                }
                if (saved_name.empty()) {
                    env_.notify(std::string(lyren::tr("Could not save image")), "triangle-alert", true);
                    return;
                }
                env_.notify(lyren::trf("Image saved: {}", saved_name), "download");
            });
        });
    }

    struct Dialogs::Alternatives {
        struct Row {
            SourceKey key;
            std::string name;
            bool searching = true;
            bool probing = false;
            std::optional<Track> match;
            std::optional<Stream> stream;
            std::string error;
        };
        Track target;
        std::vector<Row> rows;
    };

    void Dialogs::find_alternatives(ui::Ui &ui, Track track) {
        auto state = std::make_shared<Alternatives>();
        state->target = std::move(track);
        for (const auto &entry : env_.providers.entries()) {
            if (entry.source == state->target.source && entry.instance == state->target.instance) {
                continue;
            }
            if (!entry.provider->capabilities.supports_search(SearchKind::tracks)) {
                continue;
            }
            state->rows.push_back({.key = {entry.source, entry.instance}, .name = std::string(lyren::tr(entry.name))});
        }
        if (state->rows.empty()) {
            ui.toast(std::string(lyren::tr("No other platforms available")), "arrow-left-right");
            return;
        }
        const Quality quality = env_.config.playback.quality;
        for (size_t i = 0; i < state->rows.size(); ++i) {
            spawn(probe_alternative(state, i, state->target, state->rows[i].key, quality));
        }
        ui.open_modal(520, 360, [this, state](ui::Ui &u, Rect r) {
            alternatives_modal(u, r, state);
        });
    }

    Task<void> Dialogs::probe_alternative(std::weak_ptr<Alternatives> weak, size_t index, Track target, SourceKey source, Quality quality) {
        const std::weak_ptr<int> alive = life_;
        const std::string q = match_query(target);
        if (q.empty()) {
            if (auto st = weak.lock(); st && !alive.expired()) {
                st->rows[index].searching = false;
                st->rows[index].error = "No matching song found";
                env_.redraw();
            }
            co_return;
        }
        Provider &prov = env_.provider(source.source, source.instance);
        auto res = co_await prov.search(q, SearchKind::tracks, 0, 10);
        if (alive.expired() || weak.expired()) {
            co_return;
        }
        std::optional<Track> best;
        int best_score = -1;
        if (res) {
            for (const auto &cand : res->tracks) {
                if (const auto m = score_match(target, cand); m.acceptable && m.score > best_score) {
                    best_score = m.score;
                    best = cand;
                }
            }
        }
        if (!best) {
            if (auto st = weak.lock()) {
                st->rows[index].searching = false;
                st->rows[index].error = res ? "No matching song found" : "Currently unavailable";
                env_.redraw();
            }
            co_return;
        }
        if (auto st = weak.lock()) {
            st->rows[index].match = *best;
            st->rows[index].searching = false;
            st->rows[index].probing = true;
            env_.redraw();
        }
        auto stream = co_await prov.stream(*best, quality);
        if (alive.expired()) {
            co_return;
        }
        if (auto st = weak.lock()) {
            auto &row = st->rows[index];
            row.probing = false;
            if (stream) {
                row.stream = std::move(*stream);
            } else {
                row.error = "Currently unavailable";
            }
            env_.redraw();
        }
    }

    void Dialogs::alternatives_modal(ui::Ui &ui, Rect r, const std::shared_ptr<Alternatives> &state) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const std::string sub = state->target.title + (state->target.artist.empty() ? "" : " · " + state->target.artist);
        dialog_title(ui, r, lyren::tr("Find on other platforms"), sub);
        constexpr float top = 66.f, row_h = 64.f;
        p.rect({r.x, r.y + top - 6, r.w, 1}, t.line(0.8f));
        const float list_h = static_cast<float>(state->rows.size()) * row_h;
        ui.resize_modal(std::min(520.f, ui.width - 40), std::min(ui.height - 96, top + list_h + 44));

        const Rect view{r.x - 6, r.y + top, r.w + 12, std::max(0.f, r.h - top)};
        const bool scrolls = list_h > view.h + 1;
        const float content_w = view.w - (scrolls ? 10.f : 0.f);
        const float off = ui.begin_scroll(ui.id("alternatives-scroll"), view, list_h);
        const auto play_label = lyren::tr("Play");
        const float play_w = ui.button_width(play_label, true);

        std::optional<Track> play_now_track, play_next_track;
        ui.push_id(ui.id("alternatives-rows"));
        for (size_t i = 0; i < state->rows.size(); ++i) {
            const auto &row = state->rows[i];
            const Rect rr{view.x, view.y + static_cast<float>(i) * row_h - off, content_w, row_h};
            const bool active = row.searching || row.probing;
            const bool playable = !active && row.match && row.stream;
            const ui::Id rid = ui.id(static_cast<uint64_t>(i));
            if (playable) {
                const float hot = ui.anim(rid, ui.hovering(rr) ? 1.f : 0.f, pace::quick);
                if (hot > 0) {
                    p.rect(rr.inset(0, 2), t.fill(0.7f * hot), t.radius_s());
                }
            }

            const Rect art{rr.x + 6, rr.y + (rr.h - 40) / 2, 40, 40};
            if (row.match) {
                Fade f(p, playable || active ? 1.f : 0.55f);
                cover(ui, art, row.match->cover, t.radius_s());
            } else {
                p.rect(art, t.fill(0.65f), t.radius_s());
                p.icon("music", ui::center(art, 16, 16), t.faint());
            }

            float right_x = rr.right() - 6;
            if (active) {
                ui.spinner(ui::center({right_x - 28, rr.y, 28, rr.h}, 16, 16), t.text_dim, 2);
                right_x -= 36;
            } else if (playable) {
                const Rect next_r{right_x - ui::control::small, rr.y + (rr.h - ui::control::small) / 2, ui::control::small, ui::control::small};
                if (ui.icon_button(ui::sub_id(rid, 0x2), next_r, "list-start", 15, false, lyren::tr("Play next"))) {
                    play_next_track = *row.match;
                }
                const Rect play_r{next_r.x - 6 - play_w, rr.y + (rr.h - ui::control::small) / 2, play_w, ui::control::small};
                if (ui.button(ui::sub_id(rid, 0x1), play_r, play_label, ui::ButtonKind::secondary, "play-fill")) {
                    play_now_track = *row.match;
                }
                right_x = play_r.x - 10;
            }

            const float tx = art.right() + 12;
            const float tw = std::max(0.f, right_x - tx);
            const auto name_st = t.style(type::body, playable || active ? t.text : t.text_dim, playable ? text::medium : 0);
            const auto sub_st = t.style(type::caption, playable || active ? t.text_dim : t.faint());

            std::string_view tag;
            bool high_quality = false;
            if (!active) {
                if (row.stream) {
                    tag = row.stream->trial ? lyren::tr("Preview") : !row.stream->level.empty() ? quality_name(row.stream->level) : lyren::tr("Playable");
                    high_quality =
                        !row.stream->trial && (row.stream->level == "lossless" || row.stream->level == "hires" || row.stream->level == "jyeffect" || row.stream->level == "sky" ||
                                               row.stream->level == "jymaster" || row.stream->level == "flac" || row.stream->level == "high");
                } else if (row.match) {
                    tag = lyren::tr("Unavailable");
                }
            }
            const float nw = p.text(tx, rr.y + 27, row.name, name_st, tw);
            if (!tag.empty() && nw + 16 < tw) {
                const float bx = tx + nw;
                const auto sep_st = t.style(type::caption, t.faint());
                const auto tag_st = t.style(type::caption, high_quality ? t.accent : row.stream && !row.stream->trial ? t.text_dim : t.faint(), high_quality ? text::medium : 0);
                const float sw = p.text(bx, rr.y + 27, " · ", sep_st, tw - nw);
                p.text(bx + sw, rr.y + 27, tag, tag_st, std::max(0.f, tw - nw - sw));
            }

            std::string detail;
            if (row.searching) {
                detail = std::string(lyren::tr("Searching..."));
            } else if (row.probing && row.match) {
                detail = std::format("{} · {}", row.match->title, lyren::tr("Checking playback..."));
            } else if (row.match) {
                detail = row.match->title;
                if (!row.match->artist.empty()) {
                    detail += " · " + row.match->artist;
                }
                if (row.match->duration > 0) {
                    detail += " · ";
                    detail += std::string_view(mmss(row.match->duration));
                }
            } else {
                detail = std::string(lyren::tr(row.error.empty() ? "No matching song found" : row.error));
            }
            p.text(tx, rr.y + 47, detail, sub_st, tw);

            if (i + 1 < state->rows.size()) {
                p.rect({tx, rr.bottom() - 1, std::max(0.f, rr.right() - 6 - tx), 1}, t.line(0.45f));
            }
        }
        ui.pop_id();
        ui.end_scroll();

        if (play_now_track) {
            playback_.play_now(std::move(*play_now_track));
            ui.close_modal();
        } else if (play_next_track) {
            playback_.play_next(std::move(*play_next_track));
            ui.toast(std::string(lyren::tr("Added to play next")), "list-start");
        }
    }

} // namespace lyren
