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

// One text-input-v3 "done" batch from the input method.
struct TextInputEvent {
    std::string preedit;
    int preedit_cursor = -1;  // byte offset in preedit, -1 hides the cursor
    std::string commit;
    uint32_t delete_before = 0, delete_after = 0;
};

// What the focused text field tells the input method. The caret is surface-local; cursor and anchor are byte
// offsets into surrounding.
struct TextInputState {
    bool enabled = false;
    int x = 0, y = 0, w = 0, h = 0;
    std::string surrounding;
    int cursor = 0, anchor = 0;
    bool operator==(const TextInputState&) const = default;
};

// Implemented by the owner of a wl_surface; found through the surface's user data.
class SurfaceSink {
public:
    virtual ~SurfaceSink() = default;
    virtual void on_pointer(const PointerEvent&) = 0;
    virtual void on_key(const KeyEvent&) {}
    virtual void on_keyboard_focus(bool) {}
    virtual void on_text_input(const TextInputEvent&) {}
};

class Input {
public:
    explicit Input(Display& display);
    ~Input();

    void set_cursor(Cursor c);
    // Only real changes are committed: the compositor answers every commit with a done event.
    void set_text_input(TextInputState s);
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
    void text_input_enter(wl_surface* s);
    void text_input_leave();
    void text_input_done();
    TextInputEvent pending_text_;

private:
    void emit_key(uint32_t key, bool pressed, bool repeat);
    void repeat_tick();
    void stop_repeat();
    void flush_text_input();

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

    zwp_text_input_v3* text_input_ = nullptr;
    SurfaceSink* text_focus_ = nullptr;
    TextInputState ti_want_, ti_sent_;

    xkb_context* xkb_ = nullptr;
    xkb_keymap* keymap_ = nullptr;
    xkb_state* state_ = nullptr;
    uint32_t mods_ = 0;
    int repeat_rate_ = 25, repeat_delay_ = 600;
    uint32_t repeat_key_ = 0;
    uint64_t repeat_timer_ = 0;
};

}  // namespace lyren::wl
