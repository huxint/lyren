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

TEST_CASE("settings are written into the config text in place") {
    const std::string text = "# top\n[playback]\n# quality\nquality = \"lossless\"\ntrial = \"skip\"\n\n[theme]\nfont_size = 14\n";
    CHECK(set_config_value(text, "playback", "quality", "\"exhigh\"") == "# top\n[playback]\n# quality\nquality = \"exhigh\"\ntrial = \"skip\"\n\n[theme]\nfont_size = 14\n");
    // A missing key follows the last setting of its section, and a longer key is not mistaken for it.
    CHECK(set_config_value(text, "playback", "normalize", "true") ==
          "# top\n[playback]\n# quality\nquality = \"lossless\"\ntrial = \"skip\"\nnormalize = true\n\n[theme]\nfont_size = 14\n");
    CHECK(set_config_value(text, "theme", "font", "\"x\"") == text + "font = \"x\"\n");
    CHECK(set_config_value(text, "desktop_lyrics", "enabled", "true") == text + "\n[desktop_lyrics]\nenabled = true\n");

    std::string d(default_config_text());
    d = set_config_value(d, "playback", "quality", "\"exhigh\"");
    d = set_config_value(d, "playback", "normalize", "true");
    d = set_config_value(d, "desktop_lyrics", "enabled", "true");
    auto c = parse_config(d);
    REQUIRE(c.has_value());
    CHECK(c->playback.quality == Quality::exhigh);
    CHECK(c->playback.normalize);
    CHECK(c->desktop_lyrics.enabled);
    CHECK(d.find("# 音量均衡") != std::string::npos);
}

TEST_CASE("setting updates preserve inline comments and surrounding whitespace") {
    CHECK(set_config_value("[playback]\r\n  normalize\t= false  # 音量均衡\r\n", "playback", "normalize", "true") == "[playback]\r\n  normalize\t= true  # 音量均衡\r\n");
    CHECK(set_config_value("[theme]\naccent = \"#abcdef\" # 强调色\n", "theme", "accent", "\"auto\"") == "[theme]\naccent = \"auto\" # 强调色\n");
    CHECK(set_config_value("[theme]\nfont = 'Font #1'  # 字体\n", "theme", "font", "\"sans-serif\"") == "[theme]\nfont = \"sans-serif\"  # 字体\n");
    CHECK(set_config_value(R"([theme]
font = "Font \"#1" # 字体
)",
                           "theme", "font", "\"sans-serif\"") == "[theme]\nfont = \"sans-serif\" # 字体\n");
}
