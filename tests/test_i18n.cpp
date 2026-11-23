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

TEST_CASE("contextual translations disambiguate identical English keys") {
    const Language before = language();
    set_language(Language::en);
    CHECK(tr("Following") == "Following");
    CHECK(tr_ctx("nav", "Following") == "Following");
    set_language(Language::zh);
    CHECK(tr("Following") == "已关注");
    CHECK(tr_ctx("nav", "Following") == "关注");
    CHECK(tr_ctx("missing", "Play all") == "播放全部");
    CHECK(short_number(1234567) == "123.5万");
    CHECK(short_number(250000000) == "2.5亿");
    set_language(Language::en);
    CHECK(short_number(1234) == "1.2K");
    CHECK(short_number(1234567) == "1.2M");
    set_language(before);
}
