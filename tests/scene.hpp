#pragma once
#include <doctest/doctest.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/images.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"
#include "sys/bus.hpp"
#include "service.hpp"
#include "language_guard.hpp"
#include "text/atlas.hpp"
#include "ui/ui.hpp"
#include "wl/display.hpp"

// The window's content without a window: the shell over a music service that answers at once, drawn a frame at a time,
// with what each frame drew to check its layout by.
namespace lyren::test {

    struct Graphics {
        struct Validation {
            const gfx::Context &context;
            ~Validation() {
                CHECK(context.validation_errors() == 0);
            }
        };

        Loop loop;
        wl::Display display{loop};
        gfx::Context context{display.display};
        Validation validation{context};
        gfx::Renderer renderer{context};
    };

    struct SceneOptions {
        Language language = Language::en;
        bool signed_in = true;
        bool dark = true;
        double motion = 0;
        float font_size = 0;
        float radius = -1;
        std::vector<Track> queue;
        int current = -1;
    };

    class Scene {
    public:
        using Options = SceneOptions;
        static constexpr float titlebar = 48;

        explicit Scene(Options options = {})
        : images_(
              g.loop, http_, g.renderer, [] {},
              [this] {
                  return now;
              }),
          config_(configure(options)),
          shell(deps(options), saved(options)) {
            std::string errors;
            theme_ = ui::Theme::from(config_, errors);
            stage_ = theme_.stage(theme_.accent);
            REQUIRE(errors.empty());
            painter.record(true);
            if (options.signed_in) {
                spawn(shell.library.load(Source::netease));
            }
        }

        // A frame of a window w by h, a second after the one before; returns what it drew.
        std::span<const ui::Painter::Run> draw(float w = 1180, float h = 760) {
            return draw(w, h, pointer_, 1.0);
        }
        // A frame advanced by dt seconds, for testing in-flight transitions when motion > 0.
        std::span<const ui::Painter::Run> draw_dt(float w, float h, double dt) {
            return draw(w, h, pointer_, dt);
        }
        // The pointer comes to rest at (x, y).
        void point(float x, float y) {
            pointer_ = {.x = x, .y = y, .inside = true};
        }
        // A press and release at (x, y), then a frame for what it set off to show.
        std::span<const ui::Painter::Run> click(float x, float y, float w = 1180, float h = 760, uint32_t button = BTN_LEFT) {
            ui::FrameInput input{.x = x, .y = y, .inside = true};
            input.actions = {{.x = x, .y = y, .button = button, .press = true}, {.x = x, .y = y, .button = button, .press = false}};
            draw(w, h, input);
            point(x, y);
            return draw(w, h);
        }
        // Half a click at (x, y): the button goes down, or comes up, and a frame is drawn.
        std::span<const ui::Painter::Run> button(float x, float y, bool press, float w = 1180, float h = 760) {
            pointer_ = {.x = x, .y = y, .inside = true, .left_down = press};
            pointer_.actions = {{.x = x, .y = y, .button = BTN_LEFT, .press = press}};
            return draw(w, h, pointer_);
        }
        // Several clicks batched into a single frame, then a frame for what they set off to show.
        std::span<const ui::Painter::Run> clicks(std::initializer_list<std::pair<float, float>> points, float w = 1180, float h = 760) {
            ui::FrameInput input{.inside = true};
            for (const auto &[x, y] : points) {
                input.x = x;
                input.y = y;
                input.actions.push_back({.x = x, .y = y, .button = BTN_LEFT, .press = true});
                input.actions.push_back({.x = x, .y = y, .button = BTN_LEFT, .press = false});
            }
            draw(w, h, input);
            point(input.x, input.y);
            return draw(w, h);
        }
        std::span<const ui::Painter::Run> click(const ui::Painter::Run &run, float w = 1180, float h = 760) {
            return click(run.box.x + run.box.w / 2, run.box.y + run.box.h / 2, w, h);
        }
        // A key press delivered to the window, followed by a frame for the resulting state to show.
        std::span<const ui::Painter::Run> key(uint32_t keysym, uint32_t mods = 0, float w = 1180, float h = 760) {
            ui::FrameInput input = pointer_;
            input.mods = mods;
            input.keys.push_back({.keysym = keysym, .mods = mods, .pressed = true, .repeat = false});
            draw(w, h, input);
            return draw(w, h);
        }
        // The first thing drawn last frame with this text, or this icon's name.
        const ui::Painter::Run *find(std::string_view text, bool icon = false) const {
            const auto runs = painter.runs();
            const auto at = std::ranges::find_if(runs, [&](const ui::Painter::Run &run) {
                return run.icon == icon && run.text == text;
            });
            return at == runs.end() ? nullptr : &*at;
        }

