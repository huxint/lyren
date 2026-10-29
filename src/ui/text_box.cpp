#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui/ui.hpp"

namespace lyren::ui {

    constexpr double blink_half = 0.53, blink_for = 10;

    // Pen x (em) where the cluster containing byte b starts.
    static float x_at(const text::Shaped &sh, size_t size, size_t b) {
        if (b >= size) {
            return sh.width;
        }
        for (auto &g : sh.glyphs) {
            if (g.cluster >= b) {
                return g.x;
            }
        }
        return sh.width;
    }

    // Cluster boundary nearest to x (em).
    static size_t offset_at(const text::Shaped &sh, size_t size, float x) {
        auto &gs = sh.glyphs;
        for (size_t i = 0; i < gs.size();) {
            size_t j = i + 1;
            while (j < gs.size() && gs[j].cluster == gs[i].cluster) {
                ++j;
            }
            float end = j < gs.size() ? gs[j].x : sh.width;
            if (x < (gs[i].x + end) / 2) {
                return gs[i].cluster;
            }
            i = j;
        }
        return size;
    }

    struct EditLine {
        size_t from, to;
    };

    // Unlike display text, an editor keeps every space and the empty line after a trailing newline.
    static std::vector<EditLine> edit_lines(Painter &p, std::string_view text, float size, float width) {
        std::vector<EditLine> lines;
        size_t from = 0;
        for (;;) {
            size_t end = std::min(text.find('\n', from), text.size());
            auto paragraph = text.substr(from, end - from);
            const auto &sh = p.shape(paragraph, {.size = size});
            const auto &g = sh.glyphs;
            struct Cluster {
                size_t byte;
                float width;
            };
            std::vector<Cluster> clusters;
            for (size_t i = 0; i < g.size();) {
                size_t next = i + 1;
                while (next < g.size() && g[next].cluster == g[i].cluster) {
                    ++next;
                }
                float right = next < g.size() ? g[next].x : sh.width;
                clusters.push_back({g[i].cluster, (right - g[i].x) * size});
                i = next;
            }
            // HarfBuzz returns RTL glyphs in visual order; line boundaries still follow the source bytes.
            std::ranges::sort(clusters, {}, &Cluster::byte);
            size_t start = 0, word = 0;
            float used = 0;
            for (size_t i = 0; i < clusters.size(); ++i) {
                if (clusters[i].byte > 0 && paragraph[clusters[i].byte - 1] == ' ') {
                    word = i;
                }
                used += clusters[i].width;
                while (i > start && used > width) {
                    size_t cut = word > start ? word : i;
                    lines.push_back({from + clusters[start].byte, from + clusters[cut].byte});
                    while (start < cut) {
                        used -= clusters[start++].width;
                    }
                }
            }
            lines.push_back({from + (clusters.empty() ? 0 : clusters[start].byte), end});
            if (end == text.size()) {
                return lines;
            }
            from = end + 1;
        }
    }

    static size_t edit_row(const std::vector<EditLine> &lines, size_t byte, bool line_end) {
        size_t row = static_cast<size_t>(std::ranges::upper_bound(lines, byte, {}, &EditLine::from) - lines.begin() - 1);
        return line_end && row > 0 && lines[row - 1].to == byte ? row - 1 : row;
    }

