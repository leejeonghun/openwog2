#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// ==== Embedded resource loader for GENESIS2/*.EXE (DOS4GW LE) ====
// Each module parses the original LE object/page tables directly and reads resources by
// IDA virtual address (VA), so no pre-extracted .bin files are needed.
enum class ExeId { Pack, Chp, Ttl };

// How the resource length is determined.
enum class ExeResLength {
    Fixed,   // fixed_size bytes
    Rle,     // [u16 chunk_count] + ([u16 skip][u8 dwords][u8 bytes] + payload)* - self-delimiting
    Sprite,  // [u16 width][u16 height][width*height bytes] (the 4-byte header is returned too)
};

// Reads the resource at `va` of `exe` into `out`; clears `out` and returns false on failure
// (Fixed also fails when fixed_size <= 0).
bool exe_resource_load(ExeId exe, uint32_t va, ExeResLength len,
                       std::vector<uint8_t>& out, size_t fixed_size = 0);

// File name for log and error messages.
const char* exe_resource_file_name(ExeId exe);
