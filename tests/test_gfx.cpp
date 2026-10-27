#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include "app/images.hpp"
#include "text/atlas.hpp"
#include "ui/ui.hpp"
#include "wl/display.hpp"
#include "wl/window.hpp"

using namespace lyren;

namespace {

struct Graphics {
    struct Validation {
        const gfx::Context& context;
        ~Validation() { CHECK(context.validation_errors() == 0); }
    };

    Loop loop;
    wl::Display display{loop};
    gfx::Context context{display.display};
    Validation validation{context};
    gfx::Renderer renderer{context};
};

}  // namespace

TEST_CASE("scroll motion requests paced animation frames after consuming input") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    text::Fonts fonts("sans-serif");
    text::Shaper shaper(fonts);
    text::Atlas atlas(g.renderer, fonts);
    ui::Painter painter(atlas, shaper, fonts);
    ui::Theme theme;
    ui::Ui ui;
    ui::FrameInput input;
    input.inside = true;
    input.x = input.y = 50;
    auto frame = [&](double now) {
        painter.begin(1, 0, 0);
        ui.begin(painter, theme, 100, 100, now, input);
        float offset = ui.begin_scroll(ui.id("scroll"), {0, 0, 100, 100}, 2000);
        ui.end_scroll();
        ui.end();
        input.consumed();
        return offset;
    };

    SUBCASE("wheel") {
        input.scroll_y = 400;
        input.wheel = true;
        frame(1);
        frame(1.016);
        CHECK(ui.animating());
        CHECK_FALSE(ui.needs_redraw());
    }
    SUBCASE("touchpad coast") {
        input.scroll_y = 40;
        frame(1);
        input.scroll_stop = true;
        frame(1.016);
        frame(1.032);
        CHECK(ui.animating());
        CHECK_FALSE(ui.needs_redraw());
    }
    SUBCASE("wheel catches up after a long frame gap") {
        input.scroll_y = 400;
        input.wheel = true;
        frame(1);
        CHECK(frame(2) == doctest::Approx(400));
    }
    SUBCASE("coast distance is independent of frame rate") {
        for (int fps : {15, 60, 144}) {
            CAPTURE(fps);
            ui = ui::Ui{};
            input = ui::FrameInput{.x = 50, .y = 50, .inside = true, .scroll_y = 40};
            frame(1);
            input.scroll_stop = true;
            frame(1.016);
            int count = int(std::lround(0.4 * fps));
            float offset = 0;
            for (int i = 1; i <= count; ++i) offset = frame(1.016 + 0.4 * i / count);
            CHECK(offset == doctest::Approx(40 + 1200 * 0.35 * (1 - std::exp(-0.4 / 0.35))).epsilon(1e-5));
        }
    }
}

TEST_CASE("small cover textures stay bounded and preserve the current cover") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    Images images(g.loop, http, g.renderer, [] {}, [] { return 0.; });
    gfx::Image image{64, 64, std::vector<uint8_t>(64 * 64 * 4, 255)};
    for (int i = 0; i < 1600; ++i) {
        images.put("cover-" + std::to_string(i), image);
        images.end_frame();
    }
    CHECK(images.count() <= 512);
    CHECK(g.renderer.texture_count() <= 512);
    CHECK(images.bytes() <= 512 * image.px.size());
    const auto& current = images.get("cover-1599", 64);
    CHECK(current.ready);
    CHECK(current.bytes == image.px.size());
}

TEST_CASE("texture replacement preserves the uploaded pixels across submissions") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    bool configured = false;
    wl::Window window(g.display,
                      {.configure = [&] { configured = true; g.loop.quit(); },
                       .pointer = [](const wl::PointerEvent&) {},
                       .key = [](const wl::KeyEvent&) {},
                       .focus = [](bool) {},
                       .text_input = [](const wl::TextInputEvent&) {}},
                      true, 720, 480);
    auto timeout = g.loop.after(2s, [&] { g.loop.quit(); });
    g.loop.run();
    g.loop.cancel(timeout);
    REQUIRE(configured);
    auto* target = g.renderer.create_target(g.display.display, window.surface());
    g.renderer.resize(*target, window.buffer_width(), window.buffer_height());
    gfx::Prim prim{};
    prim.rect[0] = prim.rect[1] = 10;
    prim.rect[2] = prim.rect[3] = 8;
    prim.clip[2] = float(window.buffer_width());
    prim.clip[3] = float(window.buffer_height());
    prim.uv[2] = prim.uv[3] = 1;
    std::fill_n(prim.color, 4, 1);
    prim.info[0] = uint32_t(gfx::Kind::image);
    const float clear[4] = {};
    gfx::TextureId tex = 0;
    for (const std::array<uint8_t, 4> rgba : {std::array<uint8_t, 4>{255, 0, 0, 255}, {0, 255, 0, 255}}) {
        tex = g.renderer.create_texture(1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        g.renderer.upload(tex, 0, 0, 1, 1, rgba.data(), 4);
        prim.info[1] = tex;
        bool captured = false;
        g.renderer.capture_next(*target, [&](const uint8_t* bgra, uint32_t width, uint32_t) {
            const auto* pixel = bgra + (14 * width + 14) * 4;
            CHECK(pixel[0] == rgba[2]);
            CHECK(pixel[1] == rgba[1]);
            CHECK(pixel[2] == rgba[0]);
            CHECK(pixel[3] == rgba[3]);
            captured = true;
        });
        g.renderer.draw(*target, {&prim, 1}, clear, [&] { window.prepare_present(); });
        CHECK(captured);
        g.renderer.destroy_texture(tex);
    }
    g.renderer.destroy_target(target);
}
