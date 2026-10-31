#include "app/images.hpp"

#include <nexus/pool.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <exception>

#include "core/log.hpp"
#include "media/types.hpp"

namespace lyren {

    static constexpr int max_in_flight = 6;
    static constexpr int max_background = 2;
    static constexpr size_t budget = 24u << 20;
    static constexpr size_t max_entries = 512;
    static constexpr uint32_t blur_px = 48;
    // Set in the key's variant for pictures that keep their aspect; the rest of it is the size tier.
    static constexpr uint32_t whole = 1u << 31;

    struct Images::Loader {
        Loop &loop;
        Images *owner; // loop thread only
        // Image jobs are coarse and sparse. Small queues and sleeping workers keep the idle footprint low.
        huxint::nexus::basic_pool<huxint::nexus::priority, huxint::nexus::worker_cap<2>, huxint::nexus::queue_cap<64, 16>> pool{{.threads = 2, .spin_budget = 0us}};
    };

    Images::Images(Loop &loop, net::Http &http, gfx::Renderer &renderer, std::move_only_function<void()> redraw, std::move_only_function<double()> now)
    : loop_(loop),
      http_(http),
      renderer_(renderer),
      redraw_(std::move(redraw)),
      now_(std::move(now)),
      loader_(std::make_shared<Loader>(loop, this)) {}

    Images::~Images() {
        loader_->owner = nullptr;
        for (auto &[key, e] : entries_) {
            if (e.request) {
                http_.cancel(e.request);
            }
            if (e.ready) {
                renderer_.destroy_texture(e.tex);
            }
        }
        // Network callbacks may still hold Loader; none can call back into Images after this point.
        loader_->pool.shutdown();
    }

    int Images::tier(float d) {
        for (int t : {64, 128, 256, 512}) {
            if (d <= static_cast<float>(t)) {
                return t;
            }
        }
        return 1024;
    }

    Images::Cached &Images::request(CacheKey::View key, Priority priority) {
        auto it = entries_.find(key);
        const bool inserted = it == entries_.end();
        if (inserted) {
            it = entries_.emplace(CacheKey(key), Cached{}).first;
        }
        Cached &e = it->second;
        if (inserted || priority == Priority::foreground) {
            e.used = frame_;
        }
        if (key.text.empty()) {
            e.failed = true;
            return e;
        }
        if (e.failed && now_() >= e.ready_at) {
            e.failed = false;
        }
        if (!e.ready && !e.failed && !e.request && e.queued < priority) {
            if (e.queued == Priority::none) {
                e.px = key.variant ? key.variant & ~whole : blur_px;
                e.bytes = static_cast<size_t>(e.px) * e.px * 4;
                bytes_ += e.bytes;
            }
            e.queued = priority;
            if (priority == Priority::foreground) {
                queue_.emplace_back(key);
            } else {
                preloads_.emplace_back(key);
            }
            if (!pump_scheduled_) {
                pump_scheduled_ = true;
                loop_.defer([loader = loader_] {
                    if (loader->owner) {
                        loader->owner->pump();
                    }
                });
            }
        }
        return e;
    }

    const Images::Entry *Images::other_tier(std::string_view url, uint32_t px, uint32_t flags, bool larger) {
        constexpr std::array sizes{64u, 128u, 256u, 512u, 1024u};
        auto pick = [&](uint32_t size) -> const Entry * {
            if (larger ? size > px : size < px) {
                if (auto it = entries_.find(CacheKey::View{url, size | flags}); it != entries_.end() && it->second.ready) {
                    it->second.used = frame_;
                    return &it->second;
                }
            }
            return nullptr;
        };
        if (larger) {
            for (uint32_t size : sizes) {
                if (const Entry *e = pick(size)) {
                    return e;
                }
            }
        } else {
            for (uint32_t size : sizes | std::views::reverse) {
                if (const Entry *e = pick(size)) {
                    return e;
                }
            }
        }
        return nullptr;
    }

