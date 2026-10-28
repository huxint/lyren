#pragma once
#include <cstdint>
#include <format>
#include <string_view>

namespace lyren::log {

    enum class Level : uint8_t {
        debug,
        info,
        warn,
        error
    };
    inline Level min_level = Level::info;

    void write(Level level, std::string_view msg);

    template <class... A>
    void debug(std::format_string<A...> f, A &&...a) {
        if (min_level <= Level::debug) {
            write(Level::debug, std::format(f, std::forward<A>(a)...));
        }
    }
    template <class... A>
    void info(std::format_string<A...> f, A &&...a) {
        if (min_level <= Level::info) {
            write(Level::info, std::format(f, std::forward<A>(a)...));
        }
    }
    template <class... A>
    void warn(std::format_string<A...> f, A &&...a) {
        write(Level::warn, std::format(f, std::forward<A>(a)...));
    }
    template <class... A>
    void error(std::format_string<A...> f, A &&...a) {
        write(Level::error, std::format(f, std::forward<A>(a)...));
    }

} // namespace lyren::log
