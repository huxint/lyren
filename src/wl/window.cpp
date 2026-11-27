#include "wl/window.hpp"

#include <algorithm>
#include <cstdlib>
#include <span>

#include "core/log.hpp"
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

    Window::Window(Display &display, Callbacks cb, int width, int height)
    : Surface(display),
      cb_(std::move(cb)),
      width_(width),
      height_(height) {
        xdg_surface_ = xdg_wm_base_get_xdg_surface(display_.wm_base, surface_);
        xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener_, this);
        toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
        xdg_toplevel_add_listener(toplevel_, &toplevel_listener, this);
        xdg_toplevel_set_app_id(toplevel_, "lyren");
        xdg_toplevel_set_min_size(toplevel_, 720, 480);
        if (display_.decoration) {
            decoration_ = zxdg_decoration_manager_v1_get_toplevel_decoration(display_.decoration, toplevel_);
            zxdg_toplevel_decoration_v1_add_listener(decoration_, &decoration_listener, this);
            zxdg_toplevel_decoration_v1_set_mode(decoration_, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        }
        wl_surface_commit(surface_);

        // Launchers hand over focus through this variable.
        if (const char *token = std::getenv("XDG_ACTIVATION_TOKEN"); token && display_.activation) {
            xdg_activation_v1_activate(display_.activation, token, surface_);
            unsetenv("XDG_ACTIVATION_TOKEN");
        }
        while (!needs_ack_) {
            if (wl_display_dispatch(display_.display) < 0) {
                log::error("Wayland connection lost");
                std::exit(1);
            }
        }
    }

    Window::~Window() {
        if (decoration_) {
            zxdg_toplevel_decoration_v1_destroy(decoration_);
        }
        xdg_toplevel_destroy(toplevel_);
        xdg_surface_destroy(xdg_surface_);
    }

    void Window::set_title(const std::string &title) {
        xdg_toplevel_set_title(toplevel_, title.c_str());
    }

    void Window::set_shadow(int margin, int resize_border) {
        shadow_ = margin, resize_border_ = resize_border;
        update_margin();
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

    void Window::on_scale_changed() {
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

    void Window::on_frame_done() {
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
            set_viewport(surface_width(), surface_height());
            int b = margin_ ? resize_border_ : 0;
            set_input_region(margin_ - b, margin_ - b, width_ + 2 * b, height_ + 2 * b);
        }
        request_frame();
    }

} // namespace lyren::wl
