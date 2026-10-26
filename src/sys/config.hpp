#pragma once
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.hpp"

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
        bool normalize = false;  // apply the loudness gain the service reports for each track
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
        bool enabled = false;
        double font_size = 30;
        std::string highlight = "accent";
        std::string color = "#ffffff";
        std::string outline = "#000000a0";
    } desktop_lyrics;
};

// Parses TOML; the error string includes the line and column.
std::expected<Config, std::string> parse_config(std::string_view toml);
std::string_view default_config_text();
// A parse error as a message for the user.
std::string describe_config_error(const std::string& err);

// Sets key in [section] to value (TOML syntax), keeping the rest of the text and its comments. A missing key goes
// after the section's last setting, a missing section at the end.
std::string set_config_value(std::string_view toml, std::string_view section, std::string_view key, std::string_view value);

// Loads ~/.config/lyren/config.toml, writing the commented default on first run.
struct ConfigFile {
    std::filesystem::path path;
    Config config;
    std::string error;  // last load error; config keeps the previous good value
    std::string text;   // as last read or written, so saving the same text again changes nothing
};
ConfigFile load_config();
// Writes one setting into the file and reloads f.config from the new text. The file is left alone when the result
// does not parse, e.g. because of a broken hand edit elsewhere.
Result<void> save_setting(ConfigFile& f, std::string_view section, std::string_view key, std::string_view value);

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
