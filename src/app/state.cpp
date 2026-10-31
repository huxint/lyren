#include "app/state.hpp"

#include <glaze/glaze.hpp>

#include <cstdio>
#include <fstream>
#include <algorithm>
#include <utility>

#include "core/log.hpp"
#include "sys/paths.hpp"

namespace lyren {

    Result<SavedState> parse_state(std::string_view text) {
        SavedState s;
        if (auto e = glz::read<glz::opts{.error_on_unknown_keys = false}>(s, text)) {
            return std::unexpected(glz::format_error(e, text));
        }
        auto registered = [](Source source) {
            return std::to_underlying(source) < platforms.size();
        };
        auto valid_track = [&](const Track &track) {
            return registered(track.source);
        };
        if (!registered(s.source) || !std::ranges::all_of(s.queue, valid_track) || !std::ranges::all_of(s.recent, valid_track)) {
            return std::unexpected("Unknown platform in saved state");
        }
        if (std::to_underlying(s.mode) > std::to_underlying(PlayMode::shuffle)) {
            return std::unexpected("Unknown playback mode in saved state");
        }
        for (const auto snap : {s.lyrics_snap, s.island_snap}) {
            if (std::to_underlying(snap.x) > std::to_underlying(DesktopAnchor::end) || std::to_underlying(snap.y) > std::to_underlying(DesktopAnchor::end)) {
                return std::unexpected("Unknown desktop lyrics anchor in saved state");
            }
        }
        if (s.current < -1 || s.current >= static_cast<int>(s.queue.size())) {
            s.current = -1;
        }
        s.width = std::max(0, s.width), s.height = std::max(0, s.height);
        s.volume = std::clamp(s.volume, 0.f, 1.f);
        s.position = std::max(0.0, s.position);
        s.lyrics_bottom = std::max(0.f, s.lyrics_bottom);
        s.island_y = std::max(0.f, s.island_y); // screen bounds are applied when drawing
        if (s.recent.size() > 200) {
            s.recent.resize(200);
        }
        return s;
    }

    SavedState load_state() {
        std::ifstream in(paths::state_dir() / "state.json");
        if (!in) {
            return {};
        }
        std::string text{std::istreambuf_iterator<char>(in), {}};
        auto state = parse_state(text);
        if (!state) {
            log::warn("state.json: {}", state.error());
            return {};
        }
        return std::move(*state);
    }

    void save_state(const SavedState &s) {
        std::string text;
        static_cast<void>(glz::write_json(s, text));
        auto path = paths::state_dir() / "state.json", tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << text;
            if (!out) {
                return log::warn("cannot write {}", tmp.string());
            }
        }
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            log::warn("cannot replace {}", path.string());
        }
    }

} // namespace lyren
