// ---- D66A8 raw table port (COMBATII sub_11575 / sub_1198D) ----
// Original: unk_D66A8 holds 572B (0x23C) per slot x 143B per direction (13 x 11 poses),
// 13B per entry:
//   entry = D66A8 + 572*slot + 13*(11*dir + pose)
//   entry[0] = tile width, [1] = tile height, [2] = width, [3] = height, [4] = x offset,
//   [5] = y offset, [6] = shadow, [7] = sil_x, [8] = sil_y, [9..12] = sprite pointer
//   (dword_D66B1).
// The loader (sub_11575) fills the bytes from NC2/BCR/RCR files:
//   class0 (.NC2, type < 0x12C): reads [2][3][4][5][7][8] from the file and hardcodes
//     [0]=1 [1]=1 [6]=0.
//   class1 (.BCR, 0x12C <= type < 0x1F4): reads [2][3][4][5][7][8][6][0][1] all from the
//     file (+0x0E shadow, +0x0F/+0x10 tile width/height).
//   class2 (.RCR/.MAR/.WCR, type >= 0x1F4): reads only [2][3][4][5]; the rest stays BSS
//     zero. The port keeps tiles at 1x1 (NC2Parser default, equivalent to the 1x1 grid).
// Port slots: the original uses image slots (0..43, allocated and shared by sub_1198D and
// sub_119B6), while the port NC2 cache is one block per type code. Types pointing at the same
// file name (e.g. the duplicate M-MECHA3.BCR) then get identical separate blocks, which is
// equivalent for footprint, box and centre maths.
// Sprite pointers ([9..12]) are not ported: the port uses NC2Frame pixels directly.
#pragma once
#include <cstdint>

namespace g2d66a8 {
constexpr int kSlotStride = 572;   // 0x23C
constexpr int kEntryStride = 13;   // 0x0D
constexpr int kDirs = 4;
constexpr int kPoses = 11;         // poses per direction (class1/2 fill only 8, rest empty)
constexpr int kEntriesPerSlot = 44;// 4*11
constexpr int kDirStride = 143;    // 13*11

struct Raw {
    uint8_t b[kEntryStride] = {};
};

inline int tile_w(const Raw& e) { return e.b[0]; }
inline int tile_h(const Raw& e) { return e.b[1]; }
inline int spr_w(const Raw& e) { return e.b[2]; }
inline int spr_h(const Raw& e) { return e.b[3]; }
inline int x_off(const Raw& e) { return (int)(int8_t)e.b[4]; }
inline int y_off(const Raw& e) { return (int)(int8_t)e.b[5]; }
inline int shadow(const Raw& e) { return e.b[6]; }
inline int sil_x(const Raw& e) { return (int)(int8_t)e.b[7]; }
inline int sil_y(const Raw& e) { return (int)(int8_t)e.b[8]; }

// Slot offset: 572*slot + 13*(11*dir + pose).
inline int slot_offset(int slot, int dir, int pose) {
    return slot * kSlotStride + (dir * kPoses + pose) * kEntryStride;
}
} // namespace g2d66a8
