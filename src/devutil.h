#pragma once
// ==== Shared dev/verification helpers (header-only): trace-flag cache and PGM dump ====
// - Every G2_* flag is presence-based (set or not, never its value) and fixed before the
//   process starts, so reading it once and caching it is equivalent. This keeps getenv()
//   (a linear scan of the environment) out of the render and AI loops.
// - Collects the duplicated 6-line header + write sequence of the three 8-bit gray PGM
//   dumps (combatii_map x2, combatii_battle x1). File names and log text stay at the
//   call sites.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace DevUtil {
// Per-flag function-local static cache; equivalent to testing once, since the presence
// flags are fixed before the process starts.
inline bool terrain_trace() {
    static const bool v = (std::getenv("G2_TERRAIN_TRACE") != nullptr);
    return v;
}
inline bool unit_frame_trace() {
    static const bool v = (std::getenv("G2_UNIT_FRAME_TRACE") != nullptr);
    return v;
}
inline bool ai_trace() {
    static const bool v = (std::getenv("G2_AI_TRACE") != nullptr);
    return v;
}
inline bool script_trace() {
    static const bool v = (std::getenv("G2_SCRIPT_TRACE") != nullptr);
    return v;
}
inline bool spawn_fall_trace() {
    static const bool v = (std::getenv("G2_SPAWN_FALL_TRACE") != nullptr);
    return v;
}
inline bool retreat_trace() {
    static const bool v = (std::getenv("G2_RETREAT_TRACE") != nullptr);
    return v;
}
inline bool walk_trace() {
    static const bool v = (std::getenv("G2_WALK_TRACE") != nullptr);
    return v;
}
inline bool text_trace() {
    static const bool v = (std::getenv("G2_TEXT_TRACE") != nullptr);
    return v;
}
inline bool weather_trace() {
    static const bool v = (std::getenv("G2_WEATHER_TRACE") != nullptr);
    return v;
}
inline bool dialogue_trace() {
    static const bool v = (std::getenv("G2_DIALOGUE_TRACE") != nullptr);
    return v;
}
inline bool nc2_trace() {
    static const bool v = (std::getenv("G2_NC2_TRACE") != nullptr);
    return v;
}
inline bool nc2_off() {
    static const bool v = (std::getenv("G2_NC2_OFF") != nullptr);
    return v;
}

// Writes an 8-bit gray PGM; returns true on success.
inline bool dump_pgm_gray(const char* path, const uint8_t* buf, int w, int h) {
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
    std::fwrite(buf, 1, (size_t)w * h, fp);
    std::fclose(fp);
    return true;
}
}  // namespace DevUtil
