#pragma once
#include <string>
#include <string_view>

namespace lyren::encoding {

    // Views contain raw bytes and may include NULs. OpenSSL's block API accepts byte counts fitting in int.
    std::string hex(std::string_view bytes, bool upper = false);
    std::string base64(std::string_view bytes);
    // Standard padded Base64; an invalid input produces an empty string.
    std::string unbase64(std::string_view text);

} // namespace lyren::encoding
