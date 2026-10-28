#include <doctest/doctest.h>

#include "gfx/image.hpp"

using namespace lyren::gfx;

TEST_CASE("WebP covers crop and downscale the first canvas with premultiplied alpha") {
    static constexpr uint8_t still[]{
#embed "data/cover.webp"
    };
    static constexpr uint8_t animated[]{
#embed "data/cover-animated.webp"
    };
    for (std::span<const uint8_t> data : {std::span<const uint8_t>(still), std::span<const uint8_t>(animated)}) {
        CAPTURE(data.size());
        std::string error;
        Image image = decode_square(data, 2, error);
        REQUIRE(error.empty());
        CHECK(image.w == 2);
        CHECK(image.h == 2);
        CHECK(image.px == std::vector<uint8_t>{100, 50, 25, 128, 20, 40, 80, 255, 0, 0, 0, 0, 25, 50, 15, 64});

        Image small = decode_square(data, 1, error);
        CHECK(error.empty());
        CHECK(small.w == 1);
        CHECK(small.h == 1);
        CHECK(small.px == std::vector<uint8_t>{36, 35, 30, 112});

        Image large = decode_square(data, 64, error);
        CHECK(error.empty());
        CHECK(large.w == 2);
        CHECK(large.h == 2);
        CHECK(large.px == image.px);
    }
}

TEST_CASE("truncated WebP covers report a decode error") {
    static constexpr uint8_t data[]{
#embed "data/cover.webp"
    };
    std::string error;
    Image image = decode_square(std::span<const uint8_t>(data).first(sizeof(data) - 10), 2, error);
    CHECK(image.w == 0);
    CHECK(image.h == 0);
    CHECK(image.px.empty());
    CHECK_FALSE(error.empty());
}

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
