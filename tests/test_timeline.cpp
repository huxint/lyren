#include <doctest/doctest.h>

#include "media/timeline.hpp"

using namespace lyren;

TEST_CASE("offset preview controls use song time while decoding uses file time") {
    // A minute-long file covers 1:30 through 2:30 of a five-minute song.
    const Timeline timeline{300, 60, 90};
    CHECK(timeline.duration() == 300);
    CHECK(timeline.to_file(120) == 30);
    CHECK(timeline.to_song(30) == 120);
    CHECK(timeline.progress(30) == doctest::Approx(0.4));
    const auto [from, to] = timeline.playable_span();
    CHECK(from == doctest::Approx(0.3));
    CHECK(to == doctest::Approx(0.5));
    CHECK(timeline.to_file(0) == 0);
    CHECK(timeline.to_file(299) == 60);
}

TEST_CASE("opening previews keep their zero origin and clamp seeks to the playable end") {
    const Timeline timeline{300, 60};
    CHECK(timeline.to_file(30) == 30);
    CHECK(timeline.to_song(30) == 30);
    CHECK(timeline.to_file(180) == 60);
    CHECK(timeline.progress(30) == doctest::Approx(0.1));
    const auto [from, to] = timeline.playable_span();
    CHECK(from == 0);
    CHECK(to == doctest::Approx(0.2));
}

TEST_CASE("complete songs, unknown durations and empty players stay consistent") {
    const Timeline timeline{240, 240};
    CHECK(timeline.to_file(120) == 120);
    CHECK(timeline.to_song(120) == 120);
    CHECK(timeline.progress(120) == doctest::Approx(0.5));
    CHECK(timeline.playable_span() == std::pair{0.f, 1.f});
    {
        const Timeline loading{300, 0};
        CHECK(loading.to_file(120) == 120);
        CHECK(loading.playable_span() == std::pair{0.f, 1.f});
        const Timeline loaded{300, 60, 90};
        CHECK(loaded.to_file(loading.to_song(120)) == 30);
    }
    {
        const Timeline timeline{0, 60, 90};
        CHECK(timeline.duration() == 150);
        CHECK(timeline.to_song(60) == 150);
        CHECK(timeline.to_file(150) == 60);
        CHECK(timeline.progress(60) == 1);
    }
    {
        const Timeline timeline{0, 0};
        CHECK(timeline.duration() == 0);
        CHECK(timeline.progress(0) == 0);
        CHECK(timeline.to_file(120) == 0);
    }
}
