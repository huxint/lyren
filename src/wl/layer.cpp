#include "wl/layer.hpp"

#include <utility>

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

    LayerSurface::LayerSurface(Display &display, Callbacks cb, bool above_windows)
    : Surface(display),
      cb_(std::move(cb)) {
        layer_ = zwlr_layer_shell_v1_get_layer_surface(display_.layer_shell, surface_, nullptr, above_windows ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                                                       "lyren-lyrics");
        zwlr_layer_surface_v1_add_listener(layer_, &layer_listener, this);
        zwlr_layer_surface_v1_set_anchor(layer_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(layer_, 0, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(surface_);
        while (!needs_ack_) {
            wl_display_dispatch(display_.display);
        }
    }

    LayerSurface::~LayerSurface() {
        zwlr_layer_surface_v1_destroy(layer_);
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

    void LayerSurface::on_scale_changed() {
        dirty_ = true;
        if (cb_.configure) {
            cb_.configure();
        }
    }

    void LayerSurface::on_frame_done() {
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
            set_viewport(width_, height_);
            set_input_region(input_[0], input_[1], input_[2], input_[3]);
        }
        request_frame();
    }

} // namespace lyren::wl
