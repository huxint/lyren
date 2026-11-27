#pragma once
#include <optional>
#include <string>

#include "app/env.hpp"
#include "media/provider.hpp"
#include "media/types.hpp"

namespace lyren {

    // Confidence of matching a candidate track from another platform against a target track.
    struct MatchScore {
        bool acceptable = false;
        int score = 0;
        int duration_diff = 0; // absolute seconds difference when both durations are known
    };

    struct FallbackMatch {
        Track track;
        Stream stream;
    };

    // Compact search query for cross-platform lookup ("<title> <primary artist>").
    std::string match_query(const Track &target);

    // Scores candidate against target using title, artist overlap, version qualifiers, and duration tolerance (<= 6 s).
    MatchScore score_match(const Track &target, const Track &candidate);

    // Searches other registered platforms for a playable stream matching target.
    Task<std::optional<FallbackMatch>> find_fallback_stream(Env &env, const Track &target, Quality quality, bool skip_trials);

} // namespace lyren
