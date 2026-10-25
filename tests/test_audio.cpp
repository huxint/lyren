#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/spool.hpp"

using namespace lyren::audio;

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

    r.add(20, 30);  // touching on both sides joins all three
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

std::vector<float> decode_all(Decoder& d) {
    std::vector<float> out, buf(1000 * d.format.channels);
    while (size_t n = d.read(buf.data(), 1000)) out.insert(out.end(), buf.begin(), buf.begin() + ptrdiff_t(n * d.format.channels));
    return out;
}

// Largest sample difference between two decodes of the same frames.
float max_diff(const float* a, const float* b, size_t n) {
    float m = 0;
    for (size_t i = 0; i < n; ++i) m = std::max(m, std::abs(a[i] - b[i]));
    return m;
}

}  // namespace

TEST_CASE("decoders: format, length, seek") {
    struct Case {
        const char* file;
        std::string_view codec;
        float tolerance;  // after a seek, versus decoding straight through
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
        CHECK(std::abs(double(d->length) - 22050) < 2200);

        auto all = decode_all(*d);
        size_t frames = all.size() / 2;
        CHECK(std::abs(double(frames) - 22050) < 2200);
        // Left is 440 Hz at 0.5, right 660 Hz at 0.25.
        float peak_l = 0, peak_r = 0;
        for (size_t i = 0; i < frames; ++i) peak_l = std::max(peak_l, std::abs(all[i * 2])), peak_r = std::max(peak_r, std::abs(all[i * 2 + 1]));
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
