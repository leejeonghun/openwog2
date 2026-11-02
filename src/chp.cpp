#include "chp.h"
#include "chp_parser.h"
#include "pcx.h"
#include "audio.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "game_timer.h"
#include "portrait.h"
#include "talktbl.h"
#include "chp_data.h"
#include "chp_travel_data.h"
#include "chp_status_data.h"
#include "chp_reward_data.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
#include <SDL.h>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

// ---- Disassembly constants ----
constexpr int MAP_STRIDE = 960;         // 0x3C0
constexpr int MAX_POINTS = 30;          // 0x1E
constexpr int MOUSE_HIT_RANGE = 14;     // 0x0E (sub_21BC6 / loc_1F35B)
constexpr int NUM_MAP_SCAN_LINES = 157; // 0x9D in sub_1CFEB
constexpr int SENTINEL = 999;           // 0x3E7 "don't care" for triggers

// ---- Global memory arrays ----
uint8_t g_event_flags[20000];
WorldPoint g_world_points[MAX_POINTS];
CharacterData g_characters[MAX_CHARACTERS];

// ---- sub_112E0 / sub_10C6D: per-team target quest table (dword_52835) ----
// ASM: mov eax, dword_52835[eax*4], i.e. team -> quest id. Values were extracted
// from the CHP.EXE (LE) data segment dseg02 (base 0x50000): linear address 0x52835
// is file offset 0x48A35, computed from the LE object and page tables and verified
// against known cseg01 code bytes. The pattern restarts at index 46 (linear
// 0x528ED), so the array holds 46 entries (team/chapter 0..45) and byte_528ED is a
// separate table. Teams above 9 (e.g. 0x17 = 23) have their own target quest, so all
// 46 entries are used.
constexpr int TEAM_TARGET_QUEST_COUNT = 46;
static const uint16_t g_team_target_quest[TEAM_TARGET_QUEST_COUNT] = {
    // dword_52835 raw dump (CHP.EXE file offset 0x48A35, 46 dwords, little-endian)
    0, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 18, 19, 21, 22, 23, 24, 27, 28, 30, 33, 34, 36, 37, 38, 39,
    0, 40, 42, 43, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54
};

// "Has this team already run this trigger" flag for sub_10010.
// ASM: unk_8AB88[team*20 + trigger], and unk_8AB88 is unk_8A7A0 (the event flags)
// plus 1000, so this is g_event_flags[1000 + team*20 + trigger] and it is saved and
// restored with the event flags in blackbox.dat.
constexpr int EVENT_EXEC_FLAG_BASE = 1000;
static inline uint8_t* exec_flag(int team, int trigger) {
    int idx = EVENT_EXEC_FLAG_BASE + team * MAX_EVENT_TRIGGERS + trigger;
    if (team < 0 || trigger < 0 || trigger >= MAX_EVENT_TRIGGERS || idx >= 20000) return nullptr;
    return &g_event_flags[idx];
}


// Blackbox slot data (sub_15BD9/sub_15DB7): 100 slots x (gold 4B + count 2B + 300 item words)
uint32_t g_slot_gold[MAX_BLACKBOX_SLOTS];                        // dword_125030
uint16_t g_slot_item_count[MAX_BLACKBOX_SLOTS];                  // word_124F68
uint16_t g_slot_items[MAX_BLACKBOX_SLOTS][BLACKBOX_SLOT_ITEMS];  // unk_116508 (item_id = value-1, 0 = empty)

// ==== unk_80326: chapter title table (Johab), used by the sub_1FC5D banner ====
// Extracted from CHP.EXE (LE) dseg02 (base 0x50000) at file offset 0x76526, found
// through the LE object page table: 20 entries of 20 bytes indexed by chapter id
// 0..19, kept in the original 2-byte Johab encoding (chapter 1 Dagal, 2 Astania,
// ...), which Font::draw_string decodes directly. Chapters 0 and 19 are not real
// data (boundary and spare slots) and stay empty.
static const char* const g_chapter_titles[MAX_CHAPTERS] = {
    /* 0 */  "",
    /* 1 */  "\x94\x61\x88\x69",                                          // Dagal
    /* 2 */  "\xb4\x61\xaf\x61\xc8\x61\x93\xa1\xb4\x61",                  // Astania
    /* 3 */  "\xb4\x69\xc5\x41\xb5\xa1\x93\xa1\xb4\x61",                  // Alkeonia
    /* 4 */  "\xc4\xe1\xcb\xa1\xaf\x61",                                  // Curtis
    /* 5 */  "\xcc\x85\x97\x61\x9c\x81\x89\xa5",                          // Pandragon
    /* 6 */  "\xb0\xe5\x94\xe1\x96\x91",                                  // Thunderdum
    /* 7 */  "\xac\x61\xb7\xa1\x9c\xf3\xaf\x61",                          // Cyrups
    /* 8 */  "\xcb\x61\x9f\xa1\xaf\xa1\xaf\x61\xac\x61\xa0\x62",          // Trisis Desert
    /* 9 */  "\xa7\xa1\xcf\x61\x9d\xa1\xaf\x61\xcb\x61",                  // Bifrost
    /* 10 */ "\x88\x61\x9c\x61\x97\x61",                                  // Garad
    /* 11 */ "\x44\x2f\xcf\x61\x9d\x41\xaf\x61\xac\x65\xa0\x82",          // D/Pres Mountains
    /* 12 */ "\x89\x41\xb7\xa1\xaf\xa1\x9f\x61",                          // Geisir
    /* 13 */ "\xa0\xe1\xa1\x41\x93\xa1\xb4\x65",                          // Memenian
    /* 14 */ "\xa6\x82\x8b\x62\xbb\xa1\xa4\x77",                          // North Pole
    /* 15 */ "\x94\x81\xd0\x81",                                          // Great Sea
    /* 16 */ "\xa5\xb7\xb7\xa5\xbb\xa1",                                  // Sealed Land
    /* 17 */ "\xcb\x61\x9f\xa1\xaf\xa1\xaf\x61\x20\xac\x61\xa0\x62",      // Trisis Desert (spaced)
    /* 18 */ "\xcd\xa2\xce\x97\x95\xa1",                                  // Storm Island
    /* 19 */ "",
};

// unk_809A4: chapter title banner box frame (sub_1FC5D). Read from CHP.EXE at
// VA 0x809A4 as a sub_1D874 RLE stream (62 chunks, 5329 bytes) instead of
// embedding the bytes in the source.

// ---- sub_1CFEB / sub_1D202: perspective projection tables ----
static uint8_t g_line_height_table[NUM_MAP_SCAN_LINES];
static int g_screen_to_map_y[SCREEN_H];
static int g_map_to_screen_y[NUM_MAP_SCAN_LINES + 1];
static bool g_luts_initialized = false;
static uint8_t* g_map_buffer = nullptr;
static int g_map_height = 700;

// ==== 1. unk_5AABC: Mini ASCII Font Character Width Table (0x20..0x7E, 95 chars) ====
// Extracted directly from CHP.EXE data segment (VA 0x5AABC, file offset 0x50CBC)
static const uint8_t g_mini_font_widths[95] = {
    6, 2, 3, 6, 6, 6, 6, 3, 3, 3, 4, 4, 3, 4, 2, 4, // 0x20 .. 0x2F
    6, 3, 6, 6, 6, 6, 6, 6, 6, 6, 2, 3, 4, 4, 4, 5, // 0x30 .. 0x3F
    6, 6, 6, 6, 6, 6, 6, 6, 6, 2, 6, 6, 6, 6, 6, 6, // 0x40 .. 0x4F
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6, 3, 6, 6, // 0x50 .. 0x5F
    3, 6, 6, 6, 6, 6, 6, 6, 6, 2, 6, 6, 6, 6, 6, 6, // 0x60 .. 0x6F
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 2, 3, 6, // 0x70 .. 0x7E
};

// ==== 2. unk_5AB1C: Mini ASCII Font 1bpp Glyph Bitmap (8 bytes per char) ====
// Extracted directly from CHP.EXE data segment (VA 0x5AB1C, file offset 0x50D1C)
static const uint8_t g_mini_font_data[95 * 8] = {
    // 0x20 ' '
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x21 '!'
    0x80, 0x80, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00,
    // 0x22 '"'
    0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x23 '#'
    0x50, 0xF8, 0x50, 0xF8, 0x50, 0x00, 0x00, 0x00,
    // 0x24 '$'
    0x78, 0xA0, 0x70, 0x28, 0xF0, 0x00, 0x00, 0x00,
    // 0x25 '%'
    0x88, 0x10, 0x20, 0x40, 0x88, 0x00, 0x00, 0x00,
    // 0x26 '&'
    0x40, 0xA0, 0x48, 0x90, 0x68, 0x00, 0x00, 0x00,
    // 0x27 '\''
    0x40, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x28 '('
    0x40, 0x80, 0x80, 0x80, 0x40, 0x00, 0x00, 0x00,
    // 0x29 ')'
    0x80, 0x40, 0x40, 0x40, 0x80, 0x00, 0x00, 0x00,
    // 0x2A '*'
    0x00, 0xA0, 0x40, 0xA0, 0x00, 0x00, 0x00, 0x00,
    // 0x2B '+'
    0x00, 0x40, 0xE0, 0x40, 0x00, 0x00, 0x00, 0x00,
    // 0x2C ','
    0x00, 0x00, 0x00, 0x40, 0x80, 0x00, 0x00, 0x00,
    // 0x2D '-'
    0x00, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x2E '.'
    0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    // 0x2F '/'
    0x00, 0x20, 0x40, 0x80, 0x00, 0x00, 0x00, 0x00,
    // 0x30 '0'
    0x70, 0x98, 0x88, 0xC8, 0x70, 0x00, 0x00, 0x00,
    // 0x31 '1'
    0x40, 0xC0, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00,
    // 0x32 '2'
    0xF0, 0x08, 0x70, 0x80, 0xF8, 0x00, 0x00, 0x00,
    // 0x33 '3'
    0xF0, 0x08, 0x70, 0x08, 0xF0, 0x00, 0x00, 0x00,
    // 0x34 '4'
    0x38, 0x48, 0x88, 0xF8, 0x08, 0x00, 0x00, 0x00,
    // 0x35 '5'
    0xF0, 0x80, 0xF0, 0x08, 0xF0, 0x00, 0x00, 0x00,
    // 0x36 '6'
    0x30, 0x40, 0xF0, 0x88, 0x70, 0x00, 0x00, 0x00,
    // 0x37 '7'
    0xF8, 0x10, 0x20, 0x40, 0x80, 0x00, 0x00, 0x00,
    // 0x38 '8'
    0x70, 0x88, 0x70, 0x88, 0x70, 0x00, 0x00, 0x00,
    // 0x39 '9'
    0x70, 0x88, 0x78, 0x08, 0x70, 0x00, 0x00, 0x00,
    // 0x3A ':'
    0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
    // 0x3B ';'
    0x00, 0x40, 0x00, 0x40, 0x80, 0x00, 0x00, 0x00,
    // 0x3C '<'
    0x20, 0x40, 0x80, 0x40, 0x20, 0x00, 0x00, 0x00,
    // 0x3D '='
    0x00, 0xE0, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00,
    // 0x3E '>'
    0x80, 0x40, 0x20, 0x40, 0x80, 0x00, 0x00, 0x00,
    // 0x3F '?'
    0xE0, 0x10, 0x60, 0x00, 0x40, 0x00, 0x00, 0x00,
    // 0x40 '@'
    0x70, 0x88, 0xB8, 0x80, 0x70, 0x00, 0x00, 0x00,
    // 0x41 'A'
    0x70, 0x88, 0x88, 0xF8, 0x88, 0x00, 0x00, 0x00,
    // 0x42 'B'
    0xF0, 0x88, 0xF0, 0x88, 0xF0, 0x00, 0x00, 0x00,
    // 0x43 'C'
    0x78, 0x80, 0x80, 0x80, 0x78, 0x00, 0x00, 0x00,
    // 0x44 'D'
    0xF0, 0x88, 0x88, 0x88, 0xF0, 0x00, 0x00, 0x00,
    // 0x45 'E'
    0xF8, 0x80, 0xF0, 0x80, 0xF8, 0x00, 0x00, 0x00,
    // 0x46 'F'
    0xF8, 0x80, 0xF0, 0x80, 0x80, 0x00, 0x00, 0x00,
    // 0x47 'G'
    0x78, 0x80, 0x88, 0x88, 0x78, 0x00, 0x00, 0x00,
    // 0x48 'H'
    0x88, 0x88, 0xF8, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x49 'I'
    0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00,
    // 0x4A 'J'
    0x08, 0x08, 0x08, 0x08, 0xF0, 0x00, 0x00, 0x00,
    // 0x4B 'K'
    0x90, 0xA0, 0xF0, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x4C 'L'
    0x80, 0x80, 0x80, 0x80, 0xF8, 0x00, 0x00, 0x00,
    // 0x4D 'M'
    0x88, 0xD8, 0xA8, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x4E 'N'
    0x88, 0xC8, 0xA8, 0x98, 0x88, 0x00, 0x00, 0x00,
    // 0x4F 'O'
    0x70, 0x88, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00,
    // 0x50 'P'
    0xF0, 0x88, 0x88, 0xF0, 0x80, 0x00, 0x00, 0x00,
    // 0x51 'Q'
    0x70, 0x88, 0x88, 0x90, 0x68, 0x00, 0x00, 0x00,
    // 0x52 'R'
    0xF0, 0x88, 0xF0, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x53 'S'
    0x78, 0x80, 0x70, 0x08, 0xF0, 0x00, 0x00, 0x00,
    // 0x54 'T'
    0xF8, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00,
    // 0x55 'U'
    0x88, 0x88, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00,
    // 0x56 'V'
    0x88, 0x90, 0xA0, 0xC0, 0x80, 0x00, 0x00, 0x00,
    // 0x57 'W'
    0x88, 0x88, 0xA8, 0xD8, 0x88, 0x00, 0x00, 0x00,
    // 0x58 'X'
    0x88, 0x50, 0x20, 0x50, 0x88, 0x00, 0x00, 0x00,
    // 0x59 'Y'
    0x88, 0x50, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00,
    // 0x5A 'Z'
    0xF8, 0x10, 0x20, 0x40, 0xF8, 0x00, 0x00, 0x00,
    // 0x5B '['
    0xC0, 0x80, 0x80, 0x80, 0xC0, 0x00, 0x00, 0x00,
    // 0x5C '\\'
    0x80, 0x40, 0x20, 0x10, 0x08, 0x00, 0x00, 0x00,
    // 0x5D ']'
    0xC0, 0x40, 0x40, 0x40, 0xC0, 0x00, 0x00, 0x00,
    // 0x5E '^'
    0x00, 0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x00,
    // 0x5F '_'
    0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00,
    // 0x60 '`'
    0x80, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x61 'a'
    0x70, 0x88, 0x88, 0xF8, 0x88, 0x00, 0x00, 0x00,
    // 0x62 'b'
    0xF0, 0x88, 0xF0, 0x88, 0xF0, 0x00, 0x00, 0x00,
    // 0x63 'c'
    0x78, 0x80, 0x80, 0x80, 0x78, 0x00, 0x00, 0x00,
    // 0x64 'd'
    0xF0, 0x88, 0x88, 0x88, 0xF0, 0x00, 0x00, 0x00,
    // 0x65 'e'
    0xF8, 0x80, 0xF0, 0x80, 0xF8, 0x00, 0x00, 0x00,
    // 0x66 'f'
    0xF8, 0x80, 0xF0, 0x80, 0x80, 0x00, 0x00, 0x00,
    // 0x67 'g'
    0x78, 0x80, 0x88, 0x88, 0x78, 0x00, 0x00, 0x00,
    // 0x68 'h'
    0x88, 0x88, 0xF8, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x69 'i'
    0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00,
    // 0x6A 'j'
    0x08, 0x08, 0x08, 0x08, 0xF0, 0x00, 0x00, 0x00,
    // 0x6B 'k'
    0x90, 0xA0, 0xF0, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x6C 'l'
    0x80, 0x80, 0x80, 0x80, 0xF8, 0x00, 0x00, 0x00,
    // 0x6D 'm'
    0x88, 0xD8, 0xA8, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x6E 'n'
    0x88, 0xC8, 0xA8, 0x98, 0x88, 0x00, 0x00, 0x00,
    // 0x6F 'o'
    0x70, 0x88, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00,
    // 0x70 'p'
    0xF0, 0x88, 0x88, 0xF0, 0x80, 0x00, 0x00, 0x00,
    // 0x71 'q'
    0x70, 0x88, 0x88, 0x90, 0x68, 0x00, 0x00, 0x00,
    // 0x72 'r'
    0xF0, 0x88, 0xF0, 0x88, 0x88, 0x00, 0x00, 0x00,
    // 0x73 's'
    0x78, 0x80, 0x70, 0x08, 0xF0, 0x00, 0x00, 0x00,
    // 0x74 't'
    0xF8, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00,
    // 0x75 'u'
    0x88, 0x88, 0x88, 0x88, 0x70, 0x00, 0x00, 0x00,
    // 0x76 'v'
    0x88, 0x90, 0xA0, 0xC0, 0x80, 0x00, 0x00, 0x00,
    // 0x77 'w'
    0x88, 0x88, 0xA8, 0xD8, 0x88, 0x00, 0x00, 0x00,
    // 0x78 'x'
    0x88, 0x50, 0x20, 0x50, 0x88, 0x00, 0x00, 0x00,
    // 0x79 'y'
    0x88, 0x50, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00,
    // 0x7A 'z'
    0xF8, 0x10, 0x20, 0x40, 0xF8, 0x00, 0x00, 0x00,
    // 0x7B '{'
    0x40, 0x40, 0x80, 0x40, 0x40, 0x00, 0x00, 0x00,
    // 0x7C '|'
    0x80, 0x80, 0x00, 0x80, 0x80, 0x00, 0x00, 0x00,
    // 0x7D '}'
    0x80, 0x80, 0x40, 0x80, 0x80, 0x00, 0x00, 0x00,
    // 0x7E '~'
    0x00, 0x40, 0xA8, 0x10, 0x00, 0x00, 0x00, 0x00,
};

static void init_projection_tables() {
    if (g_luts_initialized) return;
    int prev_sy = 0;
    int total_lines = 0;

    for (int dy = 0; dy < NUM_MAP_SCAN_LINES; dy++) {
        g_map_to_screen_y[dy] = prev_sy;
        double t = static_cast<double>(dy + 1) / static_cast<double>(NUM_MAP_SCAN_LINES);
        int next_sy = static_cast<int>(std::floor(SCREEN_H * (0.72 * t + 0x28 * t * t / 100.0) + 0.5));
        if (dy == NUM_MAP_SCAN_LINES - 1) next_sy = SCREEN_H;
        int h = next_sy - prev_sy;
        if (h < 1) h = 1;
        if (h > 2) h = 2;
        g_line_height_table[dy] = static_cast<uint8_t>(h);
        for (int step = 0; step < h && prev_sy + step < SCREEN_H; step++)
            g_screen_to_map_y[prev_sy + step] = dy;
        prev_sy += h;
        total_lines += h;
    }
    g_map_to_screen_y[NUM_MAP_SCAN_LINES] = prev_sy;
    for (int sy = prev_sy; sy < SCREEN_H; sy++)
        g_screen_to_map_y[sy] = NUM_MAP_SCAN_LINES - 1;
    g_luts_initialized = true;
}

CHP::CHP(Video& video, Input& input) : video_(video), input_(input) { init_projection_tables(); }
CHP::~CHP() {
    shutdown_mouse();
    delete[] g_map_buffer;
    g_map_buffer = nullptr;
}

// ==== Mouse cursor ====
// Cursor sprites are read from CHP.EXE object 2 (base 0x50000), VA 0x80633..0x807A7
// (372 bytes), as consecutive variable-length sprites [width u16][height u16]
// [pixels w*h] with 0xFF transparent: the first is released (off_80633, 14x14), the
// second pressed (off_806FB, 14x12). sub_22BCB allows the whole screen (x 0..0x27F,
// y 0..0xC7), so no extra limit applies. The original starts at (150, 100) but does
// not move the OS pointer.
void CHP::init_mouse() {
    std::vector<uint8_t> raw;
    std::vector<CursorFrame> frames;
    if (exe_resource_load(ExeId::Chp, 0x80633, ExeResLength::Fixed, raw, 372) &&
        Video::cursor_parse_bin(raw.data(), raw.size(), frames) && frames.size() >= 2) {
        cursor_normal_ = video_.cursor_register(std::move(frames[0]));
        const int pressed = video_.cursor_register(std::move(frames[1]));
        video_.cursor_set_pressed_variant(cursor_normal_, pressed);   // button state != 0 -> pressed sprite
        video_.cursor_select(cursor_normal_);
        // Apply pressed state, scale and palette changes from the event pump of any
        // wait loop.
        input_.set_poll_hook([this] { video_.cursor_update(); });
        video_.cursor_show(true);
    } else {
        fprintf(stderr, "CHP: CHP.EXE VA 0x80633 cursor sprites unavailable, using the system cursor\n");
    }
}

void CHP::shutdown_mouse() {
    if (cursor_normal_ < 0) return;
    input_.set_poll_hook({});
    video_.cursor_reset();
    cursor_normal_ = -1;
}

// ==== Path helpers ====
std::string CHP::game_path(const char* f) { return file_find_case_insensitive(GAME_DATA_DIR, f); }
std::string CHP::cstldata_path(const char* f) { return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CSTLDATA", f); }
std::string CHP::chapdata_path(const char* f) { return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CHAPDATA", f); }

// ==== sub_286DC: flat 320x200 copy (tool/editor mode) ====
static void blit_map_flat(uint8_t* dest, const uint8_t* src,
                          int32_t cam_x, int32_t cam_y) {
    if (!src || !dest) return;
    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    if (cam_x > MAP_STRIDE - SCREEN_W) cam_x = MAP_STRIDE - SCREEN_W;
    if (cam_y > g_map_height - SCREEN_H) cam_y = g_map_height - SCREEN_H;
    for (int y = 0; y < SCREEN_H; y++) {
        const uint8_t* s = src + (cam_y + y) * MAP_STRIDE + cam_x;
        memcpy(dest + y * SCREEN_W, s, SCREEN_W);
    }
}

// ==== sub_1D202: world map coordinates -> projected screen coordinates ====
void CHP::map_to_screen(int16_t map_x, int16_t map_y, int& sx, int& sy) const {
    int dx = map_x - state_.camera_x;
    int dy = map_y - state_.camera_y;
    if (in_tool_mode_) { sx = dx; sy = dy; return; }
    if (dy <= 0) { sy = dy; }
    else if (dy < NUM_MAP_SCAN_LINES) { sy = g_map_to_screen_y[dy]; }
    else { sy = SCREEN_H + (dy - NUM_MAP_SCAN_LINES) * 2; }
    double scale = (sy + 240.0) / 360.0;
    sx = static_cast<int>(std::floor((dx - 130.0) * scale + 120.0 + 0.5));
}

// ==== sub_1CFEB: 157 map lines with continuous perspective ====
void CHP::render_perspective_map() {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !g_map_buffer) return;
    int cur = 0;
    for (int dy = 0; dy < NUM_MAP_SCAN_LINES && cur < SCREEN_H; dy++) {
        int my = state_.camera_y + dy;
        int rep = g_line_height_table[dy];
        if (my < 0 || my >= g_map_height) {
            for (int r = 0; r < rep && cur < SCREEN_H; r++)
                memset(dest + cur++ * SCREEN_W, 0, SCREEN_W);
            continue;
        }
        const uint8_t* src = g_map_buffer + my * MAP_STRIDE;
        uint8_t* dst_line = dest + cur * SCREEN_W;
        double scale = (cur + 240.0) / 360.0;
        if (scale < 0.1) scale = 0.1;
        for (int x = 0; x < SCREEN_W; x++) {
            int mx = static_cast<int>(std::floor(state_.camera_x + 130.0 + (x - 120.0) / scale + 0.5));
            dst_line[x] = (mx >= 0 && mx < MAP_STRIDE) ? src[mx] : 0;
        }
        cur++;
        for (int r = 1; r < rep && cur < SCREEN_H; r++)
            memcpy(dest + cur++ * SCREEN_W, dst_line, SCREEN_W);
    }
    while (cur < SCREEN_H) memset(dest + cur++ * SCREEN_W, 0, SCREEN_W);
}

// ==== Data loading ====
void CHP::load_config() {
    std::string p = game_path(IPC::GAME_CFG);
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { memset(state_.config_data, 0, sizeof(state_.config_data)); return; }
    fread(state_.config_data, 1, 10, f);
    fclose(f);
}

void CHP::load_blackbox() {
    std::string p = game_path(IPC::BLACKBOX_DAT);
    if (!file_exists(p.c_str())) {
        p = game_path(IPC::BLACKBOX_DEF);
    }
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { fprintf(stderr, "CHP: warning: blackbox.dat/def not found\n"); return; }
    uint8_t b; uint16_t w;
    // Header per sub_15DB7: word_C5D99 (1 byte), word_C5D9B day (2 bytes),
    // byte_C5D9D (1 byte), then 0x4E20 bytes of unk_8A7A0 flags at file+4, which
    // matches PACK's writer (sub_23369). Only the low byte of quest_counter is
    // restored because the file stores a single byte here.
    fread(&b, 1, 1, f); state_.quest_counter = b;
    state_.in_point = (b == 1);          // sub_15DB7: byte_C5DA7 snapshots the low byte of word_C5D99
    fread(&w, 1, 2, f); state_.chapter_day_count = w;   // (abs_year - 1207) * 220 + day
    fread(&b, 1, 1, f); state_.secondary_state = b;      // -> byte_C5EA0
    fread(g_event_flags, 0x4E20, 1, f);
    // File+0x4E24 (2 bytes) is dword_1271E4+2, the blackbox slot, not the chapter.
    // The chapter (byte_C5E9F) comes from contibak.tmp, so only the slot is kept.
    fread(&w, 2, 1, f); blackbox_slot_ = w;
    for (int i = 0; i < MAX_CHARACTERS; i++) fread(&g_characters[i], sizeof(CharacterData), 1, f);
    // 100 inventory slots (sub_15DB7: gold 4 bytes + count 2 bytes + item ids, +1 on load)
    for (int s = 0; s < MAX_BLACKBOX_SLOTS; s++) {
        fread(&g_slot_gold[s], 1, 4, f);
        fread(&g_slot_item_count[s], 1, 2, f);
        // Same file layout as PACK sub_23369 and COMBATII sub_4E4DD: count bytes of
        // 1-byte item ids, +1 on load. The word model holds at most BLACKBOX_SLOT_ITEMS
        // (300) per slot, so any overflow is read and discarded.
        const uint32_t file_cnt = g_slot_item_count[s];
        if (g_slot_item_count[s] > BLACKBOX_SLOT_ITEMS) g_slot_item_count[s] = BLACKBOX_SLOT_ITEMS;
        for (int it = 0; it < BLACKBOX_SLOT_ITEMS; it++) {
            uint8_t stored = 0;
            if (it < g_slot_item_count[s]) fread(&stored, 1, 1, f);
            g_slot_items[s][it] = (it < g_slot_item_count[s]) ? (uint16_t)(uint8_t)(stored + 1) : 0;
        }
        if (file_cnt > g_slot_item_count[s]) fseek(f, (long)(file_cnt - g_slot_item_count[s]), SEEK_CUR);
    }
    fclose(f);
    // sub_15DB7 tail: dword_1271E4 (high word = slot) -> dword_125030[slot] -> dword_C5D9E.
    // Gold is restored through the blackbox slot, not through team_active.
    state_.team_gold = g_slot_gold[blackbox_slot_ < MAX_BLACKBOX_SLOTS ? blackbox_slot_ : 0];

    // sub_15DB7 tail: day count -> absolute year (rel/220 + 1207) and day-of-year (rel % 220).
    uint32_t combined = state_.chapter_day_count;
    uint16_t abs_year = (uint16_t)(combined / DAYS_PER_YEAR + BASE_YEAR);
    uint8_t  day_of_year = (uint8_t)(combined % DAYS_PER_YEAR);
    if ((uint32_t)abs_year * DAYS_PER_YEAR + day_of_year <
        (uint32_t)state_.chapter_start_year * DAYS_PER_YEAR + state_.chapter_start_day) {
        abs_year = state_.chapter_start_year;
        day_of_year = state_.chapter_start_day;
    }
    state_.current_year = abs_year;        // word_C5D8A (absolute)
    state_.time_of_day = day_of_year;      // byte_C5EA3 (day-of-year 0..219)
    state_.current_day = (uint8_t)(day_of_year + 1);
    fprintf(stderr, "CHP: Loaded blackbox - Chapter %d, Year %d, Day %d\n",
            state_.chapter_id, state_.current_year, state_.current_day);
}

// ==== sub_14700: CONTIBAK.TMP full loader ====
bool CHP::load_contibak_file(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) { fprintf(stderr, "CHP: warning: Cannot open '%s'\n", filepath.c_str()); return false; }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    fprintf(stderr, "CHP: Loading contibak '%s' (%ld bytes)\n", filepath.c_str(), file_size);

    // Skip the 64-byte header
    fseek(fp, 0x40, SEEK_SET);

    // Read 20 chapters: points (2374 bytes) and paths (15 x 200 bytes).
    for (int ch = 0; ch < MAX_CHAPTERS; ch++) {
        if (fread(all_chapter_points_[ch], 1, CONTIBAK_CHAPTER_STRIDE, fp) < CONTIBAK_CHAPTER_STRIDE)
            break;
        for (int p = 0; p < MAX_PATHS_PER_CHAPTER; p++)
            fread(all_chapter_paths_[ch][p], 1, CONTIBAK_PATH_STRIDE, fp);
    }

    // Global state block (0xCD bytes at C5DD2)
    uint8_t state_block[0xCD];
    if (fread(state_block, 1, 0xCD, fp) == 0xCD) {
        memcpy(contibak_block_, state_block, sizeof(contibak_block_));   // written back verbatim on save
        state_.chapter_start_year = state_block[0] | (state_block[1] << 8);
        state_.chapter_start_day = state_block[2];
        state_.limit_year = state_block[3] | (state_block[4] << 8);
        state_.limit_day = state_block[5];
        state_.travel_style = state_block[8];     // byte_C5DDA (= word_C5DD9+1): travel effect option
    }

    // State variables
    uint8_t b; uint16_t w;
    fread(&b, 1, 1, fp); state_.chapter_id = b;        // byte_C5E9F
    fread(&b, 1, 1, fp); state_.current_point = b;     // byte_C5EA2
    fread(&w, 2, 1, fp); state_.current_year = w;      // word_C5D8A
    fread(&b, 1, 1, fp); state_.time_of_day = b;       // byte_C5EA3
    fread(&b, 1, 1, fp); state_.secondary_state = b;   // byte_C5EA0
    fread(&b, 1, 1, fp); state_.team_active = b;       // byte_C5EA1

    // Quest objective names (10 x 13 bytes, C5CE8 byte form)
    for (int i = 0; i < QUEST_COUNT; i++)
        fread(g_quest_objectives_[i].name, 1, QUEST_NAME_LEN, fp);

    // Script running markers (10 x 2 bytes, C5D6A)
    for (int i = 0; i < QUEST_COUNT; i++)
        fread(&g_script_markers_[i], 2, 1, fp);

    // Trigger conditions + VM script bytecode
    for (int i = 0; i < MAX_EVENT_TRIGGERS; i++) {
        if (fread(&g_event_triggers_[i], 1, TRIGGER_STRUCT_SIZE, fp) < TRIGGER_STRUCT_SIZE)
            break;
        fread(g_script_data_[i], 1, VM_SCRIPT_RECORD_SIZE, fp);
        memset(g_script_data_[i] + VM_SCRIPT_RECORD_SIZE, 0,
               VM_SCRIPT_MEM_STRIDE - VM_SCRIPT_RECORD_SIZE);
    }

    // Per-chapter trailer: restore scene markers
    int ch_idx = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t& ev_count = all_chapter_points_[ch_idx][CHAPTER_TRAIL_EVENT_COUNT];
    uint8_t& scene_b = all_chapter_points_[ch_idx][CHAPTER_TRAIL_SCENE_B];
    state_.chapter_scene_marker = (ev_count > 0) ? ev_count - 1 : 0;
    state_.chapter_scene_target = scene_b;

    fclose(fp);
    fprintf(stderr, "CHP: Loaded contibak - Ch.%d, Year=%d, Day=%d, Team=%d\n",
            state_.chapter_id, state_.current_year, state_.time_of_day, state_.team_active);
    return true;
}

