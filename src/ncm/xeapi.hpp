#pragma once
#include <string>
#include <string_view>

#include "core/result.hpp"

namespace lyren::ncm {

    struct XeapiKey {
        std::string public_key; // 32 raw X25519 bytes
        std::string version, tag;
    };
    struct XeapiBody {
        std::string c, s, r;
    };

    std::string xeapi_signature(std::string_view timestamp, std::string_view nonce);
    Result<XeapiKey> parse_xeapi_key(std::string_view response, std::string_view nonce);
    // content is the endpoint's form body. Each request negotiates its own ephemeral key.
    Result<XeapiBody> xeapi_encrypt(std::string_view content, const XeapiKey &key);
    Result<std::string> xeapi_response(std::string_view ciphertext);

} // namespace lyren::ncm
