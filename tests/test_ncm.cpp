#include <doctest/doctest.h>

#include "ncm/crypto.hpp"

using namespace lyren::ncm;

TEST_CASE("eapi known vector") {
    // From YetEaseMusic crypto.rs, computed independently with md5sum and openssl enc.
    CHECK(eapi("/api/song/lyric", R"({"id":1})") == "9B1595479B79266065903BA79E9883029338432CD7D98D1A3626E997A2C531219B13F34AFEC3DA8E69251F8EB21D1A1159D38E428357226CDD469CCFDC2CC9"
                                                    "54D27CF6A4C22E1023A03E8FAF9F5182C7BC2BCB0C4A7E53B249894EBF962CD6E6");
}

TEST_CASE("weapi matches independent AES and RSA vectors") {
    // AES-CBC computed with openssl enc; RSA with Python's pow(m, 65537, n).
    auto w = weapi(R"({"a":1})", "abcdefghijklmnop");
    CHECK(w.params == "Ho/4Pu1bYyrzwcnXPnTcXm1Gb04eSzwGSNb4NI3YqZY=");
    CHECK(w.enc_sec_key == "d15a1683c992095d0c234c19966605c5c5964911268bbeda8cb8d08d834913e59"
                           "d53b32358903a121b5fca784c1f5ae44951fd02524df58ecc98e52cc7cf8689b"
                           "42c2e93ddf05b0592512d87f5960467e2f086c018849d76014d323500e30f13e"
                           "f4cafbb0cf5a66731a3f1776c75ca35d0062dac70a3e33245afabcf47938487");
}

TEST_CASE("rsa known vector") {
    // From YetEaseMusic crypto.rs, computed with Python's pow(m, 65537, n).
    CHECK(rsa_hex("aaaaaaaaaaaaaaaa") == "d473b9eca232f1b4090dd606b0df86de318748dd2eec307e4ed4345030fc4ee30"
                                         "331e598f41d5a6f5befaab94630ea1a1eda7cfade84fbec1a907913d2e4d2c87"
                                         "44bc572b99a050075e075b4537f645ecfa994f95906c32818e076aeda6bdb906b"
                                         "fa0bb96c4cf4bc3ed6d9ab76cf08441153d9d85e1ea3d78fa8d9210d581cee");
}

TEST_CASE("base64 matches RFC 4648 vectors and UTF-8 bytes") {
    for (auto [plain, encoded] :
         {std::pair{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}, {"中文\n\x01", "5Lit5paHCgE="}}) {
        CAPTURE(plain);
        CHECK(base64(plain) == encoded);
        CHECK(unbase64(encoded) == plain);
    }
}
