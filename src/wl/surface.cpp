#include "wl/surface.hpp"

#include "wl/display.hpp"

namespace lyren::wl {

    static Surface *self(void *data) {
        return static_cast<Surface *>(data);
    }

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

    Surface::Surface(Display &display)
    : display_(display) {
        surface_ = wl_compositor_create_surface(display_.compositor);
        wl_surface_set_user_data(surface_, static_cast<SurfaceSink *>(this));
        viewport_ = wp_viewporter_get_viewport(display_.viewporter, surface_);
        fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(display_.fractional_scale, surface_);
        wp_fractional_scale_v1_add_listener(fractional_, &fractional_listener, this);
    }

    Surface::~Surface() {
        if (display_.input) {
            display_.input->detach(this);
        }
        if (frame_callback_) {
            wl_callback_destroy(frame_callback_);
        }
        wp_fractional_scale_v1_destroy(fractional_);
        wp_viewport_destroy(viewport_);
        wl_surface_destroy(surface_);
    }

    void Surface::preferred_scale(uint32_t scale120) {
        if (scale120 == scale120_) {
            return;
        }
        scale120_ = scale120;
        on_scale_changed();
    }

    void Surface::frame_done() {
        frame_callback_ = nullptr;
        on_frame_done();
    }

    void Surface::set_viewport(int w, int h) {
        wp_viewport_set_destination(viewport_, w, h);
    }

    void Surface::set_input_region(int x, int y, int w, int h) {
        wl_region *input = wl_compositor_create_region(display_.compositor);
        wl_region_add(input, x, y, w, h);
        wl_surface_set_input_region(surface_, input);
        wl_region_destroy(input);
    }

    void Surface::request_frame() {
        if (frame_callback_) {
            wl_callback_destroy(frame_callback_);
        }
        frame_callback_ = wl_surface_frame(surface_);
        wl_callback_add_listener(frame_callback_, &frame_listener, this);
    }

} // namespace lyren::wl
