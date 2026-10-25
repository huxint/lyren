#pragma once
#include <filesystem>

namespace lyren::paths {

// XDG base directories with a "lyren" subdirectory; created on first use.
std::filesystem::path config_dir();
std::filesystem::path state_dir();
std::filesystem::path cache_dir();

}  // namespace lyren::paths
