#include <fdk-aac/aacdecoder_lib.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"

namespace lyren::audio {

    namespace {

        constexpr uint32_t fourcc(const char (&s)[5]) {
            return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16 |
                   static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8 | static_cast<uint8_t>(s[3]);
        }

        // Big-endian cursor over [at, end) of a buffer. Reading past the end sets bad and yields zeros.
        struct Cursor {
            const std::vector<uint8_t> &buf;
            size_t at, end;
            bool bad = false;

            bool take(size_t n) {
                if (bad || end - at < n) {
                    return bad = true, false;
                }
                at += n;
                return true;
            }
            uint64_t be(size_t n) {
                if (!take(n)) {
                    return 0;
                }
                uint64_t v = 0;
                for (size_t i = at - n; i < at; ++i) {
                    v = v << 8 | buf[i];
                }
                return v;
            }
            uint32_t u8() {
                return static_cast<uint32_t>(be(1));
            }
            uint32_t u16() {
                return static_cast<uint32_t>(be(2));
            }
            uint32_t u32() {
                return static_cast<uint32_t>(be(4));
            }
            uint64_t u64() {
                return be(8);
            }
            void skip(size_t n) {
                take(n);
            }
        };

        // Calls f(type, body, end) for each box in [b, e).
        template <class F>
        void each_box(const std::vector<uint8_t> &buf, size_t b, size_t e, F &&f) {
            while (b < e && e - b >= 8) {
                Cursor c{buf, b, e};
                uint64_t size = c.u32();
                uint32_t type = c.u32();
                if (size == 1) {
                    size = c.u64();
                } else if (size == 0) {
                    size = e - b;
                }
                if (c.bad || size < c.at - b || size > e - b) {
                    return;
                }
                f(type, c.at, b + static_cast<size_t>(size));
                b += static_cast<size_t>(size);
            }
        }

        struct Encryption {
            // Keep the compact on-wire table and one index per sample; no allocation per IV or subsample.
            std::vector<uint8_t> entries;
            std::vector<uint32_t> offsets;
            uint32_t scheme = 0;
            uint8_t iv_size = 0;
            bool subsamples = false;
        };

        struct Track {
            std::vector<uint8_t> asc; // AudioSpecificConfig
            uint32_t codec = 0, timescale = 0;
            uint64_t duration = 0;
            std::vector<uint32_t> sizes;
            std::vector<uint64_t> offsets;
            std::optional<Encryption> encryption;
        };

        // The AudioSpecificConfig sits in esds: ES_Descriptor(3) > DecoderConfigDescriptor(4) > DecoderSpecificInfo(5).
        void parse_esds(Cursor c, Track &t) {
            c.skip(4);
            auto descriptor = [&](uint32_t &tag) -> size_t {
                tag = c.u8();
                size_t len = 0;
                for (int i = 0; i < 4; ++i) {
                    uint32_t b = c.u8();
                    len = len << 7 | (b & 0x7f);
                    if (!(b & 0x80)) {
                        break;
                    }
                }
                return len;
            };
            uint32_t tag;
            size_t len = descriptor(tag);
            if (tag != 3) {
                return;
            }
            c.skip(2);
            uint32_t flags = c.u8();
            if (flags & 0x80) {
                c.skip(2);
            }
            if (flags & 0x40) {
                c.skip(c.u8());
            }
            if (flags & 0x20) {
                c.skip(2);
            }
            while (!c.bad && c.at < c.end) {
                len = descriptor(tag);
                if (tag == 4) {
                    c.skip(13);
                    continue;
                }
                if (tag == 5 && c.end - c.at >= len) {
                    t.asc.assign(c.buf.begin() + static_cast<ptrdiff_t>(c.at), c.buf.begin() + static_cast<ptrdiff_t>(c.at + len));
                    return;
                }
                c.skip(len);
            }
        }

