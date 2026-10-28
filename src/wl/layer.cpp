#include "wl/layer.hpp"

#include <algorithm>

#include "wl/display.hpp"

namespace lyren::wl {

    static LayerSurface *self(void *data) {
        return static_cast<LayerSurface *>(data);
    }

    static const zwlr_layer_surface_v1_listener layer_listener{
        .configure =
            [](void *d, zwlr_layer_surface_v1 *, uint32_t serial, uint32_t w, uint32_t h) {
                self(d)->configure(serial, static_cast<int>(w), static_cast<int>(h));
            },
        .closed =
            [](void *d, zwlr_layer_surface_v1 *) {
                self(d)->closed();
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

    LayerSurface::LayerSurface(Display &display, Callbacks cb, bool above_windows, int height, int margin, bool top)
    : display_(display),
      cb_(std::move(cb)),
      height_(height),
      margin_(margin),
      top_(top) {
        surface_ = wl_compositor_create_surface(display_.compositor);
        wl_surface_set_user_data(surface_, static_cast<SurfaceSink *>(this));
        viewport_ = wp_viewporter_get_viewport(display_.viewporter, surface_);
        fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(display_.fractional_scale, surface_);
        wp_fractional_scale_v1_add_listener(fractional_, &fractional_listener, this);

        layer_ = zwlr_layer_shell_v1_get_layer_surface(display_.layer_shell, surface_, nullptr, above_windows ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                                                       "lyren-lyrics");
        zwlr_layer_surface_v1_add_listener(layer_, &layer_listener, this);
        zwlr_layer_surface_v1_set_anchor(layer_, (top_ ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(layer_, 0, static_cast<uint32_t>(height_));
        zwlr_layer_surface_v1_set_margin(layer_, top_ ? margin_ : 0, 0, top_ ? 0 : margin_, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(surface_);
        while (!needs_ack_) {
            wl_display_dispatch(display_.display);
        }
    }

    LayerSurface::~LayerSurface() {
        wp_fractional_scale_v1_destroy(fractional_);
        wp_viewport_destroy(viewport_);
        zwlr_layer_surface_v1_destroy(layer_);
        wl_surface_destroy(surface_);
    }

    void LayerSurface::set_height(int h) {
        if (h == height_) {
            return;
        }
        height_ = h;
        zwlr_layer_surface_v1_set_size(layer_, 0, static_cast<uint32_t>(h));
        dirty_ = true;
    }

    void LayerSurface::set_position(bool top, int margin) {
        if (margin == margin_ && top == top_) {
            return;
        }
        margin_ = margin;
        top_ = top;
        zwlr_layer_surface_v1_set_anchor(layer_, (top_ ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_margin(layer_, top_ ? margin_ : 0, 0, top_ ? 0 : margin_, 0);
        dirty_ = true;
    }

    void LayerSurface::set_input(int x, int y, int w, int h) {
        std::array r = {x, y, w, h};
        if (r == input_) {
            return;
        }
        input_ = r;
        dirty_ = true;
    }

    void LayerSurface::configure(uint32_t serial, int w, int h) {
        if (w > 0) {
            width_ = w;
        }
        if (h > 0) {
            height_ = h;
        }
        ack_serial_ = serial;
        needs_ack_ = dirty_ = true;
        if (cb_.configure) {
            cb_.configure();
        }
    }

    void LayerSurface::preferred_scale(uint32_t scale120) {
        if (scale120 == scale120_) {
            return;
        }
        scale120_ = scale120;
        dirty_ = true;
        if (cb_.configure) {
            cb_.configure();
        }
    }

    void LayerSurface::frame_done() {
        frame_pending_ = false;
        if (cb_.frame) {
            cb_.frame();
        }
    }

    void LayerSurface::prepare_present() {
        if (needs_ack_) {
            zwlr_layer_surface_v1_ack_configure(layer_, ack_serial_);
            needs_ack_ = false;
        }
        if (dirty_) {
            dirty_ = false;
            wp_viewport_set_destination(viewport_, width_, height_);
            wl_region *input = wl_compositor_create_region(display_.compositor);
            wl_region_add(input, input_[0], input_[1], input_[2], input_[3]);
            wl_surface_set_input_region(surface_, input);
            wl_region_destroy(input);
        }
        wl_callback_add_listener(wl_surface_frame(surface_), &frame_listener, this);
        frame_pending_ = true;
    }

} // namespace lyren::wl
