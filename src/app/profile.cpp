#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <ranges>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// Listener pages. Each is a portrait: their record — the songs they play most, pressed as grooves around their
// picture — and a board of tiles for what they do. A tile opens into its full view while the record rolls aside to
// the edge of the window, where a click brings it back.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;

    namespace {
        constexpr ui::Color white{1, 1, 1, 1}, vinyl{0.055f, 0.055f, 0.065f, 1};

        // Tiles in a grid of equal cells, each spanning some of them, packed in reading order into the first place
        // they fit.
        struct Tile {
            int view; // a ProfileView, or -1 for the listening tile, which opens nothing
            int w, h;
            int col = 0, row = 0;
        };

        int pack(std::vector<Tile> &tiles, int cols) {
            std::vector<std::vector<bool>> used;
            int rows = 0;
            for (auto &tile : tiles) {
                tile.w = std::min(tile.w, cols);
                for (int at = 0;; ++at) {
                    const int col = at % cols, row = at / cols;
                    if (col + tile.w > cols) {
                        continue;
                    }
                    while (static_cast<int>(used.size()) < row + tile.h) {
                        used.emplace_back(static_cast<size_t>(cols), false);
                    }
                    bool free = true;
                    for (int y = row; y < row + tile.h && free; ++y) {
                        for (int x = col; x < col + tile.w && free; ++x) {
                            free = !used[static_cast<size_t>(y)][static_cast<size_t>(x)];
                        }
                    }
                    if (!free) {
                        continue;
                    }
                    for (int y = row; y < row + tile.h; ++y) {
                        for (int x = col; x < col + tile.w; ++x) {
                            used[static_cast<size_t>(y)][static_cast<size_t>(x)] = true;
                        }
                    }
                    tile.col = col, tile.row = row;
                    rows = std::max(rows, row + tile.h);
                    break;
                }
            }
            return rows;
        }

        // The height card_grid takes for n cards in width w.
        float grid_height(float w, size_t n) {
            constexpr float gap = 20, min_w = 150;
            if (n == 0) {
                return 0;
            }
            const int cols = std::max(1, static_cast<int>((w + gap) / (min_w + gap)));
            const float cw = (w + gap) / static_cast<float>(cols) - gap;
            const auto rows = static_cast<float>((n + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));
            return rows * (cw + 54 + gap) - gap;
        }

        // "8 years", or the days of a newer account.
        std::string age(int days) {
            return days >= 365 ? trf("{} years", days / 365) : trf("{} days", std::max(days, 1));
        }
    } // namespace

    bool Shell::own_profile(const Card &card) const {
        const auto &account = library(card.source).account;
        return account && account->id == card.id;
    }

    // Another listener's songs played, with their level, fill a tile of their own.
    bool Shell::listening_tile(const Card &card) const {
        return !own_profile(card) && provider(card).capabilities.ranking;
    }

    std::vector<Shell::ProfileView> Shell::profile_tiles(const Card &card) const {
        const auto &caps = provider(card).capabilities;
        const bool self = own_profile(card);
        std::vector<ProfileView> out;
        if (caps.posts) {
            out.push_back(ProfileView::posts);
        }
        if (caps.people) {
            out.push_back(ProfileView::follows);
        }
        if (caps.ranking) {
            out.push_back(ProfileView::ranking);
        }
        if (self ? caps.library : caps.profile_playlists) {
            out.push_back(ProfileView::playlists);
        }
        if (caps.people) {
            out.push_back(ProfileView::fans);
        }
        if (self && caps.posts) {
            out.push_back(ProfileView::feed);
        }
        return out;
    }

    void Shell::open_profile(ui::Ui &ui, Source source, std::string id, std::string name, std::string avatar, Rect from) {
        if (id.empty() || !provider(source).capabilities.profiles) {
            return;
        }
        profile_from_ = from;
        Card card{.kind = CardKind::user, .source = source, .id = std::move(id), .title = std::move(name), .cover = std::move(avatar)};
        card.owned = own_profile(card);
        show_stage(false);
        open(ui, Page::profile, card);
    }

    void Shell::open_own_profile(ui::Ui &ui, Rect from) {
        const auto &account = library().account;
        if (!account) {
            return open_account(ui);
        }
        profile_from_ = from;
        show_stage(false);
        open(ui, Page::profile, {.kind = CardKind::user, .source = active_, .id = account->id, .title = account->name, .cover = account->avatar, .owned = true});
    }

    void Shell::open_profile_view(ui::Ui &ui, ProfilePage &page, ProfileView view) {
        page.view = page.shown = view;
        page.view_time = ui.now;
        const Card &card = route_.card;
        if (view == ProfileView::follows || view == ProfileView::fans) {
            load_people(route_.profile, card, view == ProfileView::fans);
        } else if (view == ProfileView::feed) {
            auto &feed = library(card.source).feed;
            if (!feed) {
                feed = std::make_shared<PostList>();
            }
            ui.scroll_to(ui.id("profile-feed"), 0);
        }
        ui.redraw();
    }

    Task<void> Shell::load_profile(std::shared_ptr<ProfilePage> page, Card card) {
        auto &lib = library(card.source);
        const auto &caps = provider(card).capabilities;
        const uint64_t account_gen = lib.account_gen;
        const bool self = own_profile(card);
        page->loading = true;
        page->error.clear();
        if (caps.posts) {
            load_posts(page->posts, card.source, card.id);
        }
        if (caps.people) {
            load_people(page, card, 0);
            load_people(page, card, 1);
        }
        if (caps.ranking) {
            load_ranking(page, card, 0);
            load_ranking(page, card, 1);
        }
        if (!self && caps.profile_playlists) {
            spawn(load_profile_playlists(page, card));
        }
        if (self && caps.posts && !lib.feed) {
            lib.feed = std::make_shared<PostList>();
        }
        // A platform without listener pages still has the account's name and picture.
        if (!caps.profiles) {
            page->loading = false;
            page->info = Profile{.id = card.id, .name = card.title, .avatar = card.cover};
            co_return;
        }
        auto r = co_await provider(card).profile(card.id);
        if (account_gen != lib.account_gen) {
            co_return;
        }
        page->loading = false;
        if (r) {
            images_.prefetch(r->avatar, 256);
            page->info = std::move(*r);
        } else {
            page->error = r.error();
        }
        redraw_();
    }

    void Shell::load_people(std::shared_ptr<ProfilePage> page, Card card, int tab) {
        PeopleList &list = page->people[static_cast<size_t>(tab)];
        if (list.loading || !list.more || !list.error.empty()) {
            return;
        }
        list.loading = true;
        spawn(fetch_people(std::move(page), std::move(card), tab, ++list.gen));
    }

    Task<void> Shell::fetch_people(std::shared_ptr<ProfilePage> page, Card card, int tab, uint64_t gen) {
        PeopleList &list = page->people[static_cast<size_t>(tab)];
        auto r = co_await provider(card).people(card.id, tab == 1, list.cursor);
        if (gen != list.gen) {
            co_return;
        }
        list.loading = false;
        if (!r) {
            list.error = r.error();
            co_return redraw_();
        }
        if (list.people.empty()) {
            list.since = now_();
        }
        for (const auto &person : r->people) {
            images_.prefetch(person.avatar, 64);
        }
        list.people.append_range(std::views::as_rvalue(r->people));
        list.total = r->total >= 0 ? r->total : list.total;
        list.more = r->more;
        list.cursor = std::move(r->cursor);
        redraw_();
    }

    void Shell::load_ranking(std::shared_ptr<ProfilePage> page, Card card, int tab) {
        RankingList &list = page->rankings[static_cast<size_t>(tab)];
        if (list.loading || list.loaded) {
            return;
        }
        list.loading = true;
        list.error.clear();
        spawn(fetch_ranking(std::move(page), std::move(card), tab));
    }

    Task<void> Shell::fetch_ranking(std::shared_ptr<ProfilePage> page, Card card, int tab) {
        auto r = co_await provider(card).ranking(card.id, tab == 1);
        RankingList &list = page->rankings[static_cast<size_t>(tab)];
        list.loading = false;
        list.loaded = true;
        list.since = now_();
        if (r) {
            images_.prefetch(r->tracks);
            list.ranking = std::move(*r);
        } else {
            list.error = r.error();
        }
        // An empty week leaves the ranking on the songs of all time.
        if (tab == 0 && list.ranking.tracks.empty()) {
            page->ranking_tab = 1;
        }
        redraw_();
    }

    Task<void> Shell::load_profile_playlists(std::shared_ptr<ProfilePage> page, Card card) {
        page->playlists.loading = true;
        auto r = co_await provider(card).user_playlists(card.id);
        page->playlists.loading = false;
        page->playlists.loaded = true;
        page->playlists.since = now_();
        if (!r) {
            page->playlists.error = r.error();
            co_return redraw_();
        }
        std::vector<Card> cards;
        if (!r->first.id.empty()) {
            cards.push_back(std::move(r->first));
        }
        cards.append_range(std::views::as_rvalue(r->second));
        // Theirs come first. They are not the user's to edit, whoever made them.
        std::ranges::stable_partition(cards, &Card::owned);
        page->created = static_cast<int>(std::ranges::count_if(cards, &Card::owned));
        for (auto &c : cards) {
            c.owned = false;
        }
        images_.prefetch(cards);
        page->playlists.cards = std::move(cards);
        redraw_();
    }

    bool Shell::following(Source source, const Person &person) const {
        return preference_value(source, Preference::user, person.id, person.followed);
    }

    int Shell::shown_fans(Source source, const Profile &who) const {
        if (who.fans < 0) {
            return who.fans;
        }
        const bool now = following(source, {.id = who.id, .followed = who.followed});
        return std::max(0, who.fans + (now == who.followed ? 0 : now ? 1 : -1));
    }

    int Shell::people_count(const ProfilePage &page, Source source, const Profile &who, bool fans) const {
        const int n = fans ? shown_fans(source, who) : who.follows;
        return n >= 0 ? n : page.people[fans].total;
    }

    void Shell::toggle_follow_user(Source source, const Person &person) {
        if (!provider(source).logged_in()) {
            return notify(lyren::trf("Sign in to {} to follow people", lyren::tr(platform(source).name)), "user-plus");
        }
        const bool on = !following(source, person);
        notify(lyren::trf(on ? "Following {}" : "Unfollowed {}", person.name), on ? "user-check" : "user-minus");
        set_preference(source, Preference::user, {}, {.kind = CardKind::user, .source = source, .id = person.id, .title = person.name, .cover = person.avatar}, on);
    }

    float Shell::follow_button_width(ui::Ui &ui) {
        float w = 0;
        for (std::string_view label : {"Follow", "Following", "Mutual"}) {
            w = std::max(w, button_width(ui, lyren::tr(label), true));
        }
        return w;
    }

    void Shell::follow_button(ui::Ui &ui, ui::Id id, Rect r, Source source, const Person &person) {
        const bool on = following(source, person), mutual = on && person.follows_me;
        const auto label = lyren::tr(!on ? "Follow" : mutual ? "Mutual" : "Following");
        if (!ui.button(id, r, label, on ? ui::ButtonKind::secondary : ui::ButtonKind::primary, !on ? "user-plus" : mutual ? "arrow-left-right" : "user-check")) {
            return;
        }
        if (!on) {
            return toggle_follow_user(source, person);
        }
        // Unfollowing is one step further away than following.
        ui.open_menu(r.x, r.bottom() + 6,
                     {{"user-minus", std::string(lyren::tr("Unfollow")),
                       [this, source, person] {
                           toggle_follow_user(source, person);
                       },
                       true}});
    }

    void Shell::profile_backdrop(ui::Ui &ui, Rect r, const Profile &who, float in) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        p.rect(r, t.surface, t.radius_l());
        // Their picture, blurred, fills the page, and their colour glows in two lights that drift while music plays.
        const std::string &art = who.background.empty() ? who.avatar : who.background;
        if (!art.empty()) {
            const auto &e = images_.get(art, 128, true);
            if (e.ready) {
                Fade lit(p, ui.progress(e.ready_at, 700) * (t.dark ? 0.55f : 0.36f));
                p.image(r, e.tex, t.radius_l(), white, cover_uv(1, r.w / r.h));
            }
        }
        const float hue = cover_hue(who.avatar, 128);
        const ui::Color glow = hue >= 0 ? ui::hsl(hue, 0.7f, t.dark ? 0.5f : 0.62f) : t.accent;
        ui.push_clip(r);
        for (int i = 0; i < 2; ++i) {
            const float a = static_cast<float>(record_turn_ * 0.35 + i * 2.4), size = r.h * (i ? 0.95f : 1.25f);
            const float cx = r.x + r.w * (i ? 0.8f : 0.16f) + std::sin(a) * r.w * 0.05f, cy = r.y + r.h * (i ? 0.88f : 0.26f) + std::cos(a * 1.3f) * r.h * 0.05f;
            p.shadow({cx - size / 2, cy - size / 2, size, size}, size / 2, size * 0.7f, glow.with_alpha((t.dark ? 0.2f : 0.16f) * in));
        }
        ui.pop_clip();
        // A veil keeps the words readable over any picture.
        p.gradient(r, t.surface.with_alpha(t.dark ? 0.3f : 0.42f), t.surface.with_alpha(t.dark ? 0.8f : 0.84f), t.radius_l());
    }

    void Shell::profile_record(ui::Ui &ui, Rect disc, ProfilePage &page, const Profile &who, float in, float aside) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float shown = smooth01(in * 1.6f);
        // It grows out of the picture the page was opened from, and turns while music plays.
        const Rect d = ui::center(disc, disc.w * (0.84f + 0.16f * in), disc.h * (0.84f + 0.16f * in));
        const float radius = d.w / 2, cx = d.x + radius, cy = d.y + radius;
        const auto angle = static_cast<float>(record_turn_);
        // The songs of all time, or the week's when those are not known: the first at the rim, as on a record.
        const Ranking &ranking = !page.rankings[1].ranking.tracks.empty() ? page.rankings[1].ranking : page.rankings[0].ranking;
        const int n = std::min(static_cast<int>(ranking.tracks.size()), 24);
        const float label_r = radius * 0.34f, inner = radius * 0.4f, outer = radius * 0.955f, band = n ? (outer - inner) / static_cast<float>(n) : 0;

        const float dx = ui.input().x - cx, dy = ui.input().y - cy, dist = std::hypot(dx, dy);
        const ui::Id id = ui.id("record");
        auto it = ui.interact(id, d);
        const bool over = it.hovered && dist <= radius, resting = aside < 0.05f;
        int groove = -1;
        if (over && resting && n > 0 && dist >= inner && dist <= outer) {
            groove = std::clamp(static_cast<int>((outer - dist) / band), 0, n - 1);
        }
        const bool on_label = over && resting && dist <= label_r;

        p.shadow({d.x, d.y + radius * 0.08f, d.w, d.h}, radius, radius * 0.32f, ui::Color{0, 0, 0, 0.5f}.with_alpha(shown));
        p.rect(d, vinyl.with_alpha(shown), radius);
        // Fine grooves, and light catching them on either side, brightest mid-record; it moves as the record turns.
        const float step = std::max(3.f, radius / 44);
        for (float g = inner; g <= outer; g += step) {
            const Rect ring = ui::center(d, g * 2, g * 2);
            p.rect(ring, {}, g, white.with_alpha(0.03f * shown), 1);
            const float light = 1 - std::abs((g - inner) / (outer - inner) - 0.55f) * 1.4f;
            for (float side : {0.f, std::numbers::pi_v<float>}) {
                p.arc(ring.inset(-0.5f), 1.5f, angle + side - 0.32f, 0.64f, white.with_alpha(0.05f * light * shown));
                p.arc(ring.inset(-0.5f), 1.5f, angle + side - 0.13f, 0.26f, white.with_alpha(0.07f * light * shown));
            }
        }
        for (int i = 0; i < n; ++i) {
            const float ro = outer - band * static_cast<float>(i), score = static_cast<float>(ranking.scores[static_cast<size_t>(i)]) / 100;
            const float hot = ui.anim(ui::sub_id(id, static_cast<uint64_t>(i) + 1), groove == i ? 1 : 0, 140);
            const float lit = std::max(hot, is_current(ranking.tracks[static_cast<size_t>(i)]) ? 0.8f : 0.f);
            // The most played shine a little; the one under the pointer, or playing, takes the accent.
            const ui::Color c = white.with_alpha(0.025f + 0.11f * score * score).mix(t.accent, lit);
            p.rect(ui::center(d, ro * 2, ro * 2), {}, ro, c.with_alpha(c.a * shown), std::max(1.f, band * (0.3f + 0.45f * hot)));
        }

        // The label: their picture, flying in from where they were found.
        Rect label = ui::center(d, label_r * 2, label_r * 2);
        if (page.origin.w > 0 && in < 1) {
            label = mix(page.origin, label, in);
        }
        p.rect(label.inset(-3), white.with_alpha(0.1f * shown), label.w / 2 + 3);
        cover(ui, label, who.avatar, label.w / 2, "user");
        const float label_hot = ui.anim(ui::sub_id(id, 0x1ab), on_label && n > 0 ? 1 : 0, 160);
        if (label_hot > 0) {
            p.rect(label, ui::Color{0, 0, 0, 0.45f * label_hot}, label.w / 2);
            p.icon("play-fill", ui::center(label, 24, 24), white.with_alpha(label_hot));
        }
        const float mark_r = label.w / 2 * 0.8f;
        p.rect({label.x + label.w / 2 + std::sin(angle) * mark_r - 2.5f, label.y + label.h / 2 - std::cos(angle) * mark_r - 2.5f, 5, 5}, white.with_alpha(0.65f * shown), 2.5f);

        if (over && (groove >= 0 || on_label || !resting)) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (it.clicked) {
            if (!resting) {
                page.view = ProfileView::board;
            } else if (groove >= 0) {
                play(ranking.tracks, groove);
            } else if (on_label && n > 0) {
                play(ranking.tracks, 0);
            }
        }
        // The groove under the pointer names its song.
        if (groove >= 0) {
            const Track &tr = ranking.tracks[static_cast<size_t>(groove)];
            const int plays = ranking.plays[static_cast<size_t>(groove)];
            const std::string top = std::format("{}  {}", groove + 1, tr.title);
            const std::string sub = plays > 0 ? std::format("{} · {}", tr.artist, lyren::trf("{} plays", plays)) : tr.artist;
            const TextStyle a{.size = t.fs(13), .color = t.text}, b{.size = t.fs(11), .color = t.text_dim};
            const float w = std::min(320.f, std::max(p.measure(top, a), p.measure(sub, b)) + 28);
            Rect tip{ui.input().x + 18, ui.input().y - 52, w, 46};
            p.shadow({tip.x, tip.y + 4, tip.w, tip.h}, t.radius, 16, t.shadow);
            p.rect(tip, t.elevated.with_alpha(0.97f), t.radius, t.border, 1);
            p.text(tip.x + 14, tip.y + 20, top, a, tip.w - 28);
            p.text(tip.x + 14, tip.y + 37, sub, b, tip.w - 28);
        }
    }

    void Shell::profile_head(ui::Ui &ui, Rect r, ProfilePage &page, const Profile &who, bool self, bool centered) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = route_.card;
        float y = r.y;
        auto line_x = [&](float w) {
            return centered ? r.x + (r.w - w) / 2 : r.x;
        };

        // The name, with the level and membership after it.
        const TextStyle name{.size = t.fs(26), .color = t.text}, badge{.size = t.fs(11), .color = t.accent};
        const std::string level = who.level >= 0 ? std::format("Lv.{}", who.level) : "";
        const float lw = level.empty() ? 0 : p.measure(level, badge) + 14, vw = who.vip ? p.measure("VIP", badge) + 14 : 0;
        const float badges = (lw > 0 ? lw + 8 : 0) + (vw > 0 ? vw + 6 : 0);
        const float nw = std::min(p.measure(who.name, name) + 1, r.w - badges);
        float x = line_x(nw + badges);
        ui.label({x, y, nw, 38}, who.name, name);
        x += nw + 8;
        if (lw > 0) {
            Rect b{x, y + 10, lw, 20};
            p.rect(b, {}, 10, t.accent.with_alpha(0.55f), 1);
            ui.label(b, level, badge, Align::center);
            x += lw + 6;
        }
        if (vw > 0) {
            Rect b{x, y + 10, vw, 20};
            p.rect(b, t.accent.with_alpha(0.18f), 10);
            ui.label(b, "VIP", badge, Align::center);
        }
        y += 44;

        // That they follow the user, until the follow button says it is mutual; then their own words.
        if (!self && who.follows_me && !following(card.source, {.id = who.id, .followed = who.followed})) {
            const auto words = lyren::tr("Follows you");
            const TextStyle st{.size = t.fs(11), .color = t.text_dim};
            const float w = p.measure(words, st) + 34;
            Rect chip{line_x(w), y, w, 22};
            p.rect(chip, t.text.with_alpha(0.07f), 11);
            p.icon("user-check", {chip.x + 9, chip.y + 4, 14, 14}, t.text_dim);
            ui.label({chip.x + 27, chip.y, w - 27, chip.h}, words, st);
            y += 30;
        }
        if (!page.error.empty() && !page.info) {
            // What went wrong, where their words would be; the rest of the page loads on its own.
            const TextStyle st{.size = t.fs(12), .color = t.text_dim};
            const auto error = lyren::tr(page.error);
            const float w = std::min(p.measure(error, st) + 21, r.w - 16), ex = line_x(w);
            p.icon("cloud-off", {ex, y + 3, 14, 14}, t.text_dim);
            p.text(ex + 20, y + 15, error, st, w - 20);
            y += 28;
        } else if (!who.signature.empty()) {
            const TextStyle st{.size = t.fs(13), .color = t.text.mix(t.text_dim, 0.4f)};
            auto lines = p.wrap_lines(who.signature, st.size, r.w - 16);
            for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i) {
                // The second line takes the rest, ellipsized.
                std::string_view s = i == 1 ? std::string_view(lines[1].data(), who.signature.data() + who.signature.size() - lines[1].data()) : lines[0];
                if (auto nl = s.find('\n'); i == 1 && nl != s.npos) {
                    s = s.substr(0, nl);
                }
                const float w = std::min(p.measure(s, st) + 1, r.w - 16);
                ui.label({line_x(w), y, w, 20}, s, st);
                y += 20;
            }
            y += 6;
        }

        // Where they are and how long they have been here; how much they listen, unless a tile tells it.
        std::vector<std::pair<std::string_view, std::string>> facts;
        if (!who.location.empty()) {
            facts.emplace_back("map-pin", who.location);
        }
        if (who.days >= 0) {
            facts.emplace_back("history", lyren::trf("Here for {}", age(who.days)));
        }
        if (who.listened >= 0 && !listening_tile(card)) {
            facts.emplace_back("headphones", lyren::trf("{} songs played", grouped(who.listened)));
        }
        if (!facts.empty()) {
            const TextStyle st{.size = t.fs(12), .color = t.text_dim, .features = text::tabular_nums};
            float w = 0;
            for (const auto &[icon, text] : facts) {
                w += 18 + p.measure(text, st) + 16;
            }
            float fx = line_x(w - 16);
            for (const auto &[icon, text] : facts) {
                p.icon(icon, {fx, y + 3, 13, 13}, t.text_dim);
                const float tw = p.measure(text, st);
                ui.label({fx + 18, y, tw + 1, 20}, text, st);
                fx += 18 + tw + 16;
            }
            y += 30;
        }

        // What the user can do here.
        y += 6;
        struct Action {
            std::string_view id, label, icon;
            ui::ButtonKind kind;
            float w;
        };
        std::vector<Action> actions;
        const auto &caps = provider(card).capabilities;
        const Ranking &ranking = !page.rankings[1].ranking.tracks.empty() ? page.rankings[1].ranking : page.rankings[0].ranking;
        if (self) {
            if (caps.post_write) {
                actions.push_back({"profile-write", lyren::tr("New post"), "square-pen", ui::ButtonKind::primary, 0});
            }
            actions.push_back({"profile-accounts", lyren::tr("Accounts"), "user", ui::ButtonKind::secondary, 0});
        } else {
            if (caps.follow_users) {
                actions.push_back({"profile-follow", "", "", ui::ButtonKind::primary, follow_button_width(ui)});
            }
            if (!ranking.tracks.empty()) {
                actions.push_back({"profile-play", lyren::tr("Play their songs"), "play-fill", ui::ButtonKind::secondary, 0});
            }
        }
        float total = 52;
        for (auto &a : actions) {
            a.w = a.w > 0 ? a.w : button_width(ui, a.label, true);
            total += a.w + 10;
        }
        float bx = line_x(total - 10);
        for (const auto &a : actions) {
            Rect b{bx, y, a.w, 38};
            if (a.id == "profile-follow") {
                follow_button(ui, ui.id(a.id), b, card.source, {.id = who.id, .name = who.name, .avatar = who.avatar, .followed = who.followed, .follows_me = who.follows_me});
            } else if (ui.button(ui.id(a.id), b, a.label, a.kind, a.icon)) {
                if (a.id == "profile-write") {
                    open_composer(ui, card.source);
                } else if (a.id == "profile-accounts") {
                    open_account(ui);
                } else if (a.id == "profile-play") {
                    play(ranking.tracks, 0);
                }
            }
            bx += a.w + 10;
        }
        if (ui.button(ui.id("profile-more"), {bx, y, 42, 38}, "", ui::ButtonKind::secondary, "ellipsis")) {
            std::vector<ui::MenuItem> items;
            if (auto link = share_url(card); !link.empty()) {
                items.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                     ui.copy = link;
                                     ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                 }});
            }
            items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this] {
                                 route_.profile = std::make_shared<ProfilePage>();
                                 route_.profile->since = -10;
                                 spawn(load_profile(route_.profile, route_.card));
                             }});
            ui.open_menu(bx, y + 44, std::move(items));
        }
    }

    Rect Shell::profile_tile(ui::Ui &ui, ui::Id id, Rect r, std::string_view icon, std::string_view title, std::string_view note, bool &clicked) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        clicked = false;
        float hot = 0, down = 0;
        if (id) {
            auto it = ui.interact(id, r);
            const auto feel = ui.feel(id, it, 200);
            hot = feel.hot, down = feel.down;
            clicked = it.clicked;
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
        }
        Rect b = r.inset(down * 2);
        b.y -= 3 * hot;
        p.shadow({b.x, b.y + 8, b.w, b.h}, t.radius_l(), 26, t.shadow.with_alpha(0.22f + 0.5f * hot));
        p.rect(b, t.surface.with_alpha(t.dark ? 0.62f : 0.74f).mix(t.elevated.with_alpha(0.9f), 0.35f * hot), t.radius_l(), t.text.with_alpha(0.07f + 0.06f * hot), 1);
        p.icon(icon, {b.x + 16, b.y + 16, 16, 16}, t.accent);
        ui.label({b.x + 40, b.y + 11, b.w - 56, 26}, title, {.size = t.fs(13), .color = t.text});
        if (id && hot > 0) {
            p.icon("chevron-right", {b.right() - 30 + 4 * hot, b.y + 15, 16, 16}, t.text_dim.with_alpha(hot));
        }
        ui.label({b.x, b.y + 11, b.w - 16 - 20 * hot, 26}, note, {.size = t.fs(12), .color = t.text_dim, .features = text::tabular_nums}, Align::right);
        return {b.x + 16, b.y + 46, b.w - 32, b.h - 60};
    }

    float Shell::profile_board(ui::Ui &ui, Rect r, ProfilePage &page, const Profile &who, bool self, bool fill) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = route_.card;
        const auto &caps = provider(card).capabilities;
        const auto views = profile_tiles(card);
        std::vector<Tile> tiles;
        for (ProfileView view : views) {
            Tile tile{static_cast<int>(view), 1, 1};
            if (view == ProfileView::posts) {
                tile.w = 2;
            } else if (view == ProfileView::ranking) {
                tile.h = 2;
            } else if (view == ProfileView::playlists) {
                tile.h = 2, tile.w = caps.ranking ? 1 : 2;
            }
            tiles.push_back(tile);
        }
        if (listening_tile(card)) {
            tiles.push_back({-1, 1, 1});
        }
        const int cols = tiles.size() <= 2 ? 2 : r.w >= 520 ? 3 : 2;
        const int rows = pack(tiles, cols);
        constexpr float gap = 14;
        const float cw = (r.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float ch = fill ? std::clamp((r.h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows), 118.f, 236.f) : 150;
        const float grid_h = static_cast<float>(rows) * ch + static_cast<float>(rows - 1) * gap;
        const float top = fill ? r.y + std::max(0.f, (r.h - grid_h) / 2) : r.y;
        const TextStyle dim{.size = t.fs(12), .color = t.text_dim, .features = text::tabular_nums};
        const Library &lib = library(card.source);

        for (size_t i = 0; i < tiles.size(); ++i) {
            const Tile &tile = tiles[i];
            Rect cell{r.x + static_cast<float>(tile.col) * (cw + gap), top + static_cast<float>(tile.row) * (ch + gap),
                      static_cast<float>(tile.w) * cw + static_cast<float>(tile.w - 1) * gap, static_cast<float>(tile.h) * ch + static_cast<float>(tile.h - 1) * gap};
            // The tiles settle one after another as the page opens.
            const float a = ui.progress(page.since + 0.16 + 0.05 * static_cast<double>(i), 620, ease::emphasized);
            Fade fade(p, a);
            cell.y += (1 - a) * 20;
            const auto view = static_cast<ProfileView>(tile.view);
            if (tile.view >= 0) {
                page.tiles[static_cast<size_t>(tile.view)] = cell;
            }
            ui.push_id(ui.id(static_cast<uint64_t>(tile.view + 1)));
            bool clicked = false;
            switch (tile.view < 0 ? ProfileView::board : view) {
                case ProfileView::posts: {
                    const PostList &list = *page.posts;
                    // Their latest post, its picture or song beside it; a song plays from its cover.
                    const Post *post = list.posts.empty() ? nullptr : &list.posts.front();
                    const Post *shown = post && post->text.empty() && !post->forwarded.empty() ? &post->forwarded.front() : post;
                    const std::string art = !shown                     ? ""
                                            : !shown->pictures.empty() ? shown->pictures.front().url
                                            : shown->track             ? shown->track->cover
                                            : shown->card              ? shown->card->cover
                                                                       : "";
                    const float aw = art.empty() ? 0 : std::min(cell.h - 60, 132.f);
                    const Rect pic{cell.right() - 16 - aw, cell.y + 46, aw, aw};
                    auto play = shown && shown->track && aw > 0 ? ui.interact(ui.id("tile-play"), pic) : ui::Interaction{};
                    Rect write{cell.right() - 16 - 30, cell.y + 10, 30, 30};
                    const bool writable = self && caps.post_write;
                    auto pen = writable ? ui.interact(ui.id("tile-write"), write) : ui::Interaction{};
                    Rect c = profile_tile(ui, ui.id("tile"), cell, "message-square", lyren::tr("Posts"), writable ? "" : who.posts >= 0 ? grouped(who.posts) : "", clicked);
                    if (writable) {
                        const float hot = ui.anim(ui.id("tile-write"), pen.hovered ? 1 : 0, 140);
                        p.rect(write, t.accent.with_alpha(0.14f + 0.16f * hot), 15);
                        p.icon("square-pen", ui::center(write, 15, 15), t.accent);
                        if (pen.clicked) {
                            open_composer(ui, card.source);
                        }
                    }
                    if (!post) {
                        if (list.loading || list.more) {
                            ui.skeleton({c.x, c.y + 4, c.w * 0.8f, 12}, 6);
                            ui.skeleton({c.x, c.y + 26, c.w * 0.5f, 12}, 6);
                        } else {
                            ui.label({c.x, c.y, c.w, 22}, lyren::tr(self ? "Share what you are listening to" : "No posts yet"), {.size = t.fs(14), .color = t.text_dim});
                        }
                        break;
                    }
                    if (aw > 0) {
                        const Rect at{c.right() - aw, c.y, aw, aw};
                        cover(ui, at, art, t.radius, "image");
                        if (shown->track) {
                            const bool on = is_current(*shown->track);
                            const float hot = ui.anim(ui.id("tile-play"), play.hovered ? 1 : 0, 140);
                            const Rect knob = ui::center(at, 34 + 4 * hot, 34 + 4 * hot);
                            p.rect(knob, ui::Color{0, 0, 0, 0.4f}.mix(t.accent, hot), knob.w / 2);
                            p.icon(on && playing() ? "pause-fill" : "play-fill", ui::center(knob, 14, 14), white);
                            if (play.hovered) {
                                ui.cursor = wl::Cursor::pointer;
                            }
                            if (play.clicked) {
                                on ? toggle_play() : play_now(*shown->track);
                            }
                        }
                    }
                    const float tw = c.w - (aw > 0 ? aw + 16 : 0);
                    ui.label({c.x, c.y - 2, tw, 18}, post_time(post->time), dim);
                    const TextStyle body{.size = t.fs(15), .color = t.text};
                    const float lh = std::round(body.size * 1.55f);
                    const std::string_view text = shown->text.empty() ? std::string_view(shown->track ? shown->track->title : shown->resource) : std::string_view(shown->text);
                    const auto lines = p.wrap_lines(text, body.size, tw);
                    const auto marks = highlights(text);
                    const auto fit = static_cast<size_t>(std::max(1.f, (c.h - 24 - (shown->track ? 24 : 0)) / lh));
                    float ly = c.y + 22;
                    for (size_t k = 0; k < std::min(fit, lines.size()); ++k) {
                        if (k + 1 == fit && k + 1 < lines.size()) {
                            // The last line that fits takes the rest, ellipsized.
                            std::string_view rest{lines[k].data(), static_cast<size_t>(lines.back().data() + lines.back().size() - lines[k].data())};
                            p.text(c.x, ly + lh * 0.72f, rest.substr(0, rest.find('\n')), body, tw);
                        } else {
                            rich_line(p, c.x, ly + lh * 0.72f, text, lines[k], marks, body, t.accent);
                        }
                        ly += lh;
                    }
                    if (shown->track && ly + 22 <= c.bottom() + 6) {
                        p.icon("music", {c.x, ly + 5, 13, 13}, t.accent);
                        p.text(c.x + 19, ly + 16, shown->track->title + " · " + shown->track->artist, {.size = t.fs(12), .color = t.text_dim}, tw - 19);
                    }
                    break;
                }
                case ProfileView::ranking: {
                    const RankingList &week = page.rankings[0], &all = page.rankings[1];
                    const bool weekly = !week.ranking.tracks.empty() || !week.loaded;
                    const RankingList &shown = weekly ? week : all;
                    Rect c = profile_tile(ui, ui.id("tile"), cell, "audio-lines", lyren::tr("Most played"), lyren::tr(weekly ? "This week" : "All time"), clicked);
                    if (shown.ranking.tracks.empty()) {
                        if (!shown.loaded) {
                            for (int k = 0; k < 4; ++k) {
                                ui.skeleton({c.x, c.y + 8 + static_cast<float>(k) * 40, c.w * (0.9f - 0.12f * static_cast<float>(k)), 12}, 6);
                            }
                        } else {
                            const bool hidden = !shown.error.empty();
                            const Rect mid = ui::center(c, c.w, 76);
                            Rect badge{mid.x + (mid.w - 40) / 2, mid.y, 40, 40};
                            p.rect(badge, t.text.with_alpha(0.06f), 20);
                            p.icon(hidden ? "lock" : "headphones", ui::center(badge, 18, 18), t.text_dim);
                            ui.label({mid.x, mid.y + 52, mid.w, 20}, hidden ? lyren::tr("Their ranking is private") : lyren::tr("Nothing played yet"),
                                     {.size = t.fs(13), .color = t.text_dim}, Align::center);
                        }
                        break;
                    }
                    const auto &tracks = shown.ranking.tracks;
                    constexpr float row_h = 44;
                    const auto fit = static_cast<size_t>(std::max(1.f, (c.h + 6) / row_h));
                    for (size_t k = 0; k < std::min(fit, tracks.size()); ++k) {
                        const float ry = c.y + static_cast<float>(k) * row_h;
                        const TextStyle rank{.size = t.fs(15), .color = k < 3 ? t.accent : t.text_dim, .features = text::tabular_nums};
                        ui.label({c.x, ry, 22, 22}, std::to_string(k + 1), rank);
                        p.text(c.x + 26, ry + 15, tracks[k].title, {.size = t.fs(13), .color = is_current(tracks[k]) ? t.accent : t.text}, c.w - 26);
                        p.text(c.x + 26, ry + 31, tracks[k].artist, {.size = t.fs(11), .color = t.text_dim}, c.w - 26);
                        const float score = static_cast<float>(shown.ranking.scores[k]) / 100;
                        p.rect({c.x + 26, ry + 37, (c.w - 26) * score, 2}, t.accent.with_alpha(0.45f), 1);
                    }
                    break;
                }
                case ProfileView::follows:
                case ProfileView::fans: {
                    const bool fans = view == ProfileView::fans;
                    const PeopleList &list = page.people[fans];
                    Rect c = profile_tile(ui, ui.id("tile"), cell, fans ? "heart" : "users", lyren::tr(fans ? "Fans" : "Follows"), "", clicked);
                    const int count = people_count(page, card.source, who, fans);
                    ui.label({c.x, c.y - 4, c.w, 44}, count >= 0 ? (count >= 100000 ? short_number(count) : grouped(count)) : "–",
                             {.size = t.fs(34), .color = t.text, .features = text::tabular_nums});
                    // The first of them, a few faces overlapping.
                    constexpr float face = 28, step = 20;
                    const auto fit = static_cast<size_t>(std::max(0.f, (c.w - face) / step + 1));
                    const size_t faces = std::min({fit, size_t{6}, list.people.size()});
                    const float fy = c.bottom() - face;
                    if (fy > c.y + 40) {
                        for (size_t k = faces; k-- > 0;) {
                            Rect f{c.x + static_cast<float>(k) * step, fy, face, face};
                            p.rect(f.inset(-2), t.surface, face / 2 + 2);
                            cover(ui, f, list.people[k].avatar, face / 2, "user");
                        }
                        if (faces == 0 && list.loading) {
                            ui.skeleton({c.x, fy, face, face}, face / 2);
                        } else if (faces == 0 && !list.error.empty()) {
                            const bool out = !provider(card).logged_in();
                            p.icon(out ? "user" : "lock", {c.x, fy + 7, 14, 14}, t.text_dim);
                            p.text(c.x + 20, fy + 19, out ? lyren::tr("Sign in to see them") : lyren::tr(list.error), {.size = t.fs(11), .color = t.text_dim}, c.w - 20);
                        }
                    }
                    break;
                }
                case ProfileView::playlists: {
                    std::vector<const Card *> cards;
                    if (self) {
                        if (!lib.liked.id.empty()) {
                            cards.push_back(&lib.liked);
                        }
                        for (const auto &c : lib.playlists) {
                            cards.push_back(&c);
                        }
                    } else {
                        for (const auto &c : page.playlists.cards) {
                            cards.push_back(&c);
                        }
                    }
                    const int count = self || page.playlists.loaded ? static_cast<int>(cards.size()) : who.playlists;
                    Rect c = profile_tile(ui, ui.id("tile"), cell, "list-music", lyren::tr("Playlists"), count >= 0 ? grouped(count) : "", clicked);
                    // Four covers as one square, and the first names beside or below it.
                    const bool beside = c.w > c.h * 1.3f;
                    const float side = std::floor(beside ? c.h : std::min(c.w, c.h - 44));
                    const float half = (side - 6) / 2;
                    for (int k = 0; k < 4; ++k) {
                        Rect q{c.x + static_cast<float>(k % 2) * (half + 6), c.y + static_cast<float>(k / 2) * (half + 6), half, half};
                        if (static_cast<size_t>(k) < cards.size()) {
                            cover(ui, q, cards[static_cast<size_t>(k)]->cover, t.radius_s(), "list-music");
                        } else {
                            p.rect(q, t.text.with_alpha(0.05f), t.radius_s());
                        }
                    }
                    const Rect names = beside ? Rect{c.x + side + 18, c.y, c.w - side - 18, c.h} : Rect{c.x, c.y + side + 10, c.w, c.bottom() - c.y - side - 10};
                    const auto lines = static_cast<size_t>(std::max(0.f, names.h / 22));
                    for (size_t k = 0; k < std::min(lines, cards.size()); ++k) {
                        p.text(names.x, names.y + 15 + static_cast<float>(k) * 22, cards[k]->title, {.size = t.fs(13), .color = k ? t.text_dim : t.text}, names.w);
                    }
                    break;
                }
                case ProfileView::feed: {
                    Rect c = profile_tile(ui, ui.id("tile"), cell, "messages-square", lyren::tr("Moments"), "", clicked);
                    const auto &feed = lib.feed;
                    if (!feed || feed->posts.empty()) {
                        if (feed && !feed->loading && !feed->more) {
                            ui.label({c.x, c.y, c.w, 20}, lyren::tr("No posts from people you follow"), {.size = t.fs(12), .color = t.text_dim});
                        } else {
                            if (feed && feed->more && !feed->loading && feed->error.empty()) {
                                load_posts(feed, card.source, "");
                            }
                            ui.skeleton({c.x, c.y + 2, 26, 26}, 13);
                            ui.skeleton({c.x + 34, c.y + 8, c.w * 0.5f, 10}, 5);
                        }
                        break;
                    }
                    const Post &post = feed->posts.front();
                    cover(ui, {c.x, c.y, 26, 26}, post.author.avatar, 13, "user");
                    p.text(c.x + 34, c.y + 17, post.author.name, {.size = t.fs(12), .color = t.text}, c.w - 34);
                    const std::string_view text = post.text.empty() && post.track ? std::string_view(post.track->title) : std::string_view(post.text);
                    const auto lines = p.wrap_lines(text, t.fs(12), c.w);
                    const auto fit = static_cast<size_t>(std::max(0.f, (c.h - 34) / 19));
                    for (size_t k = 0; k < std::min(fit, lines.size()); ++k) {
                        ui.label({c.x, c.y + 34 + static_cast<float>(k) * 19, c.w, 19}, lines[k], {.size = t.fs(12), .color = t.text_dim});
                    }
                    break;
                }
                default: {
                    // Listening, which opens nothing: songs played, and the level as a meter of ten.
                    Rect c = profile_tile(ui, 0, cell, "headphones", lyren::tr("Listening"), who.level >= 0 ? std::format("Lv.{}", who.level) : "", clicked);
                    ui.label({c.x, c.y - 4, c.w, 40}, who.listened >= 0 ? grouped(who.listened) : "–", {.size = t.fs(30), .color = t.text, .features = text::tabular_nums});
                    ui.label({c.x, c.y + 38, c.w, 18}, lyren::tr("songs played"), dim);
                    if (who.level >= 0 && c.h > 76) {
                        constexpr float seg_gap = 4;
                        const float seg = (c.w - seg_gap * 9) / 10;
                        const float lit = ui.progress(page.since + 0.5, 900, ease::emphasized) * static_cast<float>(std::min(who.level, 10));
                        for (int k = 0; k < 10; ++k) {
                            const Rect s{c.x + static_cast<float>(k) * (seg + seg_gap), c.bottom() - 6, seg, 5};
                            p.rect(s, t.text.with_alpha(0.08f), 2.5f);
                            if (const float f = std::clamp(lit - static_cast<float>(k), 0.f, 1.f); f > 0) {
                                p.rect({s.x, s.y, s.w * f, s.h}, t.accent.with_alpha(0.5f + 0.05f * static_cast<float>(k)), 2.5f);
                            }
                        }
                    }
                    break;
                }
            }
            ui.pop_id();
            if (clicked && tile.view >= 0) {
                open_profile_view(ui, page, view);
            }
        }
        return grid_h;
    }

    void Shell::profile_view(ui::Ui &ui, Rect r, ProfilePage &page, ProfileView view, const Profile &who, bool self) {
        const auto &t = ui.theme();
        const Card &card = route_.card;
        const bool closable = profile_tiles(card).size() > 1;
        Rect head{r.x + 22, r.y + 18, r.w - 44, 40};
        float x = head.x;
        if (closable) {
            if (ui.icon_button(ui.id("profile-back"), {head.x - 8, head.y + 4, 32, 32}, "chevron-left", 20)) {
                page.view = ProfileView::board;
            }
            x += 30;
        }
        static constexpr std::array<std::string_view, 7> titles = {"", "Posts", "Most played", "Follows", "Fans", "Playlists", "Moments"};
        ui.label({x, head.y - 2, head.w / 2, 28}, lyren::tr(titles[static_cast<size_t>(view)]), {.size = t.fs(20), .color = t.text});
        ui.label({x, head.y + 24, head.w / 2, 18}, view == ProfileView::feed ? lyren::tr("From people you follow") : std::string_view(who.name),
                 {.size = t.fs(12), .color = t.text_dim});

        // What each view can do, at the right of its head.
        const auto &caps = provider(card).capabilities;
        float right = head.right();
        if ((view == ProfileView::posts || view == ProfileView::feed) && self && caps.post_write) {
            const auto label = lyren::tr("New post");
            const float w = button_width(ui, label, true);
            if (ui.button(ui.id("view-write"), {right - w, head.y + 2, w, 36}, label, ui::ButtonKind::primary, "square-pen")) {
                open_composer(ui, card.source);
            }
            right -= w + 8;
        }
        if (view == ProfileView::posts || view == ProfileView::feed) {
            auto &list = view == ProfileView::feed ? library(card.source).feed : page.posts;
            ui.disabled = !list || list->loading;
            if (ui.icon_button(ui.id("view-refresh"), {right - 36, head.y + 4, 32, 32}, "refresh-cw", 17)) {
                list = std::make_shared<PostList>();
                load_posts(list, card.source, view == ProfileView::feed ? "" : card.id);
            }
            ui.disabled = false;
        } else if (view == ProfileView::follows || view == ProfileView::fans) {
            std::array<std::string, 2> labels;
            for (int k = 0; k < 2; ++k) {
                const int n = people_count(page, card.source, who, k);
                labels[static_cast<size_t>(k)] = std::string(lyren::tr(k ? "Fans" : "Follows")) + (n >= 0 ? " " + grouped(n) : "");
            }
            const std::array<std::string_view, 2> views = {labels[0], labels[1]};
            int tab = view == ProfileView::fans;
            const float w = std::min(300.f, head.w / 2);
            if (ui.segmented(ui.id("people-tabs"), {right - w, head.y + 4, w, 34}, views, tab)) {
                page.view = page.shown = tab ? ProfileView::fans : ProfileView::follows;
                page.view_time = ui.now;
                load_people(route_.profile, card, tab);
            }
        } else if (view == ProfileView::ranking) {
            const auto label = lyren::tr("Play all");
            const float w = button_width(ui, label, true);
            auto &tracks = page.rankings[static_cast<size_t>(page.ranking_tab)].ranking.tracks;
            if (ui.button(ui.id("ranking-play"), {right - w, head.y + 2, w, 36}, label, ui::ButtonKind::primary, "play-fill") && !tracks.empty()) {
                play(tracks, 0);
            }
            right -= w + 12;
            const std::array<std::string_view, 2> labels = {lyren::tr("This week"), lyren::tr("All time")};
            const float sw = std::min(220.f, right - x - 160);
            if (sw > 120 && ui.segmented(ui.id("ranking-tabs"), {right - sw, head.y + 4, sw, 34}, labels, page.ranking_tab)) {
                page.view_time = ui.now;
                ui.scroll_to(ui.id("profile-ranking"), 0);
            }
        }

        Rect body{r.x + 8, head.bottom() + 14, r.w - 16, r.bottom() - head.bottom() - 22};
        switch (view) {
            case ProfileView::posts:
                post_list(ui, body, ui.id("profile-posts"), page.posts, card.source, card.id, false, std::max(page.view_time, page.posts->since));
                break;
            case ProfileView::feed: {
                auto &feed = library(card.source).feed;
                if (!feed) {
                    feed = std::make_shared<PostList>();
                }
                post_list(ui, body, ui.id("profile-feed"), feed, card.source, "", true, std::max(page.view_time, feed->since));
                break;
            }
            case ProfileView::ranking:
                ranking_view(ui, body, page);
                break;
            case ProfileView::follows:
            case ProfileView::fans:
                people_view(ui, body, page, self);
                break;
            case ProfileView::playlists:
                playlists_view(ui, body, page, self);
                break;
            case ProfileView::board:
                break;
        }
    }

    void Shell::people_view(ui::Ui &ui, Rect r, ProfilePage &page, bool self) {
        const auto &t = ui.theme();
        const Card &card = route_.card;
        const bool fans = page.shown == ProfileView::fans;
        PeopleList &list = page.people[fans];
        const auto tab_since = std::max(page.view_time, list.since);
        if (list.people.empty()) {
            if (list.more && !list.loading && list.error.empty()) {
                load_people(route_.profile, card, fans);
            }
            if (!list.error.empty() && !provider(card).logged_in()) {
                page_status(ui, r.inset(12, 0), false, "", true);
                return;
            }
            std::string error = list.error;
            if (error.empty() && !list.loading && !list.more) {
                error = lyren::tr(fans ? (self ? "No fans yet" : "No fans to show") : (self ? "You are not following anyone yet" : "Not following anyone yet"));
            }
            page_status(ui, r.inset(12, 0), list.loading || (list.more && error.empty()), error, false, Placeholder::faces);
            return;
        }
        constexpr float gap = 12, row_h = 76;
        Rect inner = r.inset(12, 0);
        const int cols = std::max(1, static_cast<int>((inner.w + gap) / (280 + gap)));
        const float cw = (inner.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const size_t rows = (list.people.size() + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
        const float content = static_cast<float>(rows) * (row_h + gap) + (list.loading || list.more ? 56 : 12);
        const ui::Id id = ui.id(fans ? "profile-fans" : "profile-follows");
        const float off = ui.begin_scroll(id, r, content);
        const Rect clip = ui.clip();
        int shown = 0;
        for (size_t i = 0; i < list.people.size(); ++i) {
            Rect cell{inner.x + static_cast<float>(i % static_cast<size_t>(cols)) * (cw + gap), r.y - off + static_cast<float>(i / static_cast<size_t>(cols)) * (row_h + gap), cw,
                      row_h};
            if (cell.bottom() < clip.y || cell.y > clip.bottom()) {
                continue;
            }
            const float a = ui.progress(tab_since + 0.025 * std::min(shown++, 16), 420);
            Fade fade(ui.painter(), a);
            cell.y += (1 - a) * 12;
            person_card(ui, cell, card.source, list.people[i]);
        }
        const float end = r.y - off + static_cast<float>(rows) * (row_h + gap);
        if (list.loading) {
            ui.spinner(ui::center({r.x, end, r.w, 48}, 20, 20), t.text_dim, 2);
        } else if (!list.error.empty()) {
            ui.label({r.x, end + 8, r.w, 24}, lyren::tr(list.error), {.size = t.fs(12), .color = t.text_dim}, Align::center);
        } else if (list.more && end < r.bottom() + 400) {
            load_people(route_.profile, card, fans);
        }
        ui.end_scroll();
    }

    void Shell::person_card(ui::Ui &ui, Rect r, Source source, const Person &person) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ui.push_id(ui.id(person.id));
        const ui::Id id = ui.id("person");
        const float hot = ui.anim(id, ui.hovering(r) || ui.is_active(id) ? 1 : 0, 160);
        p.rect(r, t.text.with_alpha(0.035f + 0.045f * hot), t.radius_l());
        Rect face{r.x + 14, r.y + (r.h - 48) / 2, 48, 48};
        cover(ui, face, person.avatar, 24, "user");
        const auto &account = library(source).account;
        const bool followable = provider(source).capabilities.follow_users && !(account && account->id == person.id);
        const float bw = followable ? follow_button_width(ui) : 0;
        const float tx = face.right() + 14, tw = r.right() - tx - (bw > 0 ? bw + 24 : 14);
        p.text(tx, r.y + 32, person.name, {.size = t.fs(14), .color = t.text}, tw);
        std::string about = person.signature;
        if (person.fans >= 0) {
            about = about.empty() ? lyren::trf("{} fans", short_number(person.fans)) : lyren::trf("{} fans", short_number(person.fans)) + " · " + about;
        }
        p.text(tx, r.y + 52, about, {.size = t.fs(12), .color = t.text_dim}, tw);
        if (followable) {
            follow_button(ui, ui.id("follow"), {r.right() - bw - 14, r.y + (r.h - 34) / 2, bw, 34}, source, person);
        }
        auto it = ui.interact(id, r);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (it.clicked) {
            open_profile(ui, source, person.id, person.name, person.avatar, face);
        }
        ui.pop_id();
    }

    void Shell::ranking_view(ui::Ui &ui, Rect r, ProfilePage &page) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        RankingList &list = page.rankings[static_cast<size_t>(page.ranking_tab)];
        auto &tracks = list.ranking.tracks;
        if (tracks.empty()) {
            if (!list.loaded && !list.loading) {
                load_ranking(route_.profile, route_.card, page.ranking_tab);
            }
            if (list.loaded && !list.error.empty()) {
                Rect c = ui::center(r, std::min(r.w, 360.f), 120);
                p.icon("lock", {c.x + (c.w - 28) / 2, c.y + 10, 28, 28}, t.text_dim);
                ui.label({c.x, c.y + 54, c.w, 22}, lyren::tr("Their ranking is private"), {.size = t.fs(15), .color = t.text}, Align::center);
                ui.label({c.x, c.y + 80, c.w, 20}, lyren::tr("Only they can see what they play most"), {.size = t.fs(12), .color = t.text_dim}, Align::center);
                return;
            }
            page_status(ui, r.inset(12, 0), !list.loaded, list.loaded ? std::string(lyren::tr("Nothing played yet")) : "", false, Placeholder::rows);
            return;
        }
        constexpr float row_h = 58;
        const ui::Id id = ui.id("profile-ranking");
        const float off = ui.begin_scroll(id, r, static_cast<float>(tracks.size()) * row_h + 12);
        const int first = static_cast<int>(off / row_h), last = std::min(static_cast<int>((off + r.h) / row_h) + 1, static_cast<int>(tracks.size()) - 1);
        const double since = std::max(page.view_time, list.since);
        const int top = list.ranking.plays.empty() ? 0 : *std::ranges::max_element(list.ranking.plays);
        ui.push_id(ui::sub_id(id, static_cast<uint64_t>(page.ranking_tab)));
        for (int i = first; i <= last; ++i) {
            const auto k = static_cast<size_t>(i);
            const Track &tr = tracks[k];
            Rect row{r.x + 12, r.y - off + static_cast<float>(i) * row_h, r.w - 24, row_h - 4};
            const float a = ui.progress(since + 0.02 * std::max(0, i - first), 380);
            Fade fade(p, a);
            row.y += (1 - a) * 12;
            ui.push_id(item_id(ui, tr));
            const ui::Id rid = ui.id("row");
            const bool hover = ui.hovering(row) || ui.is_active(rid);
            const float hot = ui.anim(rid, hover ? 1 : 0, 140);
            if (hot > 0) {
                p.rect(row, t.text.with_alpha(0.05f * hot), t.radius_s());
            }
            Rect rank{row.x, row.y, 44, row.h};
            if (hover) {
                if (ui.icon_button(ui.id("play"), ui::center(rank, 30, 30), is_current(tr) && playing() ? "pause-fill" : "play-fill", 14)) {
                    if (is_current(tr)) {
                        toggle_play();
                    } else {
                        play(tracks, i);
                    }
                }
            } else if (is_current(tr)) {
                ui.equalizer(ui.id("eq"), ui::center(rank, 14, 14), t.accent, playing());
            } else {
                ui.label(rank, std::to_string(i + 1), {.size = t.fs(i < 3 ? 18 : 14), .color = i < 3 ? t.accent : t.text_dim, .features = text::tabular_nums}, Align::center);
            }
            Rect art{rank.right() + 4, row.y + (row.h - 40) / 2, 40, 40};
            cover(ui, art, tr.cover, t.radius_s());
            // How much it is played, against the most played.
            const float bar_w = std::min(180.f, std::round(row.w * 0.24f));
            const float tx = art.right() + 14, tw = row.right() - tx - bar_w - 28;
            p.text(tx, row.y + 24, tr.title, {.size = t.fs(14), .color = is_current(tr) ? t.accent : t.text}, tw);
            p.text(tx, row.y + 42, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
            Rect bar{row.right() - bar_w - 8, row.y + row.h / 2 + 6, bar_w, 4};
            const int plays = k < list.ranking.plays.size() ? list.ranking.plays[k] : 0;
            const float share = top > 0 && plays > 0 ? static_cast<float>(plays) / static_cast<float>(top) : static_cast<float>(list.ranking.scores[k]) / 100;
            p.rect(bar, t.text.with_alpha(0.07f), 2);
            p.rect({bar.x, bar.y, bar.w * share, bar.h}, t.accent.with_alpha(0.75f), 2);
            if (plays > 0) {
                ui.label({bar.x, row.y + 6, bar.w, 18}, lyren::trf("{} plays", grouped(plays)), {.size = t.fs(11), .color = t.text_dim, .features = text::tabular_nums},
                         Align::right);
            }
            auto it = ui.interact(rid, row);
            if (it.double_clicked) {
                play(tracks, i);
            }
            if (it.right_clicked) {
                track_menu(ui, it.px, it.py, tracks, i);
            }
            ui.pop_id();
        }
        ui.pop_id();
        ui.end_scroll();
    }

    void Shell::playlists_view(ui::Ui &ui, Rect r, ProfilePage &page, bool self) {
        const auto &t = ui.theme();
        const Card &card = route_.card;
        std::vector<Card> made, saved;
        double since = page.view_time;
        if (self) {
            const Library &lib = library(card.source);
            if (!lib.liked.id.empty()) {
                made.push_back(lib.liked);
            }
            for (const auto &c : lib.playlists) {
                (c.owned ? made : saved).push_back(c);
            }
        } else {
            if (page.playlists.cards.empty()) {
                std::string error = page.playlists.error;
                if (error.empty() && page.playlists.loaded) {
                    error = lyren::tr("No playlists yet");
                }
                page_status(ui, r.inset(12, 0), !page.playlists.loaded, error, false, Placeholder::cards);
                return;
            }
            const auto split = static_cast<size_t>(std::clamp(page.created, 0, static_cast<int>(page.playlists.cards.size())));
            made.assign(page.playlists.cards.begin(), page.playlists.cards.begin() + static_cast<std::ptrdiff_t>(split));
            saved.assign(page.playlists.cards.begin() + static_cast<std::ptrdiff_t>(split), page.playlists.cards.end());
            since = std::max(since, page.playlists.since);
        }
        Rect inner = r.inset(14, 0);
        constexpr float heading = 40, gap = 26;
        const float content = (made.empty() ? 0 : heading + grid_height(inner.w, made.size()) + gap) + (saved.empty() ? 0 : heading + grid_height(inner.w, saved.size()) + gap) + 8;
        const ui::Id id = ui.id("profile-playlists");
        const float off = ui.begin_scroll(id, r, content);
        float y = r.y - off;
        std::optional<Card> picked;
        auto section = [&](std::string_view title, const std::vector<Card> &cards, std::string_view key) {
            if (cards.empty()) {
                return;
            }
            const TextStyle st{.size = t.fs(15), .color = t.text};
            const float w = ui.painter().measure(title, st);
            ui.label({inner.x, y + 4, w + 1, 24}, title, st);
            ui.label({inner.x + w + 8, y + 6, 80, 24}, std::to_string(cards.size()), {.size = t.fs(12), .color = t.text_dim});
            y += heading;
            if (int i = card_grid(ui, {inner.x, y, inner.w, 0}, ui.id(key), cards, false, since); i >= 0) {
                picked = cards[static_cast<size_t>(i)];
            }
            y += grid_height(inner.w, cards.size()) + gap;
        };
        section(lyren::tr("Created playlists"), made, "made");
        section(lyren::tr("Saved playlists"), saved, "saved");
        ui.end_scroll();
        if (picked) {
            open(ui, *picked);
        }
    }

    void Shell::profile_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ProfilePage &page = *route_.profile;
        const Card &card = route_.card;
        const bool self = own_profile(card);
        if (card.owned && !self) {
            p.rect(r, t.surface, t.radius_l());
            page_status(ui, r, true, "", false);
            return;
        }
        Profile who = page.info ? *page.info : Profile{.id = card.id};
        if (who.name.empty()) {
            who.name = card.title;
        }
        if (who.avatar.empty()) {
            who.avatar = card.cover;
        }
        // Records turn, and the lights drift, only while music plays.
        if (playing() && t.motion > 0) {
            record_turn_ += std::min(ui.now - record_last_, 0.1) * 2 * std::numbers::pi / 9;
            ui.wake(ui.now + 1.0 / 30);
        }
        record_last_ = ui.now;
        const float in = ui.progress(page.since, 900, ease::emphasized);
        profile_backdrop(ui, r, who, in);

        const auto views = profile_tiles(card);
        const bool single = views.size() == 1;
        const bool open = !single && page.view != ProfileView::board;
        // Each page opens and closes its own views; another listener's page starts with its board.
        const ui::Id open_id = ui::sub_id(ui.id("profile-open"), reinterpret_cast<uintptr_t>(route_.profile.get()));
        const float k = single ? 0.f : std::clamp(ui.spring(open_id, open ? 1 : 0, 560, 0.06f), 0.f, 1.06f);
        ui.push_clip(r);
        if (r.w >= 860) {
            // Wide: the record and the head on the left, the board on the right. A view takes the room the board and
            // the head leave while the record rolls to the edge.
            Rect body = r.inset(40, 30);
            const float left_w = std::clamp(std::round(body.w * 0.36f), 300.f, 420.f);
            Rect left{body.x, body.y, left_w, body.h}, right{left.right() + 40, body.y, body.right() - left.right() - 40, body.h};
            const float head_h = 200;
            const float d = std::floor(std::clamp(std::min(left.w - 30, left.h - head_h - 20), 150.f, 340.f));
            Rect at_rest{left.x + (left.w - d) / 2, left.y + std::max(0.f, (left.h - d - head_h - 20) / 2), d, d};
            Rect aside{r.x - d * 0.64f, r.y + (r.h - d) / 2, d, d};
            const float leave = smooth01(k * 1.5f);
            if (leave < 1) {
                Fade fade(p, (1 - leave) * smooth01((in - 0.25f) / 0.75f));
                ui.disabled = open;
                profile_head(ui, {left.x - 20, at_rest.bottom() + 22 + 30 * leave, left.w + 40, head_h}, page, who, self, true);
                if (single) {
                    Rect panel = right;
                    p.shadow({panel.x, panel.y + 8, panel.w, panel.h}, t.radius_l(), 28, t.shadow.with_alpha(0.4f));
                    p.rect(panel, t.surface.with_alpha(0.9f), t.radius_l(), t.text.with_alpha(0.07f), 1);
                    profile_view(ui, panel, page, views.front(), who, self);
                } else {
                    profile_board(ui, right, page, who, self, true);
                }
                ui.disabled = false;
            }
            if (k > 0.002f) {
                Rect room{aside.right() + 28, r.y + 18, r.right() - aside.right() - 46, r.h - 36};
                Rect panel = mix(page.tiles[static_cast<size_t>(page.shown)], room, std::min(k, 1.f));
                p.shadow({panel.x, panel.y + 10, panel.w, panel.h}, t.radius_l(), 32, t.shadow.with_alpha(0.5f));
                p.rect(panel, t.surface.with_alpha(0.94f), t.radius_l(), t.text.with_alpha(0.07f), 1);
                ui.push_clip(panel);
                Fade content(p, smooth01((k - 0.35f) / 0.6f));
                ui.disabled = !open;
                profile_view(ui, {panel.x, panel.y, room.w, panel.h}, page, page.shown, who, self);
                ui.disabled = false;
                ui.pop_clip();
            }
            profile_record(ui, mix(at_rest, aside, std::min(k, 1.f)), page, who, in, std::min(k, 1.f));
        } else {
            // Narrow: one column, the record beside the head and the board under them; a view covers the page.
            Rect body = r.inset(24, 0);
            const float d = std::floor(std::clamp(body.w * 0.36f, 136.f, 220.f));
            {
                Fade fade(p, 1 - smooth01(k * 1.5f));
                ui.disabled = open;
                const float off = ui.begin_scroll(ui.id("profile-column"), r, page.narrow_h);
                float y = r.y + 24 - off;
                profile_record(ui, {body.x, y, d, d}, page, who, in, 0);
                profile_head(ui, {body.x + d + 24, y + std::max(0.f, (d - 190) / 2), body.w - d - 24, 190}, page, who, self, false);
                y += std::max(d, 190.f) + 28;
                const float board_h = single ? 420 : profile_board(ui, {body.x, y, body.w, 0}, page, who, self, false);
                if (single) {
                    profile_view(ui, {body.x - 8, y, body.w + 16, board_h}, page, views.front(), who, self);
                }
                page.narrow_h = y + off - r.y + board_h + 24;
                ui.end_scroll();
                ui.disabled = false;
            }
            if (k > 0.002f) {
                Rect room = r.inset(10);
                Rect panel = mix(page.tiles[static_cast<size_t>(page.shown)], room, std::min(k, 1.f));
                p.shadow({panel.x, panel.y + 10, panel.w, panel.h}, t.radius_l(), 32, t.shadow.with_alpha(0.5f));
                p.rect(panel, t.surface.with_alpha(0.97f), t.radius_l(), t.text.with_alpha(0.07f), 1);
                ui.push_clip(panel);
                Fade content(p, smooth01((k - 0.35f) / 0.6f));
                ui.disabled = !open;
                profile_view(ui, {panel.x, panel.y, room.w, panel.h}, page, page.shown, who, self);
                ui.disabled = false;
                ui.pop_clip();
            }
        }
        ui.pop_clip();
    }

} // namespace lyren
