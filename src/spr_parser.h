#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ==== SPR file parser (Softmax "New Fast&Compressed Sprite Ver3.0") ====
// Ports sub_1D26C (POINT.SPR, ITEM.SPR, TIME_GRA.SYF, ...) and sub_2E533 (CITIZEN2.SPR)
// as disassembled in CHP.ASM.

// sub_10031: patch a 320-wide RLE stream in place to the 480-pixel scratch-canvas stride.
//   Walk the chunks accumulating a 320-column cursor; when it passes the boundary, add
//   0xA0 (160) to that chunk's skip word and subtract 320 from the cursor
//   (`add dx,ax; cmp dx,140h; jl; mov ax,0A0h; add [edi],ax`).
//   The original applies this right after loading every frame set that sub_5F3FD draws on
//   the 480 canvas: ITEM.SPR (sub_18115) and SHADOW / MAP_EFFC / WEA_EFFC / CHR_EFFC /
//   BATTLE.SYB (sub_11BB8 / sub_44AB6 / sub_10420).
void spr_convert_stride_480(uint8_t* rle_data, size_t data_size);

struct SpriteEntry {
    uint16_t word0;       // +0: parameter word (width/attrib)
    uint16_t word2;       // +2: parameter word (height/attrib)
    int16_t  x_offset;    // +4: x rendering offset
    int16_t  y_offset;    // +6: y rendering offset
    uint8_t* pixel_data;  // +8: RLE-encoded pixel data
    int      data_size;   // pixel data byte count

    SpriteEntry() : word0(0), word2(0), x_offset(0), y_offset(0),
                    pixel_data(nullptr), data_size(0) {}
    ~SpriteEntry() { delete[] pixel_data; pixel_data = nullptr; }

    // Move-only semantics
    SpriteEntry(SpriteEntry&& o) noexcept
        : word0(o.word0), word2(o.word2), x_offset(o.x_offset),
          y_offset(o.y_offset), pixel_data(o.pixel_data), data_size(o.data_size) {
        o.pixel_data = nullptr; o.data_size = 0;
    }
    SpriteEntry& operator=(SpriteEntry&& o) noexcept {
        if (this != &o) {
            delete[] pixel_data;
            word0 = o.word0; word2 = o.word2;
            x_offset = o.x_offset; y_offset = o.y_offset;
            pixel_data = o.pixel_data; data_size = o.data_size;
            o.pixel_data = nullptr; o.data_size = 0;
        }
        return *this;
    }
    SpriteEntry(const SpriteEntry&) = delete;
    SpriteEntry& operator=(const SpriteEntry&) = delete;
};

class SPRParser {
public:
    SPRParser() = default;
    ~SPRParser() = default;

    // Load sprites from an SPR/SYF file (sub_1D26C / sub_2E533).
    // is_citizen: true for CITIZEN1/2.SPR (count>>4 and skip 15 sub-frames per sprite).
    bool load(const std::string& filepath, bool is_citizen = false);
    bool load_citizen(const std::string& filepath) { return load(filepath, true); }

    // sub_1A094 (PACK citizen loader): reads count@0x42 (= characters x 16) and loads all
    // 16 frames per character. A character is 16 frames = 4 facings x 4 poses, block order
    // as in PACK (down, left, right, up); index = character*16 + facing_block*4 + pose.
    // sub_2E533 (CHP) keeps only the first frame of each 16-frame group and skips the other
    // 15, so (count>>4) loses the facing/walk poses - battle unit rendering needs this path.
    bool load_citizen_full(const std::string& filepath) { return load_impl(filepath, false, true); }

    int  count() const { return (int)sprites_.size(); }
    bool empty() const { return sprites_.empty(); }
    void clear() { sprites_.clear(); error_.clear(); }

    const SpriteEntry* get(int index) const {
        if (index < 0 || index >= (int)sprites_.size()) return nullptr;
        return &sprites_[index];
    }
    const SpriteEntry* operator[](int index) const { return get(index); }

    const std::string& error() const { return error_; }

    // Apply sub_10031 to every frame (480-stride patch after loading).
    void convert_stride_480();

private:
    // full_frames: read every CITIZEN frame instead of collapsing each 16-frame group.
    bool load_impl(const std::string& filepath, bool is_citizen, bool full_frames);

    std::vector<SpriteEntry> sprites_;
    std::string error_;
};