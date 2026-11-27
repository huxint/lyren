#include <doctest/doctest.h>
#include "language_guard.hpp"

#include "core/i18n.hpp"
#include "media/types.hpp"

using namespace lyren;

TEST_CASE("language changes apply immediately without changing source keys") {
    const lyren::test::LanguageGuard language_guard;
    set_language(Language::en);
    CHECK(tr("Play all") == "Play all");
    set_language(Language::zh);
    CHECK(tr("Play all") == "播放全部");
    CHECK(tr("An untranslated service message") == "An untranslated service message");
    set_language(Language::en);
    CHECK(tr("Play all") == "Play all");
}

TEST_CASE("cached card counts follow the selected language") {
    const lyren::test::LanguageGuard language_guard;
    const lyren::Card card{.subtitle = "DJ", .count = 12, .count_kind = lyren::Card::Count::songs};
    set_language(Language::en);
    CHECK(lyren::card_subtitle(card) == "12 songs · DJ");
    set_language(Language::zh);
    CHECK(lyren::card_subtitle(card) == "12 首 · DJ");
}

TEST_CASE("contextual translations disambiguate identical English keys") {
    const lyren::test::LanguageGuard language_guard;
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
}

TEST_CASE("common English UI tokens stay untranslated and contextual Chinese copy stays concise") {
    const lyren::test::LanguageGuard language_guard;
    set_language(Language::zh);
    CHECK(tr("Settings") == "Settings");
    CHECK(tr("Dark") == "Dark");
    CHECK(tr("Light") == "Light");
    CHECK(tr("Language") == "Language");
    CHECK(tr("Open") == "Open");
    CHECK(trf("{}/{} connected", 2, 5) == "2/5 connected");
    CHECK(trf("Here for {}", trf("{} years", 5)) == "5 years");
    CHECK(tr("Added to liked songs") == "已喜欢");
    CHECK(tr("Could not connect to the Qishui signing service; check signer_url and ensure it is running") == "无法连接汽水签名服务，请检查服务地址并确认服务已启动");
    CHECK(trf("Qishui signing service request failed (HTTP {})", 404) == "汽水签名服务请求失败 (HTTP 404)");
}
