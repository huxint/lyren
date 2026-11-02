#include <doctest/doctest.h>
#include <png.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>

#include "app/images.hpp"
#include "text/atlas.hpp"
#include "ui/ui.hpp"
#include "wl/display.hpp"
#include "wl/window.hpp"
#include "http_server.hpp"

using namespace lyren;

namespace {

    std::string_view cover_body() {
        static constexpr unsigned char data[]{
#embed "data/cover.webp"
        };
        return {reinterpret_cast<const char *>(data), sizeof(data)};
    }

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

} // namespace

TEST_CASE("pointer chords keep press modifiers after keys are released before a frame") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    text::Fonts fonts("sans-serif");
    text::Shaper shaper(fonts);
    text::Atlas atlas(g.renderer, fonts);
    ui::Painter painter(atlas, shaper, fonts);
    ui::Theme theme;
    for (uint32_t mods : {wl::mod_ctrl, wl::mod_shift, wl::mod_ctrl | wl::mod_shift}) {
        ui::Ui ui;
        ui::FrameInput input{.x = 20, .y = 20, .inside = true};
        input.actions.push_back({.x = 20, .y = 20, .button = BTN_LEFT, .press = true, .mods = mods});
        input.actions.push_back({.x = 20, .y = 20, .button = BTN_LEFT, .press = false});
        painter.begin(1, 0, 0);
        ui.begin(painter, theme, 100, 100, 1, input);
        auto press = ui.interact(ui.id("row"), {0, 0, 80, 40});
        CHECK(press.pressed);
        CHECK(press.clicked);
        CHECK(press.mods == mods);
        CHECK(input.mods == 0);
        ui.end();
    }
}

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
    SUBCASE("layout anchoring preserves a wheel glide") {
        input.scroll_y = 400;
        input.wheel = true;
        frame(1);
        const float before = frame(1.016);
        ui.scroll_by(ui.id("scroll"), 300);
        CHECK(frame(1.032) > before + 300);
        CHECK(ui.animating());
        CHECK(frame(2) == doctest::Approx(700));
    }
    SUBCASE("coast distance is independent of frame rate") {
        std::optional<float> reference;
        for (int fps : {15, 60, 144}) {
            CAPTURE(fps);
            ui = ui::Ui{};
            input = ui::FrameInput{.x = 50, .y = 50, .inside = true, .scroll_y = 40};
            frame(1);
            input.scroll_stop = true;
            frame(1.016);
            int count = static_cast<int>(std::lround(0.4 * fps));
            float offset = 0;
            for (int i = 1; i <= count; ++i) {
                offset = frame(1.016 + 0.4 * i / count);
            }
            CHECK(offset > 40);
            if (reference) {
                CHECK(offset == doctest::Approx(*reference).epsilon(1e-5));
            } else {
                reference = offset;
            }
        }
    }
}

TEST_CASE("cover eviction discards old covers and preserves visible covers") {
    uint32_t side = 64;
    int count = 640;
    SUBCASE("small covers reach the entry limit") {}
    SUBCASE("large covers reach the byte budget") {
        side = 512;
        count = 40;
    }
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    Images images(
        g.loop, http, g.renderer, [] {},
        [] {
            return 0.;
        });
    gfx::Image image{side, side, std::vector<uint8_t>(static_cast<size_t>(side) * side * 4, 255)};
    images.put("visible", side, image);
    auto visible = images.get("visible", side).tex;
    for (int i = 0; i < count; ++i) {
        images.put("cover-" + std::to_string(i), side, image);
        images.get("visible", side);
        images.end_frame();
    }
    CHECK_FALSE(images.get("cover-0", side).ready);
    CHECK(images.get("cover-" + std::to_string(count - 1), side).ready);
    CHECK(images.get("visible", side).ready);
    CHECK(images.get("visible", side).tex == visible);
}

