#include "app/model/accounts.hpp"

#include <glaze/glaze.hpp>
#include <qrencode.h>

#include <format>

#include "app/model/browser.hpp"
#include "app/model/library.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/string_map.hpp"

namespace lyren {

    Accounts::Accounts(Env &env, Library &library, Browser &browser)
    : env_(env),
      library_(library),
      browser_(browser) {
        for (const auto &entry : platforms) {
            env_.provider(entry.source).on_session_changed([this, source = entry.source] {
                const auto &shelf = library_.shelf(source);
                const bool ended = !env_.signed_in(source);
                // The service ended the session: what the account showed goes with it.
                if (ended && (shelf.account || shelf.preference_gen != 0)) {
                    if (shelf.account) {
                        env_.notify(std::format("{}: {}", lyren::tr(platform(source).name), lyren::tr("Session expired; sign in again")), "user", true);
                    }
                    library_.reset(source);
                    if (source == browser_.active()) {
                        browser_.ensure();
                    }
                }
                save(ended);
            });
        }
        library.subscribe([this](const Library::Event &event) {
            if (event.kind == Library::Event::Kind::signed_out) {
                save(true);
            }
        });
        spawn(startup());
    }

    Accounts::~Accounts() {
        for (const auto &entry : platforms) {
            env_.provider(entry.source).on_session_changed(nullptr);
        }
        flush();
    }

