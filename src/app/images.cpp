#include "app/images.hpp"

#include <algorithm>

#include "core/log.hpp"
#include "gfx/image.hpp"

namespace lyren {

    static constexpr int max_in_flight = 6;
    static constexpr size_t budget = 24u << 20;
    static constexpr size_t max_entries = 512;
    static constexpr uint32_t blur_px = 48;

    Images::Images(Loop &loop, net::Http &http, gfx::Renderer &renderer, std::move_only_function<void()> redraw, std::move_only_function<double()> now)
    : loop_(loop),
      http_(http),
      renderer_(renderer),
      redraw_(std::move(redraw)),
      now_(std::move(now)) {}

    Images::~Images() {
        for (auto &[key, e] : entries_) {
            if (e.ready) {
                renderer_.destroy_texture(e.tex);
            }
        }
    }

    int Images::tier(float d) {
        for (int t : {64, 128, 256, 512}) {
            if (d <= static_cast<float>(t)) {
                return t;
            }
        }
        return 1024;
    }

    const Images::Entry &Images::get(const std::string &url, uint32_t px, bool blurred) {
        std::string key = blurred ? url + "#blur" : url;
        auto [it, inserted] = entries_.try_emplace(key);
        Entry &e = it->second;
        e.used = frame_;
        if (e.failed && !url.empty() && now_() > e.ready_at) {
            e.failed = false, inserted = true;
        }
        if (inserted && !url.empty()) {
            e.px = blurred ? blur_px : px;
            if (queue_.empty()) {
                loop_.defer([this] {
                    pump();
                });
            }
            queue_.push_back(key);
        }
        if (url.empty()) {
            e.failed = true;
        }
        return e;
    }

    void Images::erase(const std::string &key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return;
        }
        if (it->second.ready) {
            renderer_.destroy_texture(it->second.tex);
        }
        bytes_ -= it->second.bytes;
        entries_.erase(it);
    }

    void Images::put(const std::string &key, const gfx::Image &img) {
        erase(key);
        Entry e{.ready = true, .ready_at = now_(), .px = img.w, .bytes = img.px.size(), .used = frame_};
        e.tex = renderer_.create_texture(img.w, img.h, VK_FORMAT_R8G8B8A8_UNORM, false);
        renderer_.upload(e.tex, 0, 0, img.w, img.h, img.px.data(), img.w * 4);
        bytes_ += e.bytes;
        entries_[key] = e;
    }

    void Images::pump() {
        while (in_flight_ < max_in_flight && !queue_.empty()) {
            std::string key = std::move(queue_.back());
            queue_.pop_back();
            auto it = entries_.find(key);
            if (it == entries_.end()) {
                continue;
            }
            Entry &e = it->second;
            // Scrolled away before its turn: forget it, it is asked for again if it comes back.
            if (e.used + 2 < frame_) {
                entries_.erase(it);
                continue;
            }
            ++in_flight_;
            bool blurred = key.ends_with("#blur");
            std::string url = blurred ? key.substr(0, key.size() - 5) : key;
            uint32_t px = e.px;
            http_.request({.url = url}, [this, key = std::move(key), px, blurred](net::Response r) mutable {
                // Network thread: decode here so the loop thread only uploads.
                gfx::Image img;
                std::string error = std::move(r.error);
                float hue = -1, luma = 0;
                if (error.empty() && r.status == 200) {
                    img = gfx::decode_square({reinterpret_cast<const uint8_t *>(r.body.data()), r.body.size()}, blurred ? 96 : px, error);
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
                loop_.post([this, key = std::move(key), img = std::move(img), hue, luma, error = std::move(error)] {
                    --in_flight_;
                    auto it = entries_.find(key);
                    if (it != entries_.end() && it->second.used + 2 < frame_) {
                        entries_.erase(it);
                        it = entries_.end();
                    }
                    if (it != entries_.end()) {
                        Entry &e = it->second;
                        if (img.w) {
                            e.tex = renderer_.create_texture(img.w, img.h, VK_FORMAT_R8G8B8A8_UNORM, true);
                            renderer_.upload(e.tex, 0, 0, img.w, img.h, img.px.data(), img.w * 4);
                            e.ready = true;
                            e.ready_at = now_();
                            e.hue = hue;
                            e.luma = luma;
                            e.bytes = img.px.size();
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
        if (bytes_ <= budget && entries_.size() <= max_entries) {
            return;
        }
        double now = now_();
        std::vector<decltype(entries_)::iterator> old;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            const Entry &e = it->second;
            if (e.used + 1 < frame_ && (e.ready || (e.failed && now >= e.ready_at))) {
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
            erase(it->first);
        }
    }

} // namespace lyren
