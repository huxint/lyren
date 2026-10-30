#include <doctest/doctest.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "app/draw.hpp"
#include "ui/motion.hpp"
#include "ui/text_edit.hpp"
#include "ui/ui.hpp"

using namespace lyren;
using namespace lyren::ui;

TEST_CASE("bezier curves hit their ends and follow the CSS reference values") {
    for (Bezier b : {ease::standard, ease::emphasized, ease::exit, ease::linear}) {
        CHECK(b(0) == 0);
        CHECK(b(1) == 1);
        float prev = 0;
        for (int i = 1; i <= 100; ++i) {
            float v = b(static_cast<float>(i) / 100);
            CHECK(v >= prev - 1e-4f);
            prev = v;
        }
    }
    CHECK(ease::linear(0.37f) == doctest::Approx(0.37f).epsilon(1e-4));
    // CSS "ease" (0.25, 0.1, 0.25, 1) at x = 0.5 is about 0.8024.
    CHECK(Bezier{0.25f, 0.1f, 0.25f, 1}(0.5f) == doctest::Approx(0.8024f).epsilon(1e-3));
    CHECK(ease::emphasized(0.2f) > 0.6f); // decelerating: most of the way early
    CHECK(ease::exit(0.2f) < 0.1f);       // accelerating: slow start
}

TEST_CASE("spring motion settles without oscillation and is independent of frame rate") {
    Spring s;
    float previous = 0;
    for (int i = 0; i < 120; ++i) {
        s.advance(1, 1.f / 120, 0.28f);
        CHECK(s.value >= previous);
        CHECK(s.value <= 1);
        previous = s.value;
    }
    CHECK(s.value == doctest::Approx(1).epsilon(1e-5));
    CHECK(std::abs(s.velocity) < 1e-5f);

    Spring reference;
    reference.advance(1, 0.28f, 0.28f);
    CHECK(reference.value == doctest::Approx(0.99f).epsilon(1e-4));
    for (int fps : {30, 60, 144}) {
        Spring stepped;
        for (int i = 0; i < fps; ++i) {
            stepped.advance(1, 0.28f / static_cast<float>(fps), 0.28f);
        }
        CHECK(stepped.value == doctest::Approx(reference.value).epsilon(1e-5));
        CHECK(stepped.velocity == doctest::Approx(reference.velocity).epsilon(1e-4));
    }
}

TEST_CASE("spring retargeting preserves velocity and handles long frame gaps") {
    Spring s;
    s.advance(1, 0.07f, 0.28f);
    float value = s.value, velocity = s.velocity;
    s.advance(0, 0, 0.28f);
    CHECK(s.value == value);
    CHECK(s.velocity == velocity);
    // Changing direction decelerates the old motion instead of immediately reversing it.
    s.advance(0, 0.0001f, 0.28f);
    CHECK(s.value > value);
    CHECK(s.velocity > 0);
    s.advance(0, 0.1f, 0.28f);
    CHECK(s.velocity < 0);
    s.advance(0, 10, 0.28f);
    CHECK(s.value == doctest::Approx(0));
    CHECK(s.velocity == doctest::Approx(0));
}

TEST_CASE("lyric wave settles through word gaps and sustained notes") {
    LyricLine line{.time = 1, .text = "春风", .words = {{1000, 150, 0, 3}, {3000, 500, 3, 6}}};
    text::Shaped shaped{.glyphs = {{.x = 0, .cluster = 0}, {.x = 1, .cluster = 3}}, .width = 2};
    auto lift = [&](double now) {
        std::vector<Painter::Lift> out;
        fill_lyric_wave(out, shaped, line, line.text, 22, now, 1);
        return out[0];
    };

    SUBCASE("a gap lets the sung glyph return to rest") {
        CHECK(word_at(line, 1.2) == word_at(line, 1.3));
        CHECK(lift(1.2).dy > lift(1.3).dy);
        CHECK(lift(1.3).dy > 0);
        CHECK(lift(1.5).dy == 0);
        CHECK(lift(1.5).scale == 1);
    }
    SUBCASE("a long note keeps moving after its attack") {
        line.words[0].duration_ms = 1600;
        CHECK(lift(1.4).dy > lift(1.8).dy);
        CHECK(lift(1.8).dy > lift(2.2).dy);
        CHECK(lift(2.2).dy > 0);
        CHECK(lift(2.9).dy == 0);
    }
    SUBCASE("short and zero-duration syllables do not jump between frames") {
        for (uint32_t duration : {0u, 20u, 60u}) {
            CAPTURE(duration);
            line.words[0].duration_ms = duration;
            auto previous = lift(0.75);
            float peak = 0;
            for (int frame = 1; frame <= 60; ++frame) {
                const auto current = lift(0.75 + frame / 60.0);
                CHECK(std::abs(current.dy - previous.dy) < 0.35f);
                CHECK(std::abs(current.scale - previous.scale) < 0.009f);
                peak = std::max(peak, current.dy);
                previous = current;
            }
            CHECK(peak > 2.7f);
            CHECK(previous.dy == 0);
        }
    }
}

