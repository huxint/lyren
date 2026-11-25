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
        // The page is an editorial column: the main content and, when there is room for both, a framed studio rail beside it.
        constexpr float rail_gap = 32, rail_min = 276, rail_max = 324, page_w = PostFeed::column + rail_gap + rail_max;
        constexpr float tabs_h = 38, section_gap = 20;
        // The head: the picture's diameter, the room around a count for its hover pill, and how much of a long
        // signature is worth wrapping for the two lines that show.
        constexpr float face_wide = 104, face_compact = 72, stat_pad = 10, stat_gap = 4;
        constexpr size_t about_limit = 480;

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

        ui::Color studio_card_fill(const ui::Theme &t) {
            return t.dark ? t.fill(0.78f) : t.bg.mix(t.surface, 0.38f);
        }

        float studio_card_radius(const ui::Theme &t) {
            return t.radius_l() * 0.8f;
        }
    } // namespace

    ProfileView::ProfileView(Shell &shell)
    : Part(shell) {
        library_.subscribe([this](const Library::Event &) {
            playlists_dirty_ = true;
        });
    }

    void ProfileView::ensure_own_playlists(Source source) {
        if (!playlists_dirty_ && playlists_source_ == source) {
            return;
        }
        playlists_dirty_ = false;
        playlists_source_ = source;
        own_made_.clear();
        own_saved_.clear();
        own_preview_.clear();
        const auto &shelf = library_.shelf(source);
        if (!shelf.liked.id.empty()) {
            own_made_.push_back(shelf.liked);
        }
        for (const auto &playlist : shelf.playlists) {
            (playlist.owned ? own_made_ : own_saved_).push_back(playlist);
        }
        for (const auto &playlist : shelf.playlists) {
            if (playlist.owned && own_preview_.size() < 8) {
                own_preview_.push_back(playlist);
            }
        }
        for (const auto &playlist : shelf.playlists) {
            if (!playlist.owned && own_preview_.size() < 8) {
                own_preview_.push_back(playlist);
            }
        }
        if (own_preview_.empty() && !shelf.liked.id.empty()) {
            own_preview_.push_back(shelf.liked);
        }
    }

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
            const auto label = lyren::tr(!following ? "Follow" : mutual ? "Mutual" : "Following");
            const auto kind = following ? ui::ButtonKind::secondary : prominent ? ui::ButtonKind::primary : ui::ButtonKind::tonal;
            clicked |= ui.button(ui::sub_id(id, following), r, bare ? std::string_view{} : label, kind,
                                 !following ? "user-plus"
                                 : mutual   ? "arrow-left-right"
                                            : "user-check",
                                 bare ? label : std::string_view{});
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
        if (section != page->section) {
            section_dir_ = section > page->section ? 1 : -1;
        }
        social_.show(page, section);
        ui.scroll_to(scroll_, std::min(ui.scroll_offset(scroll_), header_h_));
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
            rail_repeat_rows_ = 0;
            rail_follow_rows_ = -1;
            repeat_slot_.reset();
        }
        // The page's own account of them; until it has one, or where it leaves gaps, the card that led here fills in.
        std::optional<Profile> patched;
        if (!page->info || page->info->name.empty() || page->info->avatar.empty()) {
            patched = page->info ? *page->info : Profile{.id = card.id};
            if (patched->name.empty()) {
                patched->name = card.title;
            }
            if (patched->avatar.empty()) {
                patched->avatar = card.cover;
            }
        }
        const Profile &who = patched ? *patched : *page->info;
        Rect body = r.inset(r.w >= 760 ? 36.f : r.w < 480 ? 16.f : 24.f, 0);
        if (body.w > page_w) {
            body = ui::center(body, page_w, body.h);
        }
        if (body.w < 588) {
            compact_ = true;
        } else if (body.w > 612) {
            compact_ = false;
        }

        scroll_ = scroll_id(ui, "profile");
        const float off = ui.begin_scroll(scroll_, r, std::max(content_h_, r.h));
        // Their background washes the top of the page with gentle parallax; without one, the colour of their picture.
        if (const float scroll_fade = 1 - smooth01(off / 200); scroll_fade > 0.01f) {
            const float wash_h = header_h_ > 0 ? header_h_ + tabs_h : 228.f;
            const Rect band{r.x, r.y - std::min(off * 0.22f, 44.f), r.w, wash_h};
            const auto *image = who.background.empty() ? nullptr : &env_.images.get(who.background, 128, true);
            if (image && image->ready) {
                {
                    Fade wash(p, ui.progress(image->ready_at, 480, ease::standard) * (t.dark ? 0.30f : 0.17f) * scroll_fade);
                    const float aspect = static_cast<float>(image->w) / static_cast<float>(std::max(image->h, 1u));
                    p.image(band, image->tex, t.radius_l(), white, cover_uv(aspect, band.w / band.h));
                }
                p.gradient(band, t.surface.with_alpha(0.06f), t.surface, t.radius_l());
            } else if (!image) {
                const float hue = who.avatar.empty() ? -1 : cover_hue(who.avatar, (compact_ ? face_compact : face_wide) * p.scale());
                const ui::Color tint = hue >= 0 ? ui::hsl(hue, 0.5f, 0.5f) : t.accent;
                p.gradient(band, tint.with_alpha((t.dark ? 0.10f : 0.055f) * scroll_fade), tint.with_alpha(0), t.radius_l());
            }
        }
        // What the head and tabs remember belongs to this visit: a press, a count mid-roll or the tab mark does not
        // carry over to whoever's page comes next.
        ui.push_id(ui.id(visit_));
        header_h_ = header(ui, {body.x, r.y - off, body.w, 0}, page, who, self);
        if (browser_.route().serial != visit_) {
            // A click in the head led elsewhere: the rest of this page takes no more of this frame's input.
            ui.pop_id();
            return ui.end_scroll();
        }
        if (social_.sections(card).empty()) {
            // A platform that tells nothing more about its listeners.
            const Rect rest{body.x, r.y + header_h_ - off, body.w, std::max(r.h - header_h_, 240.f)};
            if (!page_status(ui, rest, page->loading, page->error, false)) {
                empty(ui, rest, "user", lyren::tr("No profile content available on this service"));
            }
            content_h_ = header_h_ + rest.h;
            ui.pop_id();
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
        ui.pop_id();
        ui.push_clip({r.x, strip.bottom(), r.w, r.bottom() - strip.bottom()});
        float height = 0;
        const float rail_budget_h = std::max(0.f, r.h - tabs_h - section_gap - 12.f);
        {
            const float in = ui.progress(page->section_since, pace::arrive);
            Fade fade(p, in);
            const float slide_x = (1 - in) * 16.f * static_cast<float>(section_dir_);
            height = section(ui, {body.x + slide_x, r.y + header_h_ + tabs_h + section_gap - off + (1 - in) * 6, body.w, 0}, rail_budget_h, page, self);
        }
        if (pinned > 0) {
            p.gradient({r.x, strip.bottom(), r.w, 10}, t.shadow.with_alpha(0.16f * pinned), t.shadow.with_alpha(0));
        }
        ui.pop_clip();
        if (const float next = header_h_ + tabs_h + section_gap + height + 28; std::abs(next - content_h_) > 0.5f) {
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
        const bool compact = compact_;
        // The head arrives as one: the picture a little small at first, settling to its size.
        const float in = ui.progress(page->since, pace::arrive);
        Fade fade(p, in);

        const float d = ui.spring(ui.id("profile-face-d"), compact ? face_compact : face_wide, 280);
        const Rect face{r.x, r.y + 24, d, d};
        {
            const ui::Id fid = ui.id("profile-face");
            const auto it = ui.interact(fid, face);
            const bool clickable = !who.avatar.empty();
            if (it.hovered && clickable) {
                ui.cursor = wl::Cursor::pointer;
            }
            const auto [hot, down] = clickable ? ui.feel(fid, it, pace::quick) : ui::Feel{};
            const float size = d * (0.94f + 0.06f * in) - down * 1.4f;
            Rect portrait = ui::center(face, size, size);
            portrait.y -= 1.5f * hot * (1.f - down);
            p.shadow({portrait.x, portrait.y + 5 + 2.f * hot, portrait.w, portrait.h}, size / 2, 18 + 6 * hot, t.shadow.with_alpha(0.42f + 0.14f * hot));
            cover(ui, portrait, who.avatar, size / 2, "user");
            if (hot > 0.01f) {
                p.rect(portrait, white.with_alpha((t.dark ? 0.055f : 0.075f) * hot), size / 2);
            }
            p.rect(portrait, {}, size / 2, t.surface_edge(1.1f).mix(t.text.with_alpha(t.dark ? 0.26f : 0.16f), hot), 1);
            if (it.right_clicked) {
                shell_.dialogs.link_menu(ui, it.px, it.py, card);
            } else if (it.clicked && clickable) {
                shell_.dialogs.show_picture(ui, who.avatar, 1);
            }
        }

        // What can be done here: following them, or writing a post of one's own, and the rest under a menu. Beside the
        // counts along the foot of the head where both fit on one line; otherwise at the top right, as icons once the
        // head is compact.
        const auto counts = stat_list(ui, page, who, self);
        float counts_w = 0;
        for (const Stat &stat : counts) {
            counts_w += stat.w + (counts_w > 0 ? stat_gap : 0);
        }
        const bool writes = self && caps.post_write, follows = !self && caps.follow_users;
        const float tx = face.right() + (compact ? 16.f : 24.f);
        // The counts' hover pills overhang their text, so the text itself lines up with the portrait and tab strip.
        const float counts_x = r.x - stat_pad;
        auto actions_width = [&](bool bare) {
            const float main = !writes && !follows ? 0 : bare ? control::medium + 6 : writes ? ui.button_width(lyren::tr("New post"), true) : follow_width(ui);
            return control::medium + (main > 0 ? main + 8 : 0);
        };
        const bool at_foot = !compact && !counts.empty() && counts_w + 24 + actions_width(false) <= r.right() - counts_x;
        const bool bare = compact && !at_foot;
        const float actions_w = actions_width(bare);
        auto actions = [&](float y) {
            float right = r.right();
            if (const Rect more{right - control::medium, y, control::medium, control::medium};
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
                // The menu outlives a turn of the page: it refreshes the visit it was opened on, or nothing.
                items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this, visit = browser_.route().serial] {
                                     if (auto &route = browser_.route(); route.page == Page::profile && route.serial == visit) {
                                         route.profile = social_.open(route.card);
                                         const auto &c = provider(route.card).capabilities;
                                         if ((library_.own(route.card) || route.card.owned) && (c.collect_albums || c.follow_artists) && env_.signed_in(route.card.source)) {
                                             spawn(library_.load_collection(route.card.source));
                                         }
                                     }
                                 }});
                ui.open_menu(more.x, more.bottom() + 6, std::move(items));
            }
            right -= control::medium + 8;
            const float w = actions_w - control::medium - 8;
            if (writes) {
                const auto label = lyren::tr("New post");
                if (ui.button(ui.id("profile-write"), {right - w, y, w, control::medium}, bare ? std::string_view{} : label, ui::ButtonKind::primary, "square-pen",
                              bare ? label : std::string_view{})) {
                    shell_.feed.compose(ui, card.source);
                }
            } else if (follows) {
                // Until the page says whether they are followed already, the button waits.
                ui::Enabled known(ui, page->info.has_value());
                follow_button(ui, ui.id("profile-follow"), {right - w, y, w, control::medium}, card.source, person_of(who), true);
            }
        };

        // Their name, and beside it their level, VIP, and platform badge.
        const float top = face.y + (compact ? 0.f : 2.f), name_h = compact ? 30.f : 36.f;
        if (!at_foot) {
            actions(top + (name_h - control::medium) / 2);
        }
        const float room = std::max(0.f, r.right() - tx - (at_foot ? 0.f : actions_w + 12));
        const auto name_style = t.style(compact ? type::title : type::display, t.text, text::semibold);
        // VIP is gold whatever the accent: an accent taken from a grey cover would leave it looking like the level.
        const ui::Color gold = ui::accent_for_hue(0.11f, t.dark);
        struct Badge {
            std::string text;
            std::string_view icon; // as a fact, when the name leaves the badges no room
            bool vip;
            float w = 0;
        };
        std::inplace_vector<Badge, 3> badges;
        if (who.vip) {
            badges.push_back({"VIP", "sparkles", true});
        }
        if (who.level >= 0) {
            badges.push_back({std::format("Lv.{}", who.level), "audio-lines", false});
        }
        if (!self) {
            badges.push_back({std::string(lyren::tr(platform(card.source).short_name)), "music", false});
        }
        const auto badge_style = t.style(type::micro, t.text_dim, text::semibold);
        float badges_w = 0;
        for (auto &badge : badges) {
            badge.w = std::ceil(p.measure(badge.text, badge_style)) + 14;
            badges_w += badge.w + 6;
        }
        const float name_w = std::ceil(p.measure(who.name, name_style)) + 1;
        const bool tagged = name_w + 4 + badges_w <= room;
        if (room > 0) {
            ui.label({tx, top, std::min(name_w, room), name_h}, who.name, name_style);
        }
        if (tagged) {
            float x = tx + name_w + 10;
            for (const auto &badge : badges) {
                const Rect pill{x, top + (name_h - 20) / 2 + 1, badge.w, 20};
                if (badge.vip) {
                    p.rect(pill, gold.with_alpha(0.14f), 10, gold.with_alpha(0.32f), 1);
                } else {
                    p.rect(pill, t.fill(1.5f), 10);
                }
                ui.label(pill, badge.text, t.style(type::micro, badge.vip ? gold : t.text_dim, text::semibold), Align::center);
                x += badge.w + 6;
            }
        }

        // What they say of themselves, in two lines at most, the second cut short where it runs on.
        float y = top + name_h + (compact ? 2.f : 4.f);
        const float text_w = std::max(0.f, r.right() - tx);
        if (page->loading && !page->info) {
            ui.skeleton({tx, y + 5, std::min(text_w, 260.f), 10}, 5);
            y += 20;
        } else if (text_w > 0) {
            std::string_view about = !page->info && !page->error.empty() ? lyren::tr(page->error) : std::string_view(who.signature);
            // Two lines never show more than this of it, however long it runs.
            if (about.size() > about_limit) {
                size_t cut = about_limit;
                while (cut > 0 && (static_cast<unsigned char>(about[cut]) & 0xc0) == 0x80) {
                    --cut;
                }
                about = about.substr(0, cut);
            }
            const auto style = t.style(type::label, t.text_dim);
            const float w = std::min(text_w, 720.f);
            int shown = 0;
            for (auto line : p.wrap_lines(about, style.size, w)) {
                if (line.empty()) {
                    continue;
                }
                if (++shown == 2) {
                    // The rest of its paragraph: the label ends it with an ellipsis when it does not fit, or when more
                    // paragraphs follow.
                    const std::string_view rest = about.substr(static_cast<size_t>(line.data() - about.data()));
                    const size_t nl = rest.find('\n');
                    line = nl == rest.npos ? rest : rest.substr(0, nl);
                    const bool more = nl != rest.npos && rest.find_first_not_of(" \t\r\n", nl + 1) != rest.npos;
                    if (more && p.measure(line, style) <= w) {
                        ui.label({tx, y, w, 20}, std::string(line) + "…", style);
                    } else {
                        ui.label({tx, y, w, 20}, line, style);
                    }
                    y += 20;
                    break;
                }
                ui.label({tx, y, w, 20}, line, style);
                y += 20;
            }
        }
        std::inplace_vector<std::pair<std::string_view, std::string>, 6> facts;
        if (!tagged) {
            for (auto &badge : badges) {
                facts.emplace_back(badge.icon, std::move(badge.text));
            }
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
            y += 2;
            const auto style = t.style(type::caption, t.faint());
            float x = tx;
            for (const auto &[icon, text] : facts) {
                const float w = std::ceil(p.measure(text, style)) + 19;
                if (x > tx && x + w > r.right()) {
                    x = tx;
                    y += 19;
                }
                p.icon(icon, {x, y + 3, 13, 13}, t.faint());
                if (const float label_w = std::min(w - 18, r.right() - (x + 18)); label_w > 0) {
                    ui.label({x + 18, y, label_w, 19}, text, style);
                }
                x += w + 16;
            }
            y += 19;
        }

        // The counts close the head across its full width, aligned with the portrait above and the tab strip below.
        float bottom = std::max(y, face.bottom());
        if (!counts.empty()) {
            const float row_y = bottom + (compact ? 12.f : 14.f);
            bottom = row_y + stats(ui, {counts_x, row_y, r.right() - counts_x - (at_foot ? actions_w + 24 : 0), 0}, counts, page);
            if (at_foot && browser_.route().serial == visit_) {
                actions(row_y);
            }
        }
        return bottom + 16 - r.y;
    }

    auto ProfileView::stat_list(ui::Ui &ui, const Listener &page, const Profile &who, bool self) -> Stats {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &caps = provider(page->who).capabilities;
        const auto &lib = library_.shelf(page->who.source);
        // Social circle, posts, listening volume, then what of the library there is room left for.
        Stats all;
        if (caps.people || who.follows >= 0) {
            all.push_back({lyren::tr_ctx("nav", "Following"), social_.count(*page, false), caps.people ? std::optional(Section::people) : std::nullopt, 0, std::nullopt});
        }
        if (caps.people || who.fans >= 0) {
            all.push_back({lyren::tr("Followers"), social_.count(*page, true), caps.people ? std::optional(Section::people) : std::nullopt, 1, std::nullopt});
        }
        if (caps.posts) {
            all.push_back({lyren::tr("Posts"), who.posts, Section::posts, 0, std::nullopt});
        }
        if (who.listened >= 0) {
            all.push_back({lyren::tr("Songs played"), who.listened, caps.ranking ? std::optional(Section::chart) : std::nullopt, -1, std::nullopt});
        }
        if (self) {
            if (all.size() < all.capacity() && caps.likes) {
                const int liked = lib.liked.count > 0 ? lib.liked.count : lib.playlists_loaded ? static_cast<int>(lib.liked_ids.size()) : -1;
                all.push_back({lyren::tr("Liked songs"), liked, std::nullopt, -1, Page::liked});
            }
            if (all.size() < all.capacity() && caps.library) {
                const int lists = lib.playlists_loaded ? static_cast<int>(lib.playlists.size()) + (!lib.liked.id.empty() ? 1 : 0) : -1;
                all.push_back({lyren::tr("Playlists"), lists, Section::playlists, -1, std::nullopt});
            }
            if (all.size() < all.capacity() && (caps.collect_albums || caps.follow_artists)) {
                const bool ready = lib.collection[0].loaded && lib.collection[1].loaded;
                const int saved = ready ? static_cast<int>(lib.collection[0].cards.size() + lib.collection[1].cards.size()) : -1;
                all.push_back({lyren::tr("Collection"), saved, Section::collection, -1, std::nullopt});
            }
        } else if (all.size() < all.capacity() && caps.profile_playlists && (who.playlists >= 0 || page->playlists.loaded)) {
            const int lists = who.playlists >= 0 ? who.playlists : static_cast<int>(page->playlists.cards.size());
            all.push_back({lyren::tr("Playlists"), lists, Section::playlists, -1, std::nullopt});
        }
        // One count alone says too little to stand as a row.
        if (all.size() < 2) {
            all.clear();
        }
        const auto number = t.style(type::heading, {}, text::semibold | text::tabular_nums), label = t.style(type::label, {});
        for (Stat &stat : all) {
            stat.number_w = std::ceil(p.measure(stat.value < 0 ? std::string("—") : stat_number(stat.value), number)) + 1;
            stat.w = stat_pad + stat.number_w + 6 + std::ceil(p.measure(stat.label, label)) + 1 + stat_pad;
        }
        return all;
    }

    float ProfileView::stats(ui::Ui &ui, Rect r, std::span<const Stat> all, const Listener &page) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        constexpr float h = control::medium;
        // Each count sits with its name, as wide as the two; the row breaks where the next would not fit.
        float x = r.x, y = r.y;
        for (size_t i = 0; i < all.size(); ++i) {
            const Stat &stat = all[i];
            const ui::Id id = ui::sub_id(ui.id("profile-stat"), i);
            if (x > r.x && x + stat.w > r.right()) {
                x = r.x;
                y += h + 2;
            }
            const Rect slot{x, y, std::min(stat.w, std::max(0.f, r.right() - x)), h};
            float hot = 0;
            if (stat.opens || stat.opens_page) {
                const Rect hit = slot.inset(0, 2);
                const auto it = ui.interact(id, hit);
                const auto feel = ui.feel(id, it, pace::quick);
                hot = feel.hot;
                if (hot > 0) {
                    p.rect(hit.inset(feel.down), t.fill(1.15f * hot), t.round(hit.h));
                }
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    if (stat.opens) {
                        if (*stat.opens == Section::posts && stat.tab >= 0) {
                            page->posts_tab = stat.tab;
                        } else if (*stat.opens == Section::people && stat.tab >= 0) {
                            page->people_tab = stat.tab;
                        }
                        show(ui, page, *stat.opens);
                    } else if (stat.opens_page) {
                        browser_.open(*stat.opens_page);
                        break;
                    }
                }
            }
            const Rect value{slot.x + stat_pad, y, std::min(stat.number_w, std::max(0.f, slot.w - stat_pad)), h};
            const auto number = t.style(type::heading, t.text.mix(t.accent, 0.4f * hot), text::semibold | text::tabular_nums);
            if (stat.value < 0) {
                ui.label(value, "—", number);
            } else {
                rolling_count(ui, ui::sub_id(id, 0xc7), value, stat.value, stat_number, number);
            }
            // A pixel down, so the smaller name stands on the number's baseline.
            if (const float label_w = slot.right() - stat_pad - value.right() - 6; label_w > 0) {
                ui.label({value.right() + 6, y + 1, label_w, h}, stat.label, t.style(type::label, t.text_dim.mix(t.text, 0.55f * hot)));
            }
            x += stat.w + stat_gap;
        }
        return y + h - r.y;
    }

    void ProfileView::tabs(ui::Ui &ui, Rect r, const Listener &page, const Profile &who, float pinned) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        struct Tab {
            std::string_view label, icon;
        };
        // In the order of Section.
        static constexpr std::array<Tab, 6> all{
            {{"Overview", "user"}, {"Posts", "message-square"}, {"Most played", "headphones"}, {"Playlists", "list-music"}, {"Collection", "star"}, {"People", "users"}}};
        const auto sections = social_.sections(page->who);
        std::inplace_vector<std::string_view, all.size()> labels;
        const auto style = t.style(type::lead, {}, text::medium);
        float needed = 28 * static_cast<float>(sections.size() - 1);
        for (const Section part : sections) {
            const auto &tab = all[static_cast<size_t>(part)];
            labels.push_back(lyren::tr(tab.label));
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
            if (ui.icon_button(ui::sub_id(ui.id("profile-tabs"), i), ui::center(cell, std::min(w, 36.f), 36), all[static_cast<size_t>(sections[i])].icon, 18,
                               static_cast<int>(i) == selected, labels[i])) {
                show(ui, page, sections[i]);
            }
        }
        const float x = ui.spring(ui.id("profile-tab-mark"), static_cast<float>(selected) * w + (w - 24) / 2, pace::glide);
        p.rect({r.x + x, r.bottom() - 2, 24, 2}, t.accent, 1);
    }

    float ProfileView::section(ui::Ui &ui, Rect r, float rail_budget_h, const Listener &page, bool self) {
        switch (page->section) {
            case Section::overview:
                return overview(ui, r, rail_budget_h, page);
            case Section::posts:
                return posts(ui, r, rail_budget_h, page, self);
            case Section::chart:
                return chart(ui, r, page);
            case Section::playlists:
                return playlists(ui, r, page, self);
            case Section::collection:
                return collection(ui, r, page);
            case Section::people:
                return people(ui, r, page, self);
        }
        return 0;
    }

    ProfileView::RailPlan ProfileView::plan_rail(float budget_h, const Listener &page) {
        RailPlan plan;
        const auto &caps = provider(page->who).capabilities;
        const auto &follows = page->people[0];
        const bool has_ranking = caps.ranking;
        const bool has_faces = caps.people && (follows.loading || !follows.items.empty());

        constexpr float repeat_base = 58, repeat_row_h = 54, card_gap = 16;
        constexpr float follow_head_h = 46, follow_pad = 12, person_h = 48, avatar_body_h = 62;
        constexpr float avatar_card_h = follow_head_h + avatar_body_h;

        if (has_faces) {
            const float min_for_any = has_ranking ? (repeat_base + repeat_row_h + card_gap + avatar_card_h) : avatar_card_h;
            plan.show_follows = budget_h >= min_for_any;
        }
        if (plan.show_follows) {
            if (follows.items.empty() || follows.items.size() == 1) {
                plan.follows_list = true;
            } else {
                const float list_threshold =
                    has_ranking ? (repeat_base + 2 * repeat_row_h + card_gap + follow_head_h + follow_pad + 2 * person_h) : (follow_head_h + follow_pad + 2 * person_h);
                if (rail_follow_rows_ < 0) {
                    plan.follows_list = budget_h >= list_threshold;
                } else if (rail_follow_rows_ == 0) {
                    plan.follows_list = budget_h >= list_threshold + 8.f;
                } else {
                    plan.follows_list = budget_h >= list_threshold - 8.f;
                }
            }
        }

        if (has_ranking) {
            float reserve = 0;
            if (plan.show_follows) {
                if (plan.follows_list) {
                    const size_t min_rows = follows.items.empty() ? size_t{2} : std::min<size_t>(follows.items.size(), 2);
                    reserve = card_gap + follow_head_h + follow_pad + static_cast<float>(min_rows) * person_h;
                } else {
                    reserve = card_gap + avatar_card_h;
                }
            }
            const float avail = budget_h - reserve;
            int rows = std::clamp(static_cast<int>((avail - repeat_base) / repeat_row_h), 1, 5);
            if (rail_repeat_rows_ > 0 && rows != rail_repeat_rows_) {
                if (rows > rail_repeat_rows_ && avail < repeat_base + static_cast<float>(rail_repeat_rows_ + 1) * repeat_row_h + 4.f) {
                    rows = rail_repeat_rows_;
                } else if (rows < rail_repeat_rows_ && avail >= repeat_base + static_cast<float>(rail_repeat_rows_) * repeat_row_h - 4.f) {
                    rows = rail_repeat_rows_;
                }
            }
            rail_repeat_rows_ = rows;
            plan.repeat_rows = static_cast<size_t>(rows);
            plan.repeat_h = repeat_height(page, plan.repeat_rows);
            plan.total_h = plan.repeat_h;
        }

        if (plan.show_follows) {
            const float rem = budget_h - (has_ranking ? plan.repeat_h + card_gap : 0.f);
            if (!plan.follows_list) {
                rail_follow_rows_ = 0;
                plan.follow_rows = 0;
                plan.follows_body_h = avatar_body_h;
            } else if (follows.items.empty()) {
                const int fit = std::clamp(static_cast<int>((rem - follow_head_h - follow_pad) / person_h), 2, 3);
                plan.follow_rows = static_cast<size_t>(fit);
                plan.follows_body_h = static_cast<float>(plan.follow_rows) * person_h + follow_pad;
            } else {
                const int max_people = static_cast<int>(std::min<size_t>(follows.items.size(), 4));
                const int min_people = std::min(2, max_people);
                int fit = std::clamp(static_cast<int>((rem - follow_head_h - follow_pad) / person_h), min_people, max_people);
                if (rail_follow_rows_ >= 1 && fit != rail_follow_rows_) {
                    const int prev = std::clamp(rail_follow_rows_, min_people, max_people);
                    if (fit > prev && rem < follow_head_h + follow_pad + static_cast<float>(prev + 1) * person_h + 4.f) {
                        fit = prev;
                    } else if (fit < prev && rem >= follow_head_h + follow_pad + static_cast<float>(prev) * person_h - 4.f) {
                        fit = prev;
                    }
                }
                rail_follow_rows_ = fit;
                plan.follow_rows = static_cast<size_t>(fit);
                plan.follows_body_h = static_cast<float>(plan.follow_rows) * person_h + follow_pad;
            }
            plan.total_h = (has_ranking ? plan.repeat_h + card_gap : 0.f) + follow_head_h + plan.follows_body_h + 8.f;
        }
        return plan;
    }

    float ProfileView::overview(ui::Ui &ui, Rect r, float rail_budget_h, const Listener &page) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &prov = provider(card);
        const auto &caps = prov.capabilities;
        auto &lib = library_.shelf(card.source);
        const ui::Color card_fill = studio_card_fill(t);
        const float radius = studio_card_radius(t);

        constexpr float rail_bp = 540 + rail_gap + rail_min;
        if (r.w >= rail_bp + 12) {
            wide_ = true;
        } else if (r.w < rail_bp - 12) {
            wide_ = false;
        }
        const bool has_rail = wide_ && (caps.ranking || caps.people);
        const float rail_k = ui.spring(ui.id("profile-rail-k"), has_rail ? 1.f : 0.f, 300);
        const float rw = std::clamp(r.w - PostFeed::column - rail_gap, rail_min, rail_max);
        const float wide_col_w = std::min(std::max(280.f, r.w - rw - rail_gap), PostFeed::column);
        const float col_w = std::lerp(r.w, wide_col_w, rail_k);

        const Rect clip = ui.clip();
        const float side_y = std::max(r.y, clip.y + section_gap);
        const RailPlan plan = plan_rail(rail_budget_h, page);
        const Rect side{r.right() - rw, side_y, rw, rail_budget_h};
        const float wide_repeat_h = caps.ranking ? ui.spring(ui.id("rail-repeat-h"), plan.repeat_h, 240) : 0.f;
        const float inline_repeat_h = caps.ranking ? ui.spring(ui.id("inline-repeat-h"), repeat_height(page, 4), 240) : 0.f;
        const float settled_inline_repeat_h = caps.ranking ? repeat_height(page, 4) : 0.f;

        float y = r.y;
        bool drew_any = false;

        // 1. Compact quick-access shortcut strip tailored to this platform's features.
        const bool cloud_recent = bool(prov.features.recent_plays) && (page->recent.loading || !page->recent.tracks.empty());
        const auto &recent_tracks = cloud_recent ? page->recent.tracks : playback_.history(card.source);
        {
            struct Shortcut {
                std::string_view id, icon, label;
                Page target;
            };
            std::inplace_vector<Shortcut, 5> shortcuts;
            if (caps.likes) {
                shortcuts.push_back({"quick-liked", "heart-fill", lyren::tr("Liked songs"), Page::liked});
            }
            if (prov.features.daily.songs) {
                shortcuts.push_back({"quick-daily", "disc-3", lyren::tr("Daily mix"), Page::daily});
            }
            if (prov.features.radar) {
                shortcuts.push_back({"quick-radar", "sparkles", lyren::tr("Radar"), Page::radar});
            }
            if (caps.radio) {
                shortcuts.push_back({"quick-fm", "radio", lyren::tr(prov.radio_title), Page::fm});
            }
            if (prov.features.cloud && shortcuts.size() < 4) {
                shortcuts.push_back({"quick-cloud", "cloud", lyren::tr("My cloud"), Page::cloud});
            }
            if (shortcuts.size() < 2) {
                shortcuts.push_back({"quick-recent", "history", lyren::tr("Recently played"), Page::recent});
            }
            if (!shortcuts.empty()) {
                constexpr float chip_h = 34, gap = 8;
                float cx = r.x, cy = y;
                std::optional<Page> open_page;
                for (const auto &s : shortcuts) {
                    const float cw = std::min(col_w, ui.chip_width(s.label, true));
                    if (cx > r.x && cx + cw > r.x + col_w) {
                        cx = r.x;
                        cy += chip_h + gap;
                    }
                    const Rect pill{cx, cy, cw, chip_h};
                    const ui::Id id = ui.id(s.id);
                    const auto it = ui.interact(id, pill);
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    const auto [hot, down] = ui.feel(id, it, pace::quick);
                    const Rect face = pill.inset(down);
                    p.rect(face, card_fill.mix(t.fill(1.45f), hot), face.h / 2);
                    p.icon(s.icon, {face.x + 12, face.y + (face.h - 14) / 2, 14, 14}, t.text_dim.mix(t.accent, 0.35f + 0.65f * hot));
                    ui.label({face.x + 31, face.y, std::max(0.f, face.w - 43), face.h}, s.label, t.style(type::label, t.text_dim.mix(t.text, 0.65f + 0.35f * hot), text::medium));
                    if (it.clicked) {
                        open_page = s.target;
                    }
                    cx += cw + gap;
                }
                y = cy + chip_h + 18;
                drew_any = true;
                if (open_page) {
                    if (*open_page == Page::recent) {
                        lib.recent_tab = cloud_recent ? 1 : 0;
                    }
                    browser_.open(*open_page);
                }
            }
        }

        // 2. Reserve smoothly animated inline space for "On repeat" as the rail collapses; the card itself is drawn
        // last through repeat_slot_ so it physically glides between the right rail and this slot across window resizes.
        const float inline_repeat_y = y;
        float settled_extra = 0;
        if (caps.ranking) {
            const float inline_slot = (inline_repeat_h + 20.f) * (1 - rail_k);
            const float settled_slot = has_rail ? 0.f : (settled_inline_repeat_h + 20.f);
            y += inline_slot;
            settled_extra = settled_slot - inline_slot;
            drew_any = true;
        }

        // 3. Recently played songs (cloud when the service exposes it, otherwise this device's history).
        if ((cloud_recent && page->recent.loading && recent_tracks.empty()) || !recent_tracks.empty()) {
            constexpr float head_h = 46, row_h = 50;
            const size_t shown = recent_tracks.empty() ? 3 : std::min<size_t>(recent_tracks.size(), 4);
            const Rect box{r.x, y, col_w, head_h + static_cast<float>(shown) * row_h + 10};
            p.rect(box, card_fill, radius);
            {
                const auto style = t.style(type::label, t.text, text::semibold);
                const auto title = lyren::tr("Recently played");
                const float w = std::ceil(p.measure(title, style)) + 1;
                ui.label({box.x + 16, box.y + 12, w, 24}, title, style);
                float right = box.right() - 10;
                if (ui.icon_button(ui.id("overview-recent-all"), {right - 28, box.y + 9, 28, 28}, "chevron-right", 16, false, lyren::tr("See all"))) {
                    lib.recent_tab = cloud_recent ? 1 : 0;
                    browser_.open(Page::recent);
                }
                right -= 32;
                if (!recent_tracks.empty() && box.w >= 340) {
                    const auto play_lbl = lyren::tr("Play all");
                    const float pw = ui.button_width(play_lbl, true);
                    if (ui.button(ui.id("overview-recent-play"), {right - pw, box.y + 9, pw, control::small}, play_lbl, ui::ButtonKind::tonal, "play-fill")) {
                        playback_.play(recent_tracks, 0);
                    }
                    right -= pw + 10;
                }
                ui.label({box.x + 16 + w + 8, box.y + 12, std::max(0.f, right - (box.x + 16 + w + 8)), 24}, lyren::tr(cloud_recent ? "Cloud" : "This device"),
                         t.style(type::caption, t.faint()));
                p.rect({box.x + 16, box.y + head_h - 1, box.w - 32, 1}, t.line(0.7f));
            }
            float ry = box.y + head_h + 5;
            if (recent_tracks.empty()) {
                for (size_t i = 0; i < shown; ++i) {
                    const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f);
                    ui.skeleton({box.x + 16, ry + 6, 38, 38}, t.radius_s());
                    ui.skeleton({box.x + 66, ry + 12, (box.w - 86) * (0.45f + 0.35f * k), 10}, 5);
                    ui.skeleton({box.x + 66, ry + 30, (box.w - 86) * (0.28f + 0.25f * (1 - k)), 9}, 4.5f);
                    ry += row_h;
                }
            } else {
                const bool show_time = box.w >= 420;
                const double since = std::max(page->section_since, cloud_recent ? page->recent.since : page->since);
                ui.push_id(ui.id("overview-recent"));
                for (size_t i = 0; i < shown; ++i) {
                    const Track &track = recent_tracks[i];
                    const float in = ui.cascade(since, static_cast<int>(i));
                    Fade fade(p, in);
                    const Rect row{box.x + 8, ry + (1 - in) * 8, box.w - 16, row_h};
                    const ui::Id id = item_id(ui, track);
                    const auto it = ui.interact(id, row);
                    const auto [hot, down] = ui.feel(id, it, pace::quick);
                    if (hot > 0) {
                        p.rect(row.inset(down, 2 + down), t.fill(hot), t.radius_s());
                    }
                    const Rect art{row.x + 8, row.y + 6, 38, 38};
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
                    const float time_w = show_time && track.duration > 0 ? 48.f : 0.f;
                    const float tx = art.right() + 12, tw = std::max(0.f, row.right() - tx - time_w - 12);
                    p.text(tx, row.y + 22, track.title, t.style(type::label, on ? t.accent : t.text, text::medium), tw);
                    p.text(tx, row.y + 40, track.artist, t.style(type::caption, t.text_dim), tw);
                    if (time_w > 0) {
                        ui.label({row.right() - time_w - 8, row.y, time_w, row.h}, mmss(track.duration), t.style(type::caption, t.faint(), text::tabular_nums), Align::right);
                    }
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    if (it.clicked) {
                        if (on) {
                            playback_.toggle();
                        } else {
                            playback_.play(recent_tracks, static_cast<int>(i));
                        }
                    }
                    if (it.right_clicked) {
                        shell_.tracks.menu(ui, it.px, it.py, recent_tracks, static_cast<int>(i));
                    }
                    ry += row_h;
                }
                ui.pop_id();
            }
            y = box.bottom() + 22;
            drew_any = true;
        }

        // Zero-copy one-row shelf preview (Playlists, Saved albums, Followed artists) with a "See all" action.
        auto shelf_row = [&](std::string_view key, std::string_view title, std::span<const Card> all_cards, size_t total_count, bool round, double since, auto on_all) {
            if (all_cards.empty()) {
                return;
            }
            const float min_w = round ? 130.f : 150.f;
            const auto limit = static_cast<size_t>(CardGrid::columns(col_w, min_w));
            const auto slice = all_cards.first(std::min(all_cards.size(), limit));
            section_heading(ui, r.x, y, col_w - 36, title, std::to_string(total_count));
            if (ui.icon_button(ui::sub_id(ui.id(key), 0xaa), {r.x + col_w - 28, y - 1, 28, 28}, "chevron-right", 16, false, lyren::tr("See all"))) {
                on_all();
            }
            y += 38;
            if (const int i = card_grid(ui, {r.x, y, col_w, 0}, ui.id(key), slice, round, since); i >= 0) {
                browser_.open(slice[static_cast<size_t>(i)]);
            }
            y += CardGrid::settled_height(col_w, slice.size(), min_w) + 24;
            drew_any = true;
        };

        // 4. Playlists preview (owned first, then saved; bounded to one row so we never copy the whole library).
        if (caps.library) {
            ensure_own_playlists(card.source);
            const auto limit = std::min<size_t>(static_cast<size_t>(CardGrid::columns(col_w, 150.f)), 8);
            const auto preview = std::span<const Card>(own_preview_).first(std::min(own_preview_.size(), limit));
            const size_t total_lists = lib.playlists.empty() ? preview.size() : lib.playlists.size();
            if (!preview.empty()) {
                shelf_row("overview-playlists", lyren::tr("Playlists"), preview, total_lists, false, page->section_since, [&] {
                    show(ui, page, Section::playlists);
                });
            } else if (!lib.playlists_loaded && lib.error.empty()) {
                section_heading(ui, r.x, y, col_w, lyren::tr("Playlists"));
                y += 38;
                const float h = CardGrid::settled_height(col_w, static_cast<size_t>(CardGrid::columns(col_w, 150)), 150);
                page_status(ui, {r.x, y, col_w, h}, true, "", false, Placeholder::cards);
                y += h + 24;
                drew_any = true;
            }
        }

        // 5. Collection previews: saved albums and followed artists, each a single tidy row when present.
        if (caps.collect_albums && !lib.collection[0].cards.empty()) {
            shelf_row("overview-albums", lyren::tr("Albums"), lib.collection[0].cards, lib.collection[0].cards.size(), false,
                      std::max(page->section_since, lib.collection[0].since), [&] {
                          page->collection_tab = 0;
                          show(ui, page, Section::collection);
                      });
        }
        if (caps.follow_artists && !lib.collection[1].cards.empty()) {
            shelf_row("overview-artists", lyren::tr("Artists"), lib.collection[1].cards, lib.collection[1].cards.size(), true,
                      std::max(page->section_since, lib.collection[1].since), [&] {
                          page->collection_tab = 1;
                          show(ui, page, Section::collection);
                      });
        }

        // Right studio rail (Following card): fades & slides cleanly as the rail appears or collapses.
        if (rail_k > 0.01f) {
            Fade fade(p, rail_k);
            ui::Enabled live(ui, has_rail, 1);
            const Rect shifted{side.x + (1 - rail_k) * 18.f, side.y, side.w, side.h};
            rail(ui, shifted, page, plan, false, wide_repeat_h);
        }

        // Shared-element "On repeat" card drawn last so it glides smoothly above the reflowing layout.
        if (caps.ranking) {
            const Rect target = has_rail ? Rect{side.x, side.y, side.w, wide_repeat_h} : Rect{r.x, inline_repeat_y, col_w, inline_repeat_h};
            const Rect flight = repeat_slot_.resolve(ui, target, has_rail ? 1 : 0, 340);
            const bool flying = repeat_slot_.morphing(ui, 340);
            if (flying) {
                p.shadow({flight.x, flight.y + 8, flight.w, flight.h}, studio_card_radius(t), 24, t.shadow.with_alpha(0.35f));
            }
            const auto visible_rows = static_cast<size_t>(std::clamp(static_cast<int>(std::ceil((flight.h - 48.f) / 54.f)), 1, 5));
            const size_t rows = std::max(has_rail ? plan.repeat_rows : size_t{4}, visible_rows);
            repeat_card(ui, flight, page, rows);
        }

        const float beside = has_rail ? plan.total_h : 0.f;
        if (!drew_any && beside <= 0) {
            const Rect room{r.x, y, col_w, 240};
            empty(ui, room, "music", lyren::tr("Nothing here yet"));
            y += room.h;
        }
        return std::max(y - r.y + settled_extra, beside);
    }

    float ProfileView::posts(ui::Ui &ui, Rect r, float rail_budget_h, const Listener &page, bool self) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &caps = provider(card).capabilities;
        constexpr float rail_bp = 540 + rail_gap + rail_min;
        if (r.w >= rail_bp + 12) {
            wide_ = true;
        } else if (r.w < rail_bp - 12) {
            wide_ = false;
        }
        const bool has_rail = wide_ && (caps.ranking || caps.people);
        const float rail_k = ui.spring(ui.id("profile-rail-k"), has_rail ? 1.f : 0.f, 300);
        const float rw = std::clamp(r.w - PostFeed::column - rail_gap, rail_min, rail_max);
        const float wide_col_w = std::min(std::max(280.f, r.w - rw - rail_gap), PostFeed::column);
        const float feed_w = std::lerp(std::min(r.w, PostFeed::column), wide_col_w, rail_k);

        const RailPlan plan = plan_rail(rail_budget_h, page);
        if (rail_k > 0.01f) {
            // The framed studio rail stays in sight while the timeline scrolls past it.
            const Rect clip = ui.clip();
            const float sy = std::max(r.y, clip.y + section_gap);
            const Rect side{r.right() - rw + (1 - rail_k) * 18.f, sy, rw, rail_budget_h};
            Fade fade(p, rail_k);
            ui::Enabled live(ui, has_rail, 1);
            rail(ui, side, page, plan, true);
        }
        float y = r.y;
        // The account's own page also holds the feed of the people it follows.
        const bool feed = self && page->posts_tab == 1;
        if (self) {
            const std::array<std::string_view, 2> labels{lyren::tr("My posts"), lyren::tr("Following feed")};
            int tab = page->posts_tab;
            if (ui.segmented(ui.id("profile-feed"), {r.x, y, std::min(280.f, feed_w - 48), control::medium}, labels, tab)) {
                page->posts_tab = tab;
                show(ui, page, Section::posts);
            }
            const Rect again{r.x + feed_w - control::medium, y, control::medium, control::medium};
            if (const auto &list = feed ? social_.feed(card.source) : page->posts; list->loading) {
                ui.spinner(ui::center(again, 16, 16), t.text_dim, 2);
            } else if (ui.icon_button(ui.id("profile-refresh"), again, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
                social_.reload_posts(page, feed);
            }
            y += control::medium + 18;
            if (caps.post_write) {
                y += shell_.feed.prompt(ui, {r.x, y, feed_w, 0}, card.source) + 6;
            }
        }
        const auto list = feed ? social_.feed(card.source) : page->posts;
        y += shell_.feed.draw(ui, {r.x, y, feed_w, 0}, list, card.source, feed ? std::string() : card.id, feed);
        return std::max(y - r.y, has_rail ? plan.total_h : 0.f);
    }

    float ProfileView::repeat_height(const Listener &page, size_t max_rows) const {
        const auto &chart = page->charts[static_cast<size_t>(page->chart_tab)];
        const auto &tracks = chart.ranking.tracks;
        constexpr float head_h = 46, row_h = 54;
        const size_t shown = !chart.loaded ? std::min<size_t>(max_rows, 3) : std::min(tracks.size(), max_rows);
        const float body_h = !chart.loaded ? static_cast<float>(shown) * row_h + 12 : tracks.empty() ? 52.f : static_cast<float>(shown) * row_h + 12;
        return head_h + body_h;
    }

    float ProfileView::repeat_card(ui::Ui &ui, Rect r, const Listener &page, size_t max_rows) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &chart = page->charts[static_cast<size_t>(page->chart_tab)];
        auto &tracks = chart.ranking.tracks;
        constexpr float head_h = 46, row_h = 54;
        const size_t shown = !chart.loaded ? std::min<size_t>(max_rows, 3) : std::min(tracks.size(), max_rows);
        const float natural_h = repeat_height(page, max_rows);
        const Rect box{r.x, r.y, r.w, r.h > 0 ? r.h : natural_h};
        p.rect(box, studio_card_fill(t), studio_card_radius(t));
        ui.push_clip(box);
        {
            const auto style = t.style(type::label, t.text, text::semibold);
            const auto title = lyren::tr("On repeat");
            const float w = std::ceil(p.measure(title, style)) + 1;
            ui.label({box.x + 16, box.y + 12, w, 24}, title, style);
            ui.label({box.x + 16 + w + 8, box.y + 12, std::max(0.f, box.w - w - 64), 24}, lyren::tr(page->chart_tab ? "All time" : "This week"),
                     t.style(type::caption, t.faint(), text::tabular_nums));
            if (ui.icon_button(ui.id("rail-chart"), {box.right() - 36, box.y + 10, 28, 28}, "chevron-right", 16, false, lyren::tr("See all"))) {
                show(ui, page, Section::chart);
            }
            p.rect({box.x + 16, box.y + 45, box.w - 32, 1}, t.line(0.7f));
        }

        float ry = box.y + head_h + 6;
        if (!chart.loaded) {
            for (size_t i = 0; i < shown; ++i) {
                const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f);
                ui.skeleton({box.x + 16, ry + 9, 38, 38}, t.radius_s());
                ui.skeleton({box.x + 66, ry + 15, (box.w - 86) * (0.5f + 0.4f * k), 10}, 5);
                ui.skeleton({box.x + 66, ry + 33, (box.w - 86) * (0.3f + 0.3f * (1 - k)), 9}, 4.5f);
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
                ui.label({row.x + 4, row.y, 18, row.h - 4}, std::to_string(i + 1),
                         t.style(type::caption, i < 3 ? t.accent.with_alpha(0.85f) : t.faint(), text::semibold | text::tabular_nums), Align::center);
                const Rect art{row.x + 26, row.y + 8, 38, 38};
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
        ui.pop_clip();
        return box.h;
    }

    float ProfileView::rail(ui::Ui &ui, Rect r, const Listener &page, const RailPlan &plan, bool include_repeat, float repeat_h_override) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &caps = provider(card).capabilities;
        const auto &follows = page->people[0];
        const float radius = studio_card_radius(t);
        const ui::Color card_fill = studio_card_fill(t);
        float y = r.y;

        if (caps.ranking) {
            const float anim_repeat_h = repeat_h_override >= 0.f ? repeat_h_override : ui.spring(ui.id("rail-repeat-h"), plan.repeat_h, 240);
            if (include_repeat) {
                const auto visible_rows = static_cast<size_t>(std::clamp(static_cast<int>(std::ceil((anim_repeat_h - 48.f) / 54.f)), 1, 5));
                repeat_card(ui, {r.x, y, r.w, anim_repeat_h}, page, std::max(plan.repeat_rows, visible_rows));
            }
            y += anim_repeat_h + 16;
        }

        const float show_k = ui.anim(ui.id("rail-follow-show"), plan.show_follows ? 1.f : 0.f, pace::steady);
        if (show_k > 0.01f) {
            Fade show_fade(p, show_k);
            ui::Enabled show_live(ui, plan.show_follows, 1);
            const int count = social_.count(*page, false);
            constexpr float head_h = 46, person_h = 48;
            const float target_body_h = plan.follows_body_h > 0.f ? plan.follows_body_h : 62.f;
            const float body_h = ui.spring(ui.id("rail-follow-h"), target_body_h, 240);
            const Rect box{r.x, y + (1 - show_k) * 8.f, r.w, head_h + body_h};
            p.rect(box, card_fill, radius);
            ui.push_clip(box);
            {
                const auto style = t.style(type::label, t.text, text::semibold);
                const auto title = lyren::tr_ctx("nav", "Following");
                const float w = std::ceil(p.measure(title, style)) + 1;
                ui.label({box.x + 16, box.y + 12, w, 24}, title, style);
                if (count >= 0) {
                    ui.label({box.x + 16 + w + 8, box.y + 12, std::max(0.f, box.w - w - 64), 24}, stat_number(count), t.style(type::caption, t.faint(), text::tabular_nums));
                }
                if (ui.icon_button(ui.id("rail-people"), {box.right() - 36, box.y + 10, 28, 28}, "chevron-right", 16, false, lyren::tr("See all"))) {
                    page->people_tab = 0;
                    show(ui, page, Section::people);
                }
                p.rect({box.x + 16, box.y + 45, box.w - 32, 1}, t.line(0.7f));
            }

            ui.push_id(ui.id("rail-faces"));
            if (follows.items.empty()) {
                const size_t skel_rows = std::max<size_t>(plan.follow_rows, 2);
                float py = box.y + head_h + 6;
                for (size_t i = 0; i < skel_rows; ++i) {
                    const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f);
                    ui.skeleton({box.x + 16, py + 7, 34, 34}, 17);
                    ui.skeleton({box.x + 62, py + 12, (box.w - 82) * (0.42f + 0.35f * k), 10}, 5);
                    ui.skeleton({box.x + 62, py + 29, (box.w - 82) * (0.28f + 0.25f * (1 - k)), 9}, 4.5f);
                    py += person_h;
                }
            } else {
                const float list_k = ui.anim(ui.id("rail-follow-mode"), plan.follows_list ? 1.f : 0.f, pace::steady);
                const double since = std::max(page->section_since, follows.since);
                if (list_k > 0.01f) {
                    Fade mode_fade(p, list_k);
                    ui::Enabled mode_live(ui, plan.follows_list, 1);
                    const auto vis_rows = static_cast<size_t>(std::clamp(static_cast<int>(std::ceil((body_h - 6.f) / person_h)), 1, 4));
                    const size_t rows = std::min(follows.items.size(), std::max(plan.follow_rows, vis_rows));
                    float py = box.y + head_h + 6;
                    ui.push_id(ui.id("list"));
                    for (size_t i = 0; i < rows; ++i) {
                        const Person &who = follows.items[i];
                        const float in = ui.cascade(since, static_cast<int>(i));
                        Fade row_fade(p, in);
                        const Rect row{box.x + 8, py + (1 - in) * 6, box.w - 16, person_h};
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
                    ui.pop_id();
                }
                if (list_k < 0.99f) {
                    Fade mode_fade(p, 1.f - list_k);
                    ui::Enabled mode_live(ui, !plan.follows_list, 1);
                    constexpr float d = 36, gap = 8;
                    const auto n = std::min(follows.items.size(), static_cast<size_t>((box.w - 32 + gap) / (d + gap)));
                    ui.push_id(ui.id("strip"));
                    for (size_t i = 0; i < n; ++i) {
                        const Person &who = follows.items[i];
                        const float in = ui.cascade(since, static_cast<int>(i));
                        Fade av_fade(p, in);
                        const Rect face{box.x + 16 + static_cast<float>(i) * (d + gap), box.y + head_h + 13 + (1 - in) * 4, d, d};
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
                    ui.pop_id();
                }
            }
            ui.pop_id();
            ui.pop_clip();
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
        std::span<const Card> made, saved;
        double since = page->section_since;
        if (self) {
            // The account's own come from its library, the liked songs first.
            const auto &shelf = library_.shelf(card.source);
            ensure_own_playlists(card.source);
            made = own_made_;
            saved = own_saved_;
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
            const auto split = static_cast<size_t>(std::clamp(page->created, 0, static_cast<int>(listing.cards.size())));
            const std::span<const Card> all(listing.cards);
            made = all.first(split);
            saved = all.subspan(split);
            since = std::max(since, listing.since);
        }
        float y = r.y;
        std::optional<Card> picked;
        auto shelf = [&](std::string_view key, std::string_view title, std::span<const Card> cards) {
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

    float ProfileView::collection(ui::Ui &ui, Rect r, const Listener &page) {
        const auto &t = ui.theme();
        const Card &card = page->who;
        const auto &caps = provider(card).capabilities;
        const bool has_albums = caps.collect_albums, has_artists = caps.follow_artists;
        if (page->collection_tab == 0 && !has_albums && has_artists) {
            page->collection_tab = 1;
        } else if (page->collection_tab == 1 && !has_artists && has_albums) {
            page->collection_tab = 0;
        }
        const bool artists = page->collection_tab == 1;
        const auto &lib = library_.shelf(card.source);
        const Listing &res = lib.collection[static_cast<size_t>(page->collection_tab)];

        const Rect bar{r.x, r.y, r.w, control::medium};
        const Rect again{bar.right() - control::medium, bar.y, control::medium, control::medium};
        if (res.loading) {
            ui.spinner(ui::center(again, 16, 16), t.text_dim, 2);
        } else if (env_.signed_in(card.source) && ui.icon_button(ui.id("profile-collection-refresh"), again, "refresh-cw", 16, false, lyren::tr("Refresh"))) {
            spawn(library_.load_collection(card.source));
        }
        if (has_albums && has_artists) {
            const std::array<std::string_view, 2> labels{lyren::tr("Albums"), lyren::tr("Artists")};
            if (int tab = page->collection_tab; ui.segmented(ui.id("profile-collection-tabs"), {bar.x, bar.y, std::min(232.f, bar.w - control::medium - 16), bar.h}, labels, tab)) {
                page->collection_tab = tab;
                show(ui, page, Section::collection);
            }
        } else {
            section_heading(ui, bar.x, bar.y + 4, bar.w - control::medium - 16, lyren::tr(artists ? "Artists" : "Albums"),
                            res.cards.empty() ? std::string() : std::to_string(res.cards.size()));
        }

        const float top = bar.bottom() + 16;
        if (res.cards.empty()) {
            if (!res.loaded && !res.loading && res.error.empty() && env_.signed_in(card.source)) {
                spawn(library_.load_collection(card.source));
            }
            const Rect room{r.x, top, r.w, 300};
            if (!page_status(ui, room, res.loading || (!res.loaded && res.error.empty() && env_.signed_in(card.source)), res.error, !env_.signed_in(card.source),
                             artists ? Placeholder::faces : Placeholder::cards)) {
                empty(ui, room, artists ? "users" : "disc-album", lyren::tr(artists ? "No followed artists yet" : "No saved albums yet"));
            }
            return top - r.y + room.h;
        }
        const double since = std::max(page->section_since, res.since);
        if (const int i = card_grid(ui, {r.x, top, r.w, 0}, ui.id(artists ? "profile-artists" : "profile-albums"), res.cards, artists, since); i >= 0) {
            browser_.open(res.cards[static_cast<size_t>(i)]);
        }
        return top - r.y + CardGrid::settled_height(r.w, res.cards.size(), artists ? 130.f : 150.f) + 8;
    }

    float ProfileView::people(ui::Ui &ui, Rect r, const Listener &page, bool self) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &card = page->who;
        const Rect bar{r.x, r.y, r.w, control::medium};
        const std::array<std::string_view, 2> labels{lyren::tr_ctx("nav", "Following"), lyren::tr("Followers")};
        const float seg_w = std::min(232.f, bar.w);
        if (int tab = page->people_tab; ui.segmented(ui.id("profile-people-tabs"), {bar.x, bar.y, seg_w, bar.h}, labels, tab)) {
            page->people_tab = tab;
            show(ui, page, Section::people);
        }
        const bool fans = page->people_tab != 0;
        if (const int count = social_.count(*page, fans); count >= 0 && bar.w - seg_w > 140) {
            const std::string summary = fans ? lyren::trf("{} followers", stat_number(count)) : lyren::trf("Following {} people", stat_number(count));
            ui.label({bar.x + seg_w + 16, bar.y, bar.w - seg_w - 16, bar.h}, summary, t.style(type::caption, t.faint(), text::tabular_nums), Align::right);
        }

        const float top = bar.bottom() + 16;
        auto &list = page->people[fans];
        if (list.items.empty()) {
            if (list.open()) {
                social_.load_people(page, fans);
            }
            const Rect room{r.x, top, r.w, 300};
            if (!list.error.empty() && !env_.signed_in(card.source)) {
                // Some services show who follows whom only to those signed in.
                page_status(ui, room, false, "", true);
            } else if (!page_status(ui, room, list.loading || (list.more && list.error.empty()), list.error, false, Placeholder::faces)) {
                empty(ui, room, "users", lyren::tr(fans ? (self ? "No fans yet" : "No fans to show") : (self ? "You are not following anyone yet" : "Not following anyone yet")));
            }
            return top - r.y + room.h;
        }
        // In as many columns as fit, smoothly reflowing across column changes like CardGrid.
        constexpr float gap = 12, row_h = 72, step = row_h + gap;
        const auto cols = static_cast<size_t>(std::max(1, static_cast<int>((r.w + gap) / (320 + gap))));
        const float anim_cols = ui.spring(ui.id(fans ? "fans-cols" : "follows-cols"), static_cast<float>(cols), CardGrid::reflow_ms);
        const bool reflowing = std::abs(anim_cols - static_cast<float>(cols)) > 0.01f;
        const float cell_w = (r.w + gap) / anim_cols - gap;
        const size_t rows = (list.items.size() + cols - 1) / cols;
        const Rect clip = ui.clip();
        const double since = std::max(page->section_since, list.since);
        int shown = 0;
        ui.push_id(ui.id(fans ? "fans" : "follows"));
        if (reflowing) {
            for (size_t i = 0; i < list.items.size(); ++i) {
                const ui::Id pid = ui.id(list.items[i].id);
                const float c = ui.spring(ui::sub_id(pid, 0xc0), static_cast<float>(i % cols), CardGrid::reflow_ms);
                const float rw = ui.spring(ui::sub_id(pid, 0xc1), static_cast<float>(i / cols), CardGrid::reflow_ms);
                Rect cell{r.x + c * (cell_w + gap), top + rw * step, cell_w, row_h};
                if (cell.bottom() < clip.y || cell.y > clip.bottom()) {
                    continue;
                }
                const float in = ui.cascade(since, shown++);
                Fade fade(p, in);
                cell.y += (1 - in) * 10;
                person(ui, cell, card.source, list.items[i]);
            }
        } else if (rows > 0 && clip.bottom() >= top) {
            const auto first_row = static_cast<size_t>(std::max(0, static_cast<int>(std::floor((clip.y - top - 12.f) / step))));
            const auto last_row = std::min(rows - 1, static_cast<size_t>(std::max(0, static_cast<int>(std::floor((clip.bottom() - top) / step)))));
            if (first_row <= last_row) {
                const size_t end_i = std::min(list.items.size(), (last_row + 1) * cols);
                for (size_t i = first_row * cols; i < end_i; ++i) {
                    const ui::Id pid = ui.id(list.items[i].id);
                    const float c = ui.spring(ui::sub_id(pid, 0xc0), static_cast<float>(i % cols), CardGrid::reflow_ms);
                    const float rw = ui.spring(ui::sub_id(pid, 0xc1), static_cast<float>(i / cols), CardGrid::reflow_ms);
                    Rect cell{r.x + c * (cell_w + gap), top + rw * step, cell_w, row_h};
                    if (cell.bottom() < clip.y || cell.y > clip.bottom()) {
                        continue;
                    }
                    const float in = ui.cascade(since, shown++);
                    Fade fade(p, in);
                    cell.y += (1 - in) * 10;
                    person(ui, cell, card.source, list.items[i]);
                }
            }
        }
        ui.pop_id();
        const float end = top + static_cast<float>(rows) * (row_h + gap);
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
        p.rect(r, t.fill(0.7f + 0.8f * hot), t.radius);
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
