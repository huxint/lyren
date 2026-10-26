#include "app/frames.hpp"

#include <cmath>

namespace lyren {

Frames::Frames(Loop& loop, std::function<bool()> frame_pending, std::function<void()> render)
    : loop_(loop), pending_(std::move(frame_pending)), render_(std::move(render)) {}

Frames::~Frames() {
    loop_.cancel(pace_timer_);
    loop_.cancel(wake_timer_);
}

void Frames::request() {
    dirty_ = true;
    if (!scheduled_ && !pending_()) schedule();
}

void Frames::frame_done() {
    if (dirty_ && !scheduled_) schedule();
}

void Frames::schedule() {
    scheduled_ = true;
    loop_.defer([this] {
        scheduled_ = false;
        render_();
    });
}

bool Frames::begin() {
    if (!dirty_ || pending_()) return false;
    dirty_ = false;
    loop_.cancel(pace_timer_);
    loop_.cancel(wake_timer_);
    pace_timer_ = wake_timer_ = 0;
    return true;
}

void Frames::after(const ui::Ui& ui, Clock::time_point frame_start, Clock::time_point epoch, int max_fps) {
    if (ui.needs_redraw()) {
        request();
    } else if (ui.animating()) {
        auto due = frame_start + std::chrono::nanoseconds(max_fps > 0 ? 1'000'000'000 / max_fps : 0);
        if (Clock::now() >= due) request();
        else
            pace_timer_ = loop_.at(due, [this] {
                pace_timer_ = 0;
                request();
            });
    } else if (std::isfinite(ui.wake_at())) {
        auto at = epoch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(ui.wake_at()));
        wake_timer_ = loop_.at(at, [this] {
            wake_timer_ = 0;
            request();
        });
    }
}

}  // namespace lyren
