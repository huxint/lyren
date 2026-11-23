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
        ui.resize_modal(imported ? 520 : 400, std::min(imported ? 500.f : 540.f, ui.height - 40));
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
            const bool in = env_.signed_in(entry.source), qr = provider(entry.source).capabilities.qr_login;
            if (account) {
                cover(ui, face, account->avatar, 20, "user");
                p.rect(face, {}, 20, t.surface_edge(0.8f), 1);
            } else {
                p.rect(face, t.fill(1.5f), 20, t.surface_edge(0.8f), 1);
                p.icon("user", ui::center(face, 18, 18), t.text_dim);
            }
            const auto label = lyren::tr(in ? "Sign out" : qr ? "Scan to sign in" : "Connect account");
            const float bw = ui.button_width(label, true);
            const Rect button{row.right() - 14 - bw, row.y + (row.h - control::medium) / 2, bw, control::medium};
            const float text_w = button.x - face.right() - 26;
            ui.label({face.right() + 14, row.y + 12, text_w, 22}, lyren::tr(entry.name), t.style(type::lead, t.text, text::medium));
            const std::string_view status = account ? std::string_view(account->name) : in ? lyren::tr("Loading account") : lyren::tr("Not signed in");
            ui.label({face.right() + 14, row.y + 34, text_w, 18}, status, t.style(type::caption, account ? t.accent : t.text_dim));
            const ui::Id id = ui.id(entry.id);
            if (in) {
                if (ui.button(id, button, label, ui::ButtonKind::ghost, "log-out")) {
                    accounts_.sign_out(entry.source);
                }
            } else if (ui.button(id, button, label, ui::ButtonKind::secondary, qr ? "qr-code" : "link")) {
                accounts_.begin(entry.source);
            }
        }
        ui.end_scroll();
        p.icon("lock", {r.x, r.bottom() - 16, 13, 13}, t.faint());
        ui.label({r.x + 20, r.bottom() - 19, r.w - 20, 18}, lyren::tr("Sign-ins are encrypted on this device"), t.style(type::caption, t.faint()));
    }

    void AccountDialog::import(ui::Ui &ui, Rect r, Source source) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &sign_in = accounts_.sign_in();
        ui.label({r.x + 32, r.y, r.w - 72, 30}, lyren::tr("Connect account"), t.style(type::title, t.text, text::semibold));
        ui.label({r.x, r.y + 42, r.w, 20}, lyren::trf("Paste your {} connection settings", lyren::tr(platform(source).short_name)), t.style(type::body, t.text));
        ui.label({r.x, r.y + 66, r.w, 20}, lyren::tr("Use your signing service and account Cookie"), t.style(type::caption, t.text_dim));
        {
            ui::Enabled editable(ui, !sign_in.importing, 0.6f);
            ui.text_area(ui.id("account-config"), {r.x, r.y + 100, r.w, r.h - 212}, sign_in.settings, R"({"signer_url":"http://127.0.0.1:8000","cookie":"…"})");
        }
        float y = r.bottom() - 100;
        for (const auto &line : p.wrap_lines(sign_in.error, t.fs(type::caption), r.w)) {
            if (y > r.bottom() - 62) {
                break;
            }
            ui.label({r.x, y, r.w, 18}, line, t.style(type::caption, t.danger));
            y += 18;
        }
        const auto connect = lyren::tr("Connect");
        const float bw = std::max(100.f, ui.button_width(connect, true));
        const Rect button{r.right() - bw, r.bottom() - control::large, bw, control::large};
        p.icon("lock", {r.x, button.y + (button.h - 13) / 2, 13, 13}, t.faint());
        ui.label({r.x + 20, button.y, button.x - r.x - 60, button.h}, lyren::tr("Sign-ins are encrypted on this device"), t.style(type::caption, t.faint()));
        if (sign_in.importing) {
            ui.spinner({button.x - 30, button.y + (button.h - 16) / 2, 16, 16}, t.accent, 2);
        }
        ui::Enabled possible(ui, !sign_in.importing && !sign_in.settings.empty());
        if (ui.button(ui.id("account-connect"), button, connect, ui::ButtonKind::primary, "link")) {
            accounts_.import_settings();
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
                p.icon("music", ui::center(logo, 22, 22), t.on_accent());
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
            headline = sign_in.status ? lyren::tr("Sign-in incomplete") : lyren::tr("Could not get a QR code"), detail = sign_in.error;
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