        // With LYREN_TRACE_DIR set, writes what the last frame drew to <dir>/<name>.txt, one primitive, text run or
        // icon to a line with its box: the layout to read when a picture of it cannot be looked at.
        void trace(std::string_view name) const {
            const char *dir = std::getenv("LYREN_TRACE_DIR");
            if (!dir) {
                return;
            }
            std::ofstream out(std::format("{}/{}.txt", dir, name));
            static constexpr std::array<std::string_view, 6> kinds{"rect", "shadow", "glyph", "image", "arc", "wave"};
            for (const gfx::Prim &prim : painter.prims()) {
                const auto kind = static_cast<gfx::Kind>(prim.info[0]);
                if (kind == gfx::Kind::glyph) {
                    continue;
                }
                const float x0 = std::max(prim.rect[0], prim.clip[0]), y0 = std::max(prim.rect[1], prim.clip[1]);
                const float x1 = std::min(prim.rect[0] + prim.rect[2], prim.clip[2]), y1 = std::min(prim.rect[1] + prim.rect[3], prim.clip[3]);
                if (x1 <= x0 || y1 <= y0) {
                    continue;
                }
                const auto &c = prim.color, &d = prim.color2;
                const float a = c[3] > 0 ? c[3] : 1, b = d[3] > 0 ? d[3] : 1;
                out << std::format("{} {:.1f} {:.1f} {:.1f} {:.1f} rgba({:.2f},{:.2f},{:.2f},{:.2f}) rgba2({:.2f},{:.2f},{:.2f},{:.2f}) r={:.1f} f={}\n",
                                   kinds[static_cast<size_t>(prim.info[0])], x0, y0, x1 - x0, y1 - y0, c[0] / a, c[1] / a, c[2] / a, c[3], d[0] / b, d[1] / b, d[2] / b, d[3],
                                   prim.radii[0], prim.info[2]);
            }
            for (const ui::Painter::Run &run : painter.runs()) {
                out << std::format("{} {:.1f} {:.1f} {:.1f} {:.1f} a={:.2f}{} \"{}\"\n", run.icon ? "icon" : "text", run.box.x, run.box.y, run.box.w, run.box.h, run.alpha,
                                   run.cut ? " CUT" : "", run.text);
            }
        }

        Graphics g;
        text::Fonts fonts{"sans-serif"};
        text::Shaper shaper{fonts};
        text::Atlas atlas{g.renderer, fonts};
        ui::Painter painter{atlas, shaper, fonts};
        Service service;
        double now = 1;

    private:
        static Config configure(const Options &options) {
            Config config;
            config.theme.motion = options.motion;
            config.theme.mode = options.dark ? ThemeMode::dark : ThemeMode::light;
            if (options.font_size > 0) {
                config.theme.font_size = options.font_size;
            }
            if (options.radius >= 0) {
                config.theme.radius = options.radius;
            }
            config.interface.language = options.language;
            return config;
        }
        ShellDeps deps(const Options &options) {
            service.signed_in = options.signed_in;
            return {.loop = g.loop,
                    .http = http_,
                    .player = player_,
                    .providers = {service, service, service, service, service},
                    .images = images_,
                    .config = config_,
                    .set_settings = [](std::span<const SettingChange>) {},
                    .desktop_changed = [] {},
                    .open_config = [] {},
                    .redraw = [] {},
                    .now =
                        [this] {
                            return now;
                        },
                    .silent = true,
                    .bus = bus_,
                    .raise = [](std::string_view) {}};
        }
        static SavedState saved(const Options &options) {
            SavedState state;
            state.queue = options.queue;
            state.current = options.current;
            return state;
        }
        std::span<const ui::Painter::Run> draw(float w, float h, ui::FrameInput &input, double dt = 1.0) {
            now += dt;
            painter.begin(1, 0, 0);
            ui.begin(painter, theme_, w, h, now, input);
            ui::Rect area{0, 0, w, h};
            ui::Rect bar = ui::cut_top(area, titlebar);
            // As the app does: the title bar takes input first and is drawn over the page.
            painter.defer(true);
            ui::cut_left(bar, 8);
            shell.chrome.top_bar(ui, bar, w / 2);
            painter.defer(false);
            shell.draw(ui, area, stage_);
            painter.draw_deferred();
            ui.end();
            input.consumed();
            return painter.runs();
        }

        LanguageGuard language_guard_;
        net::Http http_;
        audio::Player player_{g.loop, http_};
        Images images_;
        Bus bus_{g.loop};
        Config config_;
        ui::Theme theme_, stage_;
        ui::FrameInput pointer_;

    public:
        Shell shell;
        ui::Ui ui;
    };

} // namespace lyren::test
