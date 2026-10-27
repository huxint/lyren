#include "qq/crypto.hpp"

#include <openssl/evp.h>
#include <zlib.h>

#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <span>

#include "ncm/crypto.hpp"

namespace lyren::qq {

    uint32_t hash33(std::string_view s, uint32_t h) {
        for (unsigned char c : s) {
            h = h * 33 + c;
        }
        return h & 0x7fffffff;
    }

    std::string sign(std::string_view body) {
        static constexpr std::array head = {23, 14, 6, 36, 16, 7, 19};
        static constexpr std::array tail = {16, 1, 32, 12, 19, 27, 8, 3};
        static constexpr std::array<uint8_t, 20> mask = {89, 39, 179, 150, 218, 82, 58, 252, 177, 52, 186, 123, 120, 64, 242, 133, 143, 161, 121, 179};
        std::array<unsigned char, 20> d;
        unsigned n = 0;
        EVP_Digest(body.data(), body.size(), d.data(), &n, EVP_sha1(), nullptr);
        std::string h = ncm::hex({reinterpret_cast<const char *>(d.data()), d.size()}, true), mixed(d.size(), '\0');
        for (size_t i = 0; i < d.size(); ++i) {
            mixed[i] = char(d[i] ^ mask[i]);
        }
        std::string out = "zzc";
        for (int i : head) {
            out += h[size_t(i)];
        }
        for (char c : ncm::base64(mixed)) {
            if (c != '=' && c != '/' && c != '+') {
                out += c;
            }
        }
        for (int i : tail) {
            out += h[size_t(i)];
        }
        for (char &c : out) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    namespace {

        constexpr std::array<uint8_t, 64> ip = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                                57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
        constexpr std::array<uint8_t, 64> fp = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                                36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
        constexpr std::array<uint8_t, 48> expansion = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                                                       16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
        constexpr std::array<uint8_t, 32> pbox = {16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};
        constexpr std::array<uint8_t, 56> pc1 = {57, 49, 41, 33, 25, 17, 9,  1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
                                                 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4};
        constexpr std::array<uint8_t, 48> pc2 = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
                                                 41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
        constexpr std::array<uint8_t, 16> shifts = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
        // The standard S-boxes, four rows of 16 each, but for S2 row 1 column 7 (15, not 14) and S4 row 3 column 5 (10, not 1).
        constexpr std::array<std::array<uint8_t, 64>, 8> sbox = {{
            {14, 4, 13, 1, 2,  15, 11, 8,  3,  10, 6, 12, 5, 9,  0, 7, 0,  15, 7, 4, 14, 2, 13, 1, 10, 6,  12, 11, 9,  5, 3, 8,
             4,  1, 14, 8, 13, 6,  2,  11, 15, 12, 9, 7,  3, 10, 5, 0, 15, 12, 8, 2, 4,  9, 1,  7, 5,  11, 3,  14, 10, 0, 6, 13},
            {15, 1,  8, 14, 6,  11, 3,  4, 9, 7, 2,  13, 12, 0, 5, 10, 3,  13, 4,  7, 15, 2,  8, 15, 12, 0, 1, 10, 6, 9, 11, 5,
             0,  14, 7, 11, 10, 4,  13, 1, 5, 8, 12, 6,  9,  3, 2, 15, 13, 8,  10, 1, 3,  15, 4, 2,  11, 6, 7, 12, 0, 5, 14, 9},
            {10, 0, 9, 14, 6, 3,  15, 5, 1,  13, 12, 7,  11, 4,  2,  8, 13, 7,  0,  9, 3, 4, 6, 10, 2, 8,  5,  14, 12, 11, 15, 1,
             13, 6, 4, 9,  8, 15, 3,  0, 11, 1,  2,  12, 5,  10, 14, 7, 1,  10, 13, 0, 6, 9, 8, 7,  4, 15, 14, 3,  11, 5,  2,  12},
            {7,  13, 14, 3, 0,  6,  9, 10, 1,  2, 8, 5,  11, 12, 4, 15, 13, 8,  11, 5, 6,  15, 0,  3, 4, 7, 2, 12, 1,  10, 14, 9,
             10, 6,  9,  0, 12, 11, 7, 13, 15, 1, 3, 14, 5,  2,  8, 4,  3,  15, 0,  6, 10, 10, 13, 8, 9, 4, 5, 11, 12, 7,  2,  14},
            {2, 12, 4, 1,  7,  10, 11, 6, 8,  5, 3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4, 7,  13, 1,  5, 0,  15, 10, 3,  9, 8, 6,
             4, 2,  1, 11, 10, 13, 7,  8, 15, 9, 12, 5,  6,  3, 0,  14, 11, 8,  12, 7,  1, 14, 2,  13, 6, 15, 0,  9,  10, 4, 5, 3},
            {12, 1,  10, 15, 9, 2, 6,  8, 0, 13, 3, 4,  14, 7,  5,  11, 10, 15, 4, 2,  7, 12, 9,  5,  6,  1,  13, 14, 0, 11, 3, 8,
             9,  14, 15, 5,  2, 8, 12, 3, 7, 0,  4, 10, 1,  13, 11, 6,  4,  3,  2, 12, 9, 5,  15, 10, 11, 14, 1,  7,  6, 0,  8, 13},
            {4, 11, 2,  14, 15, 0, 8, 13, 3,  12, 9, 7, 5, 10, 6, 1, 13, 0,  11, 7, 4, 9, 1,  10, 14, 3, 5, 12, 2,  15, 8, 6,
             1, 4,  11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5,  9, 2, 6,  11, 13, 8, 1, 4, 10, 7,  9,  5, 0, 15, 14, 2,  3, 12},
            {13, 2,  8, 4, 6, 15, 11, 1, 10, 9, 3,  14, 5,  0, 12, 7, 1, 15, 13, 8, 10, 3,  7, 4,  12, 5,  6, 11, 0, 14, 9, 2,
             7,  11, 4, 1, 9, 12, 14, 2, 0,  6, 10, 13, 15, 3, 5,  8, 2, 1,  14, 7, 4,  10, 8, 13, 15, 12, 9, 0,  3, 5,  6, 11},
        }};

        // The bits of v, which is width bits wide and numbered from 1 at the top, in the order of table.
        uint64_t permute(uint64_t v, std::span<const uint8_t> table, int width) {
            uint64_t out = 0;
            for (uint8_t t : table) {
                out = out << 1 | (v >> (width - t) & 1);
            }
            return out;
        }

        // QQ's DES reads the bytes of each 4-byte half of a block or a key backwards.
        uint64_t flip(uint64_t v) {
            return uint64_t(std::byteswap(uint32_t(v >> 32))) << 32 | std::byteswap(uint32_t(v));
        }

        uint64_t load(std::string_view b) {
            uint64_t v = 0;
            for (char c : b) {
                v = v << 8 | uint8_t(c);
            }
            return v;
        }

        using Keys = std::array<uint64_t, 16>;

        // Each subkey takes the D half one bit further left than the standard: PC-2 reads (D << 1).
        Keys schedule(std::string_view key) {
            uint64_t cd = permute(flip(load(key)), pc1, 64);
            uint32_t c = uint32_t(cd >> 28), d = uint32_t(cd & 0xfffffff);
            Keys out;
            for (int i = 0; i < 16; ++i) {
                auto rotate = [s = shifts[i]](uint32_t x) {
                    return (x << s | x >> (28 - s)) & 0xfffffff;
                };
                c = rotate(c), d = rotate(d);
                out[size_t(i)] = permute(uint64_t(c) << 28 | ((d << 1) & 0xfffffff), pc2, 56);
            }
            return out;
        }

        uint32_t feistel(uint32_t r, uint64_t k) {
            uint64_t x = permute(r, expansion, 32) ^ k;
            uint32_t out = 0;
            for (int i = 0; i < 8; ++i) {
                unsigned six = unsigned(x >> (42 - 6 * i)) & 63;
                out = out << 4 | sbox[i][(six & 0x20) | (six & 1) << 4 | (six >> 1 & 15)];
            }
            return uint32_t(permute(out, pbox, 32));
        }

        uint64_t des(uint64_t block, const Keys &k, bool decrypt) {
            uint64_t v = permute(flip(block), ip, 64);
            uint32_t l = uint32_t(v >> 32), r = uint32_t(v);
            for (int i = 0; i < 16; ++i) {
                uint32_t next = l ^ feistel(r, k[size_t(decrypt ? 15 - i : i)]);
                l = r, r = next;
            }
            return flip(permute(uint64_t(r) << 32 | l, fp, 64));
        }

        // Inflates to the end of the zlib stream; the cipher's padding after it is ignored.
        std::string unzip(std::string_view data) {
            z_stream z{};
            if (inflateInit(&z) != Z_OK) {
                return {};
            }
            z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
            z.avail_in = uInt(data.size());
            std::string out;
            int rc = Z_OK;
            while (rc == Z_OK) {
                std::array<char, 16384> buf;
                z.next_out = reinterpret_cast<Bytef *>(buf.data());
                z.avail_out = buf.size();
                rc = inflate(&z, Z_NO_FLUSH);
                out.append(buf.data(), buf.size() - z.avail_out);
            }
            inflateEnd(&z);
            return rc == Z_STREAM_END ? out : std::string{};
        }

    } // namespace

    uint64_t qrc_block(uint64_t block) {
        static constexpr std::string_view key = "!@#)(*$%123ZXC!@!@#)(NHL";
        static const Keys k1 = schedule(key.substr(0, 8)), k2 = schedule(key.substr(8, 8)), k3 = schedule(key.substr(16));
        return des(des(des(block, k3, true), k2, false), k1, true);
    }

    std::string qrc_decode(std::string_view hex) {
        std::string data(hex.size() / 2, '\0');
        for (size_t i = 0; i < data.size(); ++i) {
            unsigned v = 0;
            auto r = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, v, 16);
            if (r.ptr != hex.data() + 2 * i + 2) {
                return {};
            }
            data[i] = char(v);
        }
        for (size_t i = 0; i + 8 <= data.size(); i += 8) {
            uint64_t b = qrc_block(load(std::string_view(data).substr(i, 8)));
            for (size_t j = 8; j-- > 0; b >>= 8) {
                data[i + j] = char(b);
            }
        }
        return unzip(data);
    }

} // namespace lyren::qq
