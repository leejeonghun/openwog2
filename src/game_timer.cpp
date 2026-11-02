#include "game_timer.h"
#include "input.h"
#include <SDL.h>
#include <algorithm>

namespace GameTimer {
uint64_t now_us() {
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1e6 /
                      (double)SDL_GetPerformanceFrequency());
}

uint32_t now_ms() { return (uint32_t)(now_us() / 1000); }

bool delay_with_pump(Input& input, uint32_t ms) {
    if (ms == 0) return !input.poll_quit();
    const uint64_t end = now_us() + (uint64_t)ms * 1000;
    for (;;) {
        if (input.poll_quit()) return false;
        const uint64_t now = now_us();
        if (now >= end) break;
        const uint64_t remain_ms = (end - now) / 1000;
        SDL_Delay((Uint32)std::min<uint64_t>(remain_ms, 5));
    }
    return !input.poll_quit();
}

bool wait_for_press(Input& input, uint32_t max_ms) {
    const uint64_t end = (max_ms == 0) ? 0 : now_us() + (uint64_t)max_ms * 1000;
    for (;;) {
        input.update();
        if (input.quit_requested()) return false;
        if (input.was_left_clicked() || input.any_key_pressed()) return true;
        if (max_ms != 0 && now_us() >= end) return false;
        SDL_Delay(4);
    }
}

FramePacer::FramePacer(double target_fps) {
    if (target_fps > 0.0) frame_us_ = 1e6 / target_fps;
    reset();
}

void FramePacer::reset() { start_us_ = now_us(); }

bool FramePacer::pace(Input& input) {
    const uint64_t now = now_us();
    if (now - start_us_ < (uint64_t)frame_us_) {
        const uint32_t remain_ms = (uint32_t)((start_us_ + (uint64_t)frame_us_ - now) / 1000);
        if (!delay_with_pump(input, remain_ms)) return false;
    }
    start_us_ = now_us();
    return true;
}
}  // namespace GameTimer
