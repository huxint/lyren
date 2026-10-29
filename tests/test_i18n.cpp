#include <doctest/doctest.h>

#include "core/i18n.hpp"
#include "media/types.hpp"

using namespace lyren;

TEST_CASE("language changes apply immediately without changing source keys") {
    const Language before = language();
    set_language(Language::en);
    CHECK(tr("Play all") == "Play all");
    set_language(Language::zh);
    CHECK(tr("Play all") == "播放全部");
    CHECK(tr("An untranslated service message") == "An untranslated service message");
    set_language(Language::en);
    CHECK(tr("Play all") == "Play all");
    set_language(before);
}

TEST_CASE("localized messages retain their arguments") {
    const Language before = language();
    set_language(Language::en);
    CHECK(trf("{} songs", 42) == "42 songs");
    CHECK(trf("Added to {}", "Road trip") == "Added to Road trip");
    set_language(Language::zh);
    CHECK(trf("{} songs", 42) == "42 首");
    CHECK(trf("Added to {}", "Road trip") == "已添加到Road trip");
    set_language(before);
}

TEST_CASE("cached card counts follow the selected language") {
    const Language before = language();
    const lyren::Card card{.subtitle = "DJ", .count = 12, .count_kind = lyren::Card::Count::songs};
    set_language(Language::en);
    CHECK(lyren::card_subtitle(card) == "12 songs · DJ");
    set_language(Language::zh);
    CHECK(lyren::card_subtitle(card) == "12 首 · DJ");
    set_language(before);
}
