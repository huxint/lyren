#include "sys/config.hpp"

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <format>
#include <fstream>
#include <glaze/toml.hpp>

#include "core/log.hpp"
#include "core/loop.hpp"
#include "sys/paths.hpp"

template <>
struct glz::meta<lyren::Decorations> {
    using enum lyren::Decorations;
    static constexpr auto value = enumerate(client, server);
};
template <>
struct glz::meta<lyren::GpuChoice> {
    using enum lyren::GpuChoice;
    static constexpr auto value = enumerate(integrated, discrete);
};
template <>
struct glz::meta<lyren::Quality> {
    using enum lyren::Quality;
    static constexpr auto value = enumerate(standard, higher, exhigh, lossless, hires);
};
template <>
struct glz::meta<lyren::TrialMode> {
    using enum lyren::TrialMode;
    static constexpr auto value = enumerate(skip, play);
};
template <>
struct glz::meta<lyren::ThemeMode> {
    using enum lyren::ThemeMode;
    static constexpr auto value = enumerate(dark, light);
};

template <>
struct glz::meta<lyren::Language> {
    using enum lyren::Language;
    static constexpr auto value = enumerate(en, zh);
};
template <>
struct glz::meta<lyren::Config::DesktopLyrics::Island> {
    using T = lyren::Config::DesktopLyrics::Island;
    // The open island has one layout now; files from before keep parsing.
    static constexpr auto value = object("cover", &T::cover, "progress", &T::progress, "details", glz::skip{}, "controls", glz::skip{}, "volume", glz::skip{});
};
template <>
struct glz::meta<lyren::Config::DesktopLyrics> {
    using T = lyren::Config::DesktopLyrics;
    // The old visibility setting is now persisted with the player state.
    static constexpr auto value =
        object("enabled", glz::skip{}, "font_size", &T::font_size, "highlight", &T::highlight, "color", &T::color, "outline", &T::outline, "island", &T::island);
};

namespace lyren {

    static const unsigned char default_config[] = {
#embed "default-config.toml"
    };

    std::string_view default_config_text() {
        return {reinterpret_cast<const char *>(default_config), sizeof default_config};
    }

    std::expected<Config, std::string> parse_config(std::string_view toml) {
        Config c;
        std::string buf(toml);
        if (auto ec = glz::read<glz::opts{.format = glz::TOML}>(c, buf)) {
            return std::unexpected(glz::format_error(ec, buf));
        }
        return c;
    }

