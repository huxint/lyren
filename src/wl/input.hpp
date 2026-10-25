#pragma once
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <string>

#include "wl/protocols.hpp"

namespace lyren::wl {

class Display;

enum class Cursor : uint32_t {
    arrow = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
    pointer = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
    text = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
    grabbing = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING,
    n_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,
    s_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
    e_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
    w_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
    ne_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE,
    nw_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
    se_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
    sw_resize = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
};

constexpr uint32_t mod_shift = 1, mod_ctrl = 2, mod_alt = 4, mod_super = 8;

struct PointerEvent {
    enum Type { enter, leave, motion, press, release, scroll } type;
    float x = 0, y = 0;  // surface-local logical coordinates
    uint32_t button = 0;
    uint32_t serial = 0;
    uint32_t time = 0;
    float dx = 0, dy = 0;  // scroll distance in logical pixels
    bool wheel = false;    // mouse wheel notches rather than touchpad motion
    bool stop = false;     // touchpad fingers lifted
};

struct KeyEvent {
    uint32_t keysym = 0;
    std::string text;  // UTF-8 the key produces; empty for control keys
    uint32_t mods = 0;
    bool pressed = false;
    bool repeat = false;
    uint32_t serial = 0;
};

// Implemented by the owner of a wl_surface; found through the surface's user data.
class SurfaceSink {
public:
    virtual ~SurfaceSink() = default;
    virtual void on_pointer(const PointerEvent&) = 0;
    virtual void on_key(const KeyEvent&) {}
    virtual void on_keyboard_focus(bool) {}
};

class Input {
public:
    explicit Input(Display& display);
    ~Input();

    void set_cursor(Cursor c);
    // Latest input serial, for move/resize/menu requests.
    uint32_t serial() const { return serial_; }
    uint32_t mods() const { return mods_; }

    // Listener plumbing (called from the C callbacks).
    void pointer_enter(uint32_t serial, wl_surface* s, float x, float y);
    void pointer_leave(wl_surface* s);
    void pointer_motion(uint32_t time, float x, float y);
    void pointer_button(uint32_t serial, uint32_t time, uint32_t button, bool pressed);
    void pointer_axis(uint32_t axis, float value);
    void pointer_axis120(uint32_t axis, int v120);
    void pointer_axis_source(uint32_t source) { axis_source_ = source; }
    void pointer_axis_stop() { axis_stop_ = true; }
    void pointer_frame();
    void keymap(int fd, uint32_t size);
    void key_enter(wl_surface* s, uint32_t serial);
    void key_leave();
    void key(uint32_t serial, uint32_t key, bool pressed);
    void modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    void repeat_info(int rate, int delay) {
        repeat_rate_ = rate;
        repeat_delay_ = delay;
    }
    void capabilities(uint32_t caps);

private:
    void emit_key(uint32_t key, bool pressed, bool repeat);
    void repeat_tick();
    void stop_repeat();

    Display& display_;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wp_cursor_shape_device_v1* cursor_device_ = nullptr;
    SurfaceSink* pointer_focus_ = nullptr;
    SurfaceSink* key_focus_ = nullptr;
    uint32_t enter_serial_ = 0, serial_ = 0;
    Cursor cursor_ = Cursor::arrow;
    float px_ = 0, py_ = 0;
    float axis_[2]{};
    int axis120_[2]{};
    uint32_t axis_source_ = 0;
    bool axis_stop_ = false, has_axis_ = false;

    xkb_context* xkb_ = nullptr;
    xkb_keymap* keymap_ = nullptr;
    xkb_state* state_ = nullptr;
    uint32_t mods_ = 0;
    int repeat_rate_ = 25, repeat_delay_ = 600;
    uint32_t repeat_key_ = 0;
    uint64_t repeat_timer_ = 0;
};

}  // namespace lyren::wl
