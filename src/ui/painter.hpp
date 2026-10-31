#pragma once
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
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
        float right() const {
            return x + w;
        }
        float bottom() const {
            return y + h;
        }
        bool contains(float px, float py) const {
            return px >= x && py >= y && px < x + w && py < y + h;
        }
        Rect inset(float d) const {
            return {x + d, y + d, w - 2 * d, h - 2 * d};
        }
        Rect inset(float dx, float dy) const {
            return {x + dx, y + dy, w - 2 * dx, h - 2 * dy};
        }
    };

    struct TextStyle {
        float size = 14;
        Color color;
        uint8_t features = 0;
        bool silhouette = false; // colour glyphs (emoji) take the colour as a solid shape, for outlines
    };

    // Turns logical-pixel drawing calls into renderer primitives in buffer pixels. Rectangle edges and text
    // baselines snap to whole buffer pixels so edges stay sharp at fractional scales.
    class Painter {
    public:
        Painter(text::Atlas &atlas, text::Shaper &shaper, text::Fonts &fonts)
        : atlas_(atlas),
          shaper_(shaper),
          fonts_(fonts) {}

        void begin(float scale, float origin_x, float origin_y);
        std::span<const gfx::Prim> prims() const {
            return prims_;
        }
        float scale() const {
            return scale_;
        }

        void rect(Rect r, Color fill, float radius = 0);
        void rect(Rect r, Color fill, float radius, Color border, float border_width);
        void rect(Rect r, Color fill, const std::array<float, 4> &radii);
        void gradient(Rect r, Color top, Color bottom, float radius = 0);
        void hgradient(Rect r, Color left, Color right, float radius = 0);
        void shadow(Rect r, float radius, float blur, Color color, bool cutout = false);
        void icon(std::string_view name, Rect r, Color color);
        // uv selects the part of the texture that fills r.
        void image(Rect r, gfx::TextureId tex, float radius, Color tint = {1, 1, 1, 1}, Rect uv = {0, 0, 1, 1});
        // Arc inscribed in r; angles in radians, clockwise from 12 o'clock.
        void arc(Rect r, float thickness, float start, float sweep, Color color);
        // One progress stroke in r: a tapered sine in the played part and a flat tail. phase is in radians.
        void wave(Rect r, float filled, float amplitude, float wavelength, float phase, Color color, Color rest);

        // Draws one line with its baseline at y, ellipsized to max_width. Returns the drawn width.
        float text(float x, float baseline, std::string_view s, const TextStyle &style, float max_width = std::numeric_limits<float>::infinity());
        // Draws text vertically centred in r.
        float text_in(Rect r, std::string_view s, const TextStyle &style);
        float measure(std::string_view s, const TextStyle &style);
        // Display text wraps at word and CJK boundaries; returned views borrow s.
        std::vector<std::string_view> wrap(std::string_view s, float size, float width);
        // Explicit line breaks and blank lines stay.
        std::vector<std::string_view> wrap_lines(std::string_view s, float size, float width);
        const text::Shaped &shape(std::string_view s, const TextStyle &style) {
            return shaper_.shape(s, style.features);
        }
        float ascent(float size) {
            return fonts_.face(0).ascender * size;
        }
        float descent(float size) {
            return fonts_.face(0).descender * size;
        }

        // Primitives drawn while deferring are held back until draw_deferred(), so they land above what is drawn in
        // between while their widgets still take input first (the title bar over the now-playing stage).
        void defer(bool on) {
            deferring_ = on;
        }
        void draw_deferred() {
            prims_.insert(prims_.end(), deferred_.begin(), deferred_.end());
            deferred_.clear();
        }

        // Where the next primitive goes, and moving the ones drawn since a mark above everything drawn after them: for
        // a floating layer laid out before what it covers, so that it takes input first, and painted over it.
        size_t mark() const {
            return prims_.size();
        }
        void raise(size_t from, size_t to) {
            std::rotate(prims_.begin() + static_cast<std::ptrdiff_t>(from), prims_.begin() + static_cast<std::ptrdiff_t>(to), prims_.end());
        }

        void push_clip(Rect r);
        void pop_clip();
        // Multiplies the alpha of everything drawn afterwards (fades of popups and toasts).
        void set_alpha(float a) {
            alpha_ = a;
        }
        float alpha() const {
            return alpha_;
        }
        // Text drawn afterwards keeps its colour left of x = at and turns into `rest` right of it, blending across
        // `soft` px (the sung and unsung parts of a lyric line).
        void set_wipe(float at, float soft, Color rest) {
            wipe_ = Wipe{at, soft, rest};
        }
        void clear_wipe() {
            wipe_.reset();
        }
        // Text and icons drawn afterwards are scaled by s about (x, y), coordinates given to them and to set_wipe
        // included. They keep the raster of their unscaled size and skip pixel snapping, so text that resizes over a
        // few frames does not rasterize new glyphs every frame. 1 turns it off.
        void set_zoom(float s, float x = 0, float y = 0) {
            zoom_ = s, zx_ = (x + ox_) * scale_, zy_ = (y + oy_) * scale_;
        }
        // Glyphs of text drawn afterwards rise by the dy of the last entry whose byte offset in that text they have
        // reached (lyric words lifting as they are sung). Entries are in text order; the span must outlive the drawing.
        struct Lift {
            uint32_t from;
            float dy;
            float scale = 1; // and the glyph swells by this much about its own centre and the baseline
        };
        void set_lifts(std::span<const Lift> lifts) {
            lifts_ = lifts;
        }

    private:
        void premul(std::array<float, 4> &out, Color c) const;
        gfx::Prim &emit(gfx::Kind kind, float x0, float y0, float x1, float y1);
        gfx::Prim &emit_zoomed(gfx::Kind kind, float x0, float y0, float w, float h);
        float snap(float v) const;
        void glyphs(const text::Shaped &shaped, size_t count, float x, float baseline, const TextStyle &style);

        text::Atlas &atlas_;
        text::Shaper &shaper_;
        text::Fonts &fonts_;
        std::vector<gfx::Prim> prims_, deferred_;
        bool deferring_ = false;
        std::vector<std::array<float, 4>> clips_;
        float scale_ = 1, ox_ = 0, oy_ = 0, alpha_ = 1;
        float zoom_ = 1, zx_ = 0, zy_ = 0; // zx_, zy_ in buffer pixels
        struct Wipe {
            float at, soft;
            Color rest;
        };
        std::optional<Wipe> wipe_;
        std::span<const Lift> lifts_;
    };

} // namespace lyren::ui
