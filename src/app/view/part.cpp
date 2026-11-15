#include "app/view/part.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    namespace ease = ui::ease;
    namespace type = ui::type;

    namespace {
        // Track rows as the track list lays them out, in r below its header.
        void skeleton_rows(ui::Ui &ui, Rect r) {
            constexpr float row_h = 56;
            const TrackColumns head = track_columns(ui::cut_top(r, 38));
            const float radius = ui.theme().radius_s();
            for (int i = 0; r.y + static_cast<float>(i) * row_h < r.bottom(); ++i) {
                const float y = r.y + static_cast<float>(i) * row_h;
                // Each row its own lengths, so the list reads as text rather than as stripes.
                const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f);
                ui.skeleton({head.index.x + 14, y + 23, 12, 10}, 5);
                ui.skeleton({head.title.x, y + 8, 40, 40}, radius);
                const float tw = head.title.w - 62;
                ui.skeleton({head.title.x + 54, y + 13, tw * (0.4f + 0.3f * k), 12}, 6);
                ui.skeleton({head.title.x + 54, y + 33, tw * (0.22f + 0.18f * (1 - k)), 10}, 5);
                if (head.album.w > 0) {
                    ui.skeleton({head.album.x, y + 23, head.album.w * (0.35f + 0.3f * k), 10}, 5);
                }
                ui.skeleton({head.duration.right() - 32, y + 23, 32, 10}, 5);
            }
        }

        // Cards as the card grid lays them out from r's top-left; round ones are artists and listeners.
        void skeleton_cards(ui::Ui &ui, Rect r, bool round) {
            const float min_w = round ? 130 : 150, gap = CardGrid::gap;
            const int cols = CardGrid::columns(r.w, min_w);
            const float cw = (r.w + gap) / static_cast<float>(cols) - gap, ch = cw + CardGrid::caption;
            for (int i = 0; r.y + static_cast<float>(i / cols) * (ch + gap) < r.bottom(); ++i) {
                const Rect c{r.x + static_cast<float>(i % cols) * (cw + gap), r.y + static_cast<float>(i / cols) * (ch + gap), cw, ch};
                const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f), tw = cw * (0.5f + 0.35f * k);
                ui.skeleton({c.x, c.y, cw, cw}, round ? cw / 2 : ui.theme().radius);
                ui.skeleton({round ? c.x + (cw - tw) / 2 : c.x, c.y + cw + 14, tw, 12}, 6);
                ui.skeleton({round ? c.x + (cw - tw * 0.6f) / 2 : c.x, c.y + cw + 35, tw * 0.6f, 10}, 5);
            }
        }
    } // namespace

    Part::Part(Shell &shell)
    : shell_(shell),
      env_(shell.env),
      playback_(shell.playback),
      library_(shell.library),
      social_(shell.social),
      comments_(shell.comments),
      browser_(shell.browser),
      accounts_(shell.accounts) {}

    void Part::cover(ui::Ui &ui, Rect r, const std::string &url, float radius, std::string_view placeholder) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const int px = Images::tier(r.w * p.scale());
        const auto &e = env_.images.get(url, static_cast<uint32_t>(px));
        const float shown = e.ready ? ui.progress(e.ready_at, 320, ease::standard) : 0;
        if (shown < 1) {
            p.rect(r, t.fill(1.4f), radius);
            if (!placeholder.empty() && r.w >= 28) {
                const float s = std::round(std::min(r.w, r.h) * 0.36f);
                p.icon(placeholder, ui::center(r, s, s), t.text_dim.with_alpha(0.5f * (1 - shown)));
            }
        }
        if (e.ready) {
            Fade fade(p, shown);
            p.image(r, e.tex, radius);
        }
    }

    float Part::cover_hue(const std::string &url, float size) {
        const auto &e = env_.images.get(url, static_cast<uint32_t>(Images::tier(size)));
        return e.ready ? e.hue : e.failed ? -1 : -2;
    }

    void Part::empty(ui::Ui &ui, Rect r, std::string_view icon, std::string_view title, std::string_view detail) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Rect c = ui::center(r, std::min(r.w, 360.f), detail.empty() ? 92.f : 116.f);
        const Rect disc{c.x + (c.w - 48) / 2, c.y, 48, 48};
        p.rect(disc, t.fill(), 24);
        p.icon(icon, ui::center(disc, 20, 20), t.text_dim);
        ui.label({c.x, disc.bottom() + 16, c.w, 22}, title, t.style(type::lead, t.text), Align::center);
        if (!detail.empty()) {
            ui.label({c.x, disc.bottom() + 42, c.w, 20}, detail, t.style(type::label, t.text_dim), Align::center);
        }
    }

    bool Part::page_status(ui::Ui &ui, Rect r, bool loading, const std::string &error, bool needs_login, Placeholder look) {
        const auto &t = ui.theme();
        if (needs_login) {
            const Rect c = ui::center(r, std::min(r.w, 360.f), 168);
            empty(ui, {c.x, c.y, c.w, 116}, "user", lyren::tr("Sign in to use this page"), lyren::tr(platform(browser_.active()).name));
            const bool qr = provider().capabilities.qr_login;
            const auto label = lyren::tr(qr ? "Scan to sign in" : "Connect account");
            const float bw = ui.button_width(label, true);
            if (ui.button(ui.id("page-login"), {c.x + (c.w - bw) / 2, c.bottom() - ui::control::large, bw, ui::control::large}, label, ui::ButtonKind::primary,
                          qr ? "qr-code" : "link")) {
                shell_.account.open(ui);
                accounts_.begin(browser_.active());
            }
            return true;
        }
        if (loading) {
            ui.push_clip(r);
            switch (look) {
                case Placeholder::spinner:
                    ui.spinner(ui::center(r, 24, 24), t.text_dim, 2);
                    break;
                case Placeholder::rows:
                    skeleton_rows(ui, r);
                    break;
                case Placeholder::cards:
                case Placeholder::faces:
                    skeleton_cards(ui, r, look == Placeholder::faces);
                    break;
            }
            ui.pop_clip();
            return true;
        }
        if (!error.empty()) {
            empty(ui, r, "cloud-off", lyren::tr("Could not load this"), lyren::tr(error));
            return true;
        }
        return false;
    }

    float Part::heart_size(ui::Ui &ui, const Track &track, float size) const {
        const float motion = ui.theme().motion;
        if (motion == 0) {
            return size;
        }
        const float k = static_cast<float>(ui.now - library_.liked_at(track)) / (0.45f * motion);
        if (!(k >= 0 && k < 1)) {
            return size;
        }
        ui.keep_animating();
        // Up past its size and back, then a little under it.
        const float pi = std::numbers::pi_v<float>;
        return size * (k < 0.55f ? 1 + 0.32f * std::sin(pi * k / 0.55f) : 1 - 0.06f * std::sin(pi * (k - 0.55f) / 0.45f));
    }

    bool Part::heart(ui::Ui &ui, ui::Id id, Rect r, const Track &track, float size) {
        if (!provider(track).capabilities.likes) {
            return false;
        }
        const bool liked = library_.liked(track);
        if (ui.icon_button(id, r, liked ? "heart-fill" : "heart", heart_size(ui, track, size), liked, lyren::tr(liked ? "Unlike" : "Like"))) {
            library_.toggle_like(track);
        }
        return true;
    }

} // namespace lyren