    void Ui::edit_input(Id id, EditBox &box, std::string &text, bool &submitted, float multiline_width) {
        FrameInput &in = *in_;
        TextEdit &e = box.edit;
        bool multiline = multiline_width > 0;
        bool touched = false;
        // Input method, in the protocol's order: delete around the cursor, insert the commit, then the new preedit.
        if (in.delete_before || in.delete_after) {
            size_t a = char_boundary(text, e.lo() - std::min<size_t>(e.lo(), in.delete_before));
            size_t b = char_boundary(text, e.hi() + in.delete_after);
            e.erase(text, a, b);
            touched = true;
        }
        if (!in.commit.empty()) {
            e.insert(text, in.commit, multiline), touched = true;
        }
        if (in.preedit) {
            box.preedit = *in.preedit;
            box.preedit_cursor = in.preedit_cursor;
            touched = true;
        }
        if (in.paste) {
            e.insert(text, *in.paste, multiline), touched = true;
        }
        if (touched) {
            box.preferred_x = -1;
            box.line_end = false;
        }
        in.delete_before = in.delete_after = 0;
        in.commit.clear();
        in.preedit.reset();
        in.paste.reset();

        for (auto k = in.keys.begin(); k != in.keys.end() && focus == id;) {
            std::string copied;
            TextEdit::Key r;
            bool up = k->keysym == XKB_KEY_Up || k->keysym == XKB_KEY_KP_Up;
            bool down = k->keysym == XKB_KEY_Down || k->keysym == XKB_KEY_KP_Down;
            bool home = k->keysym == XKB_KEY_Home || k->keysym == XKB_KEY_KP_Home;
            bool end = k->keysym == XKB_KEY_End || k->keysym == XKB_KEY_KP_End;
            if (multiline && (up || down || ((home || end) && !(k->mods & wl::mod_ctrl)))) {
                float size = theme_->fs(13);
                auto lines = edit_lines(*p_, text, size, multiline_width);
                size_t row = edit_row(lines, e.cursor, box.line_end);
                size_t at;
                if (up || down) {
                    auto line = std::string_view(text).substr(lines[row].from, lines[row].to - lines[row].from);
                    if (box.preferred_x < 0) {
                        box.preferred_x = x_at(p_->shape(line, {}), line.size(), e.cursor - lines[row].from);
                    }
                    row = up ? (row ? row - 1 : 0) : std::min(row + 1, lines.size() - 1);
                    line = std::string_view(text).substr(lines[row].from, lines[row].to - lines[row].from);
                    at = lines[row].from + offset_at(p_->shape(line, {}), line.size(), box.preferred_x);
                } else {
                    at = home ? lines[row].from : lines[row].to;
                    box.preferred_x = -1;
                }
                e.move(at, k->mods & wl::mod_shift);
                box.line_end = at == lines[row].to;
                r = TextEdit::Key::moved;
            } else {
                auto before = e;
                r = e.apply(text, *k, copied, multiline);
                if (r == TextEdit::Key::edited || e.cursor != before.cursor || e.anchor != before.anchor) {
                    box.preferred_x = -1;
                    box.line_end = k->keysym == XKB_KEY_Left || k->keysym == XKB_KEY_KP_Left;
                }
            }
            if (r == TextEdit::Key::ignored) {
                ++k;
                continue;
            }
            k = in.keys.erase(k);
            touched = true;
            if (r == TextEdit::Key::submit) {
                submitted = true;
            } else if (r == TextEdit::Key::cancel) {
                focus = 0;
            } else if (r == TextEdit::Key::copy) {
                copy = std::move(copied);
            } else if (r == TextEdit::Key::paste) {
                want_paste = true;
            }
        }
        if (touched) {
            consumed_ = true, box.blink = now;
        }
    }

