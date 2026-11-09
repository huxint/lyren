#include "app/shell.hpp"

#include <qrencode.h>

#include <format>

#include "app/draw.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    constexpr ui::Color white{1, 1, 1, 1};

    Task<void> Shell::startup() {
        auto saved = co_await creds::load(loop_);
        if (saved) {
            credentials_ = creds::from_json(*saved);
            for (const auto &entry : platforms) {
                provider(entry.source).session() = std::move(credentials_[entry.id]);
            }
            can_save_creds_ = true;
        } else {
            notify(lyren::trf("Could not read saved credentials: {}", saved.error()), "triangle-alert", true);
        }
        for (const auto &entry : platforms) {
            spawn(initialize_platform(entry.source));
        }
        save_credentials();
    }

    Task<void> Shell::initialize_platform(Source source) {
        auto r = co_await provider(source).initialize();
        if (!r) {
            notify(std::format("{}: {}", lyren::tr(platform(source).name), r.error()), "user", true);
        }
        if (source == active_) {
            ensure_route_data();
            spawn(warm_pages(source));
        }
        if (provider(source).logged_in()) {
            co_await load_library(source);
        }
    }

    void Shell::save_credentials() {
        if (!can_save_creds_) {
            return;
        }
        // Responses often set several cookies in a row; write once they settle.
        loop_.cancel(creds_timer_);
        creds_timer_ = loop_.after(1s, [this] {
            creds_timer_ = 0;
            spawn([](Shell *self) -> Task<void> {
                for (const auto &entry : platforms) {
                    self->credentials_[entry.id] = self->provider(entry.source).session();
                }
                auto r = co_await creds::save(self->loop_, creds::to_json(self->credentials_));
                if (!r) {
                    self->notify(lyren::trf("Could not save credentials: {}", r.error()), "triangle-alert", true);
                }
            }(this));
        });
    }

    Task<void> Shell::load_library(Source source) {
        Provider &p = provider(source);
        if (!p.logged_in()) {
            co_return;
        }
        Library &lib = library(source);
        uint64_t gen = ++lib.gen;
        lib.library_error.clear();
        auto a = co_await p.account();
        if (gen != lib.gen) {
            co_return;
        }
        if (!a) {
            lib.library_error = a.error();
            notify(a.error(), "user", true);
            if (!p.logged_in()) {
                save_credentials();
            }
            co_return;
        }
        bool account_changed = !lib.account || lib.account->id != a->id;
        if (account_changed && lib.account) {
            reset_account(source);
            gen = lib.gen;
        }
        lib.account = *a;
        images_.prefetch(a->avatar, 64);
        if (account_changed && thread_.source == source && !thread_.id.empty()) {
            thread_.draft = std::make_shared<CommentDraft>();
            thread_.replies.reset();
            refresh_comments();
        }
        redraw_();
        if (p.features.daily.playlists) {
            spawn(load_daily_lists(source));
        }
        if (p.capabilities.library) {
            uint64_t revision = lib.preference_gen;
            uint64_t playlist_gen = ++lib.playlist_gen;
            auto pl = co_await p.user_playlists(a->id);
            if (gen != lib.gen) {
                co_return;
            }
            if (playlist_gen == lib.playlist_gen) {
                if (pl) {
                    apply_playlists(source, std::move(*pl), revision);
                } else {
                    lib.library_error = pl.error();
                    notify(pl.error(), "list-music", true);
                }
            }
        }
        if (p.capabilities.likes) {
            uint64_t revision = lib.preference_gen;
            auto ids = co_await p.liked_ids(a->id);
            if (gen != lib.gen) {
                co_return;
            }
            if (ids) {
                lib.liked_ids = {ids->begin(), ids->end()};
                reconcile_preferences(source, Preference::song, revision);
            } else {
                notify(ids.error(), "heart", true);
            }
        }
        // A page that waited for the login loads now.
        if (source == active_) {
            if (route_.page == Page::liked) {
                route_.list.reset();
            }
            ensure_route_data();
        }
        redraw_();
    }

    void Shell::start_login(ui::Ui &ui, Source source) {
        login_step_ = static_cast<int>(source);
        login_time_ = ui.now;
        qr_error_.clear();
        account_config_.clear();
        account_importing_ = false;
        if (provider(source).capabilities.qr_login) {
            spawn(qr_login());
        }
    }

    Task<void> Shell::import_login(Source source) {
        uint64_t gen = ++qr_gen_;
        account_importing_ = true;
        qr_error_.clear();
        auto result = co_await provider(source).import_session(account_config_);
        if (gen != qr_gen_) {
            co_return;
        }
        account_importing_ = false;
        if (!result) {
            qr_error_ = result.error();
            co_return redraw_();
        }
        can_save_creds_ = true;
        reset_account(source);
        save_credentials();
        co_await load_library(source);
        if (gen != qr_gen_) {
            co_return;
        }
        account_config_.clear();
        login_step_ = -1;
        login_time_ = now_();
        if (library(source).account) {
            notify(lyren::trf("Signed in as {}", library(source).account->name), "circle-check");
        } else {
            notify(std::string(lyren::tr("Connection settings saved")), "circle-check");
        }
        if (source == active_) {
            ensure_route_data();
        }
        spawn(warm_pages(source));
        redraw_();
    }

    Task<void> Shell::qr_login() {
        uint64_t gen = ++qr_gen_;
        Source source = static_cast<Source>(login_step_);
        qr_active_ = true;
        provider(source).cancel_login();
        qr_.clear();
        qr_image_ = false;
        images_.erase("qr");
        qr_status_.reset();
        qr_error_.clear();
        auto code = co_await provider(source).qr_start();
        if (gen != qr_gen_) {
            co_return;
        }
        if (!code) {
            qr_error_ = code.error();
            co_return redraw_();
        }
        if (!code->image.empty()) {
            auto img = gfx::decode_square({reinterpret_cast<const uint8_t *>(code->image.data()), code->image.size()}, 512, qr_error_);
            if (!img.w) {
                co_return redraw_();
            }
            images_.put("qr", 512, img);
            qr_image_ = true;
        } else {
            QRcode *q = QRcode_encodeString(code->text.c_str(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
            qr_size_ = q->width;
            qr_.resize(static_cast<size_t>(q->width) * static_cast<size_t>(q->width));
            for (size_t i = 0; i < qr_.size(); ++i) {
                qr_[i] = q->data[i] & 1;
            }
            QRcode_free(q);
        }
        qr_status_ = QrStatus{};
        redraw_();
        while (true) {
            co_await sleep{loop_, 2s};
            if (gen != qr_gen_) {
                co_return;
            }
            auto s = co_await provider(source).qr_poll();
            if (gen != qr_gen_) {
                co_return;
            }
            if (!s) {
                qr_error_ = s.error();
                co_return redraw_();
            }
            qr_status_ = *s;
            redraw_();
            if (s->state == QrState::expired) {
                co_return;
            }
            if (s->state == QrState::confirmed) {
                break;
            }
        }
        qr_active_ = false;
        can_save_creds_ = true;
        reset_account(source);
        save_credentials();
        co_await load_library(source);
        if (gen != qr_gen_) {
            co_return;
        }
        login_step_ = -1;
        login_time_ = now_();
        if (library(source).account) {
            notify(lyren::trf("Signed in as {}", library(source).account->name), "circle-check");
        }
    }

    void Shell::cancel_login() {
        ++qr_gen_;
        provider(static_cast<Source>(login_step_)).cancel_login();
        account_config_.clear();
        account_importing_ = false;
        qr_active_ = false;
        images_.erase("qr");
        qr_image_ = false;
    }

    void Shell::reset_account(Source source) {
        auto &lib = library(source);
        if (source == active_) {
            reset_selection();
        }
        ++lib.gen;
        ++lib.collection_gen;
        ++lib.account_gen;
        ++lib.playlist_gen;
        lib.playlist_writes.clear();
        lib.preference_gen = 0;
        lib.preferences = {};
        lib.account.reset();
        results_[static_cast<size_t>(source)] = {};
        lib.recommended = {};
        lib.toplists = {};
        lib.square = {};
        ++lib.square_gen;
        lib.releases = {};
        for (auto &gen : lib.release_gen) {
            ++gen;
        }
        clear_comment_writes(source);
        if (thread_.source == source) {
            thread_.draft = std::make_shared<CommentDraft>();
            thread_.replies.reset();
        }
        lib.playlists.clear();
        lib.playlists_loaded = false;
        lib.library_error.clear();
        lib.liked_ids.clear();
        lib.liked = {};
        lib.daily_lists.clear();
        lib.fm.clear();
        lib.fm_pos = 0;
        lib.fm_loading = false;
        lib.dislikes = {};
        lib.feed.reset();
        std::erase_if(post_likes_, [&](const auto &like) {
            return like.first.variant == static_cast<uint32_t>(source);
        });
        std::erase_if(post_deletes_, [&](const auto &entry) {
            return entry.first.variant == static_cast<uint32_t>(source);
        });
        if (fm_source_ == source) {
            fm_playing_ = false;
        }
        if (listen_ && listen_->source == source) {
            listen_.reset();
        }
        for (auto &c : lib.collection) {
            c = {};
        }
        auto reset_route = [source](Route &route) {
            if (route.card.source == source) {
                route.list.reset();
                route.artist.reset();
                route.profile.reset();
                if (route.card.owned) {
                    route = {Page::recommend, {.source = source}};
                }
            }
        };
        reset_route(route_);
        for (auto &route : history_) {
            reset_route(route);
        }
        redraw_();
    }

    Task<void> Shell::logout(Source source) {
        reset_account(source);
        co_await provider(source).logout();
        if (thread_.source == source && !thread_.id.empty()) {
            refresh_comments();
        }
        if (source == active_) {
            ensure_route_data();
        }
        save_credentials();
        notify(lyren::trf("Signed out of {}", lyren::tr(platform(source).name)), "log-out");
    }

    void Shell::open_account(ui::Ui &ui) {
        login_step_ = -1;
        ui.open_modal(460, std::min(540.f, ui.height - 40), [this](ui::Ui &u, Rect body) {
            account_modal(u, body);
        });
    }

    void Shell::account_modal(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // Steps slide sideways: forward into the code, back to the list.
        float in = ui.progress(login_time_, 380);
        Fade fade(p, in);
        r.x += (1 - in) * 32 * (login_step_ >= 0 ? 1 : -1);

        if (login_step_ < 0) {
            ui.resize_modal(460, std::min(540.f, ui.height - 40));
            ui.label({r.x, r.y, r.w, 30}, lyren::tr("Accounts"), {.size = t.fs(20), .color = t.text});
            ui.label({r.x, r.y + 32, r.w, 20}, lyren::tr("Sign in to each platform to see its music"), {.size = t.fs(12), .color = t.text_dim});
            Rect viewport{r.x, r.y + 68, r.w, r.h - 98};
            float offset = ui.begin_scroll(ui.id("accounts-list"), viewport, 72 * static_cast<float>(platforms.size()));
            for (size_t i = 0; i < platforms.size(); ++i) {
                const auto &entry = platforms[i];
                Rect row{r.x, viewport.y + static_cast<float>(i) * 72 - offset, r.w, 64};
                p.rect(row, t.text.with_alpha(0.04f), t.radius);
                Rect avatar{row.x + 14, row.y + 12, 40, 40};
                const auto &account = library(entry.source).account;
                bool signed_in = account.has_value();
                if (signed_in) {
                    cover(ui, avatar, account->avatar, 20, "user");
                } else {
                    p.rect(avatar, t.text.with_alpha(0.08f), 20);
                    p.icon("user", ui::center(avatar, 18, 18), t.text_dim);
                }
                ui.label({avatar.right() + 14, row.y + 12, 180, 22}, lyren::tr(entry.name), {.size = t.fs(15), .color = t.text});
                std::string_view status = signed_in                            ? std::string_view(account->name)
                                          : provider(entry.source).logged_in() ? lyren::tr("Loading account")
                                                                               : lyren::tr("Not signed in");
                ui.label({avatar.right() + 14, row.y + 34, 180, 18}, status, {.size = t.fs(12), .color = signed_in ? t.accent : t.text_dim});
                ui::Id id = ui.id(entry.id);
                const bool in = provider(entry.source).logged_in();
                const bool qr = provider(entry.source).capabilities.qr_login;
                auto label = lyren::tr(in ? "Sign out" : qr ? "Scan to sign in" : "Connect account");
                const float bw = button_width(ui, label, true);
                Rect b{row.right() - 14 - bw, row.y + 15, bw, 34};
                ui.disabled = account_importing_;
                if (in) {
                    if (ui.button(id, b, lyren::tr("Sign out"), ui::ButtonKind::ghost, "log-out")) {
                        spawn(logout(entry.source));
                    }
                } else if (ui.button(id, b, label, ui::ButtonKind::secondary, qr ? "qr-code" : "link")) {
                    start_login(ui, entry.source);
                }
                ui.disabled = false;
            }
            ui.end_scroll();
            ui.label({r.x, r.bottom() - 18, r.w, 18}, lyren::tr("Sign-ins are encrypted on this device"), {.size = t.fs(12), .color = t.text_dim});
            return;
        }

        Source source = static_cast<Source>(login_step_);
        const bool imported = provider(source).capabilities.session_import;
        ui.resize_modal(imported ? 520 : 400, std::min(imported ? 500.f : 540.f, ui.height - 40));
        if (ui.icon_button(ui.id("login-back"), {r.x - 8, r.y - 2, 32, 32}, "arrow-left", 18)) {
            cancel_login();
            login_step_ = -1;
            login_time_ = ui.now;
            return;
        }
        if (imported) {
            ui.label({r.x + 32, r.y, r.w - 64, 30}, lyren::tr("Connect account"), {.size = t.fs(20), .color = t.text});
            ui.label({r.x, r.y + 42, r.w, 20}, lyren::tr("Paste your Qishui connection settings"), {.size = t.fs(14), .color = t.text});
            ui.label({r.x, r.y + 68, r.w, 20}, lyren::tr("Use your signing service and account Cookie"), {.size = t.fs(12), .color = t.text_dim});
            ui.disabled = account_importing_;
            ui.text_area(ui.id("account-config"), {r.x, r.y + 102, r.w, r.h - 214}, account_config_, R"({"signer_url":"http://127.0.0.1:8000","cookie":"…"})");
            ui.disabled = false;
            float y = r.bottom() - 100;
            for (const auto &line : p.wrap_lines(qr_error_, t.fs(12), r.w)) {
                ui.label({r.x, y, r.w, 18}, line, {.size = t.fs(12), .color = t.danger});
                y += 18;
                if (y > r.bottom() - 56) {
                    break;
                }
            }
            ui.label({r.x, r.bottom() - 34, r.w - 110, 20}, lyren::tr("Sign-ins are encrypted on this device"), {.size = t.fs(12), .color = t.text_dim});
            ui.disabled = account_importing_ || account_config_.empty();
            if (ui.button(ui.id("account-connect"), {r.right() - 100, r.bottom() - 40, 100, 36}, lyren::tr("Connect"), ui::ButtonKind::primary, "link")) {
                spawn(import_login(source));
            }
            ui.disabled = false;
            if (account_importing_) {
                ui.spinner({r.right() - 130, r.bottom() - 30, 16, 16}, t.accent, 2);
            }
            return;
        }
        ui.label({r.x + 32, r.y, r.w - 64, 30}, lyren::tr("Scan to sign in"), {.size = t.fs(20), .color = t.text});
        ui.label({r.x, r.y + 40, r.w, 20}, lyren::tr(platform(static_cast<Source>(login_step_)).scan_hint), {.size = t.fs(13), .color = t.text_dim}, Align::center);
        Rect qr = ui::center({r.x, r.y + 76, r.w, 240}, 228, 228);
        p.shadow({qr.x, qr.y + 8, qr.w, qr.h}, 14, 30, t.shadow.with_alpha(0.6f));
        p.rect(qr, white, 14);
        QrState state = qr_status_ ? qr_status_->state : QrState::waiting;
        if (qr_image_ || (qr_size_ > 0 && !qr_.empty())) {
            // Modules as horizontal runs, so a row is a few rectangles.
            Rect q = qr.inset(14);
            ui::Color ink{0.07f, 0.07f, 0.08f, 1};
            if (qr_image_) {
                p.image(q, images_.get("qr", 512).tex, 0);
            } else {
                float m = q.w / static_cast<float>(qr_size_);
                for (int y = 0; y < qr_size_; ++y) {
                    for (int x = 0; x < qr_size_;) {
                        if (!qr_[static_cast<size_t>(y * qr_size_ + x)]) {
                            ++x;
                            continue;
                        }
                        int x1 = x;
                        while (x1 < qr_size_ && qr_[static_cast<size_t>(y * qr_size_ + x1)]) {
                            ++x1;
                        }
                        p.rect({q.x + static_cast<float>(x) * m, q.y + static_cast<float>(y) * m, static_cast<float>(x1 - x) * m, m}, ink);
                        x = x1;
                    }
                }
                Rect logo = ui::center(qr, 40, 40);
                p.rect(logo.inset(-4), white, 12);
                p.rect(logo, t.accent, 10);
                p.icon("music", ui::center(logo, 22, 22), white);
            }
            // Scanned, expired or failed: the code fades under the state.
            float veil = ui.anim(ui.id("qr-veil"), !qr_error_.empty() || state == QrState::scanned || state == QrState::expired ? 1 : 0, 240);
            if (veil > 0) {
                p.rect(qr, white.with_alpha(0.9f * veil), 14);
                Fade f(p, veil);
                if (!qr_error_.empty()) {
                    p.icon("circle-x", ui::center(qr, 48, 48), ink.with_alpha(0.7f));
                } else if (state == QrState::scanned && !qr_status_->avatar.empty()) {
                    cover(ui, ui::center(qr, 72, 72), qr_status_->avatar, 36, "user");
                } else {
                    p.icon(state == QrState::expired ? "rotate-ccw" : "circle-check", ui::center(qr, 48, 48), ink.with_alpha(0.7f));
                }
            }
        } else if (qr_error_.empty()) {
            ui.spinner(ui::center(qr, 28, 28), ui::Color{0.07f, 0.07f, 0.08f, 0.6f}, 2.5f);
        }

        float sy = qr.bottom() + 30;
        TextStyle st{.size = t.fs(14), .color = t.text};
        std::string line1, line2;
        bool spin = false;
        if (!qr_error_.empty()) {
            line1 = qr_status_ ? lyren::tr("Sign-in incomplete") : lyren::tr("Could not get a QR code"), line2 = qr_error_;
        } else if (qr_.empty() && !qr_image_) {
            line1 = lyren::tr("Generating QR code"), spin = true;
        } else if (state == QrState::waiting) {
            line1 = lyren::tr("Waiting for scan"), line2 = lyren::tr("Scan, then confirm on your phone"), spin = true;
        } else if (state == QrState::scanned) {
            line1 = lyren::tr("Scanned"),
            line2 = qr_status_->nickname.empty() ? lyren::tr("Confirm sign-in on your phone") : lyren::trf("{}, confirm sign-in on your phone", qr_status_->nickname), spin = true;
        } else if (state == QrState::expired) {
            line1 = lyren::tr("QR code expired"), line2 = lyren::tr("Refresh to scan again");
        } else {
            line1 = lyren::tr("Signing in"), spin = true;
        }
        float tw = p.measure(line1, st);
        float sx = r.x + (r.w - tw - (spin ? 26 : 0)) / 2;
        if (spin) {
            ui.spinner({sx, sy + 2, 16, 16}, t.accent, 2);
        }
        ui.label({sx + (spin ? 26 : 0), sy, tw + 1, 20}, line1, st);
        ui.label({r.x, sy + 30, r.w, 20}, line2, {.size = t.fs(12), .color = t.text_dim}, Align::center);
        float refresh_w = button_width(ui, lyren::tr("Refresh QR code"), true);
        if (ui.button(ui.id("qr-refresh"), {r.x + (r.w - refresh_w) / 2, r.bottom() - 36, refresh_w, 34}, lyren::tr("Refresh QR code"), ui::ButtonKind::secondary, "refresh-cw")) {
            spawn(qr_login());
        }
    }

} // namespace lyren
