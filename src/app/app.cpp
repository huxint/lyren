#include "app/app.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <format>
#include <fstream>

#include "core/log.hpp"
#include "core/memstat.hpp"
#include "text/icons.hpp"
#include "wl/input.hpp"

namespace lyren {

constexpr float titlebar_h = 40;
constexpr int shadow_margin = 28, resize_border = 8;

static ui::Rect button_rect(float window_w, int i) {
    constexpr float w = 36, h = 28, gap = 4;
    return {window_w - 8 - float(3 - i) * w - float(2 - i) * gap, (titlebar_h - h) / 2, w, h};
}

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

// glaze reports "line:col: code" followed by the offending line.
static std::string describe_config_error(const std::string& err) {
    auto nl = err.find('\n');
    std::string head = err.substr(0, nl), context;
    if (nl != std::string::npos) {
        auto end = err.find('\n', nl + 1);
        context = err.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1);
        context.erase(0, context.find_first_not_of(' '));
    }
    auto colon = head.find(':');
    std::string line = head.substr(0, colon);
    std::string code = head.substr(head.find(": ") == std::string::npos ? head.size() : head.find(": ") + 2);
    return std::format("配置第 {} 行有误（{}）：{}，已保留之前的设置", line, code, context);
}

App::App(Loop& loop, ConfigFile config, bool show_overlay)
    : loop_(loop),
      config_(std::move(config)),
      display_(loop),
      gpu_(display_.display),
      renderer_(gpu_),
      watcher_(loop, config_.path, [this] { reload_config(); }) {
    std::string errors;
    theme_ = ui::Theme::from(config_.config, errors);
    load_text(config_.config.theme.font);

    wl::Window::Callbacks cb{
        .configure =
            [this] {
                if (window_) request_redraw();
            },
        .close = [this] { loop_.quit(); },
        .frame =
            [this] {
                if (dirty_ && !scheduled_) {
                    scheduled_ = true;
                    loop_.defer([this] { render(); });
                }
            },
        .pointer = [this](const wl::PointerEvent& e) { on_pointer(e); },
        .key = [this](const wl::KeyEvent& e) { on_key(e); },
        .focus = [this](bool) { request_redraw(); },
    };
    window_ = std::make_unique<wl::Window>(display_, std::move(cb), config_.config.window.decorations == Decorations::client, 1100, 720);
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
}

App::~App() {
    renderer_.destroy_target(target_);
    loop_.cancel(toast_timer_);
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
    request_redraw();
}

void App::reload_config() {
    std::ifstream in(config_.path);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto parsed = parse_config(text);
    if (!parsed) {
        toast(describe_config_error(parsed.error()));
        return;
    }
    Config old = config_.config;
    config_.config = *parsed;
    log::info("config reloaded");
    if (parsed->theme.font != old.theme.font) load_text(parsed->theme.font);
    apply_config(false);
    if (parsed->gpu.device != old.gpu.device) toast("GPU 设置会在重启后生效");
}

void App::toast(std::string message) {
    log::warn("{}", message);
    toast_ = std::move(message);
    loop_.cancel(toast_timer_);
    toast_timer_ = loop_.after(6s, [this] {
        toast_.clear();
        toast_timer_ = 0;
        request_redraw();
    });
    request_redraw();
}

void App::request_redraw() {
    dirty_ = true;
    if (!scheduled_ && !window_->frame_pending()) {
        scheduled_ = true;
        loop_.defer([this] { render(); });
    }
}

void App::render() {
    scheduled_ = false;
    if (!dirty_ || window_->frame_pending()) return;
    dirty_ = false;
    auto start = Clock::now();
    renderer_.resize(*target_, window_->buffer_width(), window_->buffer_height());
    for (int pass = 0; pass < 2; ++pass) {
        atlas_->begin_frame();
        build(*painter_);
        if (!atlas_->reset()) break;
    }
    shaper_->end_frame();
    const float clear[4] = {0, 0, 0, 0};
    renderer_.draw(*target_, painter_->prims(), clear, [this] { window_->prepare_present(); });
    frame_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    // Once nothing has been drawn for a while, give back font pages and freed heap; glyphs live in the atlas.
    loop_.cancel(idle_timer_);
    idle_timer_ = loop_.after(5s, [this] {
        idle_timer_ = 0;
        fonts_->trim();
        mem_trim();
    });
    if (++frames_ == 1) {
        mem_trim();
        auto m = mem_sample();
        log::info("first frame in {:.1f} ms: RSS {:.1f} MB (anon {:.1f}, file {:.1f}), GPU {:.1f} MB", frame_ms_, m.rss / 1024.0, m.anon / 1024.0,
                  m.file / 1024.0, m.gpu / 1024.0);
    }
}

