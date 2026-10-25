#include <doctest/doctest.h>

#include "core/utf8.hpp"

using namespace lyren;

TEST_CASE("utf8 decode, append and boundaries") {
    std::string s = "a周🎵";
    size_t i = 0;
    CHECK(utf8::decode(s, i) == U'a');
    CHECK(utf8::decode(s, i) == U'周');
    CHECK(utf8::decode(s, i) == U'🎵');
    CHECK(i == s.size());

    std::string out;
    for (char32_t cp : {U'a', U'周', U'🎵'}) utf8::append(out, cp);
    CHECK(out == s);

    CHECK(utf8::next(s, 0) == 1);
    CHECK(utf8::next(s, 1) == 4);
    CHECK(utf8::prev(s, s.size()) == 4);
    CHECK(utf8::prev(s, 4) == 1);
}

TEST_CASE("malformed utf8 becomes U+FFFD one byte at a time") {
    std::string s = "\xff\xc3(";
    size_t i = 0;
    CHECK(utf8::decode(s, i) == 0xfffd);
    CHECK(i == 1);
    CHECK(utf8::decode(s, i) == 0xfffd);
    CHECK(i == 2);
    CHECK(utf8::decode(s, i) == U'(');
}