void CHP::load_chapter_data(int chapter_id) {
    if (!g_map_buffer) g_map_buffer = new uint8_t[MAP_BUFFER_SIZE];
    memset(g_map_buffer, 0, MAP_BUFFER_SIZE);

    // sub_1EAA1 reads only "chapter.<N>"; its palette is the last 768 bytes of that file
    // (sub_2DE6F seeks -0x300 and writes 256 x 3 bytes to the DAC). GENESIS.PAL is used
    // only in tool mode (sub_232A4, byte_80301 == 3), and CHAPPAL.N (sub_1ECCC) builds a
    // path but never reads it. Neither is applied while loading a chapter.

    // Map: CHAPTER.N RLE decompression plus the palette at the end of the file.
    std::vector<std::string> cands = {
        chapdata_path(("chapter." + std::to_string(chapter_id)).c_str()),
        chapdata_path(("CHAPTER." + std::to_string(chapter_id)).c_str()),
        game_path(("chapter." + std::to_string(chapter_id)).c_str()),
        game_path(("CHAPTER." + std::to_string(chapter_id)).c_str())
    };

    for (auto& p : cands) {
        if (file_exists(p.c_str())) {
            CHPParser parser;
            if (parser.load(p) && parser.get_map_data() && parser.get_map_size() > 0) {
                int sz = std::min(parser.get_map_size(), MAP_BUFFER_SIZE);
                memcpy(g_map_buffer, parser.get_map_data(), sz);
                g_map_height = 700;

                const auto (*pal)[3] = parser.get_palette();
                bool has = false;
                for (int i = 0; i < 256; i++) {
                    if (pal[i][0] || pal[i][1] || pal[i][2]) { has = true; break; }
                }
                if (has) {
                    for (int i = 0; i < 256; i++) {
                        palette_set_entry(i, pal[i][0], pal[i][1], pal[i][2]);
                    }
                }
            }
            break;
        }
    }

    // Copy the chapter's points into the live array.
    int idx = chapter_id < MAX_CHAPTERS ? chapter_id : 0;
    memcpy(g_world_points, all_chapter_points_[idx], sizeof(WorldPoint) * MAX_POINTS);

    state_.chapter_scene_marker = all_chapter_points_[idx][CHAPTER_TRAIL_SCENE_A];
    state_.chapter_scene_target = all_chapter_points_[idx][CHAPTER_TRAIL_SCENE_B];
}

// ==== sub_22C11, sub_22562, sub_2213E: point & path activation ====

void CHP::update_point_activation() {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t* ch_base = all_chapter_points_[ch];

    for (int i = 0; i < MAX_POINTS; i++) {
        uint8_t* pt = ch_base + i * 66;
        if (pt[0x14] != 0) continue;

        uint8_t gene_case = pt[0x29];
        if (gene_case > 5) continue;

        auto read_w = [&](int off) -> uint16_t {
            return pt[off] | (pt[off + 1] << 8);
        };

        switch (gene_case) {
        case 0: { // Event switch check (loc_225AE)
            // ASM compares the byte of flags[word@+0x2A] with word@+0x2C directly
            // (mov dl, ch; mov di, ...; cmp dx, di); there is no 999 wildcard.
            uint16_t sw = read_w(0x2A);
            uint16_t val = read_w(0x2C);
            if (sw < 20000) {
                const uint16_t fv = g_event_flags[sw];
                if (fv != 0 && fv == val) pt[0x14] = 1;
            }
            break;
        }
        case 1: { // Pass day check (loc_225E5)
            int elapsed = (state_.current_year - state_.chapter_start_year) * DAYS_PER_YEAR
                        + (state_.time_of_day - state_.chapter_start_day);
            uint8_t pass_day = pt[0x33];
            if (elapsed >= pass_day) {
                pt[0x14] = 1;
            }
            break;
        }
        case 2: { // Time check (loc_22640)
            int cur_time = state_.current_year * DAYS_PER_YEAR + state_.time_of_day;
            int gene_time = read_w(0x30) * DAYS_PER_YEAR + pt[0x32];
            if (cur_time >= gene_time) {
                pt[0x14] = 1;
            }
            break;
        }
        case 3: { // Point arrival - Victory (loc_22689)
            // Checked only when byte_C5DA7 (arrival/success flag) is 1.
            if (state_.in_point) {
                uint16_t p1 = read_w(0x2A), p2 = read_w(0x2C), p3 = read_w(0x2E);
                if (state_.current_point == p1 || state_.current_point == p2 || state_.current_point == p3) {
                    pt[0x14] = 1;
                }
            }
            break;
        }
        case 4: { // Point arrival - Any/Fail (loc_22734)
            if (!state_.in_point) {
                uint16_t p1 = read_w(0x2A), p2 = read_w(0x2C), p3 = read_w(0x2E);
                if (state_.current_point == p1 || state_.current_point == p2 || state_.current_point == p3) {
                    pt[0x14] = 1;
                }
            }
            break;
        }
        case 5: { // All required points cleared (loc_227B4)
            // A word passes when its point kind (+0x15) is 2 or the word itself is 0.
            // There is no 999 wildcard; out-of-range indices are treated as failing
            // because the original reads past the chapter array and can never see 2.
            auto ok = [&](uint16_t p) {
                if (p == 0) return true;
                if (p >= MAX_POINTS_PER_CHAPTER) return false;
                return ch_base[p * 66 + 0x15] == 2;
            };
            if (ok(read_w(0x2A)) && ok(read_w(0x2C)) && ok(read_w(0x2E))) {
                pt[0x14] = 1;
            }
            break;
        }
        }
    }
}

void CHP::update_path_activation() {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t* ch_base = all_chapter_points_[ch];

    for (int p = 0; p < MAX_PATHS_PER_CHAPTER; p++) {
        int off = 0x7BC + p * 26;
        if (off + 26 > CONTIBAK_CHAPTER_STRIDE) break;
        // sub_2213E head: `cmp byte ptr unk_B71F8[chapter*0xBB8 + p*200], 0 ; jz skip`.
        // The gate is the first byte of the path shape data (unk_B71F8, 200 bytes per
        // path, direction code 0 = none), not the record's active byte.
        if (all_chapter_paths_[ch][p][0] == 0) continue;

        uint8_t p_case = ch_base[off + 3];
        if (p_case > 3) continue;

        auto read_w = [&](int o) -> uint16_t {
            return (uint16_t)(ch_base[off + o] | (ch_base[off + o + 1] << 8));
        };

        bool activate = false;
        switch (p_case) {
        case 0: { // loc_221A0: compare the current point when byte_C5DA7 is 1
            if (state_.in_point) {
                uint16_t cp = state_.current_point;
                activate = (cp == read_w(4) || cp == read_w(6) || cp == read_w(8));
            }
            break;
        }
        case 1: { // loc_2225F: `tod % 2 == word@+0x0F` (remainder, 16-bit compare)
            const int parity = state_.time_of_day % 2;
            activate = (parity == (int)read_w(15));
            break;
        }
        case 2: { // loc_22282: elapsed days >= byte@+0x0D
            int elapsed = (state_.current_year - state_.chapter_start_year) * DAYS_PER_YEAR
                        + (state_.time_of_day - state_.chapter_start_day);
            activate = (elapsed >= (int)ch_base[off + 13]);
            break;
        }
        case 3: { // loc_222E6: current day >= 220 * word@+0x0A + byte@+0x0C
            uint32_t cur = (uint32_t)state_.current_year * DAYS_PER_YEAR + state_.time_of_day;
            uint32_t tgt = (uint32_t)read_w(10) * DAYS_PER_YEAR + ch_base[off + 12];
            activate = (cur >= tgt);
            break;
        }
        }
        if (activate) ch_base[off] = 1;
    }
}

void CHP::deactivate_paths() {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t* ch_base = all_chapter_points_[ch];

    for (int p = 0; p < MAX_PATHS_PER_CHAPTER; p++) {
        int off = 0x7BC + p * 26;
        if (off + 26 > CONTIBAK_CHAPTER_STRIDE) break;
        // sub_2233E uses the same gate as sub_2213E (first byte of the path shape data).
        if (all_chapter_paths_[ch][p][0] == 0) continue;

        uint8_t p_case = ch_base[off + 14];
        if (p_case > 3) continue;

        auto read_w = [&](int o) -> uint16_t {
            return (uint16_t)(ch_base[off + o] | (ch_base[off + o + 1] << 8));
        };

        bool deactivate = false;
        switch (p_case) {
        case 0: { // loc_223A0: case byte is +0x0E; compared words are +0x0F/+0x11/+0x13
            if (state_.in_point) {
                uint16_t cp = state_.current_point;
                deactivate = (cp == read_w(15) || cp == read_w(17) || cp == read_w(19));
            }
            break;
        }
        case 1: { // loc_22457: `tod % 2 == word@+0x0F`
            const int parity = state_.time_of_day % 2;
            deactivate = (parity == (int)read_w(15));
            break;
        }
        case 2: { // loc_22489: elapsed days >= byte@+0x18
            int elapsed = (state_.current_year - state_.chapter_start_year) * DAYS_PER_YEAR
                        + (state_.time_of_day - state_.chapter_start_day);
            deactivate = (elapsed >= (int)ch_base[off + 24]);
            break;
        }
        case 3: { // loc_224E7: current day >= 220 * word@+0x15 + byte@+0x17
            uint32_t cur = (uint32_t)state_.current_year * DAYS_PER_YEAR + state_.time_of_day;
            uint32_t tgt = (uint32_t)read_w(21) * DAYS_PER_YEAR + ch_base[off + 23];
            deactivate = (cur >= tgt);
            break;
        }
        }
        if (deactivate) ch_base[off] = 0;
    }
}

void CHP::deactivate_points() {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t* ch_base = all_chapter_points_[ch];

    for (int i = 0; i < MAX_POINTS; i++) {
        uint8_t* pt = ch_base + i * 66;
        if (pt[0x14] != 1) continue;
        if (pt[0x15] == 2) continue;

        // sub_2288C field layout (mirrors sub_22562): +0x34 case byte; case 0 = byte
        // +0x3E (elapsed days), case 1 = word +0x3B (year) + byte +0x3D (day),
        // case 2 = words +0x35 / +0x37 / +0x39 compared with the current point.
        uint8_t d_case = pt[0x34];
        bool deactivate = false;
        auto read_w = [&](int o) -> uint16_t {
            return (uint16_t)(pt[o] | (pt[o + 1] << 8));
        };

        switch (d_case) {
        case 0: {
            int elapsed = (state_.current_year - state_.chapter_start_year) * DAYS_PER_YEAR
                        + (state_.time_of_day - state_.chapter_start_day);
            deactivate = (elapsed >= (int)pt[0x3E]);
            break;
        }
        case 1: {
            uint32_t cur = (uint32_t)state_.current_year * DAYS_PER_YEAR + state_.time_of_day;
            uint32_t tgt = (uint32_t)read_w(0x3B) * DAYS_PER_YEAR + pt[0x3D];
            deactivate = (cur >= tgt);
            break;
        }
        case 2: {
            if (state_.in_point) {
                const uint16_t cp = state_.current_point;
                deactivate = (cp == read_w(0x35) || cp == read_w(0x37) || cp == read_w(0x39));
            }
            break;
        }
        default:
            continue;
        }

        // Only the active byte is cleared; reactivation reuses the stored fields.
        if (deactivate) pt[0x14] = 0;
    }
}

void CHP::reset_and_update_points(int mode) {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t* ch_base = all_chapter_points_[ch];

    init_camera();

    if (state_.chapter_start_year != 0 && state_.chapter_started_flag[ch]) {
        state_.current_year = state_.chapter_start_year;
        state_.time_of_day = state_.chapter_start_day;
    }

    if (mode == 1) {
        for (int i = 0; i < MAX_POINTS; i++) {
            ch_base[i * 66 + 0x14] = 0;
        }
    }

    update_point_activation();
    deactivate_points();
    update_path_activation();
    deactivate_paths();

    memcpy(g_world_points, ch_base, sizeof(WorldPoint) * MAX_POINTS);
}

// sub_220B3: sync the current point's kind byte (+0x15) with its shadow copy.
//   byte_C5DA7 == 1 : +0x15 = +0x3F, and +0x40 copies it only when the value is 0 or 2
//   otherwise       : +0x15 = +0x40
// The time-limit check is sub_22A5E (check_time_limit), a different routine.
void CHP::sync_point_kind() {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    uint8_t* ch_base = all_chapter_points_[ch];
    uint8_t* pt = ch_base + state_.current_point * 66;
    if (state_.in_point) {
        const uint8_t k = pt[0x3F];
        pt[0x15] = k;
        if (k == 2 || k == 0) pt[0x40] = k;
    } else {
        pt[0x15] = pt[0x40];
    }
    memcpy(g_world_points, ch_base, sizeof(WorldPoint) * MAX_POINTS);
}

// ==== sub_112E0: SCRIPT1..3.TBL loader ====
void CHP::load_script_tables(int mode) {
    auto load_tbl = [](const char* name, int& count, uint16_t records[][4]) -> bool {
        FILE* fp = fopen(name, "rt");
        if (!fp) { fprintf(stderr, "CHP: warning: %s not found\n", name); return false; }
        count = 0;
        int ch;
        while (count < 100) {
            bool found_open = false;
            while ((ch = fgetc(fp)) != EOF) {
                if (ch == '{') { found_open = true; break; }
                if (ch == '.') { break; }
            }
            if (!found_open || ch == EOF || ch == '.') break;
            int tmp[4] = {0};
            if (fscanf(fp, "%d %d %d %d", &tmp[0], &tmp[1], &tmp[2], &tmp[3]) == 4) {
                for (int k = 0; k < 4; k++) records[count][k] = (uint16_t)tmp[k];
                count++;
            }
        }
        fclose(fp);
        fprintf(stderr, "CHP: Loaded %s (%d records)\n", name, count);
        return true;
    };

    constexpr int MAX_TBL_RECORDS = 100;
    uint16_t tbl3[MAX_TBL_RECORDS][4] = {};
    uint16_t tbl2[MAX_TBL_RECORDS][4] = {};
    uint16_t tbl1[MAX_TBL_RECORDS][4] = {};
    int n3 = 0, n2 = 0, n1 = 0;

    std::string p1 = game_path("SCRIPT1.TBL");
    if (!file_exists(p1.c_str())) p1 = game_path("script1.tbl");
    load_tbl(p1.c_str(), n1, tbl1);

    if (mode != 0) {
        std::string p2 = game_path("SCRIPT2.TBL");
        if (!file_exists(p2.c_str())) p2 = game_path("script2.tbl");
        load_tbl(p2.c_str(), n2, tbl2);

        std::string p3 = game_path("SCRIPT3.TBL");
        if (!file_exists(p3.c_str())) p3 = game_path("script3.tbl");
        load_tbl(p3.c_str(), n3, tbl3);
    }

    // sub_112E0 loc_11568..loc_115D4: SCRIPT3.TBL condition test.
    //   mov dx, unk_8F5C0[eax]      ; col0 = target quest id
    //   cmp dx, 3E7h ; jz loc_115DE ; 999 sentinel ends the table
    //   mov dl, byte_C5EA1          ; active team
    //   cmp ebx, dword_52835[edx*4] ; matches that team's target quest?
    //   jnz loc_115D4               ; mismatch: skip the flag writes
    // dword_52835 is the per-team target quest table (g_team_target_quest). The
    // compare is unconditional, so a 0 entry matches too. The original does no
    // index check; a team id past the 46-entry table matches no quest.
    uint16_t team_target = (state_.team_active < TEAM_TARGET_QUEST_COUNT)
                               ? g_team_target_quest[state_.team_active] : 0xFFFF;

    for (int i = 0; i < n3; i++) {
        if (tbl3[i][0] == SENTINEL) break; // 999 sentinel
        // ASM: cmp ebx, dword_52835[edx*4]; jnz loc_115D4 - skip on mismatch.
        if (tbl3[i][0] != team_target) {
            continue;
        }
        if (tbl3[i][1] != 0 && tbl3[i][1] < 20000) g_event_flags[tbl3[i][1]] = 1;
        if (tbl3[i][2] != 0 && tbl3[i][2] < 20000) g_event_flags[tbl3[i][2]] = 1;
        if (tbl3[i][3] != 0 && tbl3[i][3] < 20000) g_event_flags[tbl3[i][3]] = 1;
    }

    for (int i = 0; i < n2; i++) {
        if (tbl2[i][0] == SENTINEL) break;
        bool all_met = true;
        for (int f = 1; f < 4; f++) {
            if (tbl2[i][f] != 0 && g_event_flags[tbl2[i][f]] == 0) { all_met = false; break; }
        }
        if (all_met && tbl2[i][0] < QUEST_STATE_SIZE && state_.quest_state[tbl2[i][0]] != 2)
            state_.quest_state[tbl2[i][0]] = 3;
    }

    for (int i = 0; i < n1; i++) {
        if (tbl1[i][0] == SENTINEL) break;
        bool all_met = true;
        for (int f = 1; f < 4; f++) {
            if (tbl1[i][f] != 0 && g_event_flags[tbl1[i][f]] == 0) { all_met = false; break; }
        }
        if (all_met && tbl1[i][0] < QUEST_STATE_SIZE && state_.quest_state[tbl1[i][0]] == 0)
            state_.quest_state[tbl1[i][0]] = 1;
    }
}

// ==== Display init ====
void CHP::init_display() { video_.set_mode_13h(); }
void CHP::load_fonts() { font_.load_hangul(game_path("NEWHAN.FNT").c_str()); font_.load_ascii(game_path("NEWASCII.FNT").c_str()); font_.set_text_mode(4, 0x1000, 0xFF); }
void CHP::init_audio() { Audio::init(); }

// ==== sub_1FDC4: system sprites and items loader ====
void CHP::load_all_system_sprites() {
    load_item_data(); // sub_2E4C0: load ITEM.DAT
    point_sprites_loaded_ = load_spr(point_sprites_, "CHAPDATA", "POINT.SPR", false);
    font_sprites_loaded_  = load_spr(font_sprites_, "CHAPDATA", "FONT.SPR", false);
    time_sprites_loaded_  = load_spr(time_sprites_, ".", "TIME_GRA.SYF", false);
    item_box_sprites_loaded_ = load_spr(item_box_sprites_, ".", "ITEM.SPR", false);
    load_item_sprites(); // sub_2E533: load CITIZEN2.SPR
    load_banner_box_sprite(); // unk_809A4: chapter title banner box frame
    load_extracted_sprites(); // dialog frame, shade mask, party sprites
}

// ==== unk_809A4: chapter title banner box frame (RLE) ====
// Unlike POINT.SPR/FONT.SPR this sprite lives in the EXE data segment (sub_1FC5D),
// so it is read from CHP.EXE at VA 0x809A4 as an RLE stream instead of embedding
// 5 KB of bytes. The format is the one blit_sprite_rle() expects (u16 chunk count,
// then [skip:u16][dword_count:u8][byte_count:u8] + pixel runs), used unchanged.
bool CHP::load_banner_box_sprite() {
    banner_box_sprite_loaded_ =
        exe_resource_load(ExeId::Chp, 0x809A4, ExeResLength::Rle, banner_box_sprite_);
    return banner_box_sprite_loaded_;
}

// Dialog and party sprites, read from the CHP.EXE data segment VAs.
//   off_82720 : dialog frame (sub_1D874 RLE)
//   unk_834E9 : shade mask (u16 w, u16 h, w*h bytes: 0xFF = skip, else = shaded)
//   unk_7FCA4 / off_7F754 / off_7F9A3 : party sprites (RLE)
void CHP::load_extracted_sprites() {
    exe_resource_load(ExeId::Chp, 0x82720, ExeResLength::Rle, dialog_frame_rle_);
    exe_resource_load(ExeId::Chp, 0x834E9, ExeResLength::Sprite, dialog_shade_mask_);
    for (int i = 0; i < 3; i++)
        exe_resource_load(ExeId::Chp, kChpPartySprites[i].va, ExeResLength::Rle, party_sprite_data_[i]);
}

// ==== sub_22B12: camera initialization ====
//   lea ebx, [edx-82h]   ; cam_x = mx - 130 (0x82)
//   sub eax, 5Fh         ; cam_y = my - 95  (0x5F)
//   add edx, 0BEh        ; edx = mx + 190 (0xBE)
//   cmp edx, 3C0h        ; mx + 190 >= 960 ?
//   jl  loc_22B7B
//   lea eax, [ebx-280h]  ; then edx = ebx - eax, so cam_x = 640 (0x280)
//   mov dword_A4368, edx
//   add eax, 0C8h        ; cam_y + 200 (0xC8)
//   cmp eax, 2BCh        ; >= 700 (0x2BC) ?
//   jl  loc_22BA4
//   sub eax, 1F4h        ; cam_y - 500 (0x1F4)
//   sub ebx, eax         ; ebx = 500
//   mov dword_A436C, ebx ; cam_y = 500 (0x1F4)
void CHP::init_camera() {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    int pt = state_.current_point;
    if (pt < 0 || pt >= MAX_POINTS) pt = 0;

    const WorldPoint* points = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
    int16_t mx = points[pt].map_x;
    int16_t my = points[pt].map_y;

    int32_t cam_x = mx - 0x82; // 130
    int32_t cam_y = my - 0x5F; // 95

    // sub_22B12: right edge pins cam_x to 640 (0x280), the map's right limit (960-320).
    if (mx + 0xBE >= 0x3C0) {
        cam_x = 0x280;
    }
    // sub_22B12: bottom edge pins cam_y to 500 (0x1F4), the map's bottom limit (700-200).
    if (cam_y + 0xC8 >= 0x2BC) {
        cam_y = 0x1F4;
    }
    if (cam_x < 0) cam_x = 0;
    if (cam_y < 0) cam_y = 0;
    // Clamp again after the left/top clamp (sub_22B12 tail).
    if (cam_x > MAP_STRIDE - SCREEN_W) cam_x = MAP_STRIDE - SCREEN_W;
    if (cam_y > g_map_height - SCREEN_H) cam_y = g_map_height - SCREEN_H;

    state_.camera_x = cam_x;
    state_.camera_y = cam_y;
    fprintf(stderr, "CHP: init_camera pt=%d (%d,%d) -> cam=(%d,%d) [exact match with ASM sub_22B12]\n",
            pt, mx, my, cam_x, cam_y);
}

// sub_1ED53: edge scroll, disabled while moving (byte_A2511) or panning (byte_7BCA4 == 0).
//   mouse x < 10 or Left  : cam_x > 0x51  -> -4     mouse x > 0x131 or Right : cam_x < 0x230 -> +4
//   mouse y < 10 or Up    : cam_y > 1     -> -2     mouse y > 0xB9  or Down  : cam_y < 0x1F4 -> +2
void CHP::update_camera() {
    if (moving_ || pan_active_) return;
    int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
    const uint8_t* k = SDL_GetKeyboardState(NULL);
    if ((k[SDL_SCANCODE_LEFT]  || mx < 10)    && state_.camera_x > 0x51)  state_.camera_x -= 4;
    if ((k[SDL_SCANCODE_RIGHT] || mx > 0x131) && state_.camera_x < 0x230) state_.camera_x += 4;
    if ((k[SDL_SCANCODE_UP]    || my < 10)    && state_.camera_y > 1)     state_.camera_y -= 2;
    if ((k[SDL_SCANCODE_DOWN]  || my > 0xB9)  && state_.camera_y < 0x1F4) state_.camera_y += 2;
}

// ==== Rendering ====
// sub_21C58: map background plus point sprites and names only. It draws neither
// the party sprite (sub_1D726) nor the time/day/Days Remain overlay (loc_230E2);
// those are drawn separately by the world-map loop (sub_22C8E) every frame. Only
// this routine runs before the chapter banner (sub_1FC5D).
void CHP::render_map_and_points() {
    uint8_t* d = video_.get_backbuffer();
    if (!d) return;
    // sub_1ED53: byte_A2511 == 1, byte_7BCA4 == 0 and a non-zero travel effect option
    // select the sky/ground flying view, which draws no point sprites.
    if (travel_fx_ && moving_ && pan_active_ && !in_tool_mode_ && g_map_buffer) {
        render_flying_view();
        return;
    }
    if (g_map_buffer) {
        if (in_tool_mode_) blit_map_flat(d, g_map_buffer, state_.camera_x, state_.camera_y);
        else render_perspective_map();
    } else memset(d, 0, SCREEN_W * SCREEN_H);
    render_point_sprites();
}

// Composite frame for the interactive loop (sub_22C8E / loc_230E2): map and
// points, then the party sprite and the time/day/Days Remain overlay.
void CHP::render_world_map() {
    render_map_and_points();
    render_party_sprite();
    render_ui_overlay();
}

// loc_1F0DC (the point drawing body of sub_21C58), in original order. For each
// point i (0..29) with active (+0x14) == 1:
//   (1) Marker = POINT.SPR entry kind*2. dword_9D086 steps the slot-2 pointer table
//       in 8-byte strides (0x9C722 + 2*0x4B2 == 0x9D086), so it skips every other
//       4-byte pointer. (ux, uy) = map - camera + descriptor offset, i.e. the
//       pre-projection position. Projection (sub_1D202) runs only when
//       byte_80301 != 3; drawing (sub_1D874) only in tool mode (byte_80301 == 3)
//       with 0 <= x < 0x12C and 0 < y < 0xC7.
//   (2) Name: drawn only when uy + 0x14 >= 10 and name[0] != '?'. Two 10-byte lines
//       (up to NUL) with width = chars * 4, because the branch that would count 'i'
//       as 7 compares 'i' against 'I' and therefore never runs. x = ux - width/2 + 1,
//       y = uy + 4 for the first line and uy + 12 for the second, projected and
//       drawn with sub_18047 when -10 < y < 0xC7. Colour group: 0 when the mouse is
//       within 14 pixels of the unoffset projected point and kind != 2, otherwise
//       kind -> {0:1, 1:2, 3:3, other:4}.
void CHP::render_point_sprites() {
    for (int i = 0; i < MAX_POINTS; i++) {
        const WorldPoint& pt = g_world_points[i];
        if (pt.active != 1) continue;

        // Marker descriptor (+4 = x offset, +6 = y offset); a missing or
        // out-of-range sprite contributes an offset of 0.
        const SpriteEntry* sp = point_sprites_loaded_ ? point_sprites_.get((int)pt.kind * 2) : nullptr;
        const int xoff = sp ? sp->x_offset : 0;
        const int yoff = sp ? sp->y_offset : 0;

        const int ux = pt.map_x - state_.camera_x + xoff;     // [esp+24h] / [esp+1Ch]
        const int uy = pt.map_y - state_.camera_y + yoff;     // [esp+18h]
        const int ty = uy + 0x14;                             // [esp+20h]

        // map_to_screen subtracts the camera itself, so add it back before projecting.
        auto project = [&](int x, int y, int& ox, int& oy) {
            if (in_tool_mode_) { ox = x; oy = y; return; }
            map_to_screen((int16_t)(x + state_.camera_x), (int16_t)(y + state_.camera_y), ox, oy);
        };

        // (1) Marker, tool mode only.
        if (in_tool_mode_ && sp && sp->pixel_data) {
            int mx_, my_;
            project(ux, uy, mx_, my_);
            if (mx_ >= 0 && mx_ < 0x12C && my_ > 0 && my_ < 0xC7)
                blit_sprite_rle(mx_, my_, sp->pixel_data);
        }

        // (2) Name.
        if (ty < 0x0A) continue;
        if ((uint8_t)pt.display_name[0] == 0x3F) continue;

        char line1[11], line2[11];
        memset(line1, 0, sizeof(line1)); memset(line2, 0, sizeof(line2));
        int n1 = 0, n2 = 0;
        while (n1 < 10 && pt.display_name[n1] != '\0') { line1[n1] = pt.display_name[n1]; n1++; }
        while (n2 < 10 && pt.display_name[10 + n2] != '\0') { line2[n2] = pt.display_name[10 + n2]; n2++; }
        const int w1 = n1 * 4, w2 = n2 * 4;

        // Hover test: distance from the unoffset projected point to the mouse
        // (dword_8FF24 / dword_8FF28).
        int hx, hy;
        project(pt.map_x - state_.camera_x, pt.map_y - state_.camera_y, hx, hy);
        const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
        int font_group;
        if (std::abs(hx - mx) < 14 && std::abs(hy - my) < 14 && pt.kind != 2) {
            font_group = 0;
        } else {
            switch (pt.kind) {
                case 0:  font_group = 1; break;
                case 1:  font_group = 2; break;
                case 3:  font_group = 3; break;
                default: font_group = 4; break;
            }
        }

        int tx, tyy;
        project(ux - w1 / 2 + 1, ty - 0x10, tx, tyy);
        if (tyy > -10 && tyy < 0xC7) draw_sprite_font(tx, tyy, line1, font_group);
        project(ux - w2 / 2 + 1, ty - 8, tx, tyy);
        if (tyy > -10 && tyy < 0xC7) draw_sprite_font(tx, tyy, line2, font_group);
    }
}