    // glaze reports "line:col: code" followed by the offending line.
    std::string describe_config_error(const std::string &err) {
        auto nl = err.find('\n');
        std::string head = err.substr(0, nl), context;
        if (nl != std::string::npos) {
            auto end = err.find('\n', nl + 1);
            context = err.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1);
            context.erase(0, context.find_first_not_of(' '));
        }
        auto colon = head.find(':');
        std::string line = head.substr(0, colon);
        std::string code = head.substr(head.find(": ") == std::string::npos ? head.size() : head.find(": ") + 2);
        return std::format("Config line {} is invalid ({}): {}; previous settings were kept", line, code, context);
    }

    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
            s.remove_suffix(1);
        }
        return s;
    }

    static size_t value_size(std::string_view text) {
        char quote = 0;
        size_t end = 0;
        for (; end < text.size(); ++end) {
            char c = text[end];
            if (quote) {
                if (c == quote) {
                    quote = 0;
                } else if (quote == '"' && c == '\\') {
                    ++end;
                }
            } else if (c == '#') {
                break;
            } else if (c == '"' || c == '\'') {
                quote = c;
            }
        }
        return trim(text.substr(0, end)).size();
    }

    std::string set_config_value(std::string_view toml, std::string_view section, std::string_view key, std::string_view value) {
        std::string line = std::format("{} = {}", key, value);
        std::string_view current;               // "" before the first header
        size_t insert = std::string_view::npos; // end of the section's header or last setting
        for (size_t pos = 0; pos < toml.size();) {
            size_t end = std::min(toml.find('\n', pos), toml.size());
            std::string_view l = trim(toml.substr(pos, end - pos));
            if (l.starts_with('[')) {
                current = trim(l.substr(1, l.find(']') - 1));
                if (current == section) {
                    insert = end;
                }
            } else if (current == section && !l.empty() && !l.starts_with('#')) {
                if (l.starts_with(key) && trim(l.substr(key.size())).starts_with('=')) {
                    if (value.empty()) {
                        return std::string(toml.substr(0, pos)) + std::string(toml.substr(std::min(end + 1, toml.size())));
                    }
                    auto old = trim(l.substr(l.find('=') + 1));
                    size_t begin = static_cast<size_t>(old.data() - toml.data());
                    return std::format("{}{}{}", toml.substr(0, begin), value, toml.substr(begin + value_size(old)));
                }
                insert = end;
            }
            pos = end + 1;
        }
        if (value.empty()) {
            return std::string(toml);
        }
        if (insert != std::string_view::npos) {
            return std::format("{}\n{}{}", toml.substr(0, insert), line, toml.substr(insert));
        }
        std::string out(toml);
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        return out + std::format("\n[{}]\n{}\n", section, line);
    }

    ConfigFile load_config() {
        ConfigFile f{.path = paths::config_dir() / "config.toml"};
        if (!std::filesystem::exists(f.path)) {
            std::ofstream(f.path) << default_config_text();
            log::info("wrote default config to {}", f.path.string());
        }
        std::ifstream in(f.path);
        f.text.assign(std::istreambuf_iterator<char>(in), {});
        if (auto c = parse_config(f.text)) {
            f.config = *c;
        } else {
            f.error = c.error();
        }
        return f;
    }

    Result<void> save_setting(ConfigFile &f, std::string_view section, std::string_view key, std::string_view value) {
        std::ifstream in(f.path);
        std::string text = in ? std::string{std::istreambuf_iterator<char>(in), {}} : std::string(default_config_text());
        text = set_config_value(text, section, key, value);
        auto c = parse_config(text);
        if (!c) {
            return std::unexpected(describe_config_error(c.error()));
        }
        auto tmp = f.path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << text;
            if (!out) {
                return std::unexpected(std::format("Settings were not saved: could not write {}", tmp.string()));
            }
        }
        if (std::rename(tmp.c_str(), f.path.c_str()) != 0) {
            return std::unexpected(std::format("Settings were not saved: could not replace {}", f.path.string()));
        }
        f.config = *c;
        f.error.clear();
        f.text = std::move(text);
        return {};
    }

    ConfigWatcher::ConfigWatcher(Loop &loop, std::filesystem::path path, std::move_only_function<void()> on_change)
    : loop_(loop),
      path_(std::move(path)),
      on_change_(std::move(on_change)),
      fd_(inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) {
        // Editors save by rename, so watch the directory rather than the file.
        inotify_add_watch(fd_, path_.parent_path().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        loop_.watch(fd_, EPOLLIN, [this](uint32_t) {
            alignas(inotify_event) std::array<char, 4096> buf;
            bool hit = false;
            ssize_t n;
            while ((n = read(fd_, buf.data(), buf.size())) > 0) {
                for (char *p = buf.data(); p < buf.data() + n;) {
                    auto *e = reinterpret_cast<inotify_event *>(p);
                    hit |= e->len && path_.filename() == e->name;
                    p += sizeof(inotify_event) + e->len;
                }
            }
            if (!hit) {
                return;
            }
            loop_.cancel(pending_);
            pending_ = loop_.after(150ms, [this] {
                on_change_();
            });
        });
    }

    ConfigWatcher::~ConfigWatcher() {
        loop_.unwatch(fd_);
        loop_.cancel(pending_);
        close(fd_);
    }

} // namespace lyren
