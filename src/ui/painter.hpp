#pragma once
#include <array>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "gfx/renderer.hpp"
#include "text/atlas.hpp"
#include "text/shaper.hpp"
#include "ui/theme.hpp"

namespace lyren::ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
};

struct TextStyle {
    float size = 14;
    Color color;
    uint8_t features = 0;
};

// Turns logical-pixel drawing calls into renderer primitives in buffer pixels. Rectangle edges and text
// baselines snap to whole buffer pixels so edges stay sharp at fractional scales.
class Painter {
public:
    Painter(text::Atlas& atlas, text::Shaper& shaper, text::Fonts& fonts) : atlas_(atlas), shaper_(shaper), fonts_(fonts) {}

    void begin(float scale, float origin_x, float origin_y);
    std::span<const gfx::Prim> prims() const { return prims_; }
    float scale() const { return scale_; }

    void rect(Rect r, Color fill, float radius = 0);
    void rect(Rect r, Color fill, float radius, Color border, float border_width);
    void rect(Rect r, Color fill, const float radii[4]);
    void gradient(Rect r, Color top, Color bottom, float radius = 0);
    void shadow(Rect r, float radius, float blur, Color color, bool cutout = false);
    void icon(std::string_view name, Rect r, Color color);
    // uv selects the part of the texture that fills r.
    void image(Rect r, gfx::TextureId tex, float radius, Color tint = {1, 1, 1, 1}, Rect uv = {0, 0, 1, 1});
    // Arc inscribed in r; angles in radians, clockwise from 12 o'clock.
    void arc(Rect r, float thickness, float start, float sweep, Color color);
    // Sine line from x to x + w around y; phase in radians at x.
    void wave(float x, float y, float w, float thickness, float amplitude, float wavelength, float phase, Color color);

    // Draws one line with its baseline at y, ellipsized to max_width. Returns the drawn width.
    float text(float x, float baseline, std::string_view s, const TextStyle& style, float max_width = std::numeric_limits<float>::infinity());
    // Draws text vertically centred in r.
    float text_in(Rect r, std::string_view s, const TextStyle& style);
    float measure(std::string_view s, const TextStyle& style);
    const text::Shaped& shape(std::string_view s, const TextStyle& style) { return shaper_.shape(s, style.features); }
    float ascent(float size) { return fonts_.face(0).ascender * size; }
    float descent(float size) { return fonts_.face(0).descender * size; }

    void push_clip(Rect r);
    void pop_clip();
    // Multiplies the alpha of everything drawn afterwards (fades of popups and toasts).
    void set_alpha(float a) { alpha_ = a; }
    float alpha() const { return alpha_; }

private:
    void premul(float out[4], Color c) const;
    gfx::Prim& emit(gfx::Kind kind, float x0, float y0, float x1, float y1);
    float snap(float v) const;
    void glyphs(const text::Shaped& shaped, size_t count, float x, float baseline, float size, Color color);

    text::Atlas& atlas_;
    text::Shaper& shaper_;
    text::Fonts& fonts_;
    std::vector<gfx::Prim> prims_;
    std::vector<std::array<float, 4>> clips_;
    float scale_ = 1, ox_ = 0, oy_ = 0, alpha_ = 1;
};

}  // namespace lyren::ui
