#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "wl/surface.hpp"

namespace lyren::wl {

    // xdg_toplevel with optional client-side decorations. The surface is larger than the window geometry by a
    // shadow margin on each side; the margin disappears when maximized, tiled or fullscreen.
    class Window : public Surface {
    public:
        struct Callbacks {
            std::move_only_function<void()> configure; // size, scale, state or decoration mode changed
            std::move_only_function<void()> close;
            std::move_only_function<void()> frame; // the compositor is ready for the next frame
            std::move_only_function<void(const PointerEvent &)> pointer;
            std::move_only_function<void(const KeyEvent &)> key;
            std::move_only_function<void(bool)> focus;
            std::move_only_function<void(const TextInputEvent &)> text_input;
        };

        Window(Display &display, Callbacks cb, int width, int height);
        ~Window() override;

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
        uint32_t buffer_width() const {
            return buffer_px(surface_width());
        }
        uint32_t buffer_height() const {
            return buffer_px(surface_height());
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
        // A configure arrived that the next presented frame must acknowledge.
        bool configure_pending() const {
            return needs_ack_ || geometry_dirty_;
        }

        void set_title(const std::string &title);
        // Shadow margin and the band outside the geometry that still resizes (logical px); only used with CSD.
        void set_shadow(int margin, int resize_border);

        void move();
        void resize(uint32_t xdg_edges);
        // Takes another size, as a floating window may; the compositor follows the next commit (scripted runs).
        void set_size(int width, int height) {
            width_ = width, height_ = height, geometry_dirty_ = true;
        }
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
        void close_requested() {
            cb_.close();
        }

    private:
        void on_scale_changed() override;
        void on_frame_done() override;
        void update_margin();

        Callbacks cb_;
        xdg_surface *xdg_surface_ = nullptr;
        xdg_toplevel *toplevel_ = nullptr;
        zxdg_toplevel_decoration_v1 *decoration_ = nullptr;

        int width_, height_;
        int pending_w_ = 0, pending_h_ = 0;
        bool pending_max_ = false, pending_full_ = false, pending_act_ = false, pending_tiled_ = false;
        bool maximized_ = false, fullscreen_ = false, activated_ = false, tiled_ = false;
        bool csd_ = true;
        int shadow_ = 0, resize_border_ = 0, margin_ = 0;
        uint32_t ack_serial_ = 0;
        bool configured_ = false, needs_ack_ = false, geometry_dirty_ = true;
    };

} // namespace lyren::wl
