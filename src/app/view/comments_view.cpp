#include "app/view/comments_view.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

// The comments on a song, an album, a playlist or a post, in the panel beside the page and the stage.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;
    namespace type = ui::type;
    using Feed = Comments::Feed;

    namespace {
        // The comments read like a record's sleeve notes: the record up top, the voices in one column, one quote set
        // large, and a round button floating over them that opens into the card to write in.
        struct CommentStyle {
            static constexpr float inset = 20, padding = 16, gap = 8, small_gap = 4;
            static constexpr float avatar = 34, art = 56, icon = 16, small_icon = 14, control = 30, send = 36, fab = 52;
            static constexpr float title = 15, quote = 15, body = 14, name = 13, meta = 12, time = 11;
            static constexpr float picture_w = 240, picture_h = 220, sticker = 76;
        };

        float comment_action_width(ui::Ui &ui, std::string_view label) {
            using C = CommentStyle;
            float text = label.empty() ? 0 : std::ceil(ui.painter().measure(label, {.size = ui.theme().fs(C::meta), .features = text::tabular_nums})) + C::small_gap + 2;
            return C::small_icon + text;
        }

        // An icon and a quiet label. icon_scale swells the icon about its centre, for a like landing.
        bool comment_action(ui::Ui &ui, ui::Id id, Rect r, std::string_view label, std::string_view icon, ui::Color color, float icon_scale = 1) {
            using C = CommentStyle;
            auto &p = ui.painter();
            const auto &t = ui.theme();
            auto it = ui.interact(id, r.inset(-C::small_gap, 0));
            float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
            color = color.mix(t.text, hot * 0.6f);
            float s = C::small_icon * icon_scale;
            p.icon(icon, {r.x + (C::small_icon - s) / 2, r.y + (r.h - s) / 2, s, s}, color);
            ui.label({r.x + C::small_icon + C::small_gap + 2, r.y, r.w, r.h}, label, {.size = t.fs(C::meta), .color = color, .features = text::tabular_nums});
            return it.clicked;
        }

        float thread_chip_width(ui::Ui &ui, std::string_view label) {
            using C = CommentStyle;
            return std::ceil(ui.painter().measure(label, {.size = ui.theme().fs(C::meta)})) + C::small_icon * 2 + C::gap * 3;
        }

        // The way into a comment's replies: a tinted tab hanging off the comment like the start of a thread.
        bool thread_chip(ui::Ui &ui, ui::Id id, Rect r, std::string_view label) {
            using C = CommentStyle;
            auto &p = ui.painter();
            const auto &t = ui.theme();
            auto it = ui.interact(id, r);
            const auto [hot, down] = ui.feel(id, it, 140);
            Rect b = r.inset(down, down);
            p.rect(b, t.accent.with_alpha(0.1f + 0.07f * hot), b.h / 2);
            p.icon("corner-down-right", {b.x + C::gap, b.y + (b.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent);
            ui.label({b.x + C::gap * 1.5f + C::small_icon, b.y, b.w, b.h}, label, {.size = t.fs(C::meta), .color = t.accent});
            float nudge = 2 * hot;
            p.icon("chevron-right", {b.right() - C::gap - C::small_icon + nudge, b.y + (b.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent.with_alpha(0.75f));
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            return it.clicked;
        }

        // Where a comment's picture goes in a column w wide: its own shape within bounds, or 4:3 until it is known.
        ui::Rect picture_box(const Comment &c, float w) {
            using C = CommentStyle;
            if (c.image.empty()) {
                return {};
            }
            if (c.sticker) {
                return {0, 0, C::sticker, C::sticker};
            }
            float aspect = c.image_w > 0 && c.image_h > 0 ? static_cast<float>(c.image_w) / static_cast<float>(c.image_h) : 4.f / 3;
            aspect = std::clamp(aspect, 0.56f, 2.2f);
            float mw = std::min(w, C::picture_w), mh = C::picture_h;
            return aspect >= mw / mh ? Rect{0, 0, mw, std::round(mw / aspect)} : Rect{0, 0, std::round(mh * aspect), mh};
        }

        // A record in its own colours, grooves catching the light at angle (radians).
        void vinyl(ui::Painter &p, Rect disc, ui::Color label, float angle) {
            float r = disc.w / 2;
            p.shadow({disc.x, disc.y + 2, disc.w, disc.h}, r, 8, {0, 0, 0, 0.35f});
            p.rect(disc, {0.07f, 0.07f, 0.08f, 1}, r);
            for (float k : {0.9f, 0.78f, 0.66f, 0.54f}) {
                p.rect(disc.inset(r * (1 - k)), {0, 0, 0, 0}, r * k, white.with_alpha(0.06f), 1);
            }
            for (float side : {0.f, std::numbers::pi_v<float>}) {
                p.arc(disc.inset(r * 0.2f), r * 0.42f, angle + side, 0.55f, white.with_alpha(0.08f));
            }
            Rect centre = disc.inset(r * 0.64f);
            p.rect(centre, label, centre.w / 2);
            p.rect(ui::center(disc, 3, 3), {0.07f, 0.07f, 0.08f, 1}, 1.5f);
        }
    } // namespace

    bool CommentsView::playing() const {
        const Track *tr = playback_.current();
        return tr && thread().is(tr->source, Subject::song, tr->id) && playback_.playing();
    }

    const LyricLine *CommentsView::lyric() {
        const Track *tr = playback_.current();
        if (!tr || !thread().is(tr->source, Subject::song, tr->id)) {
            return nullptr;
        }
        return playback_.line_at(*tr, playback_.song_time() + 0.2);
    }

    // A round button resting over the comments; it opens into the card it writes in, the button staying in its corner
    // as the card's send button. Laid out before the list so it takes input first, raised over it once the list is drawn.
    float CommentsView::composer(ui::Ui &ui, Rect r) {
        if (!provider(thread().source).capabilities.comment_post) {
            composer_layer_ = {};
            return 0;
        }
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto view = thread().replies;
        auto draft = view ? view->draft : thread().draft;
        bool signed_in = provider(thread().source).logged_in();
        const Comment *target = draft->reply ? &*draft->reply : view ? &view->root : nullptr;
        const LyricLine *sung = signed_in ? lyric() : nullptr;
        ui::Id field = ui.id(view ? "reply-draft" : "comment-draft");
        if (!signed_in) {
            draft->expanded = draft->focus = false;
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
        if (draft->expanded && !draft->sending && !ui.menu_open() && !ui.modal_open()) {
            auto &keys = ui.input().keys;
            auto escape = std::ranges::find_if(keys, [](const auto &key) {
                return key.keysym == XKB_KEY_Escape;
            });
            if (escape != keys.end()) {
                keys.erase(escape);
                collapse();
            }
        }
        if (!draft->expanded && ui.focus == field) {
            ui.focus = 0;
        }
        const bool editing = draft->expanded;
        float open = ui.spring(ui.id(view ? "reply-composer-open" : "comment-composer-open"), editing ? 1 : 0, 420, 0.18f);
        float reveal = smooth01(open);
        if (draft->focus) {
            ui.focus = field;
            draft->focus = false;
        }
        bool has_text = draft->text.find_first_not_of(" \t\r\n") != std::string::npos;

        // The card's measure: a line of context, the excerpt being answered, the editor growing with the text, the tools.
        const float line_h = std::round(t.fs(type::label) * 1.55f), pad = 14;
        Rect card{r.x + 12, 0, r.w - 24, 0};
        const float input_w = card.w - pad * 2;
        bool compact = r.h < 330;
        float quote_h = target && !compact ? line_h + C::gap : 0;
        float lines = static_cast<float>(p.wrap_lines(draft->text.empty() ? std::string_view(" ") : std::string_view(draft->text), t.fs(type::label), input_w).size());
        float rows = ui.spring(ui.id("comment-composer-rows"), std::clamp(lines, compact ? 1.f : 2.f, compact ? 2.f : 6.f), 220);
        card.h = pad + C::control + C::small_gap + quote_h + rows * line_h + C::gap + C::send + 12;
        card.y = r.bottom() - 12 - card.h;
        Rect fab{r.right() - 16 - C::fab, r.bottom() - 16 - C::fab, C::fab, C::fab};
        Rect sheet = mix(fab, card, open);
        sheet.h = std::max(sheet.h, C::fab), sheet.w = std::max(sheet.w, C::fab);
        const float radius = std::lerp(C::fab / 2, t.radius_l(), std::clamp(open, 0.f, 1.f));

        // The send button is the round button itself: it glides to the card's corner and stretches to fit its label.
        auto label = lyren::tr("Post");
        const float label_w = std::ceil(p.measure(label, {.size = t.fs(C::meta)}));
        const float wide = ui.anim(ui.id("comment-send-wide"), editing && has_text ? 1 : 0, 220);
        const float send_w = C::send + (label_w + C::gap) * wide;
        Rect docked{card.right() - 12 - send_w, card.bottom() - 12 - C::send, send_w, C::send};
        Rect send = mix(fab, docked, open);
        ui.disabled = shell_.panel.kind() != SidePanel::Kind::comments || draft->sending || (editing && (!has_text || open < 0.9f));
        auto post = ui.interact(ui.id(editing ? "comment-send" : "comment-fab"), send);
        ui.disabled = false;
        float hot = ui.anim(ui.id("comment-send-hover"), post.hovered ? 1 : 0, 160);
        float down = ui.anim(ui.id("comment-send-press"), post.held && post.hovered ? 1 : 0, 90);
        if (post.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }

        const size_t layer = p.mark();
        const bool listening = !editing && playback_.playing() && t.motion > 0;
        if (reveal < 0.99f) {
            Fade resting(p, 1 - reveal);
            // While the song plays beside its comments the button breathes with it.
            if (listening) {
                float k = static_cast<float>(std::fmod(ui.now, 2.4) / 2.4);
                Rect ring = fab.inset(-10 * smooth01(k));
                p.rect(ring, {0, 0, 0, 0}, ring.w / 2, t.accent.with_alpha(0.45f * (1 - k)), 1.5f);
                ui.wake(ui.now + 1.0 / 30);
            }
            // What the button does, on hover: a tab sliding out of it.
            float tip = ui.anim(ui.id("comment-fab-tip"), post.hovered && !editing ? 1 : 0, 200);
            if (tip > 0) {
                auto words = !signed_in ? lyren::trf("Sign in to {} to comment", lyren::tr(platform(thread().source).short_name))
                                        : std::string(lyren::tr(has_text ? "Continue writing" : "Write a comment"));
                float ww = std::ceil(p.measure(words, {.size = t.fs(C::meta)})) + C::padding * 2;
                Rect pill{fab.x - C::gap - ww + (1 - tip) * C::gap * 2, fab.y + (fab.h - C::control) / 2, ww, C::control};
                Fade shown(p, tip);
                auto paper = t.elevated;
                paper.a = std::max(paper.a, 0.94f);
                p.shadow({pill.x, pill.y + 2, pill.w, pill.h}, pill.h / 2, 10, t.shadow.with_alpha(0.35f));
                p.rect(pill, paper, pill.h / 2, t.border, 1);
                ui.label(pill, words, {.size = t.fs(C::meta), .color = t.text}, Align::center);
            }
        }
        if (open > 0.02f) {
            auto paper = t.elevated;
            paper.a = 1;
            float focus = ui.anim(ui.id("comment-composer-focus"), ui.focus == field && editing ? 1 : 0, 160);
            p.shadow({sheet.x, sheet.y + 8, sheet.w, sheet.h}, radius, 28, t.shadow.with_alpha(0.55f * std::min(1.f, open * 2)));
            p.rect(sheet, paper, radius, t.border.mix(t.accent, focus * 0.6f), 1);
        }
        if (reveal > 0.02f) {
            ui.push_clip(sheet);
            Fade writing(p, smooth01((reveal - 0.3f) / 0.7f));
            ui.disabled = shell_.panel.kind() != SidePanel::Kind::comments || !editing || draft->sending;
            Rect head{card.x + pad, card.y + pad, input_w, C::control};
            if (ui.icon_button(ui.id("comment-collapse"), {head.right() - C::control + 6, head.y, C::control, C::control}, "chevron-down", C::icon)) {
                collapse();
                ui.disabled = true;
            }
            if (target) {
                // Who this answers, as a tag; a reply chosen by hand can be dropped back to the thread.
                auto who = lyren::trf("Replying to {}", target->user);
                float ww = std::min(head.w - C::control - C::gap, std::ceil(p.measure(who, {.size = t.fs(C::meta)})) + C::padding * 2 + (draft->reply ? C::icon : 0));
                Rect tag{head.x, head.y + 2, ww, C::control - 4};
                p.rect(tag, t.accent.with_alpha(0.12f), tag.h / 2);
                ui.label({tag.x + C::padding - 4, tag.y, tag.w - C::padding - (draft->reply ? C::icon + 4 : 0), tag.h}, who, {.size = t.fs(C::meta), .color = t.accent});
                if (draft->reply && ui.icon_button(ui.id("comment-cancel-reply"), {tag.right() - C::icon - 8, tag.y + (tag.h - 20) / 2, 20, 20}, "x", 12)) {
                    draft->reply.reset();
                }
            } else {
                p.icon("pencil", {head.x, head.y + (head.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent);
                ui.label({head.x + C::small_icon + C::gap, head.y, head.w - C::control * 2, head.h}, lyren::tr("Write a comment"), {.size = t.fs(C::meta), .color = t.text_dim});
            }
            float y = head.bottom() + C::small_gap;
            if (quote_h > 0) {
                p.rect({head.x, y + 3, 2, line_h - 6}, t.accent.with_alpha(0.5f), 1);
                auto first = std::string_view(target->text).substr(0, target->text.find('\n'));
                ui.label({head.x + C::gap + 2, y, head.w - C::gap - 2, line_h}, first, {.size = t.fs(C::meta), .color = t.text_dim});
                y += quote_h;
            }
            Rect input{head.x, y, input_w, rows * line_h};
            if (!draft->insert.empty() && editing) {
                ui.focus = field;
                ui.input().commit.insert(0, std::exchange(draft->insert, {}));
            }
            {
                Fade enabled(p, draft->sending ? 0.5f : 1.f);
                ui.text_area(field, input, draft->text, lyren::tr(target ? "Write a reply…" : "What does this music bring to mind?"), false);
            }
            // The tools along the bottom: the line playing now, to quote, or the reminder that Enter is a new line.
            Rect tools{head.x, card.bottom() - 12 - C::send, docked.x - head.x - C::gap, C::send};
            if (sung) {
                auto quote_label = lyren::tr("Quote lyric");
                float qw = std::min(tools.w, std::ceil(p.measure(quote_label, {.size = t.fs(C::meta)})) + C::small_icon + C::padding * 2);
                Rect chip{tools.x - 4, tools.y + (tools.h - 28) / 2, qw, 28};
                auto it = ui.interact(ui.id("comment-quote-lyric"), chip);
                float h = ui.anim(ui.id("comment-quote-lyric"), it.hovered ? 1 : 0, 140);
                p.rect(chip, t.text.with_alpha(0.04f + 0.05f * h), chip.h / 2, t.border, 1);
                p.icon("music", {chip.x + C::padding - 4, chip.y + (chip.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent);
                ui.label({chip.x + C::padding + C::small_icon, chip.y, chip.w, chip.h}, quote_label, {.size = t.fs(C::meta), .color = t.text_dim.mix(t.text, h)});
                if (it.clicked) {
                    draft->insert = "「" + sung->text + "」";
                }
            } else {
                ui.label(tools, lyren::tr("Enter for a new line"), {.size = t.fs(C::time), .color = t.text_dim});
            }
            if (has_text) {
                size_t n = 0;
                for (size_t i = 0; i < draft->text.size(); i = utf8::next(draft->text, i)) {
                    ++n;
                }
                ui.label({tools.x, tools.y, tools.w, tools.h}, std::to_string(n), {.size = t.fs(C::time), .color = t.text_dim, .features = text::tabular_nums}, Align::right);
            }
            ui.disabled = false;
            ui.pop_clip();
        }

        // The button, drawn last: solid while it has something to do, softened while an open card is still empty.
        bool ready = !editing || has_text;
        ui::Color ink = t.accent.luminance() > 0.6f ? ui::Color{0.07f, 0.07f, 0.08f, 1} : white;
        Rect b = send.inset(down * 2 - hot * (1 - open) * 2);
        float b_radius = b.h / 2;
        if (open < 0.5f) {
            p.shadow({b.x, b.y + 4, b.w, b.h}, b_radius, 14 + 6 * hot, t.shadow.with_alpha(0.5f + 0.3f * hot));
        }
        p.rect(b, ready ? t.accent.mix(t.text, hot * 0.1f) : t.accent.with_alpha(0.28f), b_radius);
        ui::Color fg = ready ? ink : ink.with_alpha(0.75f);
        if (draft->sending) {
            ui.spinner(ui::center(b, C::icon, C::icon), fg, 1.5f);
        } else {
            float is = std::lerp(20.f, C::icon, reveal);
            float icon_x = b.right() - std::lerp(b.w / 2, C::send / 2, wide) - is / 2;
            Rect glyph{icon_x, b.y + (b.h - is) / 2, is, is};
            {
                Fade pen(p, 1 - reveal);
                p.icon(signed_in ? "pencil" : "qr-code", glyph, fg);
            }
            {
                Fade plane(p, reveal);
                p.icon("send", glyph, fg);
            }
            if (wide > 0) {
                Fade words(p, wide * reveal);
                ui.label({b.x + C::padding, b.y, label_w + 2, b.h}, label, {.size = t.fs(C::meta), .color = fg});
            }
            // A draft waits behind the closed button.
            if (has_text && reveal < 0.5f) {
                p.rect({b.right() - 13, b.y + 3, 10, 10}, t.surface, 5);
                p.rect({b.right() - 11, b.y + 5, 6, 6}, t.text, 3);
            }
        }
        const size_t layer_end = p.mark();

        if (post.clicked) {
            if (!signed_in) {
                shell_.account.open(ui);
                accounts_.begin(thread().source);
            } else if (!editing) {
                draft->expanded = draft->focus = true;
            } else {
                comments_.send();
            }
        }
        if (open > 0.02f) {
            // The card's blank space keeps its clicks and scrolling from the comments under it.
            ui.interact(ui.id("comment-composer-surface"), sheet);
            if (ui.pointing(sheet)) {
                ui.input().scroll_y = 0;
                ui.input().scroll_stop = false;
            }
        }
        composer_layer_ = {layer, layer_end};
        return r.bottom() - std::min(sheet.y, fab.y) + C::gap;
    }

    float CommentsView::sleeve(ui::Ui &ui, Rect r, bool compact) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float art = compact ? 44 : C::art;
        const bool record = thread().subject == Subject::song || thread().subject == Subject::album;
        Rect sleeve{r.x + C::inset, r.y + C::padding, art, art};
        const float slide = record ? art * 0.46f * ui.progress(thread().opened, 900, ui::ease::emphasized) : 0;
        const float tab_h = 36;
        const Rect bar{r.x + C::inset, sleeve.bottom() + (compact ? 6 : 12), r.w - C::inset * 2, tab_h};
        const float header_h = bar.bottom() + 1 - r.y;

        // The cover, blurred, lights the top of the panel and fades into it under the tabs.
        if (t.surface.a > 0.9f && !thread().cover.empty()) {
            const auto &e = env_.images.get(thread().cover, 128, true);
            if (e.ready) {
                Rect wash{r.x, r.y, r.w, header_h + 28};
                float a = ui.progress(e.ready_at, 500);
                {
                    Fade lit(p, a * (t.dark ? 0.6f : 0.4f));
                    p.image(wash, e.tex, t.radius_l(), white, cover_uv(1, wash.w / wash.h));
                }
                p.rect(wash, t.surface.with_alpha(0.25f * a), t.radius_l());
                p.gradient({wash.x, wash.y + wash.h * 0.3f, wash.w, wash.h * 0.7f + 1}, t.surface.with_alpha(0), t.surface);
            }
        }
        if (record) {
            // The record slides out of its sleeve when the thread opens, and turns while its song plays.
            Rect disc = sleeve.inset(art * 0.04f);
            disc.x += slide;
            float hue = cover_hue(thread().cover, art);
            ui::Color label = hue < 0 ? t.accent : ui::accent_for_hue(hue, true);
            float angle = 0.6f;
            if (playback_.playing() && t.motion > 0) {
                angle = static_cast<float>(std::fmod(ui.now, 3.6) / 3.6) * 2 * std::numbers::pi_v<float>;
                ui.wake(ui.now + 1.0 / 30);
            }
            vinyl(p, disc, label, angle);
        }
        p.shadow({sleeve.x, sleeve.y + 3, art, art}, t.radius_s(), 12, t.shadow.with_alpha(0.6f));
        static constexpr std::array placeholders = {"music", "disc-album", "list-music", "message-square"};
        cover(ui, sleeve, thread().cover, t.radius_s(), placeholders[static_cast<size_t>(thread().subject)]);

        Rect close{r.right() - C::inset - C::control + 6, r.y + C::padding - 4, C::control, C::control};
        const float tx = sleeve.right() + slide + 14, tw = close.x - tx - C::gap;
        const float title_h = std::ceil(t.fs(C::title) * 1.45f), sub_h = std::ceil(t.fs(C::meta) * 1.5f);
        const float ty = sleeve.y + (art - title_h - sub_h - 2) / 2;
        ui.label({tx, ty, tw, title_h}, thread().title, {.size = t.fs(C::title), .color = t.text});
        std::string sub(lyren::tr(platform(thread().source).name));
        // Either list knows the count once its first page is in.
        const Feed &shown = thread().feeds[thread().tab]->page ? *thread().feeds[thread().tab] : *thread().feeds[1 - thread().tab];
        if (shown.page || shown.total > 0) {
            sub += " · " + lyren::trf("{} comments", grouped(shown.total));
        }
        ui.label({tx, ty + title_h + 2, tw, sub_h}, sub, {.size = t.fs(C::meta), .color = t.text_dim, .features = text::tabular_nums});
        if (ui.icon_button(ui.id("comments-close"), close, "x", C::icon)) {
            shell_.panel.show(SidePanel::Kind::none);
        }

        // Two sides of the record: the loved and the latest, a short bar gliding under the one playing.
        const bool sortable = provider(thread().source).capabilities.comment_sort;
        const std::array<std::string_view, 2> labels = {lyren::tr(sortable ? "Popular" : "Comments"), lyren::tr("Newest")};
        const TextStyle tab_style{.size = t.fs(type::body)};
        float x = bar.x, centre = 0;
        for (int i = 0; i < (sortable ? 2 : 1); ++i) {
            float lw = p.measure(labels[static_cast<size_t>(i)], tab_style);
            Rect cell{x, bar.y, lw, bar.h};
            auto it = ui.interact(ui.id(i ? "comment-tab-new" : "comment-tab-hot"), cell.inset(-10, 0));
            float h = ui.anim(ui.id(i ? "comment-tab-new" : "comment-tab-hot"), it.hovered ? 1 : 0, 140);
            bool on = thread().tab == i;
            ui.label(cell, labels[static_cast<size_t>(i)], {.size = tab_style.size, .color = on ? t.text : t.text_dim.mix(t.text, h * 0.6f)});
            if (on) {
                centre = cell.x + cell.w / 2;
            }
            if (it.clicked && !on) {
                comments_.set_tab(i);
                ui.redraw();
            }
            x += lw + 24;
        }
        float cx = ui.spring(ui.id("comment-tab-bar"), centre - bar.x, 300, 0.2f) + bar.x;
        p.rect({cx - 8, bar.bottom() - 4, 16, 3}, t.accent, 1.5f);
        ui.disabled = thread().feeds[thread().tab]->loading;
        if (ui.icon_button(ui.id("comments-refresh"), {bar.right() - C::control + 6, bar.y + (bar.h - C::control) / 2, C::control, C::control}, "refresh-cw", C::small_icon)) {
            comments_.refresh();
        }
        ui.disabled = false;
        p.rect({r.x + C::inset, bar.bottom(), r.w - C::inset * 2, 1}, t.border.with_alpha(0.6f));
        return header_h;
    }

    void CommentsView::draw(ui::Ui &ui, Rect r) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto reply = thread().replies;
        ui::Id list_id = ui.id(reply ? "comment-replies" : thread().tab ? "comments-new" : "comments-hot");
        if (std::exchange(thread().to_top, false)) {
            ui.scroll_to(list_id, 0);
        }
        bool compact = r.h < 420;
        float header_h;
        if (reply) {
            Rect bar{r.x + C::inset, r.y + C::padding - 4, r.w - C::inset * 2, C::control};
            if (ui.icon_button(ui.id("replies-back"), {bar.x - 6, bar.y, C::control, C::control}, "chevron-left", C::icon + 2)) {
                thread().replies.reset();
                return ui.redraw();
            }
            int total = !reply->feed->page ? reply->root.replies : reply->feed->total;
            auto heading = lyren::tr("Replies");
            float hw = p.measure(heading, {.size = t.fs(C::title)});
            ui.label({bar.x + C::control, bar.y, hw + 1, bar.h}, heading, {.size = t.fs(C::title), .color = t.text});
            if (total > 0) {
                ui.label({bar.x + C::control + hw + C::gap, bar.y + 1, 80, bar.h}, grouped(total), {.size = t.fs(C::meta), .color = t.text_dim, .features = text::tabular_nums});
            }
            ui.disabled = reply->feed->loading;
            if (ui.icon_button(ui.id("comments-refresh"), {bar.right() - C::control * 2 - C::small_gap + 6, bar.y, C::control, C::control}, "refresh-cw", C::small_icon)) {
                reply->feed = std::make_shared<Feed>();
                comments_.load_replies(reply);
            }
            ui.disabled = false;
            if (ui.icon_button(ui.id("comments-close"), {bar.right() - C::control + 6, bar.y, C::control, C::control}, "x", C::icon)) {
                shell_.panel.show(SidePanel::Kind::none);
            }
            // The comment answered, pinned above its replies as a small quote.
            const float line_h = std::round(t.fs(C::name) * 1.6f);
            const float qw = bar.w - C::padding * 2;
            auto lines = p.wrap_lines(reply->root.text, t.fs(C::name), qw);
            size_t shown = std::min(lines.size(), compact ? size_t{1} : size_t{3});
            Rect quote{bar.x, bar.bottom() + C::gap + 2, bar.w, C::padding * 2 + 24 + C::small_gap + static_cast<float>(shown) * line_h};
            p.gradient(quote, t.accent.with_alpha(0.1f), t.accent.with_alpha(0.03f), t.radius_l());
            p.icon("quote-fill", {quote.right() - C::padding - 28, quote.y + C::gap + 2, 28, 28}, t.accent.with_alpha(0.18f));
            cover(ui, {quote.x + C::padding, quote.y + C::padding, 24, 24}, reply->root.avatar, 12, "user");
            ui.label({quote.x + C::padding + 24 + C::gap, quote.y + C::padding, qw - 24 - C::gap - 32, 24}, reply->root.user, {.size = t.fs(C::meta), .color = t.text_dim});
            for (size_t i = 0; i < shown; ++i) {
                ui.label({quote.x + C::padding, quote.y + C::padding + 24 + C::small_gap + static_cast<float>(i) * line_h, qw, line_h}, lines[i],
                         {.size = t.fs(C::name), .color = t.text});
            }
            header_h = quote.bottom() + C::gap - r.y;
        } else {
            header_h = sleeve(ui, r, compact);
        }
        Feed &f = reply ? *reply->feed : *thread().feeds[thread().tab];
        if (f.items.empty() && f.more && !f.loading && f.error.empty()) {
            if (reply) {
                comments_.load_replies(reply);
            } else {
                comments_.load(thread().tab);
            }
        }
        Rect view{r.x, r.y + header_h, r.w, r.h - header_h};
        float composer_h = composer(ui, view);

        float enter = ui.progress(reply ? reply->opened : thread().tab_since, 240);
        const float x = r.x + C::inset + (1 - enter) * C::padding * (reply || thread().tab ? 1 : -1), w = r.w - C::inset * 2;
        {
            Fade fade(p, enter * ui.progress(f.since, 200));
            if (f.items.empty()) {
                ui.push_clip(view);
                Rect room{view.x, view.y, view.w, view.h - composer_h};
                if (f.loading || !f.error.empty()) {
                    page_status(ui, room, f.loading, f.error, false);
                } else if (room.h < 150) {
                    ui.label(room, lyren::tr(reply ? "No replies yet" : "No comments yet"), {.size = t.fs(C::name), .color = t.text_dim}, Align::center);
                } else {
                    // Nothing said yet: a blank record waiting for its first words.
                    Rect c = ui::center(room, std::min(room.w, 320.f), 150);
                    Rect disc{c.x + (c.w - 64) / 2, c.y, 64, 64};
                    vinyl(p, disc, t.accent.with_alpha(0.85f), 0.6f);
                    ui.label({c.x, c.y + 80, c.w, 22}, lyren::tr(reply ? "No replies yet" : "No comments yet"), {.size = t.fs(type::lead), .color = t.text}, Align::center);
                    ui.label({c.x, c.y + 106, c.w, 20}, lyren::tr("Join the conversation"), {.size = t.fs(C::meta), .color = t.text_dim}, Align::center);
                }
                ui.pop_clip();
            } else {
                // Heights are kept, the wrapped lines are not: they would point into comments that move as more arrive.
                const bool featured = !reply && thread().tab == 0 && provider(thread().source).capabilities.comment_sort;
                float size = t.fs(C::body);
                if (f.heights.width != w || f.heights.size != size) {
                    f.heights.rows.clear(), f.heights.width = w, f.heights.size = size;
                }
                while (f.heights.rows.size() < f.items.size()) {
                    size_t i = f.heights.rows.size();
                    f.heights.rows.push_back(comment(ui, f.items[i], 0, 0, w, false, featured && i == 0));
                }
                // Below the last comment: the spinner while more load, or a mark that there are no more; then room to
                // scroll the last one clear of the writing button.
                float tail = f.loading || !f.more || !f.error.empty() ? 64 : C::padding;
                float content = tail + composer_h;
                for (float h : f.heights.rows) {
                    content += h;
                }
                float off = ui.begin_scroll(list_id, view, content);
                Rect clip = ui.clip();
                float y = view.y - off;
                for (size_t i = 0; i < f.items.size(); ++i) {
                    float h = f.heights.rows[i];
                    if (y + h > clip.y && y < clip.bottom()) {
                        comment(ui, f.items[i], x, y, w, true, featured && i == 0);
                    }
                    y += h;
                }
                if (f.loading) {
                    ui.spinner(ui::center({view.x, y, view.w, 64}, 20, 20), t.text_dim, 2);
                } else if (!f.error.empty()) {
                    ui.label({view.x + C::inset, y + C::small_gap, view.w - C::inset * 2, C::control}, f.error, {.size = t.fs(C::meta), .color = t.text_dim});
                    if (ui.button(ui.id("comments-retry"), {view.x + (view.w - 100) / 2, y + 32, 100, 28}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                        f.error.clear();
                        if (reply) {
                            comments_.load_replies(reply);
                        } else {
                            comments_.load(thread().tab);
                        }
                    }
                } else if (f.more && y < view.bottom() + 400) {
                    if (reply) {
                        comments_.load_replies(reply);
                    } else {
                        comments_.load(thread().tab);
                    }
                } else if (!f.more) {
                    // The run-out groove: the end of the side.
                    const TextStyle end{.size = t.fs(C::meta), .color = t.text_dim.with_alpha(0.7f)};
                    const std::string_view end_words = lyren::tr("No more comments");
                    const float ew = p.measure(end_words, end), cx = view.x + view.w / 2, ey = y + 14;
                    ui.label({cx - ew / 2, ey, ew + 1, 40}, end_words, end);
                    for (float side : {-1.f, 1.f}) {
                        Rect rail{side < 0 ? cx - ew / 2 - C::gap * 5 : cx + ew / 2 + C::gap, ey + 20, C::gap * 4, 1};
                        p.hgradient(rail, t.border.with_alpha(side < 0 ? 0 : 1), t.border.with_alpha(side < 0 ? 1 : 0), 0.5f);
                    }
                }
                if (t.surface.a > 0.9f && off > 0.5f) {
                    p.gradient({view.x + 1, view.y, view.w - 2, C::padding}, t.surface, t.surface.with_alpha(0));
                }
                ui.end_scroll();
            }
        }
        p.raise(composer_layer_.first, composer_layer_.second);
    }

    void CommentsView::picture(ui::Ui &ui, const Comment &c, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &e = env_.images.picture(c.image, static_cast<uint32_t>(Images::tier(std::max(r.w, r.h) * p.scale())));
        float a = e.ready ? ui.progress(e.ready_at, 320, ui::ease::standard) : 0;
        float aspect = e.ready && e.h ? static_cast<float>(e.w) / static_cast<float>(e.h) : 1;
        if (c.sticker) {
            // Stickers stand on the page as they are, whole and unframed.
            if (e.ready) {
                Rect fit = aspect > 1 ? Rect{r.x, r.y + (r.h - r.w / aspect) / 2, r.w, r.w / aspect} : Rect{r.x, r.y, r.h * aspect, r.h};
                Fade fade(p, a);
                p.image(fit, e.tex, 0);
            }
            return;
        }
        auto it = ui.interact(ui.id("comment-picture"), r);
        float hot = ui.anim(ui.id("comment-picture"), it.hovered ? 1 : 0, 160);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (a < 1) {
            p.rect(r, t.text.with_alpha(0.06f), t.radius);
            p.icon("image", ui::center(r, 22, 22), t.text_dim.with_alpha(0.6f * (1 - a)));
        }
        if (e.ready) {
            Fade fade(p, a);
            p.image(r, e.tex, t.radius, white.mix({0.86f, 0.86f, 0.86f, 1}, hot * 0.4f), cover_uv(aspect, r.w / r.h));
        }
        p.rect(r, {0, 0, 0, 0}, t.radius, t.text.with_alpha(0.08f + 0.1f * hot), 1);
        std::string_view url = c.image;
        if (url.substr(0, url.find('?')).ends_with(".gif")) {
            Rect badge{r.x + 8, r.bottom() - 8 - 18, 32, 18};
            p.rect(badge, {0, 0, 0, 0.55f}, 9);
            ui.label(badge, "GIF", t.style(type::micro, white), Align::center);
        }
        if (it.clicked) {
            show_picture(ui, c.image, c.image_w > 0 && c.image_h > 0 ? static_cast<float>(c.image_w) / static_cast<float>(c.image_h) : e.ready ? aspect : 4.f / 3);
        }
    }

    void CommentsView::show_picture(ui::Ui &ui, std::string url, float aspect) {
        float w = ui.width * 0.84f, h = w / aspect;
        if (h > ui.height * 0.84f) {
            h = ui.height * 0.84f, w = h * aspect;
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
                } else {
                    p.rect(r, t.elevated, t.radius_l());
                    u.spinner(ui::center(r, 24, 24), t.text_dim, 2);
                }
                if (u.interact(u.id("picture-view"), r).clicked) {
                    u.close_modal();
                }
            },
            true);
    }

    float CommentsView::comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw, bool featured) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Source source = thread().source;
        const float pad = featured ? 18 : 0;
        const TextStyle body{.size = t.fs(featured ? C::quote : C::body), .color = t.text}, quote{.size = t.fs(C::name), .color = t.text_dim};
        const float lh = std::round(body.size * (featured ? 1.7f : 1.65f)), qh = std::round(quote.size * 1.55f);
        const float name_h = std::ceil(t.fs(C::name) * 1.45f), time_h = std::ceil(t.fs(C::time) * 1.45f);
        const float action_h = std::max(C::control, std::ceil(t.fs(C::meta) * 1.6f));
        const float tx = featured ? x + pad : x + C::avatar + 12, tw = x + w - pad - tx;
        std::string replied = c.reply.empty() ? "" : '@' + c.reply_user + "：" + c.reply;
        auto lines = c.text.empty() ? std::vector<std::string_view>{} : p.wrap_lines(c.text, body.size, tw);
        auto quoted = replied.empty() ? std::vector<std::string_view>{} : p.wrap_lines(replied, quote.size, tw - C::padding - C::gap);
        const float quote_h = quoted.empty() ? 0 : static_cast<float>(quoted.size()) * qh + C::gap * 2;
        Rect picture = picture_box(c, tw);

        // One pass lays out both: a quote card with its author underneath, or a row with its author on top.
        float top = featured ? y + C::gap + pad : y + 14;
        float badge_y = top, ident_y = top;
        float cy = featured ? top + std::ceil(t.fs(C::time) * 1.6f) + C::gap : top + std::max(C::avatar, name_h + time_h) + 6;
        float body_y = cy;
        cy += static_cast<float>(lines.size()) * lh;
        float quote_y = cy + C::gap;
        cy += quote_h > 0 ? C::gap + quote_h : 0;
        picture.x = tx, picture.y = cy + C::gap;
        cy += picture.h > 0 ? C::gap + picture.h : 0;
        float author_y = cy + 12;
        if (featured) {
            cy += 12 + 26;
        }
        float action_y = cy + (featured ? 2 : 4);
        cy = action_y + action_h;
        float h = cy + (featured ? 12 + C::gap : 10) - y;
        if (!draw) {
            return h;
        }

        const Likes::Mark mine = comments_.like(c);
        bool liked = mine.liked;
        const auto &caps = provider(source).capabilities;
        bool owned = c.owned && caps.comment_delete && provider(source).logged_in();
        int count = mine.likes;
        std::string n = count > 0 ? short_number(count) : "";
        ui.push_id(ui.id(key(source, c.id)));
        if (featured) {
            // The top comment, set as a pull quote on a card tinted by the record.
            Rect card{x, y + C::gap, w, h - C::gap * 2};
            p.gradient(card, t.accent.with_alpha(0.14f), t.accent.with_alpha(0.035f), t.radius_l());
            p.rect(card, {0, 0, 0, 0}, t.radius_l(), t.accent.with_alpha(0.2f), 1);
            p.icon("quote-fill", {card.right() - pad - 36, card.y + 12, 36, 36}, t.accent.with_alpha(0.16f));
            float bh = std::ceil(t.fs(C::time) * 1.6f);
            p.icon("sparkles", {tx, badge_y + (bh - 12) / 2, 12, 12}, t.accent);
            ui.label({tx + 12 + C::small_gap + 2, badge_y, tw, bh}, lyren::tr("Top comment"), {.size = t.fs(C::time), .color = t.accent});
        } else {
            cover(ui, {x, ident_y, C::avatar, C::avatar}, c.avatar, C::avatar / 2, "user");
        }
        // The like: a count to read before anything else on a row, beside the thanks on a card.
        float pop = ui.spring(ui.id("comment-like-pop"), liked ? 1 : 0, 320, 0.45f);
        float like_w = comment_action_width(ui, n);
        Rect like = featured ? Rect{x + w - pad - like_w, action_y, like_w, action_h} : Rect{x + w - like_w, ident_y, like_w, name_h};
        {
            ui::Enabled live(ui, caps.comment_likes && !mine.pending);
            if (comment_action(ui, ui.id("comment-like"), like, n, liked ? "thumbs-up-fill" : "thumbs-up", liked ? t.accent : t.text_dim,
                               1 + 0.4f * std::abs(pop - (liked ? 1.f : 0.f)))) {
                comments_.toggle_like(c);
            }
        }
        const float name_w = std::min(p.measure(c.user, {.size = t.fs(C::name)}) + 1, featured ? tw - 24 - C::gap * 2 - 80 : like.x - tx - C::gap);
        if (featured) {
            cover(ui, {tx, author_y + 1, 24, 24}, c.avatar, 12, "user");
            ui.label({tx + 24 + C::gap, author_y, name_w, 26}, c.user, {.size = t.fs(C::name), .color = t.text});
            ui.label({tx + 24 + C::gap * 2 + name_w, author_y + 1, tw, 26}, c.time, {.size = t.fs(C::time), .color = t.text_dim});
        } else {
            ui.label({tx, ident_y, name_w, name_h}, c.user, {.size = t.fs(C::name), .color = t.text.mix(t.text_dim, 0.25f)});
            ui.label({tx, ident_y + name_h, tw, time_h}, c.time, {.size = t.fs(C::time), .color = t.text_dim});
        }
        // The author's picture and name open their page, where the service names them.
        if (!c.user_id.empty() && provider(source).capabilities.profiles) {
            const Rect face = featured ? Rect{tx, author_y + 1, 24, 24} : Rect{x, ident_y, C::avatar, C::avatar};
            const Rect who = featured ? Rect{face.x, author_y, 24 + C::gap + name_w, 26} : Rect{x, ident_y, tx - x + name_w, name_h};
            auto it = ui.interact(ui.id("comment-author"), who);
            if (!featured) {
                // The picture below the name row takes clicks too.
                auto pic = ui.interact(ui.id("comment-face"), face);
                it.clicked |= pic.clicked, it.hovered |= pic.hovered;
            }
            const float hot = ui.anim(ui.id("comment-author"), it.hovered ? 1 : 0, 140);
            if (hot > 0) {
                const float nx = featured ? tx + 24 + C::gap : tx, ny = featured ? author_y + 21 : ident_y + name_h - 2;
                p.rect({nx, ny, name_w, 1}, t.text.with_alpha(0.45f * hot));
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
            Rect q{tx, quote_y, tw, quote_h};
            p.rect(q, t.text.with_alpha(featured ? 0.05f : 0.04f), t.radius_s());
            p.rect({q.x + C::gap, q.y + C::gap, 2, q.h - C::gap * 2}, t.accent.with_alpha(0.5f), 1);
            float qy = q.y + C::gap;
            for (auto line : quoted) {
                ui.label({q.x + C::padding + 2, qy, tw - C::padding - C::gap, qh}, line, quote);
                qy += qh;
            }
        }
        if (picture.h > 0) {
            this->picture(ui, c, picture);
        }
        // Answering it, and the thread already under it.
        std::string_view reply_label = lyren::tr("Reply");
        Rect reply_button{tx, action_y, comment_action_width(ui, reply_label), action_h};
        if (caps.comment_post) {
            auto draft = thread().replies ? thread().replies->draft : thread().draft;
            ui.disabled = draft->sending;
            if (comment_action(ui, ui.id("comment-reply"), reply_button, reply_label, "message-circle", t.text_dim)) {
                draft->reply = c;
                draft->focus = true;
            }
            ui.disabled = false;
        }
        float end = featured ? like.x - C::gap : x + w;
        if (owned) {
            Rect bin{end - C::control + 6, action_y + (action_h - C::control) / 2, C::control, C::control};
            {
                ui::Enabled live(ui, !comments_.removing(c));
                if (ui.icon_button(ui.id("comment-delete"), bin, "trash", C::small_icon)) {
                    confirm_delete(ui, c);
                }
            }
            end = bin.x - C::small_gap;
        }
        if (caps.comment_replies && c.replies > 0) {
            std::string label = lyren::trf("{} replies", c.replies);
            float cw = std::min(thread_chip_width(ui, label), end - reply_button.right() - C::padding);
            if (cw > C::small_icon * 3 && thread_chip(ui, ui.id("comment-replies"), {reply_button.right() + C::padding, action_y + (action_h - 26) / 2, cw, 26}, label)) {
                comments_.open_replies(c);
            }
        }
        if (!featured) {
            p.rect({tx, y + h - 1, tw, 1}, t.border.with_alpha(0.35f));
        }
        ui.pop_id();
        return h;
    }

    void CommentsView::confirm_delete(ui::Ui &ui, Comment comment) {
        std::string detail = comment.text;
        ui.confirm(std::string(lyren::tr("Delete comment?")), std::move(detail), std::string(lyren::tr("Delete")), "trash", [this, comment = std::move(comment)] {
            comments_.remove(comment);
        });
    }

} // namespace lyren