void App::build(ui::Painter& p) {
    float m = float(window_->margin()), w = float(window_->width()), h = float(window_->height());
    p.begin(float(window_->scale()), m, m);
    bool floating = window_->client_side() && m > 0;
    float radius = floating ? theme_.radius_l() : 0;
    if (floating) p.shadow({0, 6, w, h}, radius, 20, theme_.shadow, theme_.opacity < 1);
    p.rect({0, 0, w, h}, theme_.bg.with_alpha(theme_.opacity), radius, floating ? theme_.border : ui::Color{}, floating ? 1 : 0);

    float top = 0;
    if (window_->client_side()) {
        draw_titlebar(p);
        top = titlebar_h;
    }
    p.push_clip({0, top, w, h - top});
    draw_demo(p, {0, top, w, h - top});
    overlay_.draw(p, theme_, w - 12, top + 8);
    p.pop_clip();

    if (!toast_.empty()) {
        ui::TextStyle style{.size = theme_.fs(13), .color = theme_.text};
        float tw = std::min(p.measure(toast_, style), w - 120);
        ui::Rect r{(w - tw - 56) / 2, h - 72, tw + 56, 40};
        p.shadow(r, theme_.radius, 24, theme_.shadow);
        p.rect(r, theme_.elevated, theme_.radius, theme_.border, 1);
        p.icon("triangle-alert", {r.x + 14, r.y + 12, 16, 16}, theme_.danger);
        p.text_in({r.x + 40, r.y, tw, r.h}, toast_, style);
    }
}

void App::draw_titlebar(ui::Painter& p) {
    float w = float(window_->width());
    ui::Color fg = window_->activated() ? theme_.text : theme_.text_dim;
    p.icon("disc-3", {14, 10, 20, 20}, theme_.accent);
    p.text_in({42, 0, 200, titlebar_h}, "Lyren", {.size = theme_.fs(14), .color = fg});
    const char* icons[] = {"minus", window_->maximized() ? "copy" : "square", "x"};
    const Hit hits[] = {Hit::minimize, Hit::maximize, Hit::close};
    for (int i = 0; i < 3; ++i) {
        ui::Rect r = button_rect(w, i);
        bool hot = hover_ == hits[i], down = hot && pressed_ == hits[i];
        bool close = hits[i] == Hit::close;
        if (hot) p.rect(r, close ? theme_.danger.with_alpha(down ? 0.8f : 1) : down ? theme_.press : theme_.hover, theme_.radius_s());
        float s = i == 1 ? 14 : 16;
        p.icon(icons[i], {r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, s}, hot && close ? ui::Color{1, 1, 1, 1} : fg);
    }
}

void App::draw_demo(ui::Painter& p, ui::Rect a) {
    const ui::Color white{1, 1, 1, 1};
    float x = a.x + 32, y = a.y + 20;
    ui::TextStyle h1{.size = theme_.fs(28), .color = theme_.text};
    ui::TextStyle sub{.size = theme_.fs(16), .color = theme_.text_dim};
    ui::TextStyle body{.size = theme_.fs(14), .color = theme_.text};
    ui::TextStyle small{.size = theme_.fs(12), .color = theme_.text_dim, .features = text::tabular_nums};

    p.text(x, y + p.ascent(h1.size), "双源音乐播放器", h1);
    y += h1.size * 1.45f;
    p.text(x, y + p.ascent(sub.size), "网易云音乐 · QQ 音乐 —— 在一个窗口里搜索、播放、看歌词", sub, a.w - 64);
    y += sub.size * 2.4f;

    ui::Rect card{x, y, std::min(a.w - 64, 580.f), 120};
    p.shadow({card.x, card.y + 4, card.w, card.h}, theme_.radius, 24, theme_.shadow.with_alpha(0.7f));
    p.rect(card, theme_.surface, theme_.radius, theme_.border, 1);
    ui::Rect cover{card.x + 16, card.y + 16, 88, 88};
    p.gradient(cover, theme_.accent, theme_.accent.mix(theme_.surface, 0.65f), theme_.radius_s());
    p.icon("music", {cover.x + 28, cover.y + 28, 32, 32}, white.with_alpha(0.9f));
    float tx = cover.right() + 16, tw = card.right() - 16 - tx;
    p.text(tx, card.y + 18 + p.ascent(theme_.fs(16)), "晴天", {.size = theme_.fs(16), .color = theme_.text}, tw - 140);
    p.text(tx, card.y + 44 + p.ascent(theme_.fs(13)), "周杰伦 · 叶惠美", {.size = theme_.fs(13), .color = theme_.text_dim}, tw - 140);
    const char* controls[] = {"shuffle", "skip-back", "pause", "skip-forward", "repeat"};
    for (int i = 0; i < 5; ++i) {
        ui::Rect r{card.right() - 16 - float(5 - i) * 28, card.y + 18, 20, 20};
        p.icon(controls[i], r, i == 2 ? theme_.text : theme_.text_dim);
    }
    ui::Rect bar{tx, card.y + 80, tw, 4};
    p.rect(bar, theme_.text.with_alpha(0.12f), 2);
    p.rect({bar.x, bar.y, bar.w * 0.31f, bar.h}, theme_.accent, 2);
    p.text(tx, bar.bottom() + 8 + p.ascent(small.size), "01:23", small);
    float dw = p.measure("04:29", small);
    p.text(bar.right() - dw, bar.bottom() + 8 + p.ascent(small.size), "04:29", small);
    y = card.bottom() + 32;

    p.text(x, y + p.ascent(body.size), "混排：English · 日本語のかな · 한국어 · Ελληνικά · Русский · 0123456789 · ½ → ✓", body, a.w - 64);
    y += body.size * 2;
    ui::Rect narrow{x, y, 260, 32};
    p.rect(narrow, theme_.surface, theme_.radius_s(), theme_.border, 1);
    p.text_in(narrow.inset(12, 0), "很长的歌名会在这里被截断并显示省略号 (Long Title Ellipsis)", body);
    y += 56;

    float gx = x;
    for (std::size_t i = 0; i < text::icon_source_count; ++i) {
        if (gx + 20 > a.right() - 32) {
            gx = x;
            y += 32;
        }
        p.icon(text::icon_sources[i].name, {gx, y, 20, 20}, theme_.text_dim);
        gx += 32;
    }
}

