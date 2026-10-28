#pragma once
#include <functional>
#include <string>
#include <unordered_map>

#include "wl/protocols.hpp"

namespace lyren::wl {

    class Display;

    // The regular clipboard (Ctrl+C / Ctrl+V), plain text only.
    class Clipboard {
    public:
        explicit Clipboard(Display &display);
        ~Clipboard();
        Clipboard(const Clipboard &) = delete;
        Clipboard &operator=(const Clipboard &) = delete;

        void set_text(std::string text, uint32_t serial);
        // Reads the selection without blocking; fn receives "" when it holds no text.
        void get_text(std::move_only_function<void(std::string)> fn);

        // Listener plumbing.
        void offer_mime(wl_data_offer *offer, const char *mime);
        void new_selection(wl_data_offer *offer);
        void drag_enter(wl_data_offer *offer);
        void drag_leave();
        void send(int fd);
        void cancelled();

    private:
        Display &display_;
        wl_data_device *device_ = nullptr;
        wl_data_source *source_ = nullptr;
        std::string text_;
        wl_data_offer *selection_ = nullptr;
        wl_data_offer *drag_ = nullptr;
        std::unordered_map<wl_data_offer *, std::string> mimes_; // best text type each offer advertises
    };

} // namespace lyren::wl
