#include <vorbis/vorbisfile.h>

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
                format = {uint32_t(vi->rate), uint32_t(vi->channels)};
                ogg_int64_t n = ov_pcm_total(&vf_, -1);
                length = n > 0 ? uint64_t(n) : 0;
                codec = "vorbis";
                return true;
            }

            size_t read(float *out, size_t frames) override {
                size_t ch = format.channels, done = 0;
                while (done < frames) {
                    float **pcm;
                    int section;
                    long n = ov_read_float(&vf_, &pcm, int(frames - done), &section);
                    if (n == OV_HOLE) {
                        continue;
                    }
                    if (n <= 0) {
                        break;
                    }
                    for (long i = 0; i < n; ++i) {
                        for (size_t c = 0; c < ch; ++c) {
                            out[(done + size_t(i)) * ch + c] = pcm[c][i];
                        }
                    }
                    done += size_t(n);
                }
                return done;
            }

            bool seek(uint64_t frame) override {
                return ov_pcm_seek(&vf_, ogg_int64_t(frame)) == 0;
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
                int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(d->pos_) : int64_t(d->s_.size());
                d->pos_ = uint64_t(base + off);
                return 0;
            }
            static long tell_cb(void *ud) {
                return long(static_cast<Vorbis *>(ud)->pos_);
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
