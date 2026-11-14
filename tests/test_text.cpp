#include <doctest/doctest.h>

#include "core/string_map.hpp"
#include "text/shaper.hpp"

using namespace lyren;

TEST_CASE("cache keys own borrowed text and distinguish every variant") {
    std::string backing = "_a long cover URL or title with a suffix_";
    const std::string expected = backing.substr(1, backing.size() - 2);
    CacheMap<int> cache;
    CacheKey::View view{std::string_view(backing).substr(1, backing.size() - 2), 128};
    auto [first, inserted] = cache.emplace(CacheKey(view), 7);
    REQUIRE(inserted);
    CHECK(first->first.text == expected);
    CHECK(cache.find(view)->second == 7);
    cache.emplace(CacheKey({expected, 256}), 8);
    cache.emplace(CacheKey({expected, 0}), 9);
    backing.assign(backing.size(), 'x');
    CHECK(cache.at(CacheKey::View{expected, 128}) == 7);
    CHECK(cache.at(CacheKey::View{expected, 256}) == 8);
    CHECK(cache.at(CacheKey::View{expected, 0}) == 9);
    CHECK(cache.erase(CacheKey::View{expected, 128}) == 1);
    CHECK(cache.size() == 2);

    const std::string embedded_null{"a\0b", 3};
    cache.emplace(CacheKey({embedded_null, 1}), 10);
    cache.emplace(CacheKey({"a", 1}), 11);
    CHECK(cache.at(CacheKey::View{embedded_null, 1}) == 10);
    CHECK(cache.at(CacheKey::View{"a", 1}) == 11);
}

TEST_CASE("shaping reuses borrowed text while keeping font features separate") {
    text::Fonts fonts("sans-serif");
    text::Shaper shaper(fonts);
    std::string input = "[0123456789]";
    const auto &normal = shaper.shape(std::string_view(input).substr(1, 10));
    const auto &tabular = shaper.shape("0123456789", text::tabular_nums);
    CHECK(&normal != &tabular);
    input.assign(input.size(), 'x');
    CHECK(&shaper.shape("0123456789") == &normal);
    CHECK(&shaper.shape("0123456789", text::tabular_nums) == &tabular);
    CHECK_FALSE(normal.glyphs.empty());
    CHECK(tabular.glyphs.size() == normal.glyphs.size());
    CHECK(&shaper.shape("") != &shaper.shape("", text::tabular_nums));
}

TEST_CASE("weights shape as their own runs and never lose a glyph") {
    text::Fonts fonts("sans-serif");
    text::Shaper shaper(fonts);
    const std::string_view text = "Lyren 弦间 0123";
    const auto &regular = shaper.shape(text);
    const auto &medium = shaper.shape(text, text::medium);
    const auto &semibold = shaper.shape(text, text::semibold);
    CHECK(&regular != &medium);
    CHECK(&medium != &semibold);
    // A family without the weight falls back to what it has: the same glyph count, and a width that never shrinks.
    CHECK(medium.glyphs.size() == regular.glyphs.size());
    CHECK(semibold.glyphs.size() == regular.glyphs.size());
    CHECK(medium.width >= regular.width - 1e-3f);
    CHECK(semibold.width >= medium.width - 1e-3f);
    for (int weight = 0; weight < text::Fonts::weights; ++weight) {
        CHECK(fonts.face(fonts.face_for(U'A', false, weight)).hb != nullptr);
    }
    // Out-of-range weights clamp instead of reading past the chains.
    CHECK(fonts.face_for(U'A', false, 9) == fonts.face_for(U'A', false, text::Fonts::weights - 1));
    CHECK(&shaper.shape(text, text::semibold | text::tabular_nums) != &semibold);
}