// Draw an RLE sprite (sub_1D874 format) clipped to the screen. The stream encodes
// pixels with a 320-pixel stride, so relative offsets map to (column, row).
void CHP::blit_sprite_rle_clipped(int x, int y, const uint8_t* data) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !data) return;
    int chunk_count = data[0] | (data[1] << 8);
    const uint8_t* src = data + 2;
    int rel = 0;
    for (int c = 0; c < chunk_count; c++) {
        int skip = src[0] | (src[1] << 8);
        int total = src[2] * 4 + src[3];
        src += 4;
        rel += skip;
        for (int b = 0; b < total; b++) {
            int col = (rel + b) % SCREEN_W, row = (rel + b) / SCREEN_W;
            int dx = x + col, dy = y + row;
            if (dx >= 0 && dx < SCREEN_W && dy >= 0 && dy < SCREEN_H)
                dest[dy * SCREEN_W + dx] = src[b];
        }
        rel += total;
        src += total;
    }
}

// sub_22C8E loc_22EF9..loc_23093: party sprite. Position = current point
// (map - camera, y - 8) minus the per-team offset, then projected. Team 0x17 uses
// sprite A (-20,-30); teams <= 0x18 use B (-16,-16); everything else uses
// C (-8,-20). Not drawn on the first frame (byte_80320) or while moving (byte_A2511).
void CHP::render_party_sprite() {
    if (first_frame_ || moving_) return;
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    int ci = state_.current_point;
    if (ci < 0 || ci >= MAX_POINTS) return;
    const WorldPoint& pt = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch])[ci];
    render_party_sprite_at(pt.map_x, pt.map_y - 8);
}

void CHP::render_party_sprite_at(int world_x, int world_y) {
    const uint8_t team = state_.team_active;
    const int idx = (team == 0x17) ? 0 : (team <= 0x18 ? 1 : 2);   // chp_data.h kChpPartySprites
    const ChpPartySpriteInfo& info = kChpPartySprites[idx];
    if (party_sprite_data_[idx].empty()) return;

    int sx, sy;
    map_to_screen((int16_t)(world_x - info.off_x), (int16_t)(world_y - info.off_y), sx, sy);
    if (sx <= info.min_x || sy <= info.min_y || sx >= info.max_x || sy >= 0xC7) return;
    blit_sprite_rle_clipped(sx, sy, party_sprite_data_[idx].data());
}

void CHP::render_ui_overlay() {
    render_time_display();
}

// ==== sub_1FC5D: chapter title banner ====
// 1. Draw unk_809A4, the RLE frame embedded in the CHP.EXE data segment, at
//    (52, 52) with no offset adjustment; sub_1D874 receives a bare RLE stream
//    with no x/y_offset header.
// 2. Centre unk_80326[byte_C5E9F] (20 bytes, Johab) on x = 160:
//    edi = 160 - strlen/2 * 8, i.e. half a 16 px cell per character left.
// 3. Render it with sub_2D2F4 (the Johab string renderer) at y = 60 (0x3C).
// 4. Flip and wait for a click or the timeout (sub_284AC).
void CHP::show_chapter_title_banner() {
    uint8_t* d = video_.get_backbuffer();
    if (!d) return;

    // 1. Banner box frame: banner_box_sprite_ (unk_809A4) at (52, 52). sub_1D874
    //    reads no x/y_offset header, so the coordinates are used as-is.
    if (banner_box_sprite_loaded_ && !banner_box_sprite_.empty()) {
        blit_sprite_rle(52, 52, banner_box_sprite_.data());
    }

    // 2-3. Centre the Johab chapter name (unk_80326).
    int ch = (state_.chapter_id < MAX_CHAPTERS) ? state_.chapter_id : 0;
    const char* name = g_chapter_titles[ch];
    if (name && name[0] != '\0') {
        int byte_len = (int)strlen(name);
        int num_chars = byte_len / 2; // Johab: two bytes per character
        int x = 160 - num_chars * 8;  // ASM: edi = 0xA0 - (strlen>>1)<<3
        int y = 0x3C;                 // 60

        font_.set_text_mode(4, 0x1000, 0xFF);
        font_.draw_string(d, SCREEN_W, SCREEN_H, x, y, name);
    }

    // 4. Flip, then wait for a click or key (sub_284AC).
    video_.present();
    if (!wait_for_input()) return;
}

// ==== World map <-> point zoom effects (sub_1CCB3 / sub_1CE44 / sub_1DB9B / sub_1DA88) ====
// src  = backbuffer (dword_C5EA4) + y_off*320 + x_off, a linear address whose
//        top-left is (x_off, y_off).
// dest = unk_DA370, a 320x200 buffer with slack that is preserved between calls.
// Each call rebuilds two DDA tables (unk_7B46D for X, unk_7B96D for Y):
//        s = (double)size / 320 or 200 (fild/fdiv/fstp qword) and
//        delta[k] = trunc((k+1)*s) - trunc(k*s) using fistp truncation. The x87
//        80-bit accumulator makes k*s exact for these magnitudes.
// Forward scatter places src(sx,sy) at dest(x,y), advances x by dx[sx] and ends
// the row at x >= 320, then advances y by dy[sy] and stops at y >= 200. A scale
// above 1 leaves gaps that keep the previous frame, which is the original shimmer.
// sub_1CCB3 plots one pixel (sub_1D851); sub_1CE44 fills a dx*dy rectangle
// (sub_1DB9B), used for the first zoom-out frame. sub_1DB9B does no clipping: it
// writes linearly, so x past 320 spills into the start of the next row.
namespace {

constexpr int kZoomW = 320;
constexpr int kZoomH = 200;
// Slack for the rows sub_1DB9B overruns (up to dy-1); only the first 200 are shown.
constexpr int kZoomBufSize = kZoomW * (kZoomH + 8);

struct ZoomBuf {
    uint8_t px[kZoomBufSize];
};

// Compute trunc(k * s) exactly, matching the x87 80-bit result.
static void zoom_build_dda(int arg, double base, int count, int* delta) {
    const double s = static_cast<double>(arg) / base;       // fild + fdiv + fstp qword
    int e = 0;
    const double m = std::frexp(s, &e);                      // s = m * 2^e, 0.5 <= m < 1
    const int64_t mant = static_cast<int64_t>(std::ldexp(m, 53));   // s = mant / 2^(53-e)
    const int sh = 53 - e;
    auto fl = [&](int k) -> int {
        const int64_t v = static_cast<int64_t>(k) * mant;
        return static_cast<int>(sh >= 0 ? (v >> sh) : (v << -sh));
    };
    int prev = 0;
    for (int k = 0; k < count; k++) {
        const int cur = fl(k + 1);
        delta[k] = cur - prev;
        prev = cur;
    }
}

// Common body of sub_1CCB3 and sub_1CE44: fill_rect false plots a pixel, true
// fills a dx*dy rectangle. src_off is a linear offset into the backbuffer
// (y_off*320 + x_off); reads outside it return 0.
static void zoom_scatter(ZoomBuf& dest, const uint8_t* src /*64000*/, int src_off,
                         int width, int height, bool fill_rect) {
    int dx[kZoomW], dy[kZoomH];
    zoom_build_dda(width,  320.0, kZoomW, dx);
    zoom_build_dda(height, 200.0, kZoomH, dy);

    int y = 0;
    int row_off = 0;                                  // var_10
    for (int sy = 0; sy < kZoomH; sy++) {
        int x = 0;
        int p = src_off + row_off;                    // edi
        for (int sx = 0; sx < kZoomW; sx++) {
            const uint8_t c = (p >= 0 && p < kZoomW * kZoomH) ? src[p] : 0;
            p++;
            if (!fill_rect) {
                dest.px[y * kZoomW + x] = c;          // sub_1D851
            } else {
                // sub_1DB9B(x, y, w=dx[sx], h=dy[sy], c): linear write that skips
                // (320 - w) at each row end.
                const int w = dx[sx], h = dy[sy];
                if (w > 0 && h > 0) {
                    int d = y * kZoomW + x;
                    for (int r = 0; r < h; r++) {
                        for (int q = 0; q < w; q++) {
                            if (d >= 0 && d < kZoomBufSize) dest.px[d] = c;
                            d++;
                        }
                        d += kZoomW - w;
                    }
                }
            }
            x += dx[sx];
            if (x >= kZoomW) break;
        }
        row_off += kZoomW;
        y += dy[sy];
        if (y >= kZoomH) break;
    }
}

} // namespace

// ==== sub_22C8E (0x22C8E..0x22DB8): point -> world map zoom-out ====
// Initial: width = 0x4B0 (1200), height = 0x320 (800), x_off = 0x91 (145),
// y_off = 0x26 (38), ebp = 0x358 (856), K = 0x249 (585).
// sub_21C58 renders the map into the backbuffer, then sub_35755 plays into.voc.
// First frame: sub_1CE44 fills unk_DA370 from backbuffer + 0x3011 (38*320 + 145)
// without displaying it. loop loc_22D16 ends when width <= 320 or height <= 200:
//   sub_1CCB3 scatters pixels from backbuffer + y_off*320 + x_off over the
//   previous frame, sub_1DA88 presents unk_DA370, then
//   x_off -= 9 - ebp/0x50; y_off -= 3 - K/0x78;
//   width -= 0x18; height -= 0x0F; ebp -= 0x18; K -= 0x0F.
// All motion is driven by the top-left offsets; the original has no screen centre.
void CHP::zoom_out_effect() {
    uint8_t* back = video_.get_backbuffer();
    if (!back || !g_map_buffer) return;

    render_map_and_points();                          // sub_21C58 with the current camera
    Audio::play_voc(game_path("into.voc"));           // sub_35755(2)

    static uint8_t src[kZoomW * kZoomH];
    static ZoomBuf dest;                              // unk_DA370
    memcpy(src, back, sizeof(src));

    int width = 0x4B0, height = 0x320;
    int x_off = 0x91, y_off = 0x26;
    int ebp = 0x358, K = 0x249;

    // First frame: fill rectangles so dest has no gaps; the loop's first copy displays it.
    zoom_scatter(dest, src, 0x3011, width, height, true);

    while (width > 0x140 && height > 0xC8) {
        zoom_scatter(dest, src, y_off * kZoomW + x_off, width, height, false);

        memcpy(back, dest.px, sizeof(src));           // sub_1DA88
        video_.present();
        GameTimer::delay_with_pump(input_, travel_frame_ms());   // sub_1DA88 vsync (C5CDE == 0: 70 Hz, 1: no wait)

        x_off -= 9 - ebp / 0x50;                      // idiv truncates toward zero, like C
        y_off -= 3 - K / 0x78;
        width -= 0x18;
        height -= 0x0F;
        ebp -= 0x18;
        K -= 0x0F;

        input_.update();
        if (input_.quit_requested()) { transition_exit("EXIT"); return; }
    }
    // sub_22C8E does not render or present after the loop: the last scattered frame
    // stays visible until the interactive loop's first sub_21476 + sub_21C58 pass.
}

// ==== sub_204A7 (0x204A7..0x20648): world map -> point zoom-in ====
// The caller skips the whole effect for "EXIT". Camera = (map_x-0x8C, map_y-0x69),
// then sub_21C58 renders and sub_1DA88 presents it. sub_1DDC4 backs up the palette
// (word_A3D4C) and into.voc plays. Start: width = 0x140, height = 0xC8, offsets 0.
// loc_20566: once width >= 0x4B0 or height >= 0x320, sub_1E4CD(A3D4C, 0, 0xFF, 2)
// darkens the palette by one step per call; it returns 1 for 29 calls and 0 on the
// 30th, which ends the loop. Each iteration scatters points into unk_DA370,
// adds 0x18/0x0F to width/height, presents, then
//   x_off += 4 - (width-0x140)/0xFA; y_off += 4 - (height-0xC8)/0x6E
// using the already-grown sizes. The first frame has s = 1.0 and fills dest 1:1;
// later frames leave the previous frame in the gaps.
void CHP::zoom_in_effect() {
    uint8_t* back = video_.get_backbuffer();
    if (!back || !g_map_buffer) return;

    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    int pt = state_.current_point;
    int16_t center_mx = 0, center_my = 0;
    if (pt >= 0 && pt < MAX_POINTS) {
        const WorldPoint* points = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
        center_mx = points[pt].map_x;
        center_my = points[pt].map_y;
    }
    state_.camera_x = center_mx - 0x8C;
    state_.camera_y = center_my - 0x69;
    render_map_and_points();                          // sub_21C58
    video_.present();                                 // sub_1DA88(C5EA4)

    memcpy(travel_pal_, g_palette, sizeof(travel_pal_));   // sub_1DDC4(word_A3D4C)
    Audio::play_voc(game_path("into.voc"));                // sub_35755(2)

    static uint8_t src[kZoomW * kZoomH];
    static ZoomBuf dest;                              // unk_DA370
    memcpy(src, back, sizeof(src));

    int width = 0x140, height = 0xC8;
    int x_off = 0, y_off = 0;
    fade_counter_ = 0;

    while (true) {
        if (width >= 0x4B0 || height >= 0x320) {
            if (!travel_fade_step()) break;           // sub_1E4CD: one palette step, false = stop
        }
        zoom_scatter(dest, src, y_off * kZoomW + x_off, width, height, false);
        width += 0x18;
        height += 0x0F;

        memcpy(back, dest.px, sizeof(src));           // sub_1DA88
        video_.present();
        GameTimer::delay_with_pump(input_, travel_frame_ms());   // sub_1DA88 vsync (C5CDE == 0: 70 Hz, 1: no wait)

        x_off += 4 - (width - 0x140) / 0xFA;
        y_off += 4 - (height - 0xC8) / 0x6E;

        input_.update();
        if (input_.quit_requested()) { transition_exit("EXIT"); return; }
    }
    memset(g_palette, 0, sizeof(g_palette));
    video_.present();
}

int CHP::check_point_click(int mx, int my) {
    for (int i=0;i<MAX_POINTS;i++) {
        const WorldPoint& pt = g_world_points[i];
        if (pt.active != 1) continue;
        int sx, sy; map_to_screen(pt.map_x, pt.map_y, sx, sy);
        if (std::abs(mx-sx) <= MOUSE_HIT_RANGE && std::abs(my-sy) <= MOUSE_HIT_RANGE) return i;
    }
    return -1;
}

// ==== sub_10010: event trigger scanner ====
// Scans the trigger table for the current chapter, point, quest counter, day
// count and flags, then runs each matching script once per team.
int CHP::check_event_triggers(int mode) {
    fprintf(stderr, "CHP: check_event_triggers(mode=%d)\n", mode);
    int executed = 0;

    for (int i = 0; i < MAX_EVENT_TRIGGERS; i++) {
        const EventTriggerCondition& tr = g_event_triggers_[i];

        // ASM loc_10024: state == 0 and flag_idx1 == 999 with idx2 == idx1 and
        // idx3 == idx2 marks the terminator record.
        if (tr.state_check == 0 && tr.flag_idx1 == SENTINEL &&
            tr.flag_idx2 == tr.flag_idx1 && tr.flag_idx3 == tr.flag_idx2) {
            break;
        }

        // Chapter, point and quest counter must match, or be the 999 wildcard.
        if (tr.chapter != SENTINEL && tr.chapter != (int16_t)state_.chapter_id) continue;
        if (tr.point_id != SENTINEL && tr.point_id != (int16_t)state_.current_point) continue;
        if (tr.state_check != SENTINEL && tr.state_check != (int16_t)state_.quest_counter) continue;

        // Elapsed days fail when pass_days > (year delta * 220 + day - start day)
        // (the ASM jle branch).
        if (tr.pass_days != SENTINEL) {
            int elapsed = ((int)state_.current_year - (int)state_.chapter_start_year) * DAYS_PER_YEAR
                        + (int)state_.time_of_day - (int)state_.chapter_start_day;
            if ((int)tr.pass_days > elapsed) continue;
        }

        // Flag checks: the original widens the byte to a word and compares it with
        // the 16-bit value. Indices are sign-extended (sar 16) and unchecked, so
        // anything outside 0..19999 reads as 0.
        auto flag_ok = [&](int16_t idx, int16_t val) {
            if (idx == SENTINEL) return true;
            int fv = (idx >= 0 && idx < 20000) ? g_event_flags[idx] : 0;
            return fv == (int)(uint16_t)val;
        };
        if (!flag_ok(tr.flag_idx1, tr.flag_val1)) continue;
        if (!flag_ok(tr.flag_idx2, tr.flag_val2)) continue;
        if (!flag_ok(tr.flag_idx3, tr.flag_val3)) continue;

        // Mode 0 runs exec_class 1 only; mode 1 runs exec_class 0 only. Other modes
        // (e.g. 4) run nothing.
        int team = state_.team_active;
        uint8_t* ef = exec_flag(team, i);
        bool class_match = (mode == 0 && tr.exec_class == 1) || (mode == 1 && tr.exec_class == 0);
        if (class_match && ef && *ef == 0) {
            *ef = 1;
            fprintf(stderr, "  Trigger %d [class %d] executed for team %d\n", i, tr.exec_class, team);
            execute_event_script(i);
            executed++;
        }

        // ASM loc_101DE: after every trigger that passes its conditions, clear exec
        // flags 14..19 for the current team regardless of whether it ran, so triggers
        // 14..19 can fire repeatedly.
        for (int t = 14; t <= 19; t++) {
            uint8_t* rf = exec_flag(state_.team_active, t);
            if (rf) *rf = 0;
        }
    }

    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    memcpy(g_world_points, all_chapter_points_[ch], sizeof(WorldPoint) * MAX_POINTS);

    return executed;
}

// ==== sub_2BDFB: event script VM ====
// Records are 25 words (50 bytes): opcode at word+0, arguments at word+2, +4, ...
// ip counts words (ASM ebp, +0x19 per command). Execution ends on opcode 0 or an
// opcode above 15 (outside the jump table).
int CHP::execute_event_script(int trigger_id) {
    if (trigger_id < 0 || trigger_id >= MAX_EVENT_TRIGGERS) return -1;
    const uint8_t* script = g_script_data_[trigger_id];
    const int kRecBytes = VM_SCRIPT_MEM_STRIDE;   // 4000
    int ip = 0;

    fprintf(stderr, "  VM: Start script %d\n", trigger_id);

    while (true) {
        int byte_off = ip * 2;
        if (byte_off + VM_INSTR_SIZE > kRecBytes) break;

        uint16_t opcode = script[byte_off] | (script[byte_off + 1] << 8);
        if (opcode == 0 || opcode > 15) break;   // dec(op) > 0xE -> loc_2C204 (end)

        auto word_at = [&](int off) -> uint16_t {
            int b = byte_off + off;
            if (b + 1 >= kRecBytes) return 0;
            return script[b] | (script[b + 1] << 8);
        };
        auto byte_at = [&](int off) -> uint8_t {
            int b = byte_off + off;
            return (b < kRecBytes) ? script[b] : 0;
        };

        ip += VM_INSTR_WORDS;

        switch (opcode) {
        case VM_DELETE_POINT: {
            int ch = word_at(2), pt = word_at(4);
            if (ch < MAX_CHAPTERS && pt < MAX_POINTS_PER_CHAPTER) {
                uint8_t* base = all_chapter_points_[ch];
                base[pt * 66 + 0x14] = 0;
                *reinterpret_cast<uint16_t*>(&base[pt * 66 + 0x30]) = 9999;   // 0x270F
            }
            fprintf(stderr, "    OP_DELETE_POINT ch=%d pt=%d\n", ch, pt);
            break;
        }
        case VM_NOP2:
        case VM_NOP:
            break;
        case VM_CREATE_POINT: {
            int ch = word_at(2), pt = word_at(4);
            if (ch < MAX_CHAPTERS && pt < MAX_POINTS_PER_CHAPTER)
                all_chapter_points_[ch][pt * 66 + 0x14] = 1;
            fprintf(stderr, "    OP_CREATE_POINT ch=%d pt=%d\n", ch, pt);
            break;
        }
        case VM_TEAM_SWITCH: {
            // ASM loc_2BE85: set word ptr dword_1271E4+2 (the team/gold slot,
            // blackbox_slot_) from the script word, then rebuild the party with
            // sub_15A7F + sub_15AE5. team_active (byte_C5EA1, used by exec_flag)
            // is left untouched.
            uint16_t team = word_at(2);
            blackbox_slot_ = team;
            fprintf(stderr, "    OP_TEAM_SWITCH slot=%u (team_active=%u unchanged)\n",
                    (unsigned)blackbox_slot_, (unsigned)state_.team_active);
            vm_team_rebuild();
            break;
        }
        case VM_MESSAGE_TABLE: {
            // ASM loc_2BEA3: sub_2B1BD(portrait=0, name="", talk_id=word@+2, ecx=0)
            int talk_id = (int16_t)word_at(2);
            fprintf(stderr, "    OP_MESSAGE_TABLE talk=%d\n", talk_id);
            run_talk(0, "", talk_id);
            break;
        }
        case VM_MESSAGE_INLINE: {
            // ASM loc_2BEC4: portrait = word@+2, talk_id = word@+4, speaker name
            // from +6, one character per word for 20 words.
            int portrait = (int16_t)word_at(2);
            int talk_id = (int16_t)word_at(4);
            char name[21] = {0};
            for (int c = 0; c < 20; c++) {
                name[c] = (char)byte_at(6 + c * 2);
                if (name[c] == 0) break;
            }
            fprintf(stderr, "    OP_MESSAGE_INLINE portrait=%d talk=%d name='%s'\n", portrait, talk_id, name);
            run_talk(portrait, name, talk_id);
            break;
        }
        case VM_ENTER_POINT:
            // ASM calls sub_20912 with AL zero, so the banner path (param == 1)
            // never runs here.
            fprintf(stderr, "    OP_ENTER_POINT\n");
            enter_point(0);
            break;
        case VM_WARP: {
            // ASM loc_2BF2B: change chapter and point, reload the map, refresh
            // activations and redraw. It does not advance the date, rescan triggers,
            // enter the point or check for a chapter clear.
            uint8_t new_ch = byte_at(2);
            uint8_t new_pt = byte_at(4);
            fprintf(stderr, "    OP_WARP ch=%d pt=%d\n", new_ch, new_pt);
            if (new_ch >= MAX_CHAPTERS) break;
            state_.chapter_id = new_ch;
            state_.current_point = new_pt;
            state_.chapter_scene_marker = 0;
            state_.chapter_scene_target = 0;
            load_chapter_data(state_.chapter_id);
            update_point_activation();
            update_path_activation();
            render_map_and_points();
            video_.present();
            break;
        }
        case VM_EVENT_SWITCH: {
            uint16_t flag_idx = word_at(2);
            uint8_t flag_val = byte_at(4);
            if (flag_idx < 20000) {
                g_event_flags[flag_idx] = flag_val;
                fprintf(stderr, "    OP_EVENT_SWITCH flag[%d] = %d\n", flag_idx, flag_val);
            }
            break;
        }
        case VM_SET_POINT_NAME: {
            // ASM loc_2C00F: kind = byte@+6, name = 13 bytes from +8, one per word,
            // copied with strcpy to point+0x1C.
            int ch = word_at(2), pt = word_at(4);
            if (ch < MAX_CHAPTERS && pt < MAX_POINTS_PER_CHAPTER) {
                uint8_t* base = all_chapter_points_[ch];
                int off = pt * 66;
                base[off + 0x15] = byte_at(6);
                char tmp[14] = {0};
                for (int c = 0; c < 13; c++) tmp[c] = (char)byte_at(8 + c * 2);
                strcpy(reinterpret_cast<char*>(&base[off + 0x1C]), tmp);   // bytes after the NUL survive
                fprintf(stderr, "    OP_SET_POINT_NAME ch=%d pt=%d kind=%d '%s'\n", ch, pt, byte_at(6), tmp);
            }
            break;
        }
        case VM_POINT_KIND: {
            // ASM loc_2C0A6: sub_17E45(word@+2, word@+4) sets the team field of a
            // 1-based character, then the party is rebuilt.
            int char_no = word_at(2), team = word_at(4);
            fprintf(stderr, "    OP_SET_CHAR_TEAM char=%d team=%d\n", char_no, team);
            vm_set_character_team(char_no, team);
            break;
        }
        case VM_NARRATION: {
            uint16_t talk_id = word_at(2);
            fprintf(stderr, "    OP_NARRATION talk=%d\n", talk_id);
            run_narration(talk_id);
            break;
        }
        case VM_TIME_PASS: {
            // ASM loc_2C0D9: 8-bit add; above 0xDC (220) increment the year and
            // subtract 220.
            uint8_t days = byte_at(2);
            uint8_t d = (uint8_t)(state_.time_of_day + days);
            if (d > 0xDC) {
                state_.current_year++;
                d = (uint8_t)(d - 0xDC);
            }
            state_.time_of_day = d;
            fprintf(stderr, "    OP_TIME_PASS +%d days -> %d\n", days, d);
            break;
        }
        case VM_PARTY_MERGE: {
            // ASM loc_2C10D
            int src = (int16_t)word_at(2);
            int dst = (int16_t)word_at(4);
            fprintf(stderr, "    OP_PARTY_MERGE %d -> %d\n", src, dst);
            if (src < 0 || dst < 0 || src >= MAX_BLACKBOX_SLOTS || dst >= MAX_BLACKBOX_SLOTS) break;
            for (int c = 0; c < MAX_CHARACTERS; c++)
                if (chr_team(g_characters[c]) == (uint8_t)src) chr_team(g_characters[c]) = (uint8_t)dst;
            if (src != dst) {
                // The original copies item bytes, not words: it appends
                // src_count bytes from the src block at byte offset dst_count in the
                // dst block and increments dst_count once per byte, although items
                // are 16-bit. That behaviour is reproduced here.
                uint8_t* raw = reinterpret_cast<uint8_t*>(&g_slot_items[0][0]);
                const int total = MAX_BLACKBOX_SLOTS * BLACKBOX_SLOT_STRIDE;
                uint16_t& dcnt = g_slot_item_count[dst];
                for (int i = 0; i < (int)g_slot_item_count[src]; i++) {
                    int di = dst * BLACKBOX_SLOT_STRIDE + dcnt;
                    int si = src * BLACKBOX_SLOT_STRIDE + i;
                    if (di < 0 || di >= total || si >= total) break;
                    raw[di] = raw[si];
                    dcnt++;
                }
                g_slot_item_count[src] = 0;
                g_slot_gold[dst] += g_slot_gold[src];
                g_slot_gold[src] = 0;
                if (dst == state_.team_active) state_.team_gold = g_slot_gold[dst];
            }
            vm_team_rebuild();
            break;
        }
        }
    }

    // loc_2C204: clear the "running" marker (C5D6A) of the finished script.
    for (int q = 0; q < QUEST_COUNT; q++)
        if (g_script_markers_[q] == (uint16_t)(trigger_id + 1)) g_script_markers_[q] = 0;

    return 0;
}

void CHP::vm_set_character_team(int char_no, int team) {
    // sub_17E45: byte_8FFE3[(char_no-1)*0x88] = team, then sub_15A7F + sub_15AE5.
    int idx = (char_no & 0xFFFF) - 1;
    if (idx >= 0 && idx < MAX_CHARACTERS) chr_team(g_characters[idx]) = (uint8_t)team;
    vm_team_rebuild();
}

// ==== sub_20912: point entry handler ====
// param == 1 shows the chapter banner (sub_1FC5D) first, used on chapter start
// and when a JUMP enters a new chapter.
void CHP::enter_point(int param) {
    fprintf(stderr, "CHP: enter_point(param=%d) chapter=%d point=%d\n",
            param, state_.chapter_id, state_.current_point);

    render_map_and_points();     // sub_21C58
    video_.present();            // sub_1DA67
    if (param == 1) show_chapter_title_banner();   // sub_1FC5D

    int ch = state_.chapter_id;
    int pt = state_.current_point;
    if (ch < 0 || ch >= MAX_CHAPTERS || pt < 0 || pt >= MAX_POINTS) return;

    uint8_t* point_base = all_chapter_points_[ch] + pt * 66;
    uint8_t kind = point_base[0x15];
    const char* id_name = reinterpret_cast<const char*>(point_base + 0x1C);

    switch (kind) {
    case 0: // TOWN - sub_204A7 writes id_name verbatim to the "$" file (no prefix;
            // Pack extracts the digits).
        fprintf(stderr, "  -> TOWN '%s'\n", id_name);
        do_transition(id_name);
        break;
    case 1: // BATTLE - id_name + ".SCE"
    {
        fprintf(stderr, "  -> BATTLE scenario='%s'\n", id_name);
        std::string sce = std::string(id_name) + ".SCE";
        do_transition(sce.c_str());
        break;
    }
    case 3: // JUMP - id_name[0..2] is the chapter (atoi), id_name[3] the point
            // Character ('1' -> 0).
    {
        char ch_str[4] = { (char)point_base[0x1C], (char)point_base[0x1D], (char)point_base[0x1E], 0 };
        int target_ch = atoi(ch_str);
        int target_pt = (uint8_t)(point_base[0x1F] - 0x30 - 1);
        if (target_ch < 0 || target_ch >= MAX_CHAPTERS) {
            fprintf(stderr, "  -> Invalid JUMP target chapter %d, skipping.\n", target_ch);
            return;
        }
        if (target_pt >= MAX_POINTS) target_pt = 0;
        fprintf(stderr, "  -> JUMP to chapter=%d point=%d\n", target_ch, target_pt);

        state_.chapter_id = (uint8_t)target_ch;
        state_.current_point = (uint8_t)target_pt;
        // 8-bit add; above 0xDC increment the year and subtract 0xDC.
        uint8_t d = (uint8_t)(state_.time_of_day + 3);
        if (d > 0xDC) { state_.current_year++; d = (uint8_t)(d - 0xDC); }
        state_.time_of_day = d;
        if (check_time_limit()) return;   // sub_22A5E: past the limit -> TIME OVER -> EXIT
        restore_party_hp_mp();            // loc_20B23 / loc_2184A: restore party HP/MP to max

        load_chapter_data(state_.chapter_id);
        state_.chapter_scene_marker = 0;
        state_.chapter_scene_target = 0;
        update_point_activation();
        update_path_activation();
        render_map_and_points();
        check_event_triggers(1);
        enter_point(1);
        check_event_triggers(0);
        check_chapter_clear();
        break;
    }
    default: // 2 (NULL) and anything else does nothing.
        break;
    }
}

