#include <algorithm>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// The panel beside the page and the stage: the play queue, or the comments on a song, an album or a playlist.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;

    float Shell::panel_width(Panel panel) {
        return panel == Panel::queue ? 336 : panel == Panel::comments ? 408 : 0;
    }

    void Shell::show_panel(Panel panel) {
        if (panel == panel_) {
            return;
        }
        if (panel_ != Panel::none && panel != Panel::none) {
            panel_from_ = panel_, panel_time_ = now_();
        }
        panel_ = panel;
        if (panel != Panel::none) {
            panel_last_ = panel;
        }
    }

    void Shell::side_panel(ui::Ui &ui, Rect slot, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // On the stage the panel is a pane of glass over the cover, standing clear of the controls below it.
        Rect frame{slot.x + 8, slot.y, slot.w - 8, slot.h - std::round(12 * stage)};
        p.rect(frame, t.surface, t.radius_l(), t.border.with_alpha(stage), 1);
        ui.push_clip(frame);
        auto content = [&](Panel kind, Rect r) {
            if (kind == Panel::queue) {
                queue_panel(ui, r);
            } else {
                comments_panel(ui, r);
            }
        };
        // Switching between the two: the old one fades out as the new one comes in from the right.
        float in = ui.progress(panel_time_, 300);
        if (in < 1 && panel_from_ != Panel::none && panel_from_ != panel_last_) {
            Fade out(p, std::max(0.f, 1 - in * 2.5f));
            content(panel_from_, {frame.x, frame.y, panel_width(panel_from_) - 8, frame.h});
        }
        {
            Fade fade(p, in);
            content(panel_last_, {frame.x + (1 - in) * 16, frame.y, panel_width(panel_last_) - 8, frame.h});
        }
        ui.pop_clip();
    }

    void Shell::queue_panel(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ui.label({r.x + 20, r.y + 18, 160, 28}, lyren::tr("Queue"), {.size = t.fs(16), .color = t.text});
        float qw = p.measure(lyren::tr("Queue"), {.size = t.fs(16)});
        ui.label({r.x + 28 + qw, r.y + 19, 100, 28}, std::to_string(queue_.size()), {.size = t.fs(13), .color = t.text_dim});
        std::string_view clear = lyren::tr("Clear");
        float cw = button_width(ui, clear, false) - 12;
        if (ui.button(ui.id("queue-clear"), {r.right() - 16 - cw, r.y + 18, cw, 28}, clear, ui::ButtonKind::ghost) && !queue_.empty()) {
            queue_.clear();
            current_ = -1;
            ++play_gen_;
            want_play_ = fetching_ = false;
            stop_player();
            shuffle_history_.clear();
            changed();
            ui.toast(std::string(lyren::tr("Queue cleared")), "trash");
        }
        constexpr float row_h = 54;
        Rect view{r.x + 8, r.y + 60, r.w - 16, r.h - 68};
        if (queue_.empty()) {
            ui.label({view.x, view.y + 40, view.w, 20}, lyren::tr("Nothing queued"), {.size = t.fs(13), .color = t.text_dim}, Align::center);
            return;
        }
        float off = ui.begin_scroll(ui.id("queue"), view, static_cast<float>(queue_.size()) * row_h);
        int remove = -1;
        // One highlight glides to the playing row.
        float hy = ui.spring(ui.id("queue-current"), static_cast<float>(current_) * row_h, 260);
        float ha = ui.anim(ui.id("queue-current-a"), current_ >= 0 ? 1 : 0, 200);
        if (ha > 0) {
            p.rect({view.x + 2, view.y + hy - off + 2, view.w - 4, row_h - 4}, t.accent.with_alpha(0.14f * ha), t.radius_s());
        }
        ui.push_id(ui.id("queue-rows"));
        int first = static_cast<int>(off / row_h), last = static_cast<int>((off + view.h) / row_h) + 1;
        for (int i = first; i <= last && i < static_cast<int>(queue_.size()); ++i) {
            const Track &tr = queue_[static_cast<size_t>(i)];
            Rect row{view.x, view.y + static_cast<float>(i) * row_h - off, view.w, row_h};
            ui.push_id(ui.id(static_cast<uint64_t>(i)));
            ui::Id row_id = ui.id("row");
            bool hot = ui.hovering(row) || ui.is_active(row_id), cur = i == current_;
            float h = ui.anim(row_id, hot ? 1 : 0, 140);
            if (!cur && h > 0) {
                p.rect(row.inset(2), t.hover.with_alpha(h), t.radius_s());
            }
            cover(ui, {row.x + 10, row.y + 9, 36, 36}, tr.cover, t.radius_s());
            float tw = row.w - 58 - (hot ? 40 : 12);
            p.text(row.x + 58, row.y + 23, tr.title, {.size = t.fs(13), .color = cur ? t.accent : t.text}, tw);
            p.text(row.x + 58, row.y + 41, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
            if (hot && ui.icon_button(ui.id("remove"), {row.right() - 38, row.y + 13, 28, 28}, "x", 14)) {
                remove = i;
            }
            if (ui.interact(row_id, row).double_clicked) {
                play(queue_, i);
            }
            ui.pop_id();
        }
        ui.pop_id();
        ui.end_scroll();
        if (remove >= 0) {
            queue_.erase(queue_.begin() + remove);
            shuffle_history_.clear();
            shuffle_next_ = -1;
            if (remove < current_) {
                --current_;
            } else if (remove == current_) {
                // The playing track goes: carry on with the one after it.
                bool was_playing = playing();
                current_ = queue_.empty() ? -1 : std::min(current_, static_cast<int>(queue_.size()) - 1);
                ++play_gen_;
                stop_player();
                fetching_ = false;
                if (current_ >= 0 && was_playing) {
                    start();
                } else {
                    want_play_ = false;
                }
            }
            changed();
        }
    }

    void Shell::open_comments(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover_url, bool follows) {
        set_thread(ui, source, subject, std::move(id), std::move(title), std::move(cover_url), follows);
        show_panel(Panel::comments);
    }

    void Shell::set_thread(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover_url, bool follows) {
        thread_follows_ = follows;
        // The same thread stays unless its first page failed, which this retries.
        if (thread_source_ == source && thread_subject_ == subject && thread_id_ == id && (feeds_[0].loading || feeds_[0].page > 0)) {
            return;
        }
        thread_source_ = source;
        thread_subject_ = subject;
        thread_id_ = std::move(id);
        thread_title_ = std::move(title);
        thread_cover_ = std::move(cover_url);
        thread_tab_ = 0;
        thread_time_ = ui.now;
        ++thread_gen_;
        for (auto &f : feeds_) {
            f = {};
        }
        for (const char *sid : {"comments-hot", "comments-new"}) {
            ui.scroll_to(ui.id(sid), 0);
        }
        load_comments(0);
    }

    void Shell::load_comments(int tab) {
        Feed &f = feeds_[tab];
        if (f.loading || !f.more || (f.page && !f.error.empty())) {
            return;
        }
        f.loading = true;
        f.error.clear();
        spawn(fetch_comments(thread_gen_, tab, f.page + 1, f.cursor));
    }

    Task<void> Shell::fetch_comments(uint64_t gen, int tab, int page, std::string cursor) {
        auto r = co_await provider(thread_source_).comments(thread_subject_, thread_id_, tab == 0, page, std::move(cursor));
        if (gen != thread_gen_) {
            co_return;
        }
        Feed &f = feeds_[tab];
        f.loading = false;
        if (!r) {
            f.error = r.error();
            co_return redraw_();
        }
        if (!f.page) {
            f.since = now_();
        }
        f.page = page;
        f.total = r->total;
        f.more = r->more && !r->comments.empty();
        f.cursor = std::move(r->cursor);
        f.comments.insert(f.comments.end(), std::make_move_iterator(r->comments.begin()), std::make_move_iterator(r->comments.end()));
        redraw_();
    }

    void Shell::comments_panel(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // What the comments are about: its cover and name, and how many there are.
        Rect art{r.x + 18, r.y + 18, 44, 44};
        cover(ui, art, thread_cover_, t.radius_s(), thread_subject_ == Subject::song ? "music" : thread_subject_ == Subject::album ? "disc-album" : "list-music");
        float tx = art.right() + 12, tw = r.right() - tx - 52;
        Feed &f = feeds_[thread_tab_];
        float head = p.text(tx, art.y + 18, lyren::tr("Comments"), {.size = t.fs(15), .color = t.text}, tw);
        if (f.total > 0) {
            p.text(tx + head + 8, art.y + 18, grouped(f.total), {.size = t.fs(12), .color = t.text_dim, .features = text::tabular_nums}, tw - head - 8);
        }
        p.text(tx, art.y + 38, thread_title_, {.size = t.fs(12), .color = t.text_dim}, tw);
        if (ui.icon_button(ui.id("comments-close"), {r.right() - 46, r.y + 16, 32, 32}, "x", 16)) {
            show_panel(Panel::none);
        }

        const std::array<std::string_view, 2> tabs = {lyren::tr("Popular"), lyren::tr("Newest")};
        float tabs_w = button_width(ui, tabs[0], false) + button_width(ui, tabs[1], false) - 16;
        if (ui.tabs(ui.id("comment-tabs"), {r.x + 18, r.y + 78, tabs_w, 32}, tabs, thread_tab_, ui::TabStyle::pill)) {
            thread_time_ = ui.now;
            load_comments(thread_tab_);
        }
        p.rect({r.x + 18, r.y + 124, r.w - 36, 1}, t.border);

        float enter = ui.progress(thread_time_, 380);
        Fade fade(p, enter);
        Rect view{r.x, r.y + 125, r.w, r.h - 125};
        const float x = r.x + 18 + (1 - enter) * 20 * (thread_tab_ ? 1 : -1), w = r.w - 36;
        if (f.comments.empty()) {
            page_status(ui, view, f.loading, f.error.empty() && !f.loading ? std::string(lyren::tr("No comments yet")) : f.error, false);
            return;
        }
        // Heights are kept, the wrapped lines are not: they would point into comments that move as more arrive.
        float size = t.fs(14);
        if (f.heights_w != w || f.heights_size != size) {
            f.heights.clear(), f.heights_w = w, f.heights_size = size;
        }
        while (f.heights.size() < f.comments.size()) {
            f.heights.push_back(comment(ui, f.comments[f.heights.size()], 0, 0, w, false));
        }
        // Below the last comment: the spinner while more load, or a mark that there are no more.
        float content = f.loading || !f.more ? 72 : 24;
        for (float h : f.heights) {
            content += h;
        }
        float off = ui.begin_scroll(ui.id(thread_tab_ ? "comments-new" : "comments-hot"), view, content);
        Rect clip = ui.clip();
        float y = view.y - off;
        for (size_t i = 0; i < f.comments.size(); ++i) {
            float h = f.heights[i];
            if (y + h > clip.y && y < clip.bottom()) {
                // The first page arrives one comment after another.
                float a = ui.progress(f.since + 0.03 * static_cast<double>(std::min<size_t>(i, 10)), 380);
                Fade arrive(p, a);
                comment(ui, f.comments[i], x, y + (1 - a) * 12, w, true);
            }
            y += h;
        }
        if (f.loading) {
            ui.spinner(ui::center({view.x, y, view.w, 64}, 20, 20), t.text_dim, 2);
        } else if (f.more && y < view.bottom() + 400) {
            load_comments(thread_tab_);
        } else if (!f.more) {
            const TextStyle end{.size = t.fs(12), .color = t.text_dim.with_alpha(0.7f)};
            const std::string_view words = lyren::tr("No more comments");
            const float ww = p.measure(words, end), cx = view.x + view.w / 2;
            ui.label({cx - ww / 2, y + 14, ww + 1, 40}, words, end);
            for (float side : {-1.f, 1.f}) {
                p.rect({cx + side * (ww / 2 + 12) - (side < 0 ? 24 : 0), y + 34, 24, 1}, t.border);
            }
        }
        ui.end_scroll();
    }

    float Shell::comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        TextStyle body{.size = t.fs(14), .color = t.text}, quote{.size = t.fs(13), .color = t.text_dim}, small{.size = t.fs(12), .color = t.text_dim};
        const float lh = std::round(body.size * 1.6f), qh = std::round(quote.size * 1.55f), tx = x + 44, tw = w - 44;
        std::string replied = c.reply.empty() ? "" : '@' + c.reply_user + "：" + c.reply;
        auto lines = wrap_lines(p, c.text, body.size, tw);
        std::vector<std::string_view> quoted;
        if (!replied.empty()) {
            quoted = wrap_lines(p, replied, quote.size, tw - 24);
        }
        // Avatar beside the name and likes, then the text, the comment it answers and the time.
        float quote_h = quoted.empty() ? 0 : static_cast<float>(quoted.size()) * qh + 22;
        float h = 16 + 24 + static_cast<float>(lines.size()) * lh + quote_h + 22 + 16;
        if (!draw) {
            return h;
        }
        float top = y + 16;
        cover(ui, {x, top, 32, 32}, c.avatar, 16, "user");
        float likes_w = 0;
        if (c.likes > 0) {
            std::string n = short_count(c.likes);
            float nw = p.measure(n, small);
            likes_w = nw + 22;
            ui.label({x + w - nw, top, nw + 1, 18}, n, small);
            p.icon("thumbs-up", {x + w - nw - 18, top + 2, 13, 13}, t.text_dim);
        }
        p.text(tx, top + 13, c.user, {.size = t.fs(13), .color = t.text_dim.mix(t.text, 0.35f)}, tw - likes_w - 8);
        float ly = top + 22;
        for (auto l : lines) {
            p.text(tx, ly + lh * 0.72f, l, body, tw);
            ly += lh;
        }
        if (!quoted.empty()) {
            Rect q{tx, ly + 8, tw, quote_h - 10};
            p.rect(q, t.text.with_alpha(0.05f), t.radius_s());
            p.rect({q.x, q.y + 6, 2, q.h - 12}, t.text.with_alpha(0.14f), 1);
            float qy = q.y + 6;
            for (auto l : quoted) {
                p.text(q.x + 12, qy + qh * 0.72f, l, quote, tw - 24);
                qy += qh;
            }
            ly += quote_h;
        }
        p.text(tx, ly + 18, c.time, {.size = small.size, .color = t.text_dim.with_alpha(0.75f)}, tw);
        p.rect({tx, y + h - 1, tw, 1}, t.border.with_alpha(0.7f));
        return h;
    }

} // namespace lyren