        bool parse_stsd(const std::vector<uint8_t> &buf, size_t b, size_t e, Track &t) {
            bool valid = false;
            each_box(buf, b + 8, e, [&](uint32_t type, size_t body, size_t end) {
                if (t.codec) {
                    return;
                }
                t.codec = type;
                if (type == fourcc("enca")) {
                    t.encryption.emplace();
                }
                Cursor c{buf, body, end};
                c.skip(8);
                uint32_t version = c.u16();
                c.skip(18);
                if (version == 1) {
                    c.skip(16);
                } else if (version == 2) {
                    c.skip(36);
                }
                if (c.bad) {
                    return;
                }
                valid = true;
                auto children = [&](this auto &self, size_t from, size_t to) -> void {
                    each_box(buf, from, to, [&](uint32_t child, size_t cb, size_t ce) {
                        Cursor c{buf, cb, ce};
                        if (child == fourcc("esds")) {
                            parse_esds(c, t);
                        } else if (child == fourcc("wave") || child == fourcc("sinf") || child == fourcc("schi")) {
                            self(cb, ce);
                        } else if (t.encryption) {
                            auto &enc = *t.encryption;
                            if (child == fourcc("frma")) {
                                t.codec = c.u32();
                            } else if (child == fourcc("schm")) {
                                c.skip(4);
                                enc.scheme = c.u32();
                            } else if (child == fourcc("tenc")) {
                                uint32_t version = c.u8();
                                c.skip(4); // flags and reserved byte
                                uint32_t pattern = c.u8(), protected_sample = c.u8();
                                enc.iv_size = static_cast<uint8_t>(c.u8());
                                c.skip(16); // default KID; the selected stream already supplies its key
                                valid &= version <= 1 && !pattern && protected_sample == 1;
                            }
                        }
                        valid &= !c.bad;
                    });
                };
                children(c.at, end);
                if (t.encryption) {
                    const auto &enc = *t.encryption;
                    valid &= enc.scheme == fourcc("cenc") && (enc.iv_size == 8 || enc.iv_size == 16);
                }
            });
            return valid;
        }

        bool parse_senc(Cursor c, Track &t) {
            auto &enc = *t.encryption;
            uint32_t flags = c.u32(), count = c.u32();
            if (c.bad || (flags & ~2u) || count != t.sizes.size()) {
                return false;
            }
            enc.subsamples = flags & 2;
            const size_t begin = c.at;
            enc.offsets.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                enc.offsets.push_back(static_cast<uint32_t>(c.at - begin));
                c.skip(enc.iv_size);
                if (enc.subsamples) {
                    uint32_t n = c.u16();
                    uint64_t bytes = 0;
                    for (uint32_t j = 0; j < n && !c.bad; ++j) {
                        bytes += c.u16();
                        bytes += c.u32();
                    }
                    if (n && bytes != t.sizes[i]) {
                        return false;
                    }
                }
                if (c.bad) {
                    return false;
                }
            }
            if (c.at != c.end) {
                return false;
            }
            enc.entries.assign(c.buf.begin() + static_cast<ptrdiff_t>(begin), c.buf.begin() + static_cast<ptrdiff_t>(c.end));
            return true;
        }

