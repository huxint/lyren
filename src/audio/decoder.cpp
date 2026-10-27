#include "audio/decoder.hpp"

#include <cstring>

#include "audio/spool.hpp"

namespace lyren::audio {

    std::unique_ptr<Decoder> open_decoder(Spool &spool, std::string &error) {
        unsigned char h[12];
        uint64_t at = 0;
        if (!spool.read_exact(0, h, sizeof h)) {
            error = spool.error().empty() ? "file too short" : spool.error();
            return nullptr;
        }
        if (!std::memcmp(h, "ID3", 3)) {
            // Skip the tag to see what follows; FLAC files from some CDNs carry one too.
            at = 10 + (uint64_t(h[6] & 0x7f) << 21 | uint64_t(h[7] & 0x7f) << 14 | uint64_t(h[8] & 0x7f) << 7 | (h[9] & 0x7f));
            if (h[5] & 0x10) {
                at += 10;
            }
            if (!spool.read_exact(at, h, sizeof h)) {
                error = spool.error().empty() ? "file too short" : spool.error();
                return nullptr;
            }
        }
        std::unique_ptr<Decoder> d;
        if (!std::memcmp(h, "fLaC", 4)) {
            d = open_flac(spool);
        } else if (!std::memcmp(h, "OggS", 4)) {
            d = open_vorbis(spool);
        } else if (!std::memcmp(h + 4, "ftyp", 4)) {
            d = open_mp4(spool, error);
        } else if (at || (h[0] == 0xff && (h[1] & 0xe0) == 0xe0 && (h[1] & 0x06))) {
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
