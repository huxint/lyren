#include "app/view/side_panel.hpp"

#include <algorithm>
#include <cmath>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

// The panel beside the page and the stage: the play queue, or the comments.
namespace lyren {

    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;

    float SidePanel::width(Kind kind) {
        return kind == Kind::queue ? 336 : kind == Kind::comments ? 408 : 0;
    }

    void SidePanel::show(Kind kind) {
        if (kind == panel_) {
            return;
        }
        if (panel_ != Kind::none && kind != Kind::none) {
            panel_from_ = panel_, panel_time_ = env_.now();
        }
        panel_ = kind;
        if (kind != Kind::none) {
            panel_last_ = kind;
        }
        env_.redraw();
    }

    void SidePanel::draw(ui::Ui &ui, Rect slot, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // On the stage the panel is a pane of glass over the cover, standing clear of the player below it.
        const Rect frame{slot.x + 8, slot.y, slot.w - 8, slot.h - std::round(4 * stage)};
        p.rect(frame, t.surface, t.radius_l());
        ui.push_clip(frame);
        auto content = [&](Kind kind, Rect r) {
            if (kind == Kind::queue) {
                queue(ui, r);
            } else {
                shell_.comments_view.draw(ui, r);
            }
        };
        // Switching between the two: the old one fades out as the new one comes in from the right.
        const float in = ui.progress(panel_time_, pace::glide);
        if (in < 1 && panel_from_ != Kind::none && panel_from_ != panel_last_) {
            Fade out(p, std::max(0.f, 1 - in * 2.5f));
            ui::Enabled inert(ui, false, 1);
            content(panel_from_, {frame.x, frame.y, width(panel_from_) - 8, frame.h});
        }
        {
            Fade fade(p, in);
            content(panel_last_, {frame.x + (1 - in) * 16, frame.y, width(panel_last_) - 8, frame.h});
        }
        ui.pop_clip();
    }

    void SidePanel::queue(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &songs = playback_.queue();
        const int current = playback_.index();

        const auto title = t.style(type::heading, t.text, text::semibold);
        const float title_w = p.measure(lyren::tr("Queue"), title);
        ui.label({r.x + 20, r.y + 16, title_w + 1, 28}, lyren::tr("Queue"), title);
        if (!songs.empty()) {
            ui.label({r.x + 28 + title_w, r.y + 17, 80, 28}, std::to_string(songs.size()), t.style(type::label, t.faint(), text::tabular_nums));
            const std::string_view clear = lyren::tr("Clear");
            const float w = ui.button_width(clear);
            if (ui.button(ui.id("queue-clear"), {r.right() - 14 - w, r.y + 16, w, ui::control::small}, clear, ui::ButtonKind::ghost)) {
                playback_.clear();
                ui.toast(std::string(lyren::tr("Queue cleared")), "trash");
                return;
            }
        }
        p.rect({r.x + 20, r.y + 52, r.w - 40, 1}, t.line());
        constexpr float row_h = 54;
        const Rect view{r.x + 8, r.y + 58, r.w - 16, r.h - 66};
        if (songs.empty()) {
            return empty(ui, view, "list-music", lyren::tr("Nothing queued"), lyren::tr("Songs you play line up here"));
        }
        const ui::Id scroll = ui.id("queue");
        const float off = ui.begin_scroll(scroll, view, static_cast<float>(songs.size()) * row_h);
        int remove = -1, jump = -1;
        // One highlight glides to the playing row.
        const float lit = ui.anim(ui.id("queue-lit"), current >= 0 ? 1 : 0, pace::steady);
        if (lit > 0) {
            const float y = ui.spring(ui.id("queue-current"), static_cast<float>(std::max(current, 0)) * row_h, 260);
            p.rect({view.x + 2, view.y + y - off + 2, view.w - 4, row_h - 4}, t.accent.with_alpha(0.12f * lit), t.radius_s());
        }
        ui.push_id(ui.id("queue-rows"));
        const int first = static_cast<int>(off / row_h), last = static_cast<int>((off + view.h) / row_h) + 1;
        for (int i = first; i <= last && i < static_cast<int>(songs.size()); ++i) {
            const Track &track = songs[static_cast<size_t>(i)];
            const Rect row{view.x, view.y + static_cast<float>(i) * row_h - off, view.w, row_h};
            ui.push_id(ui.id(static_cast<uint64_t>(i)));
            const ui::Id row_id = ui.id("row"), remove_id = ui.id("remove");
            // Held, the row stops counting as hovered, so the button has to keep itself alive until its release lands.
            const bool pointed = ui.hovering(row) || ui.is_active(row_id) || ui.is_active(remove_id), on = i == current;
            const float hot = ui.anim(row_id, pointed ? 1 : 0, pace::quick);
            if (!on && hot > 0) {
                p.rect(row.inset(2), t.fill(hot), t.radius_s());
            }
            const Rect thumb{row.x + 10, row.y + 9, 36, 36};
            cover(ui, thumb, track.cover, t.radius_s());
            const float text_w = row.w - 58 - (pointed || on ? 44 : 12);
            p.text(row.x + 58, row.y + 23, track.title, t.style(type::label, on ? t.accent : t.text, on ? text::medium : 0), text_w);
            p.text(row.x + 58, row.y + 41, track.artist, t.style(type::caption, t.text_dim), text_w);
            if (pointed) {
                if (ui.icon_button(remove_id, {row.right() - 38, row.y + 13, 28, 28}, "x", 14, false, lyren::tr("Remove"))) {
                    remove = i;
                }
            } else if (on) {
                ui.equalizer(ui.id("playing"), {row.right() - 31, row.y + 20, 14, 14}, t.accent, playback_.playing());
            }
            const auto row_it = ui.interact(row_id, row);
            if (row_it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (row_it.double_clicked) {
                jump = i;
            } else if (row_it.right_clicked) {
                shell_.tracks.menu(ui, row_it.px, row_it.py, songs, i);
            }
            ui.pop_id();
        }
        ui.pop_id();
        if (off > 0.5f && t.surface.a > 0.9f) {
            p.gradient({view.x, view.y, view.w, 12}, t.surface.with_alpha(0.9f), t.surface.with_alpha(0));
        }
        ui.end_scroll();
        if (remove >= 0) {
            playback_.remove(remove);
        } else if (jump >= 0) {
            playback_.jump(jump);
        }
    }

} // namespace lyren
