#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <print>

namespace lyren::log {

static const auto start = std::chrono::steady_clock::now();

void write(Level level, std::string_view msg) {
    static constexpr const char* tags[] = {"debug", "info", "warn", "error"};
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::println(stderr, "{:9.3f} {:5} {}", t, tags[int(level)], msg);
}

}  // namespace lyren::log
