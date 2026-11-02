#include "spr_parser.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ==== SPR file format (Softmax "New Fast&Compressed Sprite Ver3.0") ====
//
// Standard format (sub_1D26C in CHP.ASM):
//   - Used by POINT.SPR, FONT.SPR, TIME_GRA.SYF, ITEM.SPR, BIGONG*.SPR, SEAL.SPR, STAFF.SPR, etc.
//   - Offset 0x42: uint16_t sprite_count
//   - Offset 0x44: lseek(0x3C, SEEK_CUR) -> data starts at 0x80 (128 bytes)
//   - For each sprite:
//       1. uint32_t data_size   (4 bytes)
//       2. uint16_t word0       (2 bytes)
//       3. uint16_t word2       (2 bytes)
//       4. int16_t  x_offset    (2 bytes)
//       5. int16_t  y_offset    (2 bytes)
//       6. uint8_t  pixel_data[data_size]
//
// CITIZEN format (sub_2E533 in CHP.ASM):
//   - Used by CITIZEN2.SPR, CITIZEN1.SPR
//   - Offset 0x42: uint16_t count_raw -> sprite_count = count_raw >> 4
//   - Offset 0x80: data start
//   - For each sprite: reads 1 main sprite (same 12-byte header + data_size),
//     then skips 15 sub-animation frames (reads uint32 sub_size, lseek(sub_size + 8)).

// sub_10031 verbatim: when the accumulated 320-column cursor passes the boundary, add
// 0xA0 to that chunk's skip word. Cursor and skip wrap at 16 bits like the original
// (`add dx, ax` / `add [edi], ax`).
void spr_convert_stride_480(uint8_t* rle_data, size_t data_size) {
    if (!rle_data || data_size < 2) return;
    const uint16_t chunks = (uint16_t)(rle_data[0] | (rle_data[1] << 8));
    size_t pos = 2;
    uint16_t cursor = 0;
    for (uint16_t i = 0; i < chunks; ++i) {
        if (pos + 4 > data_size) break;
        uint16_t skip = (uint16_t)(rle_data[pos] | (rle_data[pos + 1] << 8));
        cursor = (uint16_t)(cursor + skip);
        if (cursor >= 0x140) {
            skip = (uint16_t)(skip + 0xA0);
            rle_data[pos] = (uint8_t)skip;
            rle_data[pos + 1] = (uint8_t)(skip >> 8);
            cursor = (uint16_t)(cursor - 0x140);
        }
        const size_t payload = (size_t)rle_data[pos + 2] * 4u + rle_data[pos + 3];
        if (pos + 4 + payload > data_size) break;
        cursor = (uint16_t)(cursor + payload);
        pos += 4 + payload;
    }
}

void SPRParser::convert_stride_480() {
    for (SpriteEntry& e : sprites_) {
        if (e.pixel_data && e.data_size > 0)
            spr_convert_stride_480(e.pixel_data, (size_t)e.data_size);
    }
}

bool SPRParser::load(const std::string& filepath, bool is_citizen) {
    return load_impl(filepath, is_citizen, false);
}

