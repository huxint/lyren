#include <doctest/doctest.h>
#include <png.h>

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

TEST_CASE("GIF pictures decode their first frame with transparency, cropped as covers or whole") {
    static constexpr uint8_t data[]{
#embed "data/wide.gif"
    };
    std::string error;
    Image whole = decode_fit(data, 64, error);
    REQUIRE(error.empty());
    CHECK(whole.w == 4);
    CHECK(whole.h == 2);
    CHECK(std::vector<uint8_t>(whole.px.begin(), whole.px.begin() + 4) == std::vector<uint8_t>{255, 0, 0, 255});
    CHECK(std::vector<uint8_t>(whole.px.begin() + 12, whole.px.begin() + 16) == std::vector<uint8_t>{0, 0, 0, 0});

    Image half = decode_fit(data, 2, error);
    CHECK(error.empty());
    CHECK(half.w == 2);
    CHECK(half.h == 1);
    CHECK(half.px == std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 0, 0});

    Image square = decode_square(data, 64, error);
    CHECK(error.empty());
    CHECK(square.w == 2);
    CHECK(square.h == 2);
    CHECK(std::vector<uint8_t>(square.px.begin(), square.px.begin() + 4) == std::vector<uint8_t>{255, 0, 0, 255});
    CHECK(std::vector<uint8_t>(square.px.begin() + 4, square.px.begin() + 8) == std::vector<uint8_t>{0, 0, 0, 0});

    Image truncated = decode_fit(std::span<const uint8_t>(data).first(10), 64, error);
    CHECK(truncated.w == 0);
    CHECK_FALSE(error.empty());
}

TEST_CASE("streaming PNG decode matches exact crop, premultiplied alpha, and box downscale") {
    std::vector<uint8_t> raw(8 * 4 * 4);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            uint8_t *p = &raw[(y * 8 + x) * 4];
            p[0] = static_cast<uint8_t>(30 * x + 10);
            p[1] = static_cast<uint8_t>(50 * y + 20);
            p[2] = static_cast<uint8_t>(20 * (x + y));
            p[3] = static_cast<uint8_t>(63 * (y + 1));
        }
    }
    png_image writer{};
    writer.version = PNG_IMAGE_VERSION;
    writer.width = 8;
    writer.height = 4;
    writer.format = PNG_FORMAT_RGBA;
    png_alloc_size_t len = 0;
    REQUIRE(png_image_write_to_memory(&writer, nullptr, &len, 0, raw.data(), 0, nullptr));
    std::vector<uint8_t> encoded(len);
    REQUIRE(png_image_write_to_memory(&writer, encoded.data(), &len, 0, raw.data(), 0, nullptr));
    encoded.resize(len);

    Image expected_crop{4, 4, std::vector<uint8_t>(4 * 4 * 4)};
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const uint8_t *s = &raw[(y * 8 + (x + 2)) * 4];
            uint8_t *d = &expected_crop.px[(y * 4 + x) * 4];
            for (int c = 0; c < 3; ++c) {
                d[c] = static_cast<uint8_t>((s[c] * s[3] + 127) / 255);
            }
            d[3] = s[3];
        }
    }
    const Image expected_2x2 = downscale(expected_crop, 2, 2);

    std::string error;
    const Image sq4 = decode_square(encoded, 4, error);
    REQUIRE(error.empty());
    CHECK(sq4.w == 4);
    CHECK(sq4.h == 4);
    CHECK(sq4.px == expected_crop.px);

    const Image sq2 = decode_square(encoded, 2, error);
    REQUIRE(error.empty());
    CHECK(sq2.w == 2);
    CHECK(sq2.h == 2);
    CHECK(sq2.px == expected_2x2.px);
}

