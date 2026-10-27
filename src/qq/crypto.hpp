#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace lyren::qq {

    // h = h * 33 + c over the characters (the inputs are ASCII), kept to 31 bits: the QR poll token from qrsig (h 0), g_tk
    // from p_skey or musickey (h 5381).
    uint32_t hash33(std::string_view s, uint32_t h = 0);

    // The zzc signature of a request body, for musics.fcg?sign=.
    std::string sign(std::string_view body);

    // One 8-byte block through QQ Music's triple DES (decrypt with the third key, encrypt with the second, decrypt with
    // the first), which differs from the standard in byte order, the key schedule and two S-box entries.
    uint64_t qrc_block(uint64_t block);

    // A lyrics payload: hex, the triple DES above block by block, then zlib. Empty when it does not decode.
    std::string qrc_decode(std::string_view hex);

} // namespace lyren::qq
