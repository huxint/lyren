#pragma once
#include <concepts>
#include <functional>
#include <string>
#include <vector>

#include "app/images.hpp"
#include "core/loop.hpp"
#include "media/provider.hpp"
#include "net/http.hpp"
#include "sys/config.hpp"

namespace lyren {

    // What every part of the app works with: the loop, the network, the platforms, cover art, the settings and the UI's
    // clock. Notes for the user gather here until the next frame shows them.
    struct Env {
        struct Notice {
            std::string text, icon;
            bool error = false;
        };

        Loop &loop;
        net::Http &http;
        Providers providers;
        Images &images;
        const Config &config;
        std::move_only_function<void()> redraw; // something on show changed outside a frame
        std::move_only_function<double()> now;  // the UI clock, s
        std::vector<Notice> notices;

        Provider &provider(Source source) const {
            return providers[static_cast<size_t>(source)].get();
        }
        // The platform a track, a card or anything else with a source comes from.
        template <class T>
            requires requires(const T &t) {
                { t.source } -> std::convertible_to<Source>;
            }
        Provider &provider(const T &t) const {
            return provider(t.source);
        }
        bool signed_in(Source source) const {
            return provider(source).logged_in();
        }
        void notify(std::string text, std::string icon = "info", bool error = false) {
            notices.push_back({std::move(text), std::move(icon), error});
            redraw();
        }
    };

} // namespace lyren