TEST_CASE("cover preloads finish without being drawn and survive page switches") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    int completed = 0;
    Images images(
        g.loop, http, g.renderer,
        [&] {
            if (++completed == 4) {
                g.loop.quit();
            }
        },
        [] {
            return 1.;
        });
    std::atomic<int> requests{0};
    test::Server server([&](std::string_view) {
        ++requests;
        g.loop.post([&] {
            for (int i = 0; i < 20; ++i) {
                images.end_frame();
            }
        });
        return test::ok(cover_body());
    });
    // libcurl's scheme-less loopback URLs keep this HTTP fixture out of the CDN's HTTPS normalization.
    const std::string url = server.url().substr(7);
    for (int i = 0; i < 4; ++i) {
        images.prefetch(url + '/' + std::to_string(i), 64);
        images.prefetch(url + '/' + std::to_string(i), 64);
    }
    for (int i = 0; i < 20; ++i) {
        images.end_frame();
    }
    auto timeout = g.loop.after(3s, [&] {
        g.loop.quit();
    });
    g.loop.run();
    g.loop.cancel(timeout);
    REQUIRE(completed == 4);
    CHECK(requests == 4);
    for (int i = 0; i < 4; ++i) {
        const auto &image = images.get(url + '/' + std::to_string(i), 64);
        CHECK(image.ready);
        CHECK_FALSE(image.failed);
        CHECK(image.bytes == 16);
    }
}

TEST_CASE("visible covers overtake preloads and promotion does not duplicate a download") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    std::mutex mutex;
    std::vector<std::string> requests;
    test::Server server([&](std::string_view request) {
        std::lock_guard lock(mutex);
        requests.emplace_back(request.substr(0, request.find('\r')));
        return test::ok(cover_body());
    });
    int completed = 0;
    Images images(
        g.loop, http, g.renderer,
        [&] {
            if (++completed == 5) {
                g.loop.quit();
            }
        },
        [] {
            return 1.;
        });
    const std::string url = server.url().substr(7);
    for (int i = 0; i < 4; ++i) {
        images.prefetch(url + '/' + std::to_string(i), 64);
    }
    images.prefetch(url + "/visible", 64);
    images.get(url + "/visible", 64);
    images.get(url + "/visible", 64);
    auto timeout = g.loop.after(3s, [&] {
        g.loop.quit();
    });
    g.loop.run();
    g.loop.cancel(timeout);
    REQUIRE(completed == 5);
    std::lock_guard lock(mutex);
    REQUIRE(requests.size() == 5);
    const auto visible = [](std::string_view request) {
        return request.starts_with("GET /visible ");
    };
    CHECK(std::ranges::count_if(requests, visible) == 1);
    CHECK(std::ranges::any_of(std::span(requests).first(3), visible));
}

TEST_CASE("stationary cover preloads settle under cache pressure and visible requests still load") {
    uint32_t side = 512;
    int count = 40;
    SUBCASE("pixel budget") {}
    SUBCASE("entry budget") {
        side = 64;
        count = 640;
    }
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = png.height = side;
    png.format = PNG_FORMAT_RGBA;
    std::vector<uint8_t> pixels(static_cast<size_t>(side) * side * 4, 255);
    png_alloc_size_t length = 0;
    REQUIRE(png_image_write_to_memory(&png, nullptr, &length, 0, pixels.data(), 0, nullptr));
    std::string body(length, '\0');
    REQUIRE(png_image_write_to_memory(&png, body.data(), &length, 0, pixels.data(), 0, nullptr));
    body.resize(length);
    std::atomic<int> requests{0};
    test::Server server([&](std::string_view) {
        ++requests;
        return test::ok(body);
    });
    std::vector<std::string> covers;
    for (int i = 0; i < count; ++i) {
        covers.push_back(server.url().substr(7) + '/' + std::to_string(i));
    }

    constexpr int visible = 12;
    int first = 0, completed = 0;
    bool scheduled = false, settled = false;
    Loop::TimerId idle = 0;
    std::function<void()> draw;
    Images images(
        g.loop, http, g.renderer,
        [&] {
            if (++completed > count) {
                g.loop.quit();
                return;
            }
            if (!scheduled) {
                scheduled = true;
                g.loop.defer([&] {
                    scheduled = false;
                    draw();
                });
            }
            g.loop.cancel(idle);
            idle = g.loop.after(100ms, [&] {
                for (int i = first; i < first + visible; ++i) {
                    if (!images.get(covers[i], side).ready) {
                        return;
                    }
                }
                settled = true;
                g.loop.quit();
            });
        },
        [] {
            return 0.;
        });
    draw = [&] {
        for (int i = first; i < first + visible; ++i) {
            images.get(covers[i], side);
        }
        for (int i = 0; i < count; ++i) {
            if (i < first || i >= first + visible) {
                images.prefetch(covers[i], side);
            }
        }
        images.end_frame();
    };
    auto settle = [&] {
        completed = 0;
        settled = false;
        const int before = requests.load();
        auto timeout = g.loop.after(3s, [&] {
            g.loop.quit();
        });
        draw();
        g.loop.run();
        g.loop.cancel(timeout);
        g.loop.cancel(idle);
        REQUIRE(settled);
        CHECK(requests.load() - before <= count);
        for (int i = first; i < first + visible; ++i) {
            CHECK(images.get(covers[i], side).ready);
        }
    };
    settle();
    CHECK(requests.load() > visible);
    CHECK(requests.load() < count);
    first = count - visible;
    settle();
}

