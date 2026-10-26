#pragma once
#include <vector>

#include "app/provider.hpp"

namespace lyren {

// Non-secret state kept across runs in ~/.local/state/lyren/state.json.
struct SavedState {
    int width = 0, height = 0;  // window, when it was not maximized
    Source source = Source::netease;
    float volume = 0.7f;
    bool muted = false;
    int mode = 0;  // repeat list, repeat one, shuffle
    std::vector<Track> queue;
    int current = -1;
    double position = 0;
    std::vector<Track> recent;
    // Desktop lyrics: offset of the text from the screen's centre, height above the bottom, click-through.
    float lyrics_x = 0, lyrics_bottom = 48;
    bool lyrics_locked = false;
};

SavedState load_state();
void save_state(const SavedState& s);  // atomic replace

}  // namespace lyren