        bool parse_stbl(const std::vector<uint8_t> &buf, size_t b, size_t e, Track &t) {
            std::vector<std::pair<uint32_t, uint32_t>> stsc; // first chunk (1-based), samples per chunk
            std::vector<uint64_t> chunks;
            size_t senc_begin = 0, senc_end = 0;
            bool bad = false;
            each_box(buf, b, e, [&](uint32_t type, size_t body, size_t end) {
                Cursor c{buf, body, end};
                if (type == fourcc("stsd")) {
                    bad |= !parse_stsd(buf, body, end, t);
                } else if (type == fourcc("senc")) {
                    senc_begin = body, senc_end = end;
                } else if (type == fourcc("stsc")) {
                    c.skip(4);
                    for (uint32_t n = c.u32(); n-- && !c.bad;) {
                        uint32_t first = c.u32(), per = c.u32();
                        c.skip(4);
                        stsc.emplace_back(first, per);
                    }
                } else if (type == fourcc("stsz")) {
                    c.skip(4);
                    uint32_t fixed = c.u32(), n = c.u32();
                    if (c.bad || n > 1u << 24 || (!fixed && n > (end - c.at) / 4)) {
                        bad = true;
                        return;
                    }
                    t.sizes.resize(n, fixed);
                    if (!fixed) {
                        for (auto &s : t.sizes) {
                            s = c.u32();
                        }
                    }
                } else if (type == fourcc("stco") || type == fourcc("co64")) {
                    c.skip(4);
                    uint32_t n = c.u32();
                    if (c.bad || n > (end - c.at) / (type == fourcc("co64") ? 8 : 4)) {
                        bad = true;
                        return;
                    }
                    chunks.resize(n);
                    for (auto &o : chunks) {
                        o = type == fourcc("co64") ? c.u64() : c.u32();
                    }
                } else {
                    return;
                }
                bad |= c.bad;
            });
            if (bad || stsc.empty()) {
                return false;
            }
            // Chunk k holds samples_per_chunk consecutive samples starting at the chunk offset.
            t.offsets.reserve(t.sizes.size());
            size_t s = 0;
            for (size_t k = 0, e_i = 0; k < chunks.size() && s < t.sizes.size(); ++k) {
                while (e_i + 1 < stsc.size() && stsc[e_i + 1].first <= k + 1) {
                    ++e_i;
                }
                uint64_t off = chunks[k];
                for (uint32_t i = 0; i < stsc[e_i].second && s < t.sizes.size(); ++i, ++s) {
                    t.offsets.push_back(off);
                    off += t.sizes[s];
                }
            }
            return s == t.sizes.size() && (!t.encryption || (senc_end && parse_senc({buf, senc_begin, senc_end}, t)));
        }

        // First sound track of the moov box.
        bool parse_moov(const std::vector<uint8_t> &buf, Track &out) {
            bool found = false;
            each_box(buf, 0, buf.size(), [&](uint32_t type, size_t body, size_t end) {
                if (found || type != fourcc("trak")) {
                    return;
                }
                Track t;
                bool sound = false, ok = false;
                each_box(buf, body, end, [&](uint32_t type, size_t body, size_t end) {
                    if (type != fourcc("mdia")) {
                        return;
                    }
                    each_box(buf, body, end, [&](uint32_t type, size_t body, size_t end) {
                        Cursor c{buf, body, end};
                        if (type == fourcc("mdhd")) {
                            bool v1 = c.u8() == 1;
                            c.skip(v1 ? 19 : 11);
                            t.timescale = c.u32();
                            t.duration = v1 ? c.u64() : c.u32();
                        } else if (type == fourcc("hdlr")) {
                            c.skip(8);
                            sound = c.u32() == fourcc("soun");
                        } else if (type == fourcc("minf")) {
                            each_box(buf, body, end, [&](uint32_t type, size_t body, size_t end) {
                                if (type == fourcc("stbl")) {
                                    ok = parse_stbl(buf, body, end, t);
                                }
                            });
                        }
                    });
                });
                if (sound && ok) {
                    out = std::move(t), found = true;
                }
            });
            return found;
        }

        class Aac final : public Decoder {
        public:
            Aac(Spool &s, Track t)
            : s_(s),
              t_(std::move(t)) {}
            ~Aac() override {
                if (h_) {
                    aacDecoder_Close(h_);
                }
                EVP_CIPHER_CTX_free(cipher_);
            }

            bool open(const std::optional<crypto::Aes128Key> &key) {
                packet_.reserve(std::ranges::max(t_.sizes));
                if (t_.encryption) {
                    cipher_ = EVP_CIPHER_CTX_new();
                    if (!cipher_ || EVP_DecryptInit_ex(cipher_, EVP_aes_128_ctr(), nullptr, key->data(), nullptr) != 1) {
                        return false;
                    }
                }
                if (!reset()) {
                    return false;
                }
                // The first frame tells the output rate and channels (SBR and PS change them).
                if (!decode_next()) {
                    return false;
                }
                CStreamInfo *si = aacDecoder_GetStreamInfo(h_);
                format = {static_cast<uint32_t>(si->sampleRate), static_cast<uint32_t>(si->numChannels)};
                fill(si);
                length = t_.timescale ? t_.duration * format.rate / t_.timescale : 0;
                codec = "aac";
                return true;
            }

