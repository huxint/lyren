#pragma once
#include <memory>
#include <string>

#include "app/overlay.hpp"
#include "core/loop.hpp"
#include "gfx/renderer.hpp"
#include "sys/config.hpp"
#include "text/atlas.hpp"
#include "text/fonts.hpp"
#include "text/shaper.hpp"
#include "ui/painter.hpp"
#include "ui/theme.hpp"
#include "wl/display.hpp"
#include "wl/window.hpp"

namespace lyren {

class App {
public:
    // select_driver() must already have run for config.gpu.
    App(Loop& loop, ConfigFile config, bool show_overlay);
    ~App();

private:
    enum class Hit { none, client, caption, minimize, maximize, close, resize };

    void request_redraw();
    void render();
    void build(ui::Painter& p);
    void draw_titlebar(ui::Painter& p);
    void draw_demo(ui::Painter& p, ui::Rect area);
    Hit hit_test(float x, float y, uint32_t& edges) const;
    void on_pointer(const wl::PointerEvent& e);
    void on_key(const wl::KeyEvent& e);
    void reload_config();
    void apply_config(bool initial);
    void load_text(const std::string& family);
    void toast(std::string message);
    void sample_overlay();

    Loop& loop_;
    ConfigFile config_;
    ui::Theme theme_;
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

    bool dirty_ = true, scheduled_ = false;
    long frames_ = 0;
    double frame_ms_ = 0;
    float mouse_x_ = -1, mouse_y_ = -1;  // window geometry coordinates
    Hit hover_ = Hit::none, pressed_ = Hit::none;
    uint32_t last_click_ = 0;
    std::string toast_;
    Loop::TimerId toast_timer_ = 0, overlay_timer_ = 0, idle_timer_ = 0;
};

}  // namespace lyren
