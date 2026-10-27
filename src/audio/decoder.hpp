#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lyren::audio {

    class Spool;

    struct Format {
        uint32_t rate = 0, channels = 0;
        bool operator==(const Format &) const = default;
    };

    class Decoder {
    public:
        virtual ~Decoder() = default;
        // Interleaved float frames; 0 at the end of the stream or on a broken stream.
        virtual size_t read(float *out, size_t frames) = 0;
        virtual bool seek(uint64_t frame) = 0;

        Format format;
        uint64_t length = 0; // frames, 0 if unknown
        std::string_view codec;
    };

    // Picks the decoder by the file's magic bytes. Blocks while the header downloads.
    std::unique_ptr<Decoder> open_decoder(Spool &spool, std::string &error);

    std::unique_ptr<Decoder> open_mp3(Spool &spool);
    std::unique_ptr<Decoder> open_flac(Spool &spool);
    std::unique_ptr<Decoder> open_vorbis(Spool &spool);
    std::unique_ptr<Decoder> open_mp4(Spool &spool, std::string &error);

} // namespace lyren::audio
