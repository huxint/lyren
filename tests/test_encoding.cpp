#include <doctest/doctest.h>

#include "core/encoding.hpp"

using namespace lyren::encoding;

TEST_CASE("base64 matches RFC 4648 vectors and UTF-8 bytes") {
    for (auto [plain, encoded] :
         {std::pair{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}, {"中文\n\x01", "5Lit5paHCgE="}}) {
        CAPTURE(plain);
        CHECK(base64(plain) == encoded);
        CHECK(unbase64(encoded) == plain);
    }
}
