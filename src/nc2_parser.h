#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ==== NC2 character sprite loader (COMBATII sub_11575 / sub_1198D / sub_10031) ====
//
// `CHARACTE\<name>.NC2` (large units .BCR, cavalry .RCR) is a Softmax sprite container,
// but its layout differs from the 12-byte header of `New Fast&Compressed Sprite Ver3.0`
// handled by SPRParser. The original loader (dasm/COMBATII_code.ASM sub_11575) works as
// follows.
//
// The class comes from the type code (sub_11575 / sub_1198D):
//   class 0  type < 0x12C         .NC2  44 records (11 poses per dir)  head 0x81 (+40)
//   class 1  0x12C <= type < 1F4  .BCR  32 records (8 poses per dir)   head 0x80
//   class 2  type >= 0x1F4        .RCR  32 records (8 poses per dir)   head 0x80
//   Record header size is 10, 13 and 8 bytes respectively. Only class 0 reads the
//   +0x80 flag byte; when it is non-zero, 40 bytes at +0x81 (4 dirs x 2 x 5) hold the
//   occupancy tile table (byte_BA8B0). Classes 1/2 ignore the flag and start records at
//   0x80 (`test di,di; jnz loc_1174B`). Each record then holds:
//     +0x00  u32  sprite data size (sub_11575: `mov ebx, 4; call read_`)
//     +0x04  u16  w   (the loader reads the low byte only, then seeks one byte)
//     +0x06  u16  h
//     +0x08  s16  x offset (low byte only, used as signed 8-bit)
//     +0x0A  s16  y offset
//     +0x0C  s8   silhouette copy x (entry[7], classes 0/1)
//     +0x0D  s8   silhouette copy y (entry[8], classes 0/1)
//     +0x0E  u8   shadow frame index (entry[6], class 1 only - sub_12F2A picks the
//              SHADOW.SYB frame with dword_D6658[entry6*3])
//     +0x0F  u8   tile width (entry[0], class 1 - foot decal repeat count)
//     +0x10  u8   tile height (entry[1], class 1)
//     followed by the sprite data.
//
// Sprite stream (scan by sub_10031, span list drawn by sub_10083):
//     u16 span count
//     per span: u16 x_delta, u8 n_dword, u8 n_tail, then n_dword*4 + n_tail bytes
//   cursor += x_delta; when cursor >= 320 (0x140), cursor -= 320 and fill becomes
//   cursor. Fill is 0xFF (transparent) and the pixel data follows it, so the result is a
//   row-major w*h strip with 0xFF transparent.
struct NC2Frame {
    int w = 0, h = 0;
    int8_t x_off = 0, y_off = 0;   // entry[4]/[5] - sprite top-left relative to the unit
    int8_t sil_x = 0, sil_y = 0;   // entry[7]/[8] - offsets for the silhouette copy
    int shadow_idx = 0;            // entry[6] - dword_D6658 index (class 1: file +0x0E;
                                   //   class 0: sub_11575 hardcodes unk_D66AE[idx] = 0)
    int tile_w = 1, tile_h = 1;    // entry[0]/[1] - foot decal repeat count (class 1: file
                                   //   +0x0F/0x10; class 0: hardcoded 1)
    std::vector<uint8_t> pixels;   // w*h, 0xFF = transparent
};

class NC2Parser {
public:
    static constexpr int DIR_COUNT = 4;
    static constexpr int POSE_COUNT = 11; // 11 poses per direction, 44 records total

    // type_code only selects the class (record layout), as the branch in sub_1198D does.
    bool load(const std::string& filepath, int type_code = 0);
    void clear();

    bool loaded() const { return !filepath_.empty() && count() > 0; }
    int count() const;   // number of filled (dir, pose) entries (44 or 32)

    // Mirrors entry = unk_D66A8 + 13*(11*dir + pose); nullptr when the file lacks the pose.
    const NC2Frame* frame(int dir, int pose) const {
        int i = dir * POSE_COUNT + pose;
        if (dir < 0 || dir >= DIR_COUNT || pose < 0 || pose >= POSE_COUNT ||
            frames_.empty())
            return nullptr;
        const NC2Frame& fr = frames_[(size_t)i];
        return (fr.w > 0 && fr.h > 0) ? &fr : nullptr;
    }

    const std::string& error() const { return error_; }
    const std::string& filepath() const { return filepath_; }

    // 40-byte head table at +0x81 (unk_BA8AD+3 = byte_BA8B0[slot*40]), used by
    // sub_1E61D/sub_1E2EA for attack effects. For direction d and set s (s = 1 when the
    // high word at dword_F0B8A+2 is non-zero or sub_1D6B1 hit_kind is a critical):
    //   [d*10 + s*5 + 0] = WEA_EFFC trail sprite base (-1 = no trail, gated by var_20)
    //   [+1]/[+2]        = trail x/y offset for f8 != 1 (p != 2)  (unk_BA8B1/2)
    //   [+3]/[+4]        = trail x/y offset for f8 == 1 (p == 2)  (unk_BA8B3/4)
    // The index is base + f8 (f8 = p-1, 20817-20819). With no flag (no table) all are -1.
    int8_t head(int i) const { return (i >= 0 && i < 40) ? head_table_[i] : -1; }
    bool has_head_table() const { return has_head_table_; }
    // Test helper: sum of all frame pixels, compared against the Python decoder.
    uint64_t strip_checksum() const;

private:
    // Index = 11*dir + pose (classes 1/2 leave poses 8..10 empty).
    std::vector<NC2Frame> frames_;
    std::string filepath_, error_;
    int8_t head_table_[40] = {};
    bool has_head_table_ = false;
};
