#pragma once
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/store.hpp"
#include "core/loop.hpp"
#include "media/provider.hpp"
#include "net/http.hpp"
#include "sys/config.hpp"

namespace lyren {

    // What every part of the app works with: the loop, the network, the platforms, the settings and the UI's
    // clock. Notes for the user gather here until the next frame shows them.
    struct Env {
        struct Notice {
            std::string text, icon;
            bool error = false;
        };

        Loop &loop;
        net::Http &http;
        Providers providers;
        const Config &config;
        std::move_only_function<void()> redraw; // something on show changed outside a frame
        std::move_only_function<double()> now;  // the UI clock, s
        // Optional cover-art prefetch hook provided by the UI layer (empty in headless tests).
        std::move_only_function<void(std::string_view url, uint32_t px, bool logical)> prefetch_image;
        Store *store = nullptr; // incremental SQLite persistence (optional in isolated unit tests)
        std::vector<Notice> notices;

        Provider &provider(Source source, std::string_view instance = {}) const {
            return providers.get(source, instance);
        }
        Provider &provider(std::string_view id) const {
            return providers.get(id);
        }
        // The platform a track, a card or anything else with a source comes from.
        template <class T>
            requires requires(const T &t) {
                { t.source } -> std::convertible_to<Source>;
            }
        Provider &provider(const T &t) const {
            if constexpr (requires {
                              { t.instance } -> std::convertible_to<std::string_view>;
                          }) {
                return provider(t.source, t.instance);
            } else {
                return provider(t.source);
            }
        }
        bool has_account(Source source, std::string_view instance = {}) const {
            return provider(source, instance).capabilities.accounts;
        }
        bool signed_in(Source source, std::string_view instance = {}) const {
            const auto &p = provider(source, instance);
            return !p.capabilities.accounts || p.logged_in();
        }
        void prefetch(std::string_view url, uint32_t px) {
            if (prefetch_image) {
                prefetch_image(url, px, false);
            }
        }
        void prefetch(std::span<const Track> tracks) {
            if (!prefetch_image) {
                return;
            }
            for (const auto &track : tracks.first(std::min(tracks.size(), 24uz))) {
                prefetch_image(track.cover, 40, true);
            }
        }
        void prefetch(std::span<const Card> cards) {
            if (!prefetch_image) {
                return;
            }
            for (const auto &card : cards.first(std::min(cards.size(), 12uz))) {
                prefetch_image(card.cover, 160, true);
            }
        }
        void notify(std::string text, std::string icon = "info", bool error = false) {
            notices.push_back({std::move(text), std::move(icon), error});
            redraw();
        }
    };

} // namespace lyren
