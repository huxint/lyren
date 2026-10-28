#include "wl/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <span>

#include "wl/display.hpp"

namespace lyren::wl {

    static Window *self(void *data) {
        return static_cast<Window *>(data);
    }

    static const xdg_surface_listener xdg_surface_listener_{
        .configure =
            [](void *d, xdg_surface *, uint32_t serial) {
                self(d)->surface_configure(serial);
            },
    };
    static const xdg_toplevel_listener toplevel_listener{
        .configure =
            [](void *d, xdg_toplevel *, int32_t w, int32_t h, wl_array *states) {
                self(d)->toplevel_configure(w, h, states);
            },
        .close =
            [](void *d, xdg_toplevel *) {
                self(d)->close_requested();
            },
        .configure_bounds =
            [](void *d, xdg_toplevel *, int32_t w, int32_t h) {
                self(d)->configure_bounds(w, h);
            },
        .wm_capabilities = [](void *, xdg_toplevel *, wl_array *) {},
    };
    static const zxdg_toplevel_decoration_v1_listener decoration_listener{
        .configure =
            [](void *d, zxdg_toplevel_decoration_v1 *, uint32_t mode) {
                self(d)->decoration_configure(mode);
            },
    };
    static const wp_fractional_scale_v1_listener fractional_listener{
        .preferred_scale =
            [](void *d, wp_fractional_scale_v1 *, uint32_t scale) {
                self(d)->preferred_scale(scale);
            },
    };
    static const wl_callback_listener frame_listener{
        .done =
            [](void *d, wl_callback *cb, uint32_t) {
                wl_callback_destroy(cb);
                self(d)->frame_done();
            },
    };

    Window::Window(Display &display, Callbacks cb, bool client_side, int width, int height)
    : display_(display),
      cb_(std::move(cb)),
      width_(width),
      height_(height),
      csd_(client_side),
      want_csd_(client_side) {
        surface_ = wl_compositor_create_surface(display_.compositor);
        wl_surface_set_user_data(surface_, static_cast<SurfaceSink *>(this));
        viewport_ = wp_viewporter_get_viewport(display_.viewporter, surface_);
        fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(display_.fractional_scale, surface_);
        wp_fractional_scale_v1_add_listener(fractional_, &fractional_listener, this);

        xdg_surface_ = xdg_wm_base_get_xdg_surface(display_.wm_base, surface_);
        xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener_, this);
        toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
        xdg_toplevel_add_listener(toplevel_, &toplevel_listener, this);
        xdg_toplevel_set_app_id(toplevel_, "lyren");
        xdg_toplevel_set_min_size(toplevel_, 720, 480);
        if (display_.decoration) {
            decoration_ = zxdg_decoration_manager_v1_get_toplevel_decoration(display_.decoration, toplevel_);
            zxdg_toplevel_decoration_v1_add_listener(decoration_, &decoration_listener, this);
        }
        set_decorations(client_side);
        wl_surface_commit(surface_);

