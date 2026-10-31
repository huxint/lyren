#include <doctest/doctest.h>
#include <cmath>

#include "sys/config.hpp"
#include "ui/theme.hpp"

using namespace lyren;

TEST_CASE("embedded default config parses to the built-in defaults") {
    auto c = parse_config(default_config_text());
    REQUIRE(c.has_value());
    Config d;
    CHECK(c->interface.language == Language::zh);
    CHECK(c->render.max_fps == d.render.max_fps);
    CHECK(c->playback.quality == Quality::lossless);
    CHECK(c->theme.font == "sans-serif");
    CHECK_FALSE(c->theme.colors.bg.has_value());
    CHECK(c->desktop_lyrics.outline == "#000000a0");
}

TEST_CASE("config errors carry the line number and reject unknown keys") {
    auto bad_enum = parse_config("[theme]\nmode = \"murk\"\n");
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
    CHECK(set_config_value(text, "desktop_lyrics", "font_size", "36") == text + "\n[desktop_lyrics]\nfont_size = 36\n");

    std::string d(default_config_text());
    d = set_config_value(d, "playback", "quality", "\"exhigh\"");
    d = set_config_value(d, "playback", "normalize", "true");
    d = set_config_value(d, "desktop_lyrics", "font_size", "36");
    auto c = parse_config(d);
    REQUIRE(c.has_value());
    CHECK(c->playback.quality == Quality::exhigh);
    CHECK(c->playback.normalize);
    CHECK(c->desktop_lyrics.font_size == 36);
    CHECK(d.find("# Match track loudness") != std::string::npos);

    // Inline comments, quoting and whitespace around the value survive.
    CHECK(set_config_value("[playback]\r\n  normalize\t= false  # Match track loudness\r\n", "playback", "normalize", "true") ==
          "[playback]\r\n  normalize\t= true  # Match track loudness\r\n");
    CHECK(set_config_value("[theme]\nfont = 'Font #1'  # Font\n", "theme", "font", "\"sans-serif\"") == "[theme]\nfont = \"sans-serif\"  # Font\n");
    CHECK(set_config_value(R"([theme]
font = "Font \"#1" # Font
)",
                           "theme", "font", "\"sans-serif\"") == "[theme]\nfont = \"sans-serif\" # Font\n");
}

TEST_CASE("resetting a color override restores the selected theme") {
    std::string text = "[theme]\nmode = \"light\"\n[theme.colors]\n  bg = \"#000000\" # Custom background\ntext = \"#202733\"\n";
    text = set_config_value(text, "theme.colors", "bg", "");
    CHECK(text == "[theme]\nmode = \"light\"\n[theme.colors]\ntext = \"#202733\"\n");
    auto config = parse_config(text);
    REQUIRE(config.has_value());
    CHECK_FALSE(config->theme.colors.bg.has_value());
    CHECK(config->theme.colors.text == "#202733");
    CHECK(set_config_value(text, "theme.colors", "border", "") == text);
    CHECK(set_config_value(text, "absent", "missing", "") == text);
    CHECK(set_config_value("[theme.colors]\nbg = \"#000\"", "theme.colors", "bg", "") == "[theme.colors]\n");
}

TEST_CASE("light theme separates surfaces and keeps text readable") {
    Config config;
    config.theme.mode = ThemeMode::light;
    std::string errors;
    const auto theme = ui::Theme::from(config, errors);
    auto luminance = [](ui::Color color) {
        auto linear = [](float value) {
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        };
        return 0.2126f * linear(color.r) + 0.7152f * linear(color.g) + 0.0722f * linear(color.b);
    };
    auto contrast = [&](ui::Color background, ui::Color foreground) {
        return (luminance(background) + 0.05f) / (luminance(foreground) + 0.05f);
    };
    CHECK(luminance(theme.bg) < luminance(theme.surface));
    CHECK(luminance(theme.surface) <= luminance(theme.elevated));
    CHECK(contrast(theme.bg, theme.text) >= 7);
    CHECK(contrast(theme.surface, theme.text_dim) >= 4.5);
    CHECK(contrast(theme.surface, theme.accent) >= 4.5);
    CHECK(errors.empty());
}
