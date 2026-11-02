#include "core/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>

#include "core/encoding.hpp"

namespace lyren::crypto {

    std::string md5_hex(std::string_view bytes) {
        std::array<unsigned char, 16> digest;
        unsigned size = 0;
        EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_md5(), nullptr);
        return encoding::hex({reinterpret_cast<const char *>(digest.data()), size});
    }

    std::string random_hex(size_t digits, bool upper) {
        std::string bytes((digits + 1) / 2, '\0');
        RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
        return encoding::hex(bytes, upper).substr(0, digits);
    }

} // namespace lyren::crypto
