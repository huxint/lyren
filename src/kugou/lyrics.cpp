#include "kugou/kugou.hpp"

#include <zlib.h>

#include <array>

#include "core/encoding.hpp"
#include "kugou/json.hpp"

namespace lyren::kugou {

    namespace {
        Result<std::string> unpack(std::string_view bytes) {
            if (!bytes.starts_with("krc1") || bytes.size() <= 4) {
                return std::unexpected("Invalid KuGou Music lyric data");
            }
            constexpr std::array<unsigned char, 16> key{0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47, 0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69};
            std::string compressed(bytes.substr(4));
            for (size_t i = 0; i < compressed.size(); ++i) {
                compressed[i] ^= static_cast<char>(key[i % key.size()]);
            }
            z_stream z{};
            if (inflateInit(&z) != Z_OK) {
                return std::unexpected("Could not decode KuGou Music lyrics");
            }
            z.next_in = reinterpret_cast<Bytef *>(compressed.data());
            z.avail_in = static_cast<uInt>(compressed.size());
            std::string result;
            int status = Z_OK;
            while (status == Z_OK && result.size() < 8 * 1024 * 1024) {
                std::array<char, 16384> buffer;
                z.next_out = reinterpret_cast<Bytef *>(buffer.data());
                z.avail_out = buffer.size();
                status = inflate(&z, Z_NO_FLUSH);
                result.append(buffer.data(), buffer.size() - z.avail_out);
            }
            inflateEnd(&z);
            if (status != Z_STREAM_END) {
                return std::unexpected("Invalid KuGou Music lyric data");
            }
            return result;
        }

    } // namespace

    Result<std::vector<LyricLine>> parse_krc(std::string_view bytes) {
        auto plain = unpack(bytes);
        if (!plain) {
            return std::unexpected(plain.error());
        }
        return lyren::parse_krc(*plain);
    }

    Result<std::vector<LyricLine>> parse_lyric_download(std::string_view body) {
        auto parsed = json::parse(body);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto content = encoding::unbase64(json::text(*parsed, "content"));
        if (content.empty()) {
            return std::unexpected("Invalid KuGou Music lyric data");
        }
        const auto format = json::text(*parsed, "fmt");
        if (format == "lrc") {
            return parse_lyrics(content);
        }
        if (format == "krc") {
            return parse_krc(content);
        }
        return std::unexpected("Unsupported KuGou Music lyric format");
    }

} // namespace lyren::kugou