    bool Ui::text_box(Id id, Rect r, std::string &text, std::string_view placeholder, std::string_view icon) {
        const Theme &t = *theme_;
        EditBox &box = edits_[id];
        box.edit.clamp(text);
        TextStyle st{.size = t.fs(13), .color = t.text};
        constexpr float pad = 14;
        bool has_clear = !text.empty();
        Rect clear_r{r.right() - r.h, r.y, r.h, r.h};
        Rect inner{r.x + pad + (icon.empty() ? 0 : 24), r.y, 0, r.h};
        inner.w = (has_clear ? clear_r.x : r.right() - pad) - inner.x;

        // The clear button sits inside the field, so it asks for presses first.
        Interaction clear = has_clear ? interact(sub_id(id, 0xc1), clear_r) : Interaction{};
        Interaction it = interact(id, r);
        if (it.hovered && !clear.hovered) {
            cursor = wl::Cursor::text;
        }
        if (clear.clicked) {
            text.clear();
            box.edit = {};
            focus = id;
        }
        auto hit = [&](float px) {
            return offset_at(p_->shape(text, st), text.size(), (px - inner.x + box.scroll_x) / st.size);
        };
        if (it.pressed) {
            focus = id;
            box.blink = now;
            size_t at = hit(it.px);
            if (it.double_clicked) {
                box.edit.select_word(text, at);
            } else {
                box.edit.move(at, in_->mods & wl::mod_shift);
            }
        } else if (it.held && focus == id) {
            box.edit.move(hit(in_->x), true);
        }

        bool focused = focus == id, submitted = false;
        if (focused) {
            edit_input(id, box, text, submitted);
        } else {
            box.preedit.clear();
        }
        focused = focus == id;
        box.edit.clamp(text);

        float hot = anim(id, it.hovered ? 1 : 0, 120), f = anim(sub_id(id, 0xf0), focused ? 1 : 0, 120);
        Color fill = t.text.with_alpha(0.06f + 0.03f * hot * (1 - f));
        p_->rect(r, fill, r.h / 2, fill.mix(t.accent.with_alpha(0.8f), f), 1);
        if (!icon.empty()) {
            p_->icon(icon, {r.x + pad, r.y + (r.h - 16) / 2, 16, 16}, t.text_dim.mix(t.text, f));
        }

        // What is shown: the text with the preedit spliced in at the cursor.
        const TextEdit &e = box.edit;
        std::string composed;
        std::string_view shown = text;
        size_t caret = e.cursor, pre_begin = 0, pre_end = 0;
        bool show_caret = focused;
        if (focused && !box.preedit.empty()) {
            composed = text.substr(0, e.cursor) + box.preedit + text.substr(e.cursor);
            shown = composed;
            pre_begin = e.cursor, pre_end = e.cursor + box.preedit.size();
            show_caret = box.preedit_cursor >= 0;
            caret = pre_begin + std::min(static_cast<size_t>(std::max(box.preedit_cursor, 0)), box.preedit.size());
        }
        const auto &sh = p_->shape(shown, st);
        auto x_of = [&](size_t b) {
            return x_at(sh, shown.size(), b) * st.size;
        };
        float text_w = sh.width * st.size;
        if (focused) {
            float cx = x_of(caret);
            if (cx - box.scroll_x > inner.w - 2) {
                box.scroll_x = cx - inner.w + 2;
            }
            if (cx < box.scroll_x) {
                box.scroll_x = cx;
            }
        }
        box.scroll_x = std::clamp(box.scroll_x, 0.f, std::max(0.f, text_w - inner.w + 2));

        float ox = inner.x - box.scroll_x;
        float a = p_->ascent(st.size), d = p_->descent(st.size);
        float baseline = std::round(r.y + (r.h - a - d) / 2 + a);
        float line_h = std::round(st.size * 1.4f), line_y = r.y + (r.h - line_h) / 2;
        push_clip({inner.x - 2, r.y, inner.w + 4, r.h});
        if (shown.empty()) {
            p_->text(inner.x, baseline, placeholder, {.size = st.size, .color = t.text_dim}, inner.w);
        } else {
            if (focused && e.has_selection()) {
                auto shift = [&](size_t b) {
                    return b > pre_begin ? b + (pre_end - pre_begin) : b;
                };
                float x0 = x_of(shift(e.lo())), x1 = x_of(shift(e.hi()));
                p_->rect({ox + x0, line_y, x1 - x0, line_h}, t.accent.with_alpha(0.35f), 2);
            }
            p_->text(ox, baseline, shown, st);
            if (pre_end > pre_begin) {
                p_->rect({ox + x_of(pre_begin), baseline + 3, x_of(pre_end) - x_of(pre_begin), 1}, t.text);
            }
        }
        if (focused) {
            float cx = std::round(ox + x_of(show_caret ? caret : pre_begin));
            double since = now - box.blink;
            if (show_caret && (since > blink_for || std::fmod(since, 2 * blink_half) < blink_half)) {
                p_->rect({cx, line_y, 1, line_h}, t.text);
            }
            if (show_caret && since <= blink_for) {
                wake(box.blink + (std::floor(since / blink_half) + 1) * blink_half);
            }
            text_input = {.enabled = true,
                          .x = static_cast<int>(cx),
                          .y = static_cast<int>(line_y),
                          .w = 1,
                          .h = static_cast<int>(line_h),
                          .surrounding = text,
                          .cursor = static_cast<int>(e.cursor),
                          .anchor = static_cast<int>(e.anchor)};
        }
        pop_clip();

        if (has_clear) {
            float ch = anim(sub_id(id, 0xc1), clear.hovered ? 1 : 0, 120);
            p_->icon("circle-x", center(clear_r, 16, 16), t.text_dim.mix(t.text, ch));
        }
        return submitted;
    }

