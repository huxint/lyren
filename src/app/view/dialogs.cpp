#include "app/view/dialogs.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>

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

} // namespace lyren
