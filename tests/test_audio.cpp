#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/meter.hpp"
#include "audio/output.hpp"
#include "audio/spool.hpp"

using namespace lyren::audio;

TEST_CASE("audio output rejects more channels than the PipeWire format can store") {
    // Vorbis permits up to 255 channels; PipeWire's raw format and control arrays hold at most 64.
    std::atomic<uint32_t> wake{0};
    Output output(wake, {});
    const auto result = output.start({22050, 65});
    REQUIRE_FALSE(result);
    CHECK(result.error() == "This audio file has more channels than PipeWire supports");
}

TEST_CASE("byte ranges merge and report gaps") {
    Ranges r;
    CHECK(r.gap(0, 100) == Ranges::Span{0, 100});
    r.add(10, 20);
    r.add(30, 40);
    CHECK(r.spans().size() == 2);
    CHECK(r.available(10) == 10);
    CHECK(r.available(15) == 5);
    CHECK(r.available(20) == 0);
    CHECK(r.available(5) == 0);
    CHECK(r.gap(0, 100) == Ranges::Span{0, 10});
    CHECK(r.gap(12, 100) == Ranges::Span{20, 30});
    CHECK(r.gap(35, 100) == Ranges::Span{40, 100});
    CHECK(r.gap(35, 40) == Ranges::Span{40, 40});

    r.add(20, 30); // touching on both sides joins all three
    CHECK(r.spans() == std::vector<Ranges::Span>{{10, 40}});
    r.add(0, 5);
    r.add(3, 12);
    CHECK(r.spans() == std::vector<Ranges::Span>{{0, 40}});
    r.add(50, 60);
    r.add(45, 70);
    CHECK(r.spans() == std::vector<Ranges::Span>{{0, 40}, {45, 70}});
    CHECK(r.total() == 65);
    r.add(40, 40);
    CHECK(r.spans().size() == 2);
}

namespace {

    std::vector<float> decode_all(Decoder &d) {
        std::vector<float> out, buf(1000 * d.format.channels);
        while (size_t n = d.read(buf.data(), 1000)) {
            out.insert(out.end(), buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n * d.format.channels));
        }
        return out;
    }

    // Largest sample difference between two decodes of the same frames.
    float max_diff(const float *a, const float *b, size_t n) {
        float m = 0;
        for (size_t i = 0; i < n; ++i) {
            m = std::max(m, std::abs(a[i] - b[i]));
        }
        return m;
    }

} // namespace

TEST_CASE("decoders: format, length, seek") {
    struct Case {
        const char *file;
        std::string_view codec;
        float tolerance; // after a seek, versus decoding straight through
    };
    for (auto c : {Case{"tone.flac", "flac", 0}, Case{"tone.mp3", "mp3", 1e-4f}, Case{"tone.ogg", "vorbis", 1e-4f}, Case{"tone.m4a", "aac", 1e-4f}}) {
        std::string name = c.file;
        CAPTURE(name);
        auto spool = Spool::file(std::string(LYREN_TEST_DATA "/") + c.file);
        std::string error;
        auto d = open_decoder(*spool, error);
        REQUIRE_MESSAGE(d, error);
        CHECK(d->codec == c.codec);
        CHECK(d->format == Format{22050, 2});
        CHECK(std::abs(static_cast<double>(d->length) - 22050) < 2200);

        auto all = decode_all(*d);
        size_t frames = all.size() / 2;
        REQUIRE(std::abs(static_cast<double>(frames) - 22050) < 2200);
        // Left is 440 Hz at 0.5, right 660 Hz at 0.25.
        float peak_l = 0, peak_r = 0;
        for (size_t i = 0; i < frames; ++i) {
            peak_l = std::max(peak_l, std::abs(all[i * 2])), peak_r = std::max(peak_r, std::abs(all[i * 2 + 1]));
        }
        CHECK(peak_l == doctest::Approx(0.5).epsilon(0.1));
        CHECK(peak_r == doctest::Approx(0.25).epsilon(0.1));

        for (uint64_t target : {11025ull, 3000ull, 0ull}) {
            CAPTURE(target);
            REQUIRE(d->seek(target));
            std::vector<float> buf(2048 * 2);
            REQUIRE(d->read(buf.data(), 2048) == 2048);
            CHECK(max_diff(buf.data(), all.data() + target * 2, 2048 * 2) <= c.tolerance);
        }
    }
}

