// CHP.EXE VA 0x10258 / 0x102D0.
#pragma once
#include <cstdint>

// First 9 entries of dword_10258 (sub_10975 loc_109A2 scans dx < 9 only, so the 10th value
// 0x22 is never reached).
static constexpr uint32_t kChpRewardReassignTeams[9] = { 6, 7, 8, 9, 10, 12, 18, 20, 23 };

// dword_102D0: teams paid 1000 gold (0x3E8); every other team gets 500 (0x1F4).
static constexpr uint32_t kChpRewardGoldTeams[9] = { 10, 13, 23, 28, 31, 25, 36, 39, 42 };
