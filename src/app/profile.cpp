#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// Listener profiles: identity and social links, a readable timeline, and the music that brings people together.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;

    namespace {
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

    std::vector<Shell::ProfileView> Shell::profile_views(const Card &card) const {
        const auto &caps = provider(card).capabilities;
        std::vector<ProfileView> views;
        if (caps.posts) {
            views.push_back(ProfileView::posts);
        }
        if (caps.ranking) {
            views.push_back(ProfileView::ranking);
        }
        if (own_profile(card) ? caps.library : caps.profile_playlists) {
            views.push_back(ProfileView::playlists);
        }
        if (caps.people) {
            views.push_back(ProfileView::follows);
            views.push_back(ProfileView::fans);
        }
        return views;
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
        page.view = view;
        page.view_time = ui.now;
        const auto id = ui::sub_id(ui.id("profile-scroll"), reinterpret_cast<uintptr_t>(&page));
        ui.scroll_to(id, std::min(ui.scroll_offset(id), page.header_h));
        const Card &card = route_.card;
        if (view == ProfileView::follows || view == ProfileView::fans) {
            load_people(route_.profile, card, view == ProfileView::fans);
        } else if (view == ProfileView::ranking) {
            load_ranking(route_.profile, card, page.ranking_tab);
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
        const auto views = profile_views(card);
        if (!views.empty()) {
            page->view = views.front();
        }
        if (caps.posts) {
            load_posts(page->posts, card.source, card.id);
        }
        if (caps.people) {
            load_people(page, card, 0);
            load_people(page, card, 1);
        }
        if (caps.ranking) {
            load_ranking(page, card, 0);
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
        if (tab == 0 && list.error.empty() && list.ranking.tracks.empty()) {
            page->ranking_tab = 1;
            load_ranking(page, card, 1);
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

    void Shell::follow_button(ui::Ui &ui, ui::Id id, Rect r, Source source, const Person &person, bool compact) {
        const bool on = following(source, person), mutual = on && person.follows_me;
        const auto label = compact ? std::string_view{} : lyren::tr(!on ? "Follow" : mutual ? "Mutual" : "Following");
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

    void Shell::profile_head(ui::Ui &ui, Rect r, ProfilePage &page, const Profile &who, bool self) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = route_.card;
        const auto &caps = provider(card).capabilities;
        const bool compact = r.w < 620, narrow = r.w < 560;
        const float avatar = compact ? 76.f : 104.f;
        const std::string &art = who.background.empty() ? who.avatar : who.background;
        if (!art.empty()) {
            Rect banner{r.x - 12, r.y, r.w + 24, 158};
            const auto &image = images_.get(art, 128, true);
            if (image.ready) {
                Fade wash(p, ui.progress(image.ready_at, 400) * (t.dark ? 0.18f : 0.1f));
                p.image(banner, image.tex, t.radius_l(), {1, 1, 1, 1}, cover_uv(1, banner.w / banner.h));
            }
            p.gradient(banner, t.surface.with_alpha(0.1f), t.surface, t.radius_l());
        }
        Rect face{r.x, r.y + (compact ? 18.f : 24.f), avatar, avatar};
        const float arrived = ui.progress(page.since, 320, ease::standard);
        Rect portrait = page.origin.w > 0 ? mix(page.origin, face, arrived) : face;
        p.rect(portrait.inset(-3), t.bg, portrait.w / 2 + 3);
        cover(ui, portrait, who.avatar, portrait.w / 2, "user");

        const float tx = face.right() + (compact ? 20.f : 24.f);
        const float action_y = r.y + (compact ? 18.f : 26.f);
        float right = r.right() - 36;
        if (ui.icon_button(ui.id("profile-more"), {right, action_y, 36, 36}, "ellipsis", 19)) {
            std::vector<ui::MenuItem> items;
            if (self) {
                items.push_back({"user", std::string(lyren::tr("Accounts")), [this, &ui] {
                                     open_account(ui);
                                 }});
            }
            if (auto link = share_url(card); !link.empty()) {
                items.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                     ui.copy = link;
                                     ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                 }});
            }
            items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this] {
                                 route_.profile = std::make_shared<ProfilePage>();
                                 spawn(load_profile(route_.profile, route_.card));
                             }});
            ui.open_menu(right, action_y + 42, std::move(items));
        }
        if (self && caps.post_write) {
            const auto label = narrow ? std::string_view{} : lyren::tr("New post");
            const float w = narrow ? 38.f : button_width(ui, label, true);
            right -= w + 8;
            if (ui.button(ui.id("profile-write"), {right, action_y, w, 36}, label, ui::ButtonKind::primary, "square-pen")) {
                open_composer(ui, card.source);
            }
        } else if (!self && caps.follow_users) {
            const float w = narrow ? 38.f : follow_button_width(ui);
            right -= w + 8;
            follow_button(ui, ui.id("profile-follow"), {right, action_y, w, 36}, card.source,
                          {.id = who.id, .name = who.name, .avatar = who.avatar, .followed = who.followed, .follows_me = who.follows_me}, narrow);
        }
        const TextStyle name_style{.size = t.fs(compact ? 24 : 30), .color = t.text, .features = text::semibold};
        const bool badges = right - tx > 270;
        const float badge_w = badges ? (who.level >= 0 ? 48.f : 0) + (who.vip ? 40.f : 0) : 0;
        const float name_w = std::min(p.measure(who.name, name_style) + 1, right - tx - 12 - badge_w);
        ui.label({tx, r.y + (compact ? 14.f : 18.f), name_w, 42}, who.name, name_style);
        if (badges) {
            float x = tx + name_w + 10;
            const std::array<std::string, 2> labels{who.level >= 0 ? std::format("Lv.{}", who.level) : "", who.vip ? "VIP" : ""};
            for (const auto &badge : labels) {
                if (badge.empty()) {
                    continue;
                }
                const TextStyle style{.size = t.fs(11), .color = t.accent, .features = text::medium};
                const float w = p.measure(badge, style) + 12;
                Rect pill{x, r.y + (compact ? 26.f : 30.f), w, 20};
                p.rect(pill, t.accent.with_alpha(0.12f), 10);
                ui.label(pill, badge, style, Align::center);
                x += w + 6;
            }
        }
        const auto signature = !page.info && !page.error.empty() ? lyren::tr(page.error) : std::string_view(who.signature);
        ui.label({tx, r.y + (compact ? 55.f : 66.f), r.right() - tx, 22}, signature, {.size = t.fs(13), .color = t.text_dim});

        if (!compact) {
            const TextStyle meta{.size = t.fs(12), .color = t.text_dim};
            std::vector<std::pair<std::string_view, std::string>> facts;
            if (!badges && who.level >= 0) {
                facts.emplace_back("audio-lines", std::format("Lv.{}", who.level));
            }
            if (!who.location.empty()) {
                facts.emplace_back("map-pin", who.location);
            }
            if (who.days >= 0) {
                facts.emplace_back("history", lyren::trf("Here for {}", age(who.days)));
            }
            if (!self && who.follows_me) {
                facts.emplace_back("user-check", std::string(lyren::tr("Follows you")));
            }
            float x = tx;
            for (const auto &[icon, label] : facts) {
                const float w = p.measure(label, meta) + 20;
                if (x + w > r.right()) {
                    break;
                }
                p.icon(icon, {x, r.y + 97, 13, 13}, t.text_dim);
                ui.label({x + 19, r.y + 93, w - 19, 21}, label, meta);
                x += w + 20;
            }
        }

        struct Metric {
            ProfileView view;
            std::string_view label;
            int value;
        };
        std::vector<Metric> metrics;
        if (caps.people) {
            metrics.push_back({ProfileView::follows, lyren::tr("Following people"), people_count(page, card.source, who, false)});
            metrics.push_back({ProfileView::fans, lyren::tr("Followers"), people_count(page, card.source, who, true)});
        }
        if (caps.posts) {
            metrics.push_back({ProfileView::posts, lyren::tr("Posts"), who.posts});
        }
        float x = narrow ? r.x : tx;
        const float y = r.y + (compact ? 124.f : 136.f);
        const float width = metrics.empty() ? 0 : std::min(120.f, (r.right() - x) / static_cast<float>(metrics.size()));
        for (const auto &metric : metrics) {
            const ui::Id id = ui::sub_id(ui.id("profile-stat"), std::to_underlying(metric.view));
            Rect hit{x - 8, y, width - 8, 48};
            auto it = ui.interact(id, hit);
            const float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
            p.rect(hit, t.hover.with_alpha(hot), t.radius_s());
            ui.label({x, y, width - 20, 26}, metric.value >= 0 ? grouped(metric.value) : "—", {.size = t.fs(20), .color = t.text, .features = text::medium | text::tabular_nums});
            ui.label({x, y + 27, width - 20, 18}, metric.label, {.size = t.fs(12), .color = t.text_dim.mix(t.text, hot)});
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.clicked) {
                if (metric.view == ProfileView::posts) {
                    page.posts_tab = 0;
                }
                open_profile_view(ui, page, metric.view);
            }
            x += width;
        }
    }

    void Shell::profile_rail(ui::Ui &ui, Rect r, ProfilePage &page, const Profile &who) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &caps = provider(route_.card).capabilities;
        ui.push_clip(r);
        float y = r.y + 8;
        if (caps.ranking) {
            ui.label({r.x, y, r.w - 30, 24}, lyren::tr("Most played"), {.size = t.fs(15), .color = t.text, .features = text::medium});
            if (ui.icon_button(ui.id("rail-ranking"), {r.right() - 28, y - 3, 28, 28}, "chevron-right", 17)) {
                open_profile_view(ui, page, ProfileView::ranking);
            }
            ui.label({r.x, y + 28, r.w, 18}, lyren::tr(page.ranking_tab ? "All time" : "This week"), {.size = t.fs(12), .color = t.text_dim});
            y += 58;
            const RankingList &list = page.rankings[static_cast<size_t>(page.ranking_tab)];
            const auto &tracks = list.ranking.tracks;
            if (list.loading) {
                for (int i = 0; i < 3; ++i) {
                    ui.skeleton({r.x, y, 38, 38}, t.radius_s());
                    ui.skeleton({r.x + 50, y + 6, r.w - 64, 9}, 4);
                    ui.skeleton({r.x + 50, y + 23, (r.w - 64) * 0.6f, 8}, 4);
                    y += 56;
                }
            } else if (tracks.empty()) {
                ui.label({r.x, y, r.w, 40}, lyren::tr(list.error.empty() ? "No listening history yet" : list.error), {.size = t.fs(12), .color = t.text_dim});
                y += 56;
            } else {
                const size_t visible = static_cast<size_t>(std::clamp(static_cast<int>((r.bottom() - y) / 56), 1, 5));
                for (size_t i = 0; i < std::min(tracks.size(), visible); ++i) {
                    const auto &track = tracks[i];
                    const ui::Id id = ui::sub_id(ui.id("rail-track"), i);
                    Rect row{r.x, y, r.w, 48}, art{r.x, y + 4, 38, 38};
                    auto it = ui.interact(id, row);
                    const float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
                    p.rect(row.inset(-5, 0), t.hover.with_alpha(hot), t.radius_s());
                    cover(ui, art, track.cover, t.radius_s());
                    const bool on = is_current(track);
                    if (on || hot > 0) {
                        p.rect(art, ui::Color{0, 0, 0, on ? 0.4f : 0.4f * hot}, t.radius_s());
                        p.icon(on && playing() ? "pause-fill" : "play-fill", ui::center(art, 13, 13), ui::Color{1, 1, 1, on ? 1.f : hot});
                    }
                    ui.label({art.right() + 12, y + 2, r.w - 50, 24}, track.title, {.size = t.fs(13), .color = on ? t.accent : t.text, .features = text::medium});
                    ui.label({art.right() + 12, y + 26, r.w - 50, 18}, track.artist, {.size = t.fs(12), .color = t.text_dim});
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    if (it.clicked) {
                        on ? toggle_play() : play(tracks, static_cast<int>(i));
                    }
                    const float share = static_cast<float>(list.ranking.scores[i]) / 100;
                    p.rect({art.right() + 12, y + 47, (r.w - 50) * share, 1.5f}, t.accent.with_alpha(0.42f), 0.75f);
                    y += 56;
                }
            }
        }
        if (who.listened >= 0 && y + 88 < r.bottom()) {
            p.rect({r.x, y + 8, r.w, 1}, t.border.with_alpha(0.6f));
            p.icon("headphones", {r.x, y + 31, 18, 18}, t.text_dim);
            ui.label({r.x + 30, y + 24, r.w - 30, 28}, grouped(who.listened), {.size = t.fs(20), .color = t.text, .features = text::medium});
            ui.label({r.x + 30, y + 55, r.w - 30, 20}, lyren::tr("Songs played"), {.size = t.fs(12), .color = t.text_dim});
            y += 96;
        }
        const auto &people = page.people[0].people;
        if (!people.empty() && y + 72 < r.bottom()) {
            ui.label({r.x, y + 8, r.w - 30, 22}, lyren::tr("Following people"), {.size = t.fs(13), .color = t.text_dim});
            if (ui.icon_button(ui.id("rail-people"), {r.right() - 28, y + 4, 28, 28}, "chevron-right", 16)) {
                open_profile_view(ui, page, ProfileView::follows);
            }
            for (size_t i = 0; i < std::min<size_t>(people.size(), 5); ++i) {
                Rect face{r.x + static_cast<float>(i) * 40, y + 38, 30, 30};
                cover(ui, face, people[i].avatar, 15, "user");
                const auto it = ui.interact(ui::sub_id(ui.id("rail-person"), i), face);
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    open_profile(ui, route_.card.source, people[i].id, people[i].name, people[i].avatar, face);
                }
            }
        }
        ui.pop_clip();
    }

    float Shell::profile_view(ui::Ui &ui, Rect r, ProfilePage &page, ProfileView view, const Profile &who, bool self) {
        const auto &t = ui.theme();
        const Card card = route_.card;
        Rect head{r.x, r.y, r.w, 42};
        Rect body{r.x, head.bottom() + 10, r.w, r.h - 52};
        if (view == ProfileView::posts) {
            if (self) {
                const std::array<std::string_view, 2> labels{lyren::tr("My posts"), lyren::tr("Following feed")};
                if (ui.segmented(ui.id("profile-feed-tabs"), {head.x, head.y + 3, std::min(280.f, head.w - 48), 34}, labels, page.posts_tab)) {
                    open_profile_view(ui, page, ProfileView::posts);
                }
            } else {
                ui.label({head.x, head.y, head.w - 44, head.h}, who.posts >= 0 ? lyren::trf("{} posts", grouped(who.posts)) : std::string(lyren::tr("Posts")),
                         {.size = t.fs(13), .color = t.text_dim});
            }
            const bool feed = self && page.posts_tab == 1;
            auto &list = feed ? library(card.source).feed : page.posts;
            if (!list) {
                list = std::make_shared<PostList>();
            }
            ui.disabled = list->loading;
            if (ui.icon_button(ui.id("view-refresh"), {head.right() - 32, head.y + 5, 32, 32}, "refresh-cw", 17)) {
                list = std::make_shared<PostList>();
                open_profile_view(ui, page, ProfileView::posts);
            }
            ui.disabled = false;
            return 52 + post_list(ui, body, list, card.source, feed ? "" : card.id, feed, std::max(page.view_time, list->since));
        }
        if (view == ProfileView::follows || view == ProfileView::fans) {
            const bool fans = view == ProfileView::fans;
            const int count = people_count(page, card.source, who, fans);
            const auto label =
                count < 0 ? std::string(lyren::tr(fans ? "Followers" : "Following people")) : lyren::trf(fans ? "{} followers" : "Following {} people", grouped(count));
            ui.label(head, label, {.size = t.fs(13), .color = t.text_dim});
            return 52 + people_view(ui, body, page, self);
        }
        if (view == ProfileView::ranking) {
            const auto label = head.w < 360 ? std::string_view{} : lyren::tr("Play all");
            const float w = label.empty() ? 38.f : button_width(ui, label, true);
            auto &tracks = page.rankings[static_cast<size_t>(page.ranking_tab)].ranking.tracks;
            ui.disabled = tracks.empty();
            if (ui.button(ui.id("ranking-play"), {head.right() - w, head.y + 2, w, 36}, label, ui::ButtonKind::primary, "play-fill")) {
                play(tracks, 0);
            }
            ui.disabled = false;
            const std::array<std::string_view, 2> labels{lyren::tr("This week"), lyren::tr("All time")};
            if (ui.segmented(ui.id("ranking-tabs"), {head.x, head.y + 3, std::min(230.f, head.w - w - 16), 34}, labels, page.ranking_tab)) {
                open_profile_view(ui, page, ProfileView::ranking);
            }
            return 52 + ranking_view(ui, body, page);
        }
        return playlists_view(ui, r, page, self);
    }

    float Shell::people_view(ui::Ui &ui, Rect r, ProfilePage &page, bool self) {
        const auto &t = ui.theme();
        const Card card = route_.card;
        const auto owner = route_.profile;
        const bool fans = page.view == ProfileView::fans;
        PeopleList &list = page.people[fans];
        const auto tab_since = std::max(page.view_time, list.since);
        if (list.people.empty()) {
            if (list.more && !list.loading && list.error.empty()) {
                load_people(owner, card, fans);
            }
            if (!list.error.empty() && !provider(card).logged_in()) {
                page_status(ui, r.inset(12, 0), false, "", true);
                return 200;
            }
            std::string error = list.error;
            if (error.empty() && !list.loading && !list.more) {
                error = lyren::tr(fans ? (self ? "No fans yet" : "No fans to show") : (self ? "You are not following anyone yet" : "Not following anyone yet"));
            }
            page_status(ui, r.inset(12, 0), list.loading || (list.more && error.empty()), error, false, Placeholder::faces);
            return 200;
        }
        constexpr float gap = 12, row_h = 76;
        Rect inner = r.inset(12, 0);
        const int cols = std::max(1, static_cast<int>((inner.w + gap) / (360 + gap)));
        const float cw = (inner.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const size_t rows = (list.people.size() + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
        const float content = static_cast<float>(rows) * (row_h + gap) + (list.loading || list.more ? 56 : 12);
        const Rect clip = ui.clip();
        int shown = 0;
        for (size_t i = 0; i < list.people.size(); ++i) {
            Rect cell{inner.x + static_cast<float>(i % static_cast<size_t>(cols)) * (cw + gap), r.y + static_cast<float>(i / static_cast<size_t>(cols)) * (row_h + gap), cw, row_h};
            if (cell.bottom() < clip.y || cell.y > clip.bottom()) {
                continue;
            }
            const float a = ui.progress(tab_since + 0.025 * std::min(shown++, 16), 420);
            Fade fade(ui.painter(), a);
            cell.y += (1 - a) * 12;
            person_card(ui, cell, card.source, list.people[i]);
        }
        const float end = r.y + static_cast<float>(rows) * (row_h + gap);
        if (list.loading) {
            ui.spinner(ui::center({r.x, end, r.w, 48}, 20, 20), t.text_dim, 2);
        } else if (!list.error.empty()) {
            ui.label({r.x, end + 8, r.w, 24}, lyren::tr(list.error), {.size = t.fs(12), .color = t.text_dim}, Align::center);
        } else if (list.more && end < r.bottom() + 400) {
            load_people(owner, card, fans);
        }
        return content;
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

    float Shell::ranking_view(ui::Ui &ui, Rect r, ProfilePage &page) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        RankingList &list = page.rankings[static_cast<size_t>(page.ranking_tab)];
        auto &tracks = list.ranking.tracks;
        if (tracks.empty()) {
            if (!list.loaded && !list.loading) {
                load_ranking(route_.profile, route_.card, page.ranking_tab);
            }
            if (list.error == "This listening ranking is private") {
                Rect c = ui::center(r, std::min(r.w, 360.f), 120);
                p.icon("lock", {c.x + (c.w - 28) / 2, c.y + 10, 28, 28}, t.text_dim);
                ui.label({c.x, c.y + 54, c.w, 22}, lyren::tr("Their ranking is private"), {.size = t.fs(15), .color = t.text}, Align::center);
                ui.label({c.x, c.y + 80, c.w, 20}, lyren::tr("Only they can see what they play most"), {.size = t.fs(12), .color = t.text_dim}, Align::center);
                return 200;
            }
            page_status(ui, r.inset(12, 0), !list.loaded, list.error.empty() ? (list.loaded ? std::string(lyren::tr("Nothing played yet")) : "") : list.error, false,
                        Placeholder::rows);
            return 200;
        }
        constexpr float row_h = 58;
        const ui::Id id = ui.id("profile-ranking");
        const Rect clip = ui.clip();
        const int first = std::max(0, static_cast<int>((clip.y - r.y) / row_h));
        const int last = std::min(static_cast<int>((clip.bottom() - r.y) / row_h) + 1, static_cast<int>(tracks.size()) - 1);
        const double since = std::max(page.view_time, list.since);
        const int top = list.ranking.plays.empty() ? 0 : *std::ranges::max_element(list.ranking.plays);
        ui.push_id(ui::sub_id(id, static_cast<uint64_t>(page.ranking_tab)));
        for (int i = first; i <= last; ++i) {
            const auto k = static_cast<size_t>(i);
            const Track &tr = tracks[k];
            Rect row{r.x + 12, r.y + static_cast<float>(i) * row_h, r.w - 24, row_h - 4};
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
        return static_cast<float>(tracks.size()) * row_h + 12;
    }

    float Shell::playlists_view(ui::Ui &ui, Rect r, ProfilePage &page, bool self) {
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
                return 200;
            }
            const auto split = static_cast<size_t>(std::clamp(page.created, 0, static_cast<int>(page.playlists.cards.size())));
            made.assign(page.playlists.cards.begin(), page.playlists.cards.begin() + static_cast<std::ptrdiff_t>(split));
            saved.assign(page.playlists.cards.begin() + static_cast<std::ptrdiff_t>(split), page.playlists.cards.end());
            since = std::max(since, page.playlists.since);
        }
        Rect inner = r.inset(14, 0);
        constexpr float heading = 40, gap = 26;
        const float content = (made.empty() ? 0 : heading + grid_height(inner.w, made.size()) + gap) + (saved.empty() ? 0 : heading + grid_height(inner.w, saved.size()) + gap) + 8;
        float y = r.y;
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
        if (picked) {
            open(ui, *picked);
        }
        return content;
    }

    void Shell::profile_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto keep = route_.profile;
        ProfilePage &page = *keep;
        const Card card = route_.card;
        const bool self = own_profile(card);
        if (card.owned && !self) {
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
        const float pad = r.w >= 760 ? 32.f : r.w < 440 ? 16.f : 24.f;
        Rect body = r.inset(pad, 0);
        if (body.w > 1120) {
            body.x += (body.w - 1120) / 2;
            body.w = 1120;
        }
        page.header_h = body.w < 620 ? 188.f : 204.f;
        const auto scroll = ui::sub_id(ui.id("profile-scroll"), reinterpret_cast<uintptr_t>(&page));
        const float off = ui.begin_scroll(scroll, r, std::max(page.content_h, r.h));
        profile_head(ui, {body.x, r.y - off, body.w, page.header_h}, page, who, self);
        const auto views = profile_views(card);
        if (views.empty()) {
            page_status(ui, {body.x, r.y + page.header_h, body.w, r.h - page.header_h}, page.loading,
                        page.error.empty() ? std::string(lyren::tr("No profile content available on this service")) : page.error, false);
            ui.end_scroll();
            return;
        }
        std::vector<std::string_view> labels, icons;
        for (const auto view : views) {
            switch (view) {
                case ProfileView::posts:
                    labels.push_back(lyren::tr("Posts"));
                    icons.push_back("message-square");
                    break;
                case ProfileView::ranking:
                    labels.push_back(lyren::tr("Most played"));
                    icons.push_back("headphones");
                    break;
                case ProfileView::playlists:
                    labels.push_back(lyren::tr("Playlists"));
                    icons.push_back("list-music");
                    break;
                case ProfileView::follows:
                    labels.push_back(lyren::tr("Following people"));
                    icons.push_back("users");
                    break;
                case ProfileView::fans:
                    labels.push_back(lyren::tr("Followers"));
                    icons.push_back("heart");
                    break;
            }
        }
        int selected = static_cast<int>(std::ranges::find(views, page.view) - views.begin());
        Rect tabs{body.x, std::max(r.y, r.y + page.header_h - off), body.w, 46};
        const float pinned = smooth01((off - page.header_h + 30) / 30);
        const size_t tab_begin = p.mark();
        p.rect(tabs.inset(-8, 0), t.surface);
        if (pinned > 0) {
            p.gradient({tabs.x - 8, tabs.bottom(), tabs.w + 16, 12}, t.shadow.with_alpha(0.18f * pinned), t.shadow.with_alpha(0));
        }
        p.rect({tabs.x, tabs.bottom() - 1, tabs.w, 1}, t.border.with_alpha(0.75f));
        float needed = 28.f * static_cast<float>(labels.size() - 1);
        for (const auto label : labels) {
            needed += p.measure(label, {.size = t.fs(15), .features = text::medium});
        }
        if (needed <= tabs.w) {
            if (ui.tabs(ui.id("profile-tabs"), tabs, labels, selected)) {
                open_profile_view(ui, page, views[static_cast<size_t>(selected)]);
            }
            if (pinned > 0 && tabs.w - needed > 184) {
                Fade identity(p, pinned);
                Rect avatar{tabs.right() - 152, tabs.y + 11, 24, 24};
                cover(ui, avatar, who.avatar, 12, "user");
                ui.label({avatar.right() + 10, tabs.y, 118, tabs.h}, who.name, {.size = t.fs(12), .color = t.text_dim, .features = text::medium});
            }
        } else {
            const float w = tabs.w / static_cast<float>(views.size());
            for (size_t i = 0; i < views.size(); ++i) {
                Rect tab{tabs.x + static_cast<float>(i) * w, tabs.y, w, tabs.h};
                if (ui.icon_button(ui::sub_id(ui.id("profile-tabs"), i), ui::center(tab, 36, 36), icons[i], 18, static_cast<int>(i) == selected)) {
                    open_profile_view(ui, page, views[i]);
                }
                if (static_cast<int>(i) == selected) {
                    p.rect({tab.x + (w - 24) / 2, tab.bottom() - 3, 24, 3}, t.accent, 1.5f);
                }
            }
        }
        const size_t tab_end = p.mark();
        const float top = r.y + page.header_h + tabs.h + 14 - off;
        Rect content{body.x, top, body.w, r.bottom() - top - 16};
        ui.push_clip({body.x - 8, tabs.bottom(), body.w + 16, r.bottom() - tabs.bottom()});
        Rect rail;
        if (page.view == ProfileView::posts && content.w >= 760 && provider(card).capabilities.ranking) {
            rail = ui::cut_right(content, 224);
            rail.y = tabs.bottom() + 14;
            rail.h = r.bottom() - rail.y - 16;
            ui::cut_right(content, 32);
            p.rect({rail.x - 16, rail.y + 8, 1, rail.h - 16}, t.border.with_alpha(0.55f));
        }
        const float height = profile_view(ui, content, page, page.view, who, self);
        if (rail.w > 0) {
            profile_rail(ui, rail, page, who);
        }
        ui.pop_clip();
        p.raise(tab_begin, tab_end);
        const float next = page.header_h + tabs.h + 14 + height + 20;
        if (next != page.content_h) {
            page.content_h = next;
            ui.redraw();
        }
        ui.end_scroll();
    }

} // namespace lyren