// ==== sub_1088D: chapter clear / victory check ====
void CHP::check_chapter_clear() {
    fprintf(stderr, "CHP: check_chapter_clear()\n");

    int ch = state_.chapter_id;
    int pt = state_.current_point;
    if (ch < MAX_CHAPTERS && pt < MAX_POINTS) {
        const char* cur_name = reinterpret_cast<const char*>(all_chapter_points_[ch] + pt * 66 + 0x1C);
        for (int i = 0; i < QUEST_COUNT; i++) {
            if (g_quest_objectives_[i].name[0] == 0) continue;
            if (strcmp(g_quest_objectives_[i].name, cur_name) == 0 && state_.quest_counter == 1) {
                g_quest_objectives_[i].name[0] = 0;
                fprintf(stderr, "  Objective '%s' cleared\n", cur_name);
                break;
            }
        }
    }

    // ASM loc_108FA: for the first running marker whose flag is set, clear that one
    // marker and leave the loop (mov word_C5D6A[eax], cx ; jmp loc_10929).
    for (int i = 0; i < QUEST_COUNT; i++) {
        if (g_script_markers_[i] == 0) continue;
        const int flag_idx = g_script_markers_[i];
        if (flag_idx >= 0 && flag_idx < 20000 && g_event_flags[flag_idx] != 0) {
            g_script_markers_[i] = 0;
            break;
        }
    }

    int idle_count = 0;
    for (int i = 0; i < QUEST_COUNT; i++) {
        if (g_script_markers_[i] == 0) idle_count++;
        else break;
    }
    int cleared_count = 0;
    for (int i = 0; i < QUEST_COUNT; i++) {
        if (g_quest_objectives_[i].name[0] == 0) cleared_count++;
        else break;
    }

    if (cleared_count == QUEST_COUNT && idle_count == QUEST_COUNT) {
        state_.chapter_clear = 1;
        fprintf(stderr, "  *** CHAPTER CLEARED! ***\n");
        // sub_1088D only sets byte_523C0 = 1; the gold/team reward (sub_10975) is
        // not called here. Its only caller is sub_232A4 right after chapter entry
        // (see chapter_menu).
    }
}

// sub_10975: chapter-entry reward and team fixup, called once from sub_232A4+231.
//   team = byte_C5EA1 (active team); slot = dword_C5E9B, the low word of the save
//   block's trailing 4 bytes (the editor's "Active Team").
//   cl = 1 when team appears in dword_10258[0..8]; the tenth value (0x22) lies
//   outside the scan, as in the original.
//   When byte_8A86A (g_event_flags[202]) is 1:
//     dword_1271E4+2 = slot, then rebuild the party copy for that slot;
//     byte_8A869 (g_event_flags[201]) = low byte of word_C5DDB[0];
//     if cl, retag every record whose team equals slot as 0x63 and rebuild;
//     for i in 0..20, set record[word_C5DDB[i] - 1].team = slot when the entry is
//     non-zero, rebuild, then add 1000 or 500 gold to dword_125030[slot] depending
//     on whether team is in dword_102D0[0..8]; finally byte_8A86A = 0 and
//     dword_C5D9E = dword_125030[slot].
// word_C5DDB is 21 words at +9 of the 0xCD-byte contibak block (base C5DD2);
// dword_C5E9B is at +0xC9 of that block.
void CHP::handle_chapter_reward() {
    if (g_event_flags[202] != 1) return;                   // cmp byte_8A86A, 1 / jnz loc_10AD4

    const int team = state_.team_active;                   // byte_C5EA1
    bool cl = false;                                       // loc_109A2: first 9 entries only
    for (int i = 0; i < 9; i++) if ((int)kChpRewardReassignTeams[i] == team) { cl = true; break; }
    bool gold1000 = false;                                 // loc_10A72
    for (int i = 0; i < 9; i++) if ((int)kChpRewardGoldTeams[i] == team) { gold1000 = true; break; }

    const uint16_t slot = (uint16_t)(contibak_block_[0xC9] | (contibak_block_[0xCA] << 8));   // dword_C5E9B
    const int party_team = slot;                           // dword_1271E4+2
    if (slot != (uint16_t)team)
        fprintf(stderr, "CHP: sub_10975 slot(word_C5E9B)=%u != team(byte_C5EA1)=%d - using the slot, as the original does\n", slot, team);
    blackbox_slot_ = slot;                                 // mov word ptr dword_1271E4+2, ax

    party_rebuild(party_team);                             // sub_15A7F + sub_15AE5

    // byte_8A869 = low byte of word_C5DDB[0]
    g_event_flags[201] = contibak_block_[9];

    if (cl) {
        for (int c = 0; c < MAX_CHARACTERS; c++)           // loc_109FC: 0x7B records
            if ((uint16_t)chr_team(g_characters[c]) == slot) chr_team(g_characters[c]) = 0x63;
        party_rebuild(party_team);
    }

    for (int i = 0; i < 21; i++) {                         // loc_10A30: 0x15 words
        const uint16_t w = (uint16_t)(contibak_block_[9 + 2 * i] | (contibak_block_[10 + 2 * i] << 8));
        if (w == 0) continue;
        const int idx = (int)(int16_t)w - 1;               // (word_C5DDB[i] - 1) * 0x88
        if (idx >= 0 && idx < MAX_CHARACTERS) chr_team(g_characters[idx]) = (uint8_t)slot;
        else fprintf(stderr, "CHP: warning: sub_10975 party list entry %d (id %d) out of range - the original writes outside the record array, ignored\n", i, (int)(int16_t)w);
    }
    party_rebuild(party_team);

    const int gs = slot < MAX_BLACKBOX_SLOTS ? slot : 0;   // the original does no range check
    g_slot_gold[gs] += gold1000 ? 0x3E8 : 0x1F4;
    g_event_flags[202] = 0;                                // byte_8A86A = 0
    state_.team_gold = g_slot_gold[gs];                    // dword_C5D9E
    fprintf(stderr, "CHP: sub_10975 reward: team %d slot %u +%d gold -> %u, leader flag[201]=%u\n",
            team, slot, gold1000 ? 1000 : 500, g_slot_gold[gs], g_event_flags[201]);
}

// sub_22A5E: time limit check.
//   now   = word_C5D8A * 220 + byte_C5EA3
//   limit = word_C5DD5 == 0 ? word_C5DD2 * 220 + byte_C5DD4 + byte_C5DD7
//                           : word_C5DD5 * 220 + byte_C5DD7
//   When now >= limit (signed), sub_1FD09 draws the unk_809A4 box at (0x34,0x52)
//   and "TIME OVER!" at (0x78,0x5A), flips, waits for a click anywhere (sub_284AC),
//   creates .\cstldata\GAMEOVER (creat 0x180) and writes 2 bytes (the file
//   handle), then calls sub_204A7("EXIT").
// Returns true when the EXIT transition has started; the original never returns.
bool CHP::check_time_limit() {
    const int now = (int)state_.current_year * DAYS_PER_YEAR + state_.time_of_day;
    int limit;
    if (state_.limit_year == 0)
        limit = (int)state_.chapter_start_year * DAYS_PER_YEAR + state_.chapter_start_day + state_.limit_day;
    else
        limit = (int)state_.limit_year * DAYS_PER_YEAR + state_.limit_day;
    if (now < limit) return false;

    fprintf(stderr, "CHP: TIME OVER (now=%d limit=%d)\n", now, limit);
    uint8_t* d = video_.get_backbuffer();
    if (d) {
        if (banner_box_sprite_loaded_ && !banner_box_sprite_.empty())
            blit_sprite_rle(0x34, 0x52, banner_box_sprite_.data());
        draw_status_text(0x78, 0x5A, "TIME OVER!");
        status_flip();
    }
    wait_for_input();                                      // sub_284AC(0,0,0x13F,0xC7, ...): click anywhere

    std::string gp = cstldata_path("GAMEOVER");
    file_create_handle_flag(gp.c_str());                   // the original writes 2 bytes from the buffer holding the handle; the value is unused
    do_transition("EXIT");
    return true;
}

// sub_20912 loc_20B23 and sub_21476 loc_2184A, right after a JUMP passes the time
// limit: for each party record (unk_A48B8, word_C5D84 entries) copy dword[+0x4C]
// to dword[+0x48] and word[+0x52] to word[+0x50], restoring HP/MP to maximum.
void CHP::restore_party_hp_mp() {
    const int n = (int)(party_recs_.size() / 0x88);
    for (int i = 0; i < n; i++) {
        uint8_t* r = party_rec(i);
        memcpy(r + 0x48, r + 0x4C, 4);
        memcpy(r + 0x50, r + 0x52, 2);
    }
}

// ==== Mini font and sprite font (sub_17F88, sub_17FB2, sub_17E6A, sub_1802C, sub_18047) ====

// sub_1802C: set the mini font foreground colour.
void CHP::set_mini_font_color(uint8_t color) {
    mini_font_color_ = color;
}

// sub_17E6A: draw one glyph of the 8x7 mini font.
void CHP::draw_mini_char(int x, int y, char c, uint8_t color) {
    if (c < 0x20 || c > 0x7E) return;
    int idx = (uint8_t)c - 0x20;
    int width = g_mini_font_widths[idx];
    const uint8_t* glyph = &g_mini_font_data[idx * 8];

    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;

    // sub_17E6A: row 0 to 6 (7 rows)
    for (int row = 0; row < 7; row++) {
        int py = y + row;
        if (py < 0 || py >= SCREEN_H) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < width; col++) {
            int px = x + col;
            if (px >= 0 && px < SCREEN_W) {
                if (bits & (0x80 >> col)) {
                    dest[py * SCREEN_W + px] = color;
                }
            }
        }
    }
}

// sub_17FB2: draw a string in the mini font.
void CHP::draw_mini_string(int x, int y, const char* str, uint8_t color) {
    if (!str) return;
    int cur_x = x;
    while (*str) {
        char c = *str++;
        if (c < 0x20 || c > 0x7E) continue;
        int idx = (uint8_t)c - 0x20;
        int width = g_mini_font_widths[idx];
        draw_mini_char(cur_x, y, c, color);
        cur_x += width;
    }
}

// sub_17F88: print an integer in decimal with the mini font.
void CHP::draw_mini_number(int x, int y, int value, uint8_t color) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    draw_mini_string(x, y, buf, color);
}

// sub_18047: draw an ASCII string with the FONT.SPR sprite font (variable width).
void CHP::draw_sprite_font(int x, int y, const char* str, int font_group) {
    if (!str || !*str) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;

    int cur_x = x;
    const char* p = str;

    // 1. First glyph (sub_18047..loc_180D7).
    char c0 = *p++;
    char dl = c0;
    if (dl >= 'a' && dl <= 'z') {
        dl -= 0x20; // the first glyph is drawn as uppercase
    }

    if (dl >= 'A' && dl <= 'Z') {
        if (font_sprites_loaded_) {
            // Uppercase sprite: font_group * 53 + (dl - 'A').
            int sprite_idx = font_group * 53 + (dl - 'A');
            const SpriteEntry* sp = font_sprites_.get(sprite_idx);
            if (sp && sp->pixel_data) {
                if (cur_x > 0 && cur_x < SCREEN_W && y > 0 && y < SCREEN_H) {
                    blit_sprite_rle(cur_x, y + sp->y_offset, sp->pixel_data);
                }
                // loc_180CA: mov edx, [edi-2]; sar edx, 10h -> word0, the sprite's own advance.
                int w = (int)(int16_t)sp->word0;
                cur_x += (w > 0) ? w : 6;
            } else {
                int idx = (uint8_t)dl - 0x20;
                draw_mini_char(cur_x, y, dl, 0x01);
                cur_x += (idx >= 0 && idx < 95) ? g_mini_font_widths[idx] : 6;
            }
        } else {
            int idx = (uint8_t)dl - 0x20;
            draw_mini_char(cur_x, y, dl, 0x01);
            cur_x += (idx >= 0 && idx < 95) ? g_mini_font_widths[idx] : 6;
        }
    } else {
        cur_x += 4;
    }

    // 2. Remaining glyphs (loc_180E8..loc_1818E).
    while (*p) {
        char ch = *p++;
        char c = ch;
        if (c >= 'A' && c <= 'Z') {
            c += 0x20; // from the second glyph on, uppercase maps to lowercase
        }

        if (c >= 'a' && c <= 'z') {
            if (font_sprites_loaded_) {
                // Lowercase sprite: font_group * 53 + 27 + (c - 'a').
                int sprite_idx = font_group * 53 + 27 + (c - 'a');
                const SpriteEntry* sp = font_sprites_.get(sprite_idx);
                if (sp && sp->pixel_data) {
                    if (cur_x > -5 && cur_x < SCREEN_W - 5 && y > 0 && y < SCREEN_H) {
                        blit_sprite_rle(cur_x, y + sp->y_offset, sp->pixel_data);
                    }
                    // loc_18167: mov edx, [edi-2]; sar edx, 10h -> word0, the sprite's own advance.
                    int w = (int)(int16_t)sp->word0;
                    cur_x += (w > 0) ? w : 6;
                } else {
                    int idx = (uint8_t)c - 0x20;
                    draw_mini_char(cur_x, y, c, 0x01);
                    cur_x += (idx >= 0 && idx < 95) ? g_mini_font_widths[idx] : 6;
                }
            } else {
                int idx = (uint8_t)c - 0x20;
                draw_mini_char(cur_x, y, c, 0x01);
                cur_x += (idx >= 0 && idx < 95) ? g_mini_font_widths[idx] : 6;
            }
        } else if (c == '.') {
            if (font_sprites_loaded_) {
                // Period sprite: font_group * 53 + 26.
                int sprite_idx = font_group * 53 + 26;
                const SpriteEntry* sp = font_sprites_.get(sprite_idx);
                if (sp && sp->pixel_data) {
                    if (cur_x > -5 && cur_x < SCREEN_W - 5 && y > 0 && y < SCREEN_H) {
                        blit_sprite_rle(cur_x, y + sp->y_offset, sp->pixel_data);
                    }
                    int w = (int)(int16_t)sp->word0;
                    cur_x += (w > 0) ? w : 4;
                } else {
                    draw_mini_char(cur_x, y, c, 0x01);
                    cur_x += 4;
                }
            } else {
                draw_mini_char(cur_x, y, c, 0x01);
                cur_x += 4;
            }
        } else {
            // loc_18186: advance for a space or any other character.
            cur_x += 6;
        }
    }
}

// ==== Time display (sub_22C8E: loc_230E2..loc_23253) ====

void CHP::render_time_display() {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;

    int mx = input_.get_mouse_x();

    // sub_22C8E: cmp dword_8FF24, 0A0h (160) selects the left or right position.
    int box_x = 0;
    int year_x = 0;
    int day_x = 0;
    int box_y = 2;   // ebp = 2
    int year_y = 30; // 0x1E
    int day_y = 41;  // 0x29

    if (mx > 160) {
        // Mouse on the right half: draw at the top left.
        box_x = 0;
        year_x = 27; // 0x1B
        day_x = 30;  // 0x1E
    } else {
        // Mouse on the left half: draw at the top right (loc_23129).
        box_x = 244; // 0xF4
        year_x = 271; // 0x10F
        day_x = 274;  // 0x112
    }

    // 1. Background sprite 4 of TIME_GRA.SYF (sub_1D8D5).
    if (time_sprites_loaded_) {
        const SpriteEntry* bg_sp = time_sprites_.get(4);
        if (bg_sp && bg_sp->pixel_data) {
            blit_sprite_transparent(box_x, box_y, bg_sp->pixel_data);
        }
    }

    // 2. Year (word_C5D8A), sub_17F88.
    draw_mini_number(year_x, year_y, (int)state_.current_year, 0x01);

    // 3. Day of year (byte_C5EA3), sub_17F88 (loc_23161).
    draw_mini_number(day_x, day_y, (int)state_.time_of_day, 0x01);

    // 4. Days Remain box: POINT.SPR entry 9, sub_1D874 at (1, 178).
    if (point_sprites_loaded_) {
        const SpriteEntry* remain_sp = point_sprites_.get(9);
        if (remain_sp && remain_sp->pixel_data) {
            blit_sprite_rle(1, 178, remain_sp->pixel_data);
        }
    }

    // 5. Days remaining, sub_2D2F4 at (8, 184). The original computes
    //    (word_C5DD5*220 + byte_C5DD7) - (word_C5D8A*220 + byte_C5EA3) and prints the
    //    signed value with itoa, without clamping to 0. Unlike sub_22A5E, this display
    //    does not fall back to the chapter start when word_C5DD5 is 0.
    int limit_total_days = (int)state_.limit_year * DAYS_PER_YEAR + state_.limit_day;
    int cur_total_days = (int)state_.current_year * DAYS_PER_YEAR + state_.time_of_day;
    int days_remain = limit_total_days - cur_total_days;

    char num_buf[32];
    snprintf(num_buf, sizeof(num_buf), "%d", days_remain);
    font_.set_text_mode(4, 0x1000, 0xFF);
    font_.draw_string(dest, SCREEN_W, SCREEN_H, 8, 184, num_buf);

    // 6. "Days Remain" sprite text, sub_18047 at (35, 191), group 2.
    draw_sprite_font(35, 191, "Days Remain", 2);
}

// ==== sub_204A7: module transition ====
void CHP::do_transition(const char* command) {
    fprintf(stderr, "CHP: Module transition -> '%s'\n", command);

    // ASM: on "EXIT" skip the zoom/fade/sound transition and go straight to the
    // save and exit path (loc_2062E).
    if (strcmp(command, "EXIT") != 0) {
        zoom_in_effect();
    }

    save_blackbox();
    write_config();
    save_contibak();

    std::string dollar = game_path(IPC::DOLLAR);
    FILE* fp = fopen(dollar.c_str(), "wt");
    if (fp) { fprintf(fp, "%s", command); fclose(fp); }

    strncpy(state_.pending_command, command, sizeof(state_.pending_command) - 1);
    state_.pending_command[sizeof(state_.pending_command) - 1] = 0;
    state_.transition_pending = true;
}

// ==== Dialogue system: sub_2B1BD (dialog), sub_2AAD3 (choices), sub_2BBA0 (narration) ====

namespace {
inline bool is_hangul_lead_byte(uint8_t b) { return b >= 0x80; }   // sub_2A91C
constexpr int kDlgFrameX   = 0x1E;   // frame blit X (fixed)
constexpr int kDlgShadeX   = 0x24;   // shade X (fixed)
constexpr int kDlgTextX    = 0x37;   // body text X (fixed)
constexpr uint16_t kColName = 0x0100;
constexpr uint16_t kColBody = 0x1000;
}

void CHP::draw_text_char(int x, int y, const char* ch, uint16_t color) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !ch || !ch[0]) return;
    font_.set_text_mode(4, color, 0xFF);
    font_.draw_string(dest, SCREEN_W, SCREEN_H, x, y, ch);
}

bool CHP::wait_for_input() {
    // Test hook, same convention as combatii's auto_advance_: G2_AUTOADVANCE
    // proceeds without waiting, for offscreen regression sweeps.
    static const bool auto_advance = (getenv("G2_AUTOADVANCE") != nullptr);
    if (auto_advance) return true;
    if (dos_exit_) return false;
    if (!GameTimer::wait_for_press(input_, 0)) { dos_exit_ = true; return false; }
    return true;
}

// sub_1D83A x retraces: approximate a 70 Hz vertical retrace wait.
void CHP::flip_wait(int retraces) {
    if (retraces <= 0) return;
    if (!GameTimer::delay_with_pump(input_, (uint32_t)(retraces * 1000 / 70))) dos_exit_ = true;
}

// sub_1CAB0: replace every pixel whose mask byte is not 0xFF with a dark palette
// entry: new = 7 - (R+G+B) / 0x25, using the 6-bit VGA value of the screen pixel.
void CHP::render_dialog_shade(int x, int y) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || dialog_shade_mask_.size() < 4) return;
    const int w = dialog_shade_mask_[0] | (dialog_shade_mask_[1] << 8);
    const int h = dialog_shade_mask_[2] | (dialog_shade_mask_[3] << 8);
    if ((size_t)w * h + 4 > dialog_shade_mask_.size()) return;
    const uint8_t* mask = dialog_shade_mask_.data() + 4;
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            if (mask[row * w + col] == 0xFF) continue;
            uint8_t& px = dest[dy * SCREEN_W + dx];
            int sum = (int)g_palette[px][0] + g_palette[px][1] + g_palette[px][2];
            px = (uint8_t)(7 - sum / 0x25);
        }
    }
}

// Shade, frame and portrait (sub_2B1BD loc_2B26F..loc_2B31B / loc_2B35C..loc_2B41B).
// upper means the party icon is in the top half (y < 100), so the box opens below it.
void CHP::draw_dialog_chrome(int portrait_id, bool upper) {
    const int frame_y = upper ? 0x69 : 1;
    render_dialog_shade(kDlgShadeX, upper ? 0x72 : 0x0A);
    if (!dialog_frame_rle_.empty()) blit_sprite_rle(kDlgFrameX, frame_y, dialog_frame_rle_.data());

    uint16_t w = 0, h = 0;
    std::vector<uint8_t> rle;
    std::string path = game_path("PORTRAIT.SYF");
    if (!file_exists(path.c_str())) path = game_path("portrait.syf");
    if (portrait_id >= 0 && portrait_load(path, portrait_id, w, h, rle) && !rle.empty()) {
        int px = (party_sx_ > 0xA0) ? 8 : (0x138 - (int)w);
        int py = (upper ? 0x73 : 0xC7) - (int)h;
        blit_sprite_rle(px, py, rle.data());
    }
}

// sub_2B1BD: dialog box with portrait, speaker name and TALK.TBL body text.
int CHP::run_talk(int portrait_id, const char* name, int talk_id) {
    if (dos_exit_) return 0;
    static const bool auto_advance = (getenv("G2_AUTOADVANCE") != nullptr);

    std::string raw;
    std::string path = game_path("TALK.TBL");
    if (!file_exists(path.c_str())) path = game_path("talk.tbl");
    if (!talktbl_load_raw(path, talk_id, raw)) {
        fprintf(stderr, "CHP: warning: TALK.TBL record %d not found\n", talk_id);
        return 0;
    }
    std::string body;
    std::vector<std::string> choices;
    talktbl_split_choices(raw, body, choices);
    choices.erase(std::remove_if(choices.begin(), choices.end(),
                                 [](const std::string& c) { return c.empty(); }), choices.end());
    std::vector<TalkPage> pages = talktbl_paginate_npc(body);

    const bool upper = party_sy_ < 100;                      // loc_2B238: dword_A250B+2 < 0x64
    const int frame_y = upper ? 0x69 : 1;
    const int text_base = frame_y + 0x3F;                    // var_1C
    std::string nm = name ? name : "";
    if (nm.size() > 26) nm.resize(26);

    // Save the background once (sub_1DA10 -> unk_DA370) before the first page.
    uint8_t* screen = video_.get_backbuffer();
    std::vector<uint8_t> bg(screen, screen + SCREEN_W * SCREEN_H);
    auto restore_bg = [&]() { memcpy(video_.get_backbuffer(), bg.data(), bg.size()); };

    // If a click is still held from the previous page, release then press again to
    // advance (byte_1127AA).
    bool hold_flag = true;
    auto poll = [&](bool& pressed, bool& held) {
        input_.update();
        if (input_.quit_requested()) dos_exit_ = true;
        pressed = input_.was_left_clicked() || input_.is_key_pressed(SDLK_RETURN) ||
                  input_.is_key_pressed(SDLK_SPACE);
        held = input_.is_left_button();
    };

    int result = 0;
    for (size_t pi = 0; pi < pages.size() && !dos_exit_; pi++) {
        restore_bg();
        draw_dialog_chrome(portrait_id, upper);
        const TalkPage& page = pages[pi];

        if (pi == 0) {   // First row: speaker name, colour 0x0100, X = 0x37 + 8.
            draw_text_char(kDlgTextX + 8, text_base - 0x30, nm.c_str(), kColName);
            video_.present();
        }
        int row_y = (pi == 0) ? text_base - 0x20 : text_base - 0x30;
        for (const std::string& line : page.lines) {
            int x = kDlgTextX;
            size_t i = 0;
            while (i < line.size() && !dos_exit_) {
                uint8_t c = (uint8_t)line[i];
                if (c == 0) break;
                char buf[4] = {0};
                if (is_hangul_lead_byte(c)) {
                    buf[0] = (char)c; buf[1] = (i + 1 < line.size()) ? line[i + 1] : 0;
                    i += 2;
                    draw_text_char(x, row_y, buf, kColBody);
                    x += 16;
                } else {
                    buf[0] = (char)c; i++;
                    draw_text_char(x, row_y, buf, kColBody);
                    x += 8;
                }
                video_.present();
                if (auto_advance) continue;
                bool pressed, held;
                poll(pressed, held);
                // loc_2B5BF: skip the wait only on a fresh press after the flag cleared.
                bool wait = (!pressed && !held) || hold_flag;
                if (wait) flip_wait(3);
                if (!pressed && !held) hold_flag = false;
            }
            row_y += 16;
        }
        if (dos_exit_) break;
        video_.present();

        // End of page: wait for a new click (loc_2BA86).
        if (!auto_advance) {
            hold_flag = true;
            while (!dos_exit_) {
                bool pressed, held;
                poll(pressed, held);
                if (!pressed && !held) hold_flag = false;
                if ((pressed || held) && !hold_flag) break;
                flip_wait(1);
            }
        }
    }

    if (!dos_exit_ && !choices.empty())
        result = run_talk_choice(choices, portrait_id, upper);

    restore_bg();
    video_.present();
    return result;
}

// sub_2AAD3: list up to four choices in a four-row grid; hover highlights (0x0100)
// and a click confirms. The original is mouse-only; this port also accepts Up/Down
// and Enter. Returns the 0-based index.
int CHP::run_talk_choice(const std::vector<std::string>& choices, int portrait_id, bool upper) {
    static const bool auto_advance = (getenv("G2_AUTOADVANCE") != nullptr);
    const int n = (int)std::min<size_t>(4, choices.size());
    if (n <= 0) return 0;
    if (auto_advance) return 0;

    const int frame_y = upper ? 0x69 : 1;
    const int text_base = frame_y + 0x3F;
    std::vector<uint8_t> bg(video_.get_backbuffer(), video_.get_backbuffer() + SCREEN_W * SCREEN_H);

    int selected = -1;
    int last_drawn = -2;
    bool wait_release = true;   // the click that turned the page must not pick a choice
    while (!dos_exit_) {
        input_.update();
        if (input_.quit_requested()) { dos_exit_ = true; break; }

        // Hover: row i spans [base-0x30+16i, base-0x20+16i]; higher rows win because
        // the ASM checks 4 down to 1.
        int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
        if (mx >= kDlgTextX && mx <= kDlgTextX + 0xD0) {
            for (int i = n - 1; i >= 0; i--) {
                int top = text_base - 0x30 + i * 16;
                if (my >= top && my <= top + 16) { selected = i; break; }
            }
        }
        if (input_.is_key_pressed(SDLK_UP))   selected = (selected <= 0) ? 0 : selected - 1;
        if (input_.is_key_pressed(SDLK_DOWN)) selected = (selected < 0) ? 0 : std::min(selected + 1, n - 1);

        if (selected != last_drawn) {
            memcpy(video_.get_backbuffer(), bg.data(), bg.size());
            draw_dialog_chrome(portrait_id, upper);
            for (int i = 0; i < n; i++)
                draw_text_char(kDlgTextX, text_base - 0x30 + i * 16, choices[i].c_str(),
                               (i == selected) ? kColName : kColBody);
            video_.present();
            last_drawn = selected;
        }

        bool held = input_.is_left_button();
        if (!held) wait_release = false;
        bool confirm = (input_.was_left_clicked() && !wait_release && selected >= 0) ||
                       (input_.is_key_pressed(SDLK_RETURN) && selected >= 0);
        if (confirm) break;
        flip_wait(1);
    }
    return selected < 0 ? 0 : selected;
}

// sub_2BBA0: scroll TALK.TBL body text vertically through a six-row window (20 px
// apart). Five blank rows (130 bytes) are prepended and the text rises 1 px per
// frame (3 retraces); rows outside the window (y < 20 and 130..149) are restored
// from the saved background.
void CHP::run_narration(int talk_id) {
    if (dos_exit_) return;
    std::string raw;
    std::string path = game_path("TALK.TBL");
    if (!file_exists(path.c_str())) path = game_path("talk.tbl");
    if (!talktbl_load_raw(path, talk_id, raw)) {
        fprintf(stderr, "CHP: warning: TALK.TBL record %d (narration) not found\n", talk_id);
        return;
    }
    std::string body;
    std::vector<std::string> ignored;
    talktbl_split_choices(raw, body, ignored);

    const int len = (int)body.size();
    std::string text(130, ' ');
    text += body;
    text.append(26 * 8, ' ');

    uint8_t* screen = video_.get_backbuffer();
    std::vector<uint8_t> bg(screen, screen + SCREEN_W * SCREEN_H);

    static const bool auto_advance = (getenv("G2_AUTOADVANCE") != nullptr);
    int pos = 0, off = 0;
    while (pos < len + 0x8C && !dos_exit_) {
        uint8_t* d = video_.get_backbuffer();
        memcpy(d, bg.data(), bg.size());
        for (int k = 0; k < 6; k++) {
            int idx = pos + 26 * k;
            if (idx + 26 > (int)text.size()) break;
            std::string row = text.substr(idx, 26);
            size_t z = row.find('\0');
            if (z != std::string::npos) row.resize(z);
            if (row.find_first_not_of(' ') == std::string::npos) continue;
            draw_text_char(0x32, 0x0A + off + 20 * k, row.c_str(), kColBody);
        }
        memcpy(d, bg.data(), 0x1900);                        // restore rows 0..19
        memcpy(d + 0xA280, bg.data() + 0xA280, 0x1900);      // restore rows 130..149
        if (!auto_advance) flip_wait(3);
        video_.present();

        int prev = off;
        off = prev - 1;
        if (prev == 0) { off = 20; pos += 26; }
        input_.update();
        if (input_.quit_requested()) dos_exit_ = true;
    }
    memcpy(video_.get_backbuffer(), bg.data(), bg.size());
    video_.present();
}

// ==== Portrait system ====

void CHP::load_portrait(int id) {
    // Shared loader with PACK (portrait_load); ids 0..84 verified against real data,
    // including short reads.
    std::string path = game_path("PORTRAIT.SYF");
    if (!file_exists(path.c_str())) path = game_path("portrait.syf");

    portrait_loaded_ = false;
    portrait_width_ = 0;
    portrait_height_ = 0;
    uint16_t w = 0, h = 0;
    std::vector<uint8_t> rle;
    if (!portrait_load(path, id, w, h, rle)) return;
    const size_t n = std::min(rle.size(), sizeof(portrait_large_));
    memset(portrait_large_, 0, sizeof(portrait_large_));
    memcpy(portrait_large_, rle.data(), n);
    portrait_width_ = w;
    portrait_height_ = h;
    portrait_loaded_ = true;
}

