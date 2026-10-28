#include "gfx/image.hpp"

#include <png.h>
#include <turbojpeg.h>
#include <webp/demux.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace lyren::gfx {

    static Image crop_square(Image img) {
        uint32_t s = std::min(img.w, img.h);
        if (img.w == img.h) {
            return img;
        }
        Image out{s, s, std::vector<uint8_t>(static_cast<size_t>(s) * s * 4)};
        uint32_t x0 = (img.w - s) / 2, y0 = (img.h - s) / 2;
        for (uint32_t y = 0; y < s; ++y) {
            std::copy_n(img.px.data() + (static_cast<size_t>(y + y0) * img.w + x0) * 4, static_cast<size_t>(s) * 4, out.px.data() + static_cast<size_t>(y) * s * 4);
        }
        return out;
    }

    static Image decode_jpeg(std::span<const uint8_t> data, uint32_t size, std::string &error) {
        tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
        Image img;
        if (tj3DecompressHeader(tj, data.data(), data.size()) == 0) {
            int w = tj3Get(tj, TJPARAM_JPEGWIDTH), h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
            // The smallest DCT scaling that still covers the target.
            int n = 0;
            tjscalingfactor *f = tj3GetScalingFactors(&n);
            tjscalingfactor best{1, 1};
            for (int i = 0; i < n; ++i) {
                int sw = TJSCALED(std::min(w, h), f[i]);
                if (sw >= static_cast<int>(size) && sw < TJSCALED(std::min(w, h), best)) {
                    best = f[i];
                }
            }
            tj3SetScalingFactor(tj, best);
            img.w = static_cast<uint32_t>(TJSCALED(w, best)), img.h = static_cast<uint32_t>(TJSCALED(h, best));
            img.px.resize(static_cast<size_t>(img.w) * img.h * 4);
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

    static Image decode_png(std::span<const uint8_t> data, std::string &error) {
        png_image p{};
        p.version = PNG_IMAGE_VERSION;
        Image img;
        if (png_image_begin_read_from_memory(&p, data.data(), data.size())) {
            p.format = PNG_FORMAT_RGBA;
            img.w = p.width, img.h = p.height;
            img.px.resize(PNG_IMAGE_SIZE(p));
            if (!png_image_finish_read(&p, nullptr, img.px.data(), 0, nullptr)) {
                img = {};
            }
        }
        if (!img.w) {
            error = p.message;
        }
        png_image_free(&p);
        for (size_t i = 0; i < img.px.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                img.px[i + c] = static_cast<uint8_t>((img.px[i + c] * img.px[i + 3] + 127) / 255);
            }
        }
        return img;
    }

    static Image decode_webp(std::span<const uint8_t> data, std::string &error) {
        const WebPData source{data.data(), data.size()};
        WebPAnimDecoderOptions options;
        WebPAnimDecoderOptionsInit(&options);
        options.color_mode = MODE_rgbA;
        WebPAnimDecoder *decoder = WebPAnimDecoderNew(&source, &options);
        if (!decoder) {
            error = "Invalid WebP image";
            return {};
        }
        WebPAnimInfo info;
        WebPAnimDecoderGetInfo(decoder, &info);
        uint8_t *px;
        int timestamp;
        Image img;
        // Covers stay still: use the first composited canvas for animated WebP too.
        if (WebPAnimDecoderGetNext(decoder, &px, &timestamp)) {
            img = {info.canvas_width, info.canvas_height, {px, px + static_cast<size_t>(info.canvas_width) * info.canvas_height * 4}};
        } else {
            error = "Failed to decode WebP image";
        }
        WebPAnimDecoderDelete(decoder);
        return img;
    }

    Image decode_square(std::span<const uint8_t> data, uint32_t size, std::string &error) {
        bool png = data.size() > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
        bool webp = data.size() >= 12 && std::equal(data.begin(), data.begin() + 4, "RIFF") && std::equal(data.begin() + 8, data.begin() + 12, "WEBP");
        Image img = png ? decode_png(data, error) : webp ? decode_webp(data, error) : decode_jpeg(data, size, error);
        if (!img.w) {
            return img;
        }
        img = crop_square(std::move(img));
        if (img.w > size) {
            img = downscale(img, size, size);
        }
        return img;
    }

    Image downscale(const Image &src, uint32_t w, uint32_t h) {
        Image out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
        double sx = static_cast<double>(src.w) / w, sy = static_cast<double>(src.h) / h;
        for (uint32_t y = 0; y < h; ++y) {
            double y0 = y * sy, y1 = y0 + sy;
            for (uint32_t x = 0; x < w; ++x) {
                double x0 = x * sx, x1 = x0 + sx, acc[4] = {}, area = 0;
                for (uint32_t yy = static_cast<uint32_t>(y0); yy < std::min<uint32_t>(src.h, static_cast<uint32_t>(std::ceil(y1))); ++yy) {
                    double wy = std::min(y1, yy + 1.0) - std::max(y0, static_cast<double>(yy));
                    for (uint32_t xx = static_cast<uint32_t>(x0); xx < std::min<uint32_t>(src.w, static_cast<uint32_t>(std::ceil(x1))); ++xx) {
                        double wgt = wy * (std::min(x1, xx + 1.0) - std::max(x0, static_cast<double>(xx)));
                        const uint8_t *p = &src.px[(static_cast<size_t>(yy) * src.w + xx) * 4];
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += p[c] * wgt;
                        }
                        area += wgt;
                    }
                }
                uint8_t *o = &out.px[(static_cast<size_t>(y) * w + x) * 4];
                for (int c = 0; c < 4; ++c) {
                    o[c] = static_cast<uint8_t>(std::lround(acc[c] / area));
                }
            }
        }
        return out;
    }

    void blur(Image &img, int radius) {
        std::vector<float> a(img.px.begin(), img.px.end());
        int w = static_cast<int>(img.w), h = static_cast<int>(img.h);
        std::vector<float> line(static_cast<size_t>(std::max(w, h)) * 4);
        float inv = 1.f / static_cast<float>(2 * radius + 1);
        auto pass = [&](bool horizontal) {
            int n = horizontal ? w : h, lines = horizontal ? h : w;
            size_t stride = horizontal ? 4 : static_cast<size_t>(w) * 4;
            for (int l = 0; l < lines; ++l) {
                size_t start = horizontal ? static_cast<size_t>(l) * w * 4 : static_cast<size_t>(l) * 4;
                for (int i = 0; i < n; ++i) {
                    std::copy_n(a.data() + start + static_cast<size_t>(i) * stride, 4, line.data() + static_cast<size_t>(i) * 4);
                }
                std::array<float, 4> acc{};
                for (int k = -radius; k <= radius; ++k) {
                    for (int c = 0; c < 4; ++c) {
                        acc[c] += line[static_cast<size_t>(std::clamp(k, 0, n - 1)) * 4 + c];
                    }
                }
                for (int i = 0; i < n; ++i) {
                    size_t at = start + static_cast<size_t>(i) * stride;
                    size_t leave = static_cast<size_t>(std::clamp(i - radius, 0, n - 1)) * 4;
                    size_t enter = static_cast<size_t>(std::clamp(i + radius + 1, 0, n - 1)) * 4;
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
        for (size_t i = 0; i < a.size(); ++i) {
            img.px[i] = static_cast<uint8_t>(a[i] + 0.5f);
        }
    }

    float dominant_hue(const Image &img) {
        // Hue histogram weighted by saturation and brightness, so grey and dark areas count little.
        constexpr int bins = 36;
        std::array<double, bins> weight{}, sx{}, sy{};
        double total = 0;
        for (size_t i = 0; i < img.px.size(); i += 4) {
            float r = img.px[i] / 255.f, g = img.px[i + 1] / 255.f, b = img.px[i + 2] / 255.f;
            float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
            if (d < 1e-3f) {
                continue;
            }
            float h = mx == r ? std::fmod((g - b) / d + 6, 6.f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
            h /= 6;
            double wgt = static_cast<double>(d) * mx;
            int bin = std::min(bins - 1, static_cast<int>(h * bins));
            weight[bin] += wgt, total += wgt;
            sx[bin] += wgt * std::cos(h * 2 * std::numbers::pi), sy[bin] += wgt * std::sin(h * 2 * std::numbers::pi);
        }
        if (total < 0.04 * static_cast<double>(img.w * img.h)) {
            return -1;
        }
        // The heaviest bin with its neighbours.
        int best = static_cast<int>(std::ranges::max_element(weight) - weight.begin());
        double x = 0, y = 0;
        for (int k = -1; k <= 1; ++k) {
            x += sx[(best + k + bins) % bins], y += sy[(best + k + bins) % bins];
        }
        double h = std::atan2(y, x) / (2 * std::numbers::pi);
        return static_cast<float>(h < 0 ? h + 1 : h);
    }

    float mean_luma(const Image &img) {
        double sum = 0;
        for (size_t i = 0; i < img.px.size(); i += 4) {
            sum += 0.2126 * img.px[i] + 0.7152 * img.px[i + 1] + 0.0722 * img.px[i + 2];
        }
        return img.px.empty() ? 0 : static_cast<float>(sum / (255.0 * static_cast<double>(img.px.size() / 4)));
    }

} // namespace lyren::gfx
