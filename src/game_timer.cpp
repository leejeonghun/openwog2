#include "game_timer.h"
#include "input.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>

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

static bool wait_retrace_impl(Input* input, int retraces) {
    if (retraces <= 0) return input ? !input->poll_quit() : true;
    static thread_local double next_retrace_us = 0.0;
    const double now = (double)now_us();
    const double span_us = (double)retraces * kVgaPeriodUs;
    double deadline;
    if (next_retrace_us <= 0.0) {
        deadline = now + span_us;
    } else if (now - next_retrace_us > 4.0 * kVgaPeriodUs) {
        // Long idle gap (> 4 periods): advance the virtual CRT grid to the latest edge <= now so
        // the next r retraces still span ((r-1)P, rP] like free-running 70.086Hz hardware.
        next_retrace_us += std::floor((now - next_retrace_us) / kVgaPeriodUs) * kVgaPeriodUs;
        deadline = next_retrace_us + span_us;
    } else {
        deadline = next_retrace_us + span_us;
        // A wait must never fall below (retraces-1) periods measured from now. Without this,
        // a long wait issued shortly after a previous one would end early: the deadline was
        // chained to the previous edge, so up to one period of the span was already spent.
        // Raise it to the next point of the existing retrace grid to keep the phase lock.
        if (retraces > 1) {
            const double min_deadline = now + (double)(retraces - 1) * kVgaPeriodUs;
            if (deadline < min_deadline) {
                deadline = next_retrace_us +
                           std::ceil((min_deadline - next_retrace_us) / kVgaPeriodUs) * kVgaPeriodUs;
            }
        }
        if (deadline <= now) {
            // Elapsed work since the previous retrace exceeded span_us (e.g., synchronous vsync
            // present). Return immediately without double-waiting, keeping phase lock for brief
            // jitter while clamping larger lag so subsequent waits neither burst nor stall.
            next_retrace_us = (now - deadline < 0.5 * kVgaPeriodUs) ? deadline : now;
            return input ? !input->poll_quit() : true;
        }
    }
    double last_poll_us = now;
    if (input && input->poll_quit()) {
        next_retrace_us = 0.0;
        return false;
    }
    for (;;) {
        const double cur = (double)now_us();
        if (input && cur - last_poll_us >= 1000.0) {
            last_poll_us = cur;
            if (input->poll_quit()) {
                next_retrace_us = 0.0;
                return false;
            }
        }
        const double remain_us = deadline - cur;
        if (remain_us <= 0.0) break;
        if (remain_us > 1500.0) {
            const uint64_t max_chunk = input ? 5ULL : 20ULL;
            const uint64_t chunk_ms = std::min<uint64_t>((uint64_t)((remain_us - 500.0) / 1000.0), max_chunk);
            SDL_Delay((Uint32)chunk_ms);
            if (input) {
                last_poll_us = (double)now_us();
                if (input->poll_quit()) {
                    next_retrace_us = 0.0;
                    return false;
                }
            }
        } else {
            SDL_Delay(0);
        }
    }
    next_retrace_us = deadline;
    return input ? !input->poll_quit() : true;
}

bool wait_retrace(Input& input, int retraces) {
    return wait_retrace_impl(&input, retraces);
}

void wait_retrace(int retraces) {
    (void)wait_retrace_impl(nullptr, retraces);
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

FramePacer FramePacer::from_period_us(double period_us) {
    FramePacer p(0.0);
    if (period_us > 0.0) p.frame_us_ = period_us;
    p.reset();
    return p;
}

void FramePacer::reset() { next_us_ = (double)now_us() + frame_us_; }

bool FramePacer::pace(Input& input) {
    if (input.poll_quit()) return false;
    double last_poll_us = (double)now_us();
    for (;;) {
        const double now = (double)now_us();
        if (now - last_poll_us >= 1000.0) {
            last_poll_us = now;
            if (input.poll_quit()) return false;
        }
        const double remain_us = next_us_ - now;
        if (remain_us <= 0.0) break;
        if (remain_us > 1500.0) {
            const uint64_t chunk_ms = std::min<uint64_t>((uint64_t)((remain_us - 500.0) / 1000.0), 5ULL);
            SDL_Delay((Uint32)chunk_ms);
            last_poll_us = (double)now_us();
            if (input.poll_quit()) return false;
        } else {
            SDL_Delay(0);
        }
    }
    next_us_ += frame_us_;
    const double now = (double)now_us();
    if (now - next_us_ > 4.0 * frame_us_) next_us_ = now + frame_us_;
    return true;
}
}  // namespace GameTimer