void CHP::load_small_portrait(int id) {
    std::string path = game_path("PORTSMAL.SYF");
    if (!file_exists(path.c_str())) path = game_path("portsmal.syf");

    memset(portrait_small_, 0, sizeof(portrait_small_));
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return;

    fseek(fp, 0x80, SEEK_SET);
    fseek(fp, id * 4, SEEK_CUR);
    uint32_t offset = 0;
    fread(&offset, 4, 1, fp);
    fseek(fp, offset + 4, SEEK_SET);

    fseek(fp, 8, SEEK_CUR);
    fread(portrait_small_, 1, 0x2710, fp);
    fclose(fp);
}

void CHP::blit_sprite(int x, int y, const uint8_t* data) {
    blit_sprite_rle(x, y, data);
}

// sub_1D874: RLE sprite blitter.
void CHP::blit_sprite_rle(int x, int y, const uint8_t* data) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !data) return;

    int cur_dest_offset = y * SCREEN_W + x;
    const uint8_t* src = data;

    uint16_t chunk_count = src[0] | (src[1] << 8);
    src += 2;

    for (int c = 0; c < chunk_count; c++) {
        uint16_t skip = src[0] | (src[1] << 8);
        uint8_t dword_count = src[2];
        uint8_t byte_count = src[3];
        src += 4;

        cur_dest_offset += skip;
        int total_bytes = dword_count * 4 + byte_count;

        for (int b = 0; b < total_bytes; b++) {
            int dest_idx = cur_dest_offset + b;
            if (dest_idx >= 0 && dest_idx < SCREEN_W * SCREEN_H) {
                dest[dest_idx] = src[b];
            }
        }
        cur_dest_offset += total_bytes;
        src += total_bytes;
    }
}

// sub_1D8D5: 2D sprite blitter with 0xFF transparency.
void CHP::blit_sprite_transparent(int x, int y, const uint8_t* data) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !data) return;

    uint16_t width = data[0] | (data[1] << 8);
    uint16_t height = data[2] | (data[3] << 8);
    const uint8_t* src = data + 4;

    for (int row = 0; row < height; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < width; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            uint8_t pixel = src[row * width + col];
            if (pixel != 0xFF) {
                dest[dy * SCREEN_W + dx] = pixel;
            }
        }
    }
}

void CHP::free_portrait() {
    portrait_loaded_ = false;
    portrait_width_ = 0;
    portrait_height_ = 0;
}

// ==== Equipment ====

void CHP::load_item_data() {
    std::string path = game_path("ITEM.DAT");
    if (!file_exists(path.c_str())) path = game_path("item.dat");

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "CHP: warning: ITEM.DAT not found\n");
        return;
    }

    for (int i = 0; i < MAX_ITEMS; i++) {
        if (fread(item_data_[i], 1, ITEM_SIZE, fp) < ITEM_SIZE) break;
    }
    fclose(fp);
    items_loaded_ = true;
}

bool CHP::load_spr(SPRParser& parser, const char* subdir, const char* filename, bool is_citizen) {
    std::string path;
    if (subdir && strlen(subdir) > 0 && strcmp(subdir, ".") != 0) {
        path = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/" + subdir, filename);
    }
    if (path.empty() || !file_exists(path.c_str())) {
        path = file_find_case_insensitive(GAME_DATA_DIR, filename);
    }
    if (path.empty() || !file_exists(path.c_str())) {
        path = chapdata_path(filename);
        if (path.empty() || !file_exists(path.c_str())) {
            path = cstldata_path(filename);
        }
    }
    if (path.empty() || !file_exists(path.c_str())) {
        fprintf(stderr, "CHP: warning: %s not found\n", filename);
        return false;
    }
    return parser.load(path, is_citizen);
}

void CHP::load_item_sprites() {
    item_sprites_loaded_ = load_spr(item_sprites_, "CSTLDATA", "CITIZEN2.SPR", true);
}

// ==== Party records (sub_15A7F / sub_15AE5 / sub_15B40) ====
// sub_15A7F copies every record of the 0x7B-entry character array (unk_8FF5C,
// 0x88 bytes each) whose team byte (+0x87, byte_8FFE3) equals the current team
// (dword_1271E4+2) into unk_A48B8 and stores the count in word_C5D84. sub_15AE5
// copies records with team byte 0x63 into the reserve list unk_940B4. sub_15B40
// writes party records back to slot (id-1) and stamps them with the current team.
namespace {
constexpr int kRecSize = 0x88;
constexpr uint8_t kReserveTeam = 0x63;
// Character record field offsets, confirmed against unk_A48B8 accesses in CHP.ASM.
constexpr int R_NAME = 0x02, R_ID = 0x0F, R_FACE = 0x10, R_CLASS = 0x11, R_LEVEL = 0x2B;
constexpr int R_SKILL = 0x2E;              // +0x2E..+0x33: six skill icon bytes
constexpr int R_MAG0 = 0x38, R_MAG1 = 0x39, R_MAG2 = 0x3A, R_MAG3 = 0x3B;
constexpr int R_EXPB = 0x3D;
constexpr int R_STAT_W0 = 0x44, R_STAT_W1 = 0x46;   // word
constexpr int R_STAT_B0 = 0x40, R_STAT_B1 = 0x41, R_STAT_B2 = 0x42, R_STAT_B3 = 0x43;
constexpr int R_HP = 0x48, R_HPMAX = 0x4C;          // dword, divided by 100 when drawn
constexpr int R_MP = 0x50, R_MPMAX = 0x52;          // word
constexpr int R_TOWN_SPR = 0x61;                    // word: CITIZEN2.SPR group number
constexpr int kEquipOff[5] = { 0x5B, 0x5A, 0x5D, 0x5E, 0x5F };   // equip slots, left to right
constexpr int kEquipX[5]   = { 0xA2, 0xC0, 0xDF, 0xFC, 0x11A };
inline uint16_t rd16(const uint8_t* r, int o) { return (uint16_t)(r[o] | (r[o + 1] << 8)); }
inline int32_t  rd32(const uint8_t* r, int o) { return (int32_t)(r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24)); }
}

void CHP::party_rebuild(int team_arg) {
    party_recs_.clear();
    reserve_recs_.clear();
    // sub_15A7F uses dword_1271E4+2 (the team/gold slot, blackbox_slot_).
    // team_active (byte_C5EA1) only drives exec_flag tracking and is unrelated to the party.
    const int team = team_arg < 0 ? (int)blackbox_slot_ : team_arg;
    for (int i = 0; i < MAX_CHARACTERS; i++) {
        const uint8_t* r = reinterpret_cast<const uint8_t*>(&g_characters[i]);
        if (r[CHR_TEAM_OFFSET] == team) party_recs_.insert(party_recs_.end(), r, r + kRecSize);
    }
    for (int i = 0; i < MAX_CHARACTERS; i++) {
        const uint8_t* r = reinterpret_cast<const uint8_t*>(&g_characters[i]);
        if (r[CHR_TEAM_OFFSET] == kReserveTeam) reserve_recs_.insert(reserve_recs_.end(), r, r + kRecSize);
    }
    state_.party_count = (uint16_t)(party_recs_.size() / kRecSize);
}

void CHP::party_writeback() {
    // sub_15B40 writes dword_1271E4+2 (blackbox_slot_) back into the team byte.
    const int n = (int)(party_recs_.size() / kRecSize);
    for (int i = 0; i < n; i++) {
        uint8_t* r = party_rec(i);
        r[CHR_TEAM_OFFSET] = (uint8_t)blackbox_slot_;
        const int id = r[R_ID] - 1;
        if (id >= 0 && id < MAX_CHARACTERS) memcpy(&g_characters[id], r, kRecSize);
    }
    const int m = (int)(reserve_recs_.size() / kRecSize);
    for (int i = 0; i < m; i++) {
        uint8_t* r = reserve_recs_.data() + (size_t)i * kRecSize;
        r[CHR_TEAM_OFFSET] = kReserveTeam;
        const int id = r[R_ID] - 1;
        if (id >= 0 && id < MAX_CHARACTERS) memcpy(&g_characters[id], r, kRecSize);
    }
}

void CHP::vm_team_rebuild() {
    fprintf(stderr, "  VM_TEAM_REBUILD\n");
    party_rebuild();
}

// ==== Primitives (sub_1DB05 / sub_1DAD7 / sub_1DB9B / sub_1C6F6 / sub_1C5BC / sub_33A6C) ====
// sub_1DB05: horizontal line. Skipped when y is off-screen; a negative x shortens
// the length and starts at 0; a length below 1 is skipped.
void CHP::gfx_hline(int x, int y, int len, uint8_t c) {
    uint8_t* d = video_.get_backbuffer();
    if (!d || y < 0 || y >= SCREEN_H) return;
    if (x < 0) { len += x; x = 0; }
    if (len < 1) return;
    for (int i = 0; i < len && x + i < SCREEN_W; i++) d[y * SCREEN_W + x + i] = c;
}
// sub_1DAD7: vertical line.
void CHP::gfx_vline(int x, int y, int len, uint8_t c) {
    uint8_t* d = video_.get_backbuffer();
    if (!d || x < 0 || x >= SCREEN_W) return;
    for (int i = 0; i < len; i++) {
        const int yy = y + i;
        if (yy >= 0 && yy < SCREEN_H) d[yy * SCREEN_W + x] = c;
    }
}
// sub_1DB9B: filled rectangle, skipped when w <= 0 or h <= 0.
void CHP::gfx_fill(int x, int y, int w, int h, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) gfx_hline(x, y + r, w, c);
}
// sub_1C6F6: bevel box. Top and left use byte_7B46E (light), bottom and right use
// byte_7B46F (dark), and the interior uses byte_7B470, with W = x2-x1, H = y2-y1:
//   top (x1,y1) length W; left (x1,y1) length H;
//   bottom (x1,y2) length W; right (x2,y1) length H+1;
//   interior (x1+1,y1+1) size (W-1) x (H-1).
void CHP::draw_bevel_box(int x1, int y1, int x2, int y2, uint8_t fill) {
    const int W = x2 - x1, H = y2 - y1;
    gfx_hline(x1, y1, W, kChpFrameLight);
    gfx_vline(x1, y1, H, kChpFrameLight);
    gfx_hline(x1, y2, W, kChpFrameDark);
    gfx_vline(x2, y1, H + 1, kChpFrameDark);
    gfx_fill(x1 + 1, y1 + 1, W - 1, H - 1, fill);
}
// sub_1C5BC: single-colour outline: top (x1+1,y1) length W-1, bottom (x1,y2) length W,
// left (x1,y1) length H, right (x2,y1) length H+1.
void CHP::draw_outline_rect(int x1, int y1, int x2, int y2, uint8_t color) {
    const int W = x2 - x1, H = y2 - y1;
    gfx_hline(x1 + 1, y1, W - 1, color);
    gfx_hline(x1, y2, W, color);
    gfx_vline(x1, y1, H, color);
    gfx_vline(x2, y1, H + 1, color);
}
// sub_33A6C: copy a width x height block within the 320-pixel buffer (width is
// rounded down to a multiple of 4). src and dst point at the first copied pixel;
// writes that fall outside the buffer are dropped instead of touching the previous
// row's tail as the original would.
void CHP::gfx_copy_rect(const uint8_t* src, uint8_t* dst, int width, int height) {
    const int w = (width / 4) * 4;
    if (w <= 0 || height <= 0) return;
    const uint8_t* base = video_.get_backbuffer();
    const uint8_t* end = base + (size_t)SCREEN_W * SCREEN_H;
    for (int r = 0; r < height; r++) {
        const uint8_t* s = src + (size_t)r * SCREEN_W;
        uint8_t* d = dst + (ptrdiff_t)r * SCREEN_W;
        for (int i = 0; i < w; i++) {
            if (d + i < base || d + i >= end) continue;
            d[i] = s[i];
        }
    }
}

// sub_2D2F4: print Johab or ASCII text at (x, y). Colour state is global in the
// original, so the default mode (yellow with a black outline) is used here.
void CHP::draw_status_text(int x, int y, const char* s) {
    uint8_t* d = video_.get_backbuffer();
    if (!d || !s) return;
    font_.set_text_mode(4, 0x1000, 0xFF);
    font_.draw_string(d, SCREEN_W, SCREEN_H, x, y, s);
}

void CHP::status_flip() {            // sub_28C8B / sub_1DA67 / sub_28C49: hide cursor, copy, present
    video_.present();
}
void CHP::status_flip_vsync() {      // sub_1DA88: present after waiting for the retrace
    video_.present();
    flip_wait(1);
}

// ==== Mouse and scroll helpers ====
// sub_28A06: fetch one event. With an empty queue it returns the current button
// state (1 = left, 2 = right, 0 = none) and cursor position.
void CHP::status_poll() {
    if (status_poll_hook_ && status_poll_hook_(status_ev_)) return;
    GameTimer::delay_with_pump(input_, 8);
    input_.update();
    if (input_.quit_requested()) dos_exit_ = true;
    const MouseState& m = input_.get_mouse();
    status_ev_.x = m.x;
    status_ev_.y = m.y;
    status_ev_.code = (m.left_button || m.left_clicked) ? 1 : ((m.right_button || m.right_clicked) ? 2 : 0);
}
// sub_2E6CF: wait until the left button is released (reads it once first).
void CHP::status_wait_left_release() {
    do { status_poll(); } while (status_ev_.code == 1 && !dos_exit_);
}
// sub_2E720: wait until the right button is released.
void CHP::status_wait_right_release() {
    do { status_poll(); } while (status_ev_.code == 2 && !dos_exit_);
}
// sub_2EDE2: thumb offset -> top row: top = off * (count-5) / (123-h), 0 when h == 123.
// The original divides in x87 extended precision, so the same order is kept in
// long double.
int CHP::status_scroll_to_top(int off, int h, int count) {
    if (h == 0x7B) return 0;
    long double a = (long double)(0x7B - h);
    long double q = a / (long double)off;               // off == 0 gives +inf and a result of 0
    long double r = (long double)(count - 5) / q;
    return (int)r;
}
// sub_2EE28: top row -> thumb offset: off = 123 / (count / top), 0 when top == 0.
int CHP::status_top_to_scroll(int top, int count) {
    long double q = (long double)count / (long double)top;
    long double r = (long double)0x7B / q;
    return (int)r;
}

// ==== Status assets, read from CHP.EXE VAs ====
//   off_529CC icon table, 23840 bytes; unk_89FF4 slot; unk_89A90 and unk_89D42 buttons;
//   off_2E0D2 gold window; off_2E414 hand cursor.
bool CHP::status_load_assets() {
    if (status_assets_loaded_) return true;
    exe_resource_load(ExeId::Chp, 0x529CC, ExeResLength::Fixed, status_icons_, 23840);
    exe_resource_load(ExeId::Chp, 0x89FF4, ExeResLength::Rle, status_slot_);
    exe_resource_load(ExeId::Chp, 0x89A90, ExeResLength::Rle, status_btn_a_);
    exe_resource_load(ExeId::Chp, 0x89D42, ExeResLength::Rle, status_btn_b_);
    exe_resource_load(ExeId::Chp, 0x2E0D2, ExeResLength::Rle, status_gold_);
    exe_resource_load(ExeId::Chp, 0x2E414, ExeResLength::Sprite, status_hand_);
    if (dialog_frame_rle_.empty() || dialog_shade_mask_.empty()) load_extracted_sprites();
    status_snap_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    status_prev_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    status_assets_loaded_ = true;
    return true;
}

const uint8_t* CHP::status_icon(int slot) const {
    if (slot < 0) return nullptr;
    size_t off;
    if (slot < kChpStatusIconCount) off = (size_t)slot * kChpStatusIconStride;
    else return nullptr;
    return off < status_icons_.size() ? status_icons_.data() + off : nullptr;
}

// Equipment icon: sub_1FDC4 pre-applies [+2] += 0x10, [+4] += 0x0C and [+6] = -[+2]
// to every item.spr sprite. Item number k (1-based) is table entry k-1
// (unk_9F612[k*4] == dword_9F616[(k-1)*4]).
void CHP::status_item_icon(int x_base, int item) {
    const SpriteEntry* sp = item_box_sprites_.get(item - 1);
    if (!sp || !sp->pixel_data) return;
    const int16_t h2 = (int16_t)(sp->word2 + 0x10);
    const int16_t xo = (int16_t)(sp->x_offset + 0x0C);
    const int16_t yo = (int16_t)(-h2);
    blit_sprite_rle((int16_t)(x_base + xo), (int16_t)(0x8F + yo), sp->pixel_data);
}

// sub_2DD68 (with palette) and sub_2DC95 (image only): load a PCX at 320x200.
bool CHP::status_load_pcx(const char* name, bool with_palette, std::vector<uint8_t>& dst) {
    dst.assign((size_t)SCREEN_W * SCREEN_H, 0);
    std::string path = file_find_case_insensitive(GAME_DATA_DIR, name);
    if (path.empty() || !file_exists(path.c_str())) path = game_path(name);
    uint8_t keep[256][3];
    if (!with_palette) palette_get_range(0, 255, keep);
    const bool ok = !path.empty() && file_exists(path.c_str()) &&
                    pcx_load_to_buffer(path, dst.data(), SCREEN_W, SCREEN_H);
    if (!with_palette) palette_set_range(0, 255, keep);
    if (!ok) fprintf(stderr, "CHP: error: cannot read %s\n", name);
    return ok;
}

// ==== sub_2E9BC: gold window (RLE frame plus right-aligned number) ====
void CHP::status_draw_gold(int x, int y) {
    if (!status_gold_.empty()) blit_sprite_rle(x, y, status_gold_.data());
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)state_.team_gold);
    const int len = (int)strlen(buf);
    draw_mini_number(x + 0x23 - 5 * len, y + 4, (int)state_.team_gold, mini_font_color_);
}

// ==== sub_30169: party list on the left (six rows, 0x18 apart) ====
void CHP::status_draw_party_list(int top, int sel) {
    const int count = state_.party_count;
    for (int row = 0; row < 6; row++) {
        const int idx = top + row;
        const int y0 = 4 + 0x18 * row;
        if (idx == sel) draw_bevel_box(6, y0, 0x86, 0x1C + 0x18 * row);
        if (idx >= count) continue;
        const uint8_t* rec = party_rec(idx);
        // Town sprite: first frame of that CITIZEN2.SPR group
        // (dword_9E34E[word+0x61]), drawn at (0x0A, row top) with no offset.
        const SpriteEntry* sp = item_sprites_.get(rd16(rec, R_TOWN_SPR));
        if (sp && sp->pixel_data) blit_sprite_rle(0x0A, y0, sp->pixel_data);
        draw_status_text(0x26, 5 + 0x18 * row, (const char*)rec + R_NAME);
        const int cls = rec[R_CLASS];
        const char* cn = cls < kChpClassNameCount ? kChpClassNames[cls] : "";
        const int half = (int)strlen(cn) >> 1;
        draw_mini_string(0x4E - 6 * half, 0x16 + 0x18 * row, cn, mini_font_color_);
    }
}

// Scroll thumb: only the first draw (loc_2F16C) omits the offset. Width 0x87..0x96.
void CHP::status_draw_thumb(bool with_offset) {
    const int h = (int)status_thumb_h_;
    const int off = with_offset ? status_thumb_off_ : 0;
    draw_bevel_box(0x87, 0x0F + off, 0x96, h + 0x0F + off);
}

// ==== sub_2EA3D: redraw the status window body ====
void CHP::status_draw_main(int sel, int top) {
    uint8_t* bb = video_.get_backbuffer();
    memcpy(bb, status_snap_.data(), status_snap_.size());          // sub_1DABB(DA370 -> C5EA4)
    status_draw_party_list(top, sel);
    if (sel < 0 || sel >= (int)state_.party_count) return;
    const uint8_t* rec = party_rec(sel);

    for (int k = 0; k < 5; k++) {                                   // five equip slots
        const int item = rec[kEquipOff[k]];
        if (item) status_item_icon(kEquipX[k], item);
    }

    char buf[48];
    set_mini_font_color(0x1B); draw_mini_string(0xE1, 0x21, "HP:", mini_font_color_);
    set_mini_font_color(0x21); draw_mini_string(0xE1, 0x2A, "MP:", mini_font_color_);
    set_mini_font_color(0x11); draw_mini_string(0xDC, 0x33, "LV:     EXP:", mini_font_color_);
    set_mini_font_color(0x01);
    snprintf(buf, sizeof(buf), "%d / %d", rd32(rec, R_HP) / 100, rd32(rec, R_HPMAX) / 100);
    draw_mini_string(0xF8, 0x21, buf, mini_font_color_);
    snprintf(buf, sizeof(buf), "%d / %d", (int)rd16(rec, R_MP), (int)rd16(rec, R_MPMAX));
    draw_mini_string(0xF8, 0x2A, buf, mini_font_color_);
    snprintf(buf, sizeof(buf), "%d      %d", (int)rec[R_LEVEL], (int)rec[R_EXPB]);
    draw_mini_string(0xF3, 0x33, buf, mini_font_color_);

    draw_status_text(0xDC, 0x0B, (const char*)rec + R_NAME);
    draw_mini_number(0xBC, 0x43, (int)(int16_t)rd16(rec, R_STAT_W0), mini_font_color_);
    draw_mini_number(0xEC, 0x43, rec[R_STAT_B0], mini_font_color_);
    draw_mini_number(0x11A, 0x43, rec[R_STAT_B1], mini_font_color_);
    draw_mini_number(0xBC, 0x57, (int)(int16_t)rd16(rec, R_STAT_W1), mini_font_color_);
    draw_mini_number(0xEC, 0x57, rec[R_STAT_B2], mini_font_color_);
    draw_mini_number(0x11A, 0x57, rec[R_STAT_B3], mini_font_color_);
    status_draw_gold(0xBD, 0x86);

    load_small_portrait(rec[R_FACE]);                               // sub_1FA10, then sub_1D874 at (0xA2, 9)
    blit_sprite_rle(0xA2, 9, portrait_small_);
}

// ==== loc_2F387..loc_2F859: skill and magic grid ====
// Visible only while the button is held; the equipment screen differs only in x0.
// Draw the six-cell frame (unk_89FF4), fill it with the non-zero entries of the six
// skill bytes (+0x2E..), then put the first non-zero magic byte among
// +0x38/+0x3A/+0x39/+0x3B and its value in the next cell.
void CHP::status_draw_skill_grid(const uint8_t* rec, int x0) {
    if (!status_slot_.empty())
        for (int i = 0; i < 6; i++)
            blit_sprite_rle(x0 + 48 * (i % 3), 0x3D + 21 * (i / 3), status_slot_.data());
    int n = 0;
    for (int k = 0; k < 6; k++) {
        const int b = rec[R_SKILL + k];
        if (b <= 0) continue;
        const uint8_t* ic = status_icon(2 * b + 0x14 + 4 * k);
        if (ic) blit_sprite_rle(x0 + 2 + 48 * (n % 3), 0x3E + 21 * (n / 3), ic);
        n++;
    }
    const int bx = x0 + 2 + 48 * (n % 3), by = 0x3E + 21 * (n / 3);
    const int nx = x0 + 0x1C + 48 * (n % 3), ny = 0x44 + 21 * (n / 3);
    static const int kMagOff[4]  = { R_MAG0, R_MAG2, R_MAG1, R_MAG3 };
    static const int kMagSlot[4] = { 12, 14, 16, 18 };
    for (int m = 0; m < 4; m++) {
        const int v = rec[kMagOff[m]];
        if (v <= 0) continue;
        const uint8_t* ic = status_icon(kMagSlot[m]);
        if (ic) blit_sprite_rle(bx, by, ic);
        draw_mini_number(nx, ny, v, mini_font_color_);
        break;
    }
}

// ==== sub_32ABA: item stats (7 signed bytes at ITEM.DAT +0x28..+0x2E) ====
void CHP::status_item_stats(int item) {
    if (item < 1 || item > MAX_ITEMS) return;
    const uint8_t* it = item_data_[item - 1];
    int v[7];
    for (int i = 0; i < 7; i++) v[i] = (int8_t)it[0x28 + i];       // 0x80 or above reads as negative
    static const int kIcX[4] = { 0x46, 0x78, 0xAA, 0xDC };
    static const int kIcSlot[4] = { 0, 2, 4, 6 };
    for (int i = 0; i < 4; i++) {
        const uint8_t* ic = status_icon(kIcSlot[i]);
        if (ic) blit_sprite_rle(kIcX[i], 0x55, ic);
    }
    const SpriteEntry* s9 = item_box_sprites_.get(9);               // dword_9F63A
    const SpriteEntry* s84 = item_box_sprites_.get(84);             // dword_9F766
    if (s9 && s9->pixel_data) blit_sprite_rle(0x46, 0x69, s9->pixel_data);
    if (s84 && s84->pixel_data) blit_sprite_rle(0x78, 0x69, s84->pixel_data);
    if (const uint8_t* ic = status_icon(10)) blit_sprite_rle(0xAA, 0x69, ic);
    static const int kNumX[4] = { 0x5F, 0x91, 0xC3, 0xF5 };
    for (int i = 0; i < 4; i++) draw_mini_number(kNumX[i], 0x5A, v[i], mini_font_color_);
    for (int i = 0; i < 3; i++) draw_mini_number(kNumX[i], 0x6E, v[4 + i], mini_font_color_);
}

// ==== sub_2EE3F: right-click an equip icon -> expanding rectangle and item popup ====
void CHP::status_item_popup(int item) {
    uint8_t* bb = video_.get_backbuffer();
    int x1 = status_ev_.x, y1 = status_ev_.y, x2 = x1, y2 = y1;
    const int dx1 = (0x24 - x1) / 10, dy1 = (0x3B - y1) / 10;
    const int dx2 = (0x118 - x1) / 10, dy2 = (0x82 - y1) / 10;
    memcpy(status_prev_.data(), bb, status_prev_.size());          // sub_1DABB(C5EA4 -> EB4E0)
    for (int i = 0; i < 10; i++) {
        if (i > 0) draw_outline_rect(x1, y1, x2, y2, 1);
        status_flip_vsync();
        x1 += dx1; y1 += dy1; x2 += dx2; y2 += dy2;
    }
    memcpy(bb, status_prev_.data(), status_prev_.size());
    if (!dialog_shade_mask_.empty()) blit_sprite_transparent(0x24, 0x3B, dialog_shade_mask_.data());
    if (!dialog_frame_rle_.empty()) blit_sprite_rle(0x1E, 0x32, dialog_frame_rle_.data());
    if (item >= 1 && item <= MAX_ITEMS) {
        const char* name = (const char*)item_data_[item - 1] + 1;
        const int len = (int)strlen(name);
        draw_status_text(0xA0 - (len / 2) * 8, 0x41, name);
    }
    status_item_stats(item);
    status_flip();
    status_wait_left_release();
    do { status_poll(); } while (status_ev_.code != 1 && !dos_exit_);
    status_wait_left_release();
    memcpy(bb, status_prev_.data(), status_prev_.size());
    status_flip();
}

// ==== sub_2E8FC: message popup, closes on a left click ====
void CHP::status_message(const char* msg) {
    status_wait_left_release();
    const int len = (int)strlen(msg);
    const int tx = 0x96 - 5 * len + 0x1E;
    draw_bevel_box(tx - 4, 0x55, tx + 8 * len + 4, 0x78, 0x0B);  // sub_1D266(0x0B): fill 0x0B, restored to 0x0D afterwards
    draw_status_text(tx, 0x5F, msg);
    status_flip();
    do { status_poll(); } while (status_ev_.code != 1 && !dos_exit_);
    status_wait_left_release();
}

// The two sub-screens (sub_128DA class screen, sub_30AFE equipment screen) live in
// chp_class.cpp and chp_equip.cpp.

// Slide back from a sub-screen: redraw the status window into DA370 and push the new
// screen in from the right (inventory) or left (equipment) over the old one (EB4E0),
// as sub_2F09A does at loc_2FE06 and loc_2FF9F.
void CHP::status_slide_back(int sel, bool from_inventory) {
    uint8_t* bb = video_.get_backbuffer();
    status_load_pcx("status.pcx", false, status_snap_);
    status_draw_main(sel, status_top_);
    status_draw_thumb(true);
    memcpy(status_snap_.data(), bb, status_snap_.size());      // DA370 = the newly drawn status window
    if (from_inventory) {
        int s = -2, e = 0, w = 0x9E;
        do {
            gfx_copy_rect(status_prev_.data(), bb, 0xA0, 0x94);
            const int rem = s % 4;
            gfx_copy_rect(status_snap_.data(), bb + 0x9E - e, rem + w, 0x94);
            s += 2; w += 2;
            status_flip_vsync();
            e += 2;
        } while (e <= 0xA0);
    } else {
        int s = 0, w = 0xA0, e = 0;
        do {
            gfx_copy_rect(status_prev_.data() + 0xA0, bb + 0xA0, 0xA0, 0x94);
            const int rem = s % 4;
            gfx_copy_rect(status_snap_.data() + 0xA0 - e, bb, rem + w, 0x94);
            s += 2; w += 2; e += 2;
            status_flip_vsync();
        } while (s <= 0xA0);
    }
    status_load_pcx("status.pcx", false, status_snap_);
}