        // Launchers hand over focus through this variable.
        if (const char *token = std::getenv("XDG_ACTIVATION_TOKEN"); token && display_.activation) {
            xdg_activation_v1_activate(display_.activation, token, surface_);
            unsetenv("XDG_ACTIVATION_TOKEN");
        }
        while (!needs_ack_) {
            wl_display_dispatch(display_.display);
        }
    }

    Window::~Window() {
        if (effect_) {
            ext_background_effect_surface_v1_destroy(effect_);
        }
        if (decoration_) {
            zxdg_toplevel_decoration_v1_destroy(decoration_);
        }
        wp_fractional_scale_v1_destroy(fractional_);
        wp_viewport_destroy(viewport_);
        xdg_toplevel_destroy(toplevel_);
        xdg_surface_destroy(xdg_surface_);
        wl_surface_destroy(surface_);
    }

    void Window::set_title(const std::string &title) {
        xdg_toplevel_set_title(toplevel_, title.c_str());
    }

    void Window::set_decorations(bool client_side) {
        want_csd_ = client_side;
        if (decoration_) {
            zxdg_toplevel_decoration_v1_set_mode(decoration_, client_side ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        } else {
            csd_ = true;
        }
    }

    void Window::set_shadow(int margin, int resize_border, int corner_radius) {
        shadow_ = margin, resize_border_ = resize_border, radius_ = corner_radius;
        update_margin();
    }

    void Window::set_blur(bool on) {
        if (!display_.blur_supported || on == blur_) {
            return;
        }
        blur_ = on;
        if (on && !effect_) {
            effect_ = ext_background_effect_manager_v1_get_background_effect(display_.background_effect, surface_);
        }
        geometry_dirty_ = true;
    }

    void Window::activate(const std::string &token) {
        if (!display_.activation) {
            return;
        }
        if (!token.empty()) {
            return xdg_activation_v1_activate(display_.activation, token.c_str(), surface_);
        }
        static const xdg_activation_token_v1_listener listener{
            .done =
                [](void *d, xdg_activation_token_v1 *t, const char *token) {
                    Window *w = self(d);
                    xdg_activation_v1_activate(w->display_.activation, token, w->surface_);
                    xdg_activation_token_v1_destroy(t);
                },
        };
        xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(display_.activation);
        xdg_activation_token_v1_add_listener(t, &listener, this);
        xdg_activation_token_v1_set_serial(t, display_.input->serial(), display_.seat);
        xdg_activation_token_v1_set_surface(t, surface_);
        xdg_activation_token_v1_commit(t);
    }

    void Window::move() {
        xdg_toplevel_move(toplevel_, display_.seat, display_.input->serial());
    }
    void Window::resize(uint32_t edges) {
        xdg_toplevel_resize(toplevel_, display_.seat, display_.input->serial(), edges);
    }
    void Window::show_menu(int x, int y) {
        xdg_toplevel_show_window_menu(toplevel_, display_.seat, display_.input->serial(), x - margin_, y - margin_);
    }
    void Window::minimize() {
        xdg_toplevel_set_minimized(toplevel_);
    }
    void Window::toggle_maximize() {
        if (maximized_) {
            xdg_toplevel_unset_maximized(toplevel_);
        } else {
            xdg_toplevel_set_maximized(toplevel_);
        }
    }

    void Window::toplevel_configure(int w, int h, wl_array *states) {
        pending_w_ = w, pending_h_ = h;
        pending_max_ = pending_full_ = pending_act_ = pending_tiled_ = false;
        for (uint32_t state : std::span{static_cast<const uint32_t *>(states->data), states->size / sizeof(uint32_t)}) {
            switch (state) {
                case XDG_TOPLEVEL_STATE_MAXIMIZED:
                    pending_max_ = true;
                    break;
                case XDG_TOPLEVEL_STATE_FULLSCREEN:
                    pending_full_ = true;
                    break;
                case XDG_TOPLEVEL_STATE_ACTIVATED:
                    pending_act_ = true;
                    break;
                case XDG_TOPLEVEL_STATE_TILED_LEFT:
                case XDG_TOPLEVEL_STATE_TILED_RIGHT:
                case XDG_TOPLEVEL_STATE_TILED_TOP:
                case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
                    pending_tiled_ = true;
                    break;
            }
        }
    }

    // The work area; only the initial size has to fit inside it.
    void Window::configure_bounds(int w, int h) {
        if (!configured_ && w > 0 && h > 0) {
            width_ = std::min(width_, w), height_ = std::min(height_, h);
        }
    }

    void Window::decoration_configure(uint32_t mode) {
        csd_ = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    }

    void Window::surface_configure(uint32_t serial) {
        if (pending_w_ > 0 && pending_h_ > 0) {
            width_ = pending_w_, height_ = pending_h_;
        }
        maximized_ = pending_max_, fullscreen_ = pending_full_, activated_ = pending_act_, tiled_ = pending_tiled_;
        ack_serial_ = serial;
        needs_ack_ = configured_ = true;
        update_margin();
        geometry_dirty_ = true;
        if (cb_.configure) {
            cb_.configure();
        }
    }

    void Window::preferred_scale(uint32_t scale120) {
        if (scale120 == scale120_) {
            return;
        }
        scale120_ = scale120;
        geometry_dirty_ = true;
        if (cb_.configure) {
            cb_.configure();
        }
    }

    void Window::update_margin() {
        int m = csd_ && !maximized_ && !fullscreen_ && !tiled_ ? shadow_ : 0;
        if (m != margin_) {
            margin_ = m, geometry_dirty_ = true;
        }
    }

    void Window::frame_done() {
        frame_pending_ = false;
        if (cb_.frame) {
            cb_.frame();
        }
    }

    void Window::prepare_present() {
        if (needs_ack_) {
            xdg_surface_ack_configure(xdg_surface_, ack_serial_);
            needs_ack_ = false;
        }
        if (geometry_dirty_) {
            geometry_dirty_ = false;
            xdg_surface_set_window_geometry(xdg_surface_, margin_, margin_, width_, height_);
            wp_viewport_set_destination(viewport_, surface_width(), surface_height());

            wl_region *input = wl_compositor_create_region(display_.compositor);
            int b = margin_ ? resize_border_ : 0;
            wl_region_add(input, margin_ - b, margin_ - b, width_ + 2 * b, height_ + 2 * b);
            wl_surface_set_input_region(surface_, input);
            wl_region_destroy(input);

            if (effect_) {
                wl_region *blur = nullptr;
                if (blur_) {
                    // Rounded corners as one-pixel rows; corners vanish when the margin does.
                    blur = wl_compositor_create_region(display_.compositor);
                    int r = margin_ ? radius_ : 0;
                    wl_region_add(blur, margin_, margin_ + r, width_, height_ - 2 * r);
                    for (int k = 0; k < r; ++k) {
                        double dy = r - k - 0.5;
                        int inset = static_cast<int>(std::lround(r - std::sqrt(static_cast<double>(r) * r - dy * dy)));
                        wl_region_add(blur, margin_ + inset, margin_ + k, width_ - 2 * inset, 1);
                        wl_region_add(blur, margin_ + inset, margin_ + height_ - 1 - k, width_ - 2 * inset, 1);
                    }
                }
                ext_background_effect_surface_v1_set_blur_region(effect_, blur);
                if (blur) {
                    wl_region_destroy(blur);
                }
            }
        }
        wl_callback_add_listener(wl_surface_frame(surface_), &frame_listener, this);
        frame_pending_ = true;
    }

} // namespace lyren::wl
