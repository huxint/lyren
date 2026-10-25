#pragma once
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace lyren {

class Loop;

enum class Decorations { client, server };
enum class GpuChoice { integrated, discrete };
enum class Quality { standard, higher, exhigh, lossless, hires };
enum class TrialMode { skip, play };
enum class ThemeMode { dark, light };

struct Config {
    struct Window {
        Decorations decorations = Decorations::client;
        double opacity = 1.0;
    } window;
    struct Gpu {
        GpuChoice device = GpuChoice::integrated;
    } gpu;
    struct Render {
        int max_fps = 60;
    } render;
    struct Playback {
        Quality quality = Quality::lossless;
        TrialMode trial = TrialMode::skip;
    } playback;
    struct Theme {
        ThemeMode mode = ThemeMode::dark;
        std::string accent = "auto";
        std::string font = "sans-serif";
        double font_size = 14;
        double radius = 10;
        double motion = 1.0;
        struct Colors {
            std::optional<std::string> bg, surface, elevated, border, text, text_dim, danger;
        } colors;
    } theme;
    struct DesktopLyrics {
        double font_size = 30;
        std::string highlight = "accent";
        std::string color = "#ffffff";
        std::string outline = "#000000a0";
    } desktop_lyrics;
};

// Parses TOML; the error string includes the line and column.
std::expected<Config, std::string> parse_config(std::string_view toml);
std::string_view default_config_text();

// Loads ~/.config/lyren/config.toml, writing the commented default on first run.
struct ConfigFile {
    std::filesystem::path path;
    Config config;
    std::string error;  // last load error; config keeps the previous good value
};
ConfigFile load_config();

// Watches the config file and calls on_change after each save (debounced).
class ConfigWatcher {
public:
    ConfigWatcher(Loop& loop, std::filesystem::path path, std::function<void()> on_change);
    ~ConfigWatcher();

private:
    Loop& loop_;
    std::filesystem::path path_;
    std::function<void()> on_change_;
    int fd_;
    uint64_t pending_ = 0;
};

}  // namespace lyren
