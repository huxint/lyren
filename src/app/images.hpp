#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/loop.hpp"
#include "gfx/renderer.hpp"
#include "gfx/image.hpp"
#include "net/http.hpp"

namespace lyren {

    // Cover art as textures. Images are fetched at the requested size, decoded on the network thread and uploaded on the
    // loop thread; textures that were not drawn recently are evicted once the total passes the budget.
    class Images {
    public:
        struct Entry {
            gfx::TextureId tex = 0;
            bool ready = false, failed = false;
            double ready_at = 0; // for the fade-in; for failures, when to try again
            float hue = -1;      // dominant hue, negative for colourless images
            uint32_t px = 0;
            size_t bytes = 0;
            uint64_t used = 0; // last frame it was asked for
        };

        // redraw is called when an image arrives; now gives the UI clock.
        Images(Loop &loop, net::Http &http, gfx::Renderer &renderer, std::move_only_function<void()> redraw, std::move_only_function<double()> now);
        ~Images();
        Images(const Images &) = delete;
        Images &operator=(const Images &) = delete;

        // Size tier for a cover drawn d buffer pixels wide.
        static int tier(float d);
        // The image at url, which the caller already sized to px; starts loading it on first use. blurred gives a small,
        // heavily blurred version for backgrounds.
        const Entry &get(const std::string &url, uint32_t px, bool blurred = false);
        void put(const std::string &key, const gfx::Image &image); // generated images, such as the login QR code
        void erase(const std::string &key);
        void end_frame();

    private:
        void pump();

        Loop &loop_;
        net::Http &http_;
        gfx::Renderer &renderer_;
        std::move_only_function<void()> redraw_;
        std::move_only_function<double()> now_;
        std::unordered_map<std::string, Entry> entries_;
        std::vector<std::string> queue_; // newest last, which is fetched first
        int in_flight_ = 0;
        uint64_t frame_ = 1;
        size_t bytes_ = 0;
    };

} // namespace lyren
