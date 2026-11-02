#include "nc2_parser.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr int kWrap = 0x140;       // sub_10031: `cmp dx, 140h`

// sub_11575 / sub_1198D: type code -> (file head offset, record field bytes, poses per dir)
struct ClassLayout {
    size_t start_with_flag;
    size_t start_no_flag;
    int field_bytes;
    int poses_per_dir;
    bool reads_flag;
};

ClassLayout class_layout(int type_code) {
    if (type_code < 0x12C) return {0xA9, 0x81, 10, 11, true};   // regular (.NC2)
    if (type_code < 0x1F4) return {0x80, 0x80, 13, 8, false};   // large (.BCR)
    return {0x80, 0x80, 8, 8, false};                           // cavalry (.RCR/.MAR)
}

// Decode the span list into a w*h strip (0xFF = transparent).
//   cursor += x_delta; when cursor >= 320, cursor -= 320 and fill = cursor
bool decode_strip(const uint8_t* data, size_t size, NC2Frame& fr) {
    if (size < 2 || fr.w <= 0 || fr.h <= 0) return false;
    const size_t total = (size_t)fr.w * (size_t)fr.h;
    const uint16_t runs = (uint16_t)(data[0] | (data[1] << 8));
    fr.pixels.assign(total, 0xFF);
    size_t p = 2, out = 0;
    int cursor = 0;
    for (uint16_t r = 0; r < runs; r++) {
        if (p + 4 > size) return false;
        const uint16_t delta = (uint16_t)(data[p] | (data[p + 1] << 8));
        const int n_dw = data[p + 2];
        const int n_tail = data[p + 3];
        p += 4;
        const size_t len = (size_t)n_dw * 4 + (size_t)n_tail;
        if (p + len > size) return false;
        cursor += delta;
        int fill = delta;
        if (cursor >= kWrap) {
            cursor -= kWrap;
            fill = cursor;
        }
        out += (size_t)fill;
        for (size_t b = 0; b < len; b++) {
            if (out < total) fr.pixels[out] = data[p + b];
            out++;
        }
        p += len;
        cursor += (int)len;
    }
    return true;
}

} // namespace

int NC2Parser::count() const {
    int n = 0;
    for (const auto& fr : frames_)
        if (fr.w > 0 && fr.h > 0) n++;
    return n;
}

void NC2Parser::clear() {
    frames_.clear();
    filepath_.clear();
    error_.clear();
}

bool NC2Parser::load(const std::string& filepath, int type_code) {
    clear();
    filepath_ = filepath;
    const ClassLayout layout = class_layout(type_code);
    const int kRecordHeader = 4 + layout.field_bytes;

    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
        error_ = "Cannot open: " + filepath;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size < 0x84) {
        error_ = "File too small: " + filepath;
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> buf((size_t)file_size);
    const size_t got = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    if (got != buf.size()) {
        error_ = "Short read: " + filepath;
        return false;
    }
    buf.resize(got);

    const uint16_t nrec = (uint16_t)(buf[0x42] | (buf[0x43] << 8));
    if (nrec == 0 || nrec > 64) {
        error_ = "Invalid record count in " + filepath;
        return false;
    }
    // sub_11575: only class 0 reads the +0x80 flag and the 40-byte occupancy tile table.
    size_t pos = layout.reads_flag ? (buf[0x80] ? layout.start_with_flag : layout.start_no_flag)
                                   : layout.start_no_flag;
    // Head table (unk_BA8AD..): no table when the flag is 0, so every entry stays -1.
    memset(head_table_, 0xFF, sizeof(head_table_));
    has_head_table_ = false;
    if (layout.reads_flag && buf[0x80] && buf.size() >= 0x81 + 40) {
        memcpy(head_table_, buf.data() + 0x81, 40);
        has_head_table_ = true;
    }

    frames_.assign((size_t)DIR_COUNT * POSE_COUNT, NC2Frame{});
    for (uint16_t i = 0; i < nrec; i++) {
        if (pos + kRecordHeader > buf.size()) {
            error_ = "Truncated record header in " + filepath;
            break;
        }
        const uint8_t* f = buf.data() + pos;
        const uint32_t dsize = (uint32_t)f[0] | ((uint32_t)f[1] << 8) |
                               ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
        NC2Frame fr;
        fr.w = f[4];
        fr.h = f[6];
        fr.x_off = (int8_t)f[8];
        fr.y_off = (int8_t)f[10];
        if (layout.field_bytes >= 10) {  // entry[7]/[8] - classes 0/1 only
            fr.sil_x = (int8_t)f[12];
            fr.sil_y = (int8_t)f[13];
        }
        if (layout.field_bytes == 13) {  // class 1 (.BCR) only (header 4+13 = 17 bytes):
            fr.shadow_idx = f[14];       //   entry[6] = record +0x0E (shadow frame)
            fr.tile_w = f[15];           //   entry[0] = record +0x0F (tile width)
            fr.tile_h = f[16];           //   entry[1] = record +0x10 (tile height)
        }
        pos += kRecordHeader;

        if (dsize == 0 || pos + dsize > buf.size() ||
            fr.w <= 0 || fr.h <= 0 || (size_t)fr.w * fr.h > 0x40000) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Bad record %u in %s (size=%u %dx%d)",
                     (unsigned)i, filepath.c_str(), (unsigned)dsize, fr.w, fr.h);
            error_ = msg;
            break;
        }
        if (!decode_strip(buf.data() + pos, dsize, fr)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Bad span stream in record %u of %s", (unsigned)i,
                     filepath.c_str());
            error_ = msg;
            break;
        }
        pos += dsize;
        // Classes 1/2 store only 8 poses per direction (the `var_8 += 3` skip in sub_11575).
        const int dir = i / layout.poses_per_dir;
        const int pose = i % layout.poses_per_dir;
        const size_t slot = (size_t)(dir * POSE_COUNT + pose);
        if (slot < frames_.size()) frames_[slot] = std::move(fr);
    }

    if (count() == 0) {
        if (error_.empty()) error_ = "No frames decoded: " + filepath;
        return false;
    }
    return true;
}

uint64_t NC2Parser::strip_checksum() const {
    uint64_t sum = 0;
    for (const auto& fr : frames_)
        for (uint8_t p : fr.pixels) sum += p;
    return sum;
}

