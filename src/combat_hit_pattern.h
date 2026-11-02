#pragma once
#include <cstdint>
// ==== Crit pattern ====
// unk_74144 (COMBATII.EXE object 2, VA 0x74144): the 12 x 20 crit pattern of sub_1D6B1.
// Index = unit [+2C] (row, rand(12)) * 20 + [+2D] (column counter). Values are signed bytes;
// each row is a permutation of 1..20.
static constexpr int kBattleHitPatternRows = 12;
static constexpr int kBattleHitPatternCols = 20;
static constexpr int kBattleHitPatternSize = 240;
static const int8_t kBattleHitPattern[kBattleHitPatternSize] = {
     11,   3,  15,   1,  14,   9,  18,  19,   5,  20,  10,   2,  17,  16,  12,   8,   4,  13,   7,   6,  // row 0
     19,  13,  17,   9,   2,  16,   3,  12,   7,  18,   5,   8,   1,  15,  20,   6,  14,  11,  10,   4,  // row 1
      9,  18,  14,  12,  19,  15,  13,  17,   6,   3,   7,  10,  16,   2,   4,   5,   8,  20,  11,   1,  // row 2
      9,   8,  14,   5,  13,  19,  20,  18,  15,   3,   2,   7,  12,  11,   6,   4,   1,  17,  16,  10,  // row 3
     17,  12,   6,   3,   8,   4,   5,  13,   2,  10,  18,  15,  19,  11,   9,  16,  20,   1,   7,  14,  // row 4
      9,  10,   7,  14,  11,   2,   6,  18,  16,  17,   1,  12,   4,   5,   8,   3,  15,  13,  20,  19,  // row 5
     15,   4,  13,  16,  17,   3,   7,  20,  11,   6,   8,  19,   1,   5,  18,  12,  10,   2,   9,  14,  // row 6
     12,   2,  15,   8,  11,  10,   5,  18,   1,  14,  20,   9,   3,  13,   6,  17,   4,  19,  16,   7,  // row 7
     20,  15,   5,   6,   1,  13,  12,  14,   4,   8,  10,  19,   7,   9,  16,  17,   2,  18,  11,   3,  // row 8
     16,   5,   8,  17,   1,  19,  11,  14,   2,   9,   6,  15,  10,  18,   4,   3,  20,  13,   7,  12,  // row 9
     18,   9,  14,   4,  20,   8,  16,  12,   1,   7,  13,  11,  15,   2,  10,  17,   5,   6,  19,   3,  // row 10
      8,  10,  13,   7,   9,   2,   5,  18,   4,   3,  15,   1,   6,  14,  19,  16,  12,  17,  11,  20,  // row 11
};
