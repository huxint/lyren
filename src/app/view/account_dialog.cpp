#include "app/view/account_dialog.hpp"

#include <algorithm>

#include "app/view/draw.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    namespace type = ui::type;
    namespace pace = ui::pace;
    namespace control = ui::control;

    void AccountDialog::open(ui::Ui &ui) {
        had_feedback_ = false;
        accounts_.cancel();
        ui.open_modal(460, std::min(540.f, ui.height - 40), [this](ui::Ui &u, Rect r) {
            body(u, r);
        });
    }

    void AccountDialog::body(ui::Ui &ui, Rect r) {
        const auto &sign_in = accounts_.sign_in();
        // Steps slide sideways: forward into the sign-in, back to the list.
        const float in = ui.progress(sign_in.since, pace::arrive);
        Fade fade(ui.painter(), in);
        r.x += (1 - in) * 32 * (sign_in.source ? 1 : -1);
        if (!sign_in.source) {
            ui.resize_modal(460, std::min(540.f, ui.height - 40));
            return list(ui, r);
        }
        const Source source = *sign_in.source;
        const bool imported = provider(source).capabilities.session_import;
        ui.resize_modal(imported ? 540 : 400, std::min(imported ? 600.f : 540.f, ui.height - 40));
        if (ui.icon_button(ui.id("login-back"), {r.x - 8, r.y - 2, 32, 32}, "arrow-left", 18, false, lyren::tr("Back"))) {
            return accounts_.cancel();
        }
        if (imported) {
            import(ui, r, source);
        } else {
            scan(ui, r, source);
        }
    }

    void AccountDialog::list(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        dialog_title(ui, r, lyren::tr("Accounts"), lyren::tr("Sign in to each platform to see its music"));
        constexpr float row_h = 64, step = 72;
        const Rect view{r.x, r.y + 70, r.w, r.h - 100};
        const float off = ui.begin_scroll(ui.id("accounts-list"), view, step * static_cast<float>(platforms.size()) - (step - row_h));
        for (size_t i = 0; i < platforms.size(); ++i) {
            const auto &entry = platforms[i];
            const Rect row{r.x, view.y + static_cast<float>(i) * step - off, r.w, row_h};
            p.rect(row, t.fill(0.95f), t.radius, t.surface_edge(0.85f), 1);
            const Rect face{row.x + 14, row.y + 12, 40, 40};
            const auto &account = library_.shelf(entry.source).account;
            const auto &caps = provider(entry.source).capabilities;
            const bool in = env_.signed_in(entry.source), qr = caps.qr_login, signer_only = accounts_.signer_connected(entry.source);
            if (account) {
                cover(ui, face, account->avatar, 20, "user");
                p.rect(face, {}, 20, t.surface_edge(0.8f), 1);
            } else {
                p.rect(face, signer_only ? t.accent.with_alpha(0.14f) : t.fill(1.5f), 20, t.surface_edge(0.8f), 1);
                p.icon(signer_only ? "link" : "user", ui::center(face, 18, 18), signer_only ? t.accent : t.text_dim);
            }
            const ui::Id id = ui.id(entry.id);
            float right = row.right() - 14;
            if (in) {
                const auto label = lyren::tr("Sign out");
                const float bw = ui.button_width(label, true);
                const Rect button{right - bw, row.y + (row.h - control::medium) / 2, bw, control::medium};
                if (ui.button(id, button, label, ui::ButtonKind::ghost, "log-out")) {
                    accounts_.sign_out(entry.source);
                }
                right = button.x - 8;
                if (caps.session_import) {
                    const Rect edit_r{right - control::medium, row.y + (row.h - control::medium) / 2, control::medium, control::medium};
                    if (ui.icon_button(ui::sub_id(id, 2), edit_r, "pencil", 15, false, lyren::tr("Edit connection"))) {
                        accounts_.begin(entry.source);
                    }
                    right = edit_r.x - 8;
                }
            } else {
                if (signer_only) {
                    const Rect disc_r{right - control::medium, row.y + (row.h - control::medium) / 2, control::medium, control::medium};
                    if (ui.icon_button(ui::sub_id(id, 3), disc_r, "log-out", 15, false, lyren::tr("Disconnect signing service"))) {
                        accounts_.sign_out(entry.source);
                    }
                    right = disc_r.x - 8;
                }
                const auto label = lyren::tr(qr ? "Scan to sign in" : "Connect account");
                const float bw = ui.button_width(label, true);
                const Rect button{right - bw, row.y + (row.h - control::medium) / 2, bw, control::medium};
                if (ui.button(id, button, label, ui::ButtonKind::secondary, qr ? "qr-code" : "link")) {
                    accounts_.begin(entry.source);
                }
                right = button.x - 8;
            }
            const float text_w = std::max(0.f, right - face.right() - 18);
            ui.label({face.right() + 14, row.y + 12, text_w, 22}, lyren::tr(entry.name), t.style(type::lead, t.text, text::medium));
            const std::string_view status = account               ? std::string_view(account->name)
                                            : in                  ? lyren::tr("Loading account")
                                            : signer_only         ? lyren::tr("Signing service connected · Not signed in")
                                            : caps.session_import ? lyren::tr("Not signed in · Anonymous playback ready")
                                                                  : lyren::tr("Not signed in");
            ui.label({face.right() + 14, row.y + 34, text_w, 18}, status, t.style(type::caption, account || signer_only ? t.accent : t.text_dim));
        }
        ui.end_scroll();
        p.icon("lock", {r.x, r.bottom() - 16, 13, 13}, t.faint());
        ui.label({r.x + 20, r.bottom() - 19, r.w - 20, 18}, lyren::tr("Sign-ins are encrypted on this device"), t.style(type::caption, t.faint()));
    }

    void AccountDialog::import(ui::Ui &ui, Rect r, Source source) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &sign_in = accounts_.sign_in();
        ui.label({r.x + 32, r.y, r.w - 72, 30}, lyren::trf("Connect {}", lyren::tr(platform(source).name)), t.style(type::title, t.text, text::semibold));

        const Rect view{r.x, r.y + 42, r.w, std::max(0.f, r.h - 42 - control::large - 14)};
        const ui::Id scroll_id = ui.id("account-import-scroll");
        const float off = ui.begin_scroll(scroll_id, view, std::max(import_h_, view.h));
        const float x = view.x, w = std::max(120.f, view.w - 6);
        float y = view.y - off;

        // 1. Anonymous availability callout.
        {
            const auto note_lines = p.wrap_lines(
                lyren::tr("Anonymous search, playback, and charts work without signing in. Connect a signing service and Cookie to sync your library, likes, and comments."),
                t.fs(type::caption), w - 42);
            const float note_h = std::max(38.f, static_cast<float>(note_lines.size()) * 17 + 16);
            const Rect banner{x, y, w, note_h};
            p.rect(banner, t.accent.with_alpha(t.dark ? 0.12f : 0.08f), t.radius_s(), t.accent.with_alpha(0.25f), 1);
            p.icon("info", {banner.x + 12, banner.y + 11, 15, 15}, t.accent);
            float ly = banner.y + 8;
            for (const auto &line : note_lines) {
                ui.label({banner.x + 34, ly, banner.w - 42, 17}, line, t.style(type::caption, t.text_dim));
                ly += 17;
            }
            y += note_h + 14;
        }

        const bool busy = sign_in.importing || sign_in.checking;
        const auto snapshot = std::array<std::string, 6>{sign_in.signer_url, sign_in.signer_token, sign_in.cookie, sign_in.device_id, sign_in.iid, sign_in.fp};
        sign_in.structured = true;
        {
            ui::Enabled editable(ui, !busy, 0.6f);

            // 2. Signing service URL & optional token.
            ui.label({x, y, w, 18}, lyren::tr("Signing service URL"), t.style(type::label, t.text, text::medium));
            y += 22;
            ui.text_box(ui.id("qs-signer-url"), {x, y, w, control::medium}, sign_in.signer_url, "http://127.0.0.1:8000", "link");
            y += control::medium + 12;

            ui.label({x, y, w, 18}, lyren::tr("Access token (optional)"), t.style(type::label, t.text, text::medium));
            y += 22;
            ui.text_box(ui.id("qs-signer-token"), {x, y, w, control::medium}, sign_in.signer_token, lyren::tr("Bearer token if your signing service requires one"));
            y += control::medium + 12;

            // 3. Cookie / Netscape cookie_jar / JSON paste.
            ui.label({x, y, w, 18}, lyren::tr("Account Cookie (optional)"), t.style(type::label, t.text, text::medium));
            y += 22;
            ui.text_area(ui.id("qs-cookie"), {x, y, w, 82}, sign_in.cookie, lyren::tr("sessionid=…; sid_tt=… (also accepts Cookie: header, Netscape cookie_jar, or full JSON)"));
            y += 88;

            if (accounts_.unpack_json_settings(sign_in.signer_url) || accounts_.unpack_json_settings(sign_in.cookie)) {
                ui.toast(std::string(lyren::tr("Imported JSON settings")), "circle-check");
            }
        }

        auto section_toggle = [&](std::string_view key, std::string_view label, bool &open) {
            const Rect bar{x, y, w, 28};
            const ui::Id tid = ui.id(key);
            const auto it = ui.interact(tid, bar);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.clicked) {
                open = !open;
            }
            p.icon(open ? "chevron-down" : "chevron-right", {bar.x + 2, bar.y + 7, 14, 14}, it.hovered ? t.text : t.text_dim);
            ui.label({bar.x + 20, bar.y, bar.w - 20, bar.h}, label, t.style(type::caption, it.hovered ? t.text : t.text_dim, text::medium));
            y += 30;
        };

        // 4. Collapsible advanced device identity overrides.
        section_toggle("qs-adv-toggle", lyren::tr("Advanced device identity (optional)"), sign_in.show_advanced);
        if (sign_in.show_advanced) {
            ui::Enabled editable(ui, !busy, 0.6f);
            ui.label({x + 20, y, w - 20, 16}, lyren::tr("Overrides GET /config when set; leave blank to use the signing service's identity"), t.style(type::micro, t.faint()));
            y += 20;
            const float half = std::floor((w - 20 - 8) / 2);
            ui.text_box(ui.id("qs-did"), {x + 20, y, half, control::small}, sign_in.device_id, "device_id");
            ui.text_box(ui.id("qs-iid"), {x + 20 + half + 8, y, w - 20 - half - 8, control::small}, sign_in.iid, "iid");
            y += control::small + 8;
            ui.text_box(ui.id("qs-fp"), {x + 20, y, w - 20, control::small}, sign_in.fp, "fp (optional)");
            y += control::small + 10;
        }
        if (std::array<std::string_view, 6>{sign_in.signer_url, sign_in.signer_token, sign_in.cookie, sign_in.device_id, sign_in.iid, sign_in.fp} !=
            std::array<std::string_view, 6>{snapshot[0], snapshot[1], snapshot[2], snapshot[3], snapshot[4], snapshot[5]}) {
            sign_in.error.clear();
            sign_in.check.reset();
        }

        // 5. Collapsible 3-step connection guide + protocol snippet copy.
        section_toggle("qs-guide-toggle", lyren::tr("How to connect (3 steps)"), sign_in.show_guide);
        if (sign_in.show_guide) {
            constexpr std::array<std::string_view, 3> steps{
                "1. Run a signing service that serves GET /config and POST /sign.",
                "2. Sign in on Qishui web/app and copy Cookie from DevTools Network (optional).",
                "3. Click Test connection to check each step, then click Connect.",
            };
            for (const auto step_key : steps) {
                for (const auto &line : p.wrap_lines(lyren::tr(step_key), t.fs(type::caption), w - 24)) {
                    ui.label({x + 20, y, w - 24, 18}, line, t.style(type::caption, t.text_dim));
                    y += 18;
                }
                y += 2;
            }
            const auto copy_lbl = lyren::tr("Copy protocol example");
            const float cbw = ui.button_width(copy_lbl, true);
            if (ui.button(ui.id("qs-copy-proto"), {x + 20, y + 4, cbw, control::small}, copy_lbl, ui::ButtonKind::ghost, "copy")) {
                ui.copy = "{\n  \"signer_url\": \"http://127.0.0.1:8000\",\n  \"signer_token\": \"\",\n  \"cookie\": \"sessionid=...; sid_tt=...\"\n}\n";
                ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
            }
            y += control::small + 12;
        }

        // 6. Diagnostic 3-step result card.
        if (sign_in.checking || sign_in.check) {
            y += 4;
            const SessionCheck check = sign_in.check.value_or(SessionCheck{});
            const std::array<std::pair<std::string_view, const SessionCheck::Step *>, 3> rows{{
                {lyren::tr("Signing service"), &check.signer},
                {lyren::tr("Device identity"), &check.identity},
                {lyren::tr("Account session"), &check.account},
            }};
            const Rect card_r{x, y, w, 86};
            p.rect(card_r, t.fill(0.95f), t.radius_s(), t.surface_edge(0.85f), 1);
            float ry = card_r.y + 8;
            for (const auto &[name, step_ptr] : rows) {
                const auto state = step_ptr->state;
                const Rect ic{card_r.x + 12, ry + 3, 14, 14};
                if (sign_in.checking && state == SessionCheck::State::pending) {
                    ui.spinner(ic, t.accent, 1.8f);
                } else {
                    const char *glyph = state == SessionCheck::State::ok ? "circle-check" : state == SessionCheck::State::failed ? "circle-x" : "minus";
                    const ui::Color col = state == SessionCheck::State::ok ? t.accent : state == SessionCheck::State::failed ? t.danger : t.faint();
                    p.icon(glyph, ic, col);
                }
                const float label_w = 116;
                ui.label({card_r.x + 32, ry, label_w, 20}, name, t.style(type::caption, t.text, text::medium));
                const std::string_view detail = step_ptr->detail.empty() ? std::string_view{} : lyren::tr(step_ptr->detail);
                ui.label({card_r.x + 32 + label_w + 6, ry, std::max(0.f, card_r.w - 44 - label_w), 20}, detail,
                         t.style(type::caption, state == SessionCheck::State::failed ? t.danger : t.text_dim));
                ry += 24;
            }
            y += card_r.h + 10;
        }

        // 7. Localized error message if import failed.
        if (!sign_in.error.empty()) {
            y += 2;
            for (const auto &line : p.wrap_lines(lyren::tr(sign_in.error), t.fs(type::caption), w)) {
                ui.label({x, y, w, 18}, line, t.style(type::caption, t.danger));
                y += 18;
            }
            y += 4;
        }

        const float total_h = (y - (view.y - off)) + 6;
        const bool has_feedback = sign_in.checking || sign_in.check.has_value() || !sign_in.error.empty();
        if (std::abs(import_h_ - total_h) > 0.5f) {
            import_h_ = total_h;
            ui.redraw();
        }
        if (has_feedback && !had_feedback_ && total_h > view.h) {
            ui.scroll_by(scroll_id, total_h - view.h - off);
            ui.redraw();
        }
        had_feedback_ = has_feedback;
        ui.end_scroll();

        const bool has_input = sign_in.structured ? !sign_in.signer_url.empty() : !sign_in.settings.empty();
        const auto connect_lbl = lyren::tr("Connect");
        const auto test_lbl = lyren::tr("Test connection");
        const float connect_w = std::max(96.f, ui.button_width(connect_lbl, true));
        const float test_w = ui.button_width(test_lbl, true);
        const Rect connect_btn{r.right() - connect_w, r.bottom() - control::large, connect_w, control::large};
        const Rect test_btn{connect_btn.x - 8 - test_w, connect_btn.y, test_w, control::large};
        if (test_btn.x - r.x > 130) {
            p.icon("lock", {r.x, connect_btn.y + (connect_btn.h - 13) / 2, 13, 13}, t.faint());
            ui.label({r.x + 20, connect_btn.y, test_btn.x - r.x - 36, connect_btn.h}, lyren::tr("Sign-ins are encrypted on this device"), t.style(type::caption, t.faint()));
        }
        if (sign_in.importing || sign_in.checking) {
            ui.spinner({test_btn.x - 26, connect_btn.y + (connect_btn.h - 16) / 2, 16, 16}, t.accent, 2);
        }
        {
            ui::Enabled can_test(ui, !busy && has_input);
            if (ui.button(ui.id("account-test"), test_btn, test_lbl, ui::ButtonKind::secondary, "sparkles")) {
                accounts_.test_settings();
            }
            if (ui.button(ui.id("account-connect"), connect_btn, connect_lbl, ui::ButtonKind::primary, "link")) {
                accounts_.import_settings();
            }
        }
    }

    void AccountDialog::scan(ui::Ui &ui, Rect r, Source source) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &sign_in = accounts_.sign_in();
        ui.label({r.x + 32, r.y, r.w - 72, 30}, lyren::tr("Scan to sign in"), t.style(type::title, t.text, text::semibold));
        ui.label({r.x, r.y + 40, r.w, 20}, lyren::tr(platform(source).scan_hint), t.style(type::label, t.text_dim), Align::center);
        // The code keeps its white card in every theme: phones read dark modules on white.
        const Rect card = ui::center({r.x, r.y + 76, r.w, 240}, 228, 228);
        p.shadow({card.x, card.y + 8, card.w, card.h}, 14, 30, t.shadow.with_alpha(0.55f));
        p.rect(card, white, 14, t.dark ? ui::Color{} : t.surface_edge(1.1f), t.dark ? 0.f : 1.f);
        const QrState state = sign_in.status ? sign_in.status->state : QrState::waiting;
        const bool failed = !sign_in.error.empty();
        if (sign_in.has_code()) {
            const Rect code = card.inset(14);
            if (sign_in.image) {
                p.image(code, env_.images.get("qr", 512).tex, 0);
            } else {
                // Modules as horizontal runs, so a row is a few rectangles.
                const int n = sign_in.size;
                const float m = code.w / static_cast<float>(n);
                auto module = [&](int x, int y) {
                    return sign_in.modules[static_cast<size_t>(y * n + x)] != 0;
                };
                for (int y = 0; y < n; ++y) {
                    for (int x = 0; x < n;) {
                        if (!module(x, y)) {
                            ++x;
                            continue;
                        }
                        int end = x;
                        while (end < n && module(end, y)) {
                            ++end;
                        }
                        p.rect({code.x + static_cast<float>(x) * m, code.y + static_cast<float>(y) * m, static_cast<float>(end - x) * m, m}, ink);
                        x = end;
                    }
                }
                const Rect logo = ui::center(card, 40, 40);
                p.rect(logo.inset(-4), white, 12);
                p.rect(logo, t.accent, 10);
                p.icon("lyren", ui::center(logo, 22, 22), t.on_accent());
            }
            // Scanned, expired or failed: the code fades under the state.
            const float veil = ui.anim(ui.id("qr-veil"), failed || state == QrState::scanned || state == QrState::expired ? 1 : 0, 240);
            if (veil > 0) {
                p.rect(card, white.with_alpha(0.9f * veil), 14);
                Fade fade(p, veil);
                if (failed) {
                    p.icon("circle-x", ui::center(card, 48, 48), ink.with_alpha(0.7f));
                } else if (state == QrState::scanned && !sign_in.status->avatar.empty()) {
                    cover(ui, ui::center(card, 72, 72), sign_in.status->avatar, 36, "user");
                } else {
                    p.icon(state == QrState::expired ? "rotate-ccw" : "circle-check", ui::center(card, 48, 48), ink.with_alpha(0.7f));
                }
            }
        } else if (!failed) {
            ui.spinner(ui::center(card, 28, 28), ink.with_alpha(0.6f), 2.5f);
        }

        std::string headline, detail;
        bool busy = false;
        if (failed) {
            headline = sign_in.status ? lyren::tr("Sign-in incomplete") : lyren::tr("Could not get a QR code"), detail = lyren::tr(sign_in.error);
        } else if (!sign_in.has_code()) {
            headline = lyren::tr("Generating QR code"), busy = true;
        } else if (state == QrState::waiting) {
            headline = lyren::tr("Waiting for scan"), detail = lyren::tr("Scan, then confirm on your phone"), busy = true;
        } else if (state == QrState::scanned) {
            const auto &name = sign_in.status->nickname;
            headline = lyren::tr("Scanned"), busy = true;
            detail = name.empty() ? std::string(lyren::tr("Confirm sign-in on your phone")) : lyren::trf("{}, confirm sign-in on your phone", name);
        } else if (state == QrState::expired) {
            headline = lyren::tr("QR code expired"), detail = lyren::tr("Refresh to scan again");
        } else {
            headline = lyren::tr("Signing in"), busy = true;
        }
        const float y = card.bottom() + 28;
        const auto style = t.style(type::body, t.text, text::medium);
        const float tw = p.measure(headline, style), x = r.x + (r.w - tw - (busy ? 26 : 0)) / 2;
        if (busy) {
            ui.spinner({x, y + 2, 16, 16}, t.accent, 2);
        }
        ui.label({x + (busy ? 26 : 0), y, tw + 1, 20}, headline, style);
        ui.label({r.x, y + 28, r.w, 20}, detail, t.style(type::caption, failed ? t.danger : t.text_dim), Align::center);
        const auto refresh = lyren::tr("Refresh QR code");
        const float bw = ui.button_width(refresh, true);
        if (ui.button(ui.id("qr-refresh"), {r.x + (r.w - bw) / 2, r.bottom() - control::medium, bw, control::medium}, refresh, ui::ButtonKind::secondary, "refresh-cw")) {
            accounts_.refresh_code();
        }
    }

} // namespace lyren
