#include "wl/display.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "core/log.hpp"
#include "core/loop.hpp"
#include "wl/clipboard.hpp"
#include "wl/input.hpp"

namespace lyren::wl {

    template <class T>
    static void bind(wl_registry *reg, uint32_t name, const wl_interface *iface, uint32_t have, uint32_t want, T *&out) {
        out = static_cast<T *>(wl_registry_bind(reg, name, iface, std::min(have, want)));
    }

    static void on_global(void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
        auto *d = static_cast<Display *>(data);
        auto is = [&](const wl_interface &i) {
            return std::string_view(iface) == i.name;
        };
        if (is(wl_compositor_interface)) {
            bind(reg, name, &wl_compositor_interface, ver, 6, d->compositor);
        } else if (is(xdg_wm_base_interface)) {
            bind(reg, name, &xdg_wm_base_interface, ver, 6, d->wm_base);
        } else if (is(wl_seat_interface) && !d->seat) {
            bind(reg, name, &wl_seat_interface, ver, 9, d->seat);
        } else if (is(wp_viewporter_interface)) {
            bind(reg, name, &wp_viewporter_interface, ver, 1, d->viewporter);
        } else if (is(wp_fractional_scale_manager_v1_interface)) {
            bind(reg, name, &wp_fractional_scale_manager_v1_interface, ver, 1, d->fractional_scale);
        } else if (is(wp_cursor_shape_manager_v1_interface)) {
            bind(reg, name, &wp_cursor_shape_manager_v1_interface, ver, 1, d->cursor_shape);
        } else if (is(zxdg_decoration_manager_v1_interface)) {
            bind(reg, name, &zxdg_decoration_manager_v1_interface, ver, 1, d->decoration);
        } else if (is(zwp_text_input_manager_v3_interface)) {
            bind(reg, name, &zwp_text_input_manager_v3_interface, ver, 1, d->text_input);
        } else if (is(xdg_activation_v1_interface)) {
            bind(reg, name, &xdg_activation_v1_interface, ver, 1, d->activation);
        } else if (is(zwlr_layer_shell_v1_interface)) {
            bind(reg, name, &zwlr_layer_shell_v1_interface, ver, 4, d->layer_shell);
        } else if (is(ext_background_effect_manager_v1_interface)) {
            bind(reg, name, &ext_background_effect_manager_v1_interface, ver, 1, d->background_effect);
        } else if (is(wl_data_device_manager_interface)) {
            bind(reg, name, &wl_data_device_manager_interface, ver, 3, d->data_device_manager);
        }
    }
    static void on_global_remove(void *, wl_registry *, uint32_t) {}
    static const wl_registry_listener registry_listener{on_global, on_global_remove};

    static void on_ping(void *, xdg_wm_base *wm, uint32_t serial) {
        xdg_wm_base_pong(wm, serial);
    }
    static const xdg_wm_base_listener wm_listener{on_ping};

    static void on_effect_caps(void *data, ext_background_effect_manager_v1 *, uint32_t flags) {
        static_cast<Display *>(data)->blur_supported = flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR;
    }
    static const ext_background_effect_manager_v1_listener effect_listener{on_effect_caps};

    Display::Display(Loop &l)
    : loop(l) {
        display = wl_display_connect(nullptr);
        if (!display) {
            log::error("cannot connect to a Wayland compositor");
            std::exit(1);
        }
        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registry_listener, this);
        wl_display_roundtrip(display);
        if (!compositor || !wm_base || !seat || !viewporter || !fractional_scale) {
            log::error("compositor lacks required globals (wl_compositor 6, xdg_wm_base, wl_seat, wp_viewporter, wp_fractional_scale)");
            std::exit(1);
        }
        xdg_wm_base_add_listener(wm_base, &wm_listener, this);
        if (background_effect) {
            ext_background_effect_manager_v1_add_listener(background_effect, &effect_listener, this);
        }
        input = std::make_unique<Input>(*this);
        clipboard = std::make_unique<Clipboard>(*this);
        wl_display_roundtrip(display);

        loop.set_poll_hook({
            .fd = wl_display_get_fd(display),
            .before =
                [this] {
                    while (wl_display_prepare_read(display) != 0) {
                        wl_display_dispatch_pending(display);
                    }
                    wl_display_flush(display);
                },
            .after =
                [this](bool readable) {
                    if (readable) {
                        wl_display_read_events(display);
                    } else {
                        wl_display_cancel_read(display);
                    }
                    if (wl_display_dispatch_pending(display) < 0) {
                        log::error("Wayland connection lost");
                        std::exit(1);
                    }
                },
        });
    }

    Display::~Display() {
        clipboard.reset();
        input.reset();
        wl_display_flush(display);
        wl_display_disconnect(display);
    }

} // namespace lyren::wl
