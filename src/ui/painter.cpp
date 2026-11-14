#include "ui/painter.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

#include "core/utf8.hpp"

namespace lyren::ui {

    void Painter::premul(std::array<float, 4> &out, Color c) const {
        float a = c.a * alpha_;
        out = {c.r * a, c.g * a, c.b * a, a};
    }

    void Painter::begin(float scale, float origin_x, float origin_y) {
        prims_.clear();
        deferred_.clear();
        deferring_ = false;
        clips_.assign(1, {0, 0, 1e9f, 1e9f});
        scale_ = scale, ox_ = origin_x, oy_ = origin_y, alpha_ = 1;
        zoom_ = 1;
        wipe_.reset();
        lifts_ = {};
    }

    float Painter::snap(float v) const {
        return std::round(v * scale_);
    }

    gfx::Prim &Painter::emit(gfx::Kind kind, float x0, float y0, float x1, float y1) {
        gfx::Prim &p = (deferring_ ? deferred_ : prims_).emplace_back();
        p.rect[0] = x0, p.rect[1] = y0, p.rect[2] = x1 - x0, p.rect[3] = y1 - y0;
        p.clip = clips_.back();
        p.info[0] = static_cast<uint32_t>(kind);
        return p;
    }

    gfx::Prim &Painter::emit_zoomed(gfx::Kind kind, float x0, float y0, float w, float h) {
        if (zoom_ == 1) {
            return emit(kind, x0, y0, x0 + w, y0 + h);
        }
        x0 = zx_ + (x0 - zx_) * zoom_, y0 = zy_ + (y0 - zy_) * zoom_;
        return emit(kind, x0, y0, x0 + w * zoom_, y0 + h * zoom_);
    }

    void Painter::rect(Rect r, Color fill, float radius) {
        rect(r, fill, radius, {}, 0);
    }

    void Painter::rect(Rect r, Color fill, float radius, Color border, float border_width) {
        float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        auto &p = emit(gfx::Kind::rect, x0, y0, x1, y1);
        premul(p.color, fill);
        premul(p.color2, border);
        p.radii.fill(radius * scale_);
        p.params[0] = border_width > 0 ? std::max(1.f, std::round(border_width * scale_)) : 0;
    }

    void Painter::rect(Rect r, Color fill, const std::array<float, 4> &radii) {
        float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        auto &p = emit(gfx::Kind::rect, x0, y0, x1, y1);
        premul(p.color, fill);
        std::ranges::transform(radii, p.radii.begin(), [this](float radius) {
            return radius * scale_;
        });
    }

    void Painter::gradient(Rect r, Color top, Color bottom, float radius) {
        float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        auto &p = emit(gfx::Kind::rect, x0, y0, x1, y1);
        premul(p.color, top);
        premul(p.color2, bottom);
        p.radii.fill(radius * scale_);
        p.info[2] = gfx::flag_gradient;
    }

    void Painter::hgradient(Rect r, Color left, Color right, float radius) {
        float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        auto &p = emit(gfx::Kind::rect, x0, y0, x1, y1);
        premul(p.color, left);
        premul(p.color2, right);
        p.radii.fill(radius * scale_);
        p.info[2] = gfx::flag_hgradient;
    }

    void Painter::shadow(Rect r, float radius, float blur, Color color, bool cutout) {
        auto &p = emit(gfx::Kind::shadow, (r.x + ox_) * scale_, (r.y + oy_) * scale_, (r.right() + ox_) * scale_, (r.bottom() + oy_) * scale_);
        premul(p.color, color);
        p.radii.fill(radius * scale_);
        p.params[1] = std::max(0.5f, blur * 0.5f * scale_);
        if (cutout) {
            p.info[2] = gfx::flag_cutout;
        }
    }

    void Painter::icon(std::string_view name, Rect r, Color color) {
        int size = static_cast<int>(std::lround(std::min(r.w, r.h) * scale_));
        const auto &e = atlas_.icon(name, size);
        if (!e.w) {
            return;
        }
        float x0 = snap(r.x + ox_ + (r.w - size / scale_) / 2), y0 = snap(r.y + oy_ + (r.h - size / scale_) / 2);
        auto &p = emit_zoomed(gfx::Kind::glyph, x0, y0, e.w, e.h);
        premul(p.color, color);
        float inv = 1.f / static_cast<float>(atlas_.size());
        p.uv[0] = e.x * inv, p.uv[1] = e.y * inv, p.uv[2] = (e.x + e.w) * inv, p.uv[3] = (e.y + e.h) * inv;
        p.info[1] = atlas_.texture();
    }

