#include <doctest/doctest.h>
#include <xkbcommon/xkbcommon-keysyms.h>

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
            float v = b(float(i) / 100);
            CHECK(v >= prev - 1e-4f);
            prev = v;
        }
    }
    CHECK(ease::linear(0.37f) == doctest::Approx(0.37f).epsilon(1e-4));
    // CSS "ease" (0.25, 0.1, 0.25, 1) at x = 0.5 is about 0.8024.
    CHECK(Bezier{0.25f, 0.1f, 0.25f, 1}(0.5f) == doctest::Approx(0.8024f).epsilon(1e-3));
    CHECK(ease::emphasized(0.2f) > 0.6f);  // decelerating: most of the way early
    CHECK(ease::exit(0.2f) < 0.1f);        // accelerating: slow start
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
        for (int i = 0; i < fps; ++i) stepped.advance(1, 0.28f / float(fps), 0.28f);
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
    CHECK(word_right(s, 0) == 5);              // "hello"
    CHECK(word_right(s, 5) == 7);              // ", "
    CHECK(word_right(s, 7) == 7 + 9 + 1);      // "周杰伦 "
    CHECK(word_left(s, s.size()) == 17);       // "world"
    CHECK(word_left(s, 16) == 7);              // back over the space and "周杰伦"
    CHECK(char_boundary(s, 8) == 7);           // inside 周
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
