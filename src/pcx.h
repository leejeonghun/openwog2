#pragma once
#include <cstdint>
#include <string>

// Load a PCX file into a backbuffer and optionally load its palette
// Returns true on success
bool pcx_load(const char* filename, uint8_t* buffer, int buf_w, int buf_h);
bool pcx_load_to_buffer(const std::string& filename, uint8_t* buffer, int buf_w, int buf_h);

// Load only the palette from a PCX file
bool pcx_load_palette(const char* filename);
