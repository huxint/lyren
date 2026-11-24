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
        struct BoxSpan {
            uint32_t i0 = 0, i1 = 0;
            float w0 = 0, w1 = 0;
        };

        struct TargetBox {
            uint32_t x0 = 0, y0 = 0, cw = 0, ch = 0, out_w = 0, out_h = 0;
        };

        TargetBox compute_target_box(uint32_t w, uint32_t h, uint32_t target, bool whole) {
            const uint32_t cw = whole ? w : std::min(w, h), ch = whole ? h : cw;
            const uint32_t x0 = (w - cw) / 2, y0 = (h - ch) / 2;
            uint32_t out_w = cw, out_h = ch;
            if (!whole && cw > target) {
                out_w = out_h = target;
            } else if (whole && (cw > target || ch > target)) {
                const float k = static_cast<float>(target) / static_cast<float>(std::max(cw, ch));
                out_w = std::max(1u, static_cast<uint32_t>(std::lround(cw * k)));
                out_h = std::max(1u, static_cast<uint32_t>(std::lround(ch * k)));
            }
            return {x0, y0, cw, ch, out_w, out_h};
        }

        std::vector<BoxSpan> build_box_spans(uint32_t src_n, uint32_t dst_n) {
            const double scale = static_cast<double>(src_n) / dst_n;
            std::vector<BoxSpan> spans(src_n);
            for (uint32_t c = 0; c < src_n; ++c) {
                const uint32_t i0 = std::min(dst_n - 1, static_cast<uint32_t>(c / scale));
                const double split = static_cast<double>(i0 + 1) * scale;
                if (i0 + 1 < dst_n && split < c + 1.0) {
                    spans[c] = {i0, i0 + 1, static_cast<float>(split - c), static_cast<float>(c + 1.0 - split)};
                } else {
                    spans[c] = {i0, i0, 1.f, 0.f};
                }
            }
            return spans;
        }

        // Two-row rolling box accumulator for sequential scanline downscaling (keeps accumulator memory at O(out_w)).
        struct RollingBoxScaler {
            uint32_t cw = 0, ch = 0, out_w = 0, out_h = 0, cur_j = 0;
            double sx = 1.0, sy = 1.0;
            float inv_area = 1.f;
            std::vector<BoxSpan> xspans;
            std::vector<float> row0, row1;

            RollingBoxScaler() = default;
            RollingBoxScaler(uint32_t cw_in, uint32_t ch_in, uint32_t ow, uint32_t oh)
            : cw(cw_in),
              ch(ch_in),
              out_w(ow),
              out_h(oh),
              sx(static_cast<double>(cw_in) / ow),
              sy(static_cast<double>(ch_in) / oh),
              inv_area(static_cast<float>(1.0 / (sx * sy))),
              xspans(build_box_spans(cw_in, ow)),
              row0(static_cast<size_t>(ow) * 4, 0.f),
              row1(static_cast<size_t>(ow) * 4, 0.f) {}

            void flush_row(uint8_t *dst, uint32_t j) {
                uint8_t *out = dst + static_cast<size_t>(j) * out_w * 4;
                for (size_t i = 0; i < row0.size(); ++i) {
                    out[i] = static_cast<uint8_t>(std::clamp(std::lround(row0[i] * inv_area), 0l, 255l));
                }
            }

            void advance_to(uint8_t *dst, uint32_t target_j) {
                while (cur_j < target_j) {
                    flush_row(dst, cur_j);
                    row0.swap(row1);
                    std::ranges::fill(row1, 0.f);
                    ++cur_j;
                }
            }

            void add_row(uint8_t *dst, uint32_t cy, const uint8_t *src_rgba) {
                const uint32_t j0 = std::min(out_h - 1, static_cast<uint32_t>(cy / sy));
                advance_to(dst, j0);
                const double ysplit = static_cast<double>(j0 + 1) * sy;
                float wy0 = 1.f, wy1 = 0.f;
                if (j0 + 1 < out_h && ysplit < cy + 1.0) {
                    wy0 = static_cast<float>(ysplit - cy);
                    wy1 = static_cast<float>(cy + 1.0 - ysplit);
                }
                for (uint32_t cx = 0; cx < cw; ++cx) {
                    const auto &xs = xspans[cx];
                    const uint8_t *p = src_rgba + static_cast<size_t>(cx) * 4;
                    if ((p[0] | p[1] | p[2] | p[3]) == 0) {
                        continue;
                    }
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

            void finish(uint8_t *dst) {
                advance_to(dst, out_h - 1);
                flush_row(dst, out_h - 1);
            }
        };

        struct PngState {
            std::span<const uint8_t> data;
            size_t at = 0;
            uint32_t target = 1;
            bool whole = false;
            Image img;
            std::string error;
            std::vector<uint8_t> row;
            std::vector<float> acc;
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
            png_set_alpha_mode(png, PNG_ALPHA_PNG, PNG_DEFAULT_sRGB);
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

            const TargetBox tb = compute_target_box(w, h, st.target, st.whole);
            st.row.resize(static_cast<size_t>(w) * 4);
            st.img = {tb.out_w, tb.out_h, std::vector<uint8_t>(static_cast<size_t>(tb.out_w) * tb.out_h * 4)};

            if (passes > 1) {
                // Adam7 partitions every 8x8 block into 7 disjoint pixel sets; accumulate each pass's own pixels directly
                // into the target-sized output without ever allocating a full w*h canvas.
                static constexpr std::array<uint32_t, 7> px0{0, 4, 0, 2, 0, 1, 0}, pdx{8, 8, 4, 4, 2, 2, 1};
                static constexpr std::array<uint32_t, 7> py0{0, 0, 4, 0, 2, 0, 1}, pdy{8, 8, 8, 4, 4, 2, 2};
                const bool direct = tb.out_w == tb.cw && tb.out_h == tb.ch;
                std::vector<BoxSpan> xspans, yspans;
                if (!direct) {
                    xspans = build_box_spans(tb.cw, tb.out_w);
                    yspans = build_box_spans(tb.ch, tb.out_h);
                    st.acc.assign(static_cast<size_t>(tb.out_w) * tb.out_h * 4, 0.f);
                }
                for (int pass = 0; pass < passes; ++pass) {
                    const uint32_t x_init = px0[static_cast<size_t>(pass)], dx = pdx[static_cast<size_t>(pass)];
                    const uint32_t y_init = py0[static_cast<size_t>(pass)], dy = pdy[static_cast<size_t>(pass)];
                    for (uint32_t y = 0; y < h; ++y) {
                        png_read_row(png, st.row.data(), nullptr);
                        if (w <= x_init || y < y_init || (y - y_init) % dy != 0 || y < tb.y0 || y >= tb.y0 + tb.ch) {
                            continue;
                        }
                        const uint32_t cy = y - tb.y0;
                        uint32_t x = x_init;
                        if (x < tb.x0) {
                            x += ((tb.x0 - x + dx - 1) / dx) * dx;
                        }
                        if (direct) {
                            uint8_t *dst_row = st.img.px.data() + static_cast<size_t>(cy) * tb.out_w * 4;
                            for (; x < tb.x0 + tb.cw; x += dx) {
                                uint8_t *p = st.row.data() + static_cast<size_t>(x) * 4;
                                premultiply_rgba(p, 4);
                                std::copy_n(p, 4, dst_row + static_cast<size_t>(x - tb.x0) * 4);
                            }
                        } else {
                            const auto &ys = yspans[cy];
                            float *r0 = st.acc.data() + static_cast<size_t>(ys.i0) * tb.out_w * 4;
                            float *r1 = st.acc.data() + static_cast<size_t>(ys.i1) * tb.out_w * 4;
                            for (; x < tb.x0 + tb.cw; x += dx) {
                                uint8_t *p = st.row.data() + static_cast<size_t>(x) * 4;
                                premultiply_rgba(p, 4);
                                const auto &xs = xspans[x - tb.x0];
                                const float w00 = ys.w0 * xs.w0;
                                for (int c = 0; c < 4; ++c) {
                                    r0[xs.i0 * 4 + c] += p[c] * w00;
                                }
                                if (xs.w1 > 0.f) {
                                    const float w01 = ys.w0 * xs.w1;
                                    for (int c = 0; c < 4; ++c) {
                                        r0[xs.i1 * 4 + c] += p[c] * w01;
                                    }
                                }
                                if (ys.w1 > 0.f) {
                                    const float w10 = ys.w1 * xs.w0;
                                    for (int c = 0; c < 4; ++c) {
                                        r1[xs.i0 * 4 + c] += p[c] * w10;
                                    }
                                    if (xs.w1 > 0.f) {
                                        const float w11 = ys.w1 * xs.w1;
                                        for (int c = 0; c < 4; ++c) {
                                            r1[xs.i1 * 4 + c] += p[c] * w11;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (!direct) {
                    const float inv_area = static_cast<float>(static_cast<double>(tb.out_w) * tb.out_h / (static_cast<double>(tb.cw) * tb.ch));
                    for (size_t i = 0; i < st.acc.size(); ++i) {
                        st.img.px[i] = static_cast<uint8_t>(std::clamp(std::lround(st.acc[i] * inv_area), 0l, 255l));
                    }
                }
                return;
            }

            // Stream non-interlaced PNGs one scanline at a time directly into the cropped & 2-row rolling box-downsampled target.
            if (tb.out_w == tb.cw && tb.out_h == tb.ch) {
                for (uint32_t y = 0; y < tb.y0 + tb.ch; ++y) {
                    png_read_row(png, st.row.data(), nullptr);
                    if (y < tb.y0) {
                        continue;
                    }
                    uint8_t *src = st.row.data() + static_cast<size_t>(tb.x0) * 4;
                    premultiply_rgba(src, static_cast<size_t>(tb.cw) * 4);
                    std::copy_n(src, static_cast<size_t>(tb.cw) * 4, st.img.px.data() + static_cast<size_t>(y - tb.y0) * tb.out_w * 4);
                }
                return;
            }

            RollingBoxScaler scaler(tb.cw, tb.ch, tb.out_w, tb.out_h);
            for (uint32_t y = 0; y < tb.y0 + tb.ch; ++y) {
                png_read_row(png, st.row.data(), nullptr);
                if (y < tb.y0) {
                    continue;
                }
                uint8_t *src = st.row.data() + static_cast<size_t>(tb.x0) * 4;
                premultiply_rgba(src, static_cast<size_t>(tb.cw) * 4);
                scaler.add_row(st.img.px.data(), y - tb.y0, src);
            }
            scaler.finish(st.img.px.data());
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

    static Image decode_webp_frame(std::span<const uint8_t> data, int src_w, int src_h, uint32_t size, bool whole, std::string &error) {
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config)) {
            error = "Invalid WebP image";
            return {};
        }
        int out_w = src_w, out_h = src_h;
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
            return decode_webp_frame(data, features.width, features.height, size, whole, error);
        }

        // Extract only the first frame via WebPDemux so large animated WebP covers can prescale directly instead of
        // allocating full-resolution animation canvases.
        const WebPData source{data.data(), data.size()};
        WebPDemuxer *demux = WebPDemux(&source);
        if (!demux) {
            error = "Invalid WebP image";
            return {};
        }
        const int cw = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH));
        const int ch = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT));
        Image img;
        if (!valid_dimensions(cw, ch)) {
            error = "Image dimensions are too large";
            WebPDemuxDelete(demux);
            return img;
        }
        WebPIterator iter{};
        if (WebPDemuxGetFrame(demux, 1, &iter)) {
            const std::span<const uint8_t> frag{iter.fragment.bytes, iter.fragment.size};
            if (iter.x_offset == 0 && iter.y_offset == 0 && iter.width == cw && iter.height == ch) {
                img = decode_webp_frame(frag, cw, ch, size, whole, error);
            } else if (iter.width > 0 && iter.height > 0 && iter.x_offset >= 0 && iter.y_offset >= 0 && iter.x_offset + iter.width <= cw && iter.y_offset + iter.height <= ch) {
                const int side = whole ? std::max(cw, ch) : std::min(cw, ch);
                const int target = static_cast<int>(std::max(size, 1u)) * 2;
                const double k = side > std::max(target, 256) ? static_cast<double>(target) / side : 1.0;
                const int out_cw = std::max(1, static_cast<int>(std::lround(cw * k)));
                const int out_ch = std::max(1, static_cast<int>(std::lround(ch * k)));
                const int fw = std::max(1, static_cast<int>(std::lround(iter.width * k)));
                const int fh = std::max(1, static_cast<int>(std::lround(iter.height * k)));
                const int fx = std::clamp(static_cast<int>(std::lround(iter.x_offset * k)), 0, out_cw - 1);
                const int fy = std::clamp(static_cast<int>(std::lround(iter.y_offset * k)), 0, out_ch - 1);
                WebPDecoderConfig config;
                if (WebPInitDecoderConfig(&config)) {
                    if (fw != iter.width || fh != iter.height) {
                        config.options.use_scaling = 1;
                        config.options.scaled_width = fw;
                        config.options.scaled_height = fh;
                    }
                    std::vector<uint8_t> sub(static_cast<size_t>(fw) * fh * 4);
                    config.output.colorspace = MODE_rgbA;
                    config.output.u.RGBA.rgba = sub.data();
                    config.output.u.RGBA.stride = fw * 4;
                    config.output.u.RGBA.size = sub.size();
                    config.output.is_external_memory = 1;
                    const VP8StatusCode st = WebPDecode(frag.data(), frag.size(), &config);
                    WebPFreeDecBuffer(&config.output);
                    if (st == VP8_STATUS_OK) {
                        img = {static_cast<uint32_t>(out_cw), static_cast<uint32_t>(out_ch), std::vector<uint8_t>(static_cast<size_t>(out_cw) * out_ch * 4, 0)};
                        const int copy_w = std::min(fw, out_cw - fx), copy_h = std::min(fh, out_ch - fy);
                        for (int r = 0; r < copy_h; ++r) {
                            std::copy_n(sub.data() + static_cast<size_t>(r) * fw * 4, static_cast<size_t>(copy_w) * 4,
                                        img.px.data() + (static_cast<size_t>(fy + r) * out_cw + fx) * 4);
                        }
                    } else {
                        error = "Failed to decode WebP image";
                    }
                }
            } else {
                error = "Invalid WebP frame bounds";
            }
            WebPDemuxReleaseIterator(&iter);
        } else {
            error = "Failed to decode WebP image";
        }
        WebPDemuxDelete(demux);
        return img;
    }

    // The first frame on the logical screen, streamed directly into the cropped & box-downsampled target.
    static Image decode_gif(std::span<const uint8_t> data, uint32_t size, bool whole, std::string &error) {
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
        const TargetBox tb = compute_target_box(static_cast<uint32_t>(gif->SWidth), static_cast<uint32_t>(gif->SHeight), std::max(size, 1u), whole);
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
                const bool direct = tb.out_w == tb.cw && tb.out_h == tb.ch;
                img = {tb.out_w, tb.out_h, std::vector<uint8_t>(static_cast<size_t>(tb.out_w) * tb.out_h * 4, 0)};
                std::vector<GifPixelType> line(static_cast<size_t>(d.Width));
                std::vector<uint8_t> rgba_row(direct ? 0 : static_cast<size_t>(tb.cw) * 4);
                auto expand_crop_row = [&](uint8_t *dst) {
                    std::fill_n(dst, static_cast<size_t>(tb.cw) * 4, uint8_t{0});
                    if (!map) {
                        return;
                    }
                    const int x_begin = std::max(0, static_cast<int>(tb.x0) - d.Left);
                    const int x_end = std::min(d.Width, static_cast<int>(tb.x0 + tb.cw) - d.Left);
                    for (int x = x_begin; x < x_end; ++x) {
                        const int index = line[static_cast<size_t>(x)];
                        if (index == gcb.TransparentColor || index >= map->ColorCount) {
                            continue;
                        }
                        const auto cx = static_cast<size_t>(d.Left + x - static_cast<int>(tb.x0));
                        uint8_t *o = dst + cx * 4;
                        const GifColorType &c = map->Colors[index];
                        o[0] = c.Red, o[1] = c.Green, o[2] = c.Blue, o[3] = 255;
                    }
                };
                bool line_ok = false;
                if (d.Interlace) {
                    std::vector<BoxSpan> xspans, yspans;
                    std::vector<float> acc;
                    if (!direct) {
                        xspans = build_box_spans(tb.cw, tb.out_w);
                        yspans = build_box_spans(tb.ch, tb.out_h);
                        acc.assign(static_cast<size_t>(tb.out_w) * tb.out_h * 4, 0.f);
                    }
                    constexpr int starts[]{0, 4, 2, 1}, steps[]{8, 8, 4, 2};
                    for (int pass = 0; pass < 4; ++pass) {
                        for (int y = starts[pass]; y < d.Height; y += steps[pass]) {
                            if (DGifGetLine(gif, line.data(), d.Width) == GIF_ERROR) {
                                pass = 4;
                                break;
                            }
                            line_ok = true;
                            const int sy = d.Top + y;
                            if (sy < static_cast<int>(tb.y0) || sy >= static_cast<int>(tb.y0 + tb.ch)) {
                                continue;
                            }
                            const uint32_t cy = static_cast<uint32_t>(sy) - tb.y0;
                            if (direct) {
                                expand_crop_row(img.px.data() + static_cast<size_t>(cy) * tb.out_w * 4);
                            } else {
                                expand_crop_row(rgba_row.data());
                                const auto &ys = yspans[cy];
                                float *r0 = acc.data() + static_cast<size_t>(ys.i0) * tb.out_w * 4;
                                float *r1 = acc.data() + static_cast<size_t>(ys.i1) * tb.out_w * 4;
                                for (uint32_t cx = 0; cx < tb.cw; ++cx) {
                                    const uint8_t *p = rgba_row.data() + static_cast<size_t>(cx) * 4;
                                    if (!p[3]) {
                                        continue;
                                    }
                                    const auto &xs = xspans[cx];
                                    const float w00 = ys.w0 * xs.w0;
                                    for (int c = 0; c < 4; ++c) {
                                        r0[xs.i0 * 4 + c] += p[c] * w00;
                                    }
                                    if (xs.w1 > 0.f) {
                                        const float w01 = ys.w0 * xs.w1;
                                        for (int c = 0; c < 4; ++c) {
                                            r0[xs.i1 * 4 + c] += p[c] * w01;
                                        }
                                    }
                                    if (ys.w1 > 0.f) {
                                        const float w10 = ys.w1 * xs.w0;
                                        for (int c = 0; c < 4; ++c) {
                                            r1[xs.i0 * 4 + c] += p[c] * w10;
                                        }
                                        if (xs.w1 > 0.f) {
                                            const float w11 = ys.w1 * xs.w1;
                                            for (int c = 0; c < 4; ++c) {
                                                r1[xs.i1 * 4 + c] += p[c] * w11;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (line_ok && !direct) {
                        const float inv_area = static_cast<float>(static_cast<double>(tb.out_w) * tb.out_h / (static_cast<double>(tb.cw) * tb.ch));
                        for (size_t i = 0; i < acc.size(); ++i) {
                            img.px[i] = static_cast<uint8_t>(std::clamp(std::lround(acc[i] * inv_area), 0l, 255l));
                        }
                    }
                } else {
                    RollingBoxScaler scaler;
                    if (!direct) {
                        scaler = RollingBoxScaler(tb.cw, tb.ch, tb.out_w, tb.out_h);
                    }
                    for (int y = 0; y < d.Height; ++y) {
                        if (DGifGetLine(gif, line.data(), d.Width) == GIF_ERROR) {
                            break;
                        }
                        line_ok = true;
                        const int sy = d.Top + y;
                        if (sy < static_cast<int>(tb.y0) || sy >= static_cast<int>(tb.y0 + tb.ch)) {
                            continue;
                        }
                        const uint32_t cy = static_cast<uint32_t>(sy) - tb.y0;
                        if (direct) {
                            expand_crop_row(img.px.data() + static_cast<size_t>(cy) * tb.out_w * 4);
                        } else {
                            expand_crop_row(rgba_row.data());
                            scaler.add_row(img.px.data(), cy, rgba_row.data());
                        }
                    }
                    if (line_ok && !direct) {
                        scaler.finish(img.px.data());
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
        return png    ? decode_png(data, size, whole, error)
               : webp ? decode_webp(data, size, whole, error)
               : gif  ? decode_gif(data, size, whole, error)
                      : decode_jpeg(data, size, whole, error);
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
