#include "portrait.h"
#include <cstdio>

// sub_24BA4. The offset constants below are taken instruction by instruction from
// PACK.ASM 0x27182..0x27253 (see portrait.h for the format).
namespace {
constexpr long kIndexTableOffset = 0x80;
constexpr long kWidthOffsetFromEntry  = 4;  // entry_offset + 4 = width (u16)
// Height follows at entry_offset + 6 with no seek in between.
constexpr long kSkipBytesAfterHeight  = 4;  // skip entry_offset + 8..+11
constexpr uint32_t kMaxReadBytes = 0x36B0;  // 14000 - the fixed size the original reads
}

bool portrait_load(const std::string& path, int portrait_id,
                    uint16_t& out_width, uint16_t& out_height,
                    std::vector<uint8_t>& out_rle_data) {
    out_width = 0;
    out_height = 0;
    out_rle_data.clear();
    if (portrait_id < 0) return false;

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "PORTRAIT: warning: cannot open %s\n", path.c_str());
        return false;
    }

    // 1) Index table: lseek(0x80, SEEK_SET) then lseek(portrait_id*4, SEEK_CUR).
    //    The original calls lseek twice; one fseek to 0x80 + portrait_id*4 is equivalent.
    if (fseek(fp, kIndexTableOffset + (long)portrait_id * 4, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    uint32_t entry_offset = 0;
    if (fread(&entry_offset, sizeof(entry_offset), 1, fp) != 1) {
        fprintf(stderr, "PORTRAIT: warning: index table read failed for id=%d\n", portrait_id);
        fclose(fp);
        return false;
    }

    // 2) Seek to entry_offset + 4 and read width then height back to back.
    //    The original seeks once, then reads width (2) and height (2) with no seek in
    //    between, so height comes from entry_offset + 6.
    if (fseek(fp, (long)entry_offset + kWidthOffsetFromEntry, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    uint16_t w = 0, h = 0;
    if (fread(&w, sizeof(w), 1, fp) != 1 || fread(&h, sizeof(h), 1, fp) != 1) {
        fprintf(stderr, "PORTRAIT: warning: width/height read failed for id=%d\n", portrait_id);
        fclose(fp);
        return false;
    }

    // Range guard, not present in the original: PORTRAIT.SYF holds portraits 0..84 (max
    // 115x138, including 14 blank 7x7 entries). An EVE script id of 85 or more (107/136/213)
    // reads past the index table (a 0 entry or a garbage offset), making w/h nonsense (a
    // 0 entry yields header bytes such as 24902/29811). The original blits unchecked via
    // sub_2B240, so it shows garbage pixels or a stale buffer; the port draws nothing.
    // Valid ids behave identically. The 160x200 bound leaves headroom over the measured
    // 115x138 maximum.
    if (w == 0 || h == 0 || w > 160 || h > 200) {
        fprintf(stderr, "PORTRAIT: warning: id=%d out of range (w/h=%u/%u), skip (no portrait)\n",
                portrait_id, (unsigned)w, (unsigned)h);
        fclose(fp);
        return false;
    }

    // 3) Skip the next four bytes with SEEK_CUR (unknown purpose in the original).
    if (fseek(fp, kSkipBytesAfterHeight, SEEK_CUR) != 0) {
        fclose(fp);
        return false;
    }

    // 4) Read a fixed 0x36B0 (14000) bytes rather than width*height. The data at
    //    entry_offset+12 is a sub_2B240 RLE stream that carries its own length
    //    (chunk_count), so a short read still stops the blitter at the real end. The
    //    original never checks read()'s return value, so neither do we, except to fail
    //    when not a single byte could be read.
    out_rle_data.resize(kMaxReadBytes);
    size_t got = fread(out_rle_data.data(), 1, kMaxReadBytes, fp);
    fclose(fp);

    if (got == 0) {
        out_rle_data.clear();
        return false;
    }
    out_rle_data.resize(got);

    out_width = w;
    out_height = h;
    return true;
}
