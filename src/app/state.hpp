#pragma once
#include <vector>

#include "app/provider.hpp"

namespace lyren {

// Non-secret state kept across runs in ~/.local/state/lyren/state.json.
struct SavedState {
    int width = 0, height = 0;  // window, when it was not maximized
    float volume = 0.7f;
    bool muted = false;
    int mode = 0;  // repeat list, repeat one, shuffle
    std::vector<Track> queue;
    int current = -1;
    double position = 0;
    std::vector<Track> recent;
};

SavedState load_state();
void save_state(const SavedState& s);  // atomic replace

}  // namespace lyren