    const Images::Entry &Images::lookup(std::string_view url, uint32_t px, uint32_t flags, bool tiers) {
        const CacheKey::View key{url, px | flags};
        if (auto it = entries_.find(key); it != entries_.end() && it->second.ready) {
            it->second.used = frame_;
            return it->second;
        }
        if (const Entry *larger = tiers ? other_tier(url, px, flags, true) : nullptr) {
            return *larger;
        }
        Cached &e = request(key, Priority::foreground);
        if (const Entry *smaller = tiers ? other_tier(url, px, flags, false) : nullptr) {
            return *smaller;
        }
        return e;
    }

    const Images::Entry &Images::get(std::string_view url, uint32_t px, bool blurred) {
        return blurred ? lookup(url, 0, 0, false) : lookup(url, px, 0, true);
    }

    const Images::Entry &Images::picture(std::string_view url, uint32_t px) {
        return lookup(url, px, whole, true);
    }

    void Images::prefetch(std::string_view url, uint32_t px, bool blurred) {
        const CacheKey::View key{url, blurred ? 0 : px};
        const uint32_t side = blurred ? blur_px : px;
        if (!url.empty() && bytes_ + static_cast<size_t>(side) * side * 4 <= budget && (entries_.size() < max_entries || entries_.contains(key))) {
            request(key, Priority::background);
        }
    }

    void Images::erase(CacheMap<Cached>::iterator it) {
        if (it->second.ready) {
            renderer_.destroy_texture(it->second.tex);
        }
        bytes_ -= it->second.bytes;
        entries_.erase(it);
    }

