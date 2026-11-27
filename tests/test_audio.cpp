#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/decoder.hpp"
#include "audio/meter.hpp"
#include "audio/output.hpp"
#include "audio/player.hpp"
#include "audio/spool.hpp"
#include "core/crypto.hpp"
#include "http_server.hpp"
#include "sys/paths.hpp"
#include "temp_dir.hpp"

using namespace lyren::audio;

TEST_CASE("audio output rejects more channels than the PipeWire format can store") {
    // Vorbis permits up to 255 channels; PipeWire's raw format and control arrays hold at most 64.
    std::atomic<uint32_t> wake{0};
    Output output(wake, {});
    const auto result = output.start({22050, 65}, 1);
    REQUIRE_FALSE(result);
    CHECK(result.error() == "This audio file has more channels than PipeWire supports");
}

TEST_CASE("completed audio needs no range buffer for the progress bar") {
    auto file = Spool::file(LYREN_TEST_DATA "/tone.mp3");
    const auto size = file->size();
    REQUIRE(size > 0);
    std::vector<Ranges::Span> partial;
    CHECK(file->progress(&partial) == std::pair{size, size});
    CHECK(partial.capacity() == 0);
    partial.emplace_back(0, 1);
    file->progress(&partial);
    CHECK(partial.empty());
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

TEST_CASE("truncated FLAC streams surface a decode error and chained Ogg streams decode across sections") {
    auto close_file = [](FILE *stream) {
        std::fclose(stream);
    };

    {
        std::ifstream input(LYREN_TEST_DATA "/tone.flac", std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(input), {}};
        REQUIRE(bytes.size() > 4096);
        bytes.resize(bytes.size() * 2 / 3);
        std::unique_ptr<FILE, decltype(close_file)> file(std::tmpfile(), close_file);
        REQUIRE(file);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size());
        REQUIRE(std::fflush(file.get()) == 0);
        auto spool = Spool::file("/proc/self/fd/" + std::to_string(fileno(file.get())));
        std::string error;
        auto decoder = open_decoder(*spool, error);
        REQUIRE_MESSAGE(decoder, error);
        const auto samples = decode_all(*decoder);
        CHECK(samples.size() / decoder->format.channels < decoder->length);
        CHECK_FALSE(decoder->error.empty());
    }

    {
        std::ifstream input(LYREN_TEST_DATA "/tone.ogg", std::ios::binary);
        std::string single{std::istreambuf_iterator<char>(input), {}};
        REQUIRE_FALSE(single.empty());
        std::string chained = single + single;
        std::unique_ptr<FILE, decltype(close_file)> file(std::tmpfile(), close_file);
        REQUIRE(file);
        REQUIRE(std::fwrite(chained.data(), 1, chained.size(), file.get()) == chained.size());
        REQUIRE(std::fflush(file.get()) == 0);
        auto spool = Spool::file("/proc/self/fd/" + std::to_string(fileno(file.get())));
        std::string error;
        auto decoder = open_decoder(*spool, error);
        REQUIRE_MESSAGE(decoder, error);
        const auto samples = decode_all(*decoder);
        CHECK(decoder->error.empty());
        CHECK(std::abs(static_cast<double>(samples.size() / 2) - 44100.0) < 2200.0);
    }
}

TEST_CASE("MP4 decoder bounds box recursion, surfaces corrupt AAC tails, and rejects out-of-range seeks") {
    auto close_file = [](FILE *stream) {
        std::fclose(stream);
    };

    SUBCASE("the sample description accepts eight nested containers and rejects a ninth") {
        auto nested = [&](unsigned depth) {
            std::ifstream input(LYREN_TEST_DATA "/tone.m4a", std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(input), {}};
            auto read_size = [&](size_t at) {
                uint32_t n = 0;
                for (unsigned char c : std::string_view(bytes).substr(at, 4)) {
                    n = (n << 8) | c;
                }
                return n;
            };
            auto put_size = [&](size_t at, uint32_t n) {
                for (int i = 3; i >= 0; --i, n >>= 8) {
                    bytes[at + i] = static_cast<char>(n);
                }
            };
            // Wrap the valid AAC descriptor from the fixture and move its media offsets with it.
            for (const auto box : {"moov", "trak", "mdia", "minf", "stbl", "stsd", "mp4a"}) {
                auto pos = bytes.find(box);
                REQUIRE(pos != bytes.npos);
                put_size(pos - 4, read_size(pos - 4) + 8 * depth);
            }
            const auto stco = bytes.find("stco");
            REQUIRE(stco != bytes.npos);
            for (uint32_t i = 0; i < read_size(stco + 8); ++i) {
                const size_t offset = stco + 12 + i * 4;
                put_size(offset, read_size(offset) + depth * 8);
            }
            const auto esds = bytes.find("esds");
            REQUIRE(esds != bytes.npos);
            const auto start = esds - 4;
            const uint32_t size = read_size(start);
            bytes.insert(start, depth * 8, '\0');
            for (unsigned i = 0; i < depth; ++i) {
                put_size(start + i * 8, size + (depth - i) * 8);
                bytes.replace(start + i * 8 + 4, 4, "sinf");
            }
            std::unique_ptr<FILE, decltype(close_file)> file(std::tmpfile(), close_file);
            REQUIRE(file);
            REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size());
            REQUIRE(std::fflush(file.get()) == 0);
            auto spool = Spool::file("/proc/self/fd/" + std::to_string(fileno(file.get())));
            std::string error;
            auto decoder = open_decoder(*spool, error);
            if (depth == 8) {
                REQUIRE_MESSAGE(decoder, error);
            } else {
                CHECK_FALSE(decoder);
                CHECK(error == "no audio track in mp4");
            }
        };
        nested(8);
        nested(9);
    }

    SUBCASE("corrupted AAC tail reports a decode error instead of silent EOF") {
        std::ifstream input(LYREN_TEST_DATA "/tone.m4a", std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(input), {}};
        REQUIRE(bytes.size() > 2048);
        for (size_t i = bytes.size() / 2; i < bytes.size(); ++i) {
            bytes[i] = '\xff';
        }
        std::unique_ptr<FILE, decltype(close_file)> file(std::tmpfile(), close_file);
        REQUIRE(file);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size());
        REQUIRE(std::fflush(file.get()) == 0);
        auto spool = Spool::file("/proc/self/fd/" + std::to_string(fileno(file.get())));
        std::string error;
        auto decoder = open_decoder(*spool, error);
        REQUIRE_MESSAGE(decoder, error);
        const auto samples = decode_all(*decoder);
        CHECK(samples.size() / decoder->format.channels < decoder->length);
        CHECK_FALSE(decoder->error.empty());
    }

    SUBCASE("all decoders reject out-of-range seeks") {
        for (const char *name : {"/tone.flac", "/tone.mp3", "/tone.ogg", "/tone.m4a"}) {
            CAPTURE(name);
            auto spool = Spool::file(std::string(LYREN_TEST_DATA) + name);
            std::string error;
            auto decoder = open_decoder(*spool, error);
            REQUIRE_MESSAGE(decoder, error);
            CHECK_FALSE(decoder->seek(UINT64_MAX / 2));
        }
    }
}