    void Painter::image(Rect r, gfx::TextureId tex, float radius, Color tint, Rect uv) {
        float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
        auto &p = emit(gfx::Kind::image, x0, y0, x1, y1);
        premul(p.color, tint);
        p.radii.fill(radius * scale_);
        p.uv[0] = uv.x, p.uv[1] = uv.y, p.uv[2] = uv.right(), p.uv[3] = uv.bottom();
        p.info[1] = tex;
    }

    void Painter::arc(Rect r, float thickness, float start, float sweep, Color color) {
        float x0 = (r.x + ox_) * scale_, y0 = (r.y + oy_) * scale_;
        auto &p = emit(gfx::Kind::arc, x0, y0, x0 + r.w * scale_, y0 + r.h * scale_);
        premul(p.color, color);
        p.params[0] = thickness * scale_, p.params[1] = start, p.params[2] = sweep;
    }

    void Painter::wave(Rect r, float filled, float amplitude, float wavelength, float phase, Color color, Color rest, Pluck pluck) {
        float y = r.y + r.h / 2, h = amplitude + std::abs(pluck.lift) + r.h;
        auto &p = emit(gfx::Kind::wave, (r.x + ox_) * scale_, (y + oy_ - h) * scale_, (r.right() + ox_) * scale_, (y + oy_ + h) * scale_);
        premul(p.color, color);
        premul(p.color2, rest);
        p.uv[0] = filled * scale_;
        p.params[0] = r.h * scale_, p.params[1] = amplitude * scale_, p.params[2] = wavelength * scale_, p.params[3] = phase;
        p.radii = {pluck.at * scale_, pluck.lift * scale_, std::max(pluck.spread, 1.f) * scale_, 0};
    }

    void Painter::glyphs(const text::Shaped &shaped, size_t count, float x, float baseline, const TextStyle &style) {
        const Color color = style.color;
        if (std::max(color.a, wipe_ ? wipe_->rest.a : 0.f) * alpha_ == 0) {
            return;
        }
        float px = style.size * scale_;
        float bx = (x + ox_) * scale_, by = snap(baseline + oy_);
        float inv = 1.f / static_cast<float>(atlas_.size());
        for (size_t i = 0; i < count; ++i) {
            const auto &g = shaped.glyphs[i];
            float pen = bx + g.x * px;
            float whole = std::floor(pen);
            int sub = std::min(3, static_cast<int>((pen - whole) * 4));
            const auto &e = atlas_.glyph(g.face, g.gid, px, sub);
            if (!e.w) {
                continue;
            }
            float x0 = whole + e.left, y0 = by - std::round(g.y * px) - e.top, gw = e.w, gh = e.h;
            if (!lifts_.empty()) {
                auto l = std::ranges::upper_bound(lifts_, g.cluster, {}, &Lift::from);
                if (l != lifts_.begin()) {
                    y0 -= l[-1].dy * scale_;
                    if (float swell = l[-1].scale; swell != 1) {
                        // The glyph keeps its raster and swells about its centre and the baseline.
                        gw *= swell, gh *= swell;
                        x0 += (e.w - gw) * 0.5f, y0 = by + (y0 - by) * swell;
                    }
                }
            }
            if (e.color) {
                // Emoji keep their own colours and take only the text's alpha, unless they are a silhouette.
                auto &p = emit_zoomed(gfx::Kind::image, x0, y0, gw, gh);
                premul(p.color, style.silhouette ? color : Color{1, 1, 1, color.a});
                float cinv = 1.f / static_cast<float>(atlas_.color_size());
                p.uv[0] = e.x * cinv, p.uv[1] = e.y * cinv, p.uv[2] = (e.x + e.w) * cinv, p.uv[3] = (e.y + e.h) * cinv;
                p.info[1] = atlas_.color_texture();
                continue;
            }
            auto &p = emit_zoomed(gfx::Kind::glyph, x0, y0, gw, gh);
            premul(p.color, color);
            p.uv[0] = e.x * inv, p.uv[1] = e.y * inv, p.uv[2] = (e.x + e.w) * inv, p.uv[3] = (e.y + e.h) * inv;
            p.info[1] = atlas_.texture();
            if (wipe_) {
                premul(p.color2, wipe_->rest);
                p.params[0] = zx_ + ((wipe_->at + ox_) * scale_ - zx_) * zoom_;
                p.params[1] = std::max(0.5f, wipe_->soft * scale_ * zoom_ / 2);
                p.info[2] = gfx::flag_gradient;
            }
        }
    }

