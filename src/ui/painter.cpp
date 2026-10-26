#include "ui/painter.hpp"

#include <algorithm>
#include <cmath>

namespace lyren::ui {

void Painter::premul(float out[4], Color c) const {
    float a = c.a * alpha_;
    out[0] = c.r * a, out[1] = c.g * a, out[2] = c.b * a, out[3] = a;
}

void Painter::begin(float scale, float origin_x, float origin_y) {
    prims_.clear();
    clips_.assign(1, {0, 0, 1e9f, 1e9f});
    scale_ = scale, ox_ = origin_x, oy_ = origin_y, alpha_ = 1;
    zoom_ = 1;
    wipe_.reset();
    lifts_ = {};
}

float Painter::snap(float v) const { return std::round(v * scale_); }

gfx::Prim& Painter::emit(gfx::Kind kind, float x0, float y0, float x1, float y1) {
    gfx::Prim& p = prims_.emplace_back();
    p.rect[0] = x0, p.rect[1] = y0, p.rect[2] = x1 - x0, p.rect[3] = y1 - y0;
    std::copy_n(clips_.back().data(), 4, p.clip);
    p.info[0] = uint32_t(kind);
    return p;
}

gfx::Prim& Painter::emit_zoomed(gfx::Kind kind, float x0, float y0, float w, float h) {
    if (zoom_ == 1) return emit(kind, x0, y0, x0 + w, y0 + h);
    x0 = zx_ + (x0 - zx_) * zoom_, y0 = zy_ + (y0 - zy_) * zoom_;
    return emit(kind, x0, y0, x0 + w * zoom_, y0 + h * zoom_);
}

void Painter::rect(Rect r, Color fill, float radius) { rect(r, fill, radius, {}, 0); }

void Painter::rect(Rect r, Color fill, float radius, Color border, float border_width) {
    float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
    if (x1 <= x0 || y1 <= y0) return;
    auto& p = emit(gfx::Kind::rect, x0, y0, x1, y1);
    premul(p.color, fill);
    premul(p.color2, border);
    std::fill_n(p.radii, 4, radius * scale_);
    p.params[0] = border_width > 0 ? std::max(1.f, std::round(border_width * scale_)) : 0;
}

void Painter::rect(Rect r, Color fill, const float radii[4]) {
    float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
    if (x1 <= x0 || y1 <= y0) return;
    auto& p = emit(gfx::Kind::rect, x0, y0, x1, y1);
    premul(p.color, fill);
    for (int i = 0; i < 4; ++i) p.radii[i] = radii[i] * scale_;
}

void Painter::gradient(Rect r, Color top, Color bottom, float radius) {
    float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
    if (x1 <= x0 || y1 <= y0) return;
    auto& p = emit(gfx::Kind::rect, x0, y0, x1, y1);
    premul(p.color, top);
    premul(p.color2, bottom);
    std::fill_n(p.radii, 4, radius * scale_);
    p.info[2] = gfx::flag_gradient;
}

void Painter::shadow(Rect r, float radius, float blur, Color color, bool cutout) {
    auto& p = emit(gfx::Kind::shadow, (r.x + ox_) * scale_, (r.y + oy_) * scale_, (r.right() + ox_) * scale_, (r.bottom() + oy_) * scale_);
    premul(p.color, color);
    std::fill_n(p.radii, 4, radius * scale_);
    p.params[1] = std::max(0.5f, blur * 0.5f * scale_);
    if (cutout) p.info[2] = gfx::flag_cutout;
}

void Painter::icon(std::string_view name, Rect r, Color color) {
    int size = int(std::lround(std::min(r.w, r.h) * scale_));
    const auto& e = atlas_.icon(name, size);
    if (!e.w) return;
    float x0 = snap(r.x + ox_ + (r.w - size / scale_) / 2), y0 = snap(r.y + oy_ + (r.h - size / scale_) / 2);
    auto& p = emit_zoomed(gfx::Kind::glyph, x0, y0, e.w, e.h);
    premul(p.color, color);
    float inv = 1.f / float(atlas_.size());
    p.uv[0] = e.x * inv, p.uv[1] = e.y * inv, p.uv[2] = (e.x + e.w) * inv, p.uv[3] = (e.y + e.h) * inv;
    p.info[1] = atlas_.texture();
}

void Painter::image(Rect r, gfx::TextureId tex, float radius, Color tint, Rect uv) {
    float x0 = snap(r.x + ox_), y0 = snap(r.y + oy_), x1 = snap(r.right() + ox_), y1 = snap(r.bottom() + oy_);
    auto& p = emit(gfx::Kind::image, x0, y0, x1, y1);
    premul(p.color, tint);
    std::fill_n(p.radii, 4, radius * scale_);
    p.uv[0] = uv.x, p.uv[1] = uv.y, p.uv[2] = uv.right(), p.uv[3] = uv.bottom();
    p.info[1] = tex;
}

void Painter::arc(Rect r, float thickness, float start, float sweep, Color color) {
    float x0 = (r.x + ox_) * scale_, y0 = (r.y + oy_) * scale_;
    auto& p = emit(gfx::Kind::arc, x0, y0, x0 + r.w * scale_, y0 + r.h * scale_);
    premul(p.color, color);
    p.params[0] = thickness * scale_, p.params[1] = start, p.params[2] = sweep;
}

void Painter::wave(Rect r, float filled, float amplitude, float wavelength, float phase, Color color, Color rest) {
    float y = r.y + r.h / 2, h = amplitude + r.h;
    auto& p = emit(gfx::Kind::wave, (r.x + ox_) * scale_, (y + oy_ - h) * scale_, (r.right() + ox_) * scale_, (y + oy_ + h) * scale_);
    premul(p.color, color);
    premul(p.color2, rest);
    p.uv[0] = filled * scale_;
    p.params[0] = r.h * scale_, p.params[1] = amplitude * scale_, p.params[2] = wavelength * scale_, p.params[3] = phase;
}

void Painter::glyphs(const text::Shaped& shaped, size_t count, float x, float baseline, float size, Color color) {
    if (std::max(color.a, wipe_ ? wipe_->rest.a : 0.f) * alpha_ == 0) return;
    float px = size * scale_;
    float bx = (x + ox_) * scale_, by = snap(baseline + oy_);
    float inv = 1.f / float(atlas_.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& g = shaped.glyphs[i];
        float pen = bx + g.x * px;
        float whole = std::floor(pen);
        int sub = std::min(3, int((pen - whole) * 4));
        const auto& e = atlas_.glyph(g.face, g.gid, px, sub);
        if (!e.w) continue;
        float x0 = whole + e.left, y0 = by - std::round(g.y * px) - e.top;
        if (!lifts_.empty()) {
            auto l = std::ranges::upper_bound(lifts_, g.cluster, {}, &Lift::from);
            if (l != lifts_.begin()) y0 -= l[-1].dy * scale_;
        }
        auto& p = emit_zoomed(gfx::Kind::glyph, x0, y0, e.w, e.h);
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

float Painter::text(float x, float baseline, std::string_view s, const TextStyle& style, float max_width) {
    const auto& shaped = shaper_.shape(s, style.features);
    float width = shaped.width * style.size;
    if (width <= max_width) {
        glyphs(shaped, shaped.glyphs.size(), x, baseline, style.size, style.color);
        return width;
    }
    // Keep whole clusters that fit before the ellipsis.
    const auto& dots = shaper_.shape("…");
    float limit = (max_width - dots.width * style.size) / style.size;
    size_t n = 0;
    while (n < shaped.glyphs.size()) {
        float end = n + 1 < shaped.glyphs.size() ? shaped.glyphs[n + 1].x : shaped.width;
        if (end > limit) break;
        ++n;
    }
    // A cluster may span several glyphs (marks); cut at its first glyph.
    while (n > 0 && n < shaped.glyphs.size() && shaped.glyphs[n].cluster == shaped.glyphs[n - 1].cluster) --n;
    float cut = n < shaped.glyphs.size() ? shaped.glyphs[n].x : shaped.width;
    glyphs(shaped, n, x, baseline, style.size, style.color);
    glyphs(dots, dots.glyphs.size(), x + cut * style.size, baseline, style.size, style.color);
    return (cut + dots.width) * style.size;
}

float Painter::text_in(Rect r, std::string_view s, const TextStyle& style) {
    float a = ascent(style.size), d = descent(style.size);
    return text(r.x, r.y + (r.h - a - d) / 2 + a, s, style, r.w);
}

float Painter::measure(std::string_view s, const TextStyle& style) { return shaper_.shape(s, style.features).width * style.size; }

void Painter::push_clip(Rect r) {
    auto& c = clips_.back();
    clips_.push_back({std::max(c[0], snap(r.x + ox_)), std::max(c[1], snap(r.y + oy_)), std::min(c[2], snap(r.right() + ox_)),
                      std::min(c[3], snap(r.bottom() + oy_))});
}

void Painter::pop_clip() { clips_.pop_back(); }

}  // namespace lyren::ui
