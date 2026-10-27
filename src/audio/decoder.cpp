#include "audio/decoder.hpp"

#include <array>
#include <string_view>

#include "audio/spool.hpp"

namespace lyren::audio {

    std::unique_ptr<Decoder> open_decoder(Spool &spool, std::string &error) {
        std::array<char, 12> h;
        uint64_t at = 0;
        if (!spool.read_exact(0, h.data(), h.size())) {
            error = spool.error().empty() ? "file too short" : spool.error();
            return nullptr;
        }
        std::string_view header(h.data(), h.size());
        if (header.starts_with("ID3")) {
            // Skip the tag to see what follows; FLAC files from some CDNs carry one too.
            at = 10 + (uint64_t(h[6] & 0x7f) << 21 | uint64_t(h[7] & 0x7f) << 14 | uint64_t(h[8] & 0x7f) << 7 | (h[9] & 0x7f));
            if (h[5] & 0x10) {
                at += 10;
            }
            if (!spool.read_exact(at, h.data(), h.size())) {
                error = spool.error().empty() ? "file too short" : spool.error();
                return nullptr;
            }
        }
        std::unique_ptr<Decoder> d;
        if (header.starts_with("fLaC")) {
            d = open_flac(spool);
        } else if (header.starts_with("OggS")) {
            d = open_vorbis(spool);
        } else if (header.substr(4).starts_with("ftyp")) {
            d = open_mp4(spool, error);
        } else if (at || (uint8_t(h[0]) == 0xff && (h[1] & 0xe0) == 0xe0 && (h[1] & 0x06))) {
            d = open_mp3(spool);
        } else {
            error = "unsupported format";
        }
        if (!d && error.empty()) {
            error = spool.error().empty() ? "cannot decode" : spool.error();
        }
        return d;
    }

} // namespace lyren::audio
