#pragma once
#include <memory>
#include <string>
#include <vector>

#include "app/frames.hpp"
#include "app/images.hpp"
#include "app/overlay.hpp"
#include "app/shell.hpp"
#include "app/state.hpp"
#include "audio/player.hpp"
#include "ncm/netease.hpp"
#include "net/http.hpp"
#include "core/loop.hpp"
#include "sys/bus.hpp"
#include "gfx/renderer.hpp"
#include "sys/config.hpp"
#include "text/atlas.hpp"
#include "text/fonts.hpp"
#include "text/shaper.hpp"
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
    App(Loop& loop, Bus& bus, ConfigFile config, bool show_overlay, std::vector<std::string> script);
    ~App();

private:
    void request_redraw();
    void render();
    void build(double now);
    void titlebar(ui::Rect bar);
    bool window_button(ui::Id id, ui::Rect r, std::string_view icon, float size, bool close);
    void caption_press(const ui::PointerAction& a);
    void after_frame(Clock::time_point frame_start);
    // Desktop lyrics: shown while desktop_lyrics.enabled, on a surface of their own with their own frames, so they
    // keep going while the window is minimized.
    void sync_desktop_lyrics();
    void close_desktop_lyrics();
    void render_lyrics();
    void on_lyrics_pointer(const wl::PointerEvent& e);
    void on_pointer(const wl::PointerEvent& e);
    void on_key(const wl::KeyEvent& e);
    void on_text_input(const wl::TextInputEvent& e);
    void note_input();
    void reload_config();
    void apply_config(bool initial);
    void load_text(const std::string& family);
    void toast(std::string message);
    void sample_overlay();
    // Replays synthetic input (development aid for visual checks); see main.cpp for the commands.
    void run_script(size_t line);

    Loop& loop_;
    ConfigFile config_;
    ui::Theme theme_, frame_theme_;
    wl::Display display_;
    gfx::Context gpu_;
    gfx::Renderer renderer_;
    std::unique_ptr<text::Fonts> fonts_;
    std::unique_ptr<text::Shaper> shaper_;
    std::unique_ptr<text::Atlas> atlas_;
    std::unique_ptr<ui::Painter> painter_;
    std::unique_ptr<wl::Window> window_;
    gfx::Target* target_ = nullptr;
    ConfigWatcher watcher_;
    Overlay overlay_;
    ui::Ui ui_;
    ui::FrameInput input_;
    SavedState saved_ = load_state();
    net::Http http_;
    audio::Player player_;
    ncm::Netease ncm_;
    qq::Qq qq_;
    Images images_;
    Shell shell_;

    Clock::time_point start_ = Clock::now();
    long frames_drawn_ = 0;
    uint64_t frame_hash_ = 0;
    double frame_ms_ = 0;
    uint32_t resize_edges_ = 0;  // pointer is in the resize band outside the geometry
    uint32_t last_press_time_ = 0, last_press_button_ = 0;
    float last_press_x_ = 0, last_press_y_ = 0;
    Loop::TimerId overlay_timer_ = 0, idle_timer_ = 0;
    std::vector<std::string> script_;
    Frames frames_;

    std::unique_ptr<wl::LayerSurface> lyrics_;
    gfx::Target* lyrics_target_ = nullptr;
    ui::Theme lyrics_theme_;
    ui::Ui lyrics_ui_;
    ui::FrameInput lyrics_input_;
    uint64_t lyrics_hash_ = 0;
    Frames lyric_frames_;
};

}  // namespace lyren
