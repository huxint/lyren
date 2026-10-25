#include "sys/config.hpp"

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

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

namespace lyren {

static const unsigned char default_config[] = {
#embed "default-config.toml"
};

std::string_view default_config_text() { return {reinterpret_cast<const char*>(default_config), sizeof default_config}; }

std::expected<Config, std::string> parse_config(std::string_view toml) {
    Config c;
    std::string buf(toml);
    if (auto ec = glz::read<glz::opts{.format = glz::TOML}>(c, buf)) return std::unexpected(glz::format_error(ec, buf));
    return c;
}

ConfigFile load_config() {
    ConfigFile f{.path = paths::config_dir() / "config.toml"};
    if (!std::filesystem::exists(f.path)) {
        std::ofstream(f.path) << default_config_text();
        log::info("wrote default config to {}", f.path.string());
    }
    std::ifstream in(f.path);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (auto c = parse_config(text)) f.config = *c;
    else f.error = c.error();
    return f;
}

ConfigWatcher::ConfigWatcher(Loop& loop, std::filesystem::path path, std::function<void()> on_change)
    : loop_(loop), path_(std::move(path)), on_change_(std::move(on_change)), fd_(inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) {
    // Editors save by rename, so watch the directory rather than the file.
    inotify_add_watch(fd_, path_.parent_path().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    loop_.watch(fd_, EPOLLIN, [this](uint32_t) {
        alignas(inotify_event) char buf[4096];
        bool hit = false;
        ssize_t n;
        while ((n = read(fd_, buf, sizeof buf)) > 0)
            for (char* p = buf; p < buf + n;) {
                auto* e = reinterpret_cast<inotify_event*>(p);
                hit |= e->len && path_.filename() == e->name;
                p += sizeof(inotify_event) + e->len;
            }
        if (!hit) return;
        loop_.cancel(pending_);
        pending_ = loop_.after(150ms, [this] { on_change_(); });
    });
}

ConfigWatcher::~ConfigWatcher() {
    loop_.unwatch(fd_);
    loop_.cancel(pending_);
    close(fd_);
}

}  // namespace lyren
