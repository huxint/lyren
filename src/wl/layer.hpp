#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "wl/input.hpp"
#include "wl/protocols.hpp"

namespace lyren::wl {

    class Display;

    // A wlr-layer-shell surface covering the available output that never
    // takes the keyboard. Only its input region receives the pointer; elsewhere clicks fall through.
    class LayerSurface : public SurfaceSink {
    public:
        struct Callbacks {
            std::move_only_function<void()> configure; // size or scale changed
            std::move_only_function<void()> closed;    // the compositor took it away; destroy it
            std::move_only_function<void()> frame;
            std::move_only_function<void(const PointerEvent &)> pointer;
        };

        // above_windows: the top layer; otherwise the bottom one, under normal windows.
        LayerSurface(Display &display, Callbacks cb, bool above_windows);
        ~LayerSurface() override;

        wl_surface *surface() const {
            return surface_;
        }
        int width() const {
            return width_;
        }
        int height() const {
            return height_;
        }
        double scale() const {
            return scale120_ / 120.0;
        }
        uint32_t buffer_width() const {
            return static_cast<uint32_t>(width_ * scale120_ / 120.0 + 0.5);
        }
        uint32_t buffer_height() const {
            return static_cast<uint32_t>(height_ * scale120_ / 120.0 + 0.5);
        }
        bool frame_pending() const {
            return frame_callback_ != nullptr;
        }
        // A configure or input region change that only a commit applies.
        bool needs_commit() const {
            return needs_ack_ || dirty_;
        }

        // Logical px, surface-local; an empty rectangle lets every click through.
        void set_input(int x, int y, int w, int h);

        // Must run right before the frame is presented (the present commits the surface).
        void prepare_present();

        void on_pointer(const PointerEvent &e) override {
            cb_.pointer(e);
        }

        // Listener plumbing.
        void configure(uint32_t serial, int w, int h);
        void preferred_scale(uint32_t scale120);
        void frame_done();
        void closed() {
            cb_.closed();
        }

    private:
        Display &display_;
        Callbacks cb_;
        wl_surface *surface_ = nullptr;
        zwlr_layer_surface_v1 *layer_ = nullptr;
        wp_viewport *viewport_ = nullptr;
        wp_fractional_scale_v1 *fractional_ = nullptr;
        wl_callback *frame_callback_ = nullptr;
        int width_ = 0, height_ = 0;
        std::array<int, 4> input_{};
        uint32_t scale120_ = 120, ack_serial_ = 0;
        bool needs_ack_ = false, dirty_ = true;
    };

} // namespace lyren::wl
