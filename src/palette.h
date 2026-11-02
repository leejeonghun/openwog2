#pragma once
#include <cstdint>

// VGA palette: 256 entries, 6-bit RGB (0-63)
extern uint8_t g_palette[256][3];

// VGA palette I/O emulation
void palette_set_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void palette_get_entry(uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b);
void palette_set_range(uint8_t start, uint8_t end, const uint8_t data[][3]);
void palette_get_range(uint8_t start, uint8_t end, uint8_t data[][3]);
void palette_fade_to_black(int steps);
void palette_fade_from_black(int steps);
void palette_set_all_black();
void palette_set_all_white();
