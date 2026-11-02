// Option/save/load strings extracted from CHP.EXE.
#pragma once
#include <cstdint>


// APRimaA: confirmation box (sub_28E8A) text used by sub_20BC8, Johab. VA 0x508FB
static const char kChpOptMsgMenu[] = "\x8f\x7b\x90\x89\x8c\x61\xb6\x61\x3f";

// ANotSaved. VA 0x501DE
static const char kChpSlotNotSaved[] = "\x4e\x4f\x54\x20\x53\x41\x56\x45\x44";

// ABaEbauaab: bottom row of the save/load list, x=0x68. VA 0x501E8
static const char kChpSlotTitle[] = "\x5b\x20\xa1\x41\xb7\xa5\xa1\x41\x93\x41\x9d\xa1\x20\x5d";

// ABaEbauaab_0: highlight/restore row of the save screen, x=0x78. VA 0x50287
static const char kChpSlotTitleHi[] = "\xa1\x41\xb7\xa5\xa1\x41\x93\x41\x9d\xa1";

// AI: " month " separator. VA 0x5018B
static const char kChpDateMonth[] = "\x20\xb6\xa9\x20";

// AI_0: " day " separator. VA 0x50190
static const char kChpDateDay[] = "\x20\xb7\xa9\x20";

// APb: " hour " separator. VA 0x50195
static const char kChpDateHour[] = "\x20\xaf\xa1\x20";

// AJe: " minute" separator. VA 0x5019A
static const char kChpDateMin[] = "\x20\xa6\x85";
