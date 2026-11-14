#include "app/shell.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <inplace_vector>

#include "app/draw.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;

    using ui::hsl;
    constexpr ui::Color white{1, 1, 1, 1};

    // Artwork for lists that have no cover of their own (liked, recent, cloud, daily): a gradient with two soft discs.
    static void art(ui::Painter &p, Rect r, float hue, float radius, std::string_view icon) {
        p.gradient(r, hsl(hue, 0.42f, 0.54f), hsl(hue + 0.08f, 0.4f, 0.22f), radius);
        float a = std::fmod(hue * 7.31f, 1.f), b = std::fmod(hue * 13.7f, 1.f);
        float d1 = r.w * (0.5f + 0.35f * a), d2 = r.w * (0.3f + 0.25f * b);
        p.rect({r.x + (r.w - d1) * b, r.y + (r.h - d1) * (1 - a), d1, d1}, hsl(hue + 0.1f, 0.5f, 0.72f).with_alpha(0.26f), d1 / 2);
        p.rect({r.x + (r.w - d2) * (1 - a), r.y + (r.h - d2) * b, d2, d2}, hsl(hue - 0.12f, 0.55f, 0.62f).with_alpha(0.42f), d2 / 2);
        float s = std::round(r.w * 0.36f);
        p.icon(icon, ui::center(r, s, s), white.with_alpha(0.85f));
    }

    struct Columns {
        Rect index, title, album, like, duration;
    };

    static Columns columns(Rect r) {
        Columns c;
        r = r.inset(12, 0);
        c.index = ui::cut_left(r, 40);
        ui::cut_left(r, 4);
        c.duration = ui::cut_right(r, 48);
        c.like = ui::cut_right(r, 44);
        if (r.w > 460) {
            c.album = ui::cut_right(r, std::round(r.w * 0.36f));
            ui::cut_right(r, 16);
        }
        c.title = r;
        return c;
    }

    // Animate row/column changes; ordinary resizing and scrolling stay under the pointer.
    struct Grid {
        static constexpr float gap = 20;
        static constexpr float reflow_ms = 280;
        ui::Ui &ui;
        ui::Id id;
        int cols;
        float cw, ch;
        Grid(ui::Ui &ui, ui::Id id, float width, float min_w = 150)
        : ui(ui),
          id(id) {
            cols = std::max(1, static_cast<int>((width + gap) / (min_w + gap)));
            float count = ui.spring(ui::sub_id(id, 0xc0), static_cast<float>(cols), reflow_ms);
            cw = (width + gap) / count - gap;
            ch = cw + 54;
        }
        float height(size_t n) const {
            if (!n) {
                return 0;
            }
            float last = ui.spring(ui::sub_id(id, 0xc1), static_cast<float>((n - 1) / static_cast<size_t>(cols)), reflow_ms);
            return last * (ch + gap) + ch;
        }
        Rect cell(float x, float y, size_t i, ui::Id item) const {
            float col = ui.spring(ui::sub_id(item, 0xc0), static_cast<float>(i % static_cast<size_t>(cols)), reflow_ms);
            float row = ui.spring(ui::sub_id(item, 0xc1), static_cast<float>(i / static_cast<size_t>(cols)), reflow_ms);
            return {x + col * (cw + gap), y + row * (ch + gap), cw, ch};
        }
    };

    static void page_title(ui::Ui &ui, Rect body, float y, std::string_view title, std::string_view subtitle) {
        const auto &t = ui.theme();
        ui.label({body.x, y, body.w, 38}, title, {.size = t.fs(28), .color = t.text});
        ui.label({body.x, y + 42, body.w, 20}, subtitle, {.size = t.fs(13), .color = t.text_dim});
    }

    std::string Shell::date_label() const {
        const int month = (today_[0] - '0') * 10 + today_[1] - '0';
        const int day = (today_[3] - '0') * 10 + today_[4] - '0';
        return std::format("{}/{}", month, day);
    }

    // A filter pill.
    static float chip_w(ui::Ui &ui, std::string_view label, bool icon = false) {
        return ui.painter().measure(label, {.size = ui.theme().fs(13)}) + 28 + (icon ? 18 : 0);
    }

    static bool chip(ui::Ui &ui, float x, float y, std::string_view label, bool selected, std::string_view icon = {}) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Rect c{x, y, chip_w(ui, label, !icon.empty()), 30};
        ui::Id id = ui.id(label);
        auto it = ui.interact(id, c);
        const auto [hot, down] = ui.feel(id, it);
        c = c.inset(down * 1.5f);
        p.rect(c, selected ? t.accent.with_alpha(0.2f + 0.06f * hot) : t.text.with_alpha(0.06f + 0.05f * hot), c.h / 2);
        ui::Color fg = selected ? t.accent : t.text_dim.mix(t.text, hot);
        TextStyle st{.size = t.fs(13), .color = fg};
        float tw = p.measure(label, st);
        ui.label({c.x + 14, c.y, tw + 1, c.h}, label, st);
        if (!icon.empty()) {
            p.icon(icon, {c.x + 18 + tw, c.y + (c.h - 14) / 2, 14, 14}, fg);
        }
        return it.clicked;
    }

    // Headed paragraphs in their own scrolling view: an artist's introduction, an album's notes. Laid out every frame;
    // there are only a few, and the shaping is cached.
    static void prose(ui::Ui &ui, Rect r, ui::Id id, const std::vector<std::pair<std::string, std::string>> &sections) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        TextStyle body{.size = t.fs(14), .color = t.text.mix(t.text_dim, 0.35f)};
        const float lh = std::round(body.size * 1.75f), w = r.w - 16, head_h = 36, gap = 24;
        std::vector<std::vector<std::string_view>> wrapped;
        float content = 0;
        for (const auto &[head, text] : sections) {
            wrapped.push_back(p.wrap_lines(text, body.size, w));
            content += (head.empty() ? 0 : head_h) + static_cast<float>(wrapped.back().size()) * lh + gap;
        }
        float off = ui.begin_scroll(id, r, content);
        float y = r.y - off;
        Rect clip = ui.clip();
        for (size_t i = 0; i < sections.size(); ++i) {
            if (!sections[i].first.empty()) {
                ui.label({r.x, y + 4, w, 24}, sections[i].first, {.size = t.fs(16), .color = t.text});
                y += head_h;
            }
            for (auto line : wrapped[i]) {
                if (y + lh > clip.y && y < clip.bottom()) {
                    p.text(r.x, y + lh * 0.7f, line, body, w);
                }
                y += lh;
            }
            y += gap;
        }
        ui.end_scroll();
    }

    // Track rows as track_list lays them out, in r below its header.
    static void skeleton_rows(ui::Ui &ui, Rect r) {
        constexpr float row_h = 56;
        Columns head = columns(ui::cut_top(r, 38));
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

    // Cards as card_grid lays them out from r's top-left; round ones are artists.
    static void skeleton_cards(ui::Ui &ui, Rect r, bool round) {
        const float min_w = round ? 130 : 150, gap = 20;
        const int cols = std::max(1, static_cast<int>((r.w + gap) / (min_w + gap)));
        const float cw = (r.w + gap) / static_cast<float>(cols) - gap, ch = cw + 54;
        for (int i = 0; r.y + static_cast<float>(i / cols) * (ch + gap) < r.bottom(); ++i) {
            const Rect c{r.x + static_cast<float>(i % cols) * (cw + gap), r.y + static_cast<float>(i / cols) * (ch + gap), cw, ch};
            const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f), tw = cw * (0.5f + 0.35f * k);
            ui.skeleton({c.x, c.y, cw, cw}, round ? cw / 2 : ui.theme().radius);
            ui.skeleton({round ? c.x + (cw - tw) / 2 : c.x, c.y + cw + 14, tw, 12}, 6);
            ui.skeleton({round ? c.x + (cw - tw * 0.6f) / 2 : c.x, c.y + cw + 35, tw * 0.6f, 10}, 5);
        }
    }

    bool Shell::page_status(ui::Ui &ui, Rect r, bool loading, const std::string &error, bool login, Placeholder look) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Rect c = ui::center(r, std::min(r.w, 360.f), 150);
        if (login) {
            Rect ic = {c.x + (c.w - 44) / 2, c.y, 44, 44};
            p.rect(ic, t.text.with_alpha(0.06f), 22);
            p.icon("user", ui::center(ic, 20, 20), t.text_dim);
            ui.label({c.x, c.y + 58, c.w, 22}, lyren::tr("Sign in to use this page"), {.size = t.fs(15), .color = t.text}, Align::center);
            bool qr = provider().capabilities.qr_login;
            auto label = lyren::tr(qr ? "Scan to sign in" : "Connect account");
            float bw = button_width(ui, label, true);
            if (ui.button(ui.id("page-login"), {c.x + (c.w - bw) / 2, c.y + 96, bw, 36}, label, ui::ButtonKind::primary, qr ? "qr-code" : "link")) {
                open_account(ui);
                start_login(ui, active_);
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
            p.icon("cloud-off", {c.x + (c.w - 28) / 2, c.y + 20, 28, 28}, t.text_dim);
            ui.label({c.x, c.y + 64, c.w, 20}, lyren::tr(error), {.size = t.fs(13), .color = t.text_dim}, Align::center);
            return true;
        }
        return false;
    }

    float Shell::play_buttons(ui::Ui &ui, float x, float y, std::vector<Track> &tracks) {
        const float play_w = button_width(ui, lyren::tr("Play all"), true), shuffle_w = button_width(ui, lyren::tr("Shuffle"), true);
        if (ui.button(ui.id("play-all"), {x, y, play_w, 40}, lyren::tr("Play all"), ui::ButtonKind::primary, "play-fill") && !tracks.empty()) {
            if (mode_ == Mode::shuffle) {
                set_mode(Mode::list);
            }
            play(tracks, 0);
        }
        x += play_w + 12;
        if (ui.button(ui.id("shuffle-all"), {x, y, shuffle_w, 40}, lyren::tr("Shuffle"), ui::ButtonKind::secondary, "shuffle") && !tracks.empty()) {
            set_mode(Mode::shuffle);
            play(tracks, static_cast<int>(random() % tracks.size()));
        }
        return x + shuffle_w + 12;
    }

    bool Shell::page_tabs(ui::Ui &ui, ui::Id id, Rect r, std::span<const std::string_view> labels, int &selected, std::span<const bool> enabled) {
        const int before = selected;
        std::inplace_vector<std::string_view, 8> visible;
        std::inplace_vector<int, 8> indices;
        int tab = 0;
        for (size_t i = 0; i < labels.size(); ++i) {
            if (enabled.empty() || enabled[i]) {
                if (static_cast<int>(i) == selected) {
                    tab = static_cast<int>(visible.size());
                }
                visible.push_back(labels[i]);
                indices.push_back(static_cast<int>(i));
            }
        }
        ui.tabs(id, r, visible, tab);
        selected = indices[tab];
        if (selected == before) {
            return false;
        }
        reset_selection();
        tab_time_ = ui.now;
        tab_dir_ = selected > before ? 1 : -1;
        return true;
    }

    float Shell::tab_enter(ui::Ui &ui, Rect &content) const {
        const float k = ui.progress(tab_time_, 380);
        content.x += (1 - k) * 36 * static_cast<float>(tab_dir_);
        return k;
    }

    bool Shell::hero(ui::Ui &ui, ui::Id id, Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue, std::string_view icon) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto it = ui.interact(id, r);
        const auto [hot, down] = ui.feel(id, it, 240);
        r = r.inset(down * 3);
        r.y -= 3 * hot;
        p.shadow({r.x, r.y + 10, r.w, r.h}, t.radius_l(), 32, t.shadow.with_alpha(0.35f + 0.65f * hot));
        p.gradient(r, hsl(hue, 0.6f, 0.52f), hsl(hue + 0.07f, 0.55f, 0.27f), t.radius_l());
        if (icon.empty()) {
            ui.label({r.x + 24, r.y + 16, r.w - 48, 54}, big, {.size = t.fs(42), .color = white});
        } else {
            p.icon(icon, {r.x + 24, r.y + 20, 40, 40}, white);
        }
        ui.label({r.x + 24, r.y + 74, r.w - 48, 24}, title, {.size = t.fs(18), .color = white});
        ui.label({r.x + 24, r.y + 98, r.w - 100, 20}, subtitle, {.size = t.fs(12), .color = white.with_alpha(0.75f)});
        Rect b = Rect{r.right() - 64, r.bottom() - 64, 44, 44}.inset(-2 * hot);
        p.rect(b, white.with_alpha(0.2f + 0.8f * hot), b.w / 2);
        p.icon("play-fill", ui::center(b, 18, 18), hsl(hue, 0.6f, 0.4f).mix(white, 1 - hot));
        return it.clicked;
    }

    bool Shell::card(ui::Ui &ui, ui::Id id, Rect r, const Card &c, bool round) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto it = ui.interact(id, r);
        const auto [hot, down] = ui.feel(id, it, 240);
        Rect cov = Rect{r.x, r.y - 4 * hot, r.w, r.w}.inset(down * 3);
        float radius = round ? cov.w / 2 : t.radius;
        if (hot > 0) {
            p.shadow({cov.x, cov.y + 8, cov.w, cov.h}, radius, 24, t.shadow.with_alpha(hot));
        }
        cover(ui, cov, c.cover, radius, round ? "user" : c.kind == CardKind::album ? "disc-album" : "list-music");
        if (hot > 0 && !round) {
            // A shade gathers under the play button, which rises into place.
            p.gradient({cov.x, cov.y + cov.h * 0.45f, cov.w, cov.h * 0.55f}, {0, 0, 0, 0}, {0, 0, 0, 0.3f * hot}, radius);
            Rect b{cov.right() - 48, cov.bottom() - 48 + 8 * (1 - hot), 36, 36};
            p.shadow(b, 18, 10, t.shadow.with_alpha(hot));
            p.rect(b, t.accent.with_alpha(hot), 18);
            p.icon("play-fill", ui::center(b, 16, 16), white.with_alpha(hot));
        }
        Align align = round ? Align::center : Align::left;
        ui.label({r.x, r.y + r.w + 10, r.w, 20}, c.title, {.size = t.fs(14), .color = t.text}, align);
        ui.label({r.x, r.y + r.w + 32, r.w, 18}, card_subtitle(c), {.size = t.fs(12), .color = t.text_dim}, align);
        if (it.right_clicked) {
            link_menu(ui, it.px, it.py, c);
        }
        return it.clicked;
    }

    int Shell::card_grid(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since) {
        Grid g(ui, id, r.w, round ? 130 : 150);
        Rect clip = ui.clip();
        int clicked = -1, shown = 0;
        ui.push_id(id);
        for (size_t i = 0; i < cards.size(); ++i) {
            ui::Id item = item_id(ui, cards[i]);
            Rect c = g.cell(r.x, r.y, i, item);
            if (c.bottom() < clip.y || c.y > clip.bottom()) {
                if (c.bottom() >= clip.y - clip.h && c.y <= clip.bottom() + clip.h) {
                    images_.prefetch(cards[i].cover, static_cast<uint32_t>(Images::tier(c.w * ui.painter().scale())));
                }
                continue;
            }
            // Cards arrive one after another.
            float a = ui.progress(since + 0.03 * std::min(shown++, 14), 420);
            Fade fade(ui.painter(), a);
            c.y += (1 - a) * 16;
            if (card(ui, item, c, cards[i], round)) {
                clicked = static_cast<int>(i);
            }
        }
        ui.pop_id();
        return clicked;
    }

    void Shell::recommend_page(ui::Ui &ui, Rect r) {
        const auto &t = ui.theme();
        auto &lib = library();
        const auto &features = provider().features;
        struct Hero {
            Page page;
            std::string_view id, big, title, subtitle, icon;
            float hue;
        };
        std::inplace_vector<Hero, 4> heroes;
        std::string day = today_.substr(3);
        if (features.daily.songs) {
            heroes.push_back({Page::daily, "hero-daily", day, "Daily mix", "Fresh picks, every day", {}, 0.98f});
        }
        if (supports(Page::fm, active_)) {
            heroes.push_back({Page::fm, "hero-fm", "FM", provider().radio_title, "A station that learns your taste", {}, 0.62f});
        }
        if (features.radar) {
            heroes.push_back({Page::radar, "hero-radar", {}, "Radar", "Favorites and new discoveries", "sparkles", 0.78f});
        }
        if (supports(Page::releases, active_)) {
            heroes.push_back({Page::releases, "hero-releases", {}, "New releases", "Latest songs and albums", "disc-3", 0.08f});
        }
        int hero_count = static_cast<int>(heroes.size());
        Rect body = r.inset(32, 0);
        Grid daily(ui, ui.id("daily-cards"), body.w);
        Grid g(ui, ui.id("rec-cards"), body.w);
        constexpr float hero_h = 136;
        int hero_cols = std::min(hero_count, std::max(1, static_cast<int>((body.w + Grid::gap) / 280)));
        float heroes_h = hero_count ? static_cast<float>((hero_count + hero_cols - 1) / hero_cols) * (hero_h + Grid::gap) + 20 : 0;
        float daily_h = lib.daily_lists.empty() ? 0 : 44 + daily.height(lib.daily_lists.size()) + 40;
        float content = 28 + 76 + heroes_h + daily_h + 44 + std::max(g.height(lib.recommended.cards.size()), 120.f) + 32;
        float off = ui.begin_scroll(ui.id("recommend"), r, content);
        float y = r.y + 28 - off;
        const auto &profile = lib.account;
        page_title(ui, body, y, lyren::tr("For you"),
                   std::format("{} · {}", date_label(), profile ? lyren::trf("Picked for you, {}", profile->name) : lyren::tr("Picked for you")));
        y += 76;
        if (hero_count) {
            float hw = (body.w - Grid::gap * static_cast<float>(hero_cols - 1)) / static_cast<float>(hero_cols);
            for (int i = 0; i < hero_count; ++i) {
                const auto &entry = heroes[i];
                Rect tile{body.x + static_cast<float>(i % hero_cols) * (hw + Grid::gap), y + static_cast<float>(i / hero_cols) * (hero_h + Grid::gap), hw, hero_h};
                if (hero(ui, ui.id(entry.id), tile, entry.big, lyren::tr(entry.title), lyren::tr(entry.subtitle), entry.hue, entry.icon)) {
                    open(ui, entry.page);
                }
            }
            y += heroes_h;
        }
        if (!lib.daily_lists.empty()) {
            ui.label({body.x, y, body.w, 28}, lyren::tr("Daily playlists"), {.size = t.fs(18), .color = t.text});
            if (int i = card_grid(ui, {body.x, y + 44, body.w, 0}, ui.id("daily-cards"), lib.daily_lists, false, std::max(page_time_ + 0.08, lib.daily_since)); i >= 0) {
                open(ui, lib.daily_lists[static_cast<size_t>(i)]);
            }
            y += daily_h;
        }
        ui.label({body.x, y, body.w, 28}, lyren::tr("Recommended playlists"), {.size = t.fs(18), .color = t.text});
        y += 44;
        const Results &res = lib.recommended;
        if (!page_status(ui, {body.x, y, body.w, std::max(120.f, r.bottom() - y)}, res.loading && res.cards.empty(), res.error, false, Placeholder::cards)) {
            if (int i = card_grid(ui, {body.x, y, body.w, 0}, ui.id("rec-cards"), res.cards, false, std::max(page_time_ + 0.08, res.since)); i >= 0) {
                open(ui, res.cards[static_cast<size_t>(i)]);
            }
        }
        ui.end_scroll();
    }

    void Shell::toplist_page(ui::Ui &ui, Rect r) {
        const Results &res = library().toplists;
        Rect body = r.inset(32, 0);
        Grid g(ui, ui.id("top-cards"), body.w);
        float off = ui.begin_scroll(ui.id("toplists"), r, 28 + 84 + g.height(res.cards.size()) + 32);
        float y = r.y + 28 - off;
        page_title(ui, body, y, lyren::tr("Charts"), lyren::tr("Official charts, updated daily or weekly"));
        if (!page_status(ui, {body.x, y + 84, body.w, r.bottom() - y - 84}, res.loading, res.error, false, Placeholder::cards)) {
            if (int i = card_grid(ui, {body.x, y + 84, body.w, 0}, ui.id("top-cards"), res.cards, false, std::max(page_time_ + 0.05, res.since)); i >= 0) {
                open(ui, res.cards[static_cast<size_t>(i)]);
            }
        }
        ui.end_scroll();
    }

    void Shell::square_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        const auto &t = ui.theme();
        Rect body = r.inset(32, 0);
        Grid g(ui, ui.id("square-cards"), body.w);
        constexpr float chips_y = 84, grid_y = chips_y + 54;
        float content = 28 + grid_y + std::max(g.height(lib.square.cards.size()), 120.f) + (lib.square.more() ? 64 : 32);
        float off = ui.begin_scroll(ui.id("square"), r, content);
        float y = r.y + 28 - off;
        page_title(ui, body, y, lyren::tr("Browse"), lyren::tr("Explore playlists by category"));

        // The popular categories as far as they fit, the picked one among them, the rest behind More categories.
        const Category all;
        std::vector<const Category *> categories{&all};
        if (!lib.category.id.empty() && std::ranges::find(lib.categories.hot, lib.category.id, &Category::id) == lib.categories.hot.end()) {
            categories.push_back(&lib.category);
        }
        for (const auto &category : lib.categories.hot) {
            categories.push_back(&category);
        }
        float x = body.x, more_w = lib.categories.groups.empty() ? 0 : chip_w(ui, lyren::tr("More categories"), true);
        const Category *picked = nullptr;
        ui.push_id(ui.id("square-chips"));
        for (const Category *category : categories) {
            const auto name = category->id.empty() ? lyren::tr("All") : std::string_view(category->name);
            float w = chip_w(ui, name);
            if (x + w + 8 + more_w > body.right()) {
                break;
            }
            ui.push_id(ui.id(category->id));
            if (chip(ui, x, y + chips_y, name, category->id == lib.category.id)) {
                picked = category;
            }
            ui.pop_id();
            x += w + 8;
        }
        if (more_w > 0 && chip(ui, x, y + chips_y, lyren::tr("More categories"), false, "chevron-down")) {
            ui.open_modal(600, 480, [this](ui::Ui &u, Rect m) {
                categories_modal(u, m);
            });
        }
        ui.pop_id();
        if (picked) {
            set_category(ui, *picked);
        }

        float gy = y + grid_y;
        if (!page_status(ui, {body.x, gy, body.w, std::max(120.f, r.bottom() - gy)}, lib.square.loading && lib.square.cards.empty(),
                         lib.square.cards.empty() ? lib.square.error : "", false, Placeholder::cards)) {
            double since = std::max({page_time_ + 0.05, lib.square_time, lib.square.since});
            if (int i = card_grid(ui, {body.x, gy, body.w, 0}, ui.id("square-cards"), lib.square.cards, false, since); i >= 0) {
                open(ui, lib.square.cards[static_cast<size_t>(i)]);
            }
            float end = gy + g.height(lib.square.cards.size());
            if (lib.square.loading) {
                ui.spinner(ui::center({body.x, end, body.w, 64}, 20, 20), t.text_dim, 2);
            } else if (end < r.bottom() + 400) {
                load_square();
            }
        }
        ui.end_scroll();
    }

    void Shell::fm_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        page_title(ui, r.inset(32, 0), r.y + 28, lyren::tr(provider().radio_title), lyren::tr("Tuned to what you play"));
        Rect below{r.x, r.y + 100, r.w, r.h - 100};
        if (page_status(ui, below, lib.fm_loading && lib.fm.empty(), lib.fm.empty() ? lib.fm_error : "", !signed_in())) {
            return;
        }
        if (lib.fm.empty()) {
            return;
        }
        lib.fm_pos = std::min(lib.fm_pos, static_cast<int>(lib.fm.size()) - 1);
        const Track &tr = lib.fm[static_cast<size_t>(lib.fm_pos)];
        bool on_air = is_current(tr);
        float size = std::clamp(std::min(r.w * 0.4f, r.h - 310), 160.f, 280.f);
        Rect col = ui::center(r, size, size + 150);
        col.y += 36;
        Rect cov{col.x, col.y, size, size};
        p.shadow({cov.x, cov.y + 14, cov.w, cov.h}, t.radius_l(), 44, t.shadow);
        cover(ui, cov, tr.cover, t.radius_l());
        ui.label({col.x - 100, cov.bottom() + 22, size + 200, 30}, tr.title, {.size = t.fs(20), .color = t.text}, Align::center);
        ui.label({col.x - 100, cov.bottom() + 54, size + 200, 20}, tr.artist + " · " + tr.album, {.size = t.fs(13), .color = t.text_dim}, Align::center);
        float by = cov.bottom() + 90, cx = col.x + size / 2;
        // thumbs-down, like, play, next as one centred row
        float x0 = cx - 102;
        float like_x = x0 + 52;
        if (provider().capabilities.radio_dislike && ui.icon_button(ui.id("fm-trash"), {x0, by + 4, 40, 40}, "thumbs-down", 20)) {
            spawn([](Shell *self, Track track, uint64_t gen) -> Task<void> {
                auto r = co_await self->provider(track).fm_trash(track);
                if (gen != self->library(track.source).gen) {
                    co_return;
                }
                if (r) {
                    if (self->provider(track).features.exclusions.list) {
                        spawn(self->load_dislikes(track.source, DislikeKind::song));
                    }
                    self->notify(std::string(lyren::tr("You will hear fewer songs like this")), "thumbs-down");
                } else {
                    self->notify(lyren::trf("Could not dislike this song: {}", r.error()), "triangle-alert", true);
                }
            }(this, tr, library().gen));
            next_fm();
            return;
        }
        bool like = liked(tr);
        if (provider(tr).capabilities.likes && ui.icon_button(ui.id("fm-like"), {like_x, by + 4, 40, 40}, like ? "heart-fill" : "heart", heart_size(ui, tr, 20), like)) {
            toggle_like(tr);
        }
        ui::Id pid = ui.id("fm-play");
        Rect play_r{like_x + 52, by, 48, 48};
        auto it = ui.interact(pid, play_r);
        const auto [hot, down] = ui.feel(pid, it);
        Rect pr = play_r.inset(-2 * hot + 2 * down);
        p.rect(pr, t.accent.mix(white, 0.1f * hot), pr.w / 2);
        p.icon(on_air && playing() ? "pause-fill" : "play-fill", ui::center(pr, 20, 20), white);
        if (it.clicked) {
            if (on_air) {
                toggle_play();
            } else {
                play(lib.fm, lib.fm_pos);
            }
        }
        if (ui.icon_button(ui.id("fm-next"), {like_x + 112, by + 4, 40, 40}, "skip-forward", 20)) {
            fm_playing_ = fm_playing_ || on_air;
            fm_source_ = active_;
            next_fm();
        }
    }

    void Shell::releases_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        Rect body = r.inset(32, 0);
        page_title(ui, body, r.y + 28, lyren::tr("New releases"), lyren::tr("Latest songs and albums"));
        const std::array<std::string_view, 2> tabs = {lyren::tr("Songs"), lyren::tr("Albums")};
        const std::array enabled = {!provider().release_categories[0].empty(), !provider().release_categories[1].empty()};
        if (page_tabs(ui, ui.id("release-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, lib.release_tab, enabled)) {
            load_releases();
        }
        int tab = lib.release_tab;
        const auto &categories = provider().release_categories[tab];
        std::string_view category = lyren::tr(categories[lib.release_category[tab]].name);
        float cw = button_width(ui, category, true);
        if (ui.button(ui.id("release-category"), {body.x, r.y + 148, cw, 36}, category, ui::ButtonKind::secondary, "chevron-down")) {
            std::vector<ui::MenuItem> items;
            for (size_t i = 0; i < categories.size(); ++i) {
                items.push_back({i == static_cast<size_t>(lib.release_category[tab]) ? "check" : "", std::string(lyren::tr(categories[i].name)), [this, &ui, i] {
                                     set_release_category(ui, static_cast<int>(i));
                                 }});
            }
            ui.open_menu(body.x, r.y + 190, std::move(items));
        }
        if (ui.icon_button(ui.id("release-refresh"), {body.right() - 36, r.y + 148, 36, 36}, "refresh-cw", 18)) {
            set_release_category(ui, lib.release_category[tab]);
        }
        Results &res = lib.releases[tab];
        if (tab == 0) {
            play_buttons(ui, body.x, r.y + 200, res.tracks);
        }
        float top = r.y + (tab ? 200 : 256);
        Rect content{r.x + 16, top, r.w - 32, r.bottom() - top - 8};
        Fade fade(ui.painter(), tab_enter(ui, content));
        std::string error = res.error;
        if (res.loaded && res.size() == 0 && error.empty()) {
            error = tab ? lyren::tr("No albums found") : lyren::tr("No songs here yet");
        }
        if (res.size() == 0 && page_status(ui, tab ? content.inset(12, 12) : content, res.loading, error, false, tab ? Placeholder::cards : Placeholder::rows)) {
            return;
        }
        if (tab == 0) {
            track_list(ui, content, ui.id("release-songs"), res.tracks, std::max({page_time_, tab_time_, res.since}));
        } else {
            bool near_end;
            if (int i = grid_view(ui, content, ui.id("release-albums"), res.cards, false, std::max({page_time_, tab_time_, res.since}), res.more(), near_end); i >= 0) {
                open(ui, res.cards[static_cast<size_t>(i)]);
            }
            if (near_end) {
                load_releases();
            }
        }
    }

    void Shell::collection_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        Rect body = r.inset(32, 0);
        page_title(ui, body, r.y + 28, lyren::tr("My collection"), lyren::tr("Saved albums and followed artists"));
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        const std::array<std::string_view, 2> tabs = {lyren::tr("Albums"), lyren::tr("Artists")};
        const std::array enabled = {provider().capabilities.collect_albums, provider().capabilities.follow_artists};
        page_tabs(ui, ui.id("collection-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, lib.collection_tab, enabled);
        Rect content{r.x + 16, r.y + 148, r.w - 32, r.h - 156};
        Fade fade(ui.painter(), tab_enter(ui, content));
        const Results &res = lib.collection[lib.collection_tab];
        std::string error = res.cards.empty() ? res.error : "";
        if (res.cards.empty() && res.loaded && error.empty()) {
            error = lib.collection_tab ? lyren::tr("No followed artists yet") : lyren::tr("No saved albums yet");
        }
        if (res.cards.empty() && page_status(ui, content.inset(12, 12), res.loading && !res.loaded, error, false, lib.collection_tab ? Placeholder::faces : Placeholder::cards)) {
            return;
        }
        bool near_end;
        ui::Id id = ui.id(lib.collection_tab ? "collection-artists" : "collection-albums");
        if (int i = grid_view(ui, content, id, res.cards, lib.collection_tab == 1, std::max({page_time_, tab_time_, res.since}), false, near_end); i >= 0) {
            open(ui, res.cards[static_cast<size_t>(i)]);
        }
    }

    void Shell::dislikes_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Rect body = r.inset(32, 0);
        page_title(ui, body, r.y + 28, lyren::tr("Disliked"), lyren::tr("Manage what you hear in recommendations"));
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        const std::array<std::string_view, 3> tabs = {lyren::tr("Songs"), lyren::tr("Artists"), lyren::tr("Styles")};
        const std::array icons = {"music", "mic-vocal", "sparkles"};
        if (page_tabs(ui, ui.id("dislike-tabs"), {body.x, r.y + 100, body.w - 48, 36}, tabs, lib.dislike_tab)) {
            if (!lib.dislikes[lib.dislike_tab]) {
                spawn(load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        static const DislikeList pending{.loading = true};
        const auto &cached = lib.dislikes[lib.dislike_tab];
        const DislikeList &list = cached ? *cached : pending;
        ui.disabled = list.loading;
        if (ui.icon_button(ui.id("dislike-refresh"), {body.right() - 36, r.y + 100, 36, 36}, "refresh-cw", 18)) {
            spawn(load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
        }
        ui.disabled = false;
        Rect content{body.x, r.y + 152, body.w, r.h - 164};
        Fade fade(p, tab_enter(ui, content));
        if (!list.error.empty()) {
            page_status(ui, content, false, list.error, false);
            return;
        }
        if (list.items.empty()) {
            page_status(ui, content, list.loading, std::string(lyren::tr("Nothing disliked here")), false);
            return;
        }
        constexpr float row_h = 72;
        float off = ui.begin_scroll(ui.id("dislikes-" + std::to_string(lib.dislike_tab)), content, row_h * static_cast<float>(list.items.size()));
        const size_t first = static_cast<size_t>(off / row_h);
        const size_t last = std::min(list.items.size(), static_cast<size_t>((off + content.h) / row_h) + 1);
        const float restore_w = button_width(ui, lyren::tr("Restore"), true);
        for (size_t i = first; i < last; ++i) {
            const Dislike &item = list.items[i];
            ui.push_id(ui.id(item.id));
            Rect row{content.x, content.y - off + row_h * static_cast<float>(i), content.w, row_h};
            const float k = ui.progress(std::max({page_time_, tab_time_, list.since}) + std::min(i - first, size_t(12)) * 0.024, 380);
            Fade row_fade(p, k);
            row.y += (1 - k) * 8;
            if (ui.hovering(row)) {
                p.rect(row.inset(0, 2), t.text.with_alpha(0.04f), t.radius_s());
            }
            cover(ui, {row.x + 10, row.y + 14, 44, 44}, item.cover, lib.dislike_tab == 1 ? 22 : t.radius_s(), icons[lib.dislike_tab]);
            float text_x = row.x + 70, text_w = row.w - 90 - restore_w;
            ui.label({text_x, row.y + 14, text_w, 24}, item.name.empty() ? item.id : item.name, {.size = t.fs(15), .color = t.text});
            ui.label({text_x, row.y + 40, text_w, 18}, lyren::tr("Hidden from recommendations"), {.size = t.fs(12), .color = t.text_dim});
            ui.disabled = list.loading || list.pending.contains(item.id);
            if (ui.button(ui.id("restore"), {row.right() - restore_w - 10, row.y + 18, restore_w, 36}, lyren::tr("Restore"), ui::ButtonKind::secondary, "check")) {
                set_dislike(item, false);
            }
            ui.disabled = false;
            ui.pop_id();
        }
        ui.end_scroll();
    }

    void Shell::artist_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ArtistPage &a = *route_.artist;
        const Card &c = route_.card;
        Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
        Rect cov{head.x, head.y, 148, 148};
        p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, 74, 32, t.shadow.with_alpha(0.8f));
        cover(ui, cov, a.info ? a.info->avatar : c.cover, 74, "user");
        if (auto it = ui.interact(ui.id("artist-link"), cov); it.right_clicked) {
            link_menu(ui, it.px, it.py, c);
        }
        float tx = cov.right() + 28, tw = head.right() - tx;
        ui.label({tx, head.y + 2, tw, 18}, lyren::tr("Artists"), {.size = t.fs(12), .color = t.text_dim});
        ui.label({tx, head.y + 18, tw, 38}, a.info ? a.info->name : c.title, {.size = t.fs(28), .color = t.text});
        std::string about = a.error;
        if (a.info) {
            about = lyren::trf("{} songs · {} albums", a.info->songs, a.info->albums);
            if (!a.info->alias.empty()) {
                about = a.info->alias + " · " + about;
            }
        }
        ui.label({tx, head.y + 62, tw, 20}, about, {.size = t.fs(13), .color = t.text_dim});
        std::vector<Track> &songs = a.songs.tracks;
        const float bx = play_buttons(ui, tx, head.bottom() - 40, songs);
        if (a.info && provider(c).capabilities.follow_artists) {
            bool on = preference_value(c.source, Preference::artist, c.id, a.info->followed);
            float follow_w = std::max(button_width(ui, lyren::tr("Following"), true), button_width(ui, lyren::tr("Follow"), true));
            if (ui.button(ui.id("follow"), {bx, head.bottom() - 40, follow_w, 40}, on ? lyren::tr("Following") : lyren::tr("Follow"), ui::ButtonKind::secondary,
                          on ? "user-check" : "user-plus")) {
                toggle_follow(route_.artist, {.kind = CardKind::artist, .source = c.source, .id = c.id, .title = a.info->name, .cover = a.info->avatar});
            }
        }

        // Songs and albums load a page at a time as they scroll; each tab loads the first time it is picked.
        const std::array<std::string_view, 3> tabs = {lyren::tr("Songs"), lyren::tr("Albums"), lyren::tr("About")};
        Rect bar{head.x, head.bottom() + 24, head.w, 36};
        float retry_w = button_width(ui, lyren::tr("Retry"), true);
        Rect retry{bar.right() - retry_w, bar.y, retry_w, bar.h};
        bar.w -= retry_w + 16;
        if (page_tabs(ui, ui.id("artist-tabs"), bar, std::span(tabs).first(provider(c).capabilities.artist_intro ? 3 : 2), a.tab)) {
            load_artist_tab(a.tab);
        }
        Rect content{r.x + 16, bar.bottom() + 12, r.w - 32, r.bottom() - bar.bottom() - 20};
        Fade fade(p, tab_enter(ui, content));
        Results &res = a.tab == 0 ? a.songs : a.tab == 1 ? a.albums : a.intro;
        if (res.loading || a.loading) {
            ui.spinner(ui::center(retry, 18, 18), t.text_dim, 2);
        } else if ((!res.error.empty() || !a.error.empty()) && ui.button(ui.id("artist-retry"), retry, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
            if (!a.error.empty()) {
                spawn(load_artist(route_.artist, c));
            }
            if (!res.error.empty()) {
                load_artist_tab(a.tab, true);
            }
        }
        bool empty = a.tab == 2 ? a.sections.empty() : res.size() == 0;
        std::string error = empty ? res.error : "";
        if (empty && res.loaded && error.empty()) {
            error = a.tab == 2 ? lyren::tr("No biography available") : lyren::tr("Nothing here yet");
        }
        const std::array looks{Placeholder::rows, Placeholder::cards, Placeholder::spinner};
        if (empty &&
            page_status(ui, a.tab == 1 ? content.inset(12, 12) : content, res.loading || (!res.loaded && error.empty()), error, false, looks[static_cast<size_t>(a.tab)])) {
            return;
        }
        if (!res.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), res.error, {.size = t.fs(12), .color = t.danger});
        }
        double since = std::max({page_time_ + 0.06, tab_time_, res.since});
        bool near_end = false;
        int clicked = -1;
        switch (a.tab) {
            case 0:
                near_end = track_list(ui, content, ui.id("artist-songs"), songs, since);
                break;
            case 1:
                clicked = grid_view(ui, content, ui.id("artist-albums"), a.albums.cards, false, since, a.albums.loading, near_end);
                break;
            default:
                prose(ui, {content.x + 16, content.y + 8, content.w - 16, content.h - 8}, ui.id("artist-intro"), a.sections);
        }
        if (near_end) {
            load_artist_tab(a.tab);
        }
        // Last: opening an album replaces the route this page draws from.
        if (clicked >= 0) {
            open(ui, a.albums.cards[static_cast<size_t>(clicked)]);
        }
    }

    void Shell::search_page(ui::Ui &ui, Rect r) {
        Rect body = r.inset(32, 0);
        const std::array<std::string_view, 5> tabs = {lyren::tr("Tracks"), lyren::tr("Albums"), lyren::tr("Artists"), lyren::tr("Playlists"), lyren::tr("Users")};
        float tabs_w = body.w - button_width(ui, lyren::tr("Refresh"), true) - 16;
        if (page_tabs(ui, ui.id("search-tabs"), {body.x, r.y + 100, tabs_w, 36}, tabs, search_tab_, provider().capabilities.search)) {
            ensure_route_data();
        }
        Results &shown = results_[static_cast<size_t>(active_)][search_tab_];
        ui::Id scroll = search_scroll_id(ui, active_, search_tab_);
        if (shown.loading) {
            ui.spinner({body.right() - 26, r.y + 109, 18, 18}, ui.theme().text_dim, 2);
        } else {
            auto label = lyren::tr(shown.error.empty() ? "Refresh" : "Retry");
            float w = button_width(ui, label, true);
            if (ui.button(ui.id("search-refresh"), {body.right() - w, r.y + 100, w, 36}, label, ui::ButtonKind::secondary, "refresh-cw")) {
                if (shown.error.empty()) {
                    shown = {};
                    reset_selection();
                    ui.scroll_to(scroll, 0);
                }
                load_results(search_tab_, true);
            }
        }
        const std::array kinds = {lyren::tr("songs"), lyren::tr("albums"), lyren::tr("artists"), lyren::tr("playlists"), lyren::tr("users")};
        page_title(ui, body, r.y + 28, '"' + searched_ + '"',
                   shown.loaded ? lyren::trf("{}{} {} found", shown.more() ? lyren::tr("About ") : "", shown.total, kinds[search_tab_])
                                : lyren::trf("Searching {}", lyren::tr(platform(active_).short_name)));
        // Tab content slides toward the tab that was picked.
        Rect content{r.x + 16, r.y + 148, r.w - 32, r.h - 156};
        Fade fade(ui.painter(), tab_enter(ui, content));
        double since = std::max({page_time_, tab_time_, shown.since});
        bool empty = shown.size() == 0;
        std::string error = empty ? shown.error : "";
        if (empty && shown.more() && error.empty()) {
            load_results(search_tab_);
        }
        if (empty && shown.loaded && !shown.more() && error.empty()) {
            error = lyren::tr("No results found");
        }
        const std::array looks{Placeholder::rows, Placeholder::cards, Placeholder::faces, Placeholder::cards, Placeholder::faces};
        if (empty && page_status(ui, search_tab_ ? content.inset(12, 12) : content, shown.loading, error, false, looks[static_cast<size_t>(search_tab_)])) {
            return;
        }
        if (!shown.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), shown.error, {.size = ui.theme().fs(12), .color = ui.theme().danger});
        }
        if (search_tab_ == 0) {
            if (track_list(ui, content, scroll, shown.tracks, since)) {
                load_results(0);
            }
            return;
        }
        bool near_end;
        int i = grid_view(ui, content, scroll, shown.cards, search_tab_ == 2 || search_tab_ == 4, since, shown.loading, near_end);
        if (near_end) {
            load_results(search_tab_);
        }
        if (i >= 0) {
            open(ui, shown.cards[static_cast<size_t>(i)]);
        }
    }

    int Shell::grid_view(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since, bool loading, bool &near_end) {
        Rect inner = r.inset(12, 0);
        Grid g(ui, id, inner.w, round ? 130 : 150);
        float h = 12 + g.height(cards.size()) + (loading ? 64 : 20);
        float off = ui.begin_scroll(id, r, h);
        int i = card_grid(ui, {inner.x, r.y + 12 - off, inner.w, 0}, id, cards, round, since);
        near_end = r.y + h - off < r.bottom() + 400;
        if (loading) {
            ui.spinner(ui::center({inner.x, r.y + h - off - 64, inner.w, 64}, 20, 20), ui.theme().text_dim, 2);
        }
        ui.end_scroll();
        return i;
    }

    void Shell::list_page(ui::Ui &ui, Rect r, std::vector<Track> &tracks, bool loading, const std::string &list_error, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &c = route_.card;
        Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
        Rect cov{head.x, head.y, 148, 148};
        p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, t.radius_l(), 32, t.shadow.with_alpha(0.8f));
        switch (route_.page) {
            case Page::liked:
                art(p, cov, special_hue(Page::liked), t.radius_l(), "heart-fill");
                break;
            case Page::recent:
                art(p, cov, special_hue(Page::recent), t.radius_l(), "history");
                break;
            case Page::cloud:
                art(p, cov, special_hue(Page::cloud), t.radius_l(), "cloud");
                break;
            case Page::radar:
                art(p, cov, 0.78f, t.radius_l(), "sparkles");
                break;
            case Page::daily: {
                art(p, cov, 0.98f, t.radius_l(), "");
                std::string day = today_.substr(3);
                ui.label(cov, day.substr(0, day.find(' ')), {.size = t.fs(54), .color = white}, Align::center);
                break;
            }
            default:
                cover(ui, cov, c.cover, t.radius_l(), route_.page == Page::album ? "disc-album" : "list-music");
        }
        float tx = cov.right() + 28, tw = head.right() - tx;
        TextStyle sub{.size = t.fs(13), .color = t.text_dim};
        if (route_.page == Page::playlist || route_.page == Page::album) {
            if (auto it = ui.interact(ui.id("list-link"), cov); it.right_clicked) {
                link_menu(ui, it.px, it.py, c);
            }
        }
        if (route_.page == Page::playlist || route_.page == Page::album || route_.page == Page::similar) {
            std::string_view kind = route_.page == Page::album ? lyren::tr("Albums") : route_.page == Page::similar ? lyren::tr("Similar songs") : lyren::tr("Playlists");
            ui.label({tx, head.y + 2, tw, 18}, kind, {.size = t.fs(12), .color = t.text_dim});
        }
        const bool built_in = route_.page == Page::daily || route_.page == Page::radar || route_.page == Page::cloud || route_.page == Page::liked || route_.page == Page::recent;
        ui.label({tx, head.y + 18, tw, 38}, built_in ? lyren::tr(c.title) : std::string_view(c.title), {.size = t.fs(28), .color = t.text});
        const AlbumInfo *album = route_.page == Page::album && route_.list && !route_.list->loading ? &route_.list->album : nullptr;
        if (album && !album->artist.empty()) {
            // Album: the artist links to their page, then the date and the label; the notes open in full.
            Rect ar{tx, head.y + 62, std::min(p.measure(album->artist, sub) + 1, tw), 20};
            ui::Id aid = ui.id("album-artist");
            auto it = ui.interact(aid, ar);
            float hot = ui.anim(aid, it.hovered ? 1 : 0, 120);
            ui.label(ar, album->artist, {.size = sub.size, .color = t.text_dim.mix(t.text, hot)});
            if (hot > 0) {
                p.rect({ar.x, ar.bottom() - 1, ar.w, 1}, t.text.with_alpha(0.4f * hot));
            }
            if (it.clicked && !album->artist_id.empty()) {
                open(ui, Page::artist, {.kind = CardKind::artist, .source = c.source, .id = album->artist_id, .title = album->artist.substr(0, album->artist.find(" / "))});
            }
            std::string rest;
            for (const std::string *part : {&album->date, &album->company}) {
                if (!part->empty()) {
                    rest += " · " + *part;
                }
            }
            ui.label({ar.right(), head.y + 62, tw - ar.w, 20}, rest, sub);
            if (!album->about.empty()) {
                Rect ab{tx, head.y + 84, tw, 20};
                ui::Id bid = ui.id("album-about");
                auto bt = ui.interact(bid, ab);
                float bh = ui.anim(bid, bt.hovered ? 1 : 0, 120);
                std::string_view first = album->about;
                first = first.substr(0, first.find('\n'));
                p.text(tx, ab.y + 15, lyren::trf("About: {}", first), {.size = t.fs(12), .color = t.text_dim.mix(t.text, 0.6f * bh)}, tw);
                if (bt.clicked) {
                    ui.open_modal(560, std::min(ui.height - 120, 600.f), [title = c.title, about = album->about](ui::Ui &u, Rect r) {
                        u.label({r.x, r.y, r.w - 40, 30}, title, {.size = u.theme().fs(20), .color = u.theme().text});
                        prose(u, {r.x, r.y + 52, r.w + 16, r.h - 52}, u.id("album-notes"), {{"", about}});
                    });
                }
            }
        } else {
            const std::string subtitle = route_.page == Page::daily ? lyren::trf("{} · Made for your taste", date_label()) : card_subtitle(c);
            ui.label({tx, head.y + 62, tw, 20}, built_in ? lyren::tr(subtitle) : std::string_view(subtitle), sub);
        }
        list_actions(ui, play_buttons(ui, tx, head.bottom() - 40, tracks), head.bottom() - 40);
        Rect body{r.x + 16, head.bottom() + 28, r.w - 32, r.bottom() - head.bottom() - 36};
        bool login = needs_login(route_.page) && !signed_in();
        std::string error = list_error;
        if (tracks.empty() && error.empty() && !loading && !login) {
            error = route_.page == Page::similar ? lyren::tr("No similar songs found") : lyren::tr("No songs here yet");
        }
        if (tracks.empty() && page_status(ui, body, loading, error, login, Placeholder::rows)) {
            if (!loading && !login && !list_error.empty()) {
                const auto centre = ui::center(body, std::min(body.w, 360.f), 150);
                if (ui.button(ui.id("list-retry"), {centre.x + (centre.w - 100) / 2, centre.y + 104, 100, 34}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                    if (route_.list) {
                        spawn(load_list(route_.list, route_.page, route_.card));
                    } else {
                        spawn(load_library(active_));
                    }
                }
            }
            return;
        }
        if (!list_error.empty()) {
            Rect retry = ui::cut_top(body, 40).inset(12, 0);
            const float w = button_width(ui, lyren::tr("Retry"), true);
            ui.label({retry.x, retry.y, retry.w - w - 12, 32}, lyren::tr(list_error), {.size = t.fs(12), .color = t.danger});
            if (ui.button(ui.id("list-retry"), {retry.right() - w, retry.y, w, 32}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                spawn(load_list(route_.list, route_.page, route_.card));
                if (route_.card.owned) {
                    spawn(load_library(route_.card.source));
                }
            }
        }
        track_list(ui, body, ui.id("tracks"), tracks, std::max(page_time_ + 0.06, since));
    }

    bool Shell::track_list(ui::Ui &ui, Rect r, ui::Id id, std::vector<Track> &tracks, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        if (selection_list_ != &tracks) {
            reset_selection();
            selection_list_ = &tracks;
        }
        constexpr float row_h = 56;
        Rect head = ui::cut_top(r, 34);
        Columns c = columns(head);
        TextStyle hs{.size = t.fs(12), .color = t.text_dim};
        if (choosing_) {
            const bool all = chosen_.size() == tracks.size() && !tracks.empty();
            if (ui.icon_button(ui.id("select-all"), ui::center(c.index, 30, 30), all ? "square-check" : chosen_.empty() ? "square" : "square-minus", 18, !chosen_.empty())) {
                chosen_.clear();
                if (!all) {
                    for (const auto &track : tracks) {
                        chosen_.insert(track.id);
                    }
                }
            }
        } else {
            ui.label(c.index, "#", hs, Align::center);
        }
        ui.label(c.title, lyren::tr("Title"), hs);
        if (c.album.w > 0) {
            ui.label(c.album, lyren::tr("Album"), hs);
        }
        const bool ordering = route_.page == Page::playlist && route_.list && &tracks == &route_.list->tracks && route_.list->ordering;
        if (provider().capabilities.playlist_edit && !ordering && !tracks.empty()) {
            const auto label = lyren::tr(choosing_ ? "Done selecting" : "Select songs");
            const float w = button_width(ui, label, false);
            if (ui.button(ui.id("select-songs"), {head.right() - w - 12, head.y, w, 28}, label, ui::ButtonKind::ghost)) {
                if (choosing_) {
                    reset_selection();
                    selection_list_ = &tracks;
                } else {
                    choosing_ = true;
                    if (selected_ >= 0) {
                        chosen_.insert(tracks[static_cast<size_t>(selected_)].id);
                        selection_anchor_ = selected_;
                    }
                }
            }
        } else {
            ui.label(c.duration, lyren::tr(ordering ? "Order" : "Time"), hs, Align::right);
        }
        p.rect({head.x + 12, head.bottom() - 1, head.w - 24, 1}, t.border);
        ui::cut_top(r, 4);

        if (choosing_) {
            Rect bar = ui::cut_top(r, 44).inset(12, 0);
            const bool removable = route_.page == Page::playlist && route_.card.owned && route_.list && &tracks == &route_.list->tracks;
            float right = bar.right();
            if (removable) {
                const bool busy = library().playlist_writes.contains(route_.card.id);
                const auto label = lyren::tr(busy ? "Saving..." : "Remove");
                const float w = button_width(ui, label, true);
                ui.disabled = chosen_.empty() || busy;
                bool remove;
                {
                    Fade dim(p, ui.disabled ? 0.4f : 1.f);
                    remove = ui.button(ui.id("remove-selected"), {right - w, bar.y + 4, w, 32}, label, ui::ButtonKind::secondary, "list-minus");
                }
                ui.disabled = false;
                if (remove) {
                    remove_from_playlist(route_.list, route_.card, chosen_tracks(tracks));
                }
                right -= w + 12;
            }
            const auto label = lyren::tr("Add to playlist");
            const float w = button_width(ui, label, true);
            ui.disabled = chosen_.empty();
            bool add;
            {
                Fade dim(p, ui.disabled ? 0.4f : 1.f);
                add = ui.button(ui.id("add-selected"), {right - w, bar.y + 4, w, 32}, label, ui::ButtonKind::secondary, "folder-plus");
            }
            ui.disabled = false;
            if (add) {
                open_picker(ui, chosen_tracks(tracks));
            }
            ui.label({bar.x, bar.y, right - w - bar.x - 12, 40}, lyren::trf("{} selected", chosen_.size()), hs);
        }

        float off = ui.begin_scroll(id, r, static_cast<float>(tracks.size()) * row_h + 8);
        // One row past the bottom keeps its place, so it slides in with the rest when a row above it goes.
        int first = static_cast<int>(off / row_h), last = static_cast<int>((off + r.h) / row_h) + 2;
        for (int i = std::max(0, first - 8); i < std::min(last + 16, static_cast<int>(tracks.size())); ++i) {
            if (i < first || i > last) {
                images_.prefetch(tracks[static_cast<size_t>(i)].cover, static_cast<uint32_t>(Images::tier(40 * p.scale())));
            }
        }
        // Rows glide to a new place when the list changes under them; a list that arrives anew starts in place.
        ui.push_id(ui::sub_id(id, std::bit_cast<uint64_t>(since)));
        for (int i = first; i <= last && i < static_cast<int>(tracks.size()); ++i) {
            // Rows cascade in when the list appears.
            float a = ui.progress(since + 0.022 * std::max(0, i - first), 380);
            Fade fade(p, a);
            float y = ui.spring(ui::sub_id(item_id(ui, tracks[static_cast<size_t>(i)]), 0x70), static_cast<float>(i) * row_h, 280);
            track_row(ui, {r.x, r.y + y - off + (1 - a) * 14, r.w, row_h}, tracks, i);
        }
        ui.pop_id();
        ui.end_scroll();
        return last + 10 >= static_cast<int>(tracks.size());
    }

    void Shell::track_row(ui::Ui &ui, Rect r, std::vector<Track> &tracks, int i) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Track &tr = tracks[static_cast<size_t>(i)];
        ui.push_id(item_id(ui, tr));
        ui::Id row_id = ui.id("row");
        bool hot = ui.hovering(r) || ui.is_active(row_id);
        bool now_playing = is_current(tr);
        float h = ui.anim(row_id, hot ? 1 : 0, 140);
        const bool selected = choosing_ ? chosen_.contains(tr.id) : selected_ == i;
        float s = ui.anim(ui::sub_id(row_id, 0x5e), selected ? 1 : 0, 200);
        Rect bg = r.inset(4, 2);
        if (h > 0 || s > 0) {
            p.rect(bg, (choosing_ ? t.accent : t.text).with_alpha(0.06f * h + 0.05f * s), t.radius_s());
        }

        Columns c = columns(r);
        TextStyle num{.size = t.fs(13), .color = t.text_dim, .features = text::tabular_nums};
        const bool picking =
            choosing_ || (provider(tr).capabilities.playlist_edit &&
                          ((ui.input().mods & (wl::mod_ctrl | wl::mod_shift)) || std::ranges::any_of(ui.input().actions, [&](const ui::PointerAction &action) {
                               return action.press && action.button == BTN_LEFT && r.contains(action.x, action.y) && (action.mods & (wl::mod_ctrl | wl::mod_shift));
                           })));
        if (picking) {
            p.icon(chosen_.contains(tr.id) ? "square-check" : "square", ui::center(c.index, 18, 18), chosen_.contains(tr.id) ? t.accent : t.text_dim);
        } else if (hot) {
            if (ui.icon_button(ui.id("play"), ui::center(c.index, 30, 30), now_playing && playing() ? "pause-fill" : "play-fill", 14)) {
                if (now_playing) {
                    toggle_play();
                } else {
                    play(tracks, i);
                }
            }
        } else if (now_playing) {
            ui.equalizer(ui.id("playing"), ui::center(c.index, 14, 14), t.accent, playing());
        } else {
            ui.label(c.index, std::to_string(i + 1), num, Align::center);
        }
        Rect cov{c.title.x, r.y + (r.h - 40) / 2, 40, 40};
        cover(ui, cov, tr.cover, t.radius_s());
        float tx = cov.right() + 14, tw = c.title.right() - tx - 8;
        float w = p.text(tx, r.y + 25, tr.title, {.size = t.fs(14), .color = now_playing ? t.accent : t.text}, tw);
        if (!tr.alias.empty() && w + 24 < tw) {
            p.text(tx + w + 6, r.y + 25, "（" + tr.alias + "）", {.size = t.fs(14), .color = t.text_dim}, tw - w - 6);
        }
        p.text(tx, r.y + 43, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
        if (c.album.w > 0) {
            ui.label(c.album, tr.album, {.size = t.fs(13), .color = t.text_dim});
        }
        const bool ordering = route_.page == Page::playlist && route_.list && &tracks == &route_.list->tracks && route_.list->ordering;
        if (ordering) {
            auto move = [&](std::string_view id, Rect cell, std::string_view icon, int to) {
                ui.disabled = route_.list->saving_order || route_.list->loading || !route_.list->error.empty() || library().playlist_writes.contains(route_.card.id) || to < 0 ||
                              to >= static_cast<int>(tracks.size());
                bool clicked;
                {
                    Fade dim(p, ui.disabled ? 0.3f : 1.f);
                    clicked = ui.icon_button(ui.id(id), ui::center(cell, 32, 32), icon, 18);
                }
                ui.disabled = false;
                if (clicked) {
                    move_playlist_track(route_.list, route_.card, i, to);
                }
                return clicked;
            };
            if (move("move-up", c.like, "chevron-up", i - 1) || move("move-down", c.duration, "chevron-down", i + 1)) {
                ui.pop_id();
                return;
            }
        } else {
            bool like = liked(tr);
            if (provider(tr).capabilities.likes && (hot || like) &&
                ui.icon_button(ui.id("like"), ui::center(c.like, 32, 32), like ? "heart-fill" : "heart", heart_size(ui, tr, 16), like)) {
                toggle_like(tr);
            }
            ui.label(c.duration, mmss(tr.duration), num, Align::right);
        }

        // Disabled ordering buttons still own their cells; a repeated click must not reach the row's play action.
        Rect row = r;
        if (ordering) {
            row.w = c.like.x - row.x;
        }
        auto it = ui.interact(row_id, row);
        if (it.pressed) {
            if (picking) {
                choose_track(tracks, i, it.mods & wl::mod_shift);
            } else {
                selected_ = i;
            }
        }
        if (it.double_clicked && !picking) {
            play(tracks, i);
        }
        if (it.right_clicked) {
            if (choosing_ && !chosen_.contains(tr.id)) {
                chosen_.clear();
                chosen_.insert(tr.id);
                selection_anchor_ = i;
            }
            selected_ = i;
            track_menu(ui, it.px, it.py, tracks, i);
        }
        ui.pop_id();
    }

    void Shell::list_actions(ui::Ui &ui, float x, float y) {
        const Card &c = route_.card;
        auto square = [&](std::string_view id, std::string_view icon) {
            bool clicked = ui.button(ui.id(id), {x, y, 40, 40}, "", ui::ButtonKind::secondary, icon);
            x += 52;
            return clicked;
        };
        auto labelled = [&](std::string_view id, std::string_view label, std::string_view icon, float w) {
            bool clicked = ui.button(ui.id(id), {x, y, w, 40}, label, ui::ButtonKind::secondary, icon);
            x += w + 12;
            return clicked;
        };
        if (route_.page == Page::album) {
            if (!route_.list || route_.list->loading) {
                return;
            }
            bool on = preference_value(c.source, Preference::album, c.id, route_.list->album.collected);
            float w = std::max(button_width(ui, lyren::tr("Saved"), true), button_width(ui, lyren::tr("Save"), true));
            if (provider(c).capabilities.collect_albums && labelled("collect", on ? lyren::tr("Saved") : lyren::tr("Save"), on ? "star-fill" : "star", w)) {
                toggle_collect(route_.list, {.kind = CardKind::album, .source = c.source, .id = c.id, .title = c.title, .subtitle = route_.list->album.artist, .cover = c.cover});
            }
            if (provider(c).capabilities.comments[1] && square("album-comments", "message-square")) {
                open_comments(ui, c.source, Subject::album, route_.list->album.thread, c.title, c.cover);
            }
        } else if (route_.page == Page::radar) {
            if (route_.list && !route_.list->loading && square("radar-refresh", "refresh-cw")) {
                spawn(load_list(route_.list, Page::radar, c));
            }
        } else if (route_.page == Page::liked) {
            if (!provider().features.heartbeat || !signed_in() || library().liked.id.empty() || !route_.list || route_.list->tracks.empty()) {
                return;
            }
            if (labelled("heartbeat", lyren::tr("Heartbeat"), "heart-pulse", button_width(ui, lyren::tr("Heartbeat"), true))) {
                // From the picked song, otherwise from any of them.
                const auto &v = route_.list->tracks;
                size_t seed = selected_ >= 0 && selected_ < static_cast<int>(v.size()) ? static_cast<size_t>(selected_) : random() % v.size();
                spawn(heartbeat(v[seed]));
            }
        } else if (route_.page == Page::recent) {
            const std::array<std::string_view, 2> tabs = {lyren::tr("This device"), lyren::tr("Cloud")};
            const float tabs_w = button_width(ui, tabs[0], false) + button_width(ui, tabs[1], false);
            if (provider().features.recent_plays && signed_in() && ui.tabs(ui.id("recent-tabs"), {x, y, tabs_w, 40}, tabs, library().recent_tab, ui::TabStyle::pill)) {
                tab_time_ = ui.now;
                reset_selection();
                ui.scroll_to(ui.id("tracks"), 0);
                ensure_route_data();
            }
        } else if (route_.page == Page::playlist) {
            if (provider(c).capabilities.collect_playlists && c.collectable && !c.owned && route_.list) {
                bool on = std::ranges::any_of(library(c.source).playlists, [&](const Card &card) {
                    return same(card, c);
                });
                float w = std::max(button_width(ui, lyren::tr("Saved"), true), button_width(ui, lyren::tr("Save"), true));
                if (labelled("collect-playlist", on ? lyren::tr("Saved") : lyren::tr("Save"), on ? "star-fill" : "star", w)) {
                    toggle_collect_playlist(c);
                }
            }
            if (provider(c).capabilities.comments[2] && c.commentable && square("playlist-comments", "message-square")) {
                open_comments(ui, c.source, Subject::playlist, c.thread.empty() ? c.id : c.thread, c.title, c.cover);
            }
            if (c.owned && provider(c).capabilities.playlist_edit && square("playlist-more", "ellipsis")) {
                if (library(c.source).playlist_writes.contains(c.id)) {
                    return notify(std::string(lyren::tr("The playlist is still saving")), "list-music");
                }
                std::vector<ui::MenuItem> items;
                if (provider(c).capabilities.playlist_order && route_.list && !route_.list->loading) {
                    items.push_back({"list-ordered", std::string(lyren::tr(route_.list->ordering ? "Done reordering" : "Reorder songs")), [this, c, list = route_.list] {
                                         if (!provider(c).logged_in()) {
                                             return notify(lyren::trf("Sign in to {} to reorder this playlist", lyren::tr(platform(c.source).name)), "user");
                                         }
                                         list->ordering = !list->ordering;
                                         reset_selection();
                                         if (list->ordering) {
                                             notify(std::string(lyren::tr("Use the arrows to move songs")), "list-ordered");
                                         }
                                     }});
                }
                if (route_.list && !route_.list->loading && !route_.list->saving_order) {
                    items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this, c, list = route_.list] {
                                         spawn(load_list(list, Page::playlist, c));
                                         spawn(load_library(c.source));
                                     }});
                }
                if (auto link = share_url(c); !link.empty()) {
                    items.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                         ui.copy = link;
                                         ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                     }});
                }
                items.push_back({"pencil", std::string(lyren::tr("Rename")), [this, &ui, c] {
                                     open_editor(ui, c);
                                 }});
                items.push_back({"trash", std::string(lyren::tr("Delete playlist")),
                                 [this, &ui, c] {
                                     confirm_delete(ui, c);
                                 },
                                 true});
                ui.open_menu(x - 52, y + 46, std::move(items));
            }
        }
    }

    void Shell::track_menu(ui::Ui &ui, float x, float y, std::vector<Track> &tracks, int i) {
        Track tr = tracks[static_cast<size_t>(i)];
        bool like = liked(tr);
        auto *list = &tracks;
        const auto &caps = provider(tr).capabilities;
        if (choosing_ && chosen_.size() > 1) {
            auto selected = chosen_tracks(tracks);
            std::vector<ui::MenuItem> items{{"play", std::string(lyren::tr("Play selected")),
                                             [this, selected] {
                                                 play(selected, 0);
                                             }},
                                            {"folder-plus", std::string(lyren::tr("Add to playlist")), [this, &ui, selected] {
                                                 open_picker(ui, selected);
                                             }}};
            if (route_.page == Page::playlist && route_.card.owned && caps.playlist_edit && route_.list && list == &route_.list->tracks) {
                items.push_back({"list-minus", std::string(lyren::tr("Remove from playlist")),
                                 [this, l = route_.list, c = route_.card, selected] {
                                     remove_from_playlist(l, c, selected);
                                 },
                                 true});
            }
            ui.open_menu(x, y, std::move(items));
            return;
        }
        std::vector<ui::MenuItem> items{{"play", std::string(lyren::tr("Play")),
                                         [this, list, i] {
                                             play(*list, i);
                                         }},
                                        {"list-plus", std::string(lyren::tr("Play next")), [this, &ui, tr] {
                                             if (current_ < 0) {
                                                 queue_.insert(queue_.begin(), tr);
                                                 current_ = 0;
                                                 resume_at_ = 0;
                                             } else {
                                                 queue_.insert(queue_.begin() + (current_ + 1), tr);
                                             }
                                             shuffle_next_ = mode_ == Mode::shuffle ? current_ + 1 : -1;
                                             shuffle_history_.clear();
                                             changed();
                                             ui.toast(std::string(lyren::tr("Added to play next")), "list-plus");
                                         }}};
        if (!tr.album_id.empty()) {
            items.push_back({"disc-album", std::string(lyren::tr("View album")), [this, &ui, tr] {
                                 open(ui, Page::album,
                                      {.kind = CardKind::album, .source = tr.source, .id = tr.album_id, .title = tr.album, .subtitle = tr.artist, .cover = tr.cover});
                             }});
        }
        if (!tr.artist_id.empty()) {
            items.push_back({"mic-vocal", std::string(lyren::tr("View artist")), [this, &ui, tr] {
                                 open(ui, Page::artist, {.kind = CardKind::artist, .source = tr.source, .id = tr.artist_id, .title = tr.artist.substr(0, tr.artist.find(" / "))});
                             }});
        }
        if (caps.similar) {
            items.push_back({"sparkles", std::string(lyren::tr("Similar songs")), [this, &ui, tr] {
                                 open(ui, Page::similar, {.source = tr.source, .id = tr.id, .title = tr.title, .subtitle = tr.artist, .cover = tr.cover});
                             }});
        }
        if (caps.comments[0]) {
            items.push_back({"message-square", std::string(lyren::tr("Comments")), [this, &ui, tr] {
                                 open_comments(ui, tr.source, Subject::song, tr.id, tr.title + " · " + tr.artist, tr.cover);
                             }});
        }
        if (caps.post_write) {
            items.push_back({"square-pen", std::string(lyren::tr("Share in a post")), [this, &ui, tr] {
                                 open_composer(ui, tr.source, tr);
                             }});
        }
        items.push_back({});
        if (caps.likes) {
            items.push_back({like ? "heart-off" : "heart", std::string(lyren::tr(like ? "Unlike" : "Like")), [this, tr] {
                                 toggle_like(tr);
                             }});
        }
        if (caps.playlist_edit) {
            items.push_back({"folder-plus", std::string(lyren::tr("Add to playlist")), [this, &ui, tr] {
                                 open_picker(ui, {tr});
                             }});
        }
        items.push_back({"copy", std::string(lyren::tr("Copy title and artist")), [&ui, tr] {
                             ui.copy = tr.title + " - " + tr.artist;
                             ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
                         }});
        if (auto link = share_url(tr); !link.empty()) {
            items.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                 ui.copy = link;
                                 ui.toast(std::string(lyren::tr("Link copied")), "copy");
                             }});
        }
        bool listed = route_.list && list == &route_.list->tracks;
        if (route_.page == Page::daily && listed && provider(tr).features.daily.dislike) {
            items.push_back({"thumbs-down", std::string(lyren::tr("Not interested")), [this, l = route_.list, tr] {
                                 dislike_daily(l, tr);
                             }});
        }
        if (provider(tr).features.exclusions.set) {
            items.push_back({"thumbs-down", std::string(lyren::tr("Dislike this song")), [this, tr] {
                                 set_dislike({.id = tr.id, .name = tr.title, .cover = tr.cover, .source = tr.source}, true);
                             }});
        }
        // One's own playlist can take songs out.
        if (route_.page == Page::playlist && route_.card.owned && caps.playlist_edit && listed) {
            items.push_back({"list-minus", std::string(lyren::tr("Remove from playlist")),
                             [this, l = route_.list, c = route_.card, tr] {
                                 remove_from_playlist(l, c, {tr});
                             },
                             true});
        }
        ui.open_menu(x, y, std::move(items));
    }

    void Shell::categories_modal(ui::Ui &ui, Rect r) {
        auto &lib = library();
        const auto &t = ui.theme();
        ui.resize_modal(std::min(680.f, ui.width - 40), std::min(620.f, ui.height - 40));
        ui.label({r.x, r.y, r.w - 40, 30}, lyren::tr("All categories"), {.size = t.fs(20), .color = t.text});
        const Rect viewport{r.x, r.y + 52, r.w, r.h - 52};
        const Category *picked = nullptr;
        auto layout = [&](float y, bool draw) {
            for (const auto &[heading, categories] : lib.categories.groups) {
                if (draw) {
                    ui.label({r.x, y, r.w, 22}, heading, {.size = t.fs(13), .color = t.text_dim});
                }
                y += 30;
                float x = r.x;
                for (const auto &category : categories) {
                    const float w = std::min(chip_w(ui, category.name), r.w - 8);
                    if (x + w > r.right() - 8) {
                        x = r.x;
                        y += 38;
                    }
                    if (draw) {
                        ui.push_id(ui.id(category.id));
                        if (chip(ui, x, y, category.name, category.id == lib.category.id)) {
                            picked = &category;
                        }
                        ui.pop_id();
                    }
                    x += w + 8;
                }
                y += 54;
            }
            return y;
        };
        ui.push_id(ui.id("all-categories"));
        const float offset = ui.begin_scroll(ui.id("scroll"), viewport, layout(0, false));
        layout(viewport.y - offset, true);
        ui.end_scroll();
        ui.pop_id();
        if (picked) {
            set_category(ui, *picked);
            ui.close_modal();
        }
    }

} // namespace lyren
