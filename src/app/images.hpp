#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/loop.hpp"
#include "core/string_map.hpp"
#include "gfx/renderer.hpp"
#include "gfx/image.hpp"
#include "net/http.hpp"

namespace lyren {

    struct Track;
    struct Card;

    // Cover art as textures. Downloads feed a small decode pool; uploads and cache access stay on the loop thread.
    class Images {
    public:
        struct Entry {
            gfx::TextureId tex = 0;
            bool ready = false, failed = false;
            double ready_at = 0; // for the fade-in; for failures, when to try again
            float hue = -1;      // dominant hue, negative for colourless images
            float luma = 0;      // mean luminance (0..1)
            uint32_t px = 0;
            uint32_t w = 0, h = 0; // texture size; a picture keeps its aspect
            size_t bytes = 0;      // pixel bytes, reserved until decoding completes
            uint64_t used = 0;     // last frame it was asked for
        };

        // redraw is called when an image arrives; now gives the UI clock.
        Images(Loop &loop, net::Http &http, gfx::Renderer &renderer, std::move_only_function<void()> redraw, std::move_only_function<double()> now);
        ~Images();
        Images(const Images &) = delete;
        Images &operator=(const Images &) = delete;

        // Size tier for a cover drawn d buffer pixels wide.
        static int tier(float d);
        // url is the original cover URL; resizing it for the service happens only when a download starts.
        // A cached size can stand in while a sharper image loads. blurred uses a separate, small background image.
        const Entry &get(std::string_view url, uint32_t px, bool blurred = false);
        // A whole picture, not cropped, about px on its longer side (a tier, like covers).
        const Entry &picture(std::string_view url, uint32_t px);
        // Best effort: queued pixels and entries must fit the cache budget.
        void prefetch(std::string_view url, uint32_t px, bool blurred = false);
        void prefetch(std::span<const Track> tracks);
        void prefetch(std::span<const Card> cards);
        void put(std::string_view key, uint32_t px, const gfx::Image &image); // generated images, such as the login QR code
        void erase(std::string_view key);
        void end_frame();

    private:
        enum class Priority : uint8_t {
            none,
            background,
            foreground
        };
        struct Cached : Entry {
            Priority queued = Priority::none;
            net::Http::Id request = 0;
            uint64_t ticket = 0;
            uint8_t retries = 0; // quick retries spent on dropped connections and server errors
        };
        struct Loader;
        Cached &request(CacheKey::View key, Priority priority);
        // The entry for the key, a sharper or else a softer tier standing in while it loads (with tiers).
        const Entry &lookup(std::string_view url, uint32_t px, uint32_t flags, bool tiers);
        // A sharper or a softer tier of the same image that is ready, or nullptr. flags mark pictures in the key.
        const Entry *other_tier(std::string_view url, uint32_t px, uint32_t flags, bool larger);
        void erase(CacheMap<Cached>::iterator it);
        void pump();
        // transient: the download failed in the network or on the server, not in the image itself.
        void complete(CacheKey::View key, uint64_t ticket, bool background, const gfx::Image &image, float hue, float luma, const std::string &error, bool transient);

        Loop &loop_;
        net::Http &http_;
        gfx::Renderer &renderer_;
        std::move_only_function<void()> redraw_;
        std::move_only_function<double()> now_;
        CacheMap<Cached> entries_;
        std::vector<CacheKey> queue_; // newest visible image first
        std::deque<CacheKey> preloads_;
        std::shared_ptr<Loader> loader_;
        int in_flight_ = 0, background_in_flight_ = 0;
        bool pump_scheduled_ = false;
        uint64_t ticket_ = 0;
        uint64_t frame_ = 1;
        size_t bytes_ = 0;
    };

} // namespace lyren
