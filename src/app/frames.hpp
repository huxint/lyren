#pragma once
#include <functional>

#include "core/loop.hpp"
#include "ui/ui.hpp"

namespace lyren {

// When one surface draws. A change draws at the next chance, but never before the compositor's frame callback;
// animation frames are capped at render.max_fps; a frame can also be due at a set time.
class Frames {
public:
    Frames(Loop& loop, std::function<bool()> frame_pending, std::function<void()> render);
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    void request();
    void frame_done();
    // At the start of render: false when there is nothing to draw or the last frame is still on its way.
    bool begin();
    // At the end of render: schedules the next frame from what the UI asked for.
    void after(const ui::Ui& ui, Clock::time_point frame_start, Clock::time_point epoch, int max_fps);

private:
    void schedule();

    Loop& loop_;
    std::function<bool()> pending_;
    std::function<void()> render_;
    bool dirty_ = true, scheduled_ = false;
    Loop::TimerId pace_timer_ = 0, wake_timer_ = 0;
};

}  // namespace lyren
