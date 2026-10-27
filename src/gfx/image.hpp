#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lyren::gfx {

    // Premultiplied RGBA8.
    struct Image {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px;
    };

    // Decodes JPEG or PNG into a square of at most size×size: the centre is cropped, JPEGs are scaled while decoding.
    Image decode_square(std::span<const uint8_t> data, uint32_t size, std::string &error);
    // Area-averaged downscale.
    Image downscale(const Image &src, uint32_t w, uint32_t h);
    // Three box passes approximating a Gaussian, edges clamped.
    void blur(Image &img, int radius);

    // The cover's most prominent hue (0..1), or a negative value for a colourless image.
    float dominant_hue(const Image &img);

} // namespace lyren::gfx
