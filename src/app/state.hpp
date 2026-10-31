#pragma once
#include <vector>
#include <string_view>

#include "core/result.hpp"
#include "media/types.hpp"
#include "app/desktop_snap.hpp"

namespace lyren {

    // Non-secret state kept across runs in ~/.local/state/lyren/state.json.
    struct SavedState {
        int width = 0, height = 0; // nonnegative logical pixels; zero means no saved size
        Source source = Source::netease;
        float volume = 0.7f; // slider position in [0, 1]
        bool muted = false;
        PlayMode mode = PlayMode::list;
        std::vector<Track> queue;
        int current = -1;          // -1 without a selection, otherwise [0, queue.size())
        double position = 0;       // nonnegative seconds within the selected file
        std::vector<Track> recent; // newest first, at most 200 across all platforms
        // Desktop lyrics: offset of the text from the screen's centre, height above the bottom, click-through.
        float lyrics_x = 0, lyrics_bottom = 48; // signed horizontal offset, nonnegative bottom margin; logical pixels
        // The island's offset from the centre of the screen and from its top edge; logical pixels.
        float island_x = 0, island_y = 10;
        DesktopSnap lyrics_snap, island_snap;
        bool lyrics_locked = false;
        bool lyrics_enabled = false, lyrics_island = false, lyrics_translation = true;
    };

    Result<SavedState> parse_state(std::string_view text); // validates and normalizes persisted input
    SavedState load_state();
    void save_state(const SavedState &s); // atomic replace

} // namespace lyren