bool SPRParser::load_impl(const std::string& filepath, bool is_citizen, bool full_frames) {
    clear();

    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
        error_ = "Cannot open: " + filepath;
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < 0x80) {
        error_ = "File too small: " + filepath;
        fclose(fp);
        return false;
    }

    // Auto-detect the CITIZEN format from the path (case-insensitive). The full_frames
    // path (sub_1A094) never collapses groups, so it is excluded from the detection.
    std::string upper_path = filepath;
    std::transform(upper_path.begin(), upper_path.end(), upper_path.begin(), ::toupper);
    if (!full_frames && upper_path.find("CITIZEN") != std::string::npos) {
        is_citizen = true;
    }

    // Read sprite count at offset 0x42 (sub_1D26C: lseek to 0x42, read 2 bytes)
    if (fseek(fp, 0x42, SEEK_SET) != 0) {
        error_ = "Seek to count failed: " + filepath;
        fclose(fp);
        return false;
    }

    uint16_t count_raw = 0;
    if (fread(&count_raw, 2, 1, fp) != 1) {
        error_ = "Read count failed: " + filepath;
        fclose(fp);
        return false;
    }

    int sprite_count = 0;
    if (is_citizen) {
        sprite_count = count_raw >> 4; // sub_2E533: shr dx, 4
    } else {
        sprite_count = count_raw;      // sub_1D26C: exact count
    }

    fprintf(stderr, "SPR: '%s' format=%s count=%d (raw=%d)\n",
            filepath.c_str(), is_citizen ? "citizen" : (full_frames ? "citizen-full" : "standard"),
            sprite_count, count_raw);

    if (sprite_count <= 0 || sprite_count > 10000) {
        error_ = "Invalid sprite count in " + filepath;
        fclose(fp);
        return false;
    }

    // Data starts at offset 0x80 (sub_1D26C: 0x44 + 0x3C = 0x80)
    if (fseek(fp, 0x80, SEEK_SET) != 0) {
        error_ = "Seek to data start failed: " + filepath;
        fclose(fp);
        return false;
    }

    sprites_.reserve(sprite_count);

    for (int i = 0; i < sprite_count; i++) {
        SpriteEntry entry;

        // 1. Read 4-byte data_size (sub_1D26C: mov ebx, 4; call read_)
        uint32_t data_size = 0;
        if (fread(&data_size, 4, 1, fp) != 1) {
            fprintf(stderr, "SPR: Truncated data_size at sprite %d in '%s'\n", i, filepath.c_str());
            break;
        }

        if (data_size == 0 || data_size > 0x100000) {
            fprintf(stderr, "SPR: warning: Invalid data_size %u at sprite %d in '%s'\n", data_size, i, filepath.c_str());
            break;
        }

        // 2. Read 2-byte word0
        uint16_t word0 = 0;
        if (fread(&word0, 2, 1, fp) != 1) break;

        // 3. Read 2-byte word2
        uint16_t word2 = 0;
        if (fread(&word2, 2, 1, fp) != 1) break;

        // 4. Read 2-byte x_offset (int16_t)
        int16_t x_offset = 0;
        if (fread(&x_offset, 2, 1, fp) != 1) break;

        // 5. Read 2-byte y_offset (int16_t)
        int16_t y_offset = 0;
        if (fread(&y_offset, 2, 1, fp) != 1) break;

        // 6. Allocate and read pixel data
        entry.data_size = (int)data_size;
        entry.word0 = word0;
        entry.word2 = word2;
        entry.x_offset = x_offset;
        entry.y_offset = y_offset;
        entry.pixel_data = new uint8_t[data_size];

        if (fread(entry.pixel_data, 1, data_size, fp) != data_size) {
            fprintf(stderr, "SPR: Truncated pixel data at sprite %d in '%s'\n", i, filepath.c_str());
            delete[] entry.pixel_data;
            entry.pixel_data = nullptr;
            break;
        }

        sprites_.push_back(std::move(entry));

        // CITIZEN format (sub_2E533): Skip 15 sub-animation frames per sprite
        if (is_citizen) {
            bool skip_ok = true;
            for (int s = 0; s < 15; s++) {
                uint32_t sub_size = 0;
                if (fread(&sub_size, 4, 1, fp) != 1) {
                    skip_ok = false;
                    break;
                }
                // Lseek(fd, sub_size + 8, SEEK_CUR) -> 8 bytes header (word0..3) + sub_size pixels
                if (fseek(fp, (long)(sub_size + 8), SEEK_CUR) != 0) {
                    skip_ok = false;
                    break;
                }
            }
            if (!skip_ok) break;
        }
    }

    fclose(fp);
    fprintf(stderr, "SPR: Successfully loaded %d sprites from '%s'\n",
            (int)sprites_.size(), filepath.c_str());
    return !sprites_.empty();
}