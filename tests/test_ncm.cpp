#include <doctest/doctest.h>
#include <openssl/evp.h>

#include "ncm/client.hpp"
#include "ncm/crypto.hpp"

using namespace lyren::ncm;

static std::string aes_cbc_decrypt(const std::string& data, const char* key) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(c, EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char*>(key), reinterpret_cast<const unsigned char*>("0102030405060708"));
    std::string out(data.size(), '\0');
    int n1 = 0, n2 = 0;
    EVP_DecryptUpdate(c, reinterpret_cast<unsigned char*>(out.data()), &n1, reinterpret_cast<const unsigned char*>(data.data()), int(data.size()));
    EVP_DecryptFinal_ex(c, reinterpret_cast<unsigned char*>(out.data()) + n1, &n2);
    EVP_CIPHER_CTX_free(c);
    out.resize(size_t(n1 + n2));
    return out;
}

TEST_CASE("eapi known vector") {
    // From YetEaseMusic crypto.rs, computed independently with md5sum and openssl enc.
    CHECK(eapi("/api/song/lyric", R"({"id":1})") ==
          "9B1595479B79266065903BA79E9883029338432CD7D98D1A3626E997A2C531219B13F34AFEC3DA8E69251F8EB21D1A1159D38E428357226CDD469CCFDC2CC954D27CF6A4C22E1023A03E8FAF9F5182C7BC2BCB0C4A7E53B249894EBF962CD6E6");
}

TEST_CASE("weapi layers decrypt back") {
    auto w = weapi(R"({"a":1})", "abcdefghijklmnop");
    CHECK(w.enc_sec_key.size() == 256);
    std::string first = aes_cbc_decrypt(unbase64(w.params), "abcdefghijklmnop");
    CHECK(aes_cbc_decrypt(unbase64(first), "0CoJUm6Qyw8W8jud") == R"({"a":1})");
    CHECK(weapi("{}").params.size() > 0);
}

TEST_CASE("rsa known vector") {
    // From YetEaseMusic crypto.rs, computed with Python's pow(m, 65537, n).
    std::string got = rsa_hex("aaaaaaaaaaaaaaaa");
    CHECK(got.substr(0, 32) == "d473b9eca232f1b4090dd606b0df86de");
    CHECK(got.substr(224) == "1153d9d85e1ea3d78fa8d9210d581cee");
}

TEST_CASE("base64 round trip and json strings") {
    for (std::string s : {"", "a", "ab", "abc", "abcd", "中文\n\x01"}) CHECK(unbase64(base64(s)) == s);
    CHECK(json_string("a\"b\\c\nd\x01") == R"("a\"b\\c\nd\u0001")");
}
