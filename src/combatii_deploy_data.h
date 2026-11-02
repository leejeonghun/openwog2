// Numeric tables for the battle deployment UI (sub_54743 / sub_54C2C / sub_551E5).
#pragma once
#include <cstdint>

namespace deploydata {
// unk_731AC[class*15]: mountable class flag.
static const uint8_t kClassRideFlag[165] = {
    1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
};
// 0x75498: mount icon index into frame.syb ([var_8*3 + k], used with +6); -1 = none.
static const int16_t kMountIcon[12] = {0, 1, 2, -1, 3, 2, 4, 5, 2, -1, 6, 7};
// 0x754C8 (unk_754C8): mount cost in ELD, indexed [row*3 + k].
static const uint32_t kMountCost[12] = {100, 150, 500, 0, 100, 500, 100, 200, 500, 0, 200, 500};
static const char kStrSaving[] = "\xb8\xe1\xb8\x77\xba\x97\x2e\x2e\x2e";  // VA 0x77F0C
static const char kStrLoading[] = "\xb7\xaa\x93\x65\xba\x97\x2e\x2e\x2e";  // VA 0x77F16
static const char kStrNotMounted[] = "\xc8\x73\xaf\x77\xac\x77\xc8\x81\x88\x61\x20\xb4\x61\x93\xb3\x93\xa1\x94\x61\x2e";  // VA 0x7809E
static const char kStrEldShortFmt[] = "\x25\x33\x64\x20\x45\x4c\x44\x88\x61\x20\xa6\x81\xb9\xa2\xd0\x73\x93\xa1\x94\x61\x2e";  // VA 0x78088
}  // namespace deploydata
