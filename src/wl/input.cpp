#include "wl/input.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include "core/loop.hpp"
#include "wl/display.hpp"

namespace lyren::wl {

static Input* self(void* data) { return static_cast<Input*>(data); }
static float fx(wl_fixed_t v) { return float(wl_fixed_to_double(v)); }

static const wl_pointer_listener pointer_listener{
    .enter = [](void* d, wl_pointer*, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y) { self(d)->pointer_enter(serial, s, fx(x), fx(y)); },
    .leave = [](void* d, wl_pointer*, uint32_t, wl_surface* s) { self(d)->pointer_leave(s); },
    .motion = [](void* d, wl_pointer*, uint32_t time, wl_fixed_t x, wl_fixed_t y) { self(d)->pointer_motion(time, fx(x), fx(y)); },
    .button = [](void* d, wl_pointer*, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
        self(d)->pointer_button(serial, time, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
    },
    .axis = [](void* d, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t v) { self(d)->pointer_axis(axis, fx(v)); },
    .frame = [](void* d, wl_pointer*) { self(d)->pointer_frame(); },
    .axis_source = [](void* d, wl_pointer*, uint32_t source) { self(d)->pointer_axis_source(source); },
    .axis_stop = [](void* d, wl_pointer*, uint32_t, uint32_t) { self(d)->pointer_axis_stop(); },
    .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
    .axis_value120 = [](void* d, wl_pointer*, uint32_t axis, int32_t v) { self(d)->pointer_axis120(axis, v); },
    .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
};

static const wl_keyboard_listener keyboard_listener{
    .keymap = [](void* d, wl_keyboard*, uint32_t, int32_t fd, uint32_t size) { self(d)->keymap(fd, size); },
    .enter = [](void* d, wl_keyboard*, uint32_t serial, wl_surface* s, wl_array*) { self(d)->key_enter(s, serial); },
    .leave = [](void* d, wl_keyboard*, uint32_t, wl_surface*) { self(d)->key_leave(); },
    .key = [](void* d, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
        self(d)->key(serial, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
    },
    .modifiers = [](void* d, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
        self(d)->modifiers(dep, lat, lock, group);
    },
    .repeat_info = [](void* d, wl_keyboard*, int32_t rate, int32_t delay) { self(d)->repeat_info(rate, delay); },
};

static const zwp_text_input_v3_listener text_input_listener{
    .enter = [](void* d, zwp_text_input_v3*, wl_surface* s) { self(d)->text_input_enter(s); },
    .leave = [](void* d, zwp_text_input_v3*, wl_surface*) { self(d)->text_input_leave(); },
    .preedit_string =
        [](void* d, zwp_text_input_v3*, const char* text, int32_t begin, int32_t) {
            self(d)->pending_text_.preedit = text ? text : "";
            self(d)->pending_text_.preedit_cursor = begin;
        },
    .commit_string = [](void* d, zwp_text_input_v3*, const char* text) { self(d)->pending_text_.commit = text ? text : ""; },
    .delete_surrounding_text =
        [](void* d, zwp_text_input_v3*, uint32_t before, uint32_t after) {
            self(d)->pending_text_.delete_before = before;
            self(d)->pending_text_.delete_after = after;
        },
    .done = [](void* d, zwp_text_input_v3*, uint32_t) { self(d)->text_input_done(); },
};

static const wl_seat_listener seat_listener{
    .capabilities = [](void* d, wl_seat*, uint32_t caps) { self(d)->capabilities(caps); },
    .name = [](void*, wl_seat*, const char*) {},
};

static SurfaceSink* sink(wl_surface* s) { return s ? static_cast<SurfaceSink*>(wl_surface_get_user_data(s)) : nullptr; }

Input::Input(Display& display) : display_(display), xkb_(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
    wl_seat_add_listener(display_.seat, &seat_listener, this);
    if (display_.text_input) {
        text_input_ = zwp_text_input_manager_v3_get_text_input(display_.text_input, display_.seat);
        zwp_text_input_v3_add_listener(text_input_, &text_input_listener, this);
    }
}

Input::~Input() {
    stop_repeat();
    if (text_input_) zwp_text_input_v3_destroy(text_input_);
    if (cursor_device_) wp_cursor_shape_device_v1_destroy(cursor_device_);
    if (pointer_) wl_pointer_release(pointer_);
    if (keyboard_) wl_keyboard_release(keyboard_);
    xkb_state_unref(state_);
    xkb_keymap_unref(keymap_);
    xkb_context_unref(xkb_);
}

void Input::capabilities(uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer_) {
        pointer_ = wl_seat_get_pointer(display_.seat);
        wl_pointer_add_listener(pointer_, &pointer_listener, this);
        if (display_.cursor_shape) cursor_device_ = wp_cursor_shape_manager_v1_get_pointer(display_.cursor_shape, pointer_);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard_) {
        keyboard_ = wl_seat_get_keyboard(display_.seat);
        wl_keyboard_add_listener(keyboard_, &keyboard_listener, this);
    }
}

void Input::set_text_input(TextInputState s) {
    ti_want_ = s.enabled ? std::move(s) : TextInputState{};
    flush_text_input();
}

void Input::flush_text_input() {
    if (!text_input_ || !text_focus_ || ti_want_ == ti_sent_) return;
    if (ti_want_.enabled) {
        if (!ti_sent_.enabled) {
            zwp_text_input_v3_enable(text_input_);
            zwp_text_input_v3_set_content_type(text_input_, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        }
        // The protocol caps surrounding text below 4000 bytes.
        if (ti_want_.surrounding.size() < 4000)
            zwp_text_input_v3_set_surrounding_text(text_input_, ti_want_.surrounding.c_str(), ti_want_.cursor, ti_want_.anchor);
        zwp_text_input_v3_set_cursor_rectangle(text_input_, ti_want_.x, ti_want_.y, ti_want_.w, ti_want_.h);
    } else {
        zwp_text_input_v3_disable(text_input_);
    }
    zwp_text_input_v3_commit(text_input_);
    ti_sent_ = ti_want_;
}

void Input::text_input_enter(wl_surface* s) {
    text_focus_ = sink(s);
    ti_sent_ = {};
    flush_text_input();
}

void Input::text_input_leave() {
    text_focus_ = nullptr;
    ti_sent_ = {};
}

void Input::text_input_done() {
    TextInputEvent e = std::move(pending_text_);
    pending_text_ = {};
    if (text_focus_) text_focus_->on_text_input(e);
}

void Input::set_cursor(Cursor c) {
    if (c == cursor_) return;
    cursor_ = c;
    if (cursor_device_ && pointer_focus_) wp_cursor_shape_device_v1_set_shape(cursor_device_, enter_serial_, uint32_t(c));
}

void Input::pointer_enter(uint32_t serial, wl_surface* s, float x, float y) {
    pointer_focus_ = sink(s);
    enter_serial_ = serial;
    px_ = x, py_ = y;
    if (cursor_device_) wp_cursor_shape_device_v1_set_shape(cursor_device_, serial, uint32_t(cursor_));
    if (pointer_focus_) pointer_focus_->on_pointer({.type = PointerEvent::enter, .x = x, .y = y, .serial = serial});
}

void Input::pointer_leave(wl_surface*) {
    if (pointer_focus_) pointer_focus_->on_pointer({.type = PointerEvent::leave, .x = px_, .y = py_});
    pointer_focus_ = nullptr;
}

void Input::pointer_motion(uint32_t time, float x, float y) {
    px_ = x, py_ = y;
    if (pointer_focus_) pointer_focus_->on_pointer({.type = PointerEvent::motion, .x = x, .y = y, .time = time});
}

void Input::pointer_button(uint32_t serial, uint32_t time, uint32_t button, bool pressed) {
    serial_ = serial;
    if (pointer_focus_)
        pointer_focus_->on_pointer(
            {.type = pressed ? PointerEvent::press : PointerEvent::release, .x = px_, .y = py_, .button = button, .serial = serial, .time = time});
}

void Input::pointer_axis(uint32_t axis, float value) {
    axis_[axis] += value;
    has_axis_ = true;
}

void Input::pointer_axis120(uint32_t axis, int v120) {
    axis120_[axis] += v120;
    has_axis_ = true;
}

void Input::pointer_frame() {
    if (pointer_focus_ && (has_axis_ || axis_stop_)) {
        PointerEvent e{.type = PointerEvent::scroll, .x = px_, .y = py_, .stop = axis_stop_};
        // Wheel notches become a fixed distance; touchpad motion is already in surface pixels.
        constexpr float notch_px = 48;
        e.wheel = axis_source_ == WL_POINTER_AXIS_SOURCE_WHEEL && (axis120_[0] || axis120_[1]);
        e.dx = e.wheel ? axis120_[1] / 120.f * notch_px : axis_[1];
        e.dy = e.wheel ? axis120_[0] / 120.f * notch_px : axis_[0];
        pointer_focus_->on_pointer(e);
    }
    axis_[0] = axis_[1] = 0;
    axis120_[0] = axis120_[1] = 0;
    axis_source_ = 0;
    axis_stop_ = has_axis_ = false;
}

void Input::keymap(int fd, uint32_t size) {
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    xkb_keymap* km = xkb_keymap_new_from_string(xkb_, static_cast<const char*>(map), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    xkb_state_unref(state_);
    xkb_keymap_unref(keymap_);
    keymap_ = km;
    state_ = xkb_state_new(km);
}

void Input::key_enter(wl_surface* s, uint32_t serial) {
    key_focus_ = sink(s);
    serial_ = serial;
    if (key_focus_) key_focus_->on_keyboard_focus(true);
}

void Input::key_leave() {
    stop_repeat();
    if (key_focus_) key_focus_->on_keyboard_focus(false);
    key_focus_ = nullptr;
}

void Input::key(uint32_t serial, uint32_t key, bool pressed) {
    serial_ = serial;
    if (!state_) return;
    emit_key(key, pressed, false);
    if (pressed && xkb_keymap_key_repeats(keymap_, key + 8) && repeat_rate_ > 0) {
        stop_repeat();
        repeat_key_ = key;
        repeat_timer_ = display_.loop.after(std::chrono::milliseconds(repeat_delay_), [this] { repeat_tick(); });
    } else if (!pressed && key == repeat_key_) {
        stop_repeat();
    }
}

void Input::emit_key(uint32_t key, bool pressed, bool repeat) {
    if (!key_focus_) return;
    KeyEvent e{.keysym = xkb_state_key_get_one_sym(state_, key + 8), .mods = mods_, .pressed = pressed, .repeat = repeat, .serial = serial_};
    if (pressed) {
        char buf[32];
        int n = xkb_state_key_get_utf8(state_, key + 8, buf, sizeof buf);
        if (n > 0 && !(n == 1 && (static_cast<unsigned char>(buf[0]) < 0x20 || buf[0] == 0x7f))) e.text.assign(buf, size_t(n));
    }
    key_focus_->on_key(e);
}

void Input::repeat_tick() {
    emit_key(repeat_key_, true, true);
    repeat_timer_ = display_.loop.after(std::chrono::milliseconds(1000 / repeat_rate_), [this] { repeat_tick(); });
}

void Input::stop_repeat() {
    if (repeat_timer_) display_.loop.cancel(repeat_timer_);
    repeat_timer_ = 0;
    repeat_key_ = 0;
}

void Input::modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (!state_) return;
    xkb_state_update_mask(state_, depressed, latched, locked, 0, 0, group);
    auto on = [&](const char* name) { return xkb_state_mod_name_is_active(state_, name, XKB_STATE_MODS_EFFECTIVE) > 0; };
    mods_ = (on(XKB_MOD_NAME_SHIFT) ? mod_shift : 0) | (on(XKB_MOD_NAME_CTRL) ? mod_ctrl : 0) | (on(XKB_MOD_NAME_ALT) ? mod_alt : 0) |
            (on(XKB_MOD_NAME_LOGO) ? mod_super : 0);
}

}  // namespace lyren::wl
