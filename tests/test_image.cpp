#include <doctest/doctest.h>

#include "gfx/image.hpp"

using namespace lyren::gfx;

TEST_CASE("cover blur clamps edges and keeps colour channels independent") {
    SUBCASE("one pixel stays unchanged even with a larger radius") {
        Image image{1, 1, {23, 51, 99, 180}};
        blur(image, 5);
        CHECK(image.px == std::vector<uint8_t>{23, 51, 99, 180});
    }
    SUBCASE("horizontal and vertical lines have the same clamped blur") {
        for (bool horizontal : {true, false}) {
            Image image{horizontal ? 3u : 1u, horizontal ? 1u : 3u, {0, 64, 128, 255, 0, 64, 128, 255, 255, 64, 128, 255}};
            blur(image, 1);
            CHECK(image.px == std::vector<uint8_t>{47, 64, 128, 255, 85, 64, 128, 255, 123, 64, 128, 255});
        }
    }
    SUBCASE("a corner spreads in both dimensions over three passes") {
        Image image{2, 2, {0, 64, 128, 255, 0, 64, 128, 255, 0, 64, 128, 255, 255, 64, 128, 255}};
        blur(image, 1);
        CHECK(image.px == std::vector<uint8_t>{59, 64, 128, 255, 64, 64, 128, 255, 64, 64, 128, 255, 69, 64, 128, 255});
    }
}
