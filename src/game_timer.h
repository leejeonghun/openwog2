#pragma once
#include <cstdint>

class Input;

// CombatII keeps its own DOS PIT emulation because it drives the ATB beat (game tick
// 175.47Hz = PIT 701.87Hz / 4). This module only paces the other screens (TTL, Pack, CHP,
// FLI) and keeps them responsive to a quit request while waiting. Palette fades are
// excluded because they are called without an Input.
namespace GameTimer {
// High-resolution time from SDL_GetPerformanceCounter.
uint64_t now_us();
uint32_t now_ms();

// DOS PIT 8253 channel 0 (1193182/1700 = ~701.87Hz) ticks -> ms. The combat game tick is
// the sound ISR divided by 4 (175.47Hz, kTimerHz in combatii.h), not this value; this
// function is for non-combat pacing only.
constexpr double kDosPitHz = 1193182.0 / 1700.0;
inline uint32_t pit_ticks_to_ms(int ticks) {
    return (ticks <= 0) ? 0 : static_cast<uint32_t>((ticks * 1000.0) / kDosPitHz);
}

// VGA Mode 13h hardware vertical retrace: 25.175MHz / (800 * 449) = ~70.086Hz (14.268ms).
constexpr double kVgaHz = 25175000.0 / (800.0 * 449.0);
constexpr double kVgaPeriodUs = 1000000.0 / kVgaHz;
// Autodesk FLI header.speed unit is 1/70 second.
constexpr double kFliTickHz = 70.0;

inline uint32_t retrace_ms(int retraces) {
    return (retraces <= 0) ? 0 : static_cast<uint32_t>(retraces * 1000.0 / kVgaHz + 0.5);
}

// Wait ms in 5ms slices, watching poll_quit() only. The Input queue (clicks, keys) is left
// untouched, so nothing is lost. Returns false when a quit request arrived while waiting.
bool delay_with_pump(Input& input, uint32_t ms);

// Phase-locked virtual 70.086Hz VGA vertical retrace wait (replaces sub_5E943 / sub_2B206 /
// sub_1D83A / sub_15D2D). Returns false when a quit request arrived while waiting.
bool wait_retrace(Input& input, int retraces = 1);
void wait_retrace(int retraces = 1);

// Wait for a click or any key; max_ms = 0 waits forever.
// true = input arrived, false = quit request or timeout.
bool wait_for_press(Input& input, uint32_t max_ms = 0);

// Fixed-frame pacer (holds a target fps); pace() waits only the remaining time.
class FramePacer {
public:
    explicit FramePacer(double target_fps = 60.0);
    static FramePacer from_period_us(double period_us);
    void reset();
    // False when a quit request arrived while waiting.
    bool pace(Input& input);

private:
    double frame_us_ = 1000000.0 / 60.0;
    double next_us_ = 0.0;
};
}  // namespace GameTimer