    Task<void> Accounts::startup() {
        auto saved = co_await creds::load(env_.loop);
        if (saved) {
            credentials_ = creds::from_json(*saved);
            for (const auto &entry : platforms) {
                env_.provider(entry.source).session() = credentials_[std::string(entry.id)];
            }
            saved_json_ = creds::to_json(credentials_);
            can_save_ = true;
        } else {
            env_.notify(lyren::trf("Could not read saved credentials: {}", saved.error()), "triangle-alert", true);
        }
        for (const auto &entry : platforms) {
            spawn(initialize(entry.source));
        }
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

    std::string Accounts::snapshot() {
        for (const auto &entry : platforms) {
            credentials_[std::string(entry.id)] = env_.provider(entry.source).session();
        }
        return creds::to_json(credentials_);
    }

    Task<void> Accounts::write() {
        saving_ = true;
        while (dirty_) {
            const uint64_t gen = save_gen_;
            std::string json = snapshot();
            if (json == saved_json_) {
                if (gen == save_gen_) {
                    dirty_ = false;
                }
                continue;
            }
            auto r = co_await creds::save(env_.loop, json);
            if (!r) {
                saving_ = false;
                env_.notify(lyren::trf("Could not save credentials: {}", r.error()), "triangle-alert", true);
                co_return;
            }
            saved_json_ = std::move(json);
            if (gen == save_gen_) {
                dirty_ = false;
            }
        }
        saving_ = false;
    }

    void Accounts::save(bool immediate) {
        if (!can_save_) {
            return;
        }
        dirty_ = true;
        ++save_gen_;
        env_.loop.cancel(save_timer_);
        save_timer_ = 0;
        if (immediate) {
            if (!saving_) {
                spawn(write());
            }
            return;
        }
        // Routine responses often set several cookies in a row; write once they settle.
        save_timer_ = env_.loop.after(1s, [this] {
            save_timer_ = 0;
            if (!saving_) {
                spawn(write());
            }
        });
    }

    void Accounts::flush() {
        env_.loop.cancel(save_timer_);
        save_timer_ = 0;
        if (!can_save_ || !dirty_) {
            return;
        }
        std::string json = snapshot();
        if (json == saved_json_) {
            dirty_ = false;
            return;
        }
        if (auto r = creds::save_sync(json); !r) {
            log::warn("credentials: {}", r.error());
        } else {
            saved_json_ = std::move(json);
            dirty_ = false;
        }
    }

    bool Accounts::signer_connected(Source source) const {
        if (env_.signed_in(source) || !env_.provider(source).capabilities.session_import) {
            return false;
        }
        const auto &session = env_.provider(source).session();
        const auto it = session.find("signer_url");
        return it != session.end() && !it->second.empty();
    }

    bool Accounts::unpack_json_settings(std::string_view json_text) {
        const auto first = json_text.find_first_not_of(" \t\r\n");
        if (first == json_text.npos || json_text[first] != '{') {
            return false;
        }
        const auto last = json_text.find_last_not_of(" \t\r\n");
        if (json_text[last] != '}') {
            return false;
        }
        StringMap<std::string> map;
        if (glz::read_json(map, json_text.substr(first, last - first + 1))) {
            return false;
        }
        auto first_val = [&](std::string_view a, std::string_view b) {
            const auto v = get(map, a);
            return std::string(v.empty() ? get(map, b) : v);
        };
        sign_in_.signer_url = std::string(get(map, "signer_url"));
        sign_in_.signer_token = std::string(get(map, "signer_token"));
        sign_in_.cookie = first_val("cookie", "cookie_jar");
        sign_in_.device_id = first_val("device_id", "custom_device_id");
        sign_in_.iid = first_val("iid", "custom_iid");
        sign_in_.fp = first_val("fp", "custom_fp");
        if (!sign_in_.device_id.empty() || !sign_in_.iid.empty() || !sign_in_.fp.empty()) {
            sign_in_.show_advanced = true;
        }
        sign_in_.error.clear();
        sign_in_.check.reset();
        sign_in_.settings = compose_settings();
        return true;
    }

    std::string Accounts::compose_settings() const {
        if (!sign_in_.structured && sign_in_.signer_url.empty() && sign_in_.signer_token.empty() && sign_in_.cookie.empty() && sign_in_.device_id.empty() && sign_in_.iid.empty() &&
            sign_in_.fp.empty()) {
            return sign_in_.settings;
        }
        StringMap<std::string> out;
        out["signer_url"] = sign_in_.signer_url;
        if (!sign_in_.signer_token.empty()) {
            out["signer_token"] = sign_in_.signer_token;
        }
        if (!sign_in_.cookie.empty()) {
            out["cookie"] = sign_in_.cookie;
        }
        if (!sign_in_.device_id.empty()) {
            out["device_id"] = sign_in_.device_id;
        }
        if (!sign_in_.iid.empty()) {
            out["iid"] = sign_in_.iid;
        }
        if (!sign_in_.fp.empty()) {
            out["fp"] = sign_in_.fp;
        }
        return glz::write_json(out).value_or("{}");
    }

    void Accounts::begin(Source source) {
        sign_in_ = {.source = source, .since = env_.now()};
        auto &p = env_.provider(source);
        if (p.capabilities.session_import) {
            sign_in_.structured = true;
            const auto &s = p.session();
            sign_in_.signer_url = std::string(get(s, "signer_url"));
            sign_in_.signer_token = std::string(get(s, "signer_token"));
            sign_in_.cookie = std::string(get(s, "cookie_jar"));
            sign_in_.device_id = std::string(get(s, "custom_device_id"));
            sign_in_.iid = std::string(get(s, "custom_iid"));
            sign_in_.fp = std::string(get(s, "custom_fp"));
            sign_in_.show_advanced = !sign_in_.device_id.empty() || !sign_in_.iid.empty() || !sign_in_.fp.empty();
        } else if (p.capabilities.qr_login) {
            spawn(scan());
        }
    }

    void Accounts::cancel() {
        ++gen_;
        if (sign_in_.source) {
            env_.provider(*sign_in_.source).cancel_login();
        }
        sign_in_ = {.since = env_.now()};
    }

    void Accounts::refresh_code() {
        if (sign_in_.source) {
            spawn(scan());
        }
    }

    void Accounts::import_settings() {
        if (sign_in_.source && !sign_in_.importing) {
            sign_in_.settings = compose_settings();
            spawn(import(*sign_in_.source));
        }
    }

    void Accounts::test_settings() {
        if (sign_in_.source && !sign_in_.importing && !sign_in_.checking) {
            sign_in_.settings = compose_settings();
            spawn(check(*sign_in_.source));
        }
    }

    void Accounts::sign_out(Source source) {
        if (!env_.signed_in(source) && signer_connected(source)) {
            spawn([](Accounts *self, Source source) -> Task<void> {
                self->library_.reset(source);
                co_await self->env_.provider(source).logout();
                self->save(true);
                if (source == self->browser_.active()) {
                    self->browser_.ensure();
                }
                self->env_.notify(std::string(lyren::tr("Disconnected signing service")), "log-out");
                self->env_.redraw();
            }(this, source));
            return;
        }
        spawn(library_.sign_out(source));
    }

    Task<void> Accounts::arrived(Source source, uint64_t gen) {
        can_save_ = true;
        library_.reset(source);
        save(true);
        co_await library_.load(source);
        if (gen != gen_) {
            co_return;
        }
        sign_in_ = {.since = env_.now()};
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

    Task<void> Accounts::check(Source source) {
        const uint64_t gen = ++gen_;
        sign_in_.checking = true;
        sign_in_.error.clear();
        sign_in_.check = SessionCheck{};
        env_.redraw();
        auto result = co_await env_.provider(source).check_session(sign_in_.settings);
        if (gen != gen_) {
            co_return;
        }
        sign_in_.checking = false;
        sign_in_.check = std::move(result);
        env_.redraw();
    }

    Task<void> Accounts::scan() {
        const uint64_t gen = ++gen_;
        const Source source = *sign_in_.source;
        Provider &provider = env_.provider(source);
        provider.cancel_login();
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
            sign_in_.image_bytes = std::move(code->image);
            sign_in_.image_gen = ++qr_image_gen_;
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
