#include "sys/paths.hpp"

#include <cstdlib>

namespace lyren::paths {

static std::filesystem::path xdg(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    std::filesystem::path base = v && *v ? std::filesystem::path(v) : std::filesystem::path(std::getenv("HOME")) / fallback;
    auto dir = base / "lyren";
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path config_dir() { return xdg("XDG_CONFIG_HOME", ".config"); }
std::filesystem::path state_dir() { return xdg("XDG_STATE_HOME", ".local/state"); }
std::filesystem::path cache_dir() { return xdg("XDG_CACHE_HOME", ".cache"); }

}  // namespace lyren::paths