TEST_CASE("Player reports the position after a load failure and Spool reuses persistent disk cache") {
    SUBCASE("seeking during a failed open leaves the position at zero") {
        lyren::Loop loop;
        lyren::net::Http http;
        Player player(loop, http);
        player.on_change = [&] {
            if (player.state() == Player::State::failed) {
                loop.quit();
            }
        };
        const auto guard = loop.after(std::chrono::seconds(2), [&] {
            loop.quit();
        });
        player.open_file("/nonexistent/lyren-missing-audio-file.flac", false);
        player.seek(12.5);
        CHECK(player.position() == 0);
        if (player.state() != Player::State::failed) {
            loop.run();
        }
        loop.cancel(guard);
        CHECK(player.state() == Player::State::failed);
        CHECK(player.position() == doctest::Approx(0.0));
    }

    SUBCASE("Spool caches completed streams on disk and evicts oldest entries above limit") {
        lyren::test::TempDir dir;
        struct RestoreCache {
            uint64_t limit = Spool::cache_limit();
            std::optional<std::string> previous;
            ~RestoreCache() {
                Spool::set_cache_limit(limit);
                if (previous) {
                    setenv("XDG_CACHE_HOME", previous->c_str(), 1);
                } else {
                    unsetenv("XDG_CACHE_HOME");
                }
            }
        } restore;
        if (const char *previous = std::getenv("XDG_CACHE_HOME")) {
            restore.previous = previous;
        }
        REQUIRE(setenv("XDG_CACHE_HOME", dir.path.c_str(), 1) == 0);
        std::ifstream input(LYREN_TEST_DATA "/tone.mp3", std::ios::binary);
        const std::string audio_bytes{std::istreambuf_iterator<char>(input), {}};
        REQUIRE(audio_bytes.size() > 1024);

        std::atomic<int> hits{0};
        lyren::test::Server server([&](std::string_view) {
            ++hits;
            return lyren::test::ok(audio_bytes);
        });

        Spool::set_cache_limit(static_cast<uint64_t>(audio_bytes.size()) * 2);
        lyren::net::Http http;
        auto fetch_all = [&](std::string key) {
            auto spool = Spool::url(http, server.url() + "/track.mp3", std::move(key));
            std::string out(audio_bytes.size(), '\0');
            REQUIRE(spool->read_exact(0, out.data(), out.size()));
            CHECK(out == audio_bytes);
            return spool;
        };

        auto s1 = fetch_all("test:cache:song-1");
        CHECK(hits == 1);
        CHECK_FALSE(s1->cache_hit());
        // A replay also reuses the bytes while the worker is still publishing them.
        auto s1_again = fetch_all("test:cache:song-1");
        CHECK(hits == 1);
        CHECK(s1_again->cache_hit());

        // Shrink budget to hold only 1 file and cache song-2, evicting song-1.
        Spool::set_cache_limit(static_cast<uint64_t>(audio_bytes.size()) + 512);
        fetch_all("test:cache:song-2");
        CHECK(hits == 2);
        auto s2_again = fetch_all("test:cache:song-2");
        CHECK(hits == 2);
        CHECK(s2_again->cache_hit());
        const auto cache = lyren::paths::cache_dir() / "audio";
        const auto first = cache / lyren::crypto::md5_hex("test:cache:song-1"), second = cache / lyren::crypto::md5_hex("test:cache:song-2");
        const auto evicted = [&] {
            return std::filesystem::exists(second) && !std::filesystem::exists(first);
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!evicted() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(evicted());
        CHECK(Spool::cache_usage() >= audio_bytes.size());
        CHECK(Spool::clear_cache() >= audio_bytes.size());
        CHECK(Spool::cache_usage() == 0);
        fetch_all("test:cache:song-1");
        CHECK(hits == 3);
        Spool::set_cache_limit(0);
    }
}
