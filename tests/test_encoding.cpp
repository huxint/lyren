#include <doctest/doctest.h>

#include <zlib.h>

#include "core/encoding.hpp"

using namespace lyren::encoding;

namespace {
    std::string deflate_with(std::string_view input, int window_bits) {
        z_stream zs{};
        REQUIRE(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK);
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());
        std::string out(deflateBound(&zs, static_cast<uLong>(input.size())) + 64, '\0');
        zs.next_out = reinterpret_cast<Bytef *>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());
        REQUIRE(deflate(&zs, Z_FINISH) == Z_STREAM_END);
        out.resize(zs.total_out);
        deflateEnd(&zs);
        return out;
    }
} // namespace

TEST_CASE("base64 matches RFC 4648 vectors and UTF-8 bytes") {
    for (auto [plain, encoded] :
         {std::pair{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}, {"中文\n\x01", "5Lit5paHCgE="}}) {
        CAPTURE(plain);
        CHECK(base64(plain) == encoded);
        CHECK(unbase64(encoded) == plain);
    }
    CHECK(unbase64("Zg==\r\n") == "f");
    CHECK(unbase64("Zm8= \t") == "fo");
    for (const auto invalid : {"=", "Zg=", "=g==", "Z=g=", "Zg=A", "Zg==="}) {
        CHECK(unbase64(invalid).empty());
    }
}

TEST_CASE("url_encode and query_string cover RFC 3986, URI component and form rules") {
    CHECK(url_encode("a-z_0.9~ !*'()&+中文", UrlStyle::rfc3986) == "a-z_0.9~%20%21%2A%27%28%29%26%2B%E4%B8%AD%E6%96%87");
    CHECK(url_encode("a-z_0.9~ !*'()&+", UrlStyle::uri_component) == "a-z_0.9~%20!*'()%26%2B");
    CHECK(url_encode("晴天 ~&+*", UrlStyle::form) == "%E6%99%B4%E5%A4%A9+%7E%26%2B*");
    CHECK(query_string({{"k 1", "a&b"}, {"k2", "中文"}}, UrlStyle::rfc3986) == "k%201=a%26b&k2=%E4%B8%AD%E6%96%87");
}

TEST_CASE("inflate handles zlib, gzip, raw deflate and output limits") {
    const std::string sample = "Lyren compression test payload — 音乐播放器 " + std::string(1200, 'x');
    for (int wbits : {15, 16 + 15, -15}) {
        CAPTURE(wbits);
        const auto compressed = deflate_with(sample, wbits);
        auto plain = inflate(compressed, wbits);
        REQUIRE(plain);
        CHECK(*plain == sample);
        CHECK_FALSE(inflate(compressed, wbits, 64));
    }
    CHECK_FALSE(inflate("not compressed", 15));
}
