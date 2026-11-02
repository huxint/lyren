#pragma once
#include "media/lyrics.hpp"
#include "media/quality.hpp"
#include "media/types.hpp"
#include "qishui/client.hpp"

namespace lyren::qishui {

    std::vector<LyricLine> parse_lyrics(std::string_view text, std::string_view format = {});
    Result<crypto::Aes128Key> stream_key(std::string_view play_auth);
    Result<Stream> parse_stream(const Json &response, Quality quality, uint32_t track_duration);

} // namespace lyren::qishui
