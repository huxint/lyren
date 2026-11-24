#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <limits>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"

namespace lyren::audio {

    namespace {

        class Vorbis final : public Decoder {
        public:
            explicit Vorbis(Spool &s)
            : s_(s) {}
            ~Vorbis() override {
                if (open_) {
                    ov_clear(&vf_);
                }
            }

            bool open() {
                if (ov_open_callbacks(this, &vf_, nullptr, 0, {read_cb, seek_cb, nullptr, tell_cb}) != 0) {
                    return false;
                }
                open_ = true;
                vorbis_info *vi = ov_info(&vf_, -1);
                if (!vi || vi->channels <= 0 || vi->rate <= 0) {
                    return false;
                }
                format = {static_cast<uint32_t>(vi->rate), static_cast<uint32_t>(vi->channels)};
                ogg_int64_t n = ov_pcm_total(&vf_, -1);
                length = n > 0 ? static_cast<uint64_t>(n) : 0;
                codec = "vorbis";
                return true;
            }

            size_t read(float *out, size_t frames) override {
                if (!error.empty()) {
                    return 0;
                }
                size_t ch = format.channels, done = 0;
                int holes = 0;
                while (done < frames) {
                    float **pcm = nullptr;
                    int section = 0;
                    long n = ov_read_float(&vf_, &pcm, static_cast<int>(frames - done), &section);
                    if (n == OV_HOLE) {
                        if (++holes > 64) {
                            error = "corrupt vorbis stream";
                            break;
                        }
                        continue;
                    }
                    holes = 0;
                    if (n < 0) {
                        error = "corrupt vorbis stream";
                        break;
                    }
                    if (n == 0) {
                        break;
                    }
                    vorbis_info *vi = ov_info(&vf_, section);
                    if (!vi || vi->channels <= 0 || static_cast<uint32_t>(vi->rate) != format.rate) {
                        error = "vorbis stream sample rate changed";
                        break;
                    }
                    size_t src = static_cast<size_t>(vi->channels);
                    for (long i = 0; i < n; ++i) {
                        for (size_t c = 0; c < ch; ++c) {
                            out[(done + static_cast<size_t>(i)) * ch + c] = pcm[std::min(c, src - 1)][i];
                        }
                    }
                    done += static_cast<size_t>(n);
                }
                return done;
            }

            bool seek(uint64_t frame) override {
                if (frame > static_cast<uint64_t>(std::numeric_limits<ogg_int64_t>::max()) || (length > 0 && frame > length)) {
                    return false;
                }
                error.clear();
                return ov_pcm_seek(&vf_, static_cast<ogg_int64_t>(frame)) == 0;
            }

        private:
            static size_t read_cb(void *buf, size_t size, size_t n, void *ud) {
                auto *d = static_cast<Vorbis *>(ud);
                size_t got = d->s_.read(d->pos_, buf, size * n);
                d->pos_ += got;
                return got / size;
            }
            static int seek_cb(void *ud, ogg_int64_t off, int whence) {
                auto *d = static_cast<Vorbis *>(ud);
                const auto size = static_cast<int64_t>(d->s_.size());
                const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(d->pos_) : whence == SEEK_END ? size : -1;
                if (base < 0 || off < -base || off > size - base) {
                    return -1;
                }
                d->pos_ = static_cast<uint64_t>(base + off);
                return 0;
            }
            static long tell_cb(void *ud) {
                return static_cast<long>(static_cast<Vorbis *>(ud)->pos_);
            }

            Spool &s_;
            OggVorbis_File vf_{};
            bool open_ = false;
            uint64_t pos_ = 0;
        };

    } // namespace

    std::unique_ptr<Decoder> open_vorbis(Spool &spool) {
        auto d = std::make_unique<Vorbis>(spool);
        return d->open() ? std::move(d) : nullptr;
    }

} // namespace lyren::audio
