#pragma once
#include <string>
#include <string_view>

namespace lyren::ncm {

    std::string md5_hex(std::string_view s);
    std::string random_hex(size_t digits, bool upper = false);

    // eapi: url is the /api/... path, text the JSON body. Returns the params form field.
    std::string eapi(std::string_view url, std::string_view text);

    // weapi: two AES-CBC layers, the second with a random 16-character key that is sent RSA-wrapped.
    struct Weapi {
        std::string params, enc_sec_key;
    };
    Weapi weapi(std::string_view text);
    Weapi weapi(std::string_view text, std::string_view secret);

} // namespace lyren::ncm