App::Hit App::hit_test(float x, float y, uint32_t& edges) const {
    float w = float(window_->width()), h = float(window_->height());
    edges = 0;
    if (x < 0 || y < 0 || x >= w || y >= h) {
        if (!window_->margin()) return Hit::none;
        constexpr float corner = 16;
        bool l = x < 0, r = x >= w, t = y < 0, b = y >= h;
        if (t || b) l |= x < corner, r |= x >= w - corner;
        if (l || r) t |= y < corner, b |= y >= h - corner;
        edges = (t ? XDG_TOPLEVEL_RESIZE_EDGE_TOP : 0) | (b ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM : 0) | (l ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT : 0) |
                (r ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT : 0);
        return Hit::resize;
    }
    if (!window_->client_side() || y >= titlebar_h) return Hit::client;
    const Hit hits[] = {Hit::minimize, Hit::maximize, Hit::close};
    for (int i = 0; i < 3; ++i)
        if (button_rect(w, i).contains(x, y)) return hits[i];
    return Hit::caption;
}

void App::on_pointer(const wl::PointerEvent& e) {
    float m = float(window_->margin());
    mouse_x_ = e.x - m, mouse_y_ = e.y - m;
    uint32_t edges = 0;
    Hit hit = hit_test(mouse_x_, mouse_y_, edges);
    switch (e.type) {
        case wl::PointerEvent::enter:
        case wl::PointerEvent::motion:
            display_.input->set_cursor(hit == Hit::resize ? cursor_for(edges) : wl::Cursor::arrow);
            if (hit != hover_) {
                hover_ = hit;
                request_redraw();
            }
            break;
        case wl::PointerEvent::leave:
            hover_ = pressed_ = Hit::none;
            request_redraw();
            break;
        case wl::PointerEvent::press:
            if (e.button == BTN_LEFT) {
                if (hit == Hit::resize) {
                    window_->resize(edges);
                } else if (hit == Hit::caption) {
                    if (last_click_ && e.time - last_click_ < 400) {
                        window_->toggle_maximize();
                        last_click_ = 0;
                    } else {
                        last_click_ = e.time;
                        window_->move();
                    }
                } else if (hit == Hit::minimize || hit == Hit::maximize || hit == Hit::close) {
                    pressed_ = hit;
                    request_redraw();
                }
            } else if (e.button == BTN_RIGHT && hit == Hit::caption) {
                window_->show_menu(int(e.x), int(e.y));
            }
            break;
        case wl::PointerEvent::release:
            if (e.button == BTN_LEFT && pressed_ != Hit::none) {
                if (pressed_ == hit) {
                    if (hit == Hit::minimize) window_->minimize();
                    else if (hit == Hit::maximize) window_->toggle_maximize();
                    else loop_.quit();
                }
                pressed_ = Hit::none;
                request_redraw();
            }
            break;
        case wl::PointerEvent::scroll: break;
    }
}

void App::on_key(const wl::KeyEvent& e) {
    if (!e.pressed) return;
    if (e.keysym == XKB_KEY_F12 && !e.repeat) {
        overlay_.visible = !overlay_.visible;
        loop_.cancel(overlay_timer_);
        overlay_timer_ = 0;
        if (overlay_.visible) sample_overlay();
        request_redraw();
    } else if ((e.mods & wl::mod_ctrl) && (e.keysym == XKB_KEY_q || e.keysym == XKB_KEY_Q)) {
        loop_.quit();
    }
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
    rows.emplace_back("帧", std::format("{:.2f} ms · 第 {} 帧", frame_ms_, frames_));
    rows.emplace_back("缩放", std::format("{:.2f} · {}×{}", window_->scale(), window_->width(), window_->height()));
    overlay_.set(std::move(rows));
    overlay_timer_ = loop_.after(500ms, [this] {
        sample_overlay();
        request_redraw();
    });
}

}  // namespace lyren
