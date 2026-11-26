#include "core/encoding.hpp"

#include <openssl/evp.h>
#include <zlib.h>

#include <array>

namespace lyren::encoding {

    std::string hex(std::string_view bytes, bool upper) {
        const std::string_view digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char byte : bytes) {
            out += digits[byte >> 4], out += digits[byte & 15];
        }
        return out;
    }

    std::string base64(std::string_view bytes) {
        std::string out(4 * ((bytes.size() + 2) / 3), '\0');
        const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
        out.resize(static_cast<size_t>(size));
        return out;
    }

    std::string unbase64(std::string_view text) {
        std::string out(3 * (text.size() / 4), '\0');
        const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()), reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
        if (size < 0) {
            return {};
        }
        // EVP_DecodeBlock includes the bytes represented by padding.
        const size_t padding = text.ends_with("==") ? 2 : text.ends_with('=') ? 1 : 0;
        out.resize(static_cast<size_t>(size) - padding);
        return out;
    }

    void append_url_encoded(std::string &out, std::string_view value, UrlStyle style) {
        constexpr std::string_view digits = "0123456789ABCDEF";
        const std::string_view extra = style == UrlStyle::uri_component ? "-_.!~*'()" : style == UrlStyle::form ? "-_.*" : "-_.~";
        const bool plus = style == UrlStyle::form;
        for (const unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || extra.contains(static_cast<char>(c))) {
                out += static_cast<char>(c);
            } else if (plus && c == ' ') {
                out += '+';
            } else {
                out += '%';
                out += digits[c >> 4];
                out += digits[c & 15];
            }
        }
    }

    std::string url_encode(std::string_view value, UrlStyle style) {
        std::string out;
        out.reserve(value.size());
        append_url_encoded(out, value, style);
        return out;
    }

    Result<std::string> inflate(std::string_view input, int window_bits, size_t max_bytes) {
        z_stream z{};
        if (inflateInit2(&z, window_bits) != Z_OK) {
            return std::unexpected("Decompression initialization failed");
        }
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        z.avail_in = static_cast<uInt>(input.size());
        std::string out;
        std::array<char, 16384> buffer;
        int status = Z_OK;
        while (status == Z_OK) {
            z.next_out = reinterpret_cast<Bytef *>(buffer.data());
            z.avail_out = static_cast<uInt>(buffer.size());
            status = ::inflate(&z, Z_NO_FLUSH);
            const size_t produced = buffer.size() - z.avail_out;
            if (produced > max_bytes - out.size()) {
                inflateEnd(&z);
                return std::unexpected("Decompressed data exceeds limit");
            }
            out.append(buffer.data(), produced);
        }
        inflateEnd(&z);
        if (status != Z_STREAM_END) {
            return std::unexpected("Invalid compressed data");
        }
        return out;
    }

} // namespace lyren::encoding
