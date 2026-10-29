#include "core/encoding.hpp"

#include <openssl/evp.h>

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

} // namespace lyren::encoding