            size_t read(float *out, size_t frames) override {
                size_t ch = format.channels, done = 0;
                while (done < frames) {
                    if (head_ == buf_.size()) {
                        if (!decode_next()) {
                            break;
                        }
                        fill(aacDecoder_GetStreamInfo(h_));
                    }
                    if (skip_) {
                        size_t k = std::min(skip_, buf_.size() - head_);
                        head_ += k, skip_ -= k;
                        continue;
                    }
                    size_t k = std::min((frames - done) * ch, buf_.size() - head_);
                    std::copy_n(buf_.data() + head_, k, out + done * ch);
                    head_ += k, done += k / ch;
                }
                return done;
            }

            // A fresh decoder starts one packet early: the packet before the target primes the overlap-add.
            bool seek(uint64_t frame) override {
                uint64_t k = frame / frame_size_, start = k ? k - 1 : 0;
                next_ = std::min<uint64_t>(start, t_.sizes.size());
                skip_ = static_cast<size_t>(frame - start * frame_size_) * format.channels;
                buf_.clear(), head_ = 0;
                return reset();
            }

        private:
            // Clearing only the transport buffer would keep the previous position's overlap state.
            bool reset() {
                if (h_) {
                    aacDecoder_Close(h_);
                }
                h_ = aacDecoder_Open(TT_MP4_RAW, 1);
                UCHAR *conf = t_.asc.data();
                UINT len = static_cast<UINT>(t_.asc.size());
                if (!h_ || aacDecoder_ConfigRaw(h_, &conf, &len) != AAC_DEC_OK) {
                    return false;
                }
                aacDecoder_SetParam(h_, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
                // Energy interpolation, the default concealment, delays the output by a frame.
                aacDecoder_SetParam(h_, AAC_CONCEAL_METHOD, 1);
                return true;
            }

            bool decode_next() {
                while (next_ < t_.sizes.size()) {
                    packet_.resize(t_.sizes[next_]);
                    if (!s_.read_exact(t_.offsets[next_], packet_.data(), packet_.size())) {
                        return false;
                    }
                    if (cipher_ && !decrypt(next_)) {
                        return false;
                    }
                    ++next_;
                    UCHAR *p = packet_.data();
                    UINT size = static_cast<UINT>(packet_.size()), valid = size;
                    aacDecoder_Fill(h_, &p, &size, &valid);
                    if (aacDecoder_DecodeFrame(h_, pcm_.data(), static_cast<INT>(pcm_.size()), 0) == AAC_DEC_OK) {
                        return true;
                    }
                }
                return false;
            }

            bool decrypt(size_t sample) {
                const auto &enc = *t_.encryption;
                const auto *entry = enc.entries.data() + enc.offsets[sample];
                std::array<unsigned char, 16> iv{};
                std::copy_n(entry, enc.iv_size, iv.data());
                entry += enc.iv_size;
                if (EVP_DecryptInit_ex(cipher_, nullptr, nullptr, nullptr, iv.data()) != 1) {
                    return false;
                }
                auto crypt = [&](uint8_t *bytes, size_t size) {
                    int written;
                    return EVP_DecryptUpdate(cipher_, bytes, &written, bytes, static_cast<int>(size)) == 1;
                };
                // The table was checked against sample sizes once, while opening the external file.
                auto be = [&](size_t n) {
                    uint32_t value = 0;
                    while (n--) {
                        value = value << 8 | *entry++;
                    }
                    return value;
                };
                uint32_t count = enc.subsamples ? be(2) : 0;
                if (!count) {
                    return crypt(packet_.data(), packet_.size());
                }
                auto *at = packet_.data();
                for (uint32_t i = 0; i < count; ++i) {
                    at += be(2);
                    uint32_t encrypted = be(4);
                    if (!crypt(at, encrypted)) {
                        return false;
                    }
                    at += encrypted;
                }
                return true;
            }

            // Converts the decoded frame to float in the stream's channel layout.
            void fill(const CStreamInfo *si) {
                size_t ch = format.channels, n = static_cast<size_t>(si->frameSize), src = static_cast<size_t>(si->numChannels);
                frame_size_ = n;
                buf_.resize(n * ch), head_ = 0;
                for (size_t i = 0; i < n; ++i) {
                    for (size_t c = 0; c < ch; ++c) {
                        buf_[i * ch + c] = static_cast<float>(pcm_[i * src + std::min(c, src - 1)]) / 32768.f;
                    }
                }
            }

            Spool &s_;
            Track t_;
            HANDLE_AACDECODER h_ = nullptr;
            EVP_CIPHER_CTX *cipher_ = nullptr;
            std::vector<uint8_t> packet_;
            std::vector<INT_PCM> pcm_ = std::vector<INT_PCM>(2048 * 8);
            std::vector<float> buf_;
            size_t head_ = 0, skip_ = 0;
            uint64_t next_ = 0, frame_size_ = 1024;
        };

    } // namespace