// ==== sub_2F09A: right-click status window ====
// Follows the original branch structure (loop loc_2F1CA, redraw loc_30011, exit
// loc_300FB): right click on an equip slot opens the item popup; left click selects
// a party row; holding the skill button shows the skill/magic grid; the thumb and
// arrows scroll the list; the bottom-left and bottom-right buttons open the
// inventory and equipment screens; the bottom-right corner closes the window.
void CHP::show_status_window() {
    if (!items_loaded_) load_item_data();
    if (!item_sprites_loaded_) load_item_sprites();
    if (!item_box_sprites_loaded_) item_box_sprites_loaded_ = load_spr(item_box_sprites_, ".", "ITEM.SPR", false);   // sub_1FDC4
    status_load_assets();
    party_rebuild();                                    // sync the party copy; the VM calls sub_15A7F

    int sel = 0;                                        // var_14
    int flag = 0;                                       // var_18, value returned by a sub-screen
    uint8_t* bb = video_.get_backbuffer();
    auto count = [&]() { return (int)state_.party_count; };

    // dword_1271CC: thumb length = 123 / (count / 6), and 123 below six members.
    if (count() < 6) status_thumb_h_ = 123.0f;
    else status_thumb_h_ = (float)((long double)kChpStatusFlt51D78 / ((long double)count() / (long double)kChpStatusFlt51D74));

    status_wait_right_release();                        // sub_2E720
    status_top_ = 0;                                    // dword_1271B8 (dword_1271BC too)
    palette_get_range(0, 255, status_save_pal_);        // sub_1DDC4 -> word_A3D4C
    if (!status_load_pcx("status.pcx", true, status_snap_)) {
        palette_set_range(0, 255, status_save_pal_);
        return;
    }
    status_draw_main(sel, status_top_);
    status_draw_thumb(false);
    status_flip();

    bool exit_all = false;
    auto in = [&](int x0, int x1, int y0, int y1) {
        return status_ev_.x >= x0 && status_ev_.x <= x1 && status_ev_.y >= y0 && status_ev_.y <= y1;
    };
    auto redraw = [&]() {
        status_draw_main(sel, status_top_);
        status_draw_thumb(true);
        status_flip();
    };
L_loop:                                                 // loc_2F1CA
    if (dos_exit_) goto L_finish;
    if (status_ev_.code == 2) {                         // right button: equip slot popup
        if (in(0xA2, 0x135, 0x67, 0x81)) {
            const int idx = (status_ev_.x - 0xA2) / 0x1E;
            if (idx <= 4 && sel >= 0 && sel < count()) {
                const int item = party_rec(sel)[kEquipOff[idx]];
                if (item) status_item_popup(item);
            }
        }
        redraw();                                       // loc_2F261
        status_wait_left_release();
    }
    if (status_ev_.code != 1 && flag == 0) goto L_after_sub; // loc_2F2D2
    flag = 0;                                           // loc_2F2ED
    if (in(8, 0x88, 5, 0x93)) {                         // click inside the party list
        const int t = status_top_ + (status_ev_.y - 4) / 0x18;
        sel = t;
        if (count() <= t) sel = count() - 1;
    }
    if (in(0xFC, 0x135, 0x87, 0x91)) {                  // skill grid button, while held
        if (sel >= 0 && sel < count()) status_draw_skill_grid(party_rec(sel), 0xA6);
        if (!status_btn_a_.empty()) blit_sprite_rle(0xFC, 0x87, status_btn_a_.data());
        status_flip();
        do { status_poll(); } while (status_ev_.code == 1 && !dos_exit_);
    }
    // loc_2F8AB: scrollbar area (x 0x87..0x98).
    if (status_ev_.x >= 0x87 && status_ev_.x <= 0x98) {
        const int yy = 0x0F + status_thumb_off_;
        if (yy <= status_ev_.y && !((long double)status_ev_.y > (long double)yy + (long double)status_thumb_h_)) {
            // Drag the thumb: horizontal motion is blocked (sub_28D1D), only vertical counts.
            int grab = status_ev_.y;                    // var_1C
            blit_sprite_transparent(status_ev_.x, status_ev_.y, status_hand_.data());
            for (;;) {                                  // loc_2F947
                status_flip();
                if (status_ev_.code != 1 || dos_exit_) break;
                status_poll();
                if (grab < status_ev_.y) {              // dragging down
                    const int d = status_ev_.y - grab;
                    status_thumb_off_ += d;
                    grab += d;
                    const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
                    if (fv > (long double)kChpStatusFlt51D78) {
                        const float fvf = (float)fv;
                        grab = (int)((long double)grab - ((long double)fvf + (long double)kChpStatusFlt51D7C));
                        status_thumb_off_ = (int)((long double)kChpStatusFlt51D78 - (long double)status_thumb_h_);
                    }
                    status_top_ = status_scroll_to_top(status_thumb_off_, (int)status_thumb_h_, count());
                    if (count() == 9 && status_top_ == 4) status_top_ = 3;
                } else if (grab > status_ev_.y) {       // dragging up
                    const int d = grab - status_ev_.y;
                    status_thumb_off_ -= d;
                    grab -= d;
                    if (status_thumb_off_ < 0) status_thumb_off_ = 0;
                    status_top_ = status_scroll_to_top(status_thumb_off_, (int)status_thumb_h_, count());
                }
                status_draw_main(sel, status_top_);     // loc_2FACC
                status_draw_thumb(true);
                blit_sprite_transparent(status_ev_.x, status_ev_.y, status_hand_.data());
            }
            goto L_redraw;                              // loc_2FB48 -> loc_30011
        }
    }
    if (in(0x87, 0x98, 5, 0x0F)) {                      // up arrow
        if (status_top_ > 0) status_top_--;
        goto L_arrow;
    }
    if (in(0x87, 0x98, 0x8A, 0x93)) {                   // down arrow
        if (count() - 6 > status_top_) status_top_++;
        goto L_arrow;
    }
    if (in(0x87, 0x98, 0x0F, 0x8A)) {                   // click on the scrollbar track
        status_thumb_off_ = status_ev_.y - 0x0F;
        const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
        if (fv > (long double)kChpStatusFlt51D78)
            status_thumb_off_ = (int)((long double)kChpStatusFlt51D78 - (long double)status_thumb_h_);
        status_top_ = status_scroll_to_top(status_thumb_off_, (int)status_thumb_h_, count());
        goto L_redraw;
    }
    if (in(0, 0x4E, 0x94, 0xA3)) {                      // inventory screen button (bottom left)
        status_flip();
        memcpy(status_prev_.data(), bb, status_prev_.size());
        const int r = status_inventory_screen(sel, status_top_);
        flag = r;
        if (r == 0) goto L_exit_btn;
        if (r == -1) { status_message(kChpStatusErrMsg); goto L_redraw; }
        status_slide_back(sel, true);
        goto L_after_sub;
    }
    if (in(0xEE, 0x13F, 0x94, 0xA3)) {                  // equipment screen button (bottom right)
        status_flip();
        memcpy(status_prev_.data(), bb, status_prev_.size());
        const int r = status_equipment_screen(sel);
        flag = r;
        if (r == 0) goto L_exit_btn;
        status_slide_back(sel, false);
        goto L_after_sub;
    }
    goto L_redraw;
L_arrow:                                                // loc_2FB92
    status_thumb_off_ = status_top_to_scroll(status_top_, count());
L_redraw:                                               // loc_30011
    if (flag == 0) {
        redraw();
        status_wait_left_release();
    }
L_after_sub:                                            // loc_3008D
    if (flag == 0) status_poll();
    if (in(0xFC, 0x135, 0xBB, 0xC5) && status_ev_.code == 1) goto L_exit_btn;
    goto L_loop;

L_exit_btn:                                             // loc_300FB: show the pressed close button, then finish
    if (!status_btn_b_.empty()) blit_sprite_rle(0xFC, 0xBB, status_btn_b_.data());
    status_flip();
    exit_all = true;
L_finish:
    party_writeback();                                  // sub_15B40
    status_wait_left_release();
    memset(bb, 0, (size_t)SCREEN_W * SCREEN_H);         // sub_1DB9B(full-screen fill with 0)
    video_.present();
    palette_set_range(0, 255, status_save_pal_);        // sub_1DD0D
    (void)exit_all;
}

void CHP::save_blackbox() {
    // sub_15BD9 runs sub_15B40 (slot-based writeback) first, then stores the gold of
    // dword_1271E4 (high word = slot) into dword_125030[slot].
    int cur_slot = blackbox_slot_ < MAX_BLACKBOX_SLOTS ? blackbox_slot_ : 0;
    g_slot_gold[cur_slot] = state_.team_gold;

    std::string p = game_path(IPC::BLACKBOX_DAT);
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) { fprintf(stderr, "CHP: error: cannot write blackbox.dat\n"); return; }
    uint8_t b; uint16_t w;

    // sub_15BD9: word_C5D99 (1 byte) + word_C5D9B day (2 bytes) + byte_C5D9D (1 byte).
    // Only the low byte of word_C5D99 is written, as in the original (ebx = 1); PACK
    // shares the same offsets.
    b = (uint8_t)(state_.quest_counter & 0xFF);
    fwrite(&b, 1, 1, f);

    w = (uint16_t)((state_.current_year - BASE_YEAR) * DAYS_PER_YEAR + state_.time_of_day);
    fwrite(&w, 1, 2, f);

    b = state_.secondary_state;
    fwrite(&b, 1, 1, f);
    fwrite(g_event_flags, 1, 0x4E20, f);
    // File+0x4E24 (2 bytes) is dword_1271E4+2, the team/gold slot, not the chapter;
    // the value read at load time is written back unchanged.
    w = blackbox_slot_;
    fwrite(&w, 1, 2, f);
    for (int i = 0; i < MAX_CHARACTERS; i++) fwrite(&g_characters[i], 1, sizeof(CharacterData), f);

    for (int s = 0; s < MAX_BLACKBOX_SLOTS; s++) {
        fwrite(&g_slot_gold[s], 1, 4, f);
        fwrite(&g_slot_item_count[s], 1, 2, f);
        for (int it = 0; it < g_slot_item_count[s] && it < BLACKBOX_SLOT_ITEMS; it++) {
            const uint8_t stored = (uint8_t)(g_slot_items[s][it] - 1);   // file stores 1-byte ids (sub_15BD9)
            fwrite(&stored, 1, 1, f);
        }
    }
    fclose(f);
    fprintf(stderr, "CHP: save_blackbox() completed\n");
}

void CHP::save_contibak() {
    std::string p = chapdata_path(IPC::CONTIBAK_TMP);
    FILE* fp = fopen(p.c_str(), "wb");
    if (!fp) return;

    uint8_t hdr[0x40] = {0};
    fwrite(hdr, 1, 0x40, fp);

    for (int ch = 0; ch < MAX_CHAPTERS; ch++) {
        fwrite(all_chapter_points_[ch], 1, CONTIBAK_CHAPTER_STRIDE, fp);
        for (int p_idx = 0; p_idx < MAX_PATHS_PER_CHAPTER; p_idx++)
            fwrite(all_chapter_paths_[ch][p_idx], 1, CONTIBAK_PATH_STRIDE, fp);
    }

    // Start from the 0xCD block read at load time and overwrite only the fields CHP
    // updates, so limit_year/day and the travel effect option survive a save.
    uint8_t state_block[0xCD];
    memcpy(state_block, contibak_block_, sizeof(state_block));
    state_block[0] = (uint8_t)(state_.chapter_start_year & 0xFF);
    state_block[1] = (uint8_t)(state_.chapter_start_year >> 8);
    state_block[2] = state_.chapter_start_day;
    state_block[3] = (uint8_t)(state_.limit_year & 0xFF);
    state_block[4] = (uint8_t)(state_.limit_year >> 8);
    state_block[5] = state_.limit_day;
    state_block[8] = state_.travel_style;
    fwrite(state_block, 1, 0xCD, fp);

    uint8_t b = state_.chapter_id; fwrite(&b, 1, 1, fp);
    b = state_.current_point; fwrite(&b, 1, 1, fp);
    uint16_t w = state_.current_year; fwrite(&w, 2, 1, fp);
    b = state_.time_of_day; fwrite(&b, 1, 1, fp);
    b = state_.secondary_state; fwrite(&b, 1, 1, fp);
    b = state_.team_active; fwrite(&b, 1, 1, fp);

    for (int i = 0; i < QUEST_COUNT; i++)
        fwrite(g_quest_objectives_[i].name, 1, QUEST_NAME_LEN, fp);
    for (int i = 0; i < QUEST_COUNT; i++)
        fwrite(&g_script_markers_[i], 2, 1, fp);

    for (int i = 0; i < MAX_EVENT_TRIGGERS; i++) {
        fwrite(&g_event_triggers_[i], 1, TRIGGER_STRUCT_SIZE, fp);
        fwrite(g_script_data_[i], 1, VM_SCRIPT_RECORD_SIZE, fp);
    }

    fclose(fp);
    fprintf(stderr, "CHP: save_contibak() completed\n");
}

void CHP::write_config() {
    std::string p = game_path(IPC::GAME_CFG);
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    fwrite(state_.config_data, 1, 10, f);
    fclose(f);
}

void CHP::chapter_menu_tool() {}

void CHP::transition_exit(const char* cmd) {
    // Write "$" and set the flag; run() unwinds with a normal exit code. Video and
    // Audio are closed by gene2.cpp.
    std::string dp = game_path(IPC::DOLLAR);
    FILE* fp = fopen(dp.c_str(), "wt");
    if (fp) { fprintf(fp, "%s", cmd); fclose(fp); }
    dos_exit_ = true;
}

// ==== Chapter and quest name tables (sub_10ADF / sub_10C6D) ====
// Both are extracted from the CHP.EXE data segment (file offsets):
//   name strings: 0x485C1, 20-byte stride, 54 entries (rows 0..53)
//   chapter ids (byte_528ED at linear 0x528ED): 0x48AED, 4-byte stride
// sub_10ADF and sub_10C6D index them by row (shared with quest_state) as
// row = page*3 + 0..2, i.e. three rows per page over 18 pages. Strings stay in the
// original Johab encoding for Font::draw_string.
struct ChapterNameEntry {
    int chapter_number;   // 0 marks an unused slot (byte_528ED[row])
    const char* name;     // Johab bytes, "" when the row has no name
};

constexpr int CHAPTER_NAME_TABLE_SIZE = 57;
constexpr int CHAPTER_SELECT_ROWS_PER_PAGE = 3;
constexpr int CHAPTER_SELECT_LAST_PAGE = 18;                            // var_8 starts at 0x12
constexpr int CHAPTER_SELECT_PAGE_COUNT = CHAPTER_SELECT_LAST_PAGE + 1; // 19 pages (0..18)

// dword_52835 (CHP.EXE file 0x48A35, 46 dwords): chapter number -> quest row.
//   sub_10C6D loc_10D86 starts at page var_C = dword_52835[byte_C5EA1] / 3, and
//   sub_112E0, sub_10010 and sub_10975 read the same table.
constexpr int CHAPTER_QUEST_ROW_COUNT = 46;
static const int32_t g_chapter_quest_row[CHAPTER_QUEST_ROW_COUNT] = {
    0, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 21, 22, 23, 24,
    27, 28, 30, 33, 34, 36, 37, 38, 39, 0, 40, 42, 43, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
};

static const ChapterNameEntry g_chapter_names[CHAPTER_NAME_TABLE_SIZE] = {
    { 1, "\xD2\x41\xAC\x82\xB7\x81\x20\x9D\x41\xB7\xA5\xB8\xE1" }, // row 0
    { 2, "\xB5\xD7\x8A\x82\xB7\x81\x20\xB7\x41\xAC\x65" }, // row 1
    { 0, "" }, // row 2
    { 3, "\xA6\x82\x8B\x62\x89\xD7\x28\x31\x29" }, // row 3
    { 4, "\xA6\x82\x8B\x62\x89\xD7\x28\x32\x29" }, // row 4
    { 5, "\x97\x61\x9C\x81\x89\xA5\x20\xAF\x69\x9D\x41\xB7\xA1\xB4\xE1" }, // row 5
    { 6, "\xC8\x81\xB4\xB7\xB7\x81\x20\xAF\xA5\xB8\xE5" }, // row 6
    { 7, "\xC4\xE1\xCB\xA1\xAF\x61\xB7\x81\x20\xB6\x77\xAC\x61\x97\x69" }, // row 7
    { 8, "\xAC\x61\xB7\xA1\x9C\xF3\xAF\x61\xB7\x81\x20\xB5\x61\x95\xA1" }, // row 8
    { 9, "\xB6\x41\x92\x85\xB7\x81\x20\xB6\x77\xA5\x77" }, // row 9
    { 10, "\xAF\xA9\xA4\xE1\xB4\x81\x9D\xA1\xB6\x81" }, // row 10
    { 0, "" }, // row 11
    { 12, "\xCE\x97\xB6\x85\xB7\x81\x20\xA3\xA9\xAC\x61" }, // row 12
    { 13, "\xCC\x81\xB8\x61\xB7\x81\x20\xB5\xD7\x89\xC5" }, // row 13
    { 14, "" }, // row 14: the original also has a chapter number but no name
    { 15, "\xAC\x61\xA0\x62\xCE\x97" }, // row 15
    { 16, "\xAC\xF7\xB5\xD7\xB7\x81\x20\x8B\xA9" }, // row 16
    { 0, "" }, // row 17
    { 17, "\xD0\x77\x9D\xA1\x88\x81\xC0\xE2" }, // row 18
    { 18, "\xAC\xF7\x88\xF1\xC8\x71\xAC\x82" }, // row 19
    { 0, "" }, // row 20
    { 19, "\xA7\x69\x9E\x81\xAF\xA1\x97\x61" }, // row 21
    { 20, "\xAF\xA5\x97\x69\xB7\x69\x20\xC0\x78\xB4\x61\xAC\xE1" }, // row 22
    { 0, "" }, // row 23
    { 22, "\xC0\xE5\x89\xB7\xB7\x81\x20\xB4\x61\xAC\xF7" }, // row 24
    { 0, "" }, // row 25
    { 0, "" }, // row 26
    { 23, "\xB5\x77\xB6\x97\xB8\xE5\xB8\x97" }, // row 27
    { 24, "\xA0\x61\x88\xF1\xAD\xA1\xD1\xC5" }, // row 28
    { 0, "" }, // row 29
    { 25, "\xB4\x71\xD3\x62\xAF\xA5" }, // row 30
    { 0, "" }, // row 31
    { 0, "" }, // row 32
    { 26, "\xCE\x97\xB6\x85\xB8\x81\x8B\xA1" }, // row 33
    { 27, "\xC8\x65\xAC\x97\x21\x20\xC4\xE1\xCB\xA1\xAF\x61\x89\xB7\xD1\xC1\x8A\x82" }, // row 34
    { 0, "" }, // row 35
    { 28, "\xB4\x71\xD3\x62\xAC\xF7\x20\xC8\x69\xD2\x41" }, // row 36
    { 29, "\xA7\xA1\xCF\x61\x9D\xA1\xAF\x61\xCB\x61\xB7\x81\x20\x90\x81\xB8\xE5" }, // row 37
    { 30, "\xC9\x51\xCF\x69\x9C\xE1\xBB\x61" }, // row 38
    { 31, "\x88\xF1\xB7\x65\x20\x8B\xB5\xA4\x69" }, // row 39
    { 33, "\xC8\x69\xC2\x89" }, // row 40
    { 0, "" }, // row 41
    { 34, "\xAF\xA9\xA4\xE1\xB4\x81\x9D\xA1\xB6\x81\x20\xB8\x81\xBB\xB3\x89\x69" }, // row 42
    { 35, "\x8B\x61\x9C\x61\xC9\x41\xAF\x61" }, // row 43
    { 0, "" }, // row 44
    { 36, "\xA0\xE1\xA0\x81\x93\xA1\xB4\x65\x20\xD0\x81\xB8\xE5" }, // row 45
    { 37, "\xC0\xE5\xD0\x61\xB9\x41\xB7\xA9" }, // row 46
    { 38, "\xD2\x41\xAC\x82\xB7\x81\x20\xB8\x65\xB5\x77" }, // row 47
    { 39, "\xAF\xA5\xA7\xA1\xB8\xE5\x94\x81" }, // row 48
    { 40, "\xCD\x77\xD1\xC1\xD1\x73\xB8\xF7" }, // row 49
    { 41, "\xAE\x81\xAD\xB7\xB8\x62\xB8\xE5" }, // row 50
    { 42, "\xB6\x85\xA1\x77\xB7\xA1\x20\xA6\x81\x9F\x61\x93\x65\x20\xAD\xA1\x9F\xA1" }, // row 51
    { 43, "\xB4\x61\xAF\x61\xA1\xA1\x95\x41\xB6\x81\xAF\x61" }, // row 52
    { 44, "\x90\x71\x89\x61\xBB\xA5\x20\x88\xF5\xB7\x65\x2F\x2F\x2F" }, // row 53
    { 45, "" }, // row 54: byte_528ED[54] = 45 but there is no name
    { 0, "" }, // row 55
    { 0, "" }, // row 56
};

// Name X coordinate, unchanged from the ASM.
// sub_10ADF and sub_10C6D count bytes up to the NUL with `repne scasb`, then do
// `shr ecx,1; shl ecx,3`, giving x = 0x96 - (strlen/2)*8. Johab characters are two
// bytes, so strlen/2 is the character count; the spaces around a name provide the
// centring (e.g. 12 bytes -> x = 0x96 - 48 = 102, width 96).
static int chapter_select_name_x(const char* s) {
    return 0x96 - ((int)strlen(s) >> 1) * 8;
}

// ==== sub_10ADF: compose one three-row page of the chapter select screen ====
// ASM (0x10ADF..0x10C6D): esi = page, edi = &aAmvBAaES[page * 0x14], the 20-byte
// stride name table. Row j = 0,1,2 maps to quest_state[page*3+j] with Y = 0x52, 0x66,
// 0x7A and name = edi + j*0x14. A row is skipped when its state is 0 or 3; state 2
// selects colour 0x0C0D with sub_2CDD8(4, 0x0C0D, 0xFF), then sub_2D2F4 prints the
// name at x = 0x96 - (strlen/2)*8, and sub_2CDD8(4, 0x1000, 0xFF) restores the
// default. State 1 is therefore 0x1000 and state 2 is 0x0C0D.
// The routine draws straight into the offscreen buffer (C5EA4); the caller copies
// the background (DA370) first, so dst already holds it here.
void CHP::chapter_select_compose_page(uint8_t* dst, int page) {
    if (!dst) return;
    static constexpr int ROW_Y[CHAPTER_SELECT_ROWS_PER_PAGE] = { 0x52, 0x66, 0x7A };
    for (int j = 0; j < CHAPTER_SELECT_ROWS_PER_PAGE; j++) {
        const int row = page * CHAPTER_SELECT_ROWS_PER_PAGE + j;
        if (row < 0 || row >= CHAPTER_NAME_TABLE_SIZE) continue;
        const uint8_t s = state_.quest_state[row];
        if (s == 0 || s == 3) continue;              // states 0 and 3 are not drawn
        const char* name = g_chapter_names[row].name;
        // The original's first row inherits whatever colour mode the previous screen
        // left behind, but later rows always restore 0x1000. This port always picks
        // the value from the state, which is deterministic.
        font_.set_text_mode(4, (s == 2) ? 0x0C0D : 0x1000, 0xFF);
        font_.draw_string(dst, SCREEN_W, SCREEN_H, chapter_select_name_x(name), ROW_Y[j], name);
        font_.set_text_mode(4, 0x1000, 0xFF);
    }
}

// ==== sub_10C6D: chapter (quest) select screen ====
// Flow (0x10C6D..0x112E0): up arrow dword_103BC (0x269B bytes) and down arrow
// off_10625 (0x268B) are loaded, page var_C and blink counter var_4 start at 0.
// mode == 1 highlights the previous chapter row with
// unk_8A872[dword_52835[byte_C5EA1]] = 2, then sub_112E0(mode) loads SCRIPT*.TBL.
// loc_10CF5 scans pages downward from 0x12 (three rows at a time) for a state of 1
// or 3 to find the last page. loc_10D47 walks rows 54 down to 0; if none is state 1,
// it returns 0x270F at once, which makes the caller run the ending. loc_10D86 starts
// at page dword_52835[byte_C5EA1] / 3.
// Background an02.pcx is loaded to DA370 and copied to C5EA4; pages cur and cur+1 are
// composed into C5C5C (cur at +0, cur+1 at +0xFA00) and the first is presented.
// Each frame (loc_10E3B) reads sub_28A06 into 8FF20/8FF24/8FF28, then scrolls down
// when var_C < var_8 and (y > 0xB4 or Down), scrolls up when var_C != 0 and
// (y < 0x14 or Up), and on a left click inside 0x64..0xC8 x 0x52..0x8E picks
// row = (y - 0x52) / 0x14, qrow = var_C*3 + row; when quest_state[qrow] is 1 it
// clears the screen, sets byte_C5EA1 = byte_528ED[qrow] and returns. It then copies
// C5C5C + ebp to C5EA4, redraws a hovered state-1 name in colour 0x0100, draws the
// arrows while var_4 < 0xF, presents, and advances var_4 (0x1E wraps to 0).
// chapter_select_scroll() handles the 100-frame, 2-row-per-frame animation.
int CHP::show_chapter_select(int mode) {
    fprintf(stderr, "CHP: sub_10C6D (mode=%d) chapter select screen\n", mode);

    // Mode == 1: mark the previous chapter's quest row as 2 (highlighted).
    //   ASM: xor eax,eax ; mov al, byte_C5EA1 ; mov eax, dword_52835[eax*4]
    //        ; mov byte ptr unk_8A872[eax], 2
    if (mode == 1) {
        const int ch = state_.team_active;                       // byte_C5EA1
        if (ch >= 0 && ch < CHAPTER_QUEST_ROW_COUNT) {
            const int row = g_chapter_quest_row[ch];
            if (row >= 0 && row < QUEST_STATE_SIZE) state_.quest_state[row] = 2;
        }
    }

    load_script_tables(mode);                                    // sub_112E0(mode)

    // loc_10CF5: last page (var_8).
    int max_page = CHAPTER_SELECT_LAST_PAGE;                     // var_8 = 0x12
    for (int row = CHAPTER_SELECT_LAST_PAGE * 3; ; row -= 3) {   // eax = 0x36
        bool found = false;
        for (int j = 0; j < CHAPTER_SELECT_ROWS_PER_PAGE && !found; j++) {
            const int r = row + j;
            if (r < 0 || r >= QUEST_STATE_SIZE) continue;
            const uint8_t s = state_.quest_state[r];
            if (s == 1 || s == 3) found = true;
        }
        if (found) break;
        --max_page;
        if (row - 3 <= -3) break;                                // cmp eax, -3 / jg
    }

    // loc_10D47: no in-progress (state 1) quest row at all returns 0x270F, the ending.
    //    unk_8A7A0[eax] == unk_8A872[eax - 0xD2], so this reads rows 54 down to 0.
    bool any_in_progress = false;
    for (int i = 54; i >= 0; --i) {
        if (i < QUEST_STATE_SIZE && state_.quest_state[i] == 1) { any_in_progress = true; break; }
    }
    if (!any_in_progress) {
        fprintf(stderr, "CHP: sub_10C6D - no quest in progress -> 0x270F (ending)\n");
        return 0x270F;
    }

    // loc_10D86: initial page var_C = dword_52835[byte_C5EA1] / 3.
    int page = 0;                                                // var_C
    {
        const int ch = state_.team_active;
        if (ch >= 0 && ch < CHAPTER_QUEST_ROW_COUNT) {
            const int row = g_chapter_quest_row[ch];
            if (row != 0) page = row / 3;                        // signed idiv by 3
        }
    }

    // Background and arrow assets.
    //   sub_2DD68("an02.pcx", unk_DA370): 320x200 plus palette
    std::vector<uint8_t> bg((size_t)SCREEN_W * SCREEN_H, 0);
    {
        std::string p = game_path("AN02.PCX");
        if (!file_exists(p.c_str())) p = game_path("an02.pcx");
        if (!pcx_load_to_buffer(p, bg.data(), SCREEN_W, SCREEN_H))
            fprintf(stderr, "CHP: warning: an02.pcx load failed\n");
    }
    // sub_1D74E: dword_103BC is the up arrow (0x269B bytes), off_10625 the down arrow (0x268B).
    std::vector<uint8_t> arrow_up, arrow_dn;
    exe_resource_load(ExeId::Chp, 0x103BC, ExeResLength::Fixed, arrow_up, 0x269);
    exe_resource_load(ExeId::Chp, 0x10625, ExeResLength::Fixed, arrow_dn, 0x268);

    // C5C5C: two 320x200 pages back to back (cur at +0, cur+1 at +0xFA00).
    // Scrolling copies this buffer to the screen in 0x280-byte (two-row) steps.
    constexpr size_t kPageBytes = (size_t)SCREEN_W * SCREEN_H;   // 0xFA00
    std::vector<uint8_t> pages(kPageBytes * 2);                  // dword_C5C5C (>= 0x1F400)

    auto compose = [&](int pg) {
        memcpy(pages.data(), bg.data(), kPageBytes);             // DA370 -> C5EA4
        chapter_select_compose_page(pages.data(), pg);           // sub_10ADF(pg)
        memcpy(pages.data() + kPageBytes, bg.data(), kPageBytes);
        chapter_select_compose_page(pages.data() + kPageBytes, pg + 1);
    };
    compose(page);

    uint8_t* screen = video_.get_backbuffer();
    if (!screen) return 0;
    memcpy(screen, pages.data(), kPageBytes);                    // sub_1DABB(C5C5C -> C5EA4)
    video_.present();                                            // sub_1DA88

    int scroll_off = 0;      // ebp: offset of the page image shown on screen
    int phase = 0;           // var_4: arrow blink counter (0..0x1E)
    bool have_arrows = (arrow_up.size() >= 2 && arrow_dn.size() >= 2);

    for (;;) {
        if (dos_exit_) return 0;
        input_.update();
        if (input_.quit_requested()) { transition_exit(IPC::EXIT); return 0; }

        // sub_28A06: event -> dword_8FF20 (button), 8FF24 (x), 8FF28 (y).
        const int mx = input_.get_mouse_x();
        const int my = input_.get_mouse_y();
        // The original tests the held button state every frame. The port uses the press
        // edge so one frame cannot select twice; it returns immediately on selection.
        const bool click = input_.was_left_clicked();
        const uint8_t* keys = SDL_GetKeyboardState(NULL);
        const bool key_up = keys[SDL_SCANCODE_UP] != 0;          // dword_9C638
        const bool key_dn = keys[SDL_SCANCODE_DOWN] != 0;        // dword_9C658

        // loc_10E3B: scroll down on y > 0xB4 or Down.
        if (page < max_page && (my > 0xB4 || key_dn)) {
            chapter_select_scroll(+1, page, pages, bg);
            ++page;
            scroll_off = (int)kPageBytes;                        // ebp = 0xFA00
        }
        // loc_10F6F: scroll up on y < 0x14 or Up.
        if (page != 0 && (my < 0x14 || key_up)) {
            chapter_select_scroll(-1, page, pages, bg);
            --page;
            scroll_off = 0;                                      // ebp = 0
        }

        // loc_11081: selection test, 0x64 < x < 0xC8, 0x52 < y < 0x8E and button 1.
        if (mx > 0x64 && mx < 0xC8 && my > 0x52 && my < 0x8E && click) {
            const int row = (my - 0x52) / 0x14;
            const int qrow = page * CHAPTER_SELECT_ROWS_PER_PAGE + row;
            if (qrow >= 0 && qrow < QUEST_STATE_SIZE && state_.quest_state[qrow] == 1) {
                const int ch = (qrow < CHAPTER_NAME_TABLE_SIZE)
                                   ? g_chapter_names[qrow].chapter_number : 0;
                state_.team_active = (uint8_t)ch;                // byte_C5EA1 = al
                // sub_1DB9B(0, 0, 0x140, 0xC8, 0, unk_A0000): clear the screen to black.
                memset(screen, 0, kPageBytes);
                video_.present();
                fprintf(stderr, "CHP: sub_10C6D -> chapter %d selected (quest row %d)\n", ch, qrow);
                return ch;
            }
        }

        // loc_11114: screen = C5C5C + ebp.
        memcpy(screen, pages.data() + scroll_off, kPageBytes);

        // Redraw a hovered in-progress (state 1) row in colour 0x0100.
        // ASM: y = (my-0x52)/0x14*0x14 + 0x52, x = 0x96 - (strlen/2)*8, colour 0x0100.
        {
            const int row = (my - 0x52) / 0x14;
            const int qrow = page * CHAPTER_SELECT_ROWS_PER_PAGE + row;
            if (qrow >= 0 && qrow < CHAPTER_NAME_TABLE_SIZE &&
                state_.quest_state[qrow] == 1 &&
                mx > 0x64 && mx < 0xC8 && my > 0x52 && my < 0x8E) {
                const char* name = g_chapter_names[qrow].name;
                font_.set_text_mode(4, 0x0100, 0xFF);
                font_.draw_string(screen, SCREEN_W, SCREEN_H,
                                  chapter_select_name_x(name), row * 0x14 + 0x52, name);
                font_.set_text_mode(4, 0x1000, 0xFF);
            }
        }

        // Arrows, drawn only while var_4 < 0xF.
        if (have_arrows && phase < 0x0F) {
            if (page != 0) blit_sprite_rle(0x7D, 1, arrow_up.data());        // sub_1D874(0x7D, 1)
            if (page < max_page) blit_sprite_rle(0x7D, 0xB6, arrow_dn.data()); // sub_1D874(0x7D, 0xB6)
        }

        video_.present();                                        // sub_1DA88
        phase = (phase == 0x1E) ? 0 : phase + 1;
        GameTimer::delay_with_pump(input_, 14);                  // approximate 70 Hz retrace
    }
}

