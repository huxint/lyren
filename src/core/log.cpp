#include "core/log.hpp"

#include <array>
#include <cstdio>
#include <print>

namespace lyren::log {

    void write(Level level, std::string_view msg) {
        static constexpr std::array tags = {"debug", "info", "warn", "error"};
        std::println(stderr, "{:5} {}", tags[static_cast<int>(level)], msg);
    }

} // namespace lyren::log
