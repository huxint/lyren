#include "gfx/image.hpp"

#include <png.h>
#include <turbojpeg.h>

#include <algorithm>
#include <cmath>

namespace lyren::gfx {

static Image crop_square(Image img) {
    uint32_t s = std::min(img.w, img.h);
    if (img.w == img.h) return img;
    Image out{s, s, std::vector<uint8_t>(size_t(s) * s * 4)};
    uint32_t x0 = (img.w - s) / 2, y0 = (img.h - s) / 2;
    for (uint32_t y = 0; y < s; ++y)
        std::copy_n(img.px.data() + (size_t(y + y0) * img.w + x0) * 4, size_t(s) * 4, out.px.data() + size_t(y) * s * 4);
    return out;
}

static Image decode_jpeg(std::span<const uint8_t> data, uint32_t size, std::string& error) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    Image img;
    if (tj3DecompressHeader(tj, data.data(), data.size()) == 0) {
        int w = tj3Get(tj, TJPARAM_JPEGWIDTH), h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
        // The smallest DCT scaling that still covers the target.
        int n = 0;
        tjscalingfactor* f = tj3GetScalingFactors(&n);
        tjscalingfactor best{1, 1};
        for (int i = 0; i < n; ++i) {
            int sw = TJSCALED(std::min(w, h), f[i]);
            if (sw >= int(size) && sw < TJSCALED(std::min(w, h), best)) best = f[i];
        }
        tj3SetScalingFactor(tj, best);
        img.w = uint32_t(TJSCALED(w, best)), img.h = uint32_t(TJSCALED(h, best));
        img.px.resize(size_t(img.w) * img.h * 4);
        if (tj3Decompress8(tj, data.data(), data.size(), img.px.data(), 0, TJPF_RGBA) != 0 && tj3GetErrorCode(tj) == TJERR_FATAL) {
            error = tj3GetErrorStr(tj);
            img = {};
        }
    } else {
        error = tj3GetErrorStr(tj);
    }
    tj3Destroy(tj);
    return img;
}

static Image decode_png(std::span<const uint8_t> data, std::string& error) {
    png_image p{};
    p.version = PNG_IMAGE_VERSION;
    Image img;
    if (png_image_begin_read_from_memory(&p, data.data(), data.size())) {
        p.format = PNG_FORMAT_RGBA;
        img.w = p.width, img.h = p.height;
        img.px.resize(PNG_IMAGE_SIZE(p));
        if (!png_image_finish_read(&p, nullptr, img.px.data(), 0, nullptr)) img = {};
    }
    if (!img.w) error = p.message;
    png_image_free(&p);
    for (size_t i = 0; i < img.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) img.px[i + c] = uint8_t((img.px[i + c] * img.px[i + 3] + 127) / 255);
    return img;
}

Image decode_square(std::span<const uint8_t> data, uint32_t size, std::string& error) {
    bool png = data.size() > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
    Image img = png ? decode_png(data, error) : decode_jpeg(data, size, error);
    if (!img.w) return img;
    img = crop_square(std::move(img));
    if (img.w > size) img = downscale(img, size, size);
    return img;
}

Image downscale(const Image& src, uint32_t w, uint32_t h) {
    Image out{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    double sx = double(src.w) / w, sy = double(src.h) / h;
    for (uint32_t y = 0; y < h; ++y) {
        double y0 = y * sy, y1 = y0 + sy;
        for (uint32_t x = 0; x < w; ++x) {
            double x0 = x * sx, x1 = x0 + sx, acc[4] = {}, area = 0;
            for (uint32_t yy = uint32_t(y0); yy < std::min<uint32_t>(src.h, uint32_t(std::ceil(y1))); ++yy) {
                double wy = std::min(y1, yy + 1.0) - std::max(y0, double(yy));
                for (uint32_t xx = uint32_t(x0); xx < std::min<uint32_t>(src.w, uint32_t(std::ceil(x1))); ++xx) {
                    double wgt = wy * (std::min(x1, xx + 1.0) - std::max(x0, double(xx)));
                    const uint8_t* p = &src.px[(size_t(yy) * src.w + xx) * 4];
                    for (int c = 0; c < 4; ++c) acc[c] += p[c] * wgt;
                    area += wgt;
                }
            }
            uint8_t* o = &out.px[(size_t(y) * w + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = uint8_t(std::lround(acc[c] / area));
        }
    }
    return out;
}

void blur(Image& img, int radius) {
    std::vector<float> a(img.px.begin(), img.px.end());
    int w = int(img.w), h = int(img.h);
    std::vector<float> line(size_t(std::max(w, h)) * 4);
    float inv = 1.f / float(2 * radius + 1);
    auto pass = [&](bool horizontal) {
        int n = horizontal ? w : h, lines = horizontal ? h : w;
        size_t stride = horizontal ? 4 : size_t(w) * 4;
        for (int l = 0; l < lines; ++l) {
            size_t start = horizontal ? size_t(l) * w * 4 : size_t(l) * 4;
            for (int i = 0; i < n; ++i) std::copy_n(a.data() + start + size_t(i) * stride, 4, line.data() + size_t(i) * 4);
            float acc[4] = {};
            for (int k = -radius; k <= radius; ++k)
                for (int c = 0; c < 4; ++c) acc[c] += line[size_t(std::clamp(k, 0, n - 1)) * 4 + c];
            for (int i = 0; i < n; ++i) {
                size_t at = start + size_t(i) * stride;
                size_t leave = size_t(std::clamp(i - radius, 0, n - 1)) * 4;
                size_t enter = size_t(std::clamp(i + radius + 1, 0, n - 1)) * 4;
                for (int c = 0; c < 4; ++c) {
                    a[at + c] = acc[c] * inv;
                    acc[c] += line[enter + c] - line[leave + c];
                }
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(true);
        pass(false);
    }
    for (size_t i = 0; i < a.size(); ++i) img.px[i] = uint8_t(a[i] + 0.5f);
}

float dominant_hue(const Image& img) {
    // Hue histogram weighted by saturation and brightness, so grey and dark areas count little.
    constexpr int bins = 36;
    double weight[bins] = {}, sx[bins] = {}, sy[bins] = {}, total = 0;
    for (size_t i = 0; i < img.px.size(); i += 4) {
        float r = img.px[i] / 255.f, g = img.px[i + 1] / 255.f, b = img.px[i + 2] / 255.f;
        float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
        if (d < 1e-3f) continue;
        float h = mx == r ? std::fmod((g - b) / d + 6, 6.f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h /= 6;
        double wgt = double(d) * mx;
        int bin = std::min(bins - 1, int(h * bins));
        weight[bin] += wgt, total += wgt;
        sx[bin] += wgt * std::cos(h * 2 * M_PI), sy[bin] += wgt * std::sin(h * 2 * M_PI);
    }
    if (total < 0.04 * double(img.w * img.h)) return -1;
    // The heaviest bin with its neighbours.
    int best = int(std::max_element(weight, weight + bins) - weight);
    double x = 0, y = 0;
    for (int k = -1; k <= 1; ++k) x += sx[(best + k + bins) % bins], y += sy[(best + k + bins) % bins];
    double h = std::atan2(y, x) / (2 * M_PI);
    return float(h < 0 ? h + 1 : h);
}

}  // namespace lyren::gfx
