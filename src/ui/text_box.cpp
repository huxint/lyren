#include <cmath>

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

    void Ui::edit_input(Id id, EditBox &box, std::string &text, bool &submitted) {
        FrameInput &in = *in_;
        TextEdit &e = box.edit;
        bool touched = false;
        // Input method, in the protocol's order: delete around the cursor, insert the commit, then the new preedit.
        if (in.delete_before || in.delete_after) {
            size_t a = char_boundary(text, e.lo() - std::min<size_t>(e.lo(), in.delete_before));
            size_t b = char_boundary(text, e.hi() + in.delete_after);
            e.erase(text, a, b);
            touched = true;
        }
        if (!in.commit.empty()) {
            e.insert(text, in.commit), touched = true;
        }
        if (in.preedit) {
            box.preedit = *in.preedit;
            box.preedit_cursor = in.preedit_cursor;
            touched = true;
        }
        if (in.paste) {
            e.insert(text, *in.paste), touched = true;
        }
        in.delete_before = in.delete_after = 0;
        in.commit.clear();
        in.preedit.reset();
        in.paste.reset();

        for (auto k = in.keys.begin(); k != in.keys.end() && focus == id;) {
            std::string copied;
            auto r = e.apply(text, *k, copied);
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
            caret = pre_begin + std::min(size_t(std::max(box.preedit_cursor, 0)), box.preedit.size());
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
            text_input = {.enabled = true, .x = int(cx), .y = int(line_y), .w = 1, .h = int(line_h), .surrounding = text, .cursor = int(e.cursor), .anchor = int(e.anchor)};
        }
        pop_clip();

        if (has_clear) {
            float ch = anim(sub_id(id, 0xc1), clear.hovered ? 1 : 0, 120);
            p_->icon("circle-x", center(clear_r, 16, 16), t.text_dim.mix(t.text, ch));
        }
        return submitted;
    }

} // namespace lyren::ui
