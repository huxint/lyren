#include "wl/clipboard.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include "core/loop.hpp"
#include "wl/display.hpp"

namespace lyren::wl {

static Clipboard* self(void* data) { return static_cast<Clipboard*>(data); }
static const char* const text_types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};

static const wl_data_offer_listener offer_listener{
    .offer = [](void* d, wl_data_offer* o, const char* mime) { self(d)->offer_mime(o, mime); },
    .source_actions = [](void*, wl_data_offer*, uint32_t) {},
    .action = [](void*, wl_data_offer*, uint32_t) {},
};

static const wl_data_device_listener device_listener{
    .data_offer = [](void* d, wl_data_device*, wl_data_offer* o) { wl_data_offer_add_listener(o, &offer_listener, d); },
    .enter = [](void* d, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* o) { self(d)->drag_enter(o); },
    .leave = [](void* d, wl_data_device*) { self(d)->drag_leave(); },
    .motion = [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    .drop = [](void* d, wl_data_device*) { self(d)->drag_leave(); },
    .selection = [](void* d, wl_data_device*, wl_data_offer* o) { self(d)->new_selection(o); },
};

static const wl_data_source_listener source_listener{
    .target = [](void*, wl_data_source*, const char*) {},
    .send =
        [](void* d, wl_data_source*, const char*, int32_t fd) {
            self(d)->send(fd);
        },
    .cancelled = [](void* d, wl_data_source*) { self(d)->cancelled(); },
    .dnd_drop_performed = [](void*, wl_data_source*) {},
    .dnd_finished = [](void*, wl_data_source*) {},
    .action = [](void*, wl_data_source*, uint32_t) {},
};

Clipboard::Clipboard(Display& display) : display_(display) {
    if (!display_.data_device_manager) return;
    device_ = wl_data_device_manager_get_data_device(display_.data_device_manager, display_.seat);
    wl_data_device_add_listener(device_, &device_listener, this);
}

Clipboard::~Clipboard() {
    if (source_) wl_data_source_destroy(source_);
    for (auto& [offer, mime] : mimes_) wl_data_offer_destroy(offer);
    if (device_) wl_data_device_release(device_);
}

void Clipboard::offer_mime(wl_data_offer* offer, const char* mime) {
    auto& best = mimes_[offer];
    for (const char* t : text_types) {
        if (best == t) return;
        if (!std::strcmp(mime, t)) {
            best = t;
            return;
        }
    }
}

void Clipboard::new_selection(wl_data_offer* offer) {
    if (selection_ && selection_ != offer) {
        mimes_.erase(selection_);
        wl_data_offer_destroy(selection_);
    }
    selection_ = offer;
}

void Clipboard::drag_enter(wl_data_offer* offer) {
    drag_leave();
    drag_ = offer;
}

void Clipboard::drag_leave() {
    if (!drag_) return;
    mimes_.erase(drag_);
    wl_data_offer_destroy(drag_);
    drag_ = nullptr;
}

void Clipboard::set_text(std::string text, uint32_t serial) {
    if (!device_) return;
    if (source_) wl_data_source_destroy(source_);
    text_ = std::move(text);
    source_ = wl_data_device_manager_create_data_source(display_.data_device_manager);
    wl_data_source_add_listener(source_, &source_listener, this);
    for (const char* t : text_types) wl_data_source_offer(source_, t);
    wl_data_device_set_selection(device_, source_, serial);
}

void Clipboard::send(int fd) {
    for (size_t off = 0; off < text_.size();) {
        ssize_t n = write(fd, text_.data() + off, text_.size() - off);
        if (n <= 0) break;
        off += size_t(n);
    }
    close(fd);
}

void Clipboard::cancelled() {
    wl_data_source_destroy(source_);
    source_ = nullptr;
}

void Clipboard::get_text(std::function<void(std::string)> fn) {
    auto mime = selection_ ? mimes_.find(selection_) : mimes_.end();
    if (mime == mimes_.end() || mime->second.empty()) {
        fn({});
        return;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        fn({});
        return;
    }
    wl_data_offer_receive(selection_, mime->second.c_str(), fds[1]);
    close(fds[1]);
    wl_display_flush(display_.display);
    // The owner may be this very process, so read from the loop instead of blocking.
    auto buf = std::make_shared<std::string>();
    int rfd = fds[0];
    display_.loop.watch(rfd, EPOLLIN, [this, rfd, buf, fn = std::move(fn)](uint32_t) {
        char chunk[4096];
        ssize_t n;
        while ((n = read(rfd, chunk, sizeof chunk)) > 0) buf->append(chunk, size_t(n));
        if (n < 0 && errno == EAGAIN) return;
        display_.loop.unwatch(rfd);
        close(rfd);
        fn(std::move(*buf));
    });
}

}  // namespace lyren::wl
