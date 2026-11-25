#include "app/view/comments_view.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <inplace_vector>
#include <string_view>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"
#include "core/utf8.hpp"

// The comments on a song, an album, a playlist or a post, in the panel beside the page and the stage.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;
    using Feed = Comments::Feed;

    namespace {
        struct CommentStyle {
            static constexpr float inset = 20, padding = 16, gap = 8, small_gap = 4;
            static constexpr float avatar = 32, art = 40, icon = 16, small_icon = 14, btn = 30;
            static constexpr float picture_w = 240, picture_h = 220, sticker = 76;
            static constexpr size_t preview_limit = 480;
        };

        std::string_view utf8_prefix(std::string_view text, size_t limit) {
            if (text.size() <= limit) {
                return text;
            }
            size_t cut = limit;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) {
                --cut;
            }
            return text.substr(0, cut);
        }

        std::string_view first_line(std::string_view text) {
            const size_t start = text.find_first_not_of(" \t\r\n");
            if (start == text.npos) {
                return {};
            }
            const auto rest = text.substr(start);
            return rest.substr(0, rest.find('\n'));
        }

        float comment_action_width(ui::Ui &ui, std::string_view label) {
            using C = CommentStyle;
            const auto style = ui.theme().style(type::caption, {}, text::tabular_nums);
            const float text = label.empty() ? 0 : std::ceil(ui.painter().measure(label, style)) + C::small_gap + 2;
            return C::small_icon + text;
        }

        // An icon and a quiet label. icon_scale gently emphasizes a like landing.
        bool comment_action(ui::Ui &ui, ui::Id id, Rect r, std::string_view label, std::string_view icon, ui::Color color, float icon_scale = 1) {
            using C = CommentStyle;
            auto &p = ui.painter();
            const auto &t = ui.theme();
            const auto it = ui.interact(id, r.inset(-C::small_gap, 0));
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const float hot = ui.anim(id, it.hovered ? 1 : 0, pace::quick);
            color = color.mix(t.text, hot * 0.65f);
            const float s = C::small_icon * icon_scale;
            p.icon(icon, {r.x + (C::small_icon - s) / 2, r.y + (r.h - s) / 2, s, s}, color);
            if (!label.empty()) {
                ui.label({r.x + C::small_icon + C::small_gap + 2, r.y, std::max(0.f, r.w - C::small_icon - C::small_gap - 2), r.h}, label,
                         t.style(type::caption, color, text::tabular_nums));
            }
            return it.clicked;
        }

        float replies_link_width(ui::Ui &ui, std::string_view label) {
            using C = CommentStyle;
            return std::ceil(ui.painter().measure(label, ui.theme().style(type::caption, {}, text::medium))) + C::small_icon + C::gap * 2;
        }

        // Quiet inline link to open a comment's reply thread; gains a subtle neutral hover fill only under the pointer.
        bool replies_link(ui::Ui &ui, ui::Id id, Rect r, std::string_view label) {
            using C = CommentStyle;
            auto &p = ui.painter();
            const auto &t = ui.theme();
            const auto it = ui.interact(id, r);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const auto [hot, down] = ui.feel(id, it, pace::quick);
            const Rect b = r.inset(down, down);
            if (hot > 0.01f) {
                p.rect(b, t.fill(1.1f * hot), t.round(b.h));
            }
            const ui::Color ink = t.text_dim.mix(t.text, 0.7f * hot);
            const float text_w = std::max(0.f, b.w - C::gap * 2 - C::small_icon);
            ui.label({b.x + C::gap, b.y, text_w, b.h}, label, t.style(type::caption, ink, text::medium));
            p.icon("chevron-right", {b.right() - C::gap - C::small_icon + 1.5f * hot, b.y + (b.h - C::small_icon) / 2, C::small_icon, C::small_icon}, ink.with_alpha(0.75f));
            return it.clicked;
        }

        // Where a comment's picture goes in a column w wide: its own shape within bounds, or 4:3 until it is known.
        ui::Rect picture_box(const Comment &c, float w) {
            using C = CommentStyle;
            if (c.image.empty() || w <= 0) {
                return {};
            }
            if (c.sticker) {
                return {0, 0, C::sticker, C::sticker};
            }
            float aspect = c.image_w > 0 && c.image_h > 0 ? static_cast<float>(c.image_w) / static_cast<float>(c.image_h) : 4.f / 3;
            aspect = std::clamp(aspect, 0.56f, 2.2f);
            const float mw = std::min(w, C::picture_w), mh = C::picture_h;
            return aspect >= mw / mh ? Rect{0, 0, mw, std::round(mw / aspect)} : Rect{0, 0, std::round(mh * aspect), mh};
        }

        // Keystroke consumption for Escape and Ctrl+Enter inside the docked composer.
        bool take_key(std::vector<wl::KeyEvent> &keys, auto &&pred) {
            const auto at = std::ranges::find_if(keys, pred);
            if (at == keys.end()) {
                return false;
            }
            keys.erase(at);
            return true;
        }
    } // namespace

    const LyricLine *CommentsView::lyric() {
        const Track *tr = playback_.current();
        if (!tr || !thread().is(tr->source, Subject::song, tr->id)) {
            return nullptr;
        }
        return playback_.line_at(*tr, playback_.song_time() + 0.2);
    }

    float CommentsView::header(ui::Ui &ui, Rect r, bool compact) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float art = compact ? 36.f : C::art;
        const Rect thumb{r.x + C::inset, r.y + (compact ? 12.f : 16.f), art, art};
        static constexpr std::array placeholders = {"music", "disc-album", "list-music", "message-square"};
        cover(ui, thumb, thread().cover, t.radius_s(), placeholders[static_cast<size_t>(thread().subject)]);

        const Rect close{r.right() - C::inset - C::btn + 6, thumb.y + (art - C::btn) / 2, C::btn, C::btn};
        const float tx = thumb.right() + 12;
        const float tw = std::max(0.f, close.x - tx - C::gap);
        const float title_h = std::ceil(t.fs(type::lead) * 1.4f), sub_h = std::ceil(t.fs(type::caption) * 1.45f);
        const float ty = thumb.y + (art - title_h - sub_h - 1) / 2;
        if (tw > 0) {
            ui.label({tx, ty, tw, title_h}, thread().title, t.style(type::lead, t.text, text::semibold));
            std::string sub(lyren::tr(platform(thread().source).name));
            const Feed &shown = thread().feeds[thread().tab]->page ? *thread().feeds[thread().tab] : *thread().feeds[1 - thread().tab];
            if (shown.page || shown.total > 0) {
                sub += " · " + lyren::trf("{} comments", grouped(shown.total));
            }
            ui.label({tx, ty + title_h + 1, tw, sub_h}, sub, t.style(type::caption, t.text_dim, text::tabular_nums));
        }
        if (ui.icon_button(ui.id("comments-close"), close, "x", C::icon, false, lyren::tr("Close"))) {
            shell_.panel.show(SidePanel::Kind::none);
        }

        const float tab_h = 36;
        const Rect bar{r.x + C::inset, thumb.bottom() + (compact ? 6.f : 10.f), std::max(0.f, r.w - C::inset * 2), tab_h};
        const bool sortable = provider(thread().source).capabilities.comment_sort;
        std::inplace_vector<std::string_view, 2> labels;
        labels.push_back(lyren::tr(sortable ? "Popular" : "Comments"));
        if (sortable) {
            labels.push_back(lyren::tr("Newest"));
        }
        int selected = sortable ? thread().tab : 0;
        const float tabs_w = std::max(0.f, bar.w - C::btn - C::gap);
        if (tabs_w > 0 && ui.tabs(ui.id("comment-tabs"), {bar.x, bar.y, tabs_w, bar.h}, labels, selected) && sortable) {
            comments_.set_tab(selected);
            ui.redraw();
        }
        {
            ui::Enabled ready(ui, !thread().feeds[thread().tab]->loading);
            if (ui.icon_button(ui.id("comments-refresh"), {bar.right() - C::btn + 6, bar.y + (bar.h - C::btn) / 2, C::btn, C::btn}, "refresh-cw", C::small_icon, false,
                               lyren::tr("Refresh"))) {
                comments_.refresh();
            }
        }
        p.rect({r.x + C::inset, bar.bottom(), std::max(0.f, r.w - C::inset * 2), 1}, t.line());
        return bar.bottom() + 1 - r.y;
    }

    float CommentsView::replies_header(ui::Ui &ui, Rect r, const std::shared_ptr<Comments::Replies> &reply, bool compact) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect bar{r.x + C::inset, r.y + C::padding - 4, std::max(0.f, r.w - C::inset * 2), C::btn};
        if (ui.icon_button(ui.id("replies-back"), {bar.x - 6, bar.y, C::btn, C::btn}, "chevron-left", C::icon + 2, false, lyren::tr("Back"))) {
            thread().replies.reset();
            ui.redraw();
            return -1;
        }
        const int total = !reply->feed->page ? reply->root.replies : reply->feed->total;
        const auto heading = lyren::tr("Replies");
        const auto head_style = t.style(type::heading, t.text, text::semibold);
        const float hw = std::ceil(p.measure(heading, head_style));
        ui.label({bar.x + C::btn, bar.y, hw + 1, bar.h}, heading, head_style);
        if (total > 0) {
            ui.label({bar.x + C::btn + hw + C::gap, bar.y + 1, 80, bar.h}, grouped(total), t.style(type::caption, t.faint(), text::tabular_nums));
        }
        {
            ui::Enabled ready(ui, !reply->feed->loading);
            if (ui.icon_button(ui.id("comments-refresh"), {bar.right() - C::btn * 2 - C::small_gap + 6, bar.y, C::btn, C::btn}, "refresh-cw", C::small_icon, false,
                               lyren::tr("Refresh"))) {
                reply->feed = std::make_shared<Feed>();
                comments_.load_replies(reply);
            }
        }
        if (ui.icon_button(ui.id("comments-close"), {bar.right() - C::btn + 6, bar.y, C::btn, C::btn}, "x", C::icon, false, lyren::tr("Close"))) {
            shell_.panel.show(SidePanel::Kind::none);
        }

        // Pinned root comment: clean typographic layout, separated by a hairline without decorative cards or bars.
        const float ry = bar.bottom() + 10;
        const float tx = bar.x + 24 + 10;
        const float tw = std::max(0.f, bar.right() - tx);
        cover(ui, {bar.x, ry + 1, 24, 24}, reply->root.avatar, 12, "user");
        const auto name_style = t.style(type::label, t.text, text::medium);
        const float nw = std::min(std::ceil(p.measure(reply->root.user, name_style)) + 1, tw);
        if (nw > 0) {
            ui.label({tx, ry, nw, 22}, reply->root.user, name_style);
        }
        if (const float time_x = tx + nw + C::gap; time_x < bar.right() && !reply->root.time.empty()) {
            ui.label({time_x, ry + 1, bar.right() - time_x, 22}, reply->root.time, t.style(type::micro, t.faint()));
        }
        const float line_h = std::round(t.fs(type::label) * 1.55f);
        const std::string_view root_preview = utf8_prefix(reply->root.text, C::preview_limit);
        const auto lines = tw > 0 && !root_preview.empty() ? p.wrap_lines(root_preview, t.fs(type::label), tw) : std::vector<std::string_view>{};
        const size_t max_lines = compact ? 1 : 3;
        const size_t shown = std::min(lines.size(), max_lines);
        float y = ry + 24;
        for (size_t i = 0; i < shown; ++i) {
            std::string_view line = lines[i];
            if (i + 1 == max_lines && lines.size() > max_lines) {
                const std::string_view rest = root_preview.substr(static_cast<size_t>(line.data() - root_preview.data()));
                line = rest.substr(0, rest.find('\n'));
            }
            ui.label({tx, y, tw, line_h}, line, t.style(type::label, t.text_dim));
            y += line_h;
        }
        y += 12;
        p.rect({r.x + C::inset, y, std::max(0.f, r.w - C::inset * 2), 1}, t.line());
        return y + 1 - r.y;
    }

    // Docked bottom writing tray: rests as a calm single-line prompt and smoothly expands in place when focused.
    float CommentsView::composer(ui::Ui &ui, Rect r) {
        if (!provider(thread().source).capabilities.comment_post || r.h < 54) {
            return 0;
        }
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto view = thread().replies;
        auto draft = view ? view->draft : thread().draft;
        const bool signed_in = provider(thread().source).logged_in();
        const bool live_panel = shell_.panel.kind() == SidePanel::Kind::comments;
        const ui::Id field = ui.id(view ? "reply-draft" : "comment-draft");

        constexpr float closed_h = 56;
        if (!signed_in) {
            draft->expanded = draft->focus = false;
            if (ui.focus == field) {
                ui.focus = 0;
            }
            const Rect dock{r.x, r.bottom() - closed_h, r.w, closed_h};
            p.rect({dock.x, dock.y, dock.w, 1}, t.line());
            const bool qr = provider(thread().source).capabilities.qr_login;
            const auto cta = lyren::tr(qr ? "Scan to sign in" : "Connect account");
            const float bw = ui.button_width(cta);
            const Rect btn{dock.right() - C::inset - bw, dock.y + (dock.h - control::small) / 2, bw, control::small};
            const float msg_w = std::max(0.f, btn.x - dock.x - C::inset - C::gap);
            if (msg_w > 0) {
                const auto msg = lyren::trf("Sign in to {} to comment", lyren::tr(platform(thread().source).short_name));
                ui.label({dock.x + C::inset, dock.y, msg_w, dock.h}, msg, t.style(type::label, t.text_dim));
            }
            ui::Enabled reachable(ui, live_panel, 1);
            if (ui.button(ui.id("comment-sign-in"), btn, cta, ui::ButtonKind::tonal)) {
                shell_.account.open(ui);
                accounts_.begin(thread().source);
            }
            return closed_h;
        }

        if (draft->focus) {
            draft->expanded = true;
        }
        auto collapse = [&] {
            draft->expanded = draft->focus = false;
            if (ui.focus == field) {
                ui.focus = 0;
            }
            ui.redraw();
        };
        const bool has_text = draft->text.find_first_not_of(" \t\r\n") != std::string::npos;
        if (draft->expanded && live_panel && !draft->sending && !ui.menu_open() && !ui.modal_open()) {
            if (take_key(ui.input().keys, [](const wl::KeyEvent &k) {
                    return k.keysym == XKB_KEY_Escape;
                })) {
                collapse();
            } else if (has_text && ui.focus == field && take_key(ui.input().keys, [](const wl::KeyEvent &k) {
                           return (k.mods & wl::mod_ctrl) && (k.keysym == XKB_KEY_Return || k.keysym == XKB_KEY_KP_Enter);
                       })) {
                comments_.send();
            }
        }
        if (!draft->expanded && ui.focus == field) {
            ui.focus = 0;
        }

        const bool editing = draft->expanded;
        const float open = ui.spring(ui.id(view ? "reply-composer-open" : "comment-composer-open"), editing ? 1.f : 0.f, 240);
        const float reveal = std::clamp(open, 0.f, 1.f);
        if (draft->focus) {
            ui.focus = field;
            draft->focus = false;
        }

        const Comment *target = draft->reply ? &*draft->reply : view ? &view->root : nullptr;
        const LyricLine *sung = lyric();
        const float max_dock_h = std::max(closed_h, std::min(r.h, std::max(140.f, r.h * 0.52f)));
        const float line_h = std::round(t.fs(type::body) * 1.55f);
        const float well_pad = 10;
        const float inner_w = std::max(0.f, r.w - C::inset * 2);
        const float input_w = std::max(0.f, inner_w - well_pad * 2);
        const float reply_h = target ? 24.f : 0.f;
        const float footer_h = control::small;
        const float chrome_h = 12 + (reply_h > 0 ? reply_h + 6 : 0) + well_pad * 2 + 10 + footer_h + 12;

        float rows = 2;
        if (editing || reveal > 0.01f) {
            const float max_rows = std::max(1.f, std::floor((max_dock_h - chrome_h) / std::max(line_h, 1.f)));
            const float min_rows = std::min(2.f, max_rows);
            const size_t wrapped = input_w > 0 && !draft->text.empty() ? p.wrap_lines(draft->text, t.fs(type::body), input_w).size() : size_t{1};
            rows = ui.spring(ui.id("comment-composer-rows"), std::clamp(static_cast<float>(wrapped), min_rows, std::min(5.f, max_rows)), 200);
        }
        const float expanded_h = std::min(max_dock_h, chrome_h + rows * line_h);
        const float dock_h = std::round(std::lerp(closed_h, expanded_h, reveal));
        const Rect dock{r.x, r.bottom() - dock_h, r.w, dock_h};

        p.rect({dock.x, dock.y, dock.w, 1}, t.line());
        ui.push_clip({dock.x, dock.y + 1, dock.w, std::max(0.f, dock.h - 1)});

        // Resting single-line trigger bar, cross-fading cleanly into the expanded tray.
        if (const float rest_a = 1 - smooth01(reveal * 2.4f); rest_a > 0.01f) {
            Fade resting(p, rest_a);
            const Rect bar{dock.x + C::inset, dock.y + (closed_h - 36) / 2, inner_w, 36};
            ui::Enabled reachable(ui, live_panel && !editing, 1);
            const ui::Id trigger_id = ui.id("comment-trigger");
            const auto it = ui.interact(trigger_id, bar);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const auto [hot, down] = ui.feel(trigger_id, it, pace::quick);
            const Rect box = bar.inset(down * 0.5f);
            p.rect(box, t.fill(0.95f + 0.45f * hot), t.radius);
            p.icon("pencil", {box.x + 12, box.y + (box.h - C::small_icon) / 2, C::small_icon, C::small_icon}, has_text ? t.accent : t.text_dim.mix(t.text, 0.5f * hot));
            const std::string_view preview = has_text ? first_line(draft->text) : lyren::tr(target ? "Write a reply…" : "What does this music bring to mind?");
            ui.label({box.x + 12 + C::small_icon + C::gap, box.y, std::max(0.f, box.w - 24 - C::small_icon - C::gap), box.h}, preview,
                     t.style(type::label, has_text ? t.text : t.faint()));
            if (it.clicked) {
                draft->expanded = draft->focus = true;
            }
        }

        // Expanded writing dock.
        if (const float edit_a = smooth01((reveal - 0.15f) / 0.85f); edit_a > 0.01f) {
            Fade writing(p, edit_a);
            bool live = live_panel && editing && !draft->sending && reveal >= 0.85f;
            ui::Enabled editable(ui, live, 1);
            float y = dock.y + 12;

            if (target) {
                const Rect row{dock.x + C::inset, y, inner_w, reply_h};
                float cancel_w = 0;
                if (draft->reply) {
                    cancel_w = 24;
                    if (ui.icon_button(ui.id("comment-cancel-reply"), {row.right() - 22, row.y + (row.h - 22) / 2, 22, 22}, "x", 13, false, lyren::tr("Cancel"))) {
                        draft->reply.reset();
                        // Re-derive target immediately so the rest of this frame never dereferences the freed optional.
                        target = view ? &view->root : nullptr;
                    }
                }
                if (target) {
                    const std::string who = lyren::trf("Replying to {}", target->user);
                    const auto who_style = t.style(type::caption, t.text_dim, text::medium);
                    const float avail = std::max(0.f, row.w - cancel_w - C::small_gap);
                    const float ww = std::min(std::ceil(p.measure(who, who_style)) + 1, avail);
                    if (ww > 0) {
                        ui.label({row.x, row.y, ww, row.h}, who, who_style);
                    }
                    const std::string_view snippet = first_line(target->text);
                    const float sx = row.x + ww + C::gap;
                    if (!snippet.empty() && sx + 24 < row.right() - cancel_w) {
                        ui.label({sx, row.y, row.right() - cancel_w - sx, row.h}, snippet, t.style(type::caption, t.faint()));
                    }
                }
                y += reply_h + 6;
            }

            const float well_h = std::max(line_h + well_pad * 2, dock.bottom() - 12 - footer_h - 10 - y);
            const Rect well{dock.x + C::inset, y, inner_w, well_h};
            const float focused = ui.anim(ui.id("comment-composer-focus"), ui.focus == field && editing ? 1.f : 0.f, pace::quick);
            if (focused > 0.01f) {
                p.rect(well, t.fill(0.85f), t.radius, t.accent.with_alpha(0.55f * focused), 1);
            } else {
                p.rect(well, t.fill(0.85f), t.radius);
            }
            if (!draft->insert.empty() && editing) {
                ui.focus = field;
                ui.input().commit.insert(0, std::exchange(draft->insert, {}));
            }
            {
                Fade enabled(p, draft->sending ? 0.5f : 1.f);
                const Rect input = well.inset(well_pad);
                ui.text_area(field, input, draft->text, lyren::tr(target ? "Write a reply…" : "What does this music bring to mind?"), false);
            }

            const Rect foot{dock.x + C::inset, well.bottom() + 10, inner_w, footer_h};
            const auto post_label = lyren::tr("Post");
            const float post_w = std::max(64.f, ui.button_width(post_label));
            const Rect post_r{foot.right() - post_w, foot.y, post_w, footer_h};
            const Rect collapse_r{post_r.x - C::gap - C::btn, foot.y + (footer_h - C::btn) / 2, C::btn, C::btn};
            if (ui.icon_button(ui.id("comment-collapse"), collapse_r, "chevron-down", C::icon, false, lyren::tr("Close"))) {
                collapse();
                live = false;
            }

            {
                ui::Enabled can_post(ui, live && has_text);
                if (draft->sending) {
                    p.rect(post_r, t.accent.with_alpha(0.85f), t.radius_s());
                    ui.spinner(ui::center(post_r, 15, 15), t.on_accent(), 1.5f);
                } else if (ui.button(ui.id("comment-post"), post_r, post_label, ui::ButtonKind::primary)) {
                    comments_.send();
                }
            }

            const float left_w = std::max(0.f, collapse_r.x - foot.x - C::gap);
            if (left_w > 0) {
                ui::Enabled still_open(ui, live, 1);
                float count_w = 0;
                if (has_text) {
                    size_t n = 0;
                    for (size_t i = 0; i < draft->text.size(); i = utf8::next(draft->text, i)) {
                        ++n;
                    }
                    const std::string count = std::to_string(n);
                    const auto cs = t.style(type::micro, t.faint(), text::tabular_nums);
                    count_w = std::ceil(p.measure(count, cs)) + 8;
                    ui.label({foot.x + left_w - count_w, foot.y, count_w, footer_h}, count, cs, Align::right);
                }
                const float tool_w = std::max(0.f, left_w - count_w);
                if (sung && tool_w > C::small_icon + 16) {
                    const auto quote_label = lyren::tr("Quote lyric");
                    const float qw = std::min(tool_w, std::ceil(p.measure(quote_label, t.style(type::caption, {}))) + C::small_icon + C::gap * 2 + 4);
                    const Rect qbtn{foot.x - 4, foot.y + (footer_h - 26) / 2, qw, 26};
                    const auto qid = ui.id("comment-quote-lyric");
                    const auto it = ui.interact(qid, qbtn);
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    const float h = ui.anim(qid, it.hovered ? 1.f : 0.f, pace::quick);
                    if (h > 0.01f) {
                        p.rect(qbtn, t.fill(1.1f * h), t.radius_s());
                    }
                    const ui::Color ink = t.text_dim.mix(t.text, 0.7f * h);
                    p.icon("music", {qbtn.x + 6, qbtn.y + (qbtn.h - C::small_icon) / 2, C::small_icon, C::small_icon}, ink);
                    ui.label({qbtn.x + 6 + C::small_icon + C::small_gap + 2, qbtn.y, std::max(0.f, qbtn.w - 12 - C::small_icon - C::small_gap), qbtn.h}, quote_label,
                             t.style(type::caption, ink));
                    if (it.clicked) {
                        draft->insert = "\"" + sung->text + "\"";
                    }
                } else if (tool_w > 0) {
                    ui.label({foot.x, foot.y, tool_w, footer_h}, lyren::tr("Enter for a new line"), t.style(type::micro, t.faint()));
                }
            }
        }
        ui.pop_clip();
        return dock_h;
    }

    void CommentsView::draw(ui::Ui &ui, Rect r) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto reply = thread().replies;

        // Scope all widgets and scroll containers to the current thread visit so state never leaks across subjects.
        ui.push_id(ui.id(thread().serial));
        const ui::Id list_id = reply ? ui::sub_id(ui.id("comment-replies"), ui.id(reply->root.id)) : ui.id(thread().tab ? "comments-new" : "comments-hot");
        if (std::exchange(thread().to_top, false)) {
            ui.scroll_to(list_id, 0);
        }

        const bool compact = r.h < 420;
        const float header_h = reply ? replies_header(ui, r, reply, compact) : header(ui, r, compact);
        if (header_h < 0) {
            ui.pop_id();
            return;
        }

        Feed &f = reply ? *reply->feed : *thread().feeds[thread().tab];
        if (f.items.empty() && f.more && !f.loading && f.error.empty()) {
            if (reply) {
                comments_.load_replies(reply);
            } else {
                comments_.load(thread().tab);
            }
        }

        const Rect body{r.x, r.y + header_h, r.w, std::max(0.f, r.h - header_h)};
        const float composer_h = composer(ui, body);
        const Rect view{body.x, body.y, body.w, std::max(0.f, body.h - composer_h)};

        const float enter = ui.progress(reply ? reply->opened : thread().tab_since, pace::glide);
        const float x = r.x + C::inset + (1 - enter) * 10 * (reply || thread().tab ? 1 : -1);
        const float w = std::max(0.f, r.w - C::inset * 2);
        {
            Fade fade(p, enter * ui.progress(f.since, pace::steady));
            if (f.items.empty()) {
                ui.push_clip(view);
                if (f.loading || !f.error.empty()) {
                    page_status(ui, view, f.loading, f.error, false);
                } else if (view.h < 150) {
                    ui.label(view, lyren::tr(reply ? "No replies yet" : "No comments yet"), t.style(type::label, t.text_dim), Align::center);
                } else {
                    empty(ui, view, "message-square", lyren::tr(reply ? "No replies yet" : "No comments yet"), lyren::tr("Join the conversation"));
                }
                ui.pop_clip();
            } else {
                const float size = t.fs(type::body);
                if (f.heights.width != w || f.heights.size != size) {
                    f.heights.reset(w, size);
                }
                while (f.heights.size_count() < f.items.size()) {
                    const size_t i = f.heights.size_count();
                    f.heights.push(comment(ui, f.items[i], 0, 0, w, false));
                }
                const float tail = f.loading || !f.more || !f.error.empty() ? 56.f : C::padding;
                const float content = f.heights.total() + tail;
                const float off = ui.begin_scroll(list_id, view, content);
                const Rect clip = ui.clip();
                const float origin = view.y - off;

                ui.push_id(list_id);
                for (size_t i = f.heights.first_at(clip.y - origin); i < f.items.size(); ++i) {
                    const float row_y = origin + f.heights.top(i);
                    if (row_y >= clip.bottom()) {
                        break;
                    }
                    comment(ui, f.items[i], x, row_y, w, true);
                }
                ui.pop_id();

                const float end_y = origin + f.heights.total();
                if (f.loading) {
                    ui.spinner(ui::center({view.x, end_y, view.w, 56}, 20, 20), t.text_dim, 2);
                } else if (!f.error.empty()) {
                    ui.label({view.x + C::inset, end_y + C::small_gap, std::max(0.f, view.w - C::inset * 2), C::btn}, f.error, t.style(type::caption, t.text_dim));
                    const float rw = ui.button_width(lyren::tr("Retry"), true);
                    if (ui.button(ui.id("comments-retry"), {view.x + (view.w - rw) / 2, end_y + 30, rw, control::small}, lyren::tr("Retry"), ui::ButtonKind::secondary,
                                  "refresh-cw")) {
                        f.error.clear();
                        if (reply) {
                            comments_.load_replies(reply);
                        } else {
                            comments_.load(thread().tab);
                        }
                    }
                } else if (f.more && end_y < view.bottom() + 400) {
                    if (reply) {
                        comments_.load_replies(reply);
                    } else {
                        comments_.load(thread().tab);
                    }
                } else if (!f.more) {
                    ui.label({view.x, end_y + 12, view.w, 32}, lyren::tr("No more comments"), t.style(type::caption, t.faint()), Align::center);
                }
                if (t.surface.a > 0.9f && off > 0.5f) {
                    p.gradient({view.x + 1, view.y, std::max(0.f, view.w - 2), 14}, t.surface, t.surface.with_alpha(0));
                }
                ui.end_scroll();
            }
        }
        ui.pop_id();
    }

    void CommentsView::picture(ui::Ui &ui, const Comment &c, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &e = env_.images.picture(c.image, static_cast<uint32_t>(Images::tier(std::max(r.w, r.h) * p.scale())));
        const float a = e.ready ? ui.progress(e.ready_at, 320, ease::standard) : 0;
        const float aspect = e.ready && e.h ? static_cast<float>(e.w) / static_cast<float>(e.h) : 1;
        if (c.sticker) {
            if (e.ready) {
                const Rect fit = aspect > 1 ? Rect{r.x, r.y + (r.h - r.w / aspect) / 2, r.w, r.w / aspect} : Rect{r.x, r.y, r.h * aspect, r.h};
                Fade fade(p, a);
                p.image(fit, e.tex, 0);
            }
            return;
        }
        const auto it = ui.interact(ui.id("comment-picture"), r);
        const float hot = ui.anim(ui.id("comment-picture"), it.hovered ? 1.f : 0.f, pace::quick);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (a < 1) {
            p.rect(r, t.fill(), t.radius);
            p.icon("image", ui::center(r, 22, 22), t.text_dim.with_alpha(0.6f * (1 - a)));
        }
        if (e.ready) {
            Fade fade(p, a);
            p.image(r, e.tex, t.radius, white.mix({0.86f, 0.86f, 0.86f, 1}, hot * 0.35f), cover_uv(aspect, r.w / r.h));
        }
        const std::string_view url = c.image;
        if (url.substr(0, url.find('?')).ends_with(".gif")) {
            const Rect badge{r.x + 8, r.bottom() - 8 - 18, 32, 18};
            p.rect(badge, {0, 0, 0, 0.55f}, 9);
            ui.label(badge, "GIF", t.style(type::micro, white), Align::center);
        }
        if (it.clicked) {
            shell_.dialogs.show_picture(ui, c.image, c.image_w > 0 && c.image_h > 0 ? static_cast<float>(c.image_w) / static_cast<float>(c.image_h) : e.ready ? aspect : 4.f / 3);
        }
    }

    float CommentsView::comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Source source = thread().source;
        const TextStyle body = t.style(type::body, t.text);
        const TextStyle quote = t.style(type::label, t.text_dim);
        const float lh = std::round(body.size * 1.58f), qh = std::round(quote.size * 1.5f);
        const float ident_h = std::ceil(t.fs(type::label) * 1.45f);
        const float action_h = std::max(28.f, std::ceil(t.fs(type::caption) * 1.55f));
        const float tx = x + C::avatar + 12;
        const float tw = std::max(0.f, x + w - tx);

        std::string replied;
        if (!c.reply.empty()) {
            replied.reserve(c.reply_user.size() + c.reply.size() + 4);
            replied.push_back('@');
            replied.append(c.reply_user);
            replied.append("：");
            replied.append(c.reply);
        }
        const auto lines = tw > 0 && !c.text.empty() ? p.wrap_lines(c.text, body.size, tw) : std::vector<std::string_view>{};
        const float q_inner = std::max(0.f, tw - 20);
        const auto quoted = q_inner > 0 && !replied.empty() ? p.wrap_lines(replied, quote.size, q_inner) : std::vector<std::string_view>{};
        const float quote_h = quoted.empty() ? 0 : static_cast<float>(quoted.size()) * qh + 14;
        Rect pic = picture_box(c, tw);

        // One calm, uniform row structure across every comment: avatar left, compact meta line, body, optional quote/media, quiet actions.
        const float top = y + 14;
        const float body_y = top + ident_h + 4;
        float cy = body_y + static_cast<float>(lines.size()) * lh;
        const float quote_y = cy + C::gap;
        cy += quote_h > 0 ? C::gap + quote_h : 0;
        pic.x = tx, pic.y = cy + C::gap;
        cy += pic.h > 0 ? C::gap + pic.h : 0;
        const float action_y = cy + 4;
        const float h = action_y + action_h + 10 - y;
        if (!draw) {
            return h;
        }

        const Likes::Mark mine = comments_.like(c);
        const bool liked = mine.liked;
        const auto &caps = provider(source).capabilities;
        const bool owned = c.owned && caps.comment_delete && provider(source).logged_in();
        const int count = mine.likes;
        const std::string n = count > 0 ? short_number(count) : "";

        ui.push_id(ui.id(key(source, c.id)));
        cover(ui, {x, top + 1, C::avatar, C::avatar}, c.avatar, C::avatar / 2, "user");

        // Like sits quietly at the top-right of the author row.
        const float pop = ui.spring(ui.id("comment-like-pop"), liked ? 1.f : 0.f, 300, 0.25f);
        const float like_w = comment_action_width(ui, n);
        const Rect like{x + w - like_w, top, like_w, ident_h};
        {
            ui::Enabled live(ui, caps.comment_likes && !mine.pending);
            if (comment_action(ui, ui.id("comment-like"), like, n, liked ? "thumbs-up-fill" : "thumbs-up", liked ? t.accent : t.text_dim,
                               1 + 0.25f * std::abs(pop - (liked ? 1.f : 0.f)))) {
                comments_.toggle_like(c);
            }
        }

        // Compact single-line identity: author and timestamp.
        const float meta_right = std::max(tx, like.x - C::gap);
        const auto name_style = t.style(type::label, t.text.mix(t.text_dim, 0.14f), text::medium);
        const float name_w = std::min(std::ceil(p.measure(c.user, name_style)) + 1, std::max(0.f, meta_right - tx));
        if (name_w > 0) {
            ui.label({tx, top, name_w, ident_h}, c.user, name_style);
        }
        const float mx = tx + name_w + C::gap;
        if (!c.time.empty() && mx < meta_right) {
            const auto time_style = t.style(type::micro, t.faint());
            const float tw_time = std::min(std::ceil(p.measure(c.time, time_style)) + 1, meta_right - mx);
            ui.label({mx, top + 1, tw_time, ident_h}, c.time, time_style);
        }

        if (!c.user_id.empty() && caps.profiles) {
            const Rect face{x, top + 1, C::avatar, C::avatar};
            const Rect who{x, top, tx - x + name_w, ident_h};
            auto it = ui.interact(ui.id("comment-author"), who);
            auto pic_it = ui.interact(ui.id("comment-face"), face);
            it.clicked |= pic_it.clicked, it.hovered |= pic_it.hovered;
            const float hot = ui.anim(ui.id("comment-author"), it.hovered ? 1.f : 0.f, pace::quick);
            if (hot > 0.01f) {
                p.rect({tx, top + ident_h - 2, name_w, 1}, t.text.with_alpha(0.4f * hot));
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.clicked) {
                browser_.open_profile(source, c.user_id, c.user, c.avatar);
            }
        }

        float ly = body_y;
        for (auto line : lines) {
            ui.label({tx, ly, tw, lh}, line, body);
            ly += lh;
        }
        if (!quoted.empty()) {
            const Rect q{tx, quote_y, tw, quote_h};
            p.rect(q, t.fill(0.72f), t.radius_s());
            float qy = q.y + 7;
            for (auto line : quoted) {
                ui.label({q.x + 10, qy, q_inner, qh}, line, quote);
                qy += qh;
            }
        }
        if (pic.h > 0) {
            picture(ui, c, pic);
        }

        // Quiet action row: Reply, understated inline replies link, and Delete for own comments.
        const std::string_view reply_label = lyren::tr("Reply");
        const Rect reply_button{tx, action_y, comment_action_width(ui, reply_label), action_h};
        if (caps.comment_post) {
            auto draft = thread().replies ? thread().replies->draft : thread().draft;
            ui::Enabled editable(ui, !draft->sending);
            if (comment_action(ui, ui.id("comment-reply"), reply_button, reply_label, "message-circle", t.faint())) {
                draft->reply = c;
                draft->focus = true;
            }
        }
        float end = x + w;
        if (owned) {
            const Rect bin{end - C::btn + 6, action_y + (action_h - C::btn) / 2, C::btn, C::btn};
            {
                ui::Enabled live(ui, !comments_.removing(c));
                if (ui.icon_button(ui.id("comment-delete"), bin, "trash", C::small_icon, false, lyren::tr("Delete"))) {
                    confirm_delete(ui, source, thread().subject, thread().id, c);
                }
            }
            end = bin.x - C::small_gap;
        }
        if (caps.comment_replies && c.replies > 0) {
            const std::string label = lyren::trf("{} replies", c.replies);
            const float start_x = caps.comment_post ? reply_button.right() + 10 : tx - C::gap;
            const float cw = std::min(replies_link_width(ui, label), end - start_x);
            if (cw > C::small_icon * 2 + C::gap && replies_link(ui, ui.id("comment-replies"), {start_x, action_y + (action_h - 24) / 2, cw, 24}, label)) {
                comments_.open_replies(c);
            }
        }
        p.rect({tx, y + h - 1, tw, 1}, t.line(0.55f));
        ui.pop_id();
        return h;
    }

    void CommentsView::confirm_delete(ui::Ui &ui, Source source, Subject subject, std::string resource, Comment comment) {
        std::string detail = comment.text;
        ui.confirm(std::string(lyren::tr("Delete comment?")), std::move(detail), std::string(lyren::tr("Delete")), "trash",
                   [this, source, subject, resource = std::move(resource), comment = std::move(comment)] {
                       comments_.remove(source, subject, resource, comment);
                   });
    }

} // namespace lyren