namespace {
    std::vector<uint8_t> encode_custom_png(uint32_t w, uint32_t h, int color_type, int interlace, double gamma, const std::vector<uint8_t> &rows, size_t stride) {
        std::vector<uint8_t> out;
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        REQUIRE(png != nullptr);
        png_infop info = png_create_info_struct(png);
        REQUIRE(info != nullptr);
        REQUIRE(setjmp(png_jmpbuf(png)) == 0);
        png_set_write_fn(
            png, &out,
            [](png_structp p, png_bytep data, png_size_t n) {
                auto *vec = static_cast<std::vector<uint8_t> *>(png_get_io_ptr(p));
                vec->insert(vec->end(), data, data + n);
            },
            [](png_structp) {});
        png_set_IHDR(png, info, w, h, 8, color_type, interlace, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        if (gamma > 0) {
            png_set_gAMA(png, info, gamma);
        }
        png_write_info(png, info);
        std::vector<png_const_bytep> ptrs(h);
        for (uint32_t y = 0; y < h; ++y) {
            ptrs[y] = rows.data() + static_cast<size_t>(y) * stride;
        }
        png_write_image(png, const_cast<png_bytepp>(ptrs.data()));
        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
        return out;
    }
} // namespace

TEST_CASE("streaming PNG converts linear gAMA chunks to sRGB and decodes large Adam7 interlaced images") {
    SUBCASE("linear gamma 1.0 grayscale converts to sRGB like libpng simplified API") {
        const std::vector<uint8_t> gray(16, 128);
        const auto encoded = encode_custom_png(4, 4, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, 1.0, gray, 4);

        png_image ref{};
        ref.version = PNG_IMAGE_VERSION;
        REQUIRE(png_image_begin_read_from_memory(&ref, encoded.data(), encoded.size()));
        ref.format = PNG_FORMAT_RGBA;
        std::vector<uint8_t> ref_px(4 * 4 * 4);
        REQUIRE(png_image_finish_read(&ref, nullptr, ref_px.data(), 0, nullptr));

        std::string error;
        const Image img = decode_square(encoded, 4, error);
        REQUIRE(error.empty());
        REQUIRE(img.w == 4);
        CHECK(std::abs(static_cast<int>(img.px[0]) - static_cast<int>(ref_px[0])) <= 1);
        CHECK(img.px[0] > 180);
    }

    SUBCASE("Adam7 interlaced PNG matches sequential PNG and supports covers above 4 megapixels") {
        std::vector<uint8_t> rgba(16 * 16 * 4);
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                uint8_t *p = &rgba[(y * 16 + x) * 4];
                p[0] = static_cast<uint8_t>(x * 15);
                p[1] = static_cast<uint8_t>(y * 15);
                p[2] = static_cast<uint8_t>((x + y) * 8);
                p[3] = 255;
            }
        }
        const auto seq_png = encode_custom_png(16, 16, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, 0, rgba, 16 * 4);
        const auto adam7_png = encode_custom_png(16, 16, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_ADAM7, 0, rgba, 16 * 4);
        std::string error;
        const Image seq_full = decode_square(seq_png, 16, error);
        REQUIRE(error.empty());
        const Image adam7_full = decode_square(adam7_png, 16, error);
        REQUIRE(error.empty());
        CHECK(adam7_full.px == seq_full.px);

        const Image seq_4 = decode_square(seq_png, 4, error);
        const Image adam7_4 = decode_square(adam7_png, 4, error);
        REQUIRE(error.empty());
        CHECK(adam7_4.px == seq_4.px);

        // 2049x2049 (> 4MP) interlaced PNG cover downscaled to 64x64.
        const uint32_t big_side = 2049;
        std::vector<uint8_t> big_gray(static_cast<size_t>(big_side) * big_side, 200);
        const auto big_interlaced = encode_custom_png(big_side, big_side, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_ADAM7, 0, big_gray, big_side);
        const Image big_decoded = decode_square(big_interlaced, 64, error);
        REQUIRE_MESSAGE(error.empty(), error);
        CHECK(big_decoded.w == 64);
        CHECK(big_decoded.h == 64);
        CHECK(big_decoded.px[0] == 200);
    }
}