// ==== sub_10C6D loc_10E85..loc_10F6F (down) and loc_10F93..loc_11016 (up) ====
// Recompose both pages into C5C5C (+0 and +0xFA00), then copy to the screen (C5EA4)
// in 0x280 (two-row) steps over 0xFA00/0x280 = 100 frames.
//   down: ebp goes 0 -> 0xF780, ending at 0xFA00 (new page at +0xFA00)
//   up  : ebp goes 0xFA00 -> 0x280, ending at 0 (new page at +0)
void CHP::chapter_select_scroll(int dir, int page, std::vector<uint8_t>& pages,
                                const std::vector<uint8_t>& bg) {
    constexpr int kPageBytes = SCREEN_W * SCREEN_H;   // 0xFA00
    constexpr int kStep = 0x280;                      // two rows

    // Down: C5C5C+0 holds page, C5C5C+0xFA00 holds page+1
    // up  : C5C5C+0 holds page-1, C5C5C+0xFA00 holds page
    const int first = (dir > 0) ? page : page - 1;
    memcpy(pages.data(), bg.data(), kPageBytes);
    chapter_select_compose_page(pages.data(), first);
    memcpy(pages.data() + kPageBytes, bg.data(), kPageBytes);
    chapter_select_compose_page(pages.data() + kPageBytes, first + 1);

    uint8_t* screen = video_.get_backbuffer();
    if (!screen) return;

    if (dir > 0) {
        memcpy(screen, pages.data(), kPageBytes);                 // sub_1DABB(C5C5C -> C5EA4)
        video_.present();                                         // sub_1DA88
        for (int off = 0; off < kPageBytes; off += kStep) {
            input_.update();
            if (input_.quit_requested()) { transition_exit(IPC::EXIT); return; }
            memcpy(screen, pages.data() + off, kPageBytes);       // sub_1DABB(C5C5C+off -> C5EA4)
            video_.present();
            GameTimer::delay_with_pump(input_, 14);
        }
    } else {
        memcpy(screen, pages.data() + kPageBytes, kPageBytes);    // sub_1DABB(C5C5C+0xFA00 -> C5EA4)
        video_.present();
        for (int off = kPageBytes; off > 0; off -= kStep) {
            input_.update();
            if (input_.quit_requested()) { transition_exit(IPC::EXIT); return; }
            memcpy(screen, pages.data() + off, kPageBytes);
            video_.present();
            GameTimer::delay_with_pump(input_, 14);
        }
    }
}

// ==== sub_21476 / sub_1FEDA / sub_206E1: world map click, travel, arrival ====
// The walking-path data (word_A4388/A4608 ...) stays zero in BSS and sub_1FEDA never
// fills it; the only travel effect is the camera pan in sub_1FEDA. It moves the
// camera from the old point centre to the new one by Bresenham steps (up to
// max(|dx|,|dy|) + 1 frames) and advances the time phase every 60 steps.
// When the pan ends, byte_A2511 stays 1 (moving). On the next frame (top of
// sub_21476) the phase advances once more and arrival runs immediately:
// sub_10010(4), sub_10010(1), then the point-kind entry used by sub_20912.
// Clicking the current point enters sub_20912(0) at once; team 0x17 with a NULL
// point switches to "160". sub_206E1 allows a click only when both the current and
// target point are active (it does not check path connectivity). Right click opens
// the status window (sub_2F09A).
bool CHP::is_point_pair_active(int from_pt, int to_pt) const {
    if (from_pt < 0 || to_pt < 0 || from_pt >= MAX_POINTS || to_pt >= MAX_POINTS) return false;
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    const WorldPoint* pts = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
    return pts[from_pt].active == 1 && pts[to_pt].active == 1;
}

// sub_21476 loc_21957: the first point whose projected position is within 14 pixels
// of the click, whether active or not.
int CHP::pick_point_at(int mx, int my) const {
    int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    const WorldPoint* pts = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
    for (int i = 0; i < MAX_POINTS; i++) {
        int sx, sy;
        map_to_screen(pts[i].map_x, pts[i].map_y, sx, sy);
        if (std::abs(sx - mx) < 14 && std::abs(sy - my) < 14) return i;
    }
    return -1;
}

// sub_1FEDA loc_20303..loc_2035E: increment the time phase (byte_C5EA0, 0..7); wrapping
// to 8 increments the day (byte_C5EA3), and a day above 0xDC (220) resets to 1 and
// increments the year, as this path does in the original.
void CHP::advance_time_phase() {
    uint8_t old = state_.secondary_state;
    state_.secondary_state = (uint8_t)(old + 1);
    if (old == 7) {
        state_.secondary_state = 0;
        state_.time_of_day++;
    }
    if (state_.time_of_day > 0xDC) {
        state_.time_of_day = 1;
        state_.current_year++;
    }
    state_.current_day = (uint8_t)(state_.time_of_day + 1);
}

// ==== Travel effects (word_C5DD9+1: 1 bigong1.spr, 2 bigong2.spr, 3 seal.spr) ====
// sub_21476 -> sub_21234 (takeoff) -> sub_1FEDA (flying pan) -> descent and fade ->
// sub_21374 (landing). Sprites 0..7 of SPR slot 8 are the eight flight directions
// (dword_9ECB2..9ECCE); sprite 2 is the side view used for takeoff and landing.
static constexpr int kSkyW = 960, kSkyH = 151;      // 0x23640 = 960 * 151 bytes

// sub_1D83A/sub_1DA88: C5CDE == 0 waits for the 70 Hz retrace, 1 does not wait (C5C58 = 0).
int CHP::travel_frame_ms() const { return state_.config_data[0] == 1 ? 1 : 14; }

// Present a frame: sub_1DA88 (buffer -> VGA) plus sub_1D83A (retrace wait).
void CHP::travel_flip() {
    video_.present();
    input_.update();
    if (input_.quit_requested()) { dos_exit_ = true; return; }
    GameTimer::delay_with_pump(input_, travel_frame_ms());
}

// sub_35755(volume = byte_C5CE0 * 10, 3, 1): effect 3 is flying.VOC. byte_C5CE0 is 1
// when sound effects are on and 0 when they are off.
void CHP::play_travel_sfx() {
    if (state_.config_data[2] == 0) return;
    Audio::play_voc(game_path("flying.voc"));
}

// sub_2DF77: sky background. From file offset 0x80 it is PCX-style RLE (top two bits
// set -> low six bits are the run length, followed by one value byte), decoded until
// 0x23640 bytes are filled. The 256x3 palette at the end of the file is ignored.
bool CHP::load_sky_xcp(int chapter) {
    travel_sky_.assign((size_t)kSkyW * kSkyH, 0);
    std::string p = chapdata_path(("sky" + std::to_string(chapter) + ".xcp").c_str());
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { fprintf(stderr, "CHP: warning: sky%d.xcp missing -> black sky\n", chapter); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    std::vector<uint8_t> raw;
    if (sz > 0x80) {
        raw.resize((size_t)sz - 0x80);
        fseek(f, 0x80, SEEK_SET);
        if (fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
    }
    fclose(f);
    const size_t total = travel_sky_.size();
    size_t o = 0, i = 0;
    while (o < total && i < raw.size()) {
        uint8_t c = raw[i++];
        if ((c & 0xC0) == 0xC0) {
            int n = c & 0x3F;
            if (i >= raw.size()) break;
            uint8_t v = raw[i++];
            for (; n > 0 && o < total; n--) travel_sky_[o++] = v;
        } else {
            travel_sky_[o++] = c;
        }
    }
    return true;
}

// ==== sub_33C99: ending (staff roll), CHP.ASM 0x33C99..0x341CD ====
// Called from sub_22C8E loc_22E1B when byte_523C0 (chapter clear) is 1 and
// sub_10C6D(1) returns 0x270F (no quest in progress); then sub_204A7("EXIT").
// Steps, in ASM order:
// 1. sub_1D26C(".\\chapdata\\staff.spr", 8) loads STAFF.SPR (24 frames) into slot 8,
//    overwriting the bigong/seal travel sprites as the original does.
// 2. space3.xcp (960x150) into unk_DA370 without palette (sub_2DF77).
// 3. space.xcp (960x200) into dword_C5C5C with palette (sub_2DE6F).
// 4. A4368 from 0x280 (640) down to 1: fill the screen with sub_286DC (a 320x200
//    window over the 960 stride) and overlay sprite dword_9ECB2[2 - A4368/0xDC] at
//    (0xA0 + x_off, 0x64 + y_off). Each frame presents (sub_1DA88) and waits four
//    retraces (sub_1D83A).
// 5. space2.xcp (960x700) into dword_C5C5C, then 7 rounds of 500 frames:
//    sub_1CFEB (perspective_raster) draws the window at C5C5C + 0x3C0*A436C + 0x64
//    and overlays dword_9ECBE[eax] (eax = ebp/0x96, or dword_9ED0E once 0x18 or
//    more). After round 4 (var_4 == 3) the 144,000 bytes of unk_DA370 are copied to
//    C5C5C+0x35E80, a preservation copy the original never reads again.
// 6. sub_1D7C9(8,8) releases slot 8.
// 7. Save the palette (sub_1DDC4(A3D4C)), repeat sub_1E4CD(A3D4C,0,255,2) until it
//    returns not-1 (three retraces per step), clear the screen and show oddy.xcp
//    (320x200 with palette).
// 8. sub_2BBA0(0x79A) narration, then oddy.xcp again plus sub_2B1BD(0x54, name,
//    0x79B..0x7A3) for nine lines, using the Johab names stored in CHP.EXE.
// 9. Save the palette, fade out again and return.
static constexpr int    kEndSpaceW      = 960;      // 0x3C0: space*.xcp width, the sub_286DC stride
static constexpr size_t kEndBufSize     = 0xA8750;  // dword_C5C5C (nmalloc 0xA8750)
static constexpr uint32_t kEndRunTable  = 0x5B080;  // dword_5B080 run table
// 157 rows x 320 pixels consume exactly 77554 bytes (38777 words). The terminator
// check on the last row reads a lo/hi pair, which can pass the table end by 2 to 4
// bytes, so 8 extra bytes are read; the original reads past the table as well.
static constexpr size_t kEndRunTableLen = 77554 + 8;
static constexpr uint32_t kEndHeights   = 0x5AE28;  // dword_5AE24+4: row heights x1/2, summing to 200

// Common part of sub_2DE6F, sub_2DD68 and sub_2DF77: decode a PCX at w x h.
// with_palette false leaves the palette alone, like sub_2DF77.
bool CHP::load_xcp_into(const char* name, int w, int h, std::vector<uint8_t>& dst, bool with_palette) {
    dst.assign((size_t)w * h, 0);
    std::string path = chapdata_path(name);
    if (path.empty() || !file_exists(path.c_str())) path = game_path(name);
    if (path.empty() || !file_exists(path.c_str())) {
        fprintf(stderr, "CHP: error: cannot read %s\n", name);
        return false;
    }
    uint8_t keep[256][3];
    if (!with_palette) palette_get_range(0, 255, keep);
    const bool ok = pcx_load_to_buffer(path, dst.data(), w, h);
    if (!with_palette) palette_set_range(0, 255, keep);
    if (!ok) fprintf(stderr, "CHP: warning: %s decode failed\n", name);
    return ok;
}

// sub_1CFEB: perspective raster that turns 157 source rows into 200 screen rows.
// Original structure (0x1CFEB..0x1D0E7):
//   var_10 = var_14 = var_8 = 0, ecx = 0, var_18 = src.
//   loc_1D006: esi = (int)(var_10 / 1.5) - 0x68 + var_18 (fistp rounds; 2y/3 never
//              lands on .5), ebp = screen + var_8*0x140.
//   loc_1D039: word[2*ecx] == 0 ends the row after ecx++; otherwise edi = high word
//              = copy length, esi++, ecx++; a zero high word also ends the row after
//              ecx++; then memcpy(ebp, esi, hi); ecx++; esi += hi; ebp += hi.
//   loc_1D066: var_18 += 0x3C0; ecx++; edi = 0.
//   loc_1D0A4: esi = (dword[var_14 + 0xAE26] >> 16) - 1, i.e. word[0x5AE28 + 2*y] - 1
//              row copies; ebp++ and repeat loc_1D081 while edi < esi; then
//              var_14 += 2, var_10++, var_8 = ebp, repeat while var_10 < 0x9D.
// ecx keeps growing across rows because the table is one stream for all rows. Only a
// terminator (lo == 0) advances ecx by one, so its high word is read again as the
// first run of the next row. That is original behaviour and is reproduced here.
void CHP::perspective_raster(uint8_t* dest, const std::vector<uint8_t>& buf, long base_off) {
    if (!dest || buf.empty()) return;
    if (ending_runs_.size() < 8 || ending_heights_.size() < (size_t)NUM_MAP_SCAN_LINES * 2) return;

    const uint8_t* tbl = ending_runs_.data();
    const size_t tbl_size = ending_runs_.size();
    const long   buf_size = (long)buf.size();
    const long   screen_size = (long)SCREEN_W * SCREEN_H;

    size_t idx = 0;          // ecx: word index into the run table
    long   row_base = base_off;
    int    drow = 0;

    for (int y = 0; y < NUM_MAP_SCAN_LINES && drow < SCREEN_H; y++) {
        // ASM: esi = (int)(var_10 / 1.5) - 0x68 + var_18, var_18 = base + y*0x3C0.
        // fistp uses round-to-nearest, and 2y/3 is never .5, so this equals (4y+3)/6.
        // row_base already accumulates y*0x3C0, so it is not added again here.
        long s = row_base + ((4L * y + 3) / 6) - 0x68;
        long d = (long)drow * SCREEN_W;

        while (idx * 2 + 4 <= tbl_size) {
            const unsigned lo = (unsigned)tbl[idx * 2] | ((unsigned)tbl[idx * 2 + 1] << 8);
            if (lo == 0) { idx++; break; }                     // loc_1D066 (ecx += 1)
            const unsigned hi = (unsigned)tbl[idx * 2 + 2] | ((unsigned)tbl[idx * 2 + 3] << 8);
            s += 1;                                            // inc esi
            idx++;
            if (hi == 0) { idx++; break; }                     // a zero high word also ends the row
            // The original does no bounds check (the map buffer is always at least
            // 960x700); clipping per pixel here only avoids reading outside the buffer.
            for (unsigned k = 0; k < hi; k++) {
                const long si = s + (long)k;
                const long di = d + (long)k;
                if (si < 0 || si >= buf_size || di < 0 || di >= screen_size) continue;
                dest[di] = buf[(size_t)si];
            }
            s += hi; d += hi; idx++;
        }

        const int rep = (int)(ending_heights_[y * 2] | (ending_heights_[y * 2 + 1] << 8));
        for (int r = 1; r < rep && drow + r < SCREEN_H; r++)
            memcpy(dest + (size_t)(drow + r) * SCREEN_W, dest + (size_t)drow * SCREEN_W, SCREEN_W);

        drow += rep;
        row_base += 0x3C0;
    }
    if (drow < SCREEN_H)
        memset(dest + (size_t)drow * SCREEN_W, 0, (size_t)(SCREEN_H - drow) * SCREEN_W);
}

// sub_1D26C(".\\chapdata\\staff.spr", 8), sub_2DF77 (space3) and sub_2DE6F (space),
// plus the dword_5B080 run table and word_5AE28 heights read from CHP.EXE VAs.
bool CHP::ending_load_assets() {
    bool ok = true;

    ending_sprites_.clear();
    if (!load_spr(ending_sprites_, "CHAPDATA", "staff.spr", false) ||
        ending_sprites_.count() < 24) {
        // The original assumes 24 frames and reads dword_9ECB2[0..2],
        // dword_9ECBE[0..20] and dword_9ED0E, i.e. up to slot-8 entry 23. If fewer
        // are available the sequence runs without sprites.
        fprintf(stderr, "CHP: warning: STAFF.SPR load failed (%d frames) - continuing without sprites\n",
                ending_sprites_.count());
        ok = false;
    }

    if (!load_xcp_into("space3.xcp", kEndSpaceW, 150, ending_bg3_, false)) ok = false;
    ending_buf_.assign(kEndBufSize, 0);
    if (!load_xcp_into("space.xcp", kEndSpaceW, 200, ending_buf_, true)) ok = false;

    ending_runs_.clear();
    if (!exe_resource_load(ExeId::Chp, kEndRunTable, ExeResLength::Fixed, ending_runs_, kEndRunTableLen)) {
        fprintf(stderr, "CHP: warning: sub_1CFEB run table (VA 0x%X) could not be read\n", (unsigned)kEndRunTable);
        ok = false;
    }
    ending_heights_.clear();
    if (!exe_resource_load(ExeId::Chp, kEndHeights, ExeResLength::Fixed, ending_heights_,
                           (size_t)NUM_MAP_SCAN_LINES * 2)) {
        fprintf(stderr, "CHP: warning: sub_1CFEB row-height table (VA 0x%X) could not be read\n", (unsigned)kEndHeights);
        ok = false;
    }
    return ok;
}

void CHP::run_ending() {
    fprintf(stderr, "CHP: sub_33C99 ending sequence start\n");
    ending_load_assets();

    const int sp_count = ending_sprites_.count();
    auto draw_sprite = [&](int index) {
        if (index < 0 || index >= sp_count) return;
        const SpriteEntry* sp = ending_sprites_.get(index);
        if (!sp || !sp->pixel_data) return;
        blit_sprite_rle(0xA0 + sp->x_offset, 0x64 + sp->y_offset, sp->pixel_data);
    };

    uint8_t* screen = video_.get_backbuffer();
    if (!screen) return;

    // With byte_C5CDF (background music) == 1 the original calls sub_3615B (wait for
    // music to stop) and sub_360E2(0x12, 1). The port has no music engine, only
    // single-channel VOC effects, so both calls are omitted.

    // 1) Slide space.xcp rightwards with A4368 from 0x280 (640) down to 1 (sub_286DC).
    for (int off = 0x280; off > 0 && !dos_exit_; off--) {
        for (int y = 0; y < SCREEN_H; y++) {
            const long si = (long)off + (long)y * kEndSpaceW;   // sub_286DC(eax=off, edx=0)
            if (si < 0 || si + SCREEN_W > (long)ending_buf_.size()) continue;
            memcpy(screen + (size_t)y * SCREEN_W, ending_buf_.data() + si, SCREEN_W);
        }
        int idx = 2 - (off / 220);                              // dword_9ECB2[2 - A4368/0xDC]
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        draw_sprite(idx);
        video_.present();                                       // sub_1DA88
        flip_wait(4);                                           // sub_1D83A x 4
    }

    // 2) space2.xcp over 7 rounds of 500 frames (sub_1CFEB plus dword_9ECBE/9ED0E).
    if (!dos_exit_ && load_xcp_into("space2.xcp", kEndSpaceW, 700, ending_buf_, true)) {
        int ebp = 0;                                            // var_3C: accumulated frames
        for (int outer = 0; outer < 7 && !dos_exit_; outer++) {
            for (int f = 0x1F4; f > 0 && !dos_exit_; f--) {      // dword_A436C from 0x1F4 down to 1
                // Edx = dword_C5C5C + 0x3C0*A436C + 0x64
                perspective_raster(screen, ending_buf_, (long)f * kEndSpaceW + 0x64);
                int idx = ebp / 0x96 + 3;                        // dword_9ECBE[eax] == slot 8[3+eax]
                if (idx >= 0x18) idx = 0x17;                     // dword_9ED0E == slot 8[23]
                draw_sprite(idx);
                video_.present();
                flip_wait(4);
                ebp++;
            }
            if (outer == 3 && ending_bg3_.size() == 0x23280 &&
                ending_buf_.size() >= 0x35E80 + 0x23280) {
                // The original copies space3 (144,000 bytes) to dword_C5C5C+0x35E80 and never reads
                // it again.
                memcpy(ending_buf_.data() + 0x35E80, ending_bg3_.data(), 0x23280);
            }
        }
    }
    if (dos_exit_) return;

    // 3) sub_1D7C9(8,8): release slot 8.
    ending_sprites_.clear();

    // 4) Oddy image and ending dialogue.
    // Speaker names stay as the Johab strings stored in CHP.EXE at 0x480FB (Prios),
    // 0x48107 (Deimos), 0x48112 (Belamod) and 0x4811D (Wiesbaden).
    static const char kNamePriest[]    = " \xCF\x61\x9C\x61\xB7\xA1\xB5\xA1\xAF\x61 ";   // Prios
    static const char kNameDeimos[]    = " \x95\x41\xB7\xA1\xA1\xA1\xAF\x61 ";           // Deimos
    static const char kNameBelamod[]   = " \xA5\x41\x9C\x61\xA1\xA1\x97\x61 ";           // Belamod
    static const char kNameWiesbaden[] = " \xA7\xA1\xAF\x61\xA4\x61\x95\x45 ";           // Wiesbaden

    auto show_oddy = [&]() -> bool {
        std::vector<uint8_t> oddy;
        if (!load_xcp_into("oddy.xcp", SCREEN_W, SCREEN_H, oddy, true) ||
            oddy.size() != (size_t)SCREEN_W * SCREEN_H) return false;
        memcpy(screen, oddy.data(), oddy.size());   // sub_2DD68(oddy, C5EA4) + sub_1DA67
        video_.present();
        return true;
    };

    // sub_1DDC4(A3D4C, 0, 255) saves the palette, then sub_1E4CD(A3D4C, 0, 255, 2) fades out.
    auto fade_out = [&]() {
        palette_get_range(0, 255, travel_pal_);                // sub_1DDC4
        fade_counter_ = 0;
        while (!dos_exit_ && travel_fade_step()) {             // while sub_1E4CD returns 1
            video_.present();
            flip_wait(3);                                      // sub_1D83A x 3
        }
    };

    fade_out();

    // sub_1DB9B(0, 0, 0x140, 0xC8, 0): clear the screen.
    memset(screen, 0, (size_t)SCREEN_W * SCREEN_H);
    video_.present();

    show_oddy();
    run_narration(0x79A);                     // sub_2BBA0(0x79A); ebx = 0x79B is overwritten at once

    struct EndingLine { const char* name; int talk_id; };
    static const EndingLine kLines[] = {
        { kNamePriest,    0x79B },
        { kNameDeimos,    0x79C },
        { kNamePriest,    0x79D },
        { kNameBelamod,   0x79E },
        { kNamePriest,    0x79F },
        { kNameDeimos,    0x7A0 },
        { kNamePriest,    0x7A1 },
        { kNameDeimos,    0x7A2 },
        { kNameWiesbaden, 0x7A3 },
    };
    for (const EndingLine& line : kLines) {
        if (dos_exit_) break;
        show_oddy();                          // sub_2DD68 + sub_1DA67, redisplayed before each line
        run_talk(0x54, line.name, line.talk_id);   // sub_2B1BD(0x54, name, talk_id, 0)
    }

    if (dos_exit_) return;
    fade_out();                               // 0x3418A: save the palette, then fade out again
    fprintf(stderr, "CHP: sub_33C99 ending sequence end\n");
}

// sub_3907C x 6 (from sub_1EAA1): fog shade LUT. Row r maps each colour to the
// nearest palette entry (sub_38A00) of "base * (100-pct)% + palette[1] * pct% + 9"
// with pct = 10/30/45/60/75/90. Sub_37982 only reads entries 0..254, so entry 255
// is treated as 0.
void CHP::build_fog_lut() {
    int P[256][3];
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < 3; c++) P[i][c] = (i < 255) ? travel_pal_[i][c] : 0;
    static const int kPct[6] = {10, 30, 45, 60, 75, 90};
    for (int r = 0; r < 6; r++) {
        const int pct = kPct[r], w = 100 - pct;
        const int br = P[1][0] * pct / 100, bg = P[1][1] * pct / 100, bb = P[1][2] * pct / 100;
        for (int p = 0; p < 256; p++) {
            const int R = (P[p][0] * w / 100 + br + 9) & 0xFF;
            const int G = (P[p][1] * w / 100 + bg + 9) & 0xFF;
            const int B = (P[p][2] * w / 100 + bb + 9) & 0xFF;
            int best = 0, bestd = 0x7FFF;
            for (int k = 0; k < 256; k++) {
                int d = std::abs(R - P[k][0]) + std::abs(G - P[k][1]) + std::abs(B - P[k][2]);
                if (d < bestd) { bestd = d; best = k; }
            }
            fog_lut_[r][p] = (uint8_t)best;
        }
    }
}

// sub_1E4CD(pal, 0, 255, 2): darken the palette one step per call.
//   The first call sets cur = c*1000 and delta = c*1000/32; every call subtracts
//   delta and writes cur/1000 to the DAC. Returns true for counters 0..28 and false
//   from the 29th call on, so there are 30 steps ending at about 1/16 brightness.
bool CHP::travel_fade_step() {
    if (fade_counter_ == 0) {
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++) {
                fade_cur_[i][c] = travel_pal_[i][c] * 1000;
                fade_delta_[i][c] = travel_pal_[i][c] * 1000 / 32;
            }
    }
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < 3; c++) {
            fade_cur_[i][c] -= fade_delta_[i][c];
            g_palette[i][c] = (uint8_t)(fade_cur_[i][c] / 1000);
        }
    const int before = fade_counter_++;
    if (before > 28) { fade_counter_ = 0; return false; }
    return true;
}

// One flying-view frame, the branch of sub_1ED53 where byte_A2511 == 1,
// byte_7BCA4 == 0 and the travel option is non-zero:
//   sub_286DC: copy a 320x200 window from the 960-wide sky at column travel_sky_off_
//   sub_1D0E8: ground from row 153, shrinking the map with per-row piece lengths to
//              build perspective
//   sub_3905A + sub_377F5: fog bands on rows 0x88..0xA6, mirrored around the horizon
//              over LUT rows 0..5
//   sub_1DB05 / sub_1DAD7: clear the top row and the left/right columns with colour 0
void CHP::render_flying_view() {
    uint8_t* d = video_.get_backbuffer();
    if (!d) return;
    // Sky; anything past the 151-row sky buffer reads as 0.
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            size_t idx = (size_t)y * kSkyW + travel_sky_off_ + x;
            d[y * SCREEN_W + x] = idx < travel_sky_.size() ? travel_sky_[idx] : 0;
        }
    }
    // Ground (sub_1D0E8).
    const long base = (long)state_.camera_x + (long)state_.camera_y * MAP_STRIDE;   // A4368 + A436C*0x3C0
    const long map_size = (long)MAP_STRIDE * g_map_height;
    auto map_at = [&](long i) -> uint8_t { return (g_map_buffer && i >= 0 && i < map_size) ? g_map_buffer[i] : 0; };
    int row = 153;                                   // var_1C(0x7B) * 0x140 + 0x2580 = row 153
    for (int g = 0; g < kChpGroundGroupCount; g++) {
        long s = base + (long)g * 0xF00 + (long)(g / kChpGroundColDivisor) - 150;
        int col = 0;
        for (int k = kChpGroundPieceStart[g]; k < kChpGroundPieceStart[g + 1]; k++) {
            const int len = kChpGroundPieces[k];
            s += 1;                                  // inc esi
            for (int j = 0; j < len; j++)
                if (col + j < SCREEN_W && row < SCREEN_H) d[row * SCREEN_W + col + j] = map_at(s + j);
            s += len; col += len;
        }
        const int rep = std::max<int>(kChpGroundRepeat[g], 1);
        for (int r = 1; r < rep; r++)
            if (row + r < SCREEN_H) memcpy(d + (row + r) * SCREEN_W, d + (row + r - 1) * SCREEN_W, SCREEN_W);
        row += rep;
    }
    // Fog bands (sub_377F5: x 0..318, y0 <= y < y1).
    static const int kBands[11][3] = {
        {5, 0x95, 0x98}, {4, 0x98, 0x9B}, {4, 0x94, 0x97}, {3, 0x9B, 0x9E}, {3, 0x91, 0x94},
        {2, 0x9E, 0xA1}, {2, 0x8E, 0x91}, {1, 0xA1, 0xA4}, {1, 0x8B, 0x8E}, {0, 0xA4, 0xA7}, {0, 0x88, 0x8B}};
    for (const auto& b : kBands)
        for (int y = b[1]; y < b[2] && y < SCREEN_H; y++)
            for (int x = 0; x < 0x13F; x++) {
                uint8_t& px = d[y * SCREEN_W + x];
                px = fog_lut_[b[0]][px];
            }
    // Border
    memset(d, 0, SCREEN_W);
    for (int y = 0; y < SCREEN_H; y++) { d[y * SCREEN_W] = 0; d[y * SCREEN_W + 0x13F] = 0; }
}

