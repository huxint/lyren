#include "app/app.hpp"

#include <linux/input-event-codes.h>
#include <png.h>
#include <spawn.h>
#include <sys/wait.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>

#include "core/log.hpp"

namespace lyren {

    static void write_png(const std::string &path, const uint8_t *bgra, uint32_t w, uint32_t h) {
        std::vector<uint8_t> px(bgra, bgra + static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < px.size(); i += 4) {
            if (int a = px[i + 3]; a && a < 255) {
                for (int c = 0; c < 3; ++c) {
                    px[i + c] = static_cast<uint8_t>(std::min(255, px[i + c] * 255 / a));
                }
            }
        }
        png_image img{};
        img.version = PNG_IMAGE_VERSION, img.width = w, img.height = h, img.format = PNG_FORMAT_BGRA;
        if (!png_image_write_to_file(&img, path.c_str(), 0, px.data(), 0, nullptr)) {
            log::warn("cannot write {}: {}", path, img.message);
        }
    }

    void App::run_script(size_t i) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }
        float m = static_cast<float>(window_->margin());
        auto ms = [this] {
            return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count());
        };
        while (i < script_.size()) {
            std::istringstream in(script_[i++]);
            std::string cmd, rest;
            in >> cmd;
            // "lyrics CMD …" sends a pointer command or a shot to the desktop lyrics instead of the window.
            bool to_lyrics = cmd == "lyrics";
            if (to_lyrics) {
                in >> cmd;
            }
            std::getline(in >> std::ws, rest);
            std::istringstream args(rest);
            if (cmd.empty() || cmd[0] == '#') {
                continue;
            }
            if (to_lyrics && !lyrics_) {
                log::warn("script line {}: desktop lyrics are off", i);
                continue;
            }
            auto pointer = [&](wl::PointerEvent e, uint32_t mods = 0) {
                auto &frame = (to_lyrics ? overlay_ : main_).input;
                const size_t before = frame.actions.size();
                if (to_lyrics) {
                    on_lyrics_pointer(e);
                } else {
                    e.x += m, e.y += m;
                    on_pointer(e);
                }
                if (frame.actions.size() != before) {
                    frame.actions.back().mods = mods;
                }
            };
            if (cmd == "wait") {
                int n = 0;
                args >> n;
                loop_.after(std::chrono::milliseconds(n), [this, i] {
                    run_script(i);
                });
                return;
            }
            float x = 0, y = 0;
            std::string how;
            if (cmd == "move" || cmd == "click" || cmd == "press" || cmd == "release") {
                args >> x >> y >> how;
            }
            const uint32_t pointer_mods = (how.contains("ctrl") ? wl::mod_ctrl : 0) | (how.contains("shift") ? wl::mod_shift : 0);
            if (cmd == "move") {
                pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
            } else if (cmd == "click") {
                uint32_t button = how == "right" ? BTN_RIGHT : how == "back" ? BTN_SIDE : BTN_LEFT;
                pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
                for (int k = how == "double" ? 2 : 1; k > 0; --k) {
                    pointer({.type = wl::PointerEvent::press, .x = x, .y = y, .button = button, .time = ms()}, pointer_mods);
                    pointer({.type = wl::PointerEvent::release, .x = x, .y = y, .button = button, .time = ms()}, pointer_mods);
                }
            } else if (cmd == "press" || cmd == "release") {
                pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
                pointer({.type = cmd == "press" ? wl::PointerEvent::press : wl::PointerEvent::release, .x = x, .y = y, .button = BTN_LEFT, .time = ms()}, pointer_mods);
            } else if (cmd == "scroll") {
                float dy = 0;
                args >> dy >> how;
                on_pointer({.type = wl::PointerEvent::scroll, .x = main_.input.x + m, .y = main_.input.y + m, .dy = dy, .wheel = how == "wheel"});
            } else if (cmd == "key") {
                // e.g. "key ctrl+shift+Left"
                uint32_t mods = 0;
                for (size_t plus; (plus = rest.find('+')) != std::string::npos && plus > 0; rest.erase(0, plus + 1)) {
                    std::string mod = rest.substr(0, plus);
                    mods |= mod == "ctrl" ? wl::mod_ctrl : mod == "shift" ? wl::mod_shift : mod == "alt" ? wl::mod_alt : 0;
                }
                wl::KeyEvent e{.keysym = xkb_keysym_from_name(rest.c_str(), XKB_KEYSYM_NO_FLAGS), .mods = mods, .pressed = true};
                std::array<char, 8> buf;
                if (!(mods & wl::mod_ctrl) && xkb_keysym_to_utf8(e.keysym, buf.data(), buf.size()) > 1 && static_cast<unsigned char>(buf[0]) >= 0x20) {
                    e.text = buf.data();
                }
                on_key(e);
            } else if (cmd == "commit" || cmd == "preedit") {
                wl::TextInputEvent e;
                (cmd == "commit" ? e.commit : e.preedit) = rest;
                e.preedit_cursor = cmd == "preedit" ? static_cast<int>(rest.size()) : -1;
                on_text_input(e);
            } else if (cmd == "shot") {
                Surface &surf = to_lyrics ? overlay_ : main_;
                renderer_.capture_next(*surf.target, [this, path = rest, next = i](const uint8_t *px, uint32_t w, uint32_t h) {
                    write_png(path, px, w, h);
                    loop_.defer([this, next] {
                        run_script(next);
                    });
                });
                surf.hash = 0;
                request_redraw();
                return;
            } else if (cmd == "resize") {
                int w = 0, h = 0;
                args >> w >> h;
                window_->set_size(std::max(w, 720), std::max(h, 480));
                request_redraw();
            } else if (cmd == "exec") {
                pid_t pid;
                std::array<const char *, 4> argv = {"sh", "-c", rest.c_str(), nullptr};
                posix_spawnp(&pid, "sh", nullptr, nullptr, const_cast<char *const *>(argv.data()), environ);
            } else if (cmd == "quit") {
                loop_.quit();
            } else {
                log::warn("script line {}: unknown command \"{}\"", i, cmd);
            }
        }
    }

} // namespace lyren
