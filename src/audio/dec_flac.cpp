#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"

namespace lyren::audio {

    namespace {

        class Flac final : public Decoder {
        public:
            explicit Flac(Spool &s)
            : cursor_{s} {}
            ~Flac() override {
                FLAC__stream_decoder_delete(d_);
            }

            bool open() {
                // libFLAC skips a leading ID3v2 tag by itself.
                if (FLAC__stream_decoder_init_stream(d_, read_cb, seek_cb, tell_cb, length_cb, eof_cb, write_cb, metadata_cb, error_cb, this) !=
                    FLAC__STREAM_DECODER_INIT_STATUS_OK) {
                    return false;
                }
                if (!FLAC__stream_decoder_process_until_end_of_metadata(d_) || !format.rate || !format.channels) {
                    return false;
                }
                codec = "flac";
                return true;
            }

            size_t read(float *out, size_t frames) override {
                size_t ch = format.channels, done = 0;
                while (done < frames) {
                    if (head_ == buf_.size()) {
                        buf_.clear(), head_ = 0;
                        if (FLAC__stream_decoder_get_state(d_) == FLAC__STREAM_DECODER_END_OF_STREAM || !FLAC__stream_decoder_process_single(d_)) {
                            break;
                        }
                        continue;
                    }
                    size_t k = std::min((frames - done) * ch, buf_.size() - head_);
                    std::copy_n(buf_.data() + head_, k, out + done * ch);
                    head_ += k, done += k / ch;
                }
                if (done == 0 && (bad_ || FLAC__stream_decoder_get_state(d_) == FLAC__STREAM_DECODER_ABORTED || (length > 0 && end_sample_ < length))) {
                    error = "corrupt or truncated flac stream";
                }
                return done;
            }

            bool seek(uint64_t frame) override {
                buf_.clear(), head_ = 0;
                error.clear(), bad_ = false, end_sample_ = frame;
                // The write callback gets the target frame trimmed to start at the target sample.
                if (FLAC__stream_decoder_seek_absolute(d_, frame)) {
                    return true;
                }
                if (FLAC__stream_decoder_get_state(d_) == FLAC__STREAM_DECODER_SEEK_ERROR) {
                    FLAC__stream_decoder_flush(d_);
                }
                return false;
            }

        private:
            static Flac *self(void *ud) {
                return static_cast<Flac *>(ud);
            }

            static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *, FLAC__byte buf[], size_t *n, void *ud) {
                *n = self(ud)->cursor_.read(buf, *n);
                return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
            }
            static FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder *, FLAC__uint64 off, void *ud) {
                self(ud)->cursor_.pos = off;
                return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
            }
            static FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder *, FLAC__uint64 *off, void *ud) {
                *off = self(ud)->cursor_.pos;
                return FLAC__STREAM_DECODER_TELL_STATUS_OK;
            }
            static FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder *, FLAC__uint64 *len, void *ud) {
                *len = self(ud)->cursor_.size();
                return *len ? FLAC__STREAM_DECODER_LENGTH_STATUS_OK : FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
            }
            static FLAC__bool eof_cb(const FLAC__StreamDecoder *, void *ud) {
                return self(ud)->cursor_.eof();
            }

            static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *, const FLAC__Frame *f, const FLAC__int32 *const in[], void *ud) {
                auto *d = self(ud);
                unsigned ch = d->format.channels, n = f->header.blocksize;
                if (f->header.channels != ch || f->header.sample_rate != d->format.rate) {
                    d->bad_ = true;
                    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
                }
                d->end_sample_ = f->header.number.sample_number + n;
                float scale = 1.f / static_cast<float>(1ull << (f->header.bits_per_sample - 1));
                size_t base = d->buf_.size();
                d->buf_.resize(base + static_cast<size_t>(n) * ch);
                float *o = d->buf_.data() + base;
                for (unsigned i = 0; i < n; ++i) {
                    for (unsigned c = 0; c < ch; ++c) {
                        *o++ = static_cast<float>(in[c][i]) * scale;
                    }
                }
                return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
            }
            static void metadata_cb(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *m, void *ud) {
                if (m->type != FLAC__METADATA_TYPE_STREAMINFO) {
                    return;
                }
                auto *d = self(ud);
                d->format = {m->data.stream_info.sample_rate, m->data.stream_info.channels};
                d->length = m->data.stream_info.total_samples;
            }
            static void error_cb(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *ud) {
                self(ud)->bad_ = true;
            }

            SpoolCursor cursor_;
            FLAC__StreamDecoder *d_ = FLAC__stream_decoder_new();
            uint64_t end_sample_ = 0;
            bool bad_ = false;
            std::vector<float> buf_; // one decoded block
            size_t head_ = 0;
        };

    } // namespace

    std::unique_ptr<Decoder> open_flac(Spool &spool) {
        auto d = std::make_unique<Flac>(spool);
        return d->open() ? std::move(d) : nullptr;
    }

} // namespace lyren::audio
