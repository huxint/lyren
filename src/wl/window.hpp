#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "wl/input.hpp"
#include "wl/protocols.hpp"

namespace lyren::wl {

    class Display;

    // xdg_toplevel with optional client-side decorations. The surface is larger than the window geometry by a
    // shadow margin on each side; the margin disappears when maximized, tiled or fullscreen.
    class Window : public SurfaceSink {
    public:
        struct Callbacks {
            std::function<void()> configure; // size, scale, state or decoration mode changed
            std::function<void()> close;
            std::function<void()> frame; // the compositor is ready for the next frame
            std::function<void(const PointerEvent &)> pointer;
            std::function<void(const KeyEvent &)> key;
            std::function<void(bool)> focus;
            std::function<void(const TextInputEvent &)> text_input;
        };

        Window(Display &display, Callbacks cb, bool client_side, int width, int height);
        ~Window() override;

        wl_surface *surface() const {
            return surface_;
        }
        int width() const {
            return width_;
        }
        int height() const {
            return height_;
        }
        int margin() const {
            return margin_;
        }
        int surface_width() const {
            return width_ + 2 * margin_;
        }
        int surface_height() const {
            return height_ + 2 * margin_;
        }
        double scale() const {
            return scale120_ / 120.0;
        }
        uint32_t buffer_width() const {
            return uint32_t(surface_width() * scale120_ / 120.0 + 0.5);
        }
        uint32_t buffer_height() const {
            return uint32_t(surface_height() * scale120_ / 120.0 + 0.5);
        }
        bool client_side() const {
            return csd_;
        }
        bool maximized() const {
            return maximized_;
        }
        bool fullscreen() const {
            return fullscreen_;
        }
        bool activated() const {
            return activated_;
        }
        // True between presenting a frame and the compositor's frame callback.
        bool frame_pending() const {
            return frame_pending_;
        }
        // A configure arrived that the next presented frame must acknowledge.
        bool configure_pending() const {
            return needs_ack_ || geometry_dirty_;
        }

        void set_title(const std::string &title);
        void set_decorations(bool client_side);
        // Shadow margin and the band outside the geometry that still resizes (logical px); only used with CSD.
        void set_shadow(int margin, int resize_border, int corner_radius);
        void set_blur(bool on);

        void move();
        void resize(uint32_t xdg_edges);
        void show_menu(int x, int y);
        void minimize();
        void toggle_maximize();
        // Asks to be focused. A launcher's token is honoured; without one the compositor decides, and KWin then only
        // marks the window as wanting attention unless it is already active.
        void activate(const std::string &token);

        // Must run right before the frame is presented (the present commits the surface).
        void prepare_present();

        void on_pointer(const PointerEvent &e) override {
            cb_.pointer(e);
        }
        void on_key(const KeyEvent &e) override {
            cb_.key(e);
        }
        void on_keyboard_focus(bool f) override {
            cb_.focus(f);
        }
        void on_text_input(const TextInputEvent &e) override {
            cb_.text_input(e);
        }

        // Listener plumbing.
        void toplevel_configure(int w, int h, wl_array *states);
        void surface_configure(uint32_t serial);
        void configure_bounds(int w, int h);
        void decoration_configure(uint32_t mode);
        void preferred_scale(uint32_t scale120);
        void frame_done();
        void close_requested() {
            cb_.close();
        }

    private:
        void update_margin();

        Display &display_;
        Callbacks cb_;
        wl_surface *surface_ = nullptr;
        xdg_surface *xdg_surface_ = nullptr;
        xdg_toplevel *toplevel_ = nullptr;
        zxdg_toplevel_decoration_v1 *decoration_ = nullptr;
        wp_viewport *viewport_ = nullptr;
        wp_fractional_scale_v1 *fractional_ = nullptr;
        ext_background_effect_surface_v1 *effect_ = nullptr;

        int width_, height_;
        int pending_w_ = 0, pending_h_ = 0;
        bool pending_max_ = false, pending_full_ = false, pending_act_ = false, pending_tiled_ = false;
        bool maximized_ = false, fullscreen_ = false, activated_ = false, tiled_ = false;
        bool csd_ = true, want_csd_ = true;
        int shadow_ = 0, resize_border_ = 0, radius_ = 0, margin_ = 0;
        uint32_t scale120_ = 120;
        uint32_t ack_serial_ = 0;
        bool configured_ = false, needs_ack_ = false, geometry_dirty_ = true, blur_ = false;
        bool frame_pending_ = false;
    };

} // namespace lyren::wl
