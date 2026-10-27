#include "core/log.hpp"

#include <cstdio>
#include <print>

namespace lyren::log {

    void write(Level level, std::string_view msg) {
        static constexpr const char *tags[] = {"debug", "info", "warn", "error"};
        std::println(stderr, "{:5} {}", tags[int(level)], msg);
    }

} // namespace lyren::log
