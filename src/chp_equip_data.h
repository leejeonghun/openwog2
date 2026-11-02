// Values and strings for the equipment screen (sub_30AFE), extracted from CHP.EXE.
#pragma once
#include <cstdint>

// dword_303DD: initial allow flags for the 5 equipment slots (1 = allowed).
static const uint8_t kChpEquipFlagInit[5] = { 1, 1, 1, 1, 1 };

// unk_8877A: class group -> weapon type (0xFF = none). From index 14 on these are the bytes of
// the adjacent string, not table data.
static const uint8_t kChpEquipWeaponTab[19] = { 255, 1, 5, 3, 2, 4, 255, 255, 255, 255, 255, 255, 255, 255, 75, 110, 105, 103, 104 };

// dword_2E0C6: item number + 1 of items that cannot be dropped (sub_2E8C1).
static const uint8_t kChpEquipKeepItems[12] = { 17, 22, 25, 26, 28, 29, 32, 34, 35, 36, 70, 103 };

// A2hand
static const char kChpEquipStr2Hand[] = "\x32\x48\x41\x4e\x44";

// AJob
static const char kChpEquipStrJob[] = "\x4a\x4f\x42";

// unk_5034F: backpack-full message (Johab).
static const char kChpEquipMsgFull[] = "\xb5\x61\xa6\x85\x20\xb4\x61\xb7\xa1\xc9\x51\x20\xb8\xe1\xb8\x77\x89\xb7\x88\x65\xb7\xa1\x20\xb4\xf4\xaf\x73\x93\xa1\x94\x61\x21";

