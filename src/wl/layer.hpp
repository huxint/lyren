#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "wl/surface.hpp"

namespace lyren::wl {

    // A wlr-layer-shell surface covering the available output that never
    // takes the keyboard. Only its input region receives the pointer; elsewhere clicks fall through.
    class LayerSurface : public Surface {
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

        int width() const {
            return width_;
        }
        int height() const {
            return height_;
        }
        uint32_t buffer_width() const {
            return buffer_px(width_);
        }
        uint32_t buffer_height() const {
            return buffer_px(height_);
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
        void closed() {
            cb_.closed();
        }

    private:
        void on_scale_changed() override;
        void on_frame_done() override;

        Callbacks cb_;
        zwlr_layer_surface_v1 *layer_ = nullptr;
        int width_ = 0, height_ = 0;
        std::array<int, 4> input_{};
        uint32_t ack_serial_ = 0;
        bool needs_ack_ = false, dirty_ = true;
    };

} // namespace lyren::wl
