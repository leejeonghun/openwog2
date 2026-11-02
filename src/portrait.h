#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ==== PORTRAIT.SYF loader (PACK.ASM sub_24BA4, 0x27182..0x27253) ====
//
// A shared resource: other executables (CHP.EXE) use the same SYF portrait format, so
// this loader lives outside the Pack class.
//
// File format, ported instruction by instruction from sub_24BA4:
//  - At file offset 0x80 a table of uint32 offsets follows, four bytes per entry. Entry
//    portrait_id (0-based) is the portrait's absolute byte offset (entry_offset):
//        lseek(0x80); lseek(portrait_id*4, CUR); read(&entry_offset, 4);
//  - Seek to entry_offset + 4 and read uint16 width then uint16 height with no seek in
//    between, so width comes from +4 and height from +6.
//  - Skip four bytes at entry_offset + 8..+11 (purpose unknown, probably x/y offsets but
//    never used) with SEEK_CUR; the data starts at entry_offset + 12.
//  - The data is not width*height raw pixels: the original unconditionally reads a fixed
//    0x36B0 (14000) bytes and ignores the return value, so a short file leaves the rest
//    of unk_16E78C holding the previous call's data. sub_25F21 renders that buffer at
//    loc_2656A via "call sub_2B240", the same Softmax RLE parser used for every other
//    sprite (uint16 chunk_count + [uint16 skip, uint8 dword_count, uint8 byte_count,
//    dword_count*4+byte_count bytes] repeated), not sub_2B2A1 (raw + 0xFF transparent,
//    mouse cursor only). So entry_offset+12 begins a self-terminating RLE stream, and the
//    width/height read above only drive placement (e.g. Y = 199 - height); they are never
//    passed to the blitter.
//
// Like the original, the loader reads up to 0x36B0 (14000) bytes from entry_offset+12
// (fewer when the file ends first) into out_rle_data, which can go straight to
// Pack::blit_sprite_rle().
//
// Returns true on success (out_width/out_height/out_rle_data filled). False when
// portrait.syf cannot be opened or the index table / width / height cannot be read.
// It also returns false when w/h fall outside the valid portrait range (1..160 x 1..200,
// measured maximum 115x138); that guard does not exist in the original, which blits
// unchecked and therefore draws garbage or a stale buffer for out-of-range IDs (EVE uses
// 107/136/213, but the file only holds 0..84). The port draws nothing in that case;
// valid IDs behave byte-identically.
bool portrait_load(const std::string& path, int portrait_id,
                    uint16_t& out_width, uint16_t& out_height,
                    std::vector<uint8_t>& out_rle_data);
