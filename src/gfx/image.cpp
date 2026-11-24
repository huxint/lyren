#include "gfx/image.hpp"

#include <gif_lib.h>
#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include <csetjmp>
#include <cstring>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace lyren::gfx {

    static constexpr uint32_t max_image_dim = 8192;
    static constexpr uint64_t max_image_pixels = 32ull << 20;

    static bool valid_dimensions(int w, int h) {
        return w > 0 && h > 0 && static_cast<uint32_t>(w) <= max_image_dim && static_cast<uint32_t>(h) <= max_image_dim &&
               static_cast<uint64_t>(w) * static_cast<uint64_t>(h) <= max_image_pixels;
    }

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

    // size bounds the shorter side, or the longer one with whole.
    static Image decode_jpeg(std::span<const uint8_t> data, uint32_t size, bool whole, std::string &error) {
        tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
        Image img;
        if (tj3DecompressHeader(tj, data.data(), data.size()) == 0) {
            int w = tj3Get(tj, TJPARAM_JPEGWIDTH), h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
            if (!valid_dimensions(w, h)) {
                error = "Image dimensions are too large";
                tj3Destroy(tj);
                return img;
            }
            // The smallest DCT scaling that still covers the target.
            int n = 0, side = whole ? std::max(w, h) : std::min(w, h);
            tjscalingfactor *f = tj3GetScalingFactors(&n);
            tjscalingfactor best{1, 1};
            for (int i = 0; i < n; ++i) {
                int sw = TJSCALED(side, f[i]);
                if (sw >= static_cast<int>(size) && sw < TJSCALED(side, best)) {
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

    namespace {
        struct PngState {
            std::span<const uint8_t> data;
            size_t at = 0;
            uint32_t target = 1;
            bool whole = false;
            Image img;
            std::string error;
            std::vector<uint8_t> row;
            std::vector<float> acc;
            struct Span {
                uint32_t i0 = 0, i1 = 0;
                float w0 = 0, w1 = 0;
            };
            std::vector<Span> xspans;
        };

        void premultiply_rgba(uint8_t *px, size_t count) {
            for (size_t i = 0; i < count; i += 4) {
                const uint32_t a = px[i + 3];
                px[i + 0] = static_cast<uint8_t>((px[i + 0] * a + 127) / 255);
                px[i + 1] = static_cast<uint8_t>((px[i + 1] * a + 127) / 255);
                px[i + 2] = static_cast<uint8_t>((px[i + 2] * a + 127) / 255);
            }
        }

        void read_png_stream(png_structp png, png_infop info, PngState &st) {
            png_set_read_fn(png, &st, [](png_structp p, png_bytep out, png_size_t n) {
                auto &s = *static_cast<PngState *>(png_get_io_ptr(p));
                if (s.data.size() - s.at < n) {
                    png_error(p, "Truncated PNG image");
                }
                std::memcpy(out, s.data.data() + s.at, n);
                s.at += n;
            });
            png_read_info(png, info);
            const uint32_t w = png_get_image_width(png, info), h = png_get_image_height(png, info);
            if (!valid_dimensions(static_cast<int>(w), static_cast<int>(h))) {
                png_error(png, "Image dimensions are too large");
            }
            png_set_expand(png);
            png_set_strip_16(png);
            png_set_gray_to_rgb(png);
            png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
            const int passes = png_set_interlace_handling(png);
            png_read_update_info(png, info);

            if (passes > 1) {
                if (static_cast<uint64_t>(w) * h > (4ull << 20)) {
                    png_error(png, "Interlaced PNG is too large");
                }
                st.img = {w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
                for (int pass = 0; pass < passes; ++pass) {
                    for (uint32_t y = 0; y < h; ++y) {
                        png_read_row(png, st.img.px.data() + static_cast<size_t>(y) * w * 4, nullptr);
                    }
                }
                premultiply_rgba(st.img.px.data(), st.img.px.size());
                return;
            }

            // Stream non-interlaced PNGs one scanline at a time directly into the cropped & box-downsampled target.
            const uint32_t cw = st.whole ? w : std::min(w, h), ch = st.whole ? h : cw;
            const uint32_t x0 = (w - cw) / 2, y0 = (h - ch) / 2;
            uint32_t out_w = cw, out_h = ch;
            if (!st.whole && cw > st.target) {
                out_w = out_h = st.target;
            } else if (st.whole && (cw > st.target || ch > st.target)) {
                const float k = static_cast<float>(st.target) / static_cast<float>(std::max(cw, ch));
                out_w = std::max(1u, static_cast<uint32_t>(std::lround(cw * k)));
                out_h = std::max(1u, static_cast<uint32_t>(std::lround(ch * k)));
            }

            st.row.resize(static_cast<size_t>(w) * 4);
            st.img = {out_w, out_h, std::vector<uint8_t>(static_cast<size_t>(out_w) * out_h * 4)};
            if (out_w == cw && out_h == ch) {
                for (uint32_t y = 0; y < y0 + ch; ++y) {
                    png_read_row(png, st.row.data(), nullptr);
                    if (y < y0) {
                        continue;
                    }
                    uint8_t *src = st.row.data() + static_cast<size_t>(x0) * 4;
                    premultiply_rgba(src, static_cast<size_t>(cw) * 4);
                    std::copy_n(src, static_cast<size_t>(cw) * 4, st.img.px.data() + static_cast<size_t>(y - y0) * out_w * 4);
                }
                return;
            }

            const double sx = static_cast<double>(cw) / out_w, sy = static_cast<double>(ch) / out_h;
            st.xspans.resize(cw);
            for (uint32_t cx = 0; cx < cw; ++cx) {
                const uint32_t i0 = std::min(out_w - 1, static_cast<uint32_t>(cx / sx));
                const double split = static_cast<double>(i0 + 1) * sx;
                if (i0 + 1 < out_w && split < cx + 1.0) {
                    st.xspans[cx] = {i0, i0 + 1, static_cast<float>(split - cx), static_cast<float>(cx + 1.0 - split)};
                } else {
                    st.xspans[cx] = {i0, i0, 1.f, 0.f};
                }
            }
            st.acc.assign(static_cast<size_t>(out_w) * out_h * 4, 0.f);
            for (uint32_t y = 0; y < y0 + ch; ++y) {
                png_read_row(png, st.row.data(), nullptr);
                if (y < y0) {
                    continue;
                }
                const uint32_t cy = y - y0;
                const uint32_t j0 = std::min(out_h - 1, static_cast<uint32_t>(cy / sy));
                const double ysplit = static_cast<double>(j0 + 1) * sy;
                uint32_t j1 = j0;
                float wy0 = 1.f, wy1 = 0.f;
                if (j0 + 1 < out_h && ysplit < cy + 1.0) {
                    j1 = j0 + 1;
                    wy0 = static_cast<float>(ysplit - cy);
                    wy1 = static_cast<float>(cy + 1.0 - ysplit);
                }
                uint8_t *src = st.row.data() + static_cast<size_t>(x0) * 4;
                premultiply_rgba(src, static_cast<size_t>(cw) * 4);
                float *row0 = st.acc.data() + static_cast<size_t>(j0) * out_w * 4;
                float *row1 = st.acc.data() + static_cast<size_t>(j1) * out_w * 4;
                for (uint32_t cx = 0; cx < cw; ++cx) {
                    const auto &xs = st.xspans[cx];
                    const uint8_t *p = src + static_cast<size_t>(cx) * 4;
                    const float w00 = wy0 * xs.w0;
                    for (int c = 0; c < 4; ++c) {
                        row0[xs.i0 * 4 + c] += p[c] * w00;
                    }
                    if (xs.w1 > 0.f) {
                        const float w01 = wy0 * xs.w1;
                        for (int c = 0; c < 4; ++c) {
                            row0[xs.i1 * 4 + c] += p[c] * w01;
                        }
                    }
                    if (wy1 > 0.f) {
                        const float w10 = wy1 * xs.w0;
                        for (int c = 0; c < 4; ++c) {
                            row1[xs.i0 * 4 + c] += p[c] * w10;
                        }
                        if (xs.w1 > 0.f) {
                            const float w11 = wy1 * xs.w1;
                            for (int c = 0; c < 4; ++c) {
                                row1[xs.i1 * 4 + c] += p[c] * w11;
                            }
                        }
                    }
                }
            }
            const float inv_area = static_cast<float>(1.0 / (sx * sy));
            for (size_t i = 0; i < st.acc.size(); ++i) {
                st.img.px[i] = static_cast<uint8_t>(std::clamp(std::lround(st.acc[i] * inv_area), 0l, 255l));
            }
        }

        bool run_png(png_structp png, png_infop info, PngState *st) {
            if (setjmp(png_jmpbuf(png))) {
                return false;
            }
            read_png_stream(png, info, *st);
            return true;
        }
    } // namespace

    static Image decode_png(std::span<const uint8_t> data, uint32_t size, bool whole, std::string &error) {
        PngState st{.data = data, .target = std::max(size, 1u), .whole = whole};
        png_structp png = png_create_read_struct(
            PNG_LIBPNG_VER_STRING, &st,
            [](png_structp p, png_const_charp msg) {
                auto *s = static_cast<PngState *>(png_get_error_ptr(p));
                if (s && s->error.empty() && msg) {
                    s->error = msg;
                }
                longjmp(png_jmpbuf(p), 1);
            },
            [](png_structp, png_const_charp) {});
        if (!png) {
            error = "Out of memory";
            return {};
        }
        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            error = "Out of memory";
            return {};
        }
        const bool ok = run_png(png, info, &st);
        png_destroy_read_struct(&png, &info, nullptr);
        if (!ok) {
            error = st.error.empty() ? "Invalid PNG image" : std::move(st.error);
            return {};
        }
        return std::move(st.img);
    }

    static Image decode_webp(std::span<const uint8_t> data, uint32_t size, bool whole, std::string &error) {
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK) {
            error = "Invalid WebP image";
            return {};
        }
        if (!valid_dimensions(features.width, features.height)) {
            error = "Image dimensions are too large";
            return {};
        }
        if (!features.has_animation) {
            WebPDecoderConfig config;
            if (!WebPInitDecoderConfig(&config)) {
                error = "Invalid WebP image";
                return {};
            }
            int out_w = features.width, out_h = features.height;
            const int side = whole ? std::max(out_w, out_h) : std::min(out_w, out_h);
            const int target = static_cast<int>(std::max(size, 1u)) * 2;
            if (side > std::max(target, 256)) {
                if (!whole && out_w != out_h) {
                    const int s = std::min(out_w, out_h);
                    config.options.use_cropping = 1;
                    config.options.crop_left = (out_w - s) / 2;
                    config.options.crop_top = (out_h - s) / 2;
                    config.options.crop_width = s;
                    config.options.crop_height = s;
                    out_w = out_h = target;
                } else {
                    const double k = static_cast<double>(target) / side;
                    out_w = std::max(1, static_cast<int>(std::lround(out_w * k)));
                    out_h = std::max(1, static_cast<int>(std::lround(out_h * k)));
                }
                config.options.use_scaling = 1;
                config.options.scaled_width = out_w;
                config.options.scaled_height = out_h;
            }
            Image img{static_cast<uint32_t>(out_w), static_cast<uint32_t>(out_h), std::vector<uint8_t>(static_cast<size_t>(out_w) * out_h * 4)};
            config.output.colorspace = MODE_rgbA;
            config.output.u.RGBA.rgba = img.px.data();
            config.output.u.RGBA.stride = out_w * 4;
            config.output.u.RGBA.size = img.px.size();
            config.output.is_external_memory = 1;
            const VP8StatusCode status = WebPDecode(data.data(), data.size(), &config);
            WebPFreeDecBuffer(&config.output);
            if (status != VP8_STATUS_OK) {
                error = "Failed to decode WebP image";
                return {};
            }
            return img;
        }

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
        Image img;
        if (!valid_dimensions(static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height))) {
            error = "Image dimensions are too large";
        } else {
            uint8_t *px = nullptr;
            int timestamp = 0;
            // Covers stay still: use the first composited canvas for animated WebP too.
            if (WebPAnimDecoderGetNext(decoder, &px, &timestamp)) {
                img = {info.canvas_width, info.canvas_height, {px, px + static_cast<size_t>(info.canvas_width) * info.canvas_height * 4}};
            } else {
                error = "Failed to decode WebP image";
            }
        }
        WebPAnimDecoderDelete(decoder);
        return img;
    }

    // The first frame on the logical screen; later animation frames are never read into memory.
    static Image decode_gif(std::span<const uint8_t> data, std::string &error) {
        struct Reader {
            std::span<const uint8_t> data;
            size_t at = 0;
        } reader{data};
        int code = 0;
        GifFileType *gif = DGifOpen(
            &reader,
            [](GifFileType *g, GifByteType *out, int n) {
                auto &r = *static_cast<Reader *>(g->UserData);
                size_t take = std::min(static_cast<size_t>(n), r.data.size() - r.at);
                std::copy_n(r.data.data() + r.at, take, out);
                r.at += take;
                return static_cast<int>(take);
            },
            &code);
        Image img;
        if (!gif) {
            error = GifErrorString(code);
            return img;
        }
        if (!valid_dimensions(gif->SWidth, gif->SHeight)) {
            error = "Image dimensions are too large";
            DGifCloseFile(gif, &code);
            return img;
        }
        GraphicsControlBlock gcb{.TransparentColor = NO_TRANSPARENT_COLOR};
        bool got_frame = false;
        GifRecordType rec = TERMINATE_RECORD_TYPE;
        while (DGifGetRecordType(gif, &rec) != GIF_ERROR && rec != TERMINATE_RECORD_TYPE) {
            if (rec == EXTENSION_RECORD_TYPE) {
                int ext_code = 0;
                GifByteType *ext = nullptr;
                if (DGifGetExtension(gif, &ext_code, &ext) == GIF_ERROR) {
                    break;
                }
                while (ext) {
                    if (ext_code == GRAPHICS_EXT_FUNC_CODE) {
                        DGifExtensionToGCB(ext[0], ext + 1, &gcb);
                    }
                    if (DGifGetExtensionNext(gif, &ext) == GIF_ERROR) {
                        break;
                    }
                }
            } else if (rec == IMAGE_DESC_RECORD_TYPE) {
                if (DGifGetImageDesc(gif) == GIF_ERROR) {
                    break;
                }
                const GifImageDesc &d = gif->Image;
                if (d.Width <= 0 || d.Height <= 0 || static_cast<uint32_t>(d.Width) > max_image_dim || static_cast<uint32_t>(d.Height) > max_image_dim) {
                    error = "Invalid GIF frame";
                    break;
                }
                const ColorMapObject *map = d.ColorMap ? d.ColorMap : gif->SColorMap;
                img = {static_cast<uint32_t>(gif->SWidth), static_cast<uint32_t>(gif->SHeight), std::vector<uint8_t>(static_cast<size_t>(gif->SWidth) * gif->SHeight * 4)};
                std::vector<GifPixelType> line(static_cast<size_t>(d.Width));
                auto put_row = [&](int y) {
                    const int sy = d.Top + y;
                    if (!map || sy < 0 || sy >= gif->SHeight) {
                        return;
                    }
                    for (int x = 0; x < d.Width; ++x) {
                        const int sx = d.Left + x, index = line[static_cast<size_t>(x)];
                        if (sx < 0 || sx >= gif->SWidth || index == gcb.TransparentColor || index >= map->ColorCount) {
                            continue;
                        }
                        uint8_t *o = &img.px[(static_cast<size_t>(sy) * img.w + static_cast<size_t>(sx)) * 4];
                        const GifColorType &c = map->Colors[index];
                        o[0] = c.Red, o[1] = c.Green, o[2] = c.Blue, o[3] = 255;
                    }
                };
                bool line_ok = false;
                if (d.Interlace) {
                    constexpr int starts[]{0, 4, 2, 1}, steps[]{8, 8, 4, 2};
                    for (int pass = 0; pass < 4; ++pass) {
                        for (int y = starts[pass]; y < d.Height; y += steps[pass]) {
                            if (DGifGetLine(gif, line.data(), d.Width) == GIF_ERROR) {
                                pass = 4;
                                break;
                            }
                            line_ok = true;
                            put_row(y);
                        }
                    }
                } else {
                    for (int y = 0; y < d.Height; ++y) {
                        if (DGifGetLine(gif, line.data(), d.Width) == GIF_ERROR) {
                            break;
                        }
                        line_ok = true;
                        put_row(y);
                    }
                }
                if (line_ok) {
                    got_frame = true;
                } else {
                    img = {};
                }
                break;
            }
        }
        if (!got_frame && error.empty()) {
            error = gif->Error ? GifErrorString(gif->Error) : "Empty GIF image";
        }
        DGifCloseFile(gif, &code);
        return img;
    }

    static Image decode(std::span<const uint8_t> data, uint32_t size, bool whole, std::string &error) {
        bool png = data.size() > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
        bool webp = data.size() >= 12 && std::equal(data.begin(), data.begin() + 4, "RIFF") && std::equal(data.begin() + 8, data.begin() + 12, "WEBP");
        bool gif = data.size() >= 6 && std::equal(data.begin(), data.begin() + 3, "GIF");
        return png ? decode_png(data, size, whole, error) : webp ? decode_webp(data, size, whole, error) : gif ? decode_gif(data, error) : decode_jpeg(data, size, whole, error);
    }

    Image decode_square(std::span<const uint8_t> data, uint32_t size, std::string &error) {
        Image img = decode(data, size, false, error);
        if (!img.w) {
            return img;
        }
        img = crop_square(std::move(img));
        if (img.w > size) {
            img = downscale(img, size, size);
        }
        return img;
    }

    Image decode_fit(std::span<const uint8_t> data, uint32_t size, std::string &error) {
        Image img = decode(data, size, true, error);
        if (img.w > size || img.h > size) {
            float k = static_cast<float>(size) / static_cast<float>(std::max(img.w, img.h));
            img = downscale(img, std::max(1u, static_cast<uint32_t>(std::lround(img.w * k))), std::max(1u, static_cast<uint32_t>(std::lround(img.h * k))));
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
