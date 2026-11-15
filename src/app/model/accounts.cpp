#include "app/model/accounts.hpp"

#include <qrencode.h>

#include <format>

#include "app/model/browser.hpp"
#include "app/model/library.hpp"
#include "core/i18n.hpp"
#include "gfx/image.hpp"

namespace lyren {

    Accounts::Accounts(Env &env, Library &library, Browser &browser)
    : env_(env),
      library_(library),
      browser_(browser) {
        for (const auto &entry : platforms) {
            env_.provider(entry.source).on_session_changed([this, source = entry.source] {
                const auto &shelf = library_.shelf(source);
                // The service ended the session: what the account showed goes with it.
                if (!env_.signed_in(source) && (shelf.account || shelf.preference_gen != 0)) {
                    if (shelf.account) {
                        env_.notify(std::format("{}: {}", lyren::tr(platform(source).name), lyren::tr("Session expired; sign in again")), "user", true);
                    }
                    library_.reset(source);
                    if (source == browser_.active()) {
                        browser_.ensure();
                    }
                }
                save();
            });
        }
        library.subscribe([this](const Library::Event &event) {
            if (event.kind == Library::Event::Kind::signed_out) {
                save();
            }
        });
        spawn(startup());
    }

    Accounts::~Accounts() {
        env_.loop.cancel(save_timer_);
    }

    Task<void> Accounts::startup() {
        auto saved = co_await creds::load(env_.loop);
        if (saved) {
            credentials_ = creds::from_json(*saved);
            for (const auto &entry : platforms) {
                env_.provider(entry.source).session() = std::move(credentials_[std::string(entry.id)]);
            }
            can_save_ = true;
        } else {
            env_.notify(lyren::trf("Could not read saved credentials: {}", saved.error()), "triangle-alert", true);
        }
        for (const auto &entry : platforms) {
            spawn(initialize(entry.source));
        }
        save();
    }

    Task<void> Accounts::initialize(Source source) {
        auto r = co_await env_.provider(source).initialize();
        if (!r) {
            env_.notify(std::format("{}: {}", lyren::tr(platform(source).name), r.error()), "user", true);
        }
        if (source == browser_.active()) {
            browser_.ensure();
            spawn(browser_.warm(source));
        }
        if (env_.signed_in(source)) {
            co_await library_.load(source);
        }
    }

    void Accounts::save() {
        if (!can_save_) {
            return;
        }
        // Responses often set several cookies in a row; write once they settle.
        env_.loop.cancel(save_timer_);
        save_timer_ = env_.loop.after(1s, [this] {
            save_timer_ = 0;
            spawn([](Accounts *self) -> Task<void> {
                for (const auto &entry : platforms) {
                    self->credentials_[std::string(entry.id)] = self->env_.provider(entry.source).session();
                }
                auto r = co_await creds::save(self->env_.loop, creds::to_json(self->credentials_));
                if (!r) {
                    self->env_.notify(lyren::trf("Could not save credentials: {}", r.error()), "triangle-alert", true);
                }
            }(this));
        });
    }

    void Accounts::begin(Source source) {
        sign_in_ = {.source = source, .since = env_.now()};
        if (env_.provider(source).capabilities.qr_login) {
            spawn(scan());
        }
    }

    void Accounts::cancel() {
        ++gen_;
        if (sign_in_.source) {
            env_.provider(*sign_in_.source).cancel_login();
        }
        env_.images.erase("qr");
        sign_in_ = {.since = env_.now()};
    }

    void Accounts::refresh_code() {
        if (sign_in_.source) {
            spawn(scan());
        }
    }

    void Accounts::import_settings() {
        if (sign_in_.source && !sign_in_.importing) {
            spawn(import(*sign_in_.source));
        }
    }

    void Accounts::sign_out(Source source) {
        spawn(library_.sign_out(source));
    }

    Task<void> Accounts::arrived(Source source, uint64_t gen) {
        can_save_ = true;
        library_.reset(source);
        save();
        co_await library_.load(source);
        if (gen != gen_) {
            co_return;
        }
        sign_in_ = {.since = env_.now()};
        env_.images.erase("qr");
        const auto &account = library_.shelf(source).account;
        if (account) {
            env_.notify(lyren::trf("Signed in as {}", account->name), "circle-check");
        } else if (env_.provider(source).capabilities.session_import) {
            env_.notify(std::string(lyren::tr("Connection settings saved")), "circle-check");
        }
        if (source == browser_.active()) {
            browser_.ensure();
        }
        spawn(browser_.warm(source));
        env_.redraw();
    }

    Task<void> Accounts::import(Source source) {
        const uint64_t gen = ++gen_;
        sign_in_.importing = true;
        sign_in_.error.clear();
        auto result = co_await env_.provider(source).import_session(sign_in_.settings);
        if (gen != gen_) {
            co_return;
        }
        sign_in_.importing = false;
        if (!result) {
            sign_in_.error = result.error();
            co_return env_.redraw();
        }
        co_await arrived(source, gen);
    }

    Task<void> Accounts::scan() {
        const uint64_t gen = ++gen_;
        const Source source = *sign_in_.source;
        Provider &provider = env_.provider(source);
        provider.cancel_login();
        env_.images.erase("qr");
        sign_in_ = {.source = source, .since = sign_in_.since};
        auto code = co_await provider.qr_start();
        if (gen != gen_) {
            co_return;
        }
        if (!code) {
            sign_in_.error = code.error();
            co_return env_.redraw();
        }
        if (!code->image.empty()) {
            auto picture = gfx::decode_square({reinterpret_cast<const uint8_t *>(code->image.data()), code->image.size()}, 512, sign_in_.error);
            if (!picture.w) {
                co_return env_.redraw();
            }
            env_.images.put("qr", 512, picture);
            sign_in_.image = true;
        } else if (QRcode *q = QRcode_encodeString(code->text.c_str(), 0, QR_ECLEVEL_H, QR_MODE_8, 1)) {
            sign_in_.size = q->width;
            sign_in_.modules.resize(static_cast<size_t>(q->width) * static_cast<size_t>(q->width));
            for (size_t i = 0; i < sign_in_.modules.size(); ++i) {
                sign_in_.modules[i] = q->data[i] & 1;
            }
            QRcode_free(q);
        } else {
            sign_in_.error = lyren::tr("Could not get a QR code");
            co_return env_.redraw();
        }
        sign_in_.status = QrStatus{};
        env_.redraw();
        while (true) {
            co_await sleep{env_.loop, 2s};
            if (gen != gen_) {
                co_return;
            }
            auto status = co_await provider.qr_poll();
            if (gen != gen_) {
                co_return;
            }
            if (!status) {
                sign_in_.error = status.error();
                co_return env_.redraw();
            }
            sign_in_.status = *status;
            env_.redraw();
            if (status->state == QrState::expired) {
                co_return;
            }
            if (status->state == QrState::confirmed) {
                break;
            }
        }
        co_await arrived(source, gen);
    }

} // namespace lyren
