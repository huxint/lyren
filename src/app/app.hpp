#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/images.hpp"
#include "app/state.hpp"
#include "app/store.hpp"
#include "app/view/shell.hpp"
#include "audio/player.hpp"
#include "ncm/netease.hpp"
#include "qq/qq.hpp"
#include "qishui/qishui.hpp"
#include "kugou/kugou.hpp"
#include "bodian/bodian.hpp"
#include "net/http.hpp"
#include "core/loop.hpp"
#include "sys/bus.hpp"
#include "gfx/renderer.hpp"
#include "sys/config.hpp"
#include "text/atlas.hpp"
#include "text/fonts.hpp"
#include "text/shaper.hpp"
#include "ui/frames.hpp"
#include "ui/painter.hpp"
#include "ui/theme.hpp"
#include "ui/ui.hpp"
#include "wl/display.hpp"
#include "wl/layer.hpp"
#include "wl/window.hpp"

namespace lyren {

    class App {
    public:
        // select_driver() must already have run for config.gpu. script: lines for run_script(), may be empty; a scripted
        // run never makes a sound and never takes focus.
        App(Loop &loop, Bus &bus, ConfigFile config, std::vector<std::string> script, std::string initial_uri = {});
        ~App();

    private:
        struct TextStack {
            text::Fonts fonts;
            text::Shaper shaper;
            text::Atlas atlas;
            ui::Painter painter;

            TextStack(gfx::Renderer &renderer, const std::string &family)
            : fonts(family),
              shaper(fonts),
              atlas(renderer, fonts),
              painter(atlas, shaper, fonts) {}
        };

        struct Surface {
            gfx::Target *target = nullptr;
            ui::Ui ui;
            ui::FrameInput input;
            uint64_t hash = 0;
            ui::Frames frames;

            Surface(Loop &loop, std::move_only_function<bool()> pending, std::move_only_function<void()> draw)
            : frames(loop, std::move(pending), std::move(draw)) {}
        };

        double elapsed(Clock::time_point t) const {
            return std::chrono::duration<double>(t - start_).count();
        }
        double now() const {
            return elapsed(Clock::now());
        }
        void create_window();
        void handle_close();
        void hide_window();
        void show_window(std::string_view token);
        void request_redraw();
        template <class Prepare>
        void present(Surface &surf, uint32_t bw, uint32_t bh, bool force, Prepare &&prepare);
        void render();
        void build(double now);
        // stage: how far the now-playing stage is up; the title bar keeps only the window buttons on it.
        void titlebar(ui::Rect bar, float stage);
        bool window_button(ui::Id id, ui::Rect r, std::string_view icon, float size, bool close);
        void caption_press(const ui::PointerAction &a);
        void after_frame(Clock::time_point frame_start);
        // Desktop lyrics: a remembered playback control, on a surface with its own frames, so they
        // keep going while the window is minimized.
        void sync_desktop_lyrics();
        void close_desktop_lyrics();
        void render_lyrics();
        void on_lyrics_pointer(const wl::PointerEvent &e);
        void on_pointer(const wl::PointerEvent &e);
        void on_key(const wl::KeyEvent &e);
        void on_text_input(const wl::TextInputEvent &e);
        void note_input();
        void reload_config();
        // Applies config_ after it replaced previous.
        void config_updated(const Config &previous);
        void apply_config(bool initial);
        void load_text(const std::string &family);
        void toast(std::string message);
        // Replays synthetic input (development aid for visual checks); see main.cpp for the commands.
        void run_script(size_t line);

        Loop &loop_;
        ConfigFile config_;
        Clock::time_point start_ = Clock::now(), last_input_ = start_;
        ui::Theme theme_, frame_theme_, stage_theme_, lyrics_theme_;
        wl::Display display_;
        gfx::Context gpu_;
        gfx::Renderer renderer_;
        std::optional<TextStack> text_;
        std::unique_ptr<wl::Window> window_;
        std::unique_ptr<wl::LayerSurface> lyrics_;
        ConfigWatcher watcher_;
        Store store_;
        SavedState saved_ = load_state();
        int saved_w_ = saved_.width, saved_h_ = saved_.height;
        net::Http http_;
        audio::Player player_;
        ncm::Netease ncm_;
        qq::Qq qq_;
        qishui::Qishui qishui_;
        kugou::Kugou kugou_;
        bodian::Bodian bodian_;
        Images images_;
        Shell shell_;

        uint32_t resize_edges_ = 0; // pointer is in the resize band outside the geometry
        uint32_t last_press_time_ = 0, last_press_button_ = 0;
        float last_press_x_ = 0, last_press_y_ = 0;
        Loop::TimerId idle_timer_ = 0;
        std::vector<std::string> script_;
        Surface main_;
        Surface overlay_;
    };

} // namespace lyren
