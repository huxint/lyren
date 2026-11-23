#include "app/view/profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <inplace_vector>
#include <optional>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    namespace ease = ui::ease;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    namespace {
        // The page is an editorial column: the timeline and, when there is room for both, a framed studio rail beside it.
        constexpr float rail_gap = 32, rail_min = 276, rail_max = 324, page_w = PostFeed::column + rail_gap + rail_max;
        constexpr float tabs_h = 46, section_gap = 20;

        // "8 years", or the days of a newer account.
        std::string age(int days) {
            if (days >= 365) {
                const int years = days / 365;
                return years == 1 ? std::string(tr("1 year")) : trf("{} years", years);
            }
            const int d = std::max(days, 1);
            return d == 1 ? std::string(tr("1 day")) : trf("{} days", d);
        }

        // A count as a stat shows it: whole while it fits, shortened once it is long.
        std::string stat_number(int n) {
            return n >= 1000000 ? short_number(n) : grouped(n);
        }

        Person person_of(const Profile &who) {
            return {.id = who.id, .name = who.name, .avatar = who.avatar, .signature = who.signature, .fans = who.fans, .followed = who.followed, .follows_me = who.follows_me};
        }
    } // namespace

    float ProfileView::follow_width(ui::Ui &ui) {
        float w = 0;
        for (std::string_view label : {"Follow", "Following", "Mutual"}) {
            w = std::max(w, ui.button_width(lyren::tr(label), true));
        }
        return w;
    }

    void ProfileView::follow_button(ui::Ui &ui, ui::Id id, Rect r, Source source, const Person &person, bool prominent) {
        auto &p = ui.painter();
        const bool on = library_.following(source, person), mutual = on && person.follows_me, bare = r.w < 64;
        // One state dissolves into the other; only the one that holds answers.
        const float k = ui.anim(ui::sub_id(id, 0xf0), on ? 1 : 0, pace::steady);
        bool clicked = false;
        auto state = [&](bool following, float alpha) {
            if (alpha <= 0) {
                return;
            }
            Fade fade(p, alpha);
            ui::Enabled live(ui, following == on, 1);
            const auto label = bare ? std::string_view{} : lyren::tr(!following ? "Follow" : mutual ? "Mutual" : "Following");
            const auto kind = following ? ui::ButtonKind::secondary : prominent ? ui::ButtonKind::primary : ui::ButtonKind::tonal;
            clicked |= ui.button(ui::sub_id(id, following), r, label, kind, !following ? "user-plus" : mutual ? "arrow-left-right" : "user-check");
        };
        state(!on, on ? 1 - k : k);
        state(on, on ? k : 1 - k);
        if (!clicked) {
            return;
        }
        if (!on) {
            return library_.toggle_follow_user(source, person);
        }
        // Unfollowing is one step further away than following.
        ui.open_menu(r.x, r.bottom() + 6,
                     {{"user-minus", std::string(lyren::tr("Unfollow")),
                       [this, source, person] {
                           library_.toggle_follow_user(source, person);
                       },
                       true}});
    }

    void ProfileView::show(ui::Ui &ui, const Listener &page, Section section) {
        social_.show(page, section);
        const ui::Id scroll = scroll_id(ui, "profile");
        ui.scroll_to(scroll, std::min(ui.scroll_offset(scroll), header_h_));
    }

    void ProfileView::draw(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // Held through the frame: a click may lead elsewhere while the page is still being drawn.
        const Listener page = browser_.route().profile;
        const Card card = browser_.route().card;
        const bool self = library_.own(card);
        if (!page || (card.owned && !self)) {
            // The account's own page: it waits for the account, or for someone to sign in.
            const bool coming = !card.owned || signed_in();
            page_status(ui, r, coming, "", !coming);
            return;
        }
        if (visit_ != browser_.route().serial) {
            visit_ = browser_.route().serial;
            content_h_ = 0;
        }
        Profile who = page->info ? *page->info : Profile{.id = card.id};
        if (who.name.empty()) {
            who.name = card.title;
        }
        if (who.avatar.empty()) {
            who.avatar = card.cover;
        }
        Rect body = r.inset(r.w >= 760 ? 36.f : r.w < 480 ? 16.f : 24.f, 0);
        if (body.w > page_w) {
            body = ui::center(body, page_w, body.h);
        }

        const float off = ui.begin_scroll(scroll_id(ui, "profile"), r, std::max(content_h_, r.h));
        // Their background, or their picture, washes the top of the page; the fade-to-surface runs at full opacity so
        // it dissolves imperceptibly into the canvas without a hard lower seam.
        {
            const float scroll_fade = 1 - smooth01(off / 180);
            if (scroll_fade > 0.01f) {
                const Rect band{r.x, r.y, r.w, 260};
                const std::string &art = who.background.empty() ? who.avatar : who.background;
                bool drew_art = false;
                if (!art.empty()) {
                    if (const auto &image = env_.images.get(art, 128, true); image.ready) {
                        const float k = ui.progress(image.ready_at, 480, ease::standard);
                        {
                            Fade wash(p, k * (t.dark ? 0.32f : 0.18f) * scroll_fade);
                            p.image(band, image.tex, t.radius_l(), white, cover_uv(1, band.w / band.h));
                        }
                        p.gradient(band, t.surface.with_alpha(0.08f), t.surface, t.radius_l());
                        drew_art = true;
                    }
                }
                if (!drew_art) {
                    p.gradient(band, t.accent.with_alpha((t.dark ? 0.10f : 0.06f) * scroll_fade), t.surface.with_alpha(0), t.radius_l());
                }
            }
        }
        header_h_ = header(ui, {body.x, r.y - off, body.w, 0}, page, who, self);
        if (social_.sections(card).empty()) {
            // A platform that tells nothing more about its listeners.
            const Rect rest{body.x, r.y + header_h_ - off, body.w, std::max(r.h - header_h_, 240.f)};
            if (!page_status(ui, rest, page->loading, page->error, false)) {
                empty(ui, rest, "user", lyren::tr("No profile content available on this service"));
            }
            content_h_ = header_h_ + rest.h;
            return ui.end_scroll();
        }
        // The tabs come to rest at the top of the view, with an opaque surface backing once pinned so the scrolled head
        // never bleeds through behind them.
        const Rect strip{body.x, std::max(r.y, r.y + header_h_ - off), body.w, tabs_h};
        const float pinned = smooth01((off - header_h_ + 24) / 24);
        if (pinned > 0.01f) {
            p.rect({r.x, strip.y, r.w, strip.h}, t.surface.with_alpha(pinned), {t.radius_l(), t.radius_l(), 0, 0});
        }
        const float edge = std::lerp(body.x, r.x, pinned);
        p.rect({edge, strip.bottom() - 1, r.w - 2 * (edge - r.x), 1}, t.line());
        tabs(ui, strip, page, who, pinned);
        ui.push_clip({r.x, strip.bottom(), r.w, r.bottom() - strip.bottom()});
        float height = 0;
        {
            const float in = ui.progress(page->section_since, pace::arrive);
            Fade fade(p, in);
            height = section(ui, {body.x, r.y + header_h_ + tabs_h + section_gap - off + (1 - in) * 10, body.w, 0}, page, self);
        }
        if (pinned > 0) {
            p.gradient({r.x, strip.bottom(), r.w, 10}, t.shadow.with_alpha(0.16f * pinned), t.shadow.with_alpha(0));
        }
        ui.pop_clip();
        if (const float next = header_h_ + tabs_h + section_gap + height + 28; next != content_h_) {
            content_h_ = next;
            ui.redraw();
        }
        ui.end_scroll();
    }

    float ProfileView::header(ui::Ui &ui, Rect r, const Listener &page, const Profile &who, bool self) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &caps = provider(card).capabilities;
        const bool compact = r.w < 600;
        // The head arrives as one: the picture a little small at first, settling to its size.
        const float in = ui.progress(page->since, pace::arrive);
        Fade fade(p, in);
        const float d = compact ? 72.f : 100.f;
        const Rect face{r.x, r.y + 30, d, d};
        {
            const float size = d * (0.94f + 0.06f * in);
            const Rect portrait = ui::center(face, size, size);
            p.shadow({portrait.x, portrait.y + 6, portrait.w, portrait.h}, size / 2, 24, t.shadow.with_alpha(0.55f));
            // A ring of the page's own colour sets the picture off from the wash behind it.
            p.rect(portrait.inset(-4), t.surface, size / 2 + 4, t.surface_edge(0.8f), 1);
            cover(ui, portrait, who.avatar, size / 2, "user");
            p.rect(portrait, {}, size / 2, t.surface_edge(1.0f), 1);
            const auto it = ui.interact(ui.id("profile-face"), face);
            if (it.hovered && !who.avatar.empty()) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.clicked && !who.avatar.empty()) {
                shell_.dialogs.show_picture(ui, who.avatar, 1);
            }
        }

        // What can be done here stands at the top right: following them, or writing a post of one's own.
        const float top = face.y + (compact ? 0.f : 4.f), name_h = compact ? 30.f : 40.f;
        const float button_y = top + (name_h - control::medium) / 2;
        float right = r.right();
        if (const Rect more{right - control::medium, button_y, control::medium, control::medium};
            ui.icon_button(ui.id("profile-more"), more, "ellipsis", 18, false, lyren::tr("More"))) {
            std::vector<ui::MenuItem> items;
            if (self) {
                items.push_back({"user", std::string(lyren::tr("Accounts")), [this, &ui] {
                                     shell_.account.open(ui);
                                 }});
            }
            if (auto link = share_url(card); !link.empty()) {
                items.push_back({"link", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                     ui.copy = link;
                                     ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                 }});
            }
            items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this] {
                                 if (auto &route = browser_.route(); route.page == Page::profile) {
                                     route.profile = social_.open(route.card);
                                 }
                             }});
            ui.open_menu(more.x, more.bottom() + 6, std::move(items));
        }
        right -= control::medium + 8;
        if (self && caps.post_write) {
            const auto label = compact ? std::string_view{} : lyren::tr("New post");
            const float w = compact ? control::medium + 6 : ui.button_width(label, true);
            if (ui.button(ui.id("profile-write"), {right - w, button_y, w, control::medium}, label, ui::ButtonKind::primary, "square-pen")) {
                shell_.feed.compose(ui, card.source);
            }
            right -= w + 8;
        } else if (!self && caps.follow_users) {
            // Until the page says whether they are followed already, the button waits.
            ui::Enabled known(ui, page->info.has_value());
            const float w = compact ? control::medium + 6 : follow_width(ui);
            follow_button(ui, ui.id("profile-follow"), {right - w, button_y, w, control::medium}, card.source, person_of(who), true);
            right -= w + 8;
        }

        // Their name, and beside it what the service makes of them.
        const float tx = face.right() + (compact ? 16.f : 24.f), room = right - tx - 8;
        const auto name_style = t.style(compact ? type::title : type::display, t.text, text::semibold);
        struct Badge {
            std::string text;
            bool lit;
        };
        std::inplace_vector<Badge, 2> badges;
        if (who.level >= 0) {
            badges.push_back({std::format("Lv.{}", who.level), false});
        }
        if (who.vip) {
            badges.push_back({"VIP", true});
        }
        const auto badge_style = t.style(type::micro, {}, text::medium);
        float badges_w = 0;
        for (const auto &badge : badges) {
            badges_w += std::ceil(p.measure(badge.text, badge_style)) + 20;
        }
        const float name_w = std::ceil(p.measure(who.name, name_style)) + 1;
        // The badges show where the whole name leaves them room.
        const bool tagged = name_w + 10 + badges_w <= room;
        ui.label({tx, top, std::min(name_w, room), name_h}, who.name, name_style);
        if (tagged) {
            float x = tx + name_w + 10;
            for (const auto &badge : badges) {
                const float w = std::ceil(p.measure(badge.text, badge_style)) + 14;
                const Rect pill{x, top + (name_h - 20) / 2 + 1, w, 20};
                p.rect(pill, badge.lit ? t.accent.with_alpha(0.14f) : t.fill(1.6f), 10, badge.lit ? t.accent.with_alpha(0.24f) : t.surface_edge(0.8f), 1);
                ui.label(pill, badge.text, t.style(type::micro, badge.lit ? t.accent : t.text_dim, text::medium), Align::center);
                x += w + 6;
            }
        }

        // What they say of themselves, in two lines at most.
        float y = top + name_h + (compact ? 4.f : 8.f);
        const float text_w = r.right() - tx;
        if (page->loading && !page->info) {
            ui.skeleton({tx, y + 5, std::min(text_w, 260.f), 10}, 5);
            y += 20;
        } else {
            const std::string_view about = !page->info && !page->error.empty() ? lyren::tr(page->error) : std::string_view(who.signature);
            const auto style = t.style(type::label, t.text_dim);
            int lines = 0;
            for (const auto line : p.wrap_lines(about, style.size, text_w)) {
                if (line.empty()) {
                    continue;
                }
                ui.label({tx, y, text_w, 20}, line, style);
                y += 20;
                if (++lines == 2) {
                    break;
                }
            }
        }
        std::inplace_vector<std::pair<std::string_view, std::string>, 4> facts;
        if (!tagged && who.level >= 0) {
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
        if (!facts.empty()) {
            y += 4;
            const auto style = t.style(type::caption, t.faint());
            float x = tx;
            for (const auto &[icon, text] : facts) {
                const float w = std::ceil(p.measure(text, style)) + 19;
                if (x + w > r.right()) {
                    break;
                }
                p.icon(icon, {x, y + 3, 13, 13}, t.faint());
                ui.label({x + 18, y, w - 18, 19}, text, style);
                x += w + 16;
            }
            y += 19;
        }
        y = std::max(y, face.bottom()) + 22;
        y += stats(ui, {r.x, y, r.w, 0}, page, who);
        return y + 14 - r.y;
    }

    float ProfileView::stats(ui::Ui &ui, Rect r, const Listener &page, const Profile &who) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &caps = provider(page->who).capabilities;
        // The people around them, what they have posted and how much they have played; the ones that lead somewhere
        // open their part of the page.
        struct Stat {
            std::string_view label;
            int value;
            std::optional<Section> opens;
        };
        std::inplace_vector<Stat, 4> all;
        if (caps.people) {
            all.push_back({lyren::tr_ctx("nav", "Following"), social_.count(*page, false), Section::follows});
            all.push_back({lyren::tr("Followers"), social_.count(*page, true), Section::fans});
        }
        if (caps.posts) {
            all.push_back({lyren::tr("Posts"), who.posts, Section::posts});
        }
        if (who.listened >= 0) {
            all.push_back({lyren::tr("Songs played"), who.listened, caps.ranking ? std::optional(Section::chart) : std::nullopt});
        }
        if (all.empty()) {
            return 0;
        }
        const bool tight = r.w < 460;
        constexpr float h = 64;
        const float radius = t.radius_l() * 0.75f;
        const Rect bar{r.x, r.y, r.w, h};
        p.rect(bar, t.dark ? t.fill(0.72f) : t.bg.mix(t.surface, 0.42f), radius, t.surface_edge(0.85f), 1);
        const float cell = bar.w / static_cast<float>(all.size());
        float x = bar.x;
        for (size_t i = 0; i < all.size(); ++i) {
            const Stat &stat = all[i];
            const ui::Id id = ui::sub_id(ui.id("profile-stat"), i);
            const Rect slot{x, bar.y, cell, h};
            if (i > 0) {
                p.rect({x, bar.y + 14, 1, h - 28}, t.line(0.65f));
            }
            float hot = 0;
            if (stat.opens) {
                const Rect hit = slot.inset(4);
                const auto it = ui.interact(id, hit);
                const auto feel = ui.feel(id, it, pace::quick);
                hot = feel.hot;
                if (hot > 0) {
                    p.rect(hit.inset(feel.down), t.fill(0.95f * hot), std::max(radius - 4, 4.f));
                }
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    if (*stat.opens == Section::posts) {
                        page->posts_tab = 0;
                    }
                    show(ui, page, *stat.opens);
                }
            }
            // A follow made here rolls the count over; clear vertical separation prevents any numeral/label touch.
            const float pad_x = tight ? 10.f : 18.f;
            const Rect value{slot.x + pad_x, bar.y + 9, std::max(0.f, slot.w - 2 * pad_x), 26};
            const auto num_style = t.style(tight ? type::lead : type::title, t.text.mix(t.accent, 0.35f * hot), text::semibold | text::tabular_nums);
            if (stat.value < 0) {
                ui.label(value, "—", num_style);
            } else {
                rolling_count(ui, ui::sub_id(id, 0xc7), value, stat.value, stat_number, num_style);
            }
            ui.label({slot.x + pad_x, bar.y + 38, std::max(0.f, slot.w - 2 * pad_x), 18}, stat.label, t.style(type::caption, t.text_dim.mix(t.text, 0.55f * hot)));
            x += cell;
        }
        return h;
    }

    void ProfileView::tabs(ui::Ui &ui, Rect r, const Listener &page, const Profile &who, float pinned) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        struct Tab {
            std::string_view label, icon;
        };
        // In the order of Section.
        static constexpr std::array<Tab, 5> all{
            {{"Posts", "message-square"}, {"Most played", "headphones"}, {"Playlists", "list-music"}, {"Following", "users"}, {"Followers", "heart"}}};
        const auto sections = social_.sections(page->who);
        std::inplace_vector<std::string_view, all.size()> labels;
        const auto style = t.style(type::lead, {}, text::medium);
        float needed = 28 * static_cast<float>(sections.size() - 1);
        for (const Section part : sections) {
            const auto &tab = all[static_cast<size_t>(part)];
            labels.push_back(part == Section::follows ? lyren::tr_ctx("nav", tab.label) : lyren::tr(tab.label));
            needed += p.measure(labels.back(), style);
        }
        const auto at = std::ranges::find(sections, page->section);
        int selected = at == sections.end() ? 0 : static_cast<int>(at - sections.begin());
        if (needed <= r.w) {
            if (ui.tabs(ui.id("profile-tabs"), r, labels, selected)) {
                show(ui, page, sections[static_cast<size_t>(selected)]);
            }
            // With the head scrolled away, the tabs say whose page this is.
            if (pinned > 0 && r.w - needed > 220) {
                Fade identity(p, pinned);
                const auto name = t.style(type::label, t.text_dim, text::medium);
                const float w = std::min(std::ceil(p.measure(who.name, name)) + 1, 160.f);
                const Rect face{r.right() - w - 34, r.y + (r.h - 24) / 2 + 4 * (1 - pinned), 24, 24};
                cover(ui, face, who.avatar, 12, "user");
                ui.label({face.right() + 10, face.y, w, 24}, who.name, name);
            }
            return;
        }
        // Too narrow for words: the tabs as icons, evenly spread, under a mark that glides between them.
        const float w = r.w / static_cast<float>(sections.size());
        for (size_t i = 0; i < sections.size(); ++i) {
            const Rect cell{r.x + static_cast<float>(i) * w, r.y, w, r.h};
            if (ui.icon_button(ui::sub_id(ui.id("profile-tabs"), i), ui::center(cell, 36, 36), all[static_cast<size_t>(sections[i])].icon, 18, static_cast<int>(i) == selected,
                               labels[i])) {
                show(ui, page, sections[i]);
            }
        }
        const float x = ui.spring(ui.id("profile-tab-mark"), static_cast<float>(selected) * w + (w - 24) / 2, pace::glide);
        p.rect({r.x + x, r.bottom() - 2, 24, 2}, t.accent, 1);
    }

    float ProfileView::section(ui::Ui &ui, Rect r, const Listener &page, bool self) {
        switch (page->section) {
            case Section::posts:
                return posts(ui, r, page, self);
            case Section::chart:
                return chart(ui, r, page);
            case Section::playlists:
                return playlists(ui, r, page, self);
            case Section::follows:
                return people(ui, r, page, false, self);
            case Section::fans:
                return people(ui, r, page, true, self);
        }
        return 0;
    }

    float ProfileView::posts(ui::Ui &ui, Rect r, const Listener &page, bool self) {
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &caps = provider(card).capabilities;
        Rect column = r;
        float beside = 0;
        if (r.w >= 540 + rail_gap + rail_min && (caps.ranking || caps.people)) {
            // The framed studio rail stays in sight while the timeline scrolls past it.
            const float rw = std::clamp(r.w - PostFeed::column - rail_gap, rail_min, rail_max);
            const Rect clip = ui.clip();
            Rect side = ui::cut_right(column, rw);
            ui::cut_right(column, rail_gap);
            side.y = std::max(side.y, clip.y + section_gap);
            side.h = clip.bottom() - side.y - 12;
            beside = rail(ui, side, page);
        }
        const float feed_w = std::min(column.w, PostFeed::column);
        float y = column.y;
        // The account's own page also holds the feed of the people it follows.
        const bool feed = self && page->posts_tab == 1;
        if (self) {
            const std::array<std::string_view, 2> labels{lyren::tr("My posts"), lyren::tr("Following feed")};
            int tab = page->posts_tab;
            if (ui.segmented(ui.id("profile-feed"), {column.x, y, std::min(280.f, feed_w - 48), control::medium}, labels, tab)) {
                page->posts_tab = tab;
                show(ui, page, Section::posts);
            }
            const Rect again{column.x + feed_w - control::medium, y, control::medium, control::medium};
            if (const auto &list = feed ? social_.feed(card.source) : page->posts; list->loading) {
                ui.spinner(ui::center(again, 16, 16), t.text_dim, 2);
            } else if (ui.icon_button(ui.id("profile-refresh"), again, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
                social_.reload_posts(page, feed);
            }
            y += control::medium + 18;
            if (caps.post_write) {
                y += shell_.feed.prompt(ui, {column.x, y, feed_w, 0}, card.source) + 6;
            }
        }
        const auto list = feed ? social_.feed(card.source) : page->posts;
        y += shell_.feed.draw(ui, {column.x, y, feed_w, 0}, list, card.source, feed ? std::string() : card.id, feed);
        return std::max(y - r.y, beside);
    }

    float ProfileView::rail(ui::Ui &ui, Rect r, const Listener &page) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &caps = provider(card).capabilities;
        const auto &follows = page->people[0];
        const bool faces = caps.people && !follows.items.empty();
        const float radius = t.radius_l() * 0.8f;
        const ui::Color card_fill = t.dark ? t.fill(0.78f) : t.bg.mix(t.surface, 0.38f);
        float y = r.y;

        auto card_header = [&](Rect box, ui::Id id, std::string_view title, std::string_view note, Section opens) {
            const auto style = t.style(type::label, t.text, text::semibold);
            const float w = std::ceil(p.measure(title, style)) + 1;
            ui.label({box.x + 16, box.y + 12, w, 24}, title, style);
            ui.label({box.x + 16 + w + 8, box.y + 12, std::max(0.f, box.w - w - 64), 24}, note, t.style(type::caption, t.faint(), text::tabular_nums));
            if (ui.icon_button(id, {box.right() - 36, box.y + 10, 28, 28}, "chevron-right", 16, false, lyren::tr("See all"))) {
                show(ui, page, opens);
            }
            p.rect({box.x + 16, box.y + 45, box.w - 32, 1}, t.line(0.7f));
        };

        if (caps.ranking) {
            auto &chart = page->charts[static_cast<size_t>(page->chart_tab)];
            auto &tracks = chart.ranking.tracks;
            constexpr float head_h = 46, row_h = 52;
            const auto max_rows = static_cast<size_t>(std::clamp(static_cast<int>((r.bottom() - y - head_h - (faces ? 160.f : 24.f)) / row_h), 1, 5));
            const size_t shown = !chart.loaded ? std::min<size_t>(max_rows, 3) : std::min(tracks.size(), max_rows);
            const float body_h = !chart.loaded ? static_cast<float>(shown) * row_h + 12 : tracks.empty() ? 52.f : static_cast<float>(shown) * row_h + 12;
            const Rect box{r.x, y, r.w, head_h + body_h};
            p.rect(box, card_fill, radius, t.surface_edge(0.9f), 1);
            card_header(box, ui.id("rail-chart"), lyren::tr("On repeat"), lyren::tr(page->chart_tab ? "All time" : "This week"), Section::chart);

            float ry = box.y + head_h + 6;
            if (!chart.loaded) {
                for (size_t i = 0; i < shown; ++i) {
                    const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f);
                    ui.skeleton({box.x + 16, ry + 7, 38, 38}, t.radius_s());
                    ui.skeleton({box.x + 66, ry + 13, (box.w - 86) * (0.5f + 0.4f * k), 10}, 5);
                    ui.skeleton({box.x + 66, ry + 31, (box.w - 86) * (0.3f + 0.3f * (1 - k)), 9}, 4.5f);
                    ry += row_h;
                }
            } else if (tracks.empty()) {
                const bool hidden = chart.error == "This listening ranking is private";
                p.icon(hidden ? "lock" : "headphones", {box.x + 16, ry + 11, 14, 14}, t.faint());
                ui.label({box.x + 38, ry + 8, box.w - 54, 20},
                         lyren::tr(hidden                ? "Their ranking is private"
                                   : chart.error.empty() ? "Nothing played yet"
                                                         : chart.error),
                         t.style(type::caption, t.text_dim));
            } else {
                ui.push_id(ui.id("rail-songs"));
                for (size_t i = 0; i < shown; ++i) {
                    const Track &track = tracks[i];
                    const float in = ui.cascade(std::max(page->section_since, chart.since), static_cast<int>(i));
                    Fade fade(p, in);
                    const Rect row{box.x + 8, ry + (1 - in) * 8, box.w - 16, row_h};
                    const ui::Id id = item_id(ui, track);
                    const auto it = ui.interact(id, row);
                    const auto [hot, down] = ui.feel(id, it, pace::quick);
                    if (hot > 0) {
                        p.rect(row.inset(down, 2 + down), t.fill(hot), t.radius_s());
                    }
                    ui.label({row.x + 4, row.y, 18, row.h}, std::to_string(i + 1),
                             t.style(type::caption, i < 3 ? t.accent.with_alpha(0.85f) : t.faint(), text::semibold | text::tabular_nums), Align::center);
                    const Rect art{row.x + 26, row.y + 7, 38, 38};
                    const bool on = playback_.is_current(track);
                    const float veil = std::max(hot, on ? 1.f : 0.f);
                    cover(ui, art, track.cover, t.radius_s(), veil > 0.5f ? "" : "music");
                    p.rect(art, {}, t.radius_s(), t.surface_edge(0.8f), 1);
                    if (veil > 0) {
                        p.rect(art, ui::Color{0, 0, 0, 0.42f * veil}, t.radius_s());
                        if (on && playback_.playing() && hot < 0.5f) {
                            ui.equalizer(ui::sub_id(id, 1), ui::center(art, 13, 13), white, true);
                        } else {
                            Rect glyph = ui::center(art, 13, 13);
                            glyph.x += on && playback_.playing() ? 0 : 1;
                            p.icon(on && playback_.playing() ? "pause-fill" : "play-fill", glyph, white.with_alpha(veil));
                        }
                    }
                    const float tx = art.right() + 12, tw = row.right() - tx - 8;
                    p.text(tx, row.y + 23, track.title, t.style(type::label, on ? t.accent : t.text, text::medium), tw);
                    p.text(tx, row.y + 41, track.artist, t.style(type::caption, t.text_dim), tw);
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    if (it.clicked) {
                        if (on) {
                            playback_.toggle();
                        } else {
                            playback_.play(tracks, static_cast<int>(i));
                        }
                    }
                    if (it.right_clicked) {
                        shell_.tracks.menu(ui, it.px, it.py, tracks, static_cast<int>(i));
                    }
                    ry += row_h;
                }
                ui.pop_id();
            }
            y = box.bottom() + 16;
        }

        if (faces && y + 106 <= r.bottom()) {
            const int count = social_.count(*page, false);
            constexpr float head_h = 46, person_h = 48;
            const float avail = r.bottom() - y - head_h - 14;
            const bool list_mode = avail >= person_h * 2;
            const size_t rows = list_mode ? std::min<size_t>({follows.items.size(), static_cast<size_t>(avail / person_h), 4}) : 0;
            const float body_h = list_mode ? static_cast<float>(rows) * person_h + 12 : 62.f;
            const Rect box{r.x, y, r.w, head_h + body_h};
            p.rect(box, card_fill, radius, t.surface_edge(0.9f), 1);
            card_header(box, ui.id("rail-people"), lyren::tr_ctx("nav", "Following"), count >= 0 ? stat_number(count) : std::string(), Section::follows);

            ui.push_id(ui.id("rail-faces"));
            if (list_mode) {
                float py = box.y + head_h + 6;
                for (size_t i = 0; i < rows; ++i) {
                    const Person &who = follows.items[i];
                    const Rect row{box.x + 8, py, box.w - 16, person_h};
                    const ui::Id id = ui.id(who.id);
                    const auto it = ui.interact(id, row);
                    const auto [hot, down] = ui.feel(id, it, pace::quick);
                    if (hot > 0) {
                        p.rect(row.inset(down, 2 + down), t.fill(hot), t.radius_s());
                    }
                    const Rect face{row.x + 8, row.y + (row.h - 34) / 2, 34, 34};
                    cover(ui, face, who.avatar, 17, "user");
                    p.rect(face, {}, 17, t.surface_edge(0.85f), 1);
                    const float tx = face.right() + 12, tw = row.right() - tx - 8;
                    std::string meta = who.signature.substr(0, who.signature.find('\n'));
                    if (meta.empty() && who.fans >= 0) {
                        meta = lyren::trf("{} fans", short_number(who.fans));
                    }
                    p.text(tx, row.y + (meta.empty() ? 29.f : 21.f), who.name, t.style(type::label, t.text, text::medium), tw);
                    if (!meta.empty()) {
                        p.text(tx, row.y + 38, meta, t.style(type::caption, t.text_dim), tw);
                    }
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    if (it.clicked) {
                        browser_.open_profile(card.source, who.id, who.name, who.avatar);
                    }
                    py += person_h;
                }
            } else {
                constexpr float d = 36, gap = 8;
                const auto n = std::min(follows.items.size(), static_cast<size_t>((box.w - 32 + gap) / (d + gap)));
                for (size_t i = 0; i < n; ++i) {
                    const Person &who = follows.items[i];
                    const Rect face{box.x + 16 + static_cast<float>(i) * (d + gap), box.y + head_h + 13, d, d};
                    const ui::Id id = ui.id(who.id);
                    const auto it = ui.interact(id, face);
                    const auto [hot, down] = ui.feel(id, it, pace::quick);
                    cover(ui, face.inset(down), who.avatar, d / 2, "user");
                    p.rect(face.inset(down), {}, d / 2, t.surface_edge(0.85f), 1);
                    if (hot > 0) {
                        p.rect(face.inset(-2), {}, d / 2 + 2, t.text.with_alpha(0.4f * hot), 1.5f);
                    }
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                        if (!it.held) {
                            ui.tooltip(id, face, who.name);
                        }
                    }
                    if (it.clicked) {
                        browser_.open_profile(card.source, who.id, who.name, who.avatar);
                    }
                }
            }
            ui.pop_id();
            y = box.bottom() + 8;
        }
        return y - r.y;
    }

    float ProfileView::chart(ui::Ui &ui, Rect r, const Listener &page) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &chart = page->charts[static_cast<size_t>(page->chart_tab)];
        auto &tracks = chart.ranking.tracks;
        // The week or all time on the left, playing them all on the right.
        const Rect bar{r.x, r.y, r.w, control::medium};
        const auto play = bar.w < 380 ? std::string_view{} : lyren::tr("Play all");
        const float play_w = play.empty() ? control::medium + 6 : ui.button_width(play, true);
        {
            ui::Enabled possible(ui, !tracks.empty());
            if (ui.button(ui.id("chart-play"), {bar.right() - play_w, bar.y, play_w, bar.h}, play, ui::ButtonKind::primary, "play-fill")) {
                playback_.play(tracks, 0);
            }
        }
        const std::array<std::string_view, 2> labels{lyren::tr("This week"), lyren::tr("All time")};
        if (int tab = page->chart_tab; ui.segmented(ui.id("chart-tabs"), {bar.x, bar.y, std::min(232.f, bar.w - play_w - 16), bar.h}, labels, tab)) {
            page->chart_tab = tab;
            show(ui, page, Section::chart);
        }
        const float top = bar.bottom() + 16;
        if (tracks.empty()) {
            if (!chart.loaded && !chart.loading) {
                social_.load_chart(page, page->chart_tab);
            }
            const Rect room{r.x, top, r.w, 300};
            if (chart.error == "This listening ranking is private") {
                empty(ui, room, "lock", lyren::tr("Their ranking is private"), lyren::tr("Only they can see what they play most"));
            } else if (!page_status(ui, room, !chart.loaded, chart.error, false, Placeholder::rows)) {
                empty(ui, room, "headphones", lyren::tr("Nothing played yet"));
            }
            return top - r.y + room.h;
        }
        constexpr float row_h = 56;
        const Rect clip = ui.clip();
        const int count = static_cast<int>(tracks.size());
        const int first = std::max(0, static_cast<int>((clip.y - top) / row_h)), last = std::min(static_cast<int>((clip.bottom() - top) / row_h) + 1, count - 1);
        const double since = std::max(page->section_since, chart.since);
        const auto &plays = chart.ranking.plays, &scores = chart.ranking.scores;
        const int most = plays.empty() ? 0 : std::ranges::max(plays);
        const auto numbers = t.style(type::label, t.faint(), text::tabular_nums);
        ui.push_id(ui::sub_id(ui.id("profile-chart"), static_cast<uint64_t>(page->chart_tab)));
        for (int i = first; i <= last; ++i) {
            const auto k = static_cast<size_t>(i);
            const Track &track = tracks[k];
            const float in = ui.cascade(since, i - first);
            Fade fade(p, in);
            const Rect row{r.x, top + static_cast<float>(i) * row_h + (1 - in) * 10, r.w, row_h};
            ui.push_id(item_id(ui, track));
            const ui::Id row_id = ui.id("row");
            const bool pointed = ui.hovering(row) || ui.is_active(row_id), on = playback_.is_current(track);
            if (const float hot = ui.anim(row_id, pointed ? 1 : 0, pace::quick); hot > 0) {
                p.rect(row.inset(0, 2), t.fill(hot), t.radius_s());
            }
            const Rect rank{row.x + 4, row.y, 40, row.h};
            if (pointed) {
                if (ui.icon_button(ui.id("play"), ui::center(rank, 30, 30), on && playback_.playing() ? "pause-fill" : "play-fill", 14)) {
                    if (on) {
                        playback_.toggle();
                    } else {
                        playback_.play(tracks, i);
                    }
                }
            } else if (on) {
                ui.equalizer(ui.id("playing"), ui::center(rank, 14, 14), t.accent, playback_.playing());
            } else {
                // The first three stand out.
                ui.label(rank, std::to_string(i + 1), i < 3 ? t.style(type::lead, t.accent, text::semibold | text::tabular_nums) : numbers, Align::center);
            }
            const Rect art{rank.right() + 6, row.y + 8, 40, 40};
            cover(ui, art, track.cover, t.radius_s());
            p.rect(art, {}, t.radius_s(), t.surface_edge(0.8f), 1);
            // How often it is played, against the most played; the bars grow in with their rows.
            const float meter_w = std::min(180.f, std::round(row.w * 0.24f));
            const float tx = art.right() + 14, tw = row.right() - tx - meter_w - 36;
            p.text(tx, row.y + 25, track.title, t.style(type::body, on ? t.accent : t.text, on ? text::medium : 0), tw);
            p.text(tx, row.y + 43, track.artist, t.style(type::caption, t.text_dim), tw);
            const int played = k < plays.size() ? plays[k] : 0;
            const float share = most > 0 && played > 0 ? static_cast<float>(played) / static_cast<float>(most) : k < scores.size() ? static_cast<float>(scores[k]) / 100 : 0;
            const Rect meter{row.right() - meter_w - 12, row.y + row.h / 2 + 8, meter_w, 3};
            p.rect(meter, t.fill(1.6f), 1.5f);
            p.rect({meter.x, meter.y, meter.w * share * in, meter.h}, t.accent.with_alpha(0.8f), 1.5f);
            if (played > 0) {
                ui.label({meter.x, row.y + 10, meter.w, 18}, lyren::trf("{} plays", grouped(played)), t.style(type::micro, t.faint(), text::tabular_nums), Align::right);
            }
            const auto it = ui.interact(row_id, row);
            if (it.double_clicked) {
                playback_.play(tracks, i);
            }
            if (it.right_clicked) {
                shell_.tracks.menu(ui, it.px, it.py, tracks, i);
            }
            ui.pop_id();
        }
        ui.pop_id();
        return top - r.y + static_cast<float>(count) * row_h + 8;
    }

    float ProfileView::playlists(ui::Ui &ui, Rect r, const Listener &page, bool self) {
        const Card &card = page->who;
        std::vector<Card> made, saved;
        double since = page->section_since;
        if (self) {
            // The account's own come from its library, the liked songs first.
            const auto &shelf = library_.shelf(card.source);
            if (!shelf.liked.id.empty()) {
                made.push_back(shelf.liked);
            }
            for (const auto &playlist : shelf.playlists) {
                (playlist.owned ? made : saved).push_back(playlist);
            }
            if (made.empty() && saved.empty()) {
                const Rect room{r.x, r.y, r.w, 300};
                if (!page_status(ui, room, !shelf.playlists_loaded && shelf.error.empty(), shelf.error, false, Placeholder::cards)) {
                    empty(ui, room, "list-music", lyren::tr("No playlists yet"));
                }
                return room.h;
            }
        } else {
            const auto &listing = page->playlists;
            if (listing.cards.empty()) {
                const Rect room{r.x, r.y, r.w, 300};
                if (!page_status(ui, room, !listing.loaded, listing.error, false, Placeholder::cards)) {
                    empty(ui, room, "list-music", lyren::tr("No playlists yet"));
                }
                return room.h;
            }
            const auto split = static_cast<std::ptrdiff_t>(std::clamp(page->created, 0, static_cast<int>(listing.cards.size())));
            made.assign(listing.cards.begin(), listing.cards.begin() + split);
            saved.assign(listing.cards.begin() + split, listing.cards.end());
            since = std::max(since, listing.since);
        }
        float y = r.y;
        std::optional<Card> picked;
        auto shelf = [&](std::string_view key, std::string_view title, const std::vector<Card> &cards) {
            if (cards.empty()) {
                return;
            }
            section_heading(ui, r.x, y, r.w, title, std::to_string(cards.size()));
            y += 42;
            if (const int i = card_grid(ui, {r.x, y, r.w, 0}, ui.id(key), cards, false, since); i >= 0) {
                picked = cards[static_cast<size_t>(i)];
            }
            y += CardGrid::settled_height(r.w, cards.size()) + 32;
        };
        shelf("made", lyren::tr("Created playlists"), made);
        shelf("saved", lyren::tr("Saved playlists"), saved);
        if (picked) {
            browser_.open(*picked);
        }
        return y - r.y;
    }

    float ProfileView::people(ui::Ui &ui, Rect r, const Listener &page, bool fans, bool self) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        auto &list = page->people[fans];
        if (list.items.empty()) {
            if (list.open()) {
                social_.load_people(page, fans);
            }
            const Rect room{r.x, r.y, r.w, 300};
            if (!list.error.empty() && !env_.signed_in(card.source)) {
                // Some services show who follows whom only to those signed in.
                page_status(ui, room, false, "", true);
            } else if (!page_status(ui, room, list.loading || (list.more && list.error.empty()), list.error, false, Placeholder::faces)) {
                empty(ui, room, "users", lyren::tr(fans ? (self ? "No fans yet" : "No fans to show") : (self ? "You are not following anyone yet" : "Not following anyone yet")));
            }
            return room.h;
        }
        // In as many columns as fit, a page more as the last rows come near.
        constexpr float gap = 12, row_h = 72;
        const auto cols = static_cast<size_t>(std::max(1, static_cast<int>((r.w + gap) / (320 + gap))));
        const float cell_w = (r.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const size_t rows = (list.items.size() + cols - 1) / cols;
        const Rect clip = ui.clip();
        const double since = std::max(page->section_since, list.since);
        int shown = 0;
        ui.push_id(ui.id(fans ? "fans" : "follows"));
        for (size_t i = 0; i < list.items.size(); ++i) {
            Rect cell{r.x + static_cast<float>(i % cols) * (cell_w + gap), r.y + static_cast<float>(i / cols) * (row_h + gap), cell_w, row_h};
            if (cell.bottom() < clip.y || cell.y > clip.bottom()) {
                continue;
            }
            const float in = ui.cascade(since, shown++);
            Fade fade(p, in);
            cell.y += (1 - in) * 10;
            person(ui, cell, card.source, list.items[i]);
        }
        ui.pop_id();
        const float end = r.y + static_cast<float>(rows) * (row_h + gap);
        const Rect foot{r.x, end, r.w, 56};
        if (list.loading) {
            ui.spinner(ui::center(foot, 20, 20), t.text_dim, 2);
        } else if (!list.error.empty()) {
            ui.label({foot.x, foot.y + 16, foot.w, 20}, lyren::tr(list.error), t.style(type::caption, t.text_dim), Align::center);
        } else if (list.more && end < clip.bottom() + 400) {
            social_.load_people(page, fans);
        }
        return end - r.y + (list.loading || list.more || !list.error.empty() ? foot.h : 0);
    }

    void ProfileView::person(ui::Ui &ui, Rect r, Source source, const Person &person) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ui.push_id(ui.id(person.id));
        const ui::Id id = ui.id("card");
        // Held on its follow button, the card stays lit.
        const float hot = ui.anim(id, ui.pointing(r) ? 1 : 0, pace::quick);
        p.rect(r, t.fill(0.7f + 0.8f * hot), t.radius, t.surface_edge(0.75f + 0.45f * hot), 1);
        const Rect face{r.x + 14, r.y + (r.h - 44) / 2, 44, 44};
        cover(ui, face, person.avatar, 22, "user");
        const auto &account = library_.shelf(source).account;
        const bool followable = provider(source).capabilities.follow_users && !(account && account->id == person.id);
        const float button_w = followable ? follow_width(ui) : 0;
        const float tx = face.right() + 14, tw = r.right() - tx - (followable ? button_w + 26 : 14);
        std::string about = person.signature.substr(0, person.signature.find('\n'));
        if (person.fans >= 0) {
            const std::string fans = lyren::trf("{} fans", short_number(person.fans));
            about = about.empty() ? fans : fans + " · " + about;
        }
        p.text(tx, r.y + (about.empty() ? 41.f : 31.f), person.name, t.style(type::body, t.text, text::medium), tw);
        p.text(tx, r.y + 50, about, t.style(type::caption, t.text_dim), tw);
        if (followable) {
            follow_button(ui, ui.id("follow"), {r.right() - button_w - 14, r.y + (r.h - control::small) / 2, button_w, control::small}, source, person);
        }
        const auto it = ui.interact(id, r);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (it.clicked) {
            browser_.open_profile(source, person.id, person.name, person.avatar);
        }
        ui.pop_id();
    }

} // namespace lyren
