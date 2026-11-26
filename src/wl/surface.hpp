#pragma once
#include <cstdint>

#include "wl/input.hpp"
#include "wl/protocols.hpp"

namespace lyren::wl {

    class Display;

    // Shared Wayland surface ownership for top-level windows and layer surfaces:
    // manages wl_surface, viewport, fractional scale, input detachment, and frame callbacks.
    class Surface : public SurfaceSink {
    public:
        explicit Surface(Display &display);
        ~Surface() override;

        Surface(const Surface &) = delete;
        Surface &operator=(const Surface &) = delete;

        wl_surface *surface() const {
            return surface_;
        }
        double scale() const {
            return scale120_ / 120.0;
        }
        // True between presenting a frame and the compositor's frame callback.
        bool frame_pending() const {
            return frame_callback_ != nullptr;
        }

        // Listener plumbing.
        void preferred_scale(uint32_t scale120);
        void frame_done();

    protected:
        uint32_t buffer_px(int logical) const {
            return static_cast<uint32_t>(logical * scale120_ / 120.0 + 0.5);
        }
        void set_viewport(int w, int h);
        void set_input_region(int x, int y, int w, int h);
        void request_frame();

        virtual void on_scale_changed() = 0;
        virtual void on_frame_done() = 0;

        Display &display_;
        wl_surface *surface_ = nullptr;

    private:
        wp_viewport *viewport_ = nullptr;
        wp_fractional_scale_v1 *fractional_ = nullptr;
        wl_callback *frame_callback_ = nullptr;
        uint32_t scale120_ = 120;
    };

} // namespace lyren::wl
