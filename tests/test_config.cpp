#include <doctest/doctest.h>

#include "sys/config.hpp"
#include "ui/theme.hpp"

using namespace lyren;

TEST_CASE("embedded default config parses to the built-in defaults") {
    auto c = parse_config(default_config_text());
    REQUIRE(c.has_value());
    Config d;
    CHECK(c->window.decorations == d.window.decorations);
    CHECK(c->render.max_fps == d.render.max_fps);
    CHECK(c->playback.quality == Quality::lossless);
    CHECK(c->theme.font == "sans-serif");
    CHECK_FALSE(c->theme.colors.bg.has_value());
    CHECK(c->desktop_lyrics.outline == "#000000a0");
}

TEST_CASE("config errors carry the line number") {
    auto bad_enum = parse_config("[window]\ndecorations = \"clint\"\n");
    REQUIRE_FALSE(bad_enum.has_value());
    CHECK(bad_enum.error().starts_with("2:"));
    auto unknown = parse_config("[theme]\nfont_size = 14\nradus = 3\n");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().starts_with("3:"));
}

TEST_CASE("theme colours") {
    CHECK(ui::parse_color("#ff0000")->r == doctest::Approx(1));
    CHECK(ui::parse_color("#00000080")->a == doctest::Approx(128 / 255.f));
    CHECK(ui::parse_color("#fff")->g == doctest::Approx(1));
    CHECK_FALSE(ui::parse_color("ff0000"));
    CHECK_FALSE(ui::parse_color("#12345"));
    CHECK_FALSE(ui::parse_color("#zzzzzz"));

    Config c;
    c.theme.colors.bg = "#000000";
    c.theme.colors.text = "nope";
    std::string errors;
    auto theme = ui::Theme::from(c, errors);
    CHECK(theme.bg.r == 0);
    CHECK(errors.find("theme.colors.text") != std::string::npos);
}