TEST_CASE("missing and unsupported files fail with a reason") {
    std::string error;
    auto missing = Spool::file("/nonexistent/x.flac");
    CHECK_FALSE(open_decoder(*missing, error));
    CHECK(error.find("/nonexistent/x.flac") != std::string::npos);

    error.clear();
    auto text = Spool::file(LYREN_TEST_DATA "/../test_audio.cpp");
    CHECK_FALSE(open_decoder(*text, error));
    CHECK(error == "unsupported format");
}

TEST_CASE("MP4 constant-size sample tables decode every AAC packet") {
    // fixed.m4a has 32 copies of tone.m4a's first AAC packet and a constant-size stsz table.
    auto spool = Spool::file(LYREN_TEST_DATA "/fixed.m4a");
    std::string error;
    auto decoder = open_decoder(*spool, error);
    REQUIRE_MESSAGE(decoder, error);
    CHECK(decoder->format == Format{22050, 2});
    CHECK(decoder->length == 32 * 1024);
    CHECK(decode_all(*decoder).size() == 32 * 1024 * 2);
}

TEST_CASE("CENC AAC decrypts per sample and seeks on the original audio timeline") {
    const lyren::crypto::Aes128Key key{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    std::string error;
    auto plain = Spool::file(LYREN_TEST_DATA "/tone.m4a");
    auto reference = open_decoder(*plain, error);
    REQUIRE_MESSAGE(reference, error);
    const auto samples = decode_all(*reference);
    for (const char *file : {"cenc.m4a", "cenc-subsamples.m4a"}) {
        CAPTURE(file);
        auto spool = Spool::file(std::string(LYREN_TEST_DATA "/") + file);
        CHECK_FALSE(open_decoder(*spool, error));
        CHECK(error == "encrypted mp4 requires a stream key");
        error.clear();
        auto decoder = open_decoder(*spool, error, key);
        REQUIRE_MESSAGE(decoder, error);
        CHECK(decoder->format == reference->format);
        const auto actual = decode_all(*decoder);
        REQUIRE(actual.size() == samples.size());
        CHECK(max_diff(actual.data(), samples.data(), actual.size()) == 0);
        for (uint64_t target : {11025ull, 3000ull, 0ull}) {
            CAPTURE(target);
            REQUIRE(decoder->seek(target));
            std::vector<float> output(2048 * decoder->format.channels);
            REQUIRE(decoder->read(output.data(), 2048) == 2048);
            CHECK(max_diff(output.data(), samples.data() + target * decoder->format.channels, output.size()) < 1e-4f);
        }
    }
}

TEST_CASE("CENC sample descriptions cannot decrypt beyond a packet boundary") {
    std::ifstream input(LYREN_TEST_DATA "/cenc-subsamples.m4a", std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    const auto senc = bytes.find("senc");
    REQUIRE(senc != bytes.npos);
    // First sample: FullBox/count, 16-byte IV, subsample count, clear size, encrypted size.
    bytes.replace(senc + 4 + 8 + 16 + 2 + 2, 4, 4, '\xff');
    auto close_file = [](FILE *stream) {
        std::fclose(stream);
    };
    std::unique_ptr<FILE, decltype(close_file)> file(std::tmpfile(), close_file);
    REQUIRE(file);
    REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size());
    REQUIRE(std::fflush(file.get()) == 0);
    auto spool = Spool::file("/proc/self/fd/" + std::to_string(fileno(file.get())));
    std::string error;
    CHECK_FALSE(open_decoder(*spool, error, lyren::crypto::Aes128Key{}));
    CHECK_FALSE(error.empty());
}

TEST_CASE("MP3 seeks before a frame index is built retain the exact sample timeline") {
    auto spool = Spool::file(LYREN_TEST_DATA "/seek-vbr.mp3");
    std::string error;
    auto reference = open_decoder(*spool, error);
    REQUIRE_MESSAGE(reference, error);
    const auto samples = decode_all(*reference);
    const auto format = reference->format;
    for (uint64_t second : {8, 16, 20}) {
        CAPTURE(second);
        auto source = Spool::file(LYREN_TEST_DATA "/seek-vbr.mp3");
        auto decoder = open_decoder(*source, error);
        REQUIRE_MESSAGE(decoder, error);
        const uint64_t target = second * format.rate + 137;
        REQUIRE(decoder->seek(target));
        std::vector<float> actual(2048 * format.channels);
        REQUIRE(decoder->read(actual.data(), 2048) == 2048);
        CHECK(max_diff(actual.data(), samples.data() + target * format.channels, actual.size()) < 1e-4f);
    }
}

TEST_CASE("the meter sorts tones into their bands and reads silence as zero") {
    constexpr uint32_t rate = 48000;
    constexpr float tau = 6.2831853f;
    auto tone = [&](float hz, size_t channels) {
        std::vector<float> out(4800 * channels);
        for (size_t i = 0; i < 4800; ++i) {
            for (size_t c = 0; c < channels; ++c) {
                out[i * channels + c] = 0.5f * std::sin(tau * hz * static_cast<float>(i) / rate);
            }
        }
        return out;
    };
    Meter meter;
    for (size_t channels : {size_t{1}, size_t{2}, size_t{6}}) {
        CAPTURE(channels);
        meter.reset(rate);
        const auto bass = tone(60, channels), voice = tone(700, channels), air = tone(9000, channels);
        Levels low = meter.measure(bass.data(), 4800, channels);
        CHECK(low.all == doctest::Approx(0.3536f).epsilon(0.02));
        CHECK(low.low > low.mid * 2);
        CHECK(low.low > low.high * 8);
        meter.reset(rate);
        Levels mid = meter.measure(voice.data(), 4800, channels);
        CHECK(mid.mid > mid.low * 2);
        CHECK(mid.mid > mid.high * 2);
        meter.reset(rate);
        Levels high = meter.measure(air.data(), 4800, channels);
        CHECK(high.high > high.mid * 3);
        CHECK(high.high > high.low * 30);
    }
    const std::vector<float> silence(4800 * 2, 0.f);
    for (int i = 0; i < 40; ++i) {
        meter.measure(silence.data(), 4800, 2);
    }
    Levels quiet = meter.measure(silence.data(), 4800, 2);
    CHECK(quiet.all == 0);
    CHECK(quiet.low == 0);
    CHECK(quiet.high == 0);
    CHECK(meter.measure(silence.data(), 0, 2).all == 0);
}

TEST_CASE("stereo meters preserve energy when channels have opposite phase") {
    constexpr size_t frames = 4800;
    std::vector<float> stereo(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float sample = 0.5f * std::sin(6.2831853f * 700 * static_cast<float>(i) / 48000);
        stereo[i * 2] = sample;
        stereo[i * 2 + 1] = -sample;
    }
    Meter meter;
    meter.reset(48000);
    const Levels opposite = meter.measure(stereo.data(), frames, 2);
    CHECK(opposite.all == doctest::Approx(0.3536f).epsilon(0.01));
    CHECK(opposite.mid > opposite.low * 2);
    for (size_t i = 0; i < frames; ++i) {
        stereo[i * 2 + 1] = stereo[i * 2];
    }
    meter.reset(48000);
    const Levels matching = meter.measure(stereo.data(), frames, 2);
    CHECK(opposite.all == doctest::Approx(matching.all));
    CHECK(opposite.low == doctest::Approx(matching.low));
    CHECK(opposite.mid == doctest::Approx(matching.mid));
    CHECK(opposite.high == doctest::Approx(matching.high));
}

TEST_CASE("meter followers use their whole range at any loudness and settle at rest") {
    for (float loudness : {0.4f, 0.04f}) {
        CAPTURE(loudness);
        Follower follower;
        // A loud beat, then a softer passage, then nothing.
        for (int i = 0; i < 30; ++i) {
            follower.follow({loudness, loudness, loudness * 0.5f, loudness * 0.1f}, 1 / 60.f);
        }
        CHECK(follower.all() > 0.9f);
        CHECK(follower.band(0) > 0.9f);
        CHECK(follower.band(2) > 0.9f); // each band against its own peak
        const float loud = follower.all();
        for (int i = 0; i < 30; ++i) {
            follower.follow({loudness * 0.25f, loudness * 0.25f, 0, 0}, 1 / 60.f);
        }
        CHECK(follower.all() < loud);
        CHECK(follower.all() > 0.2f);
        CHECK(follower.band(1) < 0.2f);
        for (int i = 0; i < 600; ++i) {
            follower.follow({}, 1 / 60.f);
        }
        CHECK(follower.all() == 0);
        CHECK_FALSE(follower.moving());
    }
    // A stalled frame clock neither explodes nor leaves the value outside its range.
    Follower late;
    late.follow({0.3f, 0.3f, 0.3f, 0.3f}, 30);
    CHECK(late.all() <= 1);
    late.follow({0.3f, 0.3f, 0.3f, 0.3f}, -1);
    CHECK(late.all() >= 0);
}
