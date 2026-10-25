#pragma once
#include <memory>

#include "wl/protocols.hpp"

namespace lyren {
class Loop;
}

namespace lyren::wl {

class Input;
class Clipboard;

// Connection, globals and the seat. Integrates the display fd into the loop.
class Display {
public:
    explicit Display(Loop& loop);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    Loop& loop;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractional_scale = nullptr;
    wp_cursor_shape_manager_v1* cursor_shape = nullptr;
    zxdg_decoration_manager_v1* decoration = nullptr;
    zwp_text_input_manager_v3* text_input = nullptr;
    xdg_activation_v1* activation = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    ext_background_effect_manager_v1* background_effect = nullptr;
    wl_data_device_manager* data_device_manager = nullptr;
    bool blur_supported = false;

    std::unique_ptr<Input> input;
    std::unique_ptr<Clipboard> clipboard;
};

}  // namespace lyren::wl
