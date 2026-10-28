#include <mpg123.h>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"

namespace lyren::audio {

    namespace {

        class Mp3 final : public Decoder {
        public:
            explicit Mp3(Spool &s)
            : s_(s) {}
            ~Mp3() override {
                mpg123_close(h_);
                mpg123_delete(h_);
            }

            bool open() {
                if (!h_) {
                    return false;
                }
                // Exact seeks keep audio and lyrics on the same sample timeline. A fuzzy Xing seek can be off by a second.
                // Supplying the file size avoids a separate request for a trailing ID3 tag.
                mpg123_param2(h_, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_NO_PEEK_END | MPG123_FORCE_SEEKABLE, 0);
                // Low-bitrate VBR frames need more history to refill the Layer III bit reservoir after a seek.
                mpg123_param2(h_, MPG123_PREFRAMES, 32, 0);
                mpg123_format_none(h_);
                const long *rates;
                size_t n;
                mpg123_rates(&rates, &n);
                for (size_t i = 0; i < n; ++i) {
                    mpg123_format(h_, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);
                }
                mpg123_reader64(h_, read_cb, seek_cb, nullptr);
                long rate;
                int ch, enc;
                if (mpg123_open_handle64(h_, this) != MPG123_OK) {
                    return false;
                }
                mpg123_set_filesize64(h_, static_cast<int64_t>(s_.size()));
                if (mpg123_getformat(h_, &rate, &ch, &enc) != MPG123_OK) {
                    return false;
                }
                format = {static_cast<uint32_t>(rate), static_cast<uint32_t>(ch)};
                int64_t len = mpg123_length64(h_);
                length = len > 0 ? static_cast<uint64_t>(len) : 0;
                codec = "mp3";
                return true;
            }

            size_t read(float *out, size_t frames) override {
                size_t stride = format.channels * sizeof(float), done = 0;
                for (;;) {
                    int rc = mpg123_read(h_, out, frames * stride, &done);
                    if (done || rc != MPG123_NEW_FORMAT) {
                        return done / stride;
                    }
                }
            }

            bool seek(uint64_t frame) override {
                return mpg123_seek64(h_, static_cast<int64_t>(frame), SEEK_SET) >= 0;
            }

        private:
            static int read_cb(void *ud, void *buf, size_t n, size_t *got) {
                auto *d = static_cast<Mp3 *>(ud);
                *got = d->s_.read(d->pos_, buf, n);
                d->pos_ += *got;
                return 0;
            }
            static int64_t seek_cb(void *ud, int64_t off, int whence) {
                auto *d = static_cast<Mp3 *>(ud);
                int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(d->pos_) : static_cast<int64_t>(d->s_.size());
                d->pos_ = static_cast<uint64_t>(base + off);
                return static_cast<int64_t>(d->pos_);
            }

            Spool &s_;
            mpg123_handle *h_ = mpg123_new(nullptr, nullptr);
            uint64_t pos_ = 0;
        };

    } // namespace

    std::unique_ptr<Decoder> open_mp3(Spool &spool) {
        auto d = std::make_unique<Mp3>(spool);
        return d->open() ? std::move(d) : nullptr;
    }

} // namespace lyren::audio