    void Images::erase(std::string_view key) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.text == key) {
                erase(it++);
            } else {
                ++it;
            }
        }
    }

    void Images::put(std::string_view key, uint32_t px, const gfx::Image &img) {
        const CacheKey::View lookup{key, px};
        if (auto it = entries_.find(lookup); it != entries_.end()) {
            erase(it);
        }
        auto &e = entries_.emplace(CacheKey(lookup), Cached{}).first->second;
        e.ready = true;
        e.ready_at = now_();
        e.px = img.w;
        e.w = img.w, e.h = img.h;
        e.bytes = img.px.size();
        e.used = frame_;
        e.tex = renderer_.create_texture(img.w, img.h, VK_FORMAT_R8G8B8A8_UNORM, false);
        renderer_.upload(e.tex, 0, 0, img.w, img.h, img.px.data(), img.w * 4);
        bytes_ += e.bytes;
    }

    void Images::pump() {
        pump_scheduled_ = false;
        while (in_flight_ < max_in_flight) {
            const bool background = queue_.empty();
            if (background && (preloads_.empty() || background_in_flight_ == max_background)) {
                break;
            }
            CacheKey key = background ? std::move(preloads_.front()) : std::move(queue_.back());
            if (background) {
                preloads_.pop_front();
            } else {
                queue_.pop_back();
            }
            auto it = entries_.find(key);
            if (it == entries_.end() || it->second.queued != (background ? Priority::background : Priority::foreground)) {
                continue;
            }
            Cached &e = it->second;
            e.queued = Priority::none;
            const uint64_t ticket = e.ticket = ++ticket_;
            ++in_flight_;
            background_in_flight_ += background;
            const uint32_t tier = key.variant & ~whole;
            std::string url = key.variant & whole ? picture_url(key.text, static_cast<int>(tier)) : cover_url(key.text, static_cast<int>(key.variant ? tier : 128));
            e.request = http_.request({.url = std::move(url)}, [loader = loader_, key = std::move(key), ticket, background](net::Response r) mutable {
                using namespace huxint::nexus;
                auto submitted = loader->pool.execute(
                    background ? task_priority::low : task_priority::high, [loader, key = std::move(key), ticket, background, r = std::move(r)]() mutable noexcept {
                        gfx::Image img;
                        std::string error = std::move(r.error);
                        const bool transient = !error.empty() || r.status >= 500;
                        float hue = -1, luma = 0;
                        const bool blurred = key.variant == 0;
                        if (error.empty() && r.status == 200) {
                            const std::span<const uint8_t> body{reinterpret_cast<const uint8_t *>(r.body.data()), r.body.size()};
                            img = key.variant & whole ? gfx::decode_fit(body, key.variant & ~whole, error) : gfx::decode_square(body, blurred ? 96 : key.variant, error);
                            if (img.w) {
                                gfx::Image small = gfx::downscale(img, blur_px, blur_px);
                                hue = gfx::dominant_hue(small);
                                luma = gfx::mean_luma(small);
                                if (blurred) {
                                    gfx::blur(small, 5);
                                    img = std::move(small);
                                }
                            }
                        } else if (error.empty()) {
                            error = "HTTP " + std::to_string(r.status);
                        }
                        loader->loop.post([loader, key = std::move(key), ticket, background, img = std::move(img), hue, luma, error = std::move(error), transient] {
                            if (loader->owner) {
                                loader->owner->complete(key, ticket, background, img, hue, luma, error, transient);
                            }
                        });
                    });
                if (!submitted && submitted.error() != submit_error::stopped) {
                    std::terminate();
                }
            });
        }
    }

    void Images::complete(CacheKey::View key, uint64_t ticket, bool background, const gfx::Image &img, float hue, float luma, const std::string &error, bool transient) {
        --in_flight_;
        background_in_flight_ -= background;
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second.ticket == ticket) {
            Cached &e = it->second;
            e.request = 0;
            bytes_ -= e.bytes;
            e.bytes = img.px.size();
            bytes_ += e.bytes;
            if (img.w) {
                e.tex = renderer_.create_texture(img.w, img.h, VK_FORMAT_R8G8B8A8_UNORM, true);
                renderer_.upload(e.tex, 0, 0, img.w, img.h, img.px.data(), img.w * 4);
                e.ready = true;
                e.ready_at = now_();
                e.w = img.w, e.h = img.h;
                e.hue = hue;
                e.luma = luma;
                e.retries = 0;
            } else if (transient && e.retries < 2) {
                // A reset stream or a busy server usually serves the image a moment later; the next frame that wants
                // it asks again.
                e.failed = true;
                e.ready_at = now_() + 0.8 * ++e.retries;
                log::debug("cover {}: {}, retrying", key.text.substr(0, 80), error);
                loop_.after(std::chrono::milliseconds(800 * e.retries), [loader = loader_] {
                    if (loader->owner) {
                        loader->owner->redraw_();
                    }
                });
            } else {
                log::warn("cover {}: {}", key.text.substr(0, 80), error);
                e.failed = true;
                e.retries = 0;
                e.ready_at = now_() + 30;
            }
            redraw_();
        }
        pump();
    }

    void Images::end_frame() {
        ++frame_;
        if (bytes_ <= budget && entries_.size() <= max_entries) {
            return;
        }
        const double now = now_();
        std::vector<decltype(entries_)::iterator> old;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            const Cached &e = it->second;
            if (e.used + 1 < frame_ && (e.ready || e.queued != Priority::none || (e.failed && now >= e.ready_at))) {
                old.push_back(it);
            }
        }
        std::ranges::sort(old, {}, [](auto it) {
            return it->second.used;
        });
        for (auto it : old) {
            if (bytes_ <= budget * 3 / 4 && entries_.size() <= max_entries * 3 / 4) {
                break;
            }
            erase(it);
        }
    }

    void Images::prefetch(std::span<const Card> cards) {
        for (const auto &card : cards.first(std::min(cards.size(), 16uz))) {
            prefetch(card.cover, 128);
        }
    }

    void Images::prefetch(std::span<const Track> tracks) {
        for (const auto &track : tracks.first(std::min(tracks.size(), 32uz))) {
            prefetch(track.cover, 64);
        }
    }

} // namespace lyren