// Start of travel (sub_21476 loc_21A5x..loc_21AD8 plus sub_21234): with the effect
// option on, load the SPR and play the takeoff (ship rising plus fade out).
// Returns false when the effect is not used.
bool CHP::travel_begin(int from_pt) {
    travel_fx_ = false;
    travel_sprites_.clear();
    const int style = state_.travel_style;
    if (style < 1 || style > 3 || in_tool_mode_) return false;
    static const char* kNames[4] = {nullptr, "bigong1.spr", "bigong2.spr", "seal.spr"};
    if (!load_spr(travel_sprites_, "CHAPDATA", kNames[style], false) || travel_sprites_.count() < 8) {
        fprintf(stderr, "CHP: warning: travel effect SPR(%s) load failed -> effect skipped\n", kNames[style]);
        travel_sprites_.clear();
        return false;
    }
    travel_fx_ = true;
    memcpy(travel_pal_, g_palette, sizeof(travel_pal_));   // sub_1DDC4(A3D4C, 0, 255)
    build_fog_lut();

    // sub_21234: takeoff.
    const int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    const WorldPoint* pts = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
    const SpriteEntry* sp = travel_sprites_.get(2);      // dword_9ECBA
    state_.camera_x = pts[from_pt].map_x - 0x8C;
    state_.camera_y = pts[from_pt].map_y - 0x69;
    render_map_and_points();
    uint8_t* back = video_.get_backbuffer();
    travel_snapshot_.assign(back, back + SCREEN_W * SCREEN_H);   // sub_1DABB -> DA370
    play_travel_sfx();
    const int sx = 0xA0 + sp->x_offset;
    int y = sp->y_offset + 0x64;
    double v = 0.0;
    while (!dos_exit_) {
        memcpy(back, travel_snapshot_.data(), SCREEN_W * SCREEN_H);
        if (sp->pixel_data) blit_sprite_rle_clipped(sx, y, sp->pixel_data);
        travel_flip();
        if (y < 0) break;
        y -= (int)v;
        v += kChpDepartAccel;
    }
    fade_counter_ = 0;
    while (!dos_exit_ && travel_fade_step()) travel_flip();   // one frame per darkened palette step
    memset(back, 0, SCREEN_W * SCREEN_H);                     // sub_1DB9B(0,0,320,200, 0, VRAM)
    video_.present();
    memcpy(g_palette, travel_pal_, sizeof(g_palette));        // sub_1DD0D(A3D4C, 0, 255)
    return true;
}

// End of travel (sub_21476 after loc_21AF6, plus sub_21374).
void CHP::travel_end(int to_pt) {
    if (!travel_fx_) return;
    const int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    const WorldPoint* pts = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
    uint8_t* back = video_.get_backbuffer();
    if (pts[to_pt].kind == 3) {                       // JUMP destination: just clear the screen
        memset(back, 0, SCREEN_W * SCREEN_H);
        video_.present();
    } else {
        memcpy(g_palette, travel_pal_, sizeof(g_palette));   // sub_1DD0D
        // sub_21374: landing. The ship descends over the destination point.
        const SpriteEntry* sp = travel_sprites_.get(2);
        state_.camera_x = pts[to_pt].map_x - 0x8C;
        state_.camera_y = pts[to_pt].map_y - 0x69;
        render_map_and_points();
        travel_snapshot_.assign(back, back + SCREEN_W * SCREEN_H);
        const int sx = 0xA0 + sp->x_offset;
        int y = sp->y_offset;
        double v = 2.0;
        while (!dos_exit_) {
            memcpy(back, travel_snapshot_.data(), SCREEN_W * SCREEN_H);
            if (sp->pixel_data) blit_sprite_rle_clipped(sx, y, sp->pixel_data);
            travel_flip();
            if (y > 0x69) break;
            y += (int)v;
            if (y > 0x5A) v = 1.0;
            else if (y > 0x46) v = 1.5;
        }
    }
    travel_sprites_.clear();                          // sub_1D7C9(8, 8)
    travel_fx_ = false;
}

// sub_1FEDA direction pick, i.e. which of dword_9ECB2..9ECCE to use. The integer
// ratio r = larger axis / smaller axis (0 when the smaller axis is 0) separates the
// axis-aligned cases (r >= 3) from the diagonals (r < 3):
// 0 down, 1 down-left, 2 left, 3 up-left, 4 up, 5 up-right, 6 right, 7 down-right.
static int travel_dir_index(int sx, int sy, int adx, int ady) {
    int r = 0;
    if (adx > ady) r = ady ? adx / ady : 0;
    else           r = adx ? ady / adx : 0;
    if      (sy >= 1  && r >= 3 && adx < ady) return 0;    // dword_9ECB2
    else if (sx <= -1 && sy >= 1 && r < 3)    return 1;    // 9ECB6
    else if (sx <= -1 && r >= 3 && adx > ady) return 2;    // 9ECBA
    else if (sx <= -1 && sy <= -1 && r < 3)   return 3;    // 9ECBE
    else if (sy <= -1 && r >= 3 && adx < ady) return 4;    // 9ECC2
    else if (sx >= 1  && sy <= -1 && r < 3)   return 5;    // 9ECC6
    else if (sx >= 1  && r >= 3 && adx > ady) return 6;    // 9ECCA
    else if (sx >= 1  && r < 3)               return 7;    // 9ECCE
    // The original leaves ebp unchanged for a pure axis move (r == 0) because no
    // branch matches, which is undefined behaviour; the port uses the matching axis sprite.
    if (adx == 0) return sy >= 1 ? 0 : 4;
    return sx >= 1 ? 6 : 2;
}

// sub_1FEDA: blocking camera pan from (x0,y0) to (x1,y1), given as map coordinates of
// the camera's top-left corner. With the travel effect on (travel_fx_) the same loop
// scrolls the sky, bobs the direction sprite and plays the effect every 40 steps;
// after the pan the ship drops away and the palette fades.
void CHP::camera_pan(int x0, int y0, int x1, int y1) {
    pan_active_ = true;                       // byte_7BCA4 = 0
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : (dx == 0 ? 0 : -1);
    int sy = dy > 0 ? 1 : (dy == 0 ? 0 : -1);
    int adx = std::abs(dx), ady = std::abs(dy);
    int steps = std::max(adx, ady);
    if (steps < 4) { pan_active_ = false; return; }

    const bool fx = travel_fx_ && !in_tool_mode_;
    const SpriteEntry* ship = nullptr;        // ebp: direction sprite for this move
    int sky_dir = -1;                         // var_24
    int bob_dir = 1;                          // var_14
    int ship_y = 0;                           // var_30
    travel_sky_off_ = 0xF0;                   // var_C
    if (fx) {
        load_sky_xcp(state_.chapter_id);      // sub_2DF77 (black sky when the file is missing)
        sky_dir = sx > 0 ? 1 : -1;
        const int idx = travel_dir_index(sx, sy, adx, ady);
        ship = travel_sprites_.get(idx);
        if (!ship || !ship->pixel_data) ship = nullptr;
    }

    int cam_x = x0, cam_y = y0, err_x = 0, err_y = 0;
    for (int s = 0; s <= steps && !dos_exit_; s++) {
        err_x += adx; err_y += ady;
        if (steps < err_x) { err_x -= steps; cam_x += sx; }
        if (steps < err_y) { err_y -= steps; cam_y += sy; }
        state_.camera_x = cam_x;
        state_.camera_y = cam_y;
        render_map_and_points();              // sub_286DC + sub_21C58 (the flying view during the effect)
        if (fx && ship) {
            if (s % 2 == 0) {                 // sky scroll: one column every two steps, wrapping to 0x1E0
                travel_sky_off_ += sky_dir;
                if (travel_sky_off_ == 0x1E0 || travel_sky_off_ == 0) travel_sky_off_ = 0x1E0;
            }
            if (s % 0x20 == 0) bob_dir = -bob_dir;
            const int q = (s % 0x20) / 8;
            ship_y = (bob_dir == 1) ? ship->y_offset + 0x32 + q : ship->y_offset + 0x37 - q;
            blit_sprite_rle_clipped(0xA0 + ship->x_offset, ship_y, ship->pixel_data);
            if (s % 40 == 0) play_travel_sfx();
        }
        video_.present();
        if (s % 60 == 0) advance_time_phase();
        input_.update();
        if (input_.quit_requested()) { dos_exit_ = true; break; }
        GameTimer::delay_with_pump(input_, travel_frame_ms());
    }
    render_map_and_points();                  // last frame, still the flying view (7BCA4 becomes 1 after this)
    pan_active_ = false;                      // byte_7BCA4 = 1

    // After the pan: unless the destination is a JUMP point (kind 3), the ship drops
    // away and the palette fades.
    if (fx && ship && !dos_exit_) {
        const int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
        const WorldPoint* pts = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
        const int to = state_.current_point < MAX_POINTS ? state_.current_point : 0;
        if (pts[to].kind != 3) {
            uint8_t* back = video_.get_backbuffer();
            travel_snapshot_.assign(back, back + SCREEN_W * SCREEN_H);   // sub_1DABB -> DA370
            play_travel_sfx();
            const int shx = 0xA0 + ship->x_offset;
            int y = ship_y;
            double v = 0.0;                                              // var_44
            while (!dos_exit_) {
                memcpy(back, travel_snapshot_.data(), SCREEN_W * SCREEN_H);
                blit_sprite_rle_clipped(shx, y, ship->pixel_data);
                travel_flip();
                if (y > 0xC7) break;
                y += (int)v;
                v += kChpExitAccel;
            }
            fade_counter_ = 0;
            while (!dos_exit_ && travel_fade_step()) travel_flip();
        }
    }
}

void CHP::world_map_step() {
    const int ch = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    const WorldPoint* pts = reinterpret_cast<const WorldPoint*>(all_chapter_points_[ch]);
    const int cur = state_.current_point < MAX_POINTS ? state_.current_point : 0;

    if (moving_) {
        // Moving frame (top of sub_21476). byte_804A4 is never written, so the even
        // branch always runs (path index +1, phase advance). The path array is empty,
        // so arrival happens in the same frame.
        advance_time_phase();
        // The path coordinate is 0, so the projected value is negative and the
        // left/top edge correction at loc_215B1 pulls the camera back by one pixel.
        if (state_.camera_x > 0x50) state_.camera_x -= 1;
        if (state_.camera_y > 0)    state_.camera_y -= 1;

        moving_ = false;
        click_latch_ = true;
        check_event_triggers(4);
        check_event_triggers(1);
        if (state_.transition_pending || dos_exit_) return;
        enter_point(0);     // kind 0/1/3 enters a town, battle or jump; kind 2 does nothing (loc_216DC..)
        return;
    }

    // Idle state (loc_218E7): refresh the party screen position and handle clicks.
    party_sx_ = pts[cur].map_x - state_.camera_x;
    party_sy_ = pts[cur].map_y - state_.camera_y - 8;

    if (input_.get_mouse().right_clicked) {
        // sub_22C8E loc_230D0: right click (event 2) opens the status window.
        show_status_window();
        return;
    }
    if (!input_.was_left_clicked()) { click_latch_ = false; return; }
    click_latch_ = true;

    const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
    const int target = pick_point_at(mx, my);
    if (target < 0) return;

    if (target == cur) {
        fprintf(stderr, "CHP: current point %d clicked again -> enter\n", cur);
        enter_point(0);
        if (!state_.transition_pending && state_.team_active == 0x17 && pts[target].kind == 2) {
            do_transition("160");     // unk_50957
        }
        return;
    }
    if (!is_point_pair_active(cur, target)) return;   // sub_206E1

    fprintf(stderr, "CHP: travel %d -> %d ('%s')\n", cur, target, pts[target].display_name);
    moving_ = true;
    state_.current_point = (uint8_t)target;
    const WorldPoint& from = pts[cur];
    const WorldPoint& to = pts[target];
    travel_begin(cur);          // sub_21234: load the SPR and play takeoff when enabled
    camera_pan(from.map_x - 0x8C, from.map_y - 0x69, to.map_x - 0x8C, to.map_y - 0x69);
    travel_end(target);         // sub_21374 landing, or clear the screen for a JUMP point
}

// ==== sub_22C8E loc_22E1B: chapter clear handling, checked at the top of each frame ====
//   mov dh, byte_523C0 ; cmp dh, 1 ; jnz loc_22ED9   ; not cleared: normal frame
//   mov eax, 1 ; call sub_10C6D                      ; next chapter select (mode 1)
//   mov edx, eax ; cmp ax, 270Fh ; jnz loc_22E5C
//     call sub_33C99                                 ; ending
//     mov eax, "EXIT" ; call sub_204A7
//   loc_22E5C: rebuild "\\chapdata\\<N>.chp" in unk_80302 and return via loc_2329C,
//   so main_ loc_238DF calls sub_232A4 again.
// Returns true when next_chapter_num_ is set (run() re-enters chapter_menu) and
// false on the ending path, where "EXIT" is queued and the caller stops.
bool CHP::handle_chapter_end() {
    const int sel = show_chapter_select(1);            // sub_10C6D(mode=1)
    if (sel == 0x270F) {                               // cmp ax, 270Fh
        run_ending();                                  // sub_33C99
        // sub_204A7("EXIT"): the effect is skipped but the save path at loc_2062E still
        // runs (sub_15BD9 blackbox.dat, sub_1776D game config, sub_1492E contibak.tmp, "$").
        save_blackbox();
        write_config();
        save_contibak();
        transition_exit(IPC::EXIT);
        return false;
    }
    next_chapter_num_ = sel;
    next_chapter_pending_ = true;
    fprintf(stderr, "CHP: loc_22E1B -> re-enter with next chapter %d (\"\\chapdata\\%d.chp\")\n",
            sel, sel);
    return true;
}

void CHP::world_map_loop() {
    fprintf(stderr, "CHP: Entering world map loop (sub_22C8E / sub_21476)\n");
    init_camera();

    // sub_22C8E first half: the point -> world map zoom-out, played once before the
    // interactive loop.
    // Event triggers (sub_10010) and the clear check (sub_1088D) are not inside
    // sub_22C8E; the caller runs them first (sub_232A4: sub_10010(1), sub_20912(1),
    // sub_22C8E; main_ loc_2388A: sub_22A5E, sub_1088D, sub_10010(0), sub_10010(4),
    // sub_22C8E). Trigger dialogue therefore plays over a cleanly rendered map, and the
    // zoom-out shows the point states the triggers left behind. chapter_menu and
    // resume_from_save keep that order.
    // ASM head: `mov ah, byte_523C0 ; cmp ah, 1 ; jz loc_22DB8` skips the zoom-out
    // (sub_28C8B/1CE44/1CCB3) and the party sprite setup once the chapter is cleared,
    // going straight to loc_22E1B.
    if (state_.chapter_clear != 1) zoom_out_effect();

    moving_ = false;
    pan_active_ = false;
    first_frame_ = true;     // byte_80320: no party sprite on the first frame
    click_latch_ = false;

    GameTimer::FramePacer pacer(60.0);
    while (!state_.transition_pending && !dos_exit_) {
        input_.update();
        if (input_.quit_requested()) { transition_exit("EXIT"); return; }

        // sub_22C8E loc_22E1B: chapter cleared (byte_523C0 == 1) -> next chapter or
        // ending. The original checks right after reading the event at the top of the
        // frame; a selection rebuilds the file name and re-enters sub_232A4, so this
        // loop exits here and run() re-enters.
        if (state_.chapter_clear == 1) {
            handle_chapter_end();
            return;
        }

        world_map_step();                 // sub_21476
        if (state_.transition_pending || dos_exit_) break;

        if (esc_down()) {                 // sub_22C8E: ESC -> sub_20BC8 (options/save/load)
            option_screen();
            if (state_.transition_pending || dos_exit_) break;
            while (esc_down() && !dos_exit_) {   // wait for the key release to avoid re-entry
                input_.update();
                if (input_.quit_requested()) { transition_exit("EXIT"); return; }
                SDL_Delay(5);
            }
            click_latch_ = false;
            moving_ = false;
        }

        update_camera();                  // sub_21C58 -> sub_1ED53
        render_world_map();               // map and points, party sprite, date overlay
        video_.present();
        first_frame_ = false;

        pacer.pace(input_);
    }

    if (state_.transition_pending) {
        fprintf(stderr, "CHP: Transition pending -> command='%s'\n", state_.pending_command);
        state_.transition_pending = false;
    }
}

// ==== sub_232A4 / main_: chapter entry point ====
// sub_232A4 is the chapter loop main_ loc_238DF calls repeatedly.
//   loc_2348B: sub_14700(unk_80302) with ".\chapdata\<argv[2]>.chp",
//              unk_C5C6C[byte_C5E9F] = 0, sub_22C11(1), the cursor sprite,
//              sub_1EAA1(byte_C5E9F), sub_10010(1), sub_20912(1) (banner and point
//              entry) and sub_22C8E() (world map loop).
// unk_80302 is only valid on two paths: a START file made main_+25B rebuild the name
// from the number sub_10C6D chose, or argc == 3 ("s <N>"). The paths without START
// (continue, return from battle) do not call this function; they use
// resume_from_save, whose state of truth is contibak.tmp.
void CHP::chapter_menu() {
    fprintf(stderr, "CHP: chapter_menu (sub_232A4, chapter file '%d.chp')\n", chapter_num_);
    state_.chapter_clear = 0;

    std::string cb_path = chapdata_path((std::to_string(chapter_num_) + ".chp").c_str());
    if (!file_exists(cb_path.c_str())) cb_path = chapdata_path((std::to_string(chapter_num_) + ".CHP").c_str());
    if (!file_exists(cb_path.c_str())) cb_path = game_path((std::to_string(chapter_num_) + ".chp").c_str());
    if (!file_exists(cb_path.c_str())) cb_path = game_path((std::to_string(chapter_num_) + ".CHP").c_str());
    if (!file_exists(cb_path.c_str())) cb_path = chapdata_path("CONTIBAK.DEF");
    if (!file_exists(cb_path.c_str())) cb_path = chapdata_path(IPC::CONTIBAK_TMP);
    load_contibak_file(cb_path);

    // After sub_14700 the original keeps using the byte_C5E9F (chapter) and byte_C5EA2
    // (point) read from the file and never overwrites them with the number chosen
    // outside. chapter_num_ only selects the file, so state_.chapter_id from the file
    // is authoritative from here on.
    if (state_.chapter_id != (uint8_t)chapter_num_) {
        fprintf(stderr, "CHP: chapter_num_=%d but loaded byte_C5E9F=%d (trusting the file)\n",
                chapter_num_, state_.chapter_id);
    }
    int ci = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
    state_.chapter_started_flag[ci] = 0;

    reset_and_update_points(1);

    // sub_232A4 loc_234B4..loc_234CF: party context flags written on chapter entry,
    // right after reset_and_update_points (sub_22C11, mode 1). PACK's town entry events
    // (TEC flag indices in the 200s) and PACK's leader test (byte_7D955) read them;
    // without them team-gated events (SM22 271/272/273 and others) never fire.
    //   flag[200] = team_active (byte_C5EA1)
    //   flag[201] = the team's default leader from the 14-byte table below, taken from
    //               CHP.EXE VA 0x1FB47 (file 0x23D47) with a movsd*3 + movsw 14-byte
    //               copy; IDA misread it as code through `mov esi, offset loc_1FB46+1`.
    //   flag[202] = 1 (reward pending; sub_10975 consumes and clears it)
    // The resume path (loc_237E0) has no equivalent and does not write these.
    // handle_chapter_reward (sub_10975) follows; with flag[202] == 1 it always runs and
    // rebuilds the party and team, sets flag[201] to the party list head, pays 500 or
    // 1000 gold and clears flag[202].
    static constexpr uint8_t kChapterEntryLeader[14] = {
        1, 2, 1, 7, 1, 1, 3, 1, 11, 3, 8, 3, 3, 3,
    };
    g_event_flags[200] = state_.team_active;
    {
        int t = (int)state_.team_active;
        uint8_t leader = 0;
        if (t >= 0 && t < 14) leader = kChapterEntryLeader[t];
        else fprintf(stderr, "CHP: team %d outside leader table, flag[201]=0\n", t);
        g_event_flags[201] = leader;
    }
    g_event_flags[202] = 1;
    fprintf(stderr, "CHP: chapter entry flags: [200]=%u (team) [201]=%u (leader) [202]=1\n",
            g_event_flags[200], g_event_flags[201]);
    handle_chapter_reward();                 // sub_232A4+231: call sub_10975

    // sub_232A4 does not call sub_112E0 (SCRIPT*.TBL); only the chapter/quest select
    // screen sub_10C6D reads those tables.
    load_chapter_data(state_.chapter_id);

    check_event_triggers(1);
    if (state_.transition_pending || dos_exit_) return;   // a script requested a transition (the original exits here)

    // sub_20912(1): plain map and point rendering (sub_21C58), present, then the
    // chapter title banner. The banner stage draws neither the party sprite nor the
    // time/day/Days Remain overlay, since both belong to the sub_22C8E interactive
    // loop, so the plain renderer is used instead of render_world_map().
    enter_point(1);       // renders the map, shows the banner and enters the point

    // sub_20912: after the banner click the point kind switches immediately
    // (TOWN -> SM, BATTLE -> SCE). When enter_point(1) already queued a transition
    // (kind 0, 1 or 3) the original exits instead of running the interactive world
    // map; only kind 2 (NULL, no transition) continues into sub_22C8E.
    if (state_.transition_pending || dos_exit_) {
        fprintf(stderr, "CHP: chapter_menu -> immediate transition/exit on point entry (pending='%s'), world map skipped\n",
                state_.pending_command);
        return;
    }

    // sub_232A4 goes straight to sub_22C8E after sub_20912(1). sub_1088D
    // (check_chapter_clear) only appears on the JUMP (kind 3) tail of sub_20912
    // (loc_20A7C ... sub_10010(0), sub_1088D); every other kind leaves through
    // loc_20BC0, so it is not called here. The kind 3 branch of enter_point covers it.
    world_map_loop();
}

// ==== resume_from_save: main_ loc_237E0 (0x237E0..0x238CC) ====
// Entry when no START file exists: continue (LOADED) or return from battle.
// MAIN.EXE only reaches it through `spawnl("chp.exe", "p", NULL)`.
// ASM order:
//   sub_14700(".\chapdata\contibak.tmp") progress (points, paths, scripts, state)
//   sub_15DB7()                          blackbox.dat (flags, characters, gold)
//   sub_22B12()                          camera around the current point
//   sub_1EAA1(byte_C5E9F)                chapter map and palette
//   unk_AC1C3 / unk_AC1C5 = 0            clear the scene markers
//   no LOADED -> sub_22562/2213E/220B3/2288C/2233E refresh point and path activation
//   sub_21C58() map and points / sub_22A5E() point kind sync
//   sub_1088D() clear check (quest objectives, rewards) / sub_10010(0), sub_10010(4)
//   sub_22C8E()                          world map loop
// loc_2388A finally unlinks LOADED.
// It never calls sub_20912 (point entry) or sub_1FC5D (chapter banner): returning from
// battle resumes the world map instead of replaying the point just finished.
void CHP::resume_from_save(bool from_load_flag) {
    fprintf(stderr, "CHP: resume_from_save (loc_237E0, LOADED=%d)\n", from_load_flag ? 1 : 0);
    state_.chapter_clear = 0;

    // sub_14700(".\chapdata\contibak.tmp")
    std::string cb = chapdata_path(IPC::CONTIBAK_TMP);
    if (!load_contibak_file(cb)) {
        // The original prints "%s read error!", waits for a key and returns to DOS;
        // the port continues with defaults when there is no saved progress.
        fprintf(stderr, "CHP: warning: contibak.tmp missing - continuing with defaults (chapter=%d)\n", chapter_num_);
        state_.chapter_id = (uint8_t)chapter_num_;
    }
    // sub_14700's byte_C5E9F (chapter) and byte_C5EA2 (point) are the state of record.
    const int cb_chapter = state_.chapter_id;
    const int cb_point   = state_.current_point;
    const int cb_year    = state_.current_year;
    const int cb_tod     = state_.time_of_day;

    // sub_15DB7(): blackbox.dat. The original leaves byte_C5E9F/byte_C5EA2 alone and
    // only moves the date forward to the later of the two (loc_15FEC: cmp eax, esi /
    // jge loc_16008).
    load_blackbox();
    load_script_tables(1);   // shared port behaviour: SCRIPT1..3.TBL dialogue tables
    vm_team_rebuild();       // sub_15DB7 tail (sub_15A7F + sub_15AE5)
    state_.chapter_id = (uint8_t)cb_chapter;
    state_.current_point = (uint8_t)cb_point;
    if ((int)cb_year * DAYS_PER_YEAR + cb_tod >
        (int)state_.current_year * DAYS_PER_YEAR + (int)state_.time_of_day) {
        state_.current_year = cb_year;
        state_.time_of_day = cb_tod;
        state_.current_day = (uint8_t)(cb_tod + 1);
    }

    // sub_22B12(): camera around the current point.
    init_camera();

    // sub_1EAA1(byte_C5E9F): chapter map and palette, and g_world_points sync.
    load_chapter_data(state_.chapter_id);

    // loc_23827 / loc_2382F: unk_AC1C3 and unk_AC1C5 = 0 (clear the scene markers).
    state_.chapter_scene_marker = 0;
    state_.chapter_scene_target = 0;

    // loc_23867: recompute point and path activation only without LOADED; with it the
    // state read from the file is trusted as-is (loc_2388A).
    if (!from_load_flag) {
        sync_point_kind();          // sub_220B3
        update_point_activation();  // sub_22562
        deactivate_points();        // sub_2288C
        update_path_activation();   // sub_2213E
        deactivate_paths();         // sub_2233E
        int ci = state_.chapter_id < MAX_CHAPTERS ? state_.chapter_id : 0;
        memcpy(g_world_points, all_chapter_points_[ci], sizeof(WorldPoint) * MAX_POINTS);
    }

    // loc_2388A: unlink(".\cstldata\LOADED") after use, before the world map loop.
    std::string loaded = cstldata_path(IPC::LOADED);
    if (file_exists(loaded.c_str())) remove(loaded.c_str());

    // sub_21C58(): map and points; the party sprite and overlay come with the loop.
    render_map_and_points();
    video_.present();

    // sub_22A5E(): time limit, then sub_1088D(): chapter clear check.
    if (check_time_limit()) return;
    check_chapter_clear();

    // loc_238B6 / loc_238BD: sub_10010(0), then sub_10010(4).
    check_event_triggers(0);
    if (state_.transition_pending || dos_exit_) return;   // a transition skips sub_10010(4) and sub_22C8E
    check_event_triggers(4);
    if (state_.transition_pending || dos_exit_) return;

    // loc_238C7: sub_22C8E(), the world map loop.
    world_map_loop();
}

// ==== CHP::run: entry point, following main_'s argv model ====
int CHP::run(const char* argv1, const char* argv2) {
    char a1 = (argv1 && argv1[0]) ? argv1[0] : 0;
    const char* chapter_arg = (argv2 && argv2[0]) ? argv2 : nullptr;
    // Convenience: a numeric argv1 such as `genesis2 chp 1` is read as 's 1'.
    if (a1 && isdigit((unsigned char)a1)) { chapter_arg = argv1; a1 = 's'; }

    fprintf(stderr, "CHP: === Module starting (argv[1]='%c', argv[2]=%s) ===\n",
            a1 ? a1 : ' ', chapter_arg ? chapter_arg : "(none)");

    // Main_+393 (0x238D8): argv[1][0] == 't' or 'T' sets byte_80301 = 3 (generator tool).
    in_tool_mode_ = (a1 == 't' || a1 == 'T');

    state_.clear();
    state_.transition_pending = false;
    memset(state_.pending_command, 0, sizeof(state_.pending_command));

    init_display();
    init_mouse();   // main_ -> sub_22BCB -> sub_288B9
    // Restore the cursor whichever path exits (chapter_menu, cancelled select, resume);
    // this is sub_28C27.
    struct MouseGuard { CHP* c; ~MouseGuard() { c->shutdown_mouse(); } } mouse_guard{this};
    load_config();
    init_audio();
    load_fonts();
    load_all_system_sprites();

    if (chapter_arg) {
        chapter_num_ = atoi(chapter_arg);
        if (chapter_num_ <= 0) chapter_num_ = 1;
    }
    state_.chapter_id = (uint8_t)chapter_num_;

    // Main_+1D8 (0x23716): tests argv[1][0] against 'p'/'P' first. The 'p' path ignores
    // argv[2]: a START file makes the select screen rebuild the name, otherwise
    // contibak.tmp is used.
    const bool is_p = (a1 == 'p' || a1 == 'P');

    std::string start = game_path(IPC::START);
    if (is_p && file_exists(start.c_str())) {
        // Main_+1E0..+237 (loc_23729): a START file means a new game.
        // ASM: open("START"), close and unlink, sub_15DB7 (load_blackbox),
        //      `xor eax,eax ; mov edx,esp ; call sub_10C6D` (mode 0, chapter select),
        //      then itoa the result into "<n>.chp" and call sub_232A4 (chapter_menu).
        // argv[2] is not consulted; the name is rebuilt from the selected number.
        fprintf(stderr, "CHP: START detected -> new game\n");
        remove(start.c_str());
        load_blackbox();
        chapter_num_ = show_chapter_select(0);   // sub_10C6D(mode=0)
        if (chapter_num_ < 0 || dos_exit_) return 1; // window closed: skip chapter_menu
        state_.chapter_id = (uint8_t)chapter_num_;
        chapter_menu();                          // sub_232A4
    } else if (is_p) {
        // Main_+208 (loc_237E0): 'p' without START means continue or return from battle.
        std::string loaded = cstldata_path(IPC::LOADED);
        resume_from_save(file_exists(loaded.c_str()));
    } else if (chapter_arg || in_tool_mode_) {
        // Main_+0x3B (0x23579) and loc_238D8: build "\chapdata\<N>.chp" from argv[2],
        // used for argc == 3 with argv[1][0] 's'/'S' and for the numeric convenience form.
        // 't'/'T' (byte_80301 = 3) also reaches the same sub_232A4 loop through main_
        // loc_238CE, because argv[1] is not 'p'; there is no START check on this path.
        if (chapter_arg)
            fprintf(stderr, "CHP: chapter file argv[2]='%s' -> chapter_menu (sub_232A4)\n", chapter_arg);
        else
            fprintf(stderr, "CHP: tool mode -> chapter_menu (sub_232A4, chapter %d)\n", chapter_num_);
        chapter_menu();                          // sub_232A4
    } else {
        // Direct port launch without argv[1] behaves like 'p'.
        std::string loaded = cstldata_path(IPC::LOADED);
        resume_from_save(file_exists(loaded.c_str()));
    }

    // Main_ loc_238DF: `call sub_232A4 ; jmp loc_238DF`, the endless chapter loop.
    // When sub_22C8E loc_22E1B picks the next chapter after a clear, loc_22E5C rebuilds
    // ".\chapdata\<N>.chp" and calls sub_232A4 again. The ending (0x270F) sets dos_exit_
    // through sub_33C99 and sub_204A7("EXIT"), which leaves the loop.
    while (next_chapter_pending_ && !dos_exit_) {
        next_chapter_pending_ = false;
        chapter_num_ = next_chapter_num_;
        state_.chapter_id = (uint8_t)chapter_num_;
        state_.transition_pending = false;
        state_.pending_command[0] = '\0';
        chapter_menu();                  // re-enter sub_232A4
    }

    return dos_exit_ ? 1 : 0;
}