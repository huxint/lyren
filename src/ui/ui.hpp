#pragma once
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ui/motion.hpp"
#include "ui/painter.hpp"
#include "ui/text_edit.hpp"
#include "ui/theme.hpp"
#include "wl/input.hpp"

namespace lyren::ui {

using Id = uint64_t;

// Id of a part of a widget (its hover animation, its scrollbar, ...).
inline Id sub_id(Id id, uint64_t part) {
    uint64_t z = id ^ (part * 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Layout by cutting rectangles: each cut removes a strip from r and returns it.
inline Rect cut_left(Rect& r, float w) {
    Rect s{r.x, r.y, std::min(w, r.w), r.h};
    r.x += s.w, r.w -= s.w;
    return s;
}
inline Rect cut_right(Rect& r, float w) {
    Rect s{r.right() - std::min(w, r.w), r.y, std::min(w, r.w), r.h};
    r.w -= s.w;
    return s;
}
inline Rect cut_top(Rect& r, float h) {
    Rect s{r.x, r.y, r.w, std::min(h, r.h)};
    r.y += s.h, r.h -= s.h;
    return s;
}
inline Rect cut_bottom(Rect& r, float h) {
    Rect s{r.x, r.bottom() - std::min(h, r.h), r.w, std::min(h, r.h)};
    r.h -= s.h;
    return s;
}
inline Rect center(Rect outer, float w, float h) { return {outer.x + (outer.w - w) / 2, outer.y + (outer.h - h) / 2, w, h}; }
inline Rect intersect(Rect a, Rect b) {
    float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
    return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
}

struct PointerAction {
    float x, y;
    uint32_t button;
    bool press;
    bool double_click = false;
    bool consumed = false;
};

// Everything that happened since the previous frame, in window geometry coordinates.
struct FrameInput {
    float x = -1, y = -1;
    bool inside = false;  // pointer over the window geometry
    bool left_down = false;
    uint32_t mods = 0;
    std::vector<PointerAction> actions;
    float scroll_x = 0, scroll_y = 0;
    bool wheel = false, scroll_stop = false;
    std::vector<wl::KeyEvent> keys;
    // Input method: the latest preedit replaces the previous one; commits accumulate.
    std::optional<std::string> preedit;
    int preedit_cursor = -1;
    std::string commit;
    uint32_t delete_before = 0, delete_after = 0;
    std::optional<std::string> paste;

    // Keeps pointer state, drops the events.
    void consumed() {
        actions.clear();
        scroll_x = scroll_y = 0;
        wheel = scroll_stop = false;
        keys.clear();
        preedit.reset();
        commit.clear();
        delete_before = delete_after = 0;
        paste.reset();
    }
};

// Popups sit above the modal so a menu can open inside it.
enum class Layer : uint8_t { base, modal, popup };
enum class ButtonKind { primary, secondary, ghost };
enum class Align { left, center, right };
enum class TabStyle { underline, pill };

struct SliderStyle {
    bool accent = true;  // accent fill, or a neutral one
    float wave = 0;      // amplitude (px) of the moving sine drawn over the filled part
    std::span<const std::pair<float, float>> marked;  // ranges (0..1) drawn lighter on the track, e.g. what is buffered
};

struct Interaction {
    bool hovered = false;
    bool pressed = false;  // left press landed on it this frame
    bool held = false;     // left button still down after pressing it
    bool clicked = false;  // released over it
    bool double_clicked = false;
    bool right_clicked = false;
    float px = 0, py = 0;  // pointer (or the right-click) position
};

struct MenuItem {
    std::string icon;
    std::string label;
    std::function<void()> action;  // empty = separator
    bool danger = false;
};

// Immediate-mode UI: widgets are functions called every frame; per-widget state lives in maps keyed by Id.
class Ui {
public:
    Painter& painter() { return *p_; }
    const Theme& theme() const { return *theme_; }
    double now = 0;
    float width = 0, height = 0;
    float toast_bottom = 24;  // distance of the toast stack from the bottom edge
    float window_radius = 0;  // corner radius of the window, for the modal backdrop

    void begin(Painter& painter, const Theme& theme, float w, float h, double now_s, FrameInput& input);
    // Draws the modal, popups and toasts and finishes the frame.
    void end();
    // After end(): another frame right away (input changed state), a frame paced by max_fps (animation), and
    // the time a frame is due anyway (caret blink, toast expiry).
    bool needs_redraw() const { return redraw_; }
    bool animating() const { return animating_; }
    double wake_at() const { return wake_; }

    Id id(std::string_view s) const;
    Id id(uint64_t n) const;
    void push_id(Id i) { ids_.push_back(i); }
    void pop_id() { ids_.pop_back(); }

    void push_clip(Rect r);
    void pop_clip();
    Rect clip() const { return clips_.back(); }

    // Hover and clicks for r. A press goes to the first widget that asks for it, so a nested child must call
    // interact before its parent; parents can draw their hover state with hovering() first.
    Interaction interact(Id id, Rect r);
    bool hovering(Rect r) const;
    bool is_active(Id id) const { return active_ == id; }
    FrameInput& input() { return *in_; }

    Id focus = 0;  // keyboard focus

    // Eased value that follows target over ms (scaled by theme motion). The first call returns target.
    float anim(Id id, float target, float ms = 160, Bezier curve = ease::standard);
    // Eased 0..1 for a transition that started at the given time.
    float progress(double since, float ms, Bezier curve = ease::emphasized);
    void keep_animating() { animating_ = true; }
    void redraw() { redraw_ = true; }
    void wake(double at) { wake_ = std::min(wake_, at); }

    // Results the platform layer applies after the frame. copy and want_paste stay set until the app takes them.
    wl::Cursor cursor = wl::Cursor::arrow;
    wl::TextInputState text_input;  // caret in window geometry coordinates
    std::optional<std::string> copy;
    bool want_paste = false;

    // Widgets.
    bool button(Id id, Rect r, std::string_view label, ButtonKind kind = ButtonKind::secondary, std::string_view icon = {});
    bool icon_button(Id id, Rect r, std::string_view icon, float icon_size = 18, bool active = false);
    // Returns true when Enter is pressed.
    bool text_box(Id id, Rect r, std::string& text, std::string_view placeholder, std::string_view icon = {});
    // Returns true while the user changes the value (0..1).
    bool slider(Id id, Rect r, float& value, SliderStyle style = {});
    bool tabs(Id id, Rect r, std::span<const std::string_view> labels, int& selected, TabStyle style = TabStyle::underline);
    bool toggle(Id id, Rect r, bool& on);
    void spinner(Rect r, Color color, float thickness = 2);
    void label(Rect r, std::string_view s, const TextStyle& style, Align align = Align::left);

    // Scrollable viewport: draw the content shifted up by the returned offset, then call end_scroll(), which
    // draws the scrollbar. Wheel scrolling is smoothed, touchpad scrolling keeps its momentum.
    float begin_scroll(Id id, Rect viewport, float content_height);
    void end_scroll();
    void scroll_to(Id id, float offset);

    void open_menu(float x, float y, std::vector<MenuItem> items);
    bool menu_open() const { return !menu_.empty(); }
    void open_modal(float w, float h, std::function<void(Ui&, Rect)> body);
    // A closing modal is still drawn while it fades out but no longer takes input.
    bool modal_open() const { return modal_ && modal_closed_ < 0; }
    // Changes the size of the open modal; the panel animates to it.
    void resize_modal(float w, float h) { modal_w_ = w, modal_h_ = h; }
    void close_modal() {
        if (modal_closed_ < 0) modal_closed_ = now, focus = 0, redraw_ = true;
    }
    void toast(std::string text, std::string icon = "info", bool error = false);

private:
    struct Anim {
        float from, to;
        double start;
        float ms;
        Bezier curve;
        uint64_t frame;
    };
    struct Scroll {
        Rect view;
        float offset = 0, target = 0, velocity = 0, content = 0;
        double last_frame = 0, last_input = -10, last_move = -10;
        bool smooth = false, coasting = false;
        float grab = -1;  // thumb drag: pointer offset inside the thumb
    };
    struct EditBox {
        TextEdit edit;
        float scroll_x = 0;
        double blink = 0;
        std::string preedit;
        int preedit_cursor = -1;
    };
    struct Toast {
        std::string text, icon;
        bool error;
        double start = -1, until = 0;
    };

    Layer layer_at(float x, float y) const;
    float max_offset(const Scroll& s) const { return std::max(0.f, s.content - s.view.h); }
    void step_scroll(Scroll& s);
    void edit_input(Id id, EditBox& box, std::string& text, bool& submitted);
    void draw_menu();
    void draw_modal();
    void draw_toasts();

    Painter* p_ = nullptr;
    const Theme* theme_ = nullptr;
    FrameInput* in_ = nullptr;
    std::vector<Id> ids_{0xcbf29ce484222325ull};
    std::vector<Rect> clips_;
    Layer layer_ = Layer::base, pointer_layer_ = Layer::base;
    std::vector<std::pair<Layer, Rect>> layer_rects_, prev_layer_rects_;
    Id active_ = 0, pressed_id_ = 0;
    bool had_press_ = false, consumed_ = false;
    uint64_t frame_ = 0;
    bool animating_ = false, redraw_ = false;
    double wake_ = std::numeric_limits<double>::infinity();
    bool wheel_taken_ = false;
    std::unordered_map<Id, Anim> anims_;
    std::unordered_map<Id, Scroll> scrolls_;
    std::vector<Id> scroll_stack_;
    std::unordered_map<Id, EditBox> edits_;
    std::vector<MenuItem> menu_;
    float menu_x_ = 0, menu_y_ = 0;
    double menu_time_ = 0;
    std::function<void(Ui&, Rect)> modal_;
    float modal_w_ = 0, modal_h_ = 0;
    double modal_time_ = 0, modal_closed_ = -1;
    std::vector<Toast> toasts_;
};

}  // namespace lyren::ui
