#pragma once
// ==== CHP.EXE resources read straight from the LE data segment ====
// Sprites are not embedded: exe_resource_load() (src/exe_resource.h) walks the LE object and
// page tables and reads them from these VAs at runtime.
//   dialog frame                 off_82720  (sub_1D874 RLE, 3529 bytes)
//   shade mask                   unk_834E9  (u16 w=250, u16 h=76, w*h bytes; 0xFF = skip)
//   party sprite, team 0x17      unk_7FCA4  (1629 bytes)
//   party sprite, team <= 0x18   off_7F754  (591 bytes)
//   party sprite, other teams    off_7F9A3  (769 bytes)
//   chapter banner box           unk_809A4  (RLE, 5329 bytes)
// ====
#include <cstdint>

// sub_22C8E loc_22EF9..loc_23093: per-team party sprite placement.
// Position = (stronghold screen coord - off) after perspective projection; the visible
// range is bounded by min/max.
struct ChpPartySpriteInfo {
    uint32_t va;               // CHP.EXE VA (RLE stream)
    int off_x, off_y;          // offset subtracted from the stronghold coordinate
    int min_x, min_y, max_x;   // hidden when sx<=min_x / sy<=min_y / sx>=max_x (sy>=0xC7 always is)
};
static const ChpPartySpriteInfo kChpPartySprites[3] = {
    { 0x7FCA4, 0x14, 0x1E,   0, -20, 0x126 },   // team 0x17
    { 0x7F754, 0x10, 0x10, -10, -16, 0x130 },   // team <= 0x18
    { 0x7F9A3, 0x08, 0x14, -10, -16, 0x130 },   // other teams
};