    void Ui::text_area(Id id, Rect r, std::string &text, std::string_view placeholder) {
        const Theme &t = *theme_;
        EditBox &box = edits_[id];
        box.edit.clamp(text);
        TextStyle st{.size = t.fs(13), .color = t.text};
        const float line_h = std::round(st.size * 1.55f);
        Rect inner = r.inset(10);
        auto it = interact(id, r);
        if (it.hovered) {
            cursor = wl::Cursor::text;
        }
        auto hit = [&](float x, float y) {
            auto lines = edit_lines(*p_, text, st.size, inner.w);
            size_t row = static_cast<size_t>(std::clamp(std::floor((y - inner.y + box.scroll_y) / line_h), 0.f, static_cast<float>(lines.size() - 1)));
            auto line = std::string_view(text).substr(lines[row].from, lines[row].to - lines[row].from);
            size_t at = lines[row].from + offset_at(p_->shape(line, st), line.size(), (x - inner.x) / st.size);
            box.line_end = at == lines[row].to;
            return at;
        };
        if (it.pressed) {
            focus = id;
            box.blink = now;
            box.preferred_x = -1;
            size_t at = hit(it.px, it.py);
            if (it.double_clicked) {
                box.edit.select_word(text, at);
                box.line_end = false;
            } else {
                box.edit.move(at, in_->mods & wl::mod_shift);
            }
        } else if (it.held && focus == id) {
            box.edit.move(hit(in_->x, in_->y), true);
            box.blink = now;
        }

        bool focused = focus == id && !disabled, submitted = false;
        if (focused) {
            edit_input(id, box, text, submitted, inner.w);
        } else {
            box.preedit.clear();
        }
        focused = focus == id && !disabled;
        box.edit.clamp(text);
        float f = anim(sub_id(id, 0xf0), focused ? 1 : 0, 120);
        Color fill = t.text.with_alpha(0.05f);
        p_->rect(r, fill, t.radius_s(), t.border.mix(t.accent, f), 1);

        const TextEdit &e = box.edit;
        std::string composed;
        std::string_view shown = text;
        size_t caret = e.cursor, pre_begin = 0, pre_end = 0;
        bool show_caret = focused;
        if (focused && !box.preedit.empty()) {
            composed = text.substr(0, e.cursor) + box.preedit + text.substr(e.cursor);
            shown = composed;
            pre_begin = e.cursor, pre_end = e.cursor + box.preedit.size();
            show_caret = box.preedit_cursor >= 0;
            caret = pre_begin + std::min(static_cast<size_t>(std::max(box.preedit_cursor, 0)), box.preedit.size());
        }
        auto lines = edit_lines(*p_, shown, st.size, inner.w);
        size_t caret_row = edit_row(lines, caret, box.line_end);
        float caret_top = static_cast<float>(caret_row) * line_h;
        float max_scroll = std::max(0.f, static_cast<float>(lines.size()) * line_h - inner.h);
        if (focused && box.blink == now) {
            if (caret_top < box.scroll_y) {
                box.scroll_y = caret_top;
            } else if (caret_top + line_h > box.scroll_y + inner.h) {
                box.scroll_y = caret_top + line_h - inner.h;
            }
        }
        if (it.hovered && in_->scroll_y != 0) {
            box.scroll_y += in_->scroll_y;
            in_->scroll_y = 0;
            consumed_ = true;
        }
        box.scroll_y = std::clamp(box.scroll_y, 0.f, max_scroll);
        auto shifted = [&](size_t byte) {
            return byte > pre_begin ? byte + pre_end - pre_begin : byte;
        };
        float origin = inner.y - box.scroll_y;
        float baseline = (line_h - p_->ascent(st.size) - p_->descent(st.size)) / 2 + p_->ascent(st.size);
        push_clip(inner);
        for (size_t i = 0; i < lines.size(); ++i) {
            float y = origin + static_cast<float>(i) * line_h;
            if (y + line_h < inner.y || y > inner.bottom()) {
                continue;
            }
            const auto &line = lines[i];
            auto part = shown.substr(line.from, line.to - line.from);
            const auto &sh = p_->shape(part, st);
            auto x_of = [&](size_t byte) {
                return inner.x + x_at(sh, part.size(), std::clamp(byte, line.from, line.to) - line.from) * st.size;
            };
            if (focused && e.has_selection() && shifted(e.lo()) <= line.to && shifted(e.hi()) > line.from) {
                float left = x_of(shifted(e.lo()));
                float right = shifted(e.hi()) > line.to ? inner.right() : x_of(shifted(e.hi()));
                p_->rect({left, y, right - left, line_h}, t.accent.with_alpha(0.35f), 2);
            }
            if (pre_end > line.from && pre_begin < line.to) {
                float left = x_of(pre_begin), right = x_of(pre_end);
                p_->rect({left, y + baseline + 3, right - left, 1}, t.text);
            }
            p_->text(inner.x, y + baseline, part, st);
        }
        if (shown.empty()) {
            p_->text(inner.x, inner.y + baseline, placeholder, {.size = st.size, .color = t.text_dim}, inner.w);
        }
        if (focused) {
            const auto &line = lines[caret_row];
            auto part = shown.substr(line.from, line.to - line.from);
            float cx = std::round(inner.x + x_at(p_->shape(part, st), part.size(), caret - line.from) * st.size);
            float cy = origin + caret_top;
            double since = now - box.blink;
            if (show_caret && (since > blink_for || std::fmod(since, 2 * blink_half) < blink_half)) {
                p_->rect({cx, cy, 1, line_h}, t.text);
            }
            if (show_caret && since <= blink_for) {
                wake(box.blink + (std::floor(since / blink_half) + 1) * blink_half);
            }
            text_input = {.enabled = true,
                          .x = static_cast<int>(cx),
                          .y = static_cast<int>(std::clamp(cy, inner.y, std::max(inner.y, inner.bottom() - line_h))),
                          .w = 1,
                          .h = static_cast<int>(line_h),
                          .surrounding = text,
                          .cursor = static_cast<int>(e.cursor),
                          .anchor = static_cast<int>(e.anchor)};
        }
        pop_clip();
    }

} // namespace lyren::ui
