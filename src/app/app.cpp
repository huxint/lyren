#include "app/app.hpp"

#include <linux/input-event-codes.h>
#include <png.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>

#include "core/log.hpp"
#include "core/memstat.hpp"
#include "wl/clipboard.hpp"
#include "wl/input.hpp"

namespace lyren {

constexpr float titlebar_h = 52;
constexpr int shadow_margin = 28, resize_border = 8;

static wl::Cursor cursor_for(uint32_t edges) {
    switch (edges) {
        case XDG_TOPLEVEL_RESIZE_EDGE_TOP: return wl::Cursor::n_resize;
        case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: return wl::Cursor::s_resize;
        case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: return wl::Cursor::w_resize;
        case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: return wl::Cursor::e_resize;
        case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: return wl::Cursor::nw_resize;
        case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: return wl::Cursor::ne_resize;
        case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: return wl::Cursor::sw_resize;
        default: return wl::Cursor::se_resize;
    }
}

// Resize edges for a point in the band outside the w×h geometry; corners take 16 px along each side.
static uint32_t edges_at(float x, float y, float w, float h) {
    constexpr float corner = 16;
    bool l = x < 0, r = x >= w, t = y < 0, b = y >= h;
    if (t || b) l |= x < corner, r |= x >= w - corner;
    if (l || r) t |= y < corner, b |= y >= h - corner;
    return (t ? XDG_TOPLEVEL_RESIZE_EDGE_TOP : 0) | (b ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM : 0) | (l ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT : 0) |
           (r ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT : 0);
}

App::App(Loop& loop, Bus& bus, ConfigFile config, bool show_overlay, std::vector<std::string> script)
    : loop_(loop),
      config_(std::move(config)),
      display_(loop),
      gpu_(display_.display),
      renderer_(gpu_),
      watcher_(loop, config_.path, [this] { reload_config(); }),
      player_(loop, http_),
      ncm_(loop, http_),
      qq_(loop, http_),
      images_(
          loop, http_, renderer_, [this] { request_redraw(); }, [this] { return std::chrono::duration<double>(Clock::now() - start_).count(); }),
      shell_(ShellDeps{.loop = loop,
                       .http = http_,
                       .player = player_,
                       .providers = {&ncm_, &qq_},
                       .images = images_,
                       .config = config_.config,
                       .set_setting =
                           [this](std::string_view section, std::string_view key, std::string_view value) {
                               if (auto r = save_setting(config_, section, key, value); !r) return toast(r.error());
                               log::info("config: {}.{} = {}", section, key, value);
                               shell_.config_changed();
                               // Settings change mid-frame; the lyrics surface may be the one drawing.
                               loop_.defer([this] { sync_desktop_lyrics(); });
                           },
                       .redraw = [this] { request_redraw(); },
                       .now = [this] { return std::chrono::duration<double>(Clock::now() - start_).count(); },
                       .silent = !script.empty(),
                       .bus = bus,
                       .raise =
                           [this](std::string_view token) {
                               if (!script_.empty()) return log::info("raise requested ({})", token.empty() ? "no token" : token);
                               window_->activate(std::string(token));
                           }},
             saved_),
      script_(std::move(script)),
      frames_(loop, [this] { return window_->frame_pending(); }, [this] { render(); }),
      lyric_frames_(loop, [this] { return lyrics_ && lyrics_->frame_pending(); }, [this] { render_lyrics(); }) {
    std::string errors;
    theme_ = ui::Theme::from(config_.config, errors);
    load_text(config_.config.theme.font);

    wl::Window::Callbacks cb{
        .configure =
            [this] {
                if (!window_) return;
                note_input();
                request_redraw();
            },
        .close = [this] { loop_.quit(); },
        .frame = [this] { frames_.frame_done(); },
        .pointer = [this](const wl::PointerEvent& e) { on_pointer(e); },
        .key = [this](const wl::KeyEvent& e) { on_key(e); },
        .focus = [this](bool) { request_redraw(); },
        .text_input = [this](const wl::TextInputEvent& e) { on_text_input(e); },
    };
    int w = saved_.width > 0 ? std::max(saved_.width, 720) : 1180, h = saved_.height > 0 ? std::max(saved_.height, 480) : 760;
    saved_ = {};
    window_ = std::make_unique<wl::Window>(display_, std::move(cb), config_.config.window.decorations == Decorations::client, w, h);
    window_->set_title("Lyren");
    target_ = renderer_.create_target(display_.display, window_->surface());
    apply_config(true);
    if (!config_.error.empty()) toast(describe_config_error(config_.error));
    else if (!errors.empty()) toast(errors.substr(0, errors.find('\n')));
    if (show_overlay) {
        overlay_.visible = true;
        sample_overlay();
    }
    request_redraw();
    if (!script_.empty()) loop_.after(500ms, [this] { run_script(0); });
}

App::~App() {
    shell_.save(window_->maximized() ? std::nullopt : std::optional{std::pair{window_->width(), window_->height()}});
    if (lyrics_) close_desktop_lyrics();
    renderer_.destroy_target(target_);
    loop_.cancel(overlay_timer_);
    loop_.cancel(idle_timer_);
}

void App::load_text(const std::string& family) {
    painter_.reset();
    atlas_.reset();
    shaper_.reset();
    fonts_ = std::make_unique<text::Fonts>(family);
    shaper_ = std::make_unique<text::Shaper>(*fonts_);
    atlas_ = std::make_unique<text::Atlas>(renderer_, *fonts_);
    painter_ = std::make_unique<ui::Painter>(*atlas_, *shaper_, *fonts_);
}

void App::apply_config(bool initial) {
    std::string errors;
    theme_ = ui::Theme::from(config_.config, errors);
    if (!initial && !errors.empty()) toast(errors.substr(0, errors.find('\n')));
    window_->set_decorations(config_.config.window.decorations == Decorations::client);
    window_->set_shadow(shadow_margin, resize_border, int(theme_.radius_l()));
    window_->set_blur(theme_.opacity < 1);
    sync_desktop_lyrics();
    request_redraw();
}

void App::reload_config() {
    std::ifstream in(config_.path);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (text == config_.text) return;
    config_.text = text;
    auto parsed = parse_config(text);
    if (!parsed) {
        toast(describe_config_error(parsed.error()));
        return;
    }
    Config old = config_.config;
    config_.config = *parsed;
    log::info("config reloaded");
    shell_.config_changed();
    if (parsed->theme.font != old.theme.font) load_text(parsed->theme.font);
    apply_config(false);
    if (parsed->gpu.device != old.gpu.device) toast("GPU 设置会在重启后生效");
}

void App::toast(std::string message) {
    log::warn("{}", message);
    ui_.toast(std::move(message), "triangle-alert", true);
    request_redraw();
}

void App::request_redraw() {
    // Async results can arrive while the window is still being set up.
    if (!window_) return;
    frames_.request();
    lyric_frames_.request();
}

// Once input stops for a while, give back font pages and freed heap; glyphs live in the atlas.
void App::note_input() {
    loop_.cancel(idle_timer_);
    idle_timer_ = loop_.after(5s, [this] {
        idle_timer_ = 0;
        fonts_->trim();
        mem_trim();
    });
}

void App::render() {
    if (!frames_.begin()) return;
    auto start = Clock::now();
    double now = std::chrono::duration<double>(start - start_).count();
    renderer_.resize(*target_, window_->buffer_width(), window_->buffer_height());
    input_.mods = display_.input->mods();
    // A full atlas is cleared mid-frame; the second pass redraws with the events already handled.
    for (int pass = 0; pass < 2; ++pass) {
        atlas_->begin_frame();
        build(now);
        input_.consumed();
        if (!atlas_->reset()) break;
    }
    shaper_->end_frame();
    // Pointer motion over static areas rebuilds an identical frame; skip presenting it.
    auto prims = painter_->prims();
    uint64_t hash = std::hash<std::string_view>{}({reinterpret_cast<const char*>(prims.data()), prims.size_bytes()}) ^
                    (uint64_t(window_->buffer_width()) << 32 | window_->buffer_height());
    if (hash != frame_hash_ || window_->configure_pending()) {
        const float clear[4] = {0, 0, 0, 0};
        renderer_.draw(*target_, prims, clear, [this] { window_->prepare_present(); });
        frame_hash_ = hash;
    }
    frame_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    after_frame(start);
    if (++frames_drawn_ == 1) {
        note_input();
        mem_trim();
        auto m = mem_sample();
        log::info("first frame in {:.1f} ms: RSS {:.1f} MB (anon {:.1f}, file {:.1f}), GPU {:.1f} MB", frame_ms_, m.rss / 1024.0, m.anon / 1024.0,
                  m.file / 1024.0, m.gpu / 1024.0);
    }
}

void App::build(double now) {
    float m = float(window_->margin()), w = float(window_->width()), h = float(window_->height());
    auto& p = *painter_;
    p.begin(float(window_->scale()), m, m);
    frame_theme_ = theme_;
    bool accent_moving = false;
    if (theme_.auto_accent) frame_theme_.accent = shell_.accent(now, theme_.dark, accent_moving);

    bool floating = window_->client_side() && m > 0;
    float radius = floating ? theme_.radius_l() : 0;
    if (floating) p.shadow({0, 6, w, h}, radius, 20, theme_.shadow, theme_.opacity < 1);
    p.rect({0, 0, w, h}, theme_.bg.with_alpha(theme_.opacity), radius, floating ? theme_.border : ui::Color{}, floating ? 1 : 0);

    ui_.window_radius = radius;
    ui_.toast_bottom = Shell::player_h + 20;
    ui_.begin(p, frame_theme_, w, h, now, input_);
    if (accent_moving) ui_.keep_animating();
    ui::Rect area{0, 0, w, h};
    titlebar(ui::cut_top(area, titlebar_h));
    shell_.draw(ui_, area);
    ui_.end();
    for (auto& a : input_.actions)
        if (a.press && !a.consumed) caption_press(a);
    overlay_.draw(p, theme_, w - 12, titlebar_h + 8);
}

void App::titlebar(ui::Rect bar) {
    auto& p = *painter_;
    const auto& t = frame_theme_;
    ui::Rect r = bar;
    if (window_->client_side()) {
        ui::Rect ctl = ui::cut_right(r, 3 * 36 + 2 * 4 + 16);
        float x = ctl.x + 4, y = bar.y + (bar.h - 28) / 2;
        if (window_button(ui_.id("win-min"), {x, y, 36, 28}, "minus", 16, false)) window_->minimize();
        if (window_button(ui_.id("win-max"), {x + 40, y, 36, 28}, window_->maximized() ? "copy" : "square", 14, false)) window_->toggle_maximize();
        if (window_button(ui_.id("win-close"), {x + 80, y, 36, 28}, "x", 16, true)) loop_.quit();
        float name_w = p.measure("Lyren", {.size = t.fs(15)});
            ui::Rect mark = ui::cut_left(r, 60 + name_w);
        p.icon("disc-3", {mark.x + 20, bar.y + (bar.h - 20) / 2, 20, 20}, t.accent);
        p.text_in({mark.x + 48, bar.y, name_w, bar.h}, "Lyren", {.size = t.fs(15), .color = window_->activated() ? t.text : t.text_dim});
    } else {
        ui::cut_left(r, 8);
    }
    shell_.top_bar(ui_, r, bar.w / 2);
}

bool App::window_button(ui::Id id, ui::Rect r, std::string_view icon, float size, bool close) {
    auto& p = *painter_;
    const auto& t = frame_theme_;
    auto it = ui_.interact(id, r);
    float hot = ui_.anim(id, it.hovered ? 1 : 0, 150);
    bool down = it.held && it.hovered;
    ui::Color fg = window_->activated() ? t.text : t.text_dim;
    if (hot > 0) p.rect(r, close ? t.danger.with_alpha(hot * (down ? 0.8f : 1)) : (down ? t.press : t.hover).with_alpha(hot), t.radius_s());
    if (close) fg = fg.mix({1, 1, 1, 1}, hot);
    p.icon(icon, ui::center(r, size, size), fg);
    return it.clicked;
}

// Presses no widget took: empty title bar space moves, maximizes or opens the window menu.
void App::caption_press(const ui::PointerAction& a) {
    if (!window_->client_side() || a.y >= titlebar_h) return;
    if (a.button == BTN_LEFT) {
        if (a.double_click) window_->toggle_maximize();
        else window_->move();
    } else if (a.button == BTN_RIGHT) {
        int m = window_->margin();
        window_->show_menu(int(a.x) + m, int(a.y) + m);
    }
}

void App::after_frame(Clock::time_point frame_start) {
    if (display_.input->pointer_focus() == window_.get()) display_.input->set_cursor(resize_edges_ ? cursor_for(resize_edges_) : ui_.cursor);
    wl::TextInputState ti = ui_.text_input;
    if (ti.enabled) ti.x += window_->margin(), ti.y += window_->margin();
    display_.input->set_text_input(std::move(ti));
    if (ui_.copy) {
        display_.clipboard->set_text(std::move(*ui_.copy), display_.input->serial());
        ui_.copy.reset();
    }
    if (std::exchange(ui_.want_paste, false))
        display_.clipboard->get_text([this](std::string s) {
            input_.paste = std::move(s);
            request_redraw();
        });

    // A frame that handled input may have changed what the desktop lyrics show.
    if (ui_.needs_redraw()) lyric_frames_.request();
    frames_.after(ui_, frame_start, start_, config_.config.render.max_fps);
}

void App::sync_desktop_lyrics() {
    bool want = config_.config.desktop_lyrics.enabled;
    if (want && !display_.layer_shell) return log::warn("desktop lyrics need wlr-layer-shell, which the compositor lacks");
    int height = int(Shell::desktop_height(theme_.lyric_size));
    if (want && !lyrics_) {
        wl::LayerSurface::Callbacks cb{
            .configure = [this] { lyric_frames_.request(); },
            .closed = [this] { loop_.defer([this] { close_desktop_lyrics(); }); },
            .frame = [this] { lyric_frames_.frame_done(); },
            .pointer = [this](const wl::PointerEvent& e) { on_lyrics_pointer(e); },
        };
        // A scripted run keeps them under the windows, out of the way of whoever uses the desktop.
        lyrics_ = std::make_unique<wl::LayerSurface>(display_, std::move(cb), script_.empty(), height, shell_.desktop_bottom());
        lyrics_target_ = renderer_.create_target(display_.display, lyrics_->surface());
        lyrics_hash_ = 0;
    } else if (!want && lyrics_) {
        close_desktop_lyrics();
    }
    if (lyrics_) lyrics_->set_height(height);
    lyric_frames_.request();
}

void App::close_desktop_lyrics() {
    if (!lyrics_) return;
    renderer_.destroy_target(lyrics_target_);
    lyrics_target_ = nullptr;
    lyrics_.reset();
    lyrics_input_ = {};
}

void App::render_lyrics() {
    if (!lyrics_ || !lyric_frames_.begin()) return;
    auto start = Clock::now();
    double now = std::chrono::duration<double>(start - start_).count();
    float w = float(lyrics_->width()), h = float(lyrics_->height());
    renderer_.resize(*lyrics_target_, lyrics_->buffer_width(), lyrics_->buffer_height());
    lyrics_input_.mods = display_.input->mods();
    ui::Rect input;
    for (int pass = 0; pass < 2; ++pass) {
        atlas_->begin_frame();
        painter_->begin(float(lyrics_->scale()), 0, 0);
        lyrics_theme_ = theme_;
        bool accent_moving = false;
        if (theme_.auto_accent) lyrics_theme_.accent = shell_.accent(now, theme_.dark, accent_moving);
        lyrics_ui_.begin(*painter_, lyrics_theme_, w, h, now, lyrics_input_);
        if (accent_moving) lyrics_ui_.keep_animating();
        input = shell_.desktop_lyrics(lyrics_ui_, w, h);
        lyrics_ui_.end();
        lyrics_input_.consumed();
        if (!atlas_->reset()) break;
    }
    shaper_->end_frame();
    lyrics_->set_bottom(shell_.desktop_bottom());
    lyrics_->set_input(int(std::floor(input.x)), int(std::floor(input.y)), int(std::ceil(input.w)), int(std::ceil(input.h)));
    auto prims = painter_->prims();
    uint64_t hash = std::hash<std::string_view>{}({reinterpret_cast<const char*>(prims.data()), prims.size_bytes()}) ^
                    (uint64_t(lyrics_->buffer_width()) << 32 | lyrics_->buffer_height());
    if (hash != lyrics_hash_ || lyrics_->needs_commit()) {
        const float clear[4] = {0, 0, 0, 0};
        renderer_.draw(*lyrics_target_, prims, clear, [this] { lyrics_->prepare_present(); });
        lyrics_hash_ = hash;
    }
    if (display_.input->pointer_focus() == lyrics_.get()) display_.input->set_cursor(lyrics_ui_.cursor);
    lyric_frames_.after(lyrics_ui_, start, start_, config_.config.render.max_fps);
}

void App::on_lyrics_pointer(const wl::PointerEvent& e) {
    auto& in = lyrics_input_;
    switch (e.type) {
        case wl::PointerEvent::enter: in.left_down = false; [[fallthrough]];
        case wl::PointerEvent::motion: in.x = e.x, in.y = e.y, in.inside = true; break;
        case wl::PointerEvent::leave: in.inside = in.left_down = false; break;
        case wl::PointerEvent::press:
            in.actions.push_back({e.x, e.y, e.button, true});
            if (e.button == BTN_LEFT) in.left_down = true;
            break;
        case wl::PointerEvent::release:
            if (e.button == BTN_LEFT) in.left_down = false;
            in.actions.push_back({e.x, e.y, e.button, false});
            break;
        case wl::PointerEvent::scroll: return;
    }
    lyric_frames_.request();
}

void App::on_pointer(const wl::PointerEvent& e) {
    float m = float(window_->margin()), w = float(window_->width()), h = float(window_->height());
    float x = e.x - m, y = e.y - m;
    switch (e.type) {
        case wl::PointerEvent::enter: input_.left_down = false; [[fallthrough]];
        case wl::PointerEvent::motion:
            input_.x = x, input_.y = y;
            input_.inside = x >= 0 && y >= 0 && x < w && y < h;
            resize_edges_ = input_.inside || input_.left_down ? 0 : edges_at(x, y, w, h);
            display_.input->set_cursor(resize_edges_ ? cursor_for(resize_edges_) : ui_.cursor);
            break;
        case wl::PointerEvent::leave:
            input_.inside = input_.left_down = false;
            resize_edges_ = 0;
            break;
        case wl::PointerEvent::press: {
            if (resize_edges_) {
                if (e.button == BTN_LEFT) window_->resize(resize_edges_);
                return;
            }
            bool dbl = e.button == last_press_button_ && e.time - last_press_time_ < 400 && std::abs(x - last_press_x_) < 4 &&
                       std::abs(y - last_press_y_) < 4;
            last_press_time_ = dbl ? 0 : e.time, last_press_button_ = e.button;
            last_press_x_ = x, last_press_y_ = y;
            input_.actions.push_back({x, y, e.button, true, dbl});
            if (e.button == BTN_LEFT) input_.left_down = true;
            break;
        }
        case wl::PointerEvent::release:
            if (e.button == BTN_LEFT) input_.left_down = false;
            input_.actions.push_back({x, y, e.button, false});
            break;
        case wl::PointerEvent::scroll:
            input_.scroll_x += e.dx, input_.scroll_y += e.dy;
            input_.wheel |= e.wheel, input_.scroll_stop |= e.stop;
            break;
    }
    note_input();
    request_redraw();
}

void App::on_key(const wl::KeyEvent& e) {
    if (!e.pressed) return;
    note_input();
    if (e.keysym == XKB_KEY_F12 && !e.repeat) {
        overlay_.visible = !overlay_.visible;
        loop_.cancel(overlay_timer_);
        overlay_timer_ = 0;
        if (overlay_.visible) sample_overlay();
    } else if ((e.mods & wl::mod_ctrl) && (e.keysym == XKB_KEY_q || e.keysym == XKB_KEY_Q)) {
        loop_.quit();
        return;
    } else {
        input_.keys.push_back(e);
    }
    request_redraw();
}

void App::on_text_input(const wl::TextInputEvent& e) {
    log::debug("text input: preedit \"{}\" cursor {}, commit \"{}\", delete {}/{}", e.preedit, e.preedit_cursor, e.commit, e.delete_before,
               e.delete_after);
    input_.preedit = e.preedit;
    input_.preedit_cursor = e.preedit_cursor;
    input_.commit += e.commit;
    input_.delete_before += e.delete_before;
    input_.delete_after += e.delete_after;
    note_input();
    request_redraw();
}

void App::sample_overlay() {
    auto m = mem_sample();
    auto mb = [](double kib) { return std::format("{:.1f} MB", kib / 1024.0); };
    std::vector<std::pair<std::string, std::string>> rows{
        {"RSS", mb(double(m.rss))},
        {"  anon / file / shmem", std::format("{:.1f} / {:.1f} / {:.1f}", m.anon / 1024.0, m.file / 1024.0, m.shmem / 1024.0)},
        {"GPU（DRM 常驻）", mb(double(m.gpu))},
        {"堆 已用 / 空闲", std::format("{:.1f} / {:.1f} MB", m.heap_used / 1024.0, m.heap_free / 1024.0)},
    };
    int i = 0;
    for (auto& heap : gpu_.heap_usage()) {
        if (heap.used)
            rows.emplace_back(std::format("VK 堆 {}{}", i, heap.device_local ? "（显存）" : ""),
                              std::format("{:.1f} / {:.0f} MB", heap.used / 1048576.0, heap.budget / 1048576.0));
        ++i;
    }
    rows.emplace_back("设备", gpu_.device_name);
    rows.emplace_back("字体映射", std::format("{:.1f} MB · {}/{} 个", fonts_->mapped_bytes() / 1048576.0, fonts_->opened_count(), fonts_->face_count()));
    rows.emplace_back("字形图集", std::format("{}² · {:.0f}% · {} 项", atlas_->size(), atlas_->fill() * 100, atlas_->entries()));
    rows.emplace_back("整形缓存", std::format("{} 段 · {} 字形", shaper_->cached(), shaper_->cached_glyphs()));
    rows.emplace_back("纹理", std::format("{} 个 · {:.1f} MB", renderer_.texture_count(), renderer_.texture_bytes() / 1048576.0));
    rows.emplace_back("  其中封面", std::format("{} 个 · {:.1f} MB", images_.count(), images_.bytes() / 1048576.0));
    rows.emplace_back("帧", std::format("{:.2f} ms · 第 {} 帧", frame_ms_, frames_drawn_));
    rows.emplace_back("缩放", std::format("{:.2f} · {}×{}", window_->scale(), window_->width(), window_->height()));
    overlay_.set(std::move(rows));
    overlay_timer_ = loop_.after(500ms, [this] {
        sample_overlay();
        request_redraw();
    });
}

static void write_png(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(bgra, bgra + size_t(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4)
        if (int a = px[i + 3]; a && a < 255)
            for (int c = 0; c < 3; ++c) px[i + c] = uint8_t(std::min(255, px[i + c] * 255 / a));
    png_image img{};
    img.version = PNG_IMAGE_VERSION, img.width = w, img.height = h, img.format = PNG_FORMAT_BGRA;
    if (!png_image_write_to_file(&img, path.c_str(), 0, px.data(), 0, nullptr)) log::warn("cannot write {}: {}", path, img.message);
}

void App::run_script(size_t i) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    float m = float(window_->margin());
    auto ms = [this] { return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count()); };
    while (i < script_.size()) {
        std::istringstream in(script_[i++]);
        std::string cmd, rest;
        in >> cmd;
        // "lyrics CMD …" sends a pointer command or a shot to the desktop lyrics instead of the window.
        bool to_lyrics = cmd == "lyrics";
        if (to_lyrics) in >> cmd;
        std::getline(in >> std::ws, rest);
        std::istringstream args(rest);
        if (cmd.empty() || cmd[0] == '#') continue;
        if (to_lyrics && !lyrics_) {
            log::warn("script line {}: desktop lyrics are off", i);
            continue;
        }
        auto pointer = [&](wl::PointerEvent e) {
            if (to_lyrics) return on_lyrics_pointer(e);
            e.x += m, e.y += m;
            on_pointer(e);
        };
        if (cmd == "wait") {
            int n = 0;
            args >> n;
            loop_.after(std::chrono::milliseconds(n), [this, i] { run_script(i); });
            return;
        }
        float x = 0, y = 0;
        std::string how;
        if (cmd == "move" || cmd == "click" || cmd == "press" || cmd == "release") args >> x >> y >> how;
        if (cmd == "move") {
            pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
        } else if (cmd == "click") {
            uint32_t button = how == "right" ? BTN_RIGHT : how == "back" ? BTN_SIDE : BTN_LEFT;
            pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
            for (int k = how == "double" ? 2 : 1; k > 0; --k) {
                pointer({.type = wl::PointerEvent::press, .x = x, .y = y, .button = button, .time = ms()});
                pointer({.type = wl::PointerEvent::release, .x = x, .y = y, .button = button, .time = ms()});
            }
        } else if (cmd == "press" || cmd == "release") {
            pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
            pointer({.type = cmd == "press" ? wl::PointerEvent::press : wl::PointerEvent::release, .x = x, .y = y, .button = BTN_LEFT, .time = ms()});
        } else if (cmd == "scroll") {
            float dy = 0;
            args >> dy >> how;
            on_pointer({.type = wl::PointerEvent::scroll, .x = input_.x + m, .y = input_.y + m, .dy = dy, .wheel = how == "wheel"});
        } else if (cmd == "key") {
            // e.g. "key ctrl+shift+Left"
            uint32_t mods = 0;
            for (size_t plus; (plus = rest.find('+')) != std::string::npos && plus > 0; rest.erase(0, plus + 1)) {
                std::string mod = rest.substr(0, plus);
                mods |= mod == "ctrl" ? wl::mod_ctrl : mod == "shift" ? wl::mod_shift : mod == "alt" ? wl::mod_alt : 0;
            }
            wl::KeyEvent e{.keysym = xkb_keysym_from_name(rest.c_str(), XKB_KEYSYM_NO_FLAGS), .mods = mods, .pressed = true};
            char buf[8];
            if (!(mods & wl::mod_ctrl) && xkb_keysym_to_utf8(e.keysym, buf, sizeof buf) > 1 && static_cast<unsigned char>(buf[0]) >= 0x20)
                e.text = buf;
            on_key(e);
        } else if (cmd == "commit" || cmd == "preedit") {
            wl::TextInputEvent e;
            (cmd == "commit" ? e.commit : e.preedit) = rest;
            e.preedit_cursor = cmd == "preedit" ? int(rest.size()) : -1;
            on_text_input(e);
        } else if (cmd == "shot") {
            renderer_.capture_next(to_lyrics ? *lyrics_target_ : *target_, [path = rest](const uint8_t* px, uint32_t w, uint32_t h) { write_png(path, px, w, h); });
            (to_lyrics ? lyrics_hash_ : frame_hash_) = 0;
            request_redraw();
        } else if (cmd == "exec") {
            pid_t pid;
            const char* argv[] = {"sh", "-c", rest.c_str(), nullptr};
            posix_spawnp(&pid, "sh", nullptr, nullptr, const_cast<char* const*>(argv), environ);
        } else if (cmd == "quit") {
            loop_.quit();
        } else {
            log::warn("script line {}: unknown command \"{}\"", i, cmd);
        }
    }
}

}  // namespace lyren
