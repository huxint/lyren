#include "wl/clipboard.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <array>
#include <memory>
#include <string_view>

#include "core/loop.hpp"
#include "wl/display.hpp"

namespace lyren::wl {

    static Clipboard *self(void *data) {
        return static_cast<Clipboard *>(data);
    }
    static constexpr std::array text_types = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};

    static const wl_data_offer_listener offer_listener{
        .offer =
            [](void *d, wl_data_offer *o, const char *mime) {
                self(d)->offer_mime(o, mime);
            },
        .source_actions = [](void *, wl_data_offer *, uint32_t) {},
        .action = [](void *, wl_data_offer *, uint32_t) {},
    };

    static const wl_data_device_listener device_listener{
        .data_offer =
            [](void *d, wl_data_device *, wl_data_offer *o) {
                wl_data_offer_add_listener(o, &offer_listener, d);
            },
        .enter =
            [](void *d, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t, wl_data_offer *o) {
                self(d)->drag_enter(o);
            },
        .leave =
            [](void *d, wl_data_device *) {
                self(d)->drag_leave();
            },
        .motion = [](void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {},
        .drop =
            [](void *d, wl_data_device *) {
                self(d)->drag_leave();
            },
        .selection =
            [](void *d, wl_data_device *, wl_data_offer *o) {
                self(d)->new_selection(o);
            },
    };

    static const wl_data_source_listener source_listener{
        .target = [](void *, wl_data_source *, const char *) {},
        .send =
            [](void *d, wl_data_source *, const char *, int32_t fd) {
                self(d)->send(fd);
            },
        .cancelled =
            [](void *d, wl_data_source *) {
                self(d)->cancelled();
            },
        .dnd_drop_performed = [](void *, wl_data_source *) {},
        .dnd_finished = [](void *, wl_data_source *) {},
        .action = [](void *, wl_data_source *, uint32_t) {},
    };

    Clipboard::Clipboard(Display &display)
    : display_(display) {
        signal(SIGPIPE, SIG_IGN);
        if (!display_.data_device_manager) {
            return;
        }
        device_ = wl_data_device_manager_get_data_device(display_.data_device_manager, display_.seat);
        wl_data_device_add_listener(device_, &device_listener, this);
    }

    Clipboard::~Clipboard() {
        for (auto &[fd, _] : sending_) {
            display_.loop.unwatch(fd);
            close(fd);
        }
        sending_.clear();
        for (auto &[fd, _] : reading_) {
            display_.loop.unwatch(fd);
            close(fd);
        }
        reading_.clear();
        if (source_) {
            wl_data_source_destroy(source_);
        }
        for (auto &[offer, mime] : mimes_) {
            wl_data_offer_destroy(offer);
        }
        if (device_) {
            wl_data_device_release(device_);
        }
    }

    void Clipboard::offer_mime(wl_data_offer *offer, const char *mime) {
        auto &best = mimes_[offer];
        for (const char *t : text_types) {
            if (best == t) {
                return;
            }
            if (std::string_view(mime) == t) {
                best = t;
                return;
            }
        }
    }

    void Clipboard::new_selection(wl_data_offer *offer) {
        if (selection_ && selection_ != offer) {
            mimes_.erase(selection_);
            wl_data_offer_destroy(selection_);
        }
        selection_ = offer;
    }

    void Clipboard::drag_enter(wl_data_offer *offer) {
        drag_leave();
        drag_ = offer;
    }

    void Clipboard::drag_leave() {
        if (!drag_) {
            return;
        }
        mimes_.erase(drag_);
        wl_data_offer_destroy(drag_);
        drag_ = nullptr;
    }

    void Clipboard::set_text(std::string text, uint32_t serial) {
        if (!device_) {
            return;
        }
        if (source_) {
            wl_data_source_destroy(source_);
        }
        text_ = std::move(text);
        source_ = wl_data_device_manager_create_data_source(display_.data_device_manager);
        wl_data_source_add_listener(source_, &source_listener, this);
        for (const char *t : text_types) {
            wl_data_source_offer(source_, t);
        }
        wl_data_device_set_selection(device_, source_, serial);
    }

    void Clipboard::set_data(std::string data, std::string mime, uint32_t serial) {
        if (!device_ || mime.empty()) {
            return;
        }
        if (source_) {
            wl_data_source_destroy(source_);
        }
        text_ = std::move(data);
        source_ = wl_data_device_manager_create_data_source(display_.data_device_manager);
        wl_data_source_add_listener(source_, &source_listener, this);
        wl_data_source_offer(source_, mime.c_str());
        wl_data_device_set_selection(device_, source_, serial);
    }

    void Clipboard::send(int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        size_t off = 0;
        while (off < text_.size()) {
            const ssize_t n = write(fd, text_.data() + off, text_.size() - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                sending_[fd] = {std::make_shared<const std::string>(text_), off};
                display_.loop.watch(fd, EPOLLOUT, [this, fd](uint32_t events) {
                    auto it = sending_.find(fd);
                    if (it == sending_.end()) {
                        return;
                    }
                    auto &out = it->second;
                    bool keep_waiting = false;
                    if (!(events & (EPOLLERR | EPOLLHUP))) {
                        while (out.offset < out.bytes->size()) {
                            const ssize_t k = write(fd, out.bytes->data() + out.offset, out.bytes->size() - out.offset);
                            if (k > 0) {
                                out.offset += static_cast<size_t>(k);
                                continue;
                            }
                            if (k < 0 && errno == EINTR) {
                                continue;
                            }
                            keep_waiting = k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
                            break;
                        }
                    }
                    if (keep_waiting && out.offset < out.bytes->size()) {
                        return;
                    }
                    sending_.erase(it);
                    display_.loop.unwatch(fd);
                    close(fd);
                });
                return;
            }
            break;
        }
        close(fd);
    }

    void Clipboard::cancelled() {
        wl_data_source_destroy(source_);
        source_ = nullptr;
    }

    void Clipboard::get_text(std::move_only_function<void(std::string)> fn) {
        auto mime = selection_ ? mimes_.find(selection_) : mimes_.end();
        if (mime == mimes_.end() || mime->second.empty()) {
            fn({});
            return;
        }
        std::array<int, 2> fds;
        if (pipe2(fds.data(), O_CLOEXEC | O_NONBLOCK) < 0) {
            fn({});
            return;
        }
        wl_data_offer_receive(selection_, mime->second.c_str(), fds[1]);
        close(fds[1]);
        wl_display_flush(display_.display);
        // The owner may be this very process, so read from the loop instead of blocking.
        int rfd = fds[0];
        reading_[rfd] = {{}, std::move(fn)};
        display_.loop.watch(rfd, EPOLLIN, [this, rfd](uint32_t) {
            static constexpr size_t max_paste_bytes = 4u << 20;
            auto it = reading_.find(rfd);
            if (it == reading_.end()) {
                return;
            }
            std::array<char, 4096> chunk;
            ssize_t n = 0;
            bool overflow = false;
            for (;;) {
                n = read(rfd, chunk.data(), chunk.size());
                if (n > 0) {
                    const size_t room = max_paste_bytes - std::min(it->second.text.size(), max_paste_bytes);
                    it->second.text.append(chunk.data(), std::min(static_cast<size_t>(n), room));
                    if (static_cast<size_t>(n) > room) {
                        overflow = true;
                        break;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            if (!overflow && n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            auto incoming = std::move(it->second);
            reading_.erase(it);
            display_.loop.unwatch(rfd);
            close(rfd);
            incoming.done(std::move(incoming.text));
        });
    }

} // namespace lyren::wl
