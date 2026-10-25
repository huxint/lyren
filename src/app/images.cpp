#include "app/images.hpp"

#include <algorithm>
#include <memory>

#include "core/log.hpp"
#include "gfx/image.hpp"

namespace lyren {

static constexpr int max_in_flight = 6;
static constexpr size_t budget = 24u << 20;
static constexpr uint32_t blur_px = 48;

Images::Images(Loop& loop, net::Http& http, gfx::Renderer& renderer, std::function<void()> redraw, std::function<double()> now)
    : loop_(loop), http_(http), renderer_(renderer), redraw_(std::move(redraw)), now_(std::move(now)) {}

Images::~Images() {
    for (auto& [key, e] : entries_)
        if (e.ready) renderer_.destroy_texture(e.tex);
}

int Images::tier(float d) {
    for (int t : {64, 128, 256, 512})
        if (d <= float(t)) return t;
    return 1024;
}

const Images::Entry& Images::get(const std::string& url, uint32_t px, bool blurred) {
    std::string key = blurred ? url + "#blur" : url;
    auto [it, inserted] = entries_.try_emplace(key);
    Entry& e = it->second;
    e.used = frame_;
    if (e.failed && !url.empty() && now_() > e.ready_at) e.failed = false, inserted = true;
    if (inserted && !url.empty()) {
        e.px = blurred ? blur_px : px;
        e.queued = true;
        queue_.push_back(key);
        loop_.defer([this] { pump(); });
    }
    if (url.empty()) e.failed = true;
    return e;
}

void Images::pump() {
    while (in_flight_ < max_in_flight && !queue_.empty()) {
        std::string key = std::move(queue_.back());
        queue_.pop_back();
        auto it = entries_.find(key);
        if (it == entries_.end()) continue;
        Entry& e = it->second;
        e.queued = false;
        // Scrolled away before its turn: forget it, it is asked for again if it comes back.
        if (e.used + 2 < frame_) {
            entries_.erase(it);
            continue;
        }
        ++in_flight_;
        bool blurred = key.ends_with("#blur");
        std::string url = blurred ? key.substr(0, key.size() - 5) : key;
        uint32_t px = e.px;
        http_.request({.url = url}, [this, key, px, blurred](net::Response r) {
            // Network thread: decode here so the loop thread only uploads.
            auto img = std::make_shared<gfx::Image>();
            std::string error = r.error;
            float hue = -1;
            if (error.empty() && r.status == 200) {
                *img = gfx::decode_square({reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size()}, blurred ? 96 : px, error);
                if (img->w) {
                    gfx::Image small = gfx::downscale(*img, blur_px, blur_px);
                    hue = gfx::dominant_hue(small);
                    if (blurred) {
                        gfx::blur(small, 5);
                        *img = std::move(small);
                    }
                }
            } else if (error.empty()) {
                error = std::format("HTTP {}", r.status);
            }
            loop_.post([this, key, img, hue, error] {
                --in_flight_;
                auto it = entries_.find(key);
                if (it != entries_.end() && it->second.used + 2 < frame_) {
                    entries_.erase(it);
                    it = entries_.end();
                }
                if (it != entries_.end()) {
                    Entry& e = it->second;
                    if (img->w) {
                        e.tex = renderer_.create_texture(img->w, img->h, VK_FORMAT_R8G8B8A8_UNORM, true);
                        renderer_.upload(e.tex, 0, 0, img->w, img->h, img->px.data(), img->w * 4);
                        e.ready = true;
                        e.ready_at = now_();
                        e.hue = hue;
                        e.bytes = size_t(img->w) * img->h * 4;
                        bytes_ += e.bytes;
                    } else {
                        log::warn("cover {}: {}", key.substr(0, 80), error);
                        e.failed = true;
                        e.ready_at = now_() + 30;
                    }
                    redraw_();
                }
                pump();
            });
        });
    }
}

void Images::end_frame() {
    ++frame_;
    if (bytes_ <= budget) return;
    std::vector<std::pair<uint64_t, std::string>> old;
    for (auto& [key, e] : entries_)
        if (e.ready && e.used + 1 < frame_) old.emplace_back(e.used, key);
    std::ranges::sort(old);
    for (auto& [used, key] : old) {
        if (bytes_ <= budget * 3 / 4) break;
        Entry& e = entries_[key];
        renderer_.destroy_texture(e.tex);
        bytes_ -= e.bytes;
        entries_.erase(key);
    }
}

}  // namespace lyren