TEST_CASE("cached covers fill other sizes while sharp and blurred versions remain separate") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    Images images(
        g.loop, http, g.renderer, [] {},
        [] {
            return 0.;
        });
    gfx::Image image{128, 128, std::vector<uint8_t>(128 * 128 * 4, 255)};
    images.put("cover", 128, image);
    const auto texture = images.get("cover", 128).tex;
    CHECK(images.get("cover", 64).tex == texture);
    CHECK(images.get("cover", 256).tex == texture);
    CHECK_FALSE(images.get("cover", 128, true).ready);
    images.erase("cover");
    CHECK_FALSE(images.get("cover", 128).ready);
}

TEST_CASE("destroying a cover cache with pending downloads leaves the HTTP loop usable") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    net::Http http;
    int redraws = 0;
    bool ping = false;
    auto images = std::make_unique<Images>(
        g.loop, http, g.renderer,
        [&] {
            ++redraws;
        },
        [] {
            return 0.;
        });
    test::Server server([&](std::string_view request) {
        if (request.starts_with("GET /cover ")) {
            g.loop.post([&] {
                images.reset();
            });
            return test::ok(cover_body());
        }
        return test::ok("pong");
    });
    images->get(server.url().substr(7) + "/cover", 64);
    auto timeout = g.loop.after(3s, [&] {
        g.loop.quit();
    });
    g.loop.after(50ms, [&] {
        http.request({.url = server.url() + "/ping"}, [&](net::Response response) {
            g.loop.post([&, response = std::move(response)] {
                ping = response.body == "pong";
                g.loop.quit();
            });
        });
    });
    g.loop.run();
    g.loop.cancel(timeout);
    CHECK_FALSE(images);
    CHECK(redraws == 0);
    CHECK(ping);
}

TEST_CASE("texture replacement preserves the uploaded pixels across submissions") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    bool configured = false;
    wl::Window window(g.display,
                      {.configure =
                           [&] {
                               configured = true;
                               g.loop.quit();
                           },
                       .pointer = [](const wl::PointerEvent &) {},
                       .key = [](const wl::KeyEvent &) {},
                       .focus = [](bool) {},
                       .text_input = [](const wl::TextInputEvent &) {}},
                      720, 480);
    auto timeout = g.loop.after(2s, [&] {
        g.loop.quit();
    });
    g.loop.run();
    g.loop.cancel(timeout);
    REQUIRE(configured);
    auto *target = g.renderer.create_target(g.display.display, window.surface());
    g.renderer.resize(*target, window.buffer_width(), window.buffer_height());
    gfx::Prim prim{};
    prim.rect[0] = prim.rect[1] = 10;
    prim.rect[2] = prim.rect[3] = 8;
    prim.clip[2] = static_cast<float>(window.buffer_width());
    prim.clip[3] = static_cast<float>(window.buffer_height());
    prim.uv[2] = prim.uv[3] = 1;
    prim.color.fill(1);
    prim.info[0] = static_cast<uint32_t>(gfx::Kind::image);
    gfx::TextureId tex = 0;
    for (const std::array<uint8_t, 4> rgba : {std::array<uint8_t, 4>{255, 0, 0, 255}, {0, 255, 0, 255}}) {
        tex = g.renderer.create_texture(1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        g.renderer.upload(tex, 0, 0, 1, 1, rgba.data(), 4);
        prim.info[1] = tex;
        bool captured = false;
        g.renderer.capture_next(*target, [&](const uint8_t *bgra, uint32_t width, uint32_t) {
            const auto *pixel = bgra + (14 * width + 14) * 4;
            CHECK(pixel[0] == rgba[2]);
            CHECK(pixel[1] == rgba[1]);
            CHECK(pixel[2] == rgba[0]);
            CHECK(pixel[3] == rgba[3]);
            captured = true;
        });
        g.renderer.draw(*target, {&prim, 1}, {}, [&] {
            window.prepare_present();
        });
        CHECK(captured);
        g.renderer.destroy_texture(tex);
    }
    g.renderer.destroy_target(target);
}

