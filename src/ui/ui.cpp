#include "ui/ui.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>

namespace lyren::ui {

    constexpr uint64_t fnv_prime = 0x100000001b3ull;

    static bool take_escape(std::vector<wl::KeyEvent> &keys) {
        auto it = std::ranges::find_if(keys, [](const wl::KeyEvent &k) {
            return k.keysym == XKB_KEY_Escape;
        });
        if (it == keys.end()) {
            return false;
        }
        keys.erase(it);
        return true;
    }

    Id Ui::id(std::string_view s) const {
        Id h = ids_.back();
        for (unsigned char c : s) {
            h = (h ^ c) * fnv_prime;
        }
        return h;
    }

    Id Ui::id(uint64_t n) const {
        Id h = ids_.back() ^ 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < 8; ++i) {
            h = (h ^ ((n >> (i * 8)) & 0xff)) * fnv_prime;
        }
        return h;
    }

    void Ui::begin(Painter &painter, const Theme &theme, float w, float h, double now_s, FrameInput &input) {
        p_ = &painter, theme_ = &theme, in_ = &input;
        width = w, height = h, now = now_s;
        ++frame_;
        ids_.resize(1);
        clips_.assign(1, {0, 0, w, h});
        prev_layer_rects_.swap(layer_rects_);
        layer_rects_.clear();
        layer_ = Layer::base;
        cursor = wl::Cursor::arrow;
        text_input = {};
        animating_ = redraw_ = consumed_ = wheel_taken_ = false;
        wake_ = std::numeric_limits<double>::infinity();
        pressed_id_ = 0;
        had_press_ = std::ranges::any_of(in_->actions, [](const PointerAction &a) {
            return a.press && a.button == BTN_LEFT;
        });
        scroll_stack_.clear();

        // A press outside an open menu closes it; a left press stops there, a right press may open the next menu.
        if (!menu_.empty()) {
            bool close = take_escape(in_->keys);
            for (auto &a : in_->actions) {
                if (a.press && !close && layer_at(a.x, a.y) != Layer::popup) {
                    close = true;
                    a.consumed = a.button == BTN_LEFT;
                }
            }
            if (close) {
                menu_.clear();
                std::erase_if(prev_layer_rects_, [](auto &lr) {
                    return lr.first == Layer::popup;
                });
                consumed_ = true;
            }
        }
        pointer_layer_ = layer_at(in_->x, in_->y);
    }

    void Ui::end() {
        draw_modal();
        if (modal_open() && take_escape(in_->keys)) {
            close_modal();
        }
        draw_menu();
        draw_toasts();
        if (had_press_ && pressed_id_ != focus) {
            focus = 0;
        }
        if (!in_->left_down) {
            active_ = 0;
        }
        if (consumed_) {
            redraw_ = true;
        }
        std::erase_if(anims_, [this](auto &kv) {
            return kv.second.frame + 2 < frame_;
        });
        std::erase_if(springs_, [this](auto &kv) {
            return kv.second.frame + 2 < frame_;
        });
    }

    Layer Ui::layer_at(float x, float y) const {
        Layer best = Layer::base;
        for (auto &[l, r] : prev_layer_rects_) {
            if (l > best && r.contains(x, y)) {
                best = l;
            }
        }
        return best;
    }

    void Ui::push_clip(Rect r) {
        clips_.push_back(intersect(r, clip()));
        p_->push_clip(r);
    }

    void Ui::pop_clip() {
        clips_.pop_back();
        p_->pop_clip();
    }

    bool Ui::hovering(Rect r) const {
        return !disabled && active_ == 0 && pointing(r);
    }

    bool Ui::pointing(Rect r) const {
        return in_->inside && layer_ == pointer_layer_ && intersect(r, clip()).contains(in_->x, in_->y);
    }

    Interaction Ui::interact(Id id, Rect r) {
        Interaction it;
        if (disabled) {
            return it;
        }
        Rect hit = intersect(r, clip());
        it.hovered = in_->inside && layer_ == pointer_layer_ && (active_ == 0 || active_ == id) && hit.contains(in_->x, in_->y);
        it.px = in_->x, it.py = in_->y;
        for (auto &a : in_->actions) {
            if (a.consumed) {
                continue;
            }
            bool over = hit.contains(a.x, a.y) && layer_at(a.x, a.y) == layer_;
            if (a.press) {
                if (!over || (a.button != BTN_LEFT && a.button != BTN_RIGHT)) {
                    continue;
                }
                a.consumed = consumed_ = true;
                if (a.button == BTN_LEFT) {
                    it.pressed = true;
                    it.double_clicked |= a.double_click;
                    active_ = pressed_id_ = id;
                } else if (a.button == BTN_RIGHT) {
                    it.right_clicked = true;
                    it.px = a.x, it.py = a.y;
                }
            } else if (a.button == BTN_LEFT && active_ == id) {
                a.consumed = consumed_ = true;
                it.clicked |= over;
                active_ = 0;
            }
        }
        it.held = active_ == id && in_->left_down;
        return it;
    }

    float Ui::anim(Id id, float target, float ms, Bezier curve) {
        auto [it, fresh] = anims_.try_emplace(id, Anim{target, target, now, 0, curve, frame_});
        Anim &a = it->second;
        a.frame = frame_;
        auto value = [&] {
            float t = a.ms > 0 ? static_cast<float>((now - a.start) * 1000 / a.ms) : 1;
            return t >= 1 ? a.to : a.from + (a.to - a.from) * a.curve(t);
        };
        if (target != a.to) {
            a.from = value();
            a.to = target, a.start = now, a.ms = ms * theme_->motion, a.curve = curve;
        }
        float v = value();
        if (v != a.to) {
            animating_ = true;
        }
        return v;
    }

    float Ui::spring(Id id, float target, float response_ms, float bounce) {
        auto [it, fresh] = springs_.try_emplace(id, Following{{target}, target, now, frame_});
        Following &s = it->second;
        float response = response_ms * theme_->motion / 1000;
        if (response > 0) {
            s.motion.advance(s.target, static_cast<float>(now - s.time), response, bounce);
        }
        s.target = target, s.time = now, s.frame = frame_;
        if (response == 0 || (std::abs(s.motion.value - target) < 0.001f && std::abs(s.motion.velocity) < 0.01f)) {
            s.motion = {target};
        } else {
            animating_ = true;
        }
        return s.motion.value;
    }

    float Ui::progress(double since, float ms, Bezier curve) {
        ms *= theme_->motion;
        float t = ms > 0 ? static_cast<float>((now - since) * 1000 / ms) : 1;
        if (t >= 1) {
            return 1;
        }
        animating_ = true;
        return curve(std::max(t, 0.f));
    }

    // Scrolling.

    constexpr float bar_hit = 10;

    static Rect thumb_rect(Rect view, float offset, float content, float max) {
        float h = std::max(28.f, view.h * view.h / content);
        return {view.right() - bar_hit, view.y + (view.h - h) * (max > 0 ? offset / max : 0), bar_hit, h};
    }

    float Ui::begin_scroll(Id id, Rect view, float content) {
        Scroll &s = scrolls_[id];
        s.view = view, s.content = content;
        scroll_stack_.push_back(id);
        float max = max_offset(s);

        bool input = in_->scroll_y != 0 || in_->scroll_stop;
        if (input && !wheel_taken_ && hovering(view)) {
            wheel_taken_ = consumed_ = true;
            if (in_->wheel) {
                if (!s.smooth) {
                    s.target = s.offset, s.last_frame = now - 1 / 120.0;
                }
                s.target = std::clamp(s.target + in_->scroll_y, 0.f, max);
                s.smooth = true, s.coasting = false;
            } else if (in_->scroll_y != 0) {
                // Touchpad: follow the fingers and estimate their speed for the coast after they lift.
                double dt = now - s.last_input;
                float v = in_->scroll_y / static_cast<float>(std::clamp(dt, 1 / 240.0, 1 / 30.0));
                s.velocity = dt < 0.1 ? 0.4f * s.velocity + 0.6f * v : v;
                s.offset = s.target = std::clamp(s.offset + in_->scroll_y, 0.f, max);
                s.smooth = s.coasting = false;
                s.last_input = now;
            }
            if (in_->scroll_stop && !in_->wheel && !s.smooth) {
                s.coasting = now - s.last_input < 0.1 && std::abs(s.velocity) > 60;
                s.last_frame = now;
            }
            s.last_move = now;
        }
        step_scroll(s);

        if (content > view.h) {
            Id bar = sub_id(id, 0x5c);
            Interaction it = interact(bar, {view.right() - bar_hit, view.y, bar_hit, view.h});
            if (it.pressed) {
                Rect thumb = thumb_rect(view, s.offset, content, max);
                s.grab = thumb.contains(in_->x, in_->y) ? in_->y - thumb.y : thumb.h / 2;
            }
            if ((it.pressed || it.held) && s.grab >= 0) {
                float travel = view.h - thumb_rect(view, 0, content, max).h;
                float y = std::clamp(in_->y - s.grab - view.y, 0.f, travel);
                s.offset = s.target = travel > 0 ? y / travel * max : 0;
                s.smooth = s.coasting = false;
                s.last_move = now;
            }
            if (!it.held) {
                s.grab = -1;
            }
        }
        push_clip(view);
        return s.offset;
    }

    void Ui::step_scroll(Scroll &s) {
        float max = max_offset(s);
        double dt = now - s.last_frame;
        s.last_frame = now;
        if (s.smooth) {
            s.offset += (s.target - s.offset) * static_cast<float>(1 - std::exp(-dt / 0.08));
            if (std::abs(s.target - s.offset) < 0.5f) {
                s.offset = s.target, s.smooth = false;
            } else {
                animating_ = true;
            }
            s.last_move = now;
        } else if (s.coasting) {
            float decay = static_cast<float>(std::exp(-dt / 0.35));
            s.offset += s.velocity * 0.35f * (1 - decay);
            s.velocity *= decay;
            s.coasting = s.offset > 0 && s.offset < max && std::abs(s.velocity) > 15;
            if (s.coasting) {
                animating_ = true;
            }
            s.last_move = now;
        }
        s.offset = std::clamp(s.offset, 0.f, max);
        s.target = std::clamp(s.target, 0.f, max);
    }

    void Ui::end_scroll() {
        pop_clip();
        Id id = scroll_stack_.back();
        scroll_stack_.pop_back();
        Scroll &s = scrolls_[id];
        if (s.content <= s.view.h) {
            return;
        }
        const Theme &t = *theme_;
        Rect track{s.view.right() - bar_hit, s.view.y, bar_hit, s.view.h};
        bool dragging = is_active(sub_id(id, 0x5c));
        bool hot = hovering(track) || dragging;
        bool recent = now - s.last_move < 1.0;
        if (recent) {
            wake(s.last_move + 1.0);
        }
        float shown = anim(sub_id(id, 0x5d), hot || recent ? 1 : 0, hot || recent ? 100 : 400);
        if (shown <= 0.01f) {
            return;
        }
        float wide = anim(sub_id(id, 0x5e), hot ? 1 : 0, 120);
        float w = 4 + 3 * wide;
        Rect thumb = thumb_rect(s.view, s.offset, s.content, max_offset(s));
        p_->rect({thumb.right() - w - 2, thumb.y + 2, w, thumb.h - 4}, t.text.with_alpha((0.25f + 0.2f * wide) * shown), w / 2);
    }

    void Ui::scroll_to(Id id, float offset, bool smooth) {
        Scroll &s = scrolls_[id];
        s.target = std::clamp(offset, 0.f, max_offset(s));
        if (!smooth) {
            s.offset = s.target;
        } else if (!s.smooth) {
            s.last_frame = now;
        }
        s.smooth = smooth && theme_->motion > 0, s.coasting = false;
        redraw_ = true;
    }

    // Menu, modal and toasts.

    void Ui::open_menu(float x, float y, std::vector<MenuItem> items) {
        menu_ = std::move(items);
        menu_x_ = x, menu_y_ = y, menu_time_ = now;
        redraw_ = true;
    }

    void Ui::draw_menu() {
        if (menu_.empty()) {
            return;
        }
        const Theme &t = *theme_;
        TextStyle st{.size = t.fs(13), .color = t.text};
        constexpr float item_h = 32, sep_h = 9, pad = 5;
        float w = 0, h = pad * 2;
        for (auto &m : menu_) {
            w = std::max(w, p_->measure(m.label, st));
            h += m.action ? item_h : sep_h;
        }
        w = std::clamp(w + 64, 160.f, 320.f);
        float x = std::clamp(menu_x_, 4.f, width - w - 4);
        float y = menu_y_ + h > height - 4 ? std::max(4.f, menu_y_ - h) : menu_y_;
        Rect r{x, y, w, h};
        layer_ = Layer::popup;
        layer_rects_.push_back({Layer::popup, r});

        float in = progress(menu_time_, 180);
        p_->set_alpha(in);
        r.y += (menu_y_ > y ? 6 : -6) * (1 - in);
        p_->shadow({r.x, r.y + 4, r.w, r.h}, t.radius, 18, t.shadow);
        p_->rect(r, t.elevated, t.radius, t.border, 1);
        std::function<void()> chosen;
        float iy = r.y + pad;
        for (size_t i = 0; i < menu_.size(); ++i) {
            auto &m = menu_[i];
            if (!m.action) {
                p_->rect({r.x + 10, iy + 4, r.w - 20, 1}, t.border);
                iy += sep_h;
                continue;
            }
            Rect row{r.x + pad, iy, r.w - pad * 2, item_h};
            auto it = interact(id(0x6d656e75ull + i), row);
            if (it.hovered) {
                p_->rect(row, t.hover, t.radius_s());
            }
            Color fg = m.danger ? t.danger : t.text;
            if (!m.icon.empty()) {
                p_->icon(m.icon, {row.x + 10, row.y + 8, 16, 16}, m.danger ? t.danger : t.text_dim);
            }
            p_->text_in({row.x + 36, row.y, row.w - 44, row.h}, m.label, {.size = st.size, .color = fg});
            if (it.clicked) {
                chosen = m.action;
            }
            iy += item_h;
        }
        interact(id("menu"), r);
        p_->set_alpha(1);
        layer_ = Layer::base;
        if (chosen) {
            menu_.clear();
            chosen();
            redraw_ = true;
        }
    }

    void Ui::open_modal(float w, float h, std::function<void(Ui &, Rect)> body, bool bare) {
        modal_ = std::move(body);
        modal_bare_ = bare;
        modal_w_ = w, modal_h_ = h, modal_time_ = now;
        modal_closed_ = -1;
        focus = 0;
        redraw_ = true;
    }

    void Ui::draw_modal() {
        if (!modal_) {
            return;
        }
        const Theme &t = *theme_;
        Rect all{0, 0, width, height};
        float shown = progress(modal_time_, 280);
        if (modal_closed_ >= 0) {
            shown = std::min(shown, 1 - progress(modal_closed_, 160, ease::exit));
            if (shown <= 0) {
                modal_ = nullptr;
                modal_closed_ = -1;
                redraw_ = true;
                return;
            }
        }
        layer_ = Layer::modal;
        if (modal_closed_ < 0) {
            layer_rects_.push_back({Layer::modal, all});
        }
        p_->set_alpha(shown);
        p_->rect(all, {0, 0, 0, 0.5f}, window_radius);
        Rect panel = center(all, spring(id("modal-w"), modal_w_, 300), spring(id("modal-h"), modal_h_, 300));
        panel = panel.inset((1 - shown) * 10);
        panel.y += (1 - shown) * 16;
        auto body = modal_; // the body may replace the modal while it runs
        if (modal_bare_) {
            body(*this, panel);
        } else {
            p_->shadow({panel.x, panel.y + 8, panel.w, panel.h}, t.radius_l(), 48, t.shadow);
            p_->rect(panel, t.elevated, t.radius_l(), t.border, 1);
            if (icon_button(id("modal-close"), {panel.right() - 44, panel.y + 12, 32, 32}, "x", 18)) {
                close_modal();
            }
            body(*this, panel.inset(28, 24));
        }
        interact(id("modal-panel"), panel);
        if (interact(id("modal-backdrop"), all).clicked) {
            close_modal();
        }
        p_->set_alpha(1);
        layer_ = Layer::base;
    }

    void Ui::toast(std::string text, std::string icon, bool error) {
        toasts_.push_back({std::move(text), std::move(icon), error});
        if (toasts_.size() > 3) {
            toasts_.erase(toasts_.begin());
        }
        redraw_ = true;
    }

    void Ui::draw_toasts() {
        std::erase_if(toasts_, [this](const Toast &t) {
            return t.start >= 0 && now >= t.until;
        });
        const Theme &t = *theme_;
        TextStyle st{.size = t.fs(13), .color = t.text};
        float y = height - toast_bottom;
        for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
            Toast &ts = *it;
            if (ts.start < 0) {
                ts.start = now, ts.until = now + (ts.error ? 6 : 3);
            }
            constexpr double fade = 0.22;
            float in = progress(ts.start, 320);
            float out = 1;
            if (now < ts.until - fade) {
                wake(ts.until - fade);
            } else {
                out = 1 - progress(ts.until - fade, static_cast<float>(fade * 1000), ease::exit);
            }
            float tw = std::min(p_->measure(ts.text, st), width - 160);
            Rect r{(width - tw - 56) / 2, y - 40 + (1 - in) * 16, tw + 56, 40};
            p_->set_alpha(in * out);
            p_->shadow({r.x, r.y + 6, r.w, r.h}, t.radius, 24, t.shadow);
            p_->rect(r, t.elevated, t.radius, t.border, 1);
            p_->icon(ts.icon, {r.x + 16, r.y + 12, 16, 16}, ts.error ? t.danger : t.accent);
            p_->text_in({r.x + 40, r.y, tw + 1, r.h}, ts.text, st);
            y -= 48 * in * out;
        }
        p_->set_alpha(1);
    }

} // namespace lyren::ui
