#include "app/view/feed.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <inplace_vector>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"
#include "core/utf8.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    namespace {
        // The measures of a post: the column its avatar or its date stands in, the room above and below it, the space
        // between its parts, and what it carries.
        namespace look {
            constexpr float gutter = 54, avatar = 40, pad = 18, gap = 12;
            constexpr float chip = 64, actions = 32;
            constexpr float picture_w = 400, picture_h = 300, thumb = 148;
        } // namespace look

        std::tm local(int64_t ms) {
            const auto at = static_cast<std::time_t>(ms / 1000);
            std::tm out{};
            localtime_r(&at, &out);
            return out;
        }

        // What a post says, in a line or two: its words, or the song it shares.
        std::string_view gist(const Post &post) {
            return post.text.empty() && post.track ? std::string_view(post.track->title) : std::string_view(post.text);
        }

        float action_width(ui::Ui &ui, std::string_view label) {
            const auto &t = ui.theme();
            return 32 + (label.empty() ? 0 : 6 + std::ceil(ui.painter().measure(label, t.style(type::caption, {}, text::tabular_nums))));
        }

        // An icon and a count as a quiet button; word stands in for a count of none, and scale swells the icon for a
        // like landing.
        bool action(ui::Ui &ui, ui::Id id, Rect r, std::string_view icon, int count, std::string_view word, ui::Color color, float scale = 1) {
            auto &p = ui.painter();
            const auto &t = ui.theme();
            const auto it = ui.interact(id, r);
            const auto [hot, down] = ui.feel(id, it, pace::quick);
            if (hot > 0) {
                p.rect(r.inset(down), t.fill(1.2f * hot), r.h / 2);
            }
            const ui::Color ink = color.mix(t.text, 0.5f * hot);
            const float size = 16 * scale;
            p.icon(icon, {r.x + 8 + (16 - size) / 2, r.y + (r.h - size) / 2, size, size}, ink);
            rolling_count(
                ui, ui::sub_id(id, 0xc7), {r.x + 30, r.y, r.w - 30, r.h}, count,
                [&](int n) {
                    return n > 0 ? short_number(n) : std::string(word);
                },
                t.style(type::caption, ink, text::tabular_nums));
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            return it.clicked;
        }

        // Pictures: one at its own shape, more as square thumbnails three to a row (two for two or four).
        std::inplace_vector<Rect, 9> picture_layout(const std::vector<Picture> &pictures, float x, float y, float w) {
            std::inplace_vector<Rect, 9> out;
            const size_t count = std::min<size_t>(pictures.size(), 9);
            if (count == 1) {
                const Picture &picture = pictures.front();
                const float aspect = std::clamp(picture.w > 0 && picture.h > 0 ? static_cast<float>(picture.w) / static_cast<float>(picture.h) : 4.f / 3, 0.56f, 2.2f);
                const float max_w = std::min(w, look::picture_w), max_h = look::picture_h;
                out.push_back(aspect >= max_w / max_h ? Rect{x, y, max_w, std::round(max_w / aspect)} : Rect{x, y, std::round(max_h * aspect), max_h});
                return out;
            }
            constexpr float space = 6;
            const size_t cols = count == 2 || count == 4 ? 2 : 3;
            const float size = std::floor(std::min(look::thumb, (w - space * static_cast<float>(cols - 1)) / static_cast<float>(cols)));
            for (size_t i = 0; i < count; ++i) {
                out.push_back({x + static_cast<float>(i % cols) * (size + space), y + static_cast<float>(i / cols) * (size + space), size, size});
            }
            return out;
        }

        // Posts on their way: the shape of a few of them.
        void skeleton(ui::Ui &ui, Rect r, bool byline) {
            float y = r.y + look::pad;
            for (int i = 0; i < 3; ++i) {
                const float k = std::fmod(static_cast<float>(i) * 0.618f, 1.f), x = r.x + look::gutter, w = r.w - look::gutter;
                ui.skeleton({r.x, y, look::avatar, look::avatar}, byline ? look::avatar / 2 : ui.theme().radius_s());
                ui.skeleton({x, y + 4, 120, 12}, 6);
                ui.skeleton({x, y + 26, 64, 10}, 5);
                ui.skeleton({x, y + 56, w * (0.7f + 0.25f * k), 12}, 6);
                ui.skeleton({x, y + 80, w * (0.4f + 0.3f * (1 - k)), 12}, 6);
                ui.skeleton({x, y + 108, std::min(w, 360.f), look::chip}, ui.theme().radius);
                y += 108 + look::chip + look::pad * 2;
            }
        }
    } // namespace

    void PostFeed::compose(ui::Ui &ui, Source source, std::optional<Track> track, std::optional<Post> forward) {
        if (!social_.compose(source, std::move(track), std::move(forward))) {
            return;
        }
        ui.open_modal(540, composer_height(), [this](ui::Ui &u, Rect r) {
            composer(u, r);
        });
    }

    // The composer fits what the post carries: the post passed on, the song, or the offer of the one playing.
    float PostFeed::composer_height() const {
        const auto &draft = social_.draft();
        const Track *playing = playback_.current();
        const float carried = draft.forward ? 76 : draft.track ? 64 : playing && playing->source == draft.source ? 44 : 0;
        return 48 + 64 + 120 + (carried > 0 ? 12 + carried : 0) + 20 + control::large;
    }

    void PostFeed::composer(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &draft = social_.draft();
        ui.resize_modal(540, composer_height());
        dialog_title(ui, r, lyren::tr(draft.forward ? "Repost" : "New post"), lyren::tr(platform(draft.source).name));
        const ui::Id field = ui.id("post-draft");
        if (std::exchange(draft.focus, false)) {
            ui.focus = field;
        }
        const Rect editor{r.x, r.y + 64, r.w, 120};
        ui.text_area(field, editor, draft.text, lyren::tr(draft.forward ? "Add your thoughts…" : "What are you listening to?"));
        const float y = editor.bottom() + 12;
        // What the post carries: the post passed on, the song, or the song playing now, offered.
        if (draft.forward) {
            const Post &quoted = *draft.forward;
            const Rect quote{r.x, y, r.w, 76};
            p.rect(quote, t.fill(), t.radius);
            ui.label({quote.x + 16, quote.y + 11, quote.w - 32, 20}, "@" + quoted.author.name, t.style(type::label, t.accent, text::medium));
            const auto lines = p.wrap_lines(gist(quoted), t.fs(type::label), quote.w - 32);
            for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i) {
                ui.label({quote.x + 16, quote.y + 32 + static_cast<float>(i) * 19, quote.w - 32, 19}, lines[i], t.style(type::label, t.text_dim));
            }
        } else if (draft.track) {
            const Rect chip{r.x, y, r.w, 64};
            p.rect(chip, t.fill(), t.radius);
            cover(ui, {chip.x + 8, chip.y + 8, 48, 48}, draft.track->cover, t.radius_s());
            p.text(chip.x + 68, chip.y + 28, draft.track->title, t.style(type::body, t.text, text::medium), chip.w - 116);
            p.text(chip.x + 68, chip.y + 47, draft.track->artist, t.style(type::caption, t.text_dim), chip.w - 116);
            if (ui.icon_button(ui.id("post-untrack"), {chip.right() - 42, chip.y + 16, 32, 32}, "x", 15, false, lyren::tr("Remove"))) {
                draft.track.reset();
            }
        } else if (const Track *playing = playback_.current(); playing && playing->source == draft.source) {
            const Rect chip{r.x, y, r.w, 44};
            const ui::Id id = ui.id("post-attach");
            const auto it = ui.interact(id, chip);
            const auto [hot, down] = ui.feel(id, it, pace::quick);
            p.rect(chip.inset(down), t.fill(0.6f + 0.8f * hot), t.radius);
            p.icon("plus", {chip.x + 14, chip.y + 14, 16, 16}, t.accent);
            p.text(chip.x + 40, chip.y + 27, lyren::trf("Attach the song playing: {} · {}", playing->title, playing->artist), t.style(type::label, t.text_dim.mix(t.text, hot)),
                   chip.w - 56);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.clicked) {
                draft.track = *playing;
            }
        }

        ui.label({r.x, r.bottom() - control::large, 120, control::large}, int_text(static_cast<int64_t>(utf8::count(draft.text))),
                 t.style(type::caption, t.faint(), text::tabular_nums));
        // A post needs words unless it shares a song or passes one on.
        const bool blank = draft.text.find_first_not_of(" \t\r\n") == std::string::npos;
        if (dialog_actions(ui, r, lyren::tr("Post"), !draft.sending && (!blank || draft.track || draft.forward), "send")) {
            ui.close_modal();
            spawn(social_.send());
        }
    }

    float PostFeed::prompt(ui::Ui &ui, Rect r, Source source) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        constexpr float h = 44;
        const auto &account = library_.shelf(source).account;
        const Rect face{r.x, r.y + (h - look::avatar) / 2, look::avatar, look::avatar};
        cover(ui, face, account ? account->avatar : std::string(), look::avatar / 2, "user");
        Rect field{r.x + look::gutter, r.y, r.w - look::gutter, h};
        // The song playing is one click from being shared.
        const Track *playing = playback_.current();
        const bool shareable = playing && playing->source == source;
        if (shareable) {
            const Rect share = ui::cut_right(field, h);
            ui::cut_right(field, 8);
            const ui::Id sid = ui.id("prompt-share");
            const auto sit = ui.interact(sid, share);
            if (sit.hovered) {
                ui.cursor = wl::Cursor::pointer;
                if (!sit.held) {
                    ui.tooltip(sid, share, lyren::tr("Share the song playing"));
                }
            }
            const auto [shot, sdown] = ui.feel(sid, sit, pace::quick);
            const Rect disc = share.inset(sdown);
            p.rect(disc, t.fill(0.95f + 0.7f * shot), disc.h / 2);
            p.icon("music", ui::center(disc, 17, 17), t.text_dim.mix(t.accent, shot));
            if (sit.clicked) {
                compose(ui, source, *playing);
            }
        }
        const ui::Id id = ui.id("prompt");
        const auto it = ui.interact(id, field);
        const auto [hot, down] = ui.feel(id, it, pace::quick);
        const Rect pill = field.inset(down);
        p.rect(pill, t.fill(0.9f + 0.7f * hot), t.round(pill.h));
        p.icon("square-pen", {pill.x + 16, pill.y + (pill.h - 16) / 2, 16, 16}, t.faint().mix(t.text_dim, hot));
        ui.label({pill.x + 42, pill.y, pill.w - 58, pill.h}, lyren::tr("What are you listening to?"), t.style(type::body, t.faint().mix(t.text_dim, hot)));
        if (it.hovered) {
            ui.cursor = wl::Cursor::text;
        }
        if (it.clicked) {
            compose(ui, source);
        }
        return h;
    }

    float PostFeed::attachments(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        float h = 0;
        // A song plays where it is, after the one playing; a playlist or an album opens.
        if (post.track || post.card) {
            if (draw) {
                const Rect chip{x, y, w, look::chip};
                const ui::Id id = ui.id(post.track ? "track" : "card");
                const auto it = ui.interact(id, chip);
                const auto [hot, down] = ui.feel(id, it, pace::quick);
                const Rect c = chip.inset(down);
                p.rect(c, t.fill(0.9f + 0.8f * hot), t.radius);
                const Rect art{c.x + 8, c.y + 8, 48, 48};
                cover(ui, art, post.track ? post.track->cover : post.card->cover, t.radius_s(),
                      post.track                           ? "music"
                      : post.card->kind == CardKind::album ? "disc-album"
                                                           : "list-music");
                const float tx = art.right() + 12, tw = c.right() - tx - 52;
                const bool on = post.track && playback_.is_current(*post.track);
                p.text(tx, c.y + 28, post.track ? post.track->title : post.card->title, t.style(type::body, on ? t.accent : t.text, text::medium), tw);
                std::string sub;
                if (post.track) {
                    sub = post.track->artist;
                } else {
                    sub = lyren::tr(post.card->kind == CardKind::album ? "Album" : "Playlist");
                    if (const auto more = card_subtitle(*post.card); !more.empty()) {
                        sub += " · " + more;
                    }
                }
                p.text(tx, c.y + 47, sub, t.style(type::caption, t.text_dim), tw);
                const Rect knob{c.right() - 44, c.y + (c.h - 32) / 2, 32, 32};
                if (post.track) {
                    p.rect(knob, t.accent.with_alpha(0.14f + 0.12f * hot), 16);
                    if (on && playback_.playing()) {
                        ui.equalizer(ui.id("eq"), ui::center(knob, 13, 13), t.accent, true);
                    } else {
                        Rect glyph = ui::center(knob, 12, 12);
                        glyph.x += 1;
                        p.icon("play-fill", glyph, t.accent);
                    }
                } else {
                    p.icon("chevron-right", ui::center(knob, 16, 16), t.text_dim.mix(t.text, hot));
                }
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    if (!post.track) {
                        browser_.open(*post.card);
                    } else if (on) {
                        playback_.toggle();
                    } else {
                        playback_.play_now(*post.track);
                    }
                }
            }
            h += look::chip + look::gap;
        } else if (!post.resource.empty()) {
            // Something the app cannot show: named, as the service words it.
            if (draw) {
                const auto style = t.style(type::label, t.text_dim);
                const Rect chip{x, y, std::min(w, p.measure(post.resource, style) + 48), control::medium};
                p.rect(chip, t.fill(), chip.h / 2);
                p.icon("link", {chip.x + 14, chip.y + (chip.h - 15) / 2, 15, 15}, t.text_dim);
                p.text(chip.x + 36, chip.y + 22, post.resource, style, chip.w - 48);
            }
            h += control::medium + look::gap;
        }
        if (!post.pictures.empty()) {
            const auto boxes = picture_layout(post.pictures, x, y + h, w);
            float bottom = y + h;
            for (const auto &box : boxes) {
                bottom = std::max(bottom, box.bottom());
            }
            if (draw) {
                ui.push_id(ui.id("pictures"));
                for (size_t i = 0; i < boxes.size(); ++i) {
                    const Picture &picture = post.pictures[i];
                    const Rect &box = boxes[i];
                    const ui::Id id = ui.id(static_cast<uint64_t>(i));
                    const auto it = ui.interact(id, box);
                    const float hot = ui.anim(id, it.hovered ? 1 : 0, pace::steady);
                    float aspect = picture.w > 0 && picture.h > 0 ? static_cast<float>(picture.w) / static_cast<float>(picture.h) : 1;
                    if (boxes.size() == 1) {
                        // Whole, at its own shape.
                        const auto &e = images_.picture(picture.url, static_cast<uint32_t>(Images::tier(std::max(box.w, box.h) * p.scale())));
                        const float shown = e.ready ? ui.progress(e.ready_at, 320, ease::standard) : 0;
                        if (shown < 1) {
                            p.rect(box, t.fill(1.4f), t.radius);
                        }
                        if (e.ready) {
                            aspect = e.h ? static_cast<float>(e.w) / static_cast<float>(e.h) : aspect;
                            Fade fade(p, shown);
                            p.image(box, e.tex, t.radius, white.mix({0.86f, 0.86f, 0.86f, 1}, hot * 0.3f), cover_uv(aspect, box.w / box.h));
                        }
                    } else {
                        cover(ui, box, picture.url, t.radius_s(), "image");
                        if (hot > 0) {
                            p.rect(box, ui::Color{0, 0, 0, 0.12f * hot}, t.radius_s());
                        }
                    }
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    if (it.clicked) {
                        shell_.dialogs.show_picture(ui, picture.url, aspect);
                    }
                }
                ui.pop_id();
            }
            h = bottom - y + look::gap;
        }
        // A post passed on: its author, its words and what it carries, set in beside a rule.
        if (!post.forwarded.empty()) {
            const Post &quoted = post.forwarded.front();
            constexpr float inset = 16;
            const auto body = t.style(type::body, t.text.mix(t.text_dim, 0.25f));
            const float lh = std::round(body.size * 1.55f), inner = w - inset * 2;
            const std::string name = "@" + quoted.author.name, text = name + (quoted.text.empty() ? "" : "：" + quoted.text);
            const auto lines = p.wrap_lines(text, body.size, inner);
            ui.push_id(ui.id("forwarded"));
            const float carried = attachments(ui, source, quoted, x + inset, 0, inner, false);
            const float block_h = inset + static_cast<float>(lines.size()) * lh + (carried > 0 ? 8 + carried - look::gap : 0) + inset;
            if (draw) {
                const Rect block{x, y + h, w, block_h};
                p.rect(block, t.fill(0.85f), t.radius);
                // The author's name opens their page.
                const auto it = ui.interact(ui.id("author"), {block.x + inset, block.y + inset, p.measure(name, body), lh});
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    browser_.open_profile(source, quoted.author.id, quoted.author.name, quoted.author.avatar);
                }
                const auto marks = highlights(text);
                float line_y = block.y + inset;
                for (auto line : lines) {
                    rich_line(p, block.x + inset, line_y + lh * 0.72f, text, line, marks, body, t.accent);
                    line_y += lh;
                }
                if (carried > 0) {
                    attachments(ui, source, quoted, block.x + inset, line_y + 8, inner, true);
                }
            }
            ui.pop_id();
            h += block_h + look::gap;
        }
        return h;
    }

    float PostFeed::post(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw, bool byline) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float cx = x + look::gutter, inner = w - look::gutter;
        const auto body = t.style(type::lead, t.text);
        const float lh = std::round(body.size * 1.6f);
        const float head_y = y + look::pad, head_h = byline ? look::avatar : 20;
        const auto lines = post.text.empty() ? std::vector<std::string_view>{} : p.wrap_lines(post.text, body.size, inner);
        const float text_y = head_y + head_h + (byline ? 10 : 6);
        const float attach_y = text_y + static_cast<float>(lines.size()) * lh + (lines.empty() ? 0 : 10);
        ui.push_id(ui.id(post.id));
        const float action_y = attach_y + attachments(ui, source, post, cx, attach_y, inner, false);
        const float h = action_y + look::actions + look::pad - 6 - y;
        if (!draw) {
            ui.pop_id();
            return h;
        }

        // A post on its way out fades and stops answering.
        const bool leaving = social_.removing(source, post);
        ui::Enabled alive(ui, !leaving, 0.4f);
        p.rect({cx, y + h - 1, inner, 1}, t.line());
        const std::tm when = local(post.time);
        const auto quiet = t.style(type::caption, t.faint(), text::tabular_nums);
        if (byline) {
            // Who posted, which opens their page.
            const Rect face{x, head_y, look::avatar, look::avatar};
            const ui::Id id = ui.id("face");
            const auto it = ui.interact(id, face);
            cover(ui, face, post.author.avatar, look::avatar / 2, "user");
            if (const float hot = ui.anim(id, it.hovered ? 1 : 0, pace::quick); hot > 0) {
                p.rect(face.inset(-2), {}, look::avatar / 2 + 2, t.text.with_alpha(0.4f * hot), 1.5f);
                ui.cursor = wl::Cursor::pointer;
            }
            const bool named = ui.link(ui.id("author"), {cx, head_y, inner - 40, 22}, post.author.name, t.style(type::body, t.text, text::medium));
            if (it.clicked || named) {
                browser_.open_profile(source, post.author.id, post.author.name, post.author.avatar);
            }
            ui.label({cx, head_y + 21, inner - 40, 18}, post_time(post.time), quiet);
        } else {
            // On one listener's timeline the day stands where the face would: the posts read as a journal.
            if (post.time > 0) {
                ui.label({x, head_y - 2, look::avatar, 24}, std::format("{:02}", when.tm_mday), t.style(type::title, t.text.with_alpha(0.82f), text::semibold | text::tabular_nums),
                         Align::center);
                ui.label({x - 4, head_y + 24, look::avatar + 8, 16}, std::format("{}/{:02}", when.tm_year + 1900, when.tm_mon + 1),
                         t.style(type::micro, t.faint(), text::tabular_nums), Align::center);
                ui.label({cx, head_y, inner - 40, head_h}, std::format("{:02}:{:02}", when.tm_hour, when.tm_min), quiet);
            }
        }
        const auto marks = highlights(post.text);
        float line_y = text_y;
        for (auto line : lines) {
            rich_line(p, cx, line_y + lh * 0.72f, post.text, line, marks, body, t.accent);
            line_y += lh;
        }
        attachments(ui, source, post, cx, attach_y, inner, true);

        // Likes, comments and reposts along the bottom; the rest behind the menu.
        const auto &caps = provider(source).capabilities;
        const auto mark = social_.like(source, post);
        // With no room for words a count of none shows the icon alone.
        const bool wordy = inner >= 300;
        auto word = [&](std::string_view english) {
            return wordy ? lyren::tr(english) : std::string_view{};
        };
        auto label = [&](int count, std::string_view english) {
            return count > 0 ? short_number(count) : std::string(word(english));
        };
        float ax = cx - 8;
        {
            // A like lands with a small swell of its icon. One write at a time: the next click waits for the answer.
            const float pop = ui.spring(ui.id("like-pop"), mark.liked ? 1 : 0, 320, 0.45f);
            const float aw = action_width(ui, label(mark.likes, "Like"));
            ui::Enabled settled(ui, !mark.pending, 1);
            if (action(ui, ui.id("like"), {ax, action_y, aw, look::actions}, mark.liked ? "thumbs-up-fill" : "thumbs-up", mark.likes, word("Like"),
                       mark.liked ? t.accent : t.text_dim, 1 + 0.4f * std::abs(pop - (mark.liked ? 1.f : 0.f)))) {
                social_.toggle_like(source, post);
            }
            ax += aw + 6;
        }
        if (caps.comments[3]) {
            const float aw = action_width(ui, label(post.comments, "Comment"));
            if (action(ui, ui.id("comments"), {ax, action_y, aw, look::actions}, "message-square", post.comments, word("Comment"), t.text_dim)) {
                const std::string &art = post.track ? post.track->cover : !post.pictures.empty() ? post.pictures.front().url : post.author.avatar;
                shell_.open_comments(source, Subject::post, post.thread, lyren::trf("{}'s post", post.author.name), art);
            }
            ax += aw + 6;
        }
        if (caps.post_write) {
            const float aw = action_width(ui, label(post.forwards, "Repost"));
            if (action(ui, ui.id("forward"), {ax, action_y, aw, look::actions}, "repeat-2", post.forwards, word("Repost"), t.text_dim)) {
                compose(ui, source, {}, post);
            }
        }
        const Rect more{x + w - 30, head_y - 3, 28, 28};
        if (leaving) {
            ui.spinner(ui::center(more, 14, 14), t.text_dim, 1.5f);
        } else if (ui.icon_button(ui.id("more"), more, "ellipsis", 16, false, lyren::tr("More"))) {
            menu(ui, more, source, post, byline);
        }
        ui.pop_id();
        return h;
    }

    void PostFeed::menu(ui::Ui &ui, Rect anchor, Source source, const Post &post, bool byline) {
        std::vector<ui::MenuItem> items;
        if (!post.text.empty()) {
            items.push_back({"copy", std::string(lyren::tr("Copy text")), [&ui, text = post.text] {
                                 ui.copy = text;
                                 ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
                             }});
        }
        if (post.track) {
            items.push_back({"list-plus", std::string(lyren::tr("Play next")), [this, &ui, track = *post.track] {
                                 playback_.play_next(track);
                                 ui.toast(std::string(lyren::tr("Added to play next")), "list-plus");
                             }});
        }
        if (byline && !post.author.id.empty()) {
            items.push_back({"user", std::string(lyren::tr("View profile")), [this, source, author = post.author] {
                                 browser_.open_profile(source, author.id, author.name, author.avatar);
                             }});
        }
        if (post.owned) {
            if (!items.empty()) {
                items.push_back({});
            }
            items.push_back({"trash", std::string(lyren::tr("Delete post")),
                             [this, &ui, source, post] {
                                 ui.confirm(std::string(lyren::tr("Delete this post?")), std::string(gist(post)), std::string(lyren::tr("Delete")), "trash", [this, source, post] {
                                     social_.remove(source, post);
                                 });
                             },
                             true});
        }
        if (!items.empty()) {
            ui.open_menu(anchor.x, anchor.bottom() + 4, std::move(items));
        }
    }

    float PostFeed::draw(ui::Ui &ui, Rect r, const std::shared_ptr<Social::Posts> &list, Source source, const std::string &author, bool byline) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &posts = *list;
        const float w = std::min(r.w, column);
        if (posts.items.empty()) {
            if (posts.open()) {
                social_.load_posts(list, source, author);
            }
            const Rect room{r.x, r.y, w, 320};
            if (!posts.error.empty()) {
                empty(ui, {room.x, room.y, room.w, 200}, "cloud-off", lyren::tr("Could not load this"), lyren::tr(posts.error));
                const auto retry = lyren::tr("Retry");
                const float bw = ui.button_width(retry, true);
                if (ui.button(ui.id("posts-retry"), {room.x + (room.w - bw) / 2, room.y + 190, bw, control::medium}, retry, ui::ButtonKind::secondary, "refresh-cw")) {
                    posts.error.clear();
                    social_.load_posts(list, source, author);
                }
            } else if (posts.loading || posts.more) {
                skeleton(ui, {room.x, room.y, room.w, room.h}, byline);
                return 3 * (108 + look::chip + look::pad * 2);
            } else {
                empty(ui, room, "messages-square", lyren::tr(author.empty() ? "No posts from people you follow" : "No posts yet"),
                      author.empty() ? lyren::tr("Follow listeners to see what they share") : std::string_view{});
            }
            return room.h;
        }
        auto &heights = posts.heights;
        if (const float size = t.fs(type::lead); heights.width != w || heights.size != size) {
            heights.reset(w, size);
        }
        while (heights.size_count() < posts.items.size()) {
            heights.push(post(ui, source, posts.items[heights.size_count()], 0, 0, w, false, byline));
        }
        // Only the posts in sight are drawn; the first to arrive come in one after another.
        const Rect clip = ui.clip();
        int shown = 0;
        for (size_t i = heights.first_at(clip.y - r.y); i < posts.items.size(); ++i) {
            const float row_y = r.y + heights.top(i);
            if (row_y >= clip.bottom()) {
                break;
            }
            const float in = ui.cascade(posts.since, shown++);
            Fade fade(p, in);
            post(ui, source, posts.items[i], r.x, row_y + (1 - in) * 10, w, true, byline);
        }
        const float y = r.y + heights.total();
        const Rect foot{r.x, y, w, 64};
        if (posts.loading) {
            ui.spinner(ui::center(foot, 20, 20), t.text_dim, 2);
        } else if (!posts.error.empty()) {
            ui.label({foot.x, foot.y + 8, foot.w, 20}, lyren::tr(posts.error), t.style(type::caption, t.text_dim), Align::center);
            const auto retry = lyren::tr("Retry");
            const float bw = ui.button_width(retry, true);
            if (ui.button(ui.id("posts-retry"), {foot.x + (foot.w - bw) / 2, foot.y + 32, bw, control::small}, retry, ui::ButtonKind::ghost, "refresh-cw")) {
                posts.error.clear();
                social_.load_posts(list, source, author);
            }
        } else if (posts.more) {
            // The next page is asked for before the end comes into sight.
            if (y < clip.bottom() + 600) {
                social_.load_posts(list, source, author);
            }
        } else {
            ui.label({foot.x, foot.y + 18, foot.w, 24}, lyren::tr("That's everything"), t.style(type::caption, t.faint()), Align::center);
        }
        return y - r.y + foot.h;
    }

} // namespace lyren