    float Painter::text(float x, float baseline, std::string_view s, const TextStyle &style, float max_width) {
        const auto &shaped = shaper_.shape(s, style.features);
        float width = shaped.width * style.size;
        if (width <= max_width) {
            glyphs(shaped, shaped.glyphs.size(), x, baseline, style);
            return width;
        }
        // Keep whole clusters that fit before the ellipsis.
        const auto &dots = shaper_.shape("…");
        float limit = (max_width - dots.width * style.size) / style.size;
        size_t n = 0;
        while (n < shaped.glyphs.size()) {
            float end = n + 1 < shaped.glyphs.size() ? shaped.glyphs[n + 1].x : shaped.width;
            if (end > limit) {
                break;
            }
            ++n;
        }
        // A cluster may span several glyphs (marks); cut at its first glyph.
        while (n > 0 && n < shaped.glyphs.size() && shaped.glyphs[n].cluster == shaped.glyphs[n - 1].cluster) {
            --n;
        }
        float cut = n < shaped.glyphs.size() ? shaped.glyphs[n].x : shaped.width;
        glyphs(shaped, n, x, baseline, style);
        glyphs(dots, dots.glyphs.size(), x + cut * style.size, baseline, style);
        return (cut + dots.width) * style.size;
    }

    float Painter::text_in(Rect r, std::string_view s, const TextStyle &style) {
        float a = ascent(style.size), d = descent(style.size);
        return text(r.x, r.y + (r.h - a - d) / 2 + a, s, style, r.w);
    }

    float Painter::measure(std::string_view s, const TextStyle &style) {
        return shaper_.shape(s, style.features).width * style.size;
    }

    void Painter::push_clip(Rect r) {
        auto &c = clips_.back();
        clips_.push_back({std::max(c[0], snap(r.x + ox_)), std::max(c[1], snap(r.y + oy_)), std::min(c[2], snap(r.right() + ox_)), std::min(c[3], snap(r.bottom() + oy_))});
    }

    void Painter::pop_clip() {
        clips_.pop_back();
    }

    // Greedy line breaking at spaces and around CJK characters, from a single shaping pass. The lines point into s.
    std::vector<std::string_view> Painter::wrap(std::string_view s, float size, float width, uint8_t features) {
        const text::Shaped &sh = shape(s, {.size = size, .features = features});
        if (sh.width * size <= width || sh.glyphs.size() < 2) {
            return {s};
        }
        auto at = [&](uint32_t byte) {
            size_t i = byte;
            return utf8::decode(s, i);
        };
        auto wide = [&](uint32_t byte) {
            return at(byte) >= 0x2e80;
        };
        // Closing punctuation stays at the end of the line before.
        auto closing = [&](uint32_t byte) {
            return std::u32string_view(U"，。、；：？！）」』》〉】”’…").contains(at(byte));
        };
        std::vector<std::string_view> out;
        auto emit = [&](size_t from, size_t to) {
            std::string_view v = s.substr(from, to - from);
            while (v.ends_with(' ')) {
                v.remove_suffix(1);
            }
            while (v.starts_with(' ')) {
                v.remove_prefix(1);
            }
            if (!v.empty()) {
                out.push_back(v);
            }
        };
        const auto &g = sh.glyphs;
        size_t start = 0, candidate = 0;
        for (size_t i = 1; i < g.size(); ++i) {
            uint32_t c = g[i].cluster, pc = g[i - 1].cluster;
            if (c != pc && c > 0 && (s[c - 1] == ' ' || wide(c) || wide(pc)) && !closing(c)) {
                candidate = i;
            }
            float end = (i + 1 < g.size() ? g[i + 1].x : sh.width) - g[start].x;
            if (end * size > width && candidate > start) {
                emit(g[start].cluster, g[candidate].cluster);
                start = candidate;
            }
        }
        emit(g[start].cluster, s.size());
        return out;
    }

    std::vector<std::string_view> Painter::wrap_lines(std::string_view s, float size, float width, uint8_t features) {
        std::vector<std::string_view> out;
        for (auto part : std::views::split(s, '\n')) {
            auto lines = wrap(std::string_view(part.begin(), part.end()), size, width, features);
            out.insert(out.end(), lines.begin(), lines.end());
        }
        return out;
    }

} // namespace lyren::ui
