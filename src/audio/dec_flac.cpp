#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"

namespace lyren::audio {

namespace {

class Flac final : public Decoder {
public:
    explicit Flac(Spool& s) : s_(s) {}
    ~Flac() override { FLAC__stream_decoder_delete(d_); }

    bool open() {
        // libFLAC skips a leading ID3v2 tag by itself.
        if (FLAC__stream_decoder_init_stream(d_, read_cb, seek_cb, tell_cb, length_cb, eof_cb, write_cb, metadata_cb, error_cb, this) !=
            FLAC__STREAM_DECODER_INIT_STATUS_OK)
            return false;
        if (!FLAC__stream_decoder_process_until_end_of_metadata(d_) || !format.rate) return false;
        codec = "flac";
        return true;
    }

    size_t read(float* out, size_t frames) override {
        size_t ch = format.channels, done = 0;
        while (done < frames) {
            if (head_ == buf_.size()) {
                buf_.clear(), head_ = 0;
                if (FLAC__stream_decoder_get_state(d_) == FLAC__STREAM_DECODER_END_OF_STREAM || !FLAC__stream_decoder_process_single(d_))
                    break;
                continue;
            }
            size_t k = std::min((frames - done) * ch, buf_.size() - head_);
            std::copy_n(buf_.data() + head_, k, out + done * ch);
            head_ += k, done += k / ch;
        }
        return done;
    }

    bool seek(uint64_t frame) override {
        buf_.clear(), head_ = 0;
        // The write callback gets the target frame trimmed to start at the target sample.
        if (FLAC__stream_decoder_seek_absolute(d_, frame)) return true;
        if (FLAC__stream_decoder_get_state(d_) == FLAC__STREAM_DECODER_SEEK_ERROR) FLAC__stream_decoder_flush(d_);
        return false;
    }

private:
    static Flac* self(void* ud) { return static_cast<Flac*>(ud); }

    static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder*, FLAC__byte buf[], size_t* n, void* ud) {
        auto* d = self(ud);
        *n = d->s_.read(d->pos_, buf, *n);
        d->pos_ += *n;
        return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    static FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder*, FLAC__uint64 off, void* ud) {
        self(ud)->pos_ = off;
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }
    static FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder*, FLAC__uint64* off, void* ud) {
        *off = self(ud)->pos_;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }
    static FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder*, FLAC__uint64* len, void* ud) {
        *len = self(ud)->s_.size();
        return *len ? FLAC__STREAM_DECODER_LENGTH_STATUS_OK : FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }
    static FLAC__bool eof_cb(const FLAC__StreamDecoder*, void* ud) { return self(ud)->pos_ >= self(ud)->s_.size(); }

    static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* f, const FLAC__int32* const in[], void* ud) {
        auto* d = self(ud);
        unsigned ch = d->format.channels, n = f->header.blocksize;
        if (f->header.channels != ch) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        float scale = 1.f / float(1ull << (f->header.bits_per_sample - 1));
        size_t base = d->buf_.size();
        d->buf_.resize(base + size_t(n) * ch);
        float* o = d->buf_.data() + base;
        for (unsigned i = 0; i < n; ++i)
            for (unsigned c = 0; c < ch; ++c) *o++ = float(in[c][i]) * scale;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    static void metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* ud) {
        if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
        auto* d = self(ud);
        d->format = {m->data.stream_info.sample_rate, m->data.stream_info.channels};
        d->length = m->data.stream_info.total_samples;
    }
    static void error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

    Spool& s_;
    FLAC__StreamDecoder* d_ = FLAC__stream_decoder_new();
    uint64_t pos_ = 0;
    std::vector<float> buf_;  // one decoded block
    size_t head_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> open_flac(Spool& spool) {
    auto d = std::make_unique<Flac>(spool);
    return d->open() ? std::move(d) : nullptr;
}

}  // namespace lyren::audio