TEST_CASE("emoji are drawn in colour from their own sheet") {
    gfx::select_driver(GpuChoice::integrated);
    Graphics g;
    text::Fonts fonts("sans-serif");
    text::Shaper shaper(fonts);
    const text::Shaped &shaped = shaper.shape("a😀");
    REQUIRE(shaped.glyphs.size() == 2);
    CHECK_FALSE(fonts.face(shaped.glyphs[0].face).color);
    if (!fonts.face(shaped.glyphs[1].face).color) {
        MESSAGE("no colour emoji font installed");
        return;
    }
    text::Atlas atlas(g.renderer, fonts);
    ui::Painter painter(atlas, shaper, fonts);
    painter.begin(1, 0, 0);
    painter.text(10, 50, "😀", {.size = 32, .color = {1, 1, 1, 1}});
    REQUIRE(painter.prims().size() == 1);
    const gfx::Prim emoji = painter.prims()[0];
    CHECK(emoji.info[0] == static_cast<uint32_t>(gfx::Kind::image));
    CHECK(emoji.info[1] == atlas.color_texture());
    CHECK(emoji.rect[2] == doctest::Approx(emoji.rect[3]).epsilon(0.2));
    CHECK(emoji.rect[3] >= 28);
    CHECK(emoji.rect[3] <= 44);

    bool configured = false;
    wl::Window window(g.display,
                      {.configure =
                           [&] {
                               configured = true;
                               g.loop.quit();
                           },
                       .pointer = [](const wl::PointerEvent &) {},
                       .key = [](const wl::KeyEvent &) {},
                       .focus = [](bool) {},
                       .text_input = [](const wl::TextInputEvent &) {}},
                      720, 480);
    auto timeout = g.loop.after(2s, [&] {
        g.loop.quit();
    });
    g.loop.run();
    g.loop.cancel(timeout);
    REQUIRE(configured);
    auto *target = g.renderer.create_target(g.display.display, window.surface());
    g.renderer.resize(*target, window.buffer_width(), window.buffer_height());
    gfx::Prim drawn = emoji;
    drawn.clip = {0, 0, static_cast<float>(window.buffer_width()), static_cast<float>(window.buffer_height())};
    bool captured = false;
    g.renderer.capture_next(*target, [&](const uint8_t *bgra, uint32_t width, uint32_t) {
        // The face is yellow: plenty of red and green, little blue.
        const auto x = static_cast<uint32_t>(drawn.rect[0] + drawn.rect[2] / 2), y = static_cast<uint32_t>(drawn.rect[1] + drawn.rect[3] * 0.3f);
        const uint8_t *pixel = bgra + (y * width + x) * 4;
        CHECK(pixel[2] > 180);
        CHECK(pixel[1] > 120);
        CHECK(pixel[0] + 60 < pixel[2]);
        captured = true;
    });
    g.renderer.draw(*target, {&drawn, 1}, {}, [&] {
        window.prepare_present();
    });
    CHECK(captured);
    g.renderer.destroy_target(target);
}