TEST_CASE("lyric wave preserves cluster timing across wrapped rows") {
    LyricLine line{.time = 1, .text = "春风来", .words = {{1000, 1200, 0, 9}}};
    // More than one glyph may belong to one text cluster.
    text::Shaped shaped{.glyphs = {{.x = 0, .cluster = 0}, {.x = 0.2f, .cluster = 0}, {.x = 1, .cluster = 3}, {.x = 2, .cluster = 6}}, .width = 3};
    std::vector<Painter::Lift> whole, wrapped;
    fill_lyric_wave(whole, shaped, line, line.text, 22, 1.85, 1);
    fill_lyric_wave(wrapped, shaped, line, std::string_view(line.text).substr(6), 22, 1.85, 1);
    REQUIRE(whole.size() == 3);
    REQUIRE(wrapped.size() == 1);
    CHECK(wrapped[0].from == 0);
    CHECK(wrapped[0].dy == whole[2].dy);
    CHECK(wrapped[0].scale == whole[2].scale);
    CHECK(whole[2].dy > whole[0].dy);

    fill_lyric_wave(wrapped, shaped, line, line.text, 22, 1.85, 0);
    CHECK(wrapped.empty());
}

TEST_CASE("rect cuts take strips off the remaining rect") {
    Rect r{0, 0, 100, 50};
    Rect l = cut_left(r, 20), t = cut_top(r, 10), rr = cut_right(r, 30), b = cut_bottom(r, 5);
    CHECK(l.w == 20);
    CHECK(t.x == 20);
    CHECK(t.h == 10);
    CHECK(rr.x == 70);
    CHECK(b.y == 45);
    CHECK(r.x == 20);
    CHECK(r.y == 10);
    CHECK(r.w == 50);
    CHECK(r.h == 35);
    Rect small{0, 0, 10, 10};
    CHECK(cut_left(small, 40).w == 10);
    CHECK(small.w == 0);
    CHECK(intersect({0, 0, 10, 10}, {20, 20, 5, 5}).w == 0);
}

static wl::KeyEvent key(uint32_t sym, uint32_t mods = 0, std::string text = {}) {
    return {.keysym = sym, .text = std::move(text), .mods = mods, .pressed = true};
}

TEST_CASE("word boundaries treat spaces, punctuation, latin and CJK runs separately") {
    std::string s = "hello, 周杰伦 world";
    CHECK(word_right(s, 0) == 5);         // "hello"
    CHECK(word_right(s, 5) == 7);         // ", "
    CHECK(word_right(s, 7) == 7 + 9 + 1); // "周杰伦 "
    CHECK(word_left(s, s.size()) == 17);  // "world"
    CHECK(word_left(s, 16) == 7);         // back over the space and "周杰伦"
    CHECK(char_boundary(s, 8) == 7);      // inside 周
}

TEST_CASE("text edit keys") {
    std::string text, copied;
    TextEdit e;
    e.apply(text, key(XKB_KEY_a, 0, "a"), copied);
    e.insert(text, "周杰伦");
    CHECK(text == "a周杰伦");
    CHECK(e.cursor == text.size());
    e.apply(text, key(XKB_KEY_BackSpace), copied);
    CHECK(text == "a周杰");
    e.apply(text, key(XKB_KEY_Left), copied);
    e.apply(text, key(XKB_KEY_Left, wl::mod_shift), copied);
    CHECK(e.selection(text) == "周");
    CHECK(e.apply(text, key(XKB_KEY_x, wl::mod_ctrl), copied) == TextEdit::Key::copy);
    CHECK(copied == "周");
    CHECK(text == "a杰");
    e.apply(text, key(XKB_KEY_a, wl::mod_ctrl), copied);
    e.insert(text, "line\none");
    CHECK(text == "line one");
    e.apply(text, key(XKB_KEY_BackSpace, wl::mod_ctrl), copied);
    CHECK(text == "line ");
    CHECK(e.apply(text, key(XKB_KEY_Return), copied) == TextEdit::Key::submit);
    CHECK(e.apply(text, key(XKB_KEY_v, wl::mod_ctrl), copied) == TextEdit::Key::paste);
    CHECK(e.apply(text, key(XKB_KEY_F5), copied) == TextEdit::Key::ignored);
    e.select_word("say 你好 now", 5);
    CHECK(e.anchor == 4);
    CHECK(e.cursor == 10);
}

TEST_CASE("multiline comments preserve pasted lines and Enter variants never submit") {
    std::string text, copied;
    TextEdit e;
    e.insert(text, "第一行\r\n第二行🎵\r第三行", true);
    CHECK(text == "第一行\n第二行🎵\n第三行");
    for (uint32_t mods : {0u, uint32_t(wl::mod_ctrl), uint32_t(wl::mod_shift), uint32_t(wl::mod_ctrl | wl::mod_shift)}) {
        CHECK(e.apply(text, key(XKB_KEY_Return, mods), copied, true) == TextEdit::Key::edited);
    }
    CHECK(text.ends_with("第三行\n\n\n\n"));
    CHECK(e.apply(text, key(XKB_KEY_KP_Enter, wl::mod_ctrl), copied, true) == TextEdit::Key::edited);
}