    std::unique_ptr<Decoder> open_mp4(Spool &spool, std::string &error, std::optional<crypto::Aes128Key> key) {
        // Top-level boxes; moov may come after mdat, which costs one extra range request.
        uint64_t at = 0, size = spool.size();
        std::vector<uint8_t> moov;
        while (at + 8 <= size) {
            std::array<uint8_t, 16> h;
            if (!spool.read_exact(at, h.data(), std::min<uint64_t>(h.size(), size - at))) {
                return nullptr;
            }
            uint64_t n = static_cast<uint64_t>(h[0]) << 24 | h[1] << 16 | h[2] << 8 | h[3], hdr = 8;
            uint32_t type = static_cast<uint32_t>(h[4]) << 24 | h[5] << 16 | h[6] << 8 | h[7];
            if (n == 1) {
                if (size - at < 16) {
                    break;
                }
                n = 0, hdr = 16;
                for (int i = 8; i < 16; ++i) {
                    n = n << 8 | h[i];
                }
            } else if (n == 0) {
                n = size - at;
            }
            if (n < hdr || n > size - at) {
                break;
            }
            if (type == fourcc("moov")) {
                if (n > 64 << 20) {
                    break;
                }
                moov.resize(n - hdr);
                if (!spool.read_exact(at + hdr, moov.data(), moov.size())) {
                    return nullptr;
                }
                break;
            }
            at += n;
        }
        Track t;
        if (moov.empty() || !parse_moov(moov, t)) {
            error = "no audio track in mp4";
            return nullptr;
        }
        if (t.codec != fourcc("mp4a") || t.asc.empty()) {
            std::string cc = {static_cast<char>(t.codec >> 24), static_cast<char>(t.codec >> 16), static_cast<char>(t.codec >> 8), static_cast<char>(t.codec)};
            error = "unsupported mp4 codec " + cc;
            return nullptr;
        }
        if (t.offsets.empty()) {
            error = "fragmented mp4 is not supported";
            return nullptr;
        }
        if (t.encryption && !key) {
            error = "encrypted mp4 requires a stream key";
            return nullptr;
        }
        for (size_t i = 0; i < t.sizes.size(); ++i) {
            if (t.sizes[i] > std::numeric_limits<int>::max() || t.offsets[i] > size || t.sizes[i] > size - t.offsets[i]) {
                error = "mp4 sample exceeds the file bounds";
                return nullptr;
            }
        }
        auto d = std::make_unique<Aac>(spool, std::move(t));
        return d->open(key) ? std::move(d) : nullptr;
    }

} // namespace lyren::audio
