#include "app/view/dialogs.hpp"

#include <algorithm>
#include <optional>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;

    void Dialogs::edit_playlist(ui::Ui &ui, Card card, std::vector<Track> tracks) {
        edit_name_ = card.title;
        edit_hidden_ = false;
        edit_focus_ = true;
        edit_tracks_ = std::move(tracks);
        edit_source_ = card.id.empty() ? edit_tracks_.empty() ? browser_.active() : edit_tracks_.front().source : card.source;
        const bool with_privacy = card.id.empty() && provider(edit_source_).capabilities.private_playlists;
        ui.open_modal(400, with_privacy ? 290 : 226, [this, card](ui::Ui &u, Rect r) {
            editor(u, r, card);
        });
    }

    void Dialogs::editor(ui::Ui &ui, Rect r, Card card) {
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
        std::string name = edit_name_;
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        const bool busy = library_.writing(edit_source_, card.id);
        submit |= dialog_actions(ui, r, create ? lyren::tr("Create") : lyren::tr("Save changes"), !busy && !name.empty());
        if (!submit || name.empty() || busy) {
            return;
        }
        ui.close_modal();
        if (create) {
            spawn(library_.create_playlist(edit_source_, std::move(name), edit_hidden_, edit_tracks_));
        } else if (name != card.title) {
            spawn(library_.rename_playlist(std::move(card), std::move(name)));
        }
    }

    void Dialogs::confirm_delete(ui::Ui &ui, Card card) {
        std::string detail = lyren::trf("{} will be permanently deleted.", card.title);
        ui.confirm(std::string(lyren::tr("Delete playlist")), std::move(detail), std::string(lyren::tr("Delete")), "trash", [this, card = std::move(card)] {
            browser_.delete_playlist(card);
        });
    }

    void Dialogs::pick_playlist(ui::Ui &ui, std::vector<Track> tracks) {
        if (tracks.empty()) {
            return;
        }
        const Source source = tracks.front().source;
        if (!env_.signed_in(source)) {
            return ui.toast(lyren::trf("Sign in to {} to add songs to a playlist", lyren::tr(platform(source).name)), "folder-plus");
        }
        ui.open_modal(420, 480, [this, tracks = std::move(tracks)](ui::Ui &u, Rect r) {
            picker(u, r, tracks);
        });
    }

    void Dialogs::picker(ui::Ui &ui, Rect r, const std::vector<Track> &tracks) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Source source = tracks.front().source;
        dialog_title(ui, r, lyren::tr("Add to playlist"),
                     tracks.size() == 1 ? tracks.front().title + " · " + tracks.front().artist : lyren::trf("{} songs · {}", tracks.size(), lyren::tr(platform(source).name)));
        std::vector<const Card *> own;
        for (const Card &playlist : library_.shelf(source).playlists) {
            if (playlist.owned) {
                own.push_back(&playlist);
            }
        }
        constexpr float row_h = 56, top = 68;
        const float list_h = static_cast<float>(own.size() + 1) * row_h;
        ui.resize_modal(420, std::min(ui.height - 120, top + list_h + 48));
        const Rect view{r.x - 8, r.y + top, r.w + 24, r.h - top};
        const float off = ui.begin_scroll(ui.id("picker"), view, list_h);
        auto entry = [&](size_t i, std::string_view title, std::string_view sub, const std::string *art, bool busy) {
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
                p.rect(art_box, {}, t.radius_s(), t.surface_edge(0.8f), 1);
            } else {
                p.rect(art_box, t.fill(1.4f), t.radius_s(), t.surface_edge(0.8f), 1);
                p.icon("plus", ui::center(art_box, 18, 18), t.text_dim);
            }
            p.text(art_box.right() + 14, row.y + (sub.empty() ? 34 : 25), title, t.style(type::body, t.text), row.w - 80);
            if (!sub.empty()) {
                p.text(art_box.right() + 14, row.y + 43, sub, t.style(type::caption, t.text_dim), row.w - 80);
            }
            return it.clicked;
        };
        std::optional<Card> picked;
        ui.push_id(ui.id("picker-rows"));
        const bool create = entry(0, lyren::tr("New playlist"), "", nullptr, library_.writing(source, ""));
        for (size_t i = 0; i < own.size(); ++i) {
            if (entry(i + 1, own[i]->title, card_subtitle(*own[i]), &own[i]->cover, library_.writing(source, own[i]->id))) {
                picked = *own[i];
            }
        }
        ui.pop_id();
        ui.end_scroll();
        if (create) {
            edit_playlist(ui, {}, tracks);
        } else if (picked) {
            ui.close_modal();
            browser_.add_to_playlist(tracks, std::move(*picked));
        }
    }

    void Dialogs::link_menu(ui::Ui &ui, float x, float y, const Card &card) {
        auto link = share_url(card);
        if (link.empty()) {
            return;
        }
        ui.open_menu(x, y, {{"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                 ui.copy = link;
                                 ui.toast(std::string(lyren::tr("Link copied")), "copy");
                             }}});
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
                const auto &e = env_.images.picture(url, 1024);
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
                p.rect(r, {}, t.radius_l(), t.surface_edge(1.2f), 1);
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

} // namespace lyren
