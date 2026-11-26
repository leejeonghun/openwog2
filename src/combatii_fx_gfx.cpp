// combatii_fx_gfx: low-level graphics and DAC primitives used by the magic effects.
//   sub_5F570  expand (pixel duplication) blit - growing Barrier/Black Hole/Amagedon frames
//   sub_5F611  shrink (pixel skipping) blit    - shrinking frames of the same group
//   sub_5F945  DAC range -> palette buffer     (snapshot taken before a fade)
//   sub_5F9E9  palette buffer -> DAC range     (restore the original palette)
//   sub_5FA2C  palette buffer * mul% -> DAC range (brightness step)
//   sub_5FC21  palette buffer - source (accumulate a delta)
//   sub_5623E  set the ramp reference colour (word_11233C = high word of unk_11233A)
//   sub_56245  build the ramp palette; calls 5FC21 + 5FD1C (pal_ramp_apply)
//   sub_562C1  advance the ramp one 3% step (word_7550C/7550E state) (pal_ramp_step)
//   sub_5F9AE  is the same subroutine as sub_5F9E9 (palette buffer -> DAC);
//              pal_load_range covers both.
//   sub_5FD1C  DAC[i] = master[i] + delta[i]*mul/100 - lives inside pal_ramp_apply and
//              pal_shift_fade.
//   The "5F945 -> delta -> 5FC21 -> 5FD1C" chain of 276B9/2799A/27C5A is done in one call
//              by pal_shift_fade.
//   sub_5F3FD  is already ported as blit_sprite_480 in combatii_map.cpp with the same
//              convention (u16 chunk count + chunks {u16 skip, u8 dword count, u8 byte count}).
//
// Shared precondition: 5F570/5F611 take [w u16][h u16] + w*h pixels (0xFF = transparent) and
//   write to the 480-stride canvas address returned by 5EB00. The port uses the backbuffer as
//   the canvas (same convention as blit_sprite_480) and drops off-screen columns. Transparent
//   pixels are NOT written - the original leaves those cells alone too, so the background
//   already drawn shows through.
#include "combatii.h"
#include "combatii_internal.h"
#include "palette.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// sub_5EB00 equivalent: canvas coords -> backbuffer pixel. Off-screen is dropped.
inline void fx_gfx_put(uint8_t* bb, int x, int y, uint8_t px) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    bb[(size_t)y * (size_t)SCREEN_W + (size_t)x] = px;
}

inline int fx_gfx_word(const uint8_t* p) { return (int)(p[0] | (p[1] << 8)); }

} // namespace

// sub_5F570(eax = x, edx = y, ecx = target width, [esp+8] = target height,
//           ebx = [w][h] + pixels): expand only.
//   Accumulates the step (var_4/var_8 = source size * 100000 / target size) to replicate each
//   source column and row across several destination pixels:
//     - Column: for every source column it writes the same pixel until acc_x >= 0 (at least
//       once), then does acc_x -= step and acc_x -= 100000 before moving to the next column.
//     - Row: the same rule decides how many destination rows each source row covers.
//   The total pixel count does not match the target size exactly (original rounding).
void CombatII::blit_scale_expand(int x, int y, const uint8_t* data, int dst_w, int dst_h) {
    if (!data || dst_w <= 0 || dst_h <= 0) return;
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    const int w = fx_gfx_word(data);
    const int h = fx_gfx_word(data + 2);
    if (w <= 0 || h <= 0 || w > SCREEN_W || h > SCREEN_H) return;
    const uint8_t* src = data + 4;
    const int step_x = w * 100000 / dst_w;      // var_4
    const int step_y = h * 100000 / dst_h;      // var_8
    if (step_x <= 0 || step_y <= 0) return;

    int acc_y = -100000;                        // edi
    int src_row = 0;
    int out_row = 0;
    for (int r = 0; r < h && out_row < dst_h; ++r) {
        // Repeat one source row into destination rows until acc_y >= 0 (at least once).
        for (;;) {
            int acc_x = -100000;                // esi
            int src_col = 0;
            int out_col = 0;
            for (int c = 0; c < w && out_col < dst_w; ++c) {
                // loc_5F5CD: write, acc_x += step, and repeat the same pixel while negative.
                int guard = 0;
                for (;;) {
                    const uint8_t px = src[(size_t)src_row * (size_t)w + (size_t)src_col];
                    if (px != 0xFF) fx_gfx_put(bb, x + out_col, y + out_row, px);
                    ++out_col;
                    acc_x += step_x;
                    if (acc_x >= 0 || ++guard > 0x1000) break;
                }
                acc_x -= step_x;
                acc_x -= 100000;
                if (src_col + 1 < w) ++src_col; // inc ebx
            }
            ++out_row;
            acc_y += step_y;
            if (acc_y >= 0 || out_row >= dst_h) break;
        }
        acc_y -= step_y;
        acc_y -= 100000;
        if (src_row + 1 < h) ++src_row;
    }
}

// sub_5F611(eax = x, edx = y, ecx = target width, [esp+8] = target height,
//           ebx = [w][h] + pixels): shrink only.
//   For every source column/row it subtracts 100000 from acc and writes one destination pixel
//   only when the result is >= 0 (otherwise it adds step and moves on). The written side never
//   restores acc, so the original rounding quirk (an occasional duplicated neighbouring row)
//   is preserved.
void CombatII::blit_scale_shrink(int x, int y, const uint8_t* data, int dst_w, int dst_h) {
    if (!data || dst_w <= 0 || dst_h <= 0) return;
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    const int w = fx_gfx_word(data);
    const int h = fx_gfx_word(data + 2);
    if (w <= 0 || h <= 0 || w > SCREEN_W || h > SCREEN_H) return;
    const uint8_t* src = data + 4;
    const int step_x = w * 100000 / dst_w;
    const int step_y = h * 100000 / dst_h;
    if (step_x <= 0 || step_y <= 0) return;

    int acc_y = step_y;
    int src_row = 0;
    int out_row = 0;
    for (int r = 0; r < h; ++r) {
        acc_y -= 100000;
        if (acc_y >= 0) {                       // loc_5F672: emit this row as a destination row
            int acc_x = step_x;
            int out_col = 0;
            for (int c = 0; c < w; ++c) {
                acc_x -= 100000;
                if (acc_x >= 0) {
                    const uint8_t px = src[(size_t)src_row * (size_t)w + (size_t)c];
                    // The original has no clip and overruns the columns for a very small target,
                    // so the port clamps to the target width only here.
                    if (px != 0xFF && out_col < dst_w) fx_gfx_put(bb, x + out_col, y + out_row, px);
                    ++out_col;
                } else {
                    acc_x += step_x;
                }
            }
            ++out_row;
            if (out_row >= dst_h) break;
        } else {                                // skip one source row
            if (src_row + 1 < h) ++src_row;
            acc_y += step_y;
        }
    }
}

// sub_5F945(eax = start, edx = end(exclusive), ebx = buffer): copy the current DAC values for
//   [start,end) into the buffer. The original reads the VGA DAC through ports 3C7h/3C9h
//   (6-bit RGB).
void CombatII::pal_snapshot_range(int start, int end, uint8_t* out) {
    if (!out) return;
    if (start < 0) start = 0;
    if (end > 256) end = 256;
    for (int i = start; i < end; ++i) {
        uint8_t r = 0, g = 0, b = 0;
        palette_get_entry((uint8_t)i, r, g, b); // out 3C9h, three channels
        out[i * 3 + 0] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
}

// sub_5F9E9(eax = start, edx = end(exclusive), ebx = buffer): write the buffer into DAC
//   [start,end). 241F7/24466 call `5F9E9(0, 100h, unk_12824C)` every frame to restore the
//   original palette.
void CombatII::pal_load_range(int start, int end, const uint8_t* src) {
    if (!src) return;
    if (start < 0) start = 0;
    if (end > 256) end = 256;
    for (int i = start; i < end; ++i) {
        palette_set_entry((uint8_t)i, src[i * 3 + 0], src[i * 3 + 1], src[i * 3 + 2]); // out 3C9h
    }
}

// sub_20021(eax = name): load MAGICSPR\<name> (.rpl) and program the DAC entries it lists.
//   Format: u16 first, u16 last, then one R,G,B triple per index in [first,last]. Values are
//   6-bit DAC values with no conversion; sub_5F986 writes 3C8h/3C9h directly.
//   sub_20021 reads the channels one byte at a time and does not check the result, so a file
//   that ends early repeats the last channel values. DEATH.RPL and BARRIER.RPL hold 15 triples
//   for 240..255 and DARKZONE.RPL/ENERGY.RPL hold 13 for 240..253; the missing index is outside
//   the bands the drivers scroll.
bool CombatII::battle_rpl_apply(const char* name) {
    if (!name) return false;
    const std::string path = game_path((std::string("MAGICSPR/") + name).c_str());
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 20021 %s not found\n", name);
        return false;
    }
    uint8_t hdr[4] = {};
    if (fread(hdr, 1, sizeof hdr, fp) != sizeof hdr) { fclose(fp); return false; }
    const int first = hdr[0] | (hdr[1] << 8);
    const int last  = hdr[2] | (hdr[3] << 8);
    uint8_t rgb[3] = {0, 0, 0};
    for (int i = first; i <= last; ++i) {
        for (int c = 0; c < 3; ++c) {
            uint8_t v = 0;
            if (fread(&v, 1, 1, fp) == 1) rgb[c] = v;   // short read leaves the old channel
        }
        palette_set_entry((uint8_t)i, rgb[0], rgb[1], rgb[2]);   // sub_5F986
    }
    fclose(fp);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 20021 %s first=%d last=%d\n", name, first, last);
    return true;
}

// sub_601E5(eax = start, dl = len): register a DAC scroll slot (byte_128564/byte_12820C).
//   len > 0 uses [start, start+len); len < 0 uses [start+len, start). The slot stores the signed
//   len as both the rotation length (+2) and the initial counter (+3), then snapshots the range
//   into its buffer with 5F945.
void CombatII::pal_scroll_begin(int start, int len) {
    PalScrollSlot s;
    s.len = (int)(int8_t)len;
    s.counter = s.len;
    const uint8_t st = (uint8_t)(start & 0xFF);
    const uint8_t en = (uint8_t)(st + (int8_t)len);
    if (s.len > 0) { s.start = st; s.end = en; }
    else           { s.start = en; s.end = st; }
    wait_retrace(1);                                // sub_5F945 waits before reading DAC
    pal_snapshot_range(s.start, s.end, s.buf);      // sub_5F945
    pal_scroll_slots_.push_back(s);
}

// sub_602E3: for each active slot rotate its len colours by one and upload the range with 5F9AE.
//   The counter ping-pongs: with len > 0 the first len frames shift upward (e[i] = e[i-1],
//   e[0] = old e[len-1]) and the next len frames shift downward; len < 0 starts in the opposite
//   direction (loc_6041B). The counter steps toward zero and reloads as -len when it was
//   positive, +len when it was not (loc_6037A / loc_604F7). Processing stops at the first free
//   slot, as in loc_602ED.
void CombatII::pal_scroll_step() {
    for (size_t n = 0; n < pal_scroll_slots_.size(); ++n) {
        PalScrollSlot& s = pal_scroll_slots_[n];
        if (s.len == 0) break;                          // `test dl,dl; jz loc_60533`
        const int count = (s.len < 0) ? -s.len : s.len; // abs(len)
        if (count > 16) break;                          // unk_127EDC holds 16 colours per slot
        if (s.start < s.end && s.start + count <= 256) {
            const int base = s.start;
            const bool upward = ((s.len < 0) != (s.counter > 0));
            uint8_t tmp[3];
            if (upward) {                               // loc_60341: e[i] = e[i-1]
                memcpy(tmp, s.buf + (base + count - 1) * 3, 3);
                for (int i = count - 1; i >= 1; --i)
                    memcpy(s.buf + (base + i) * 3, s.buf + (base + i - 1) * 3, 3);
                memcpy(s.buf + base * 3, tmp, 3);
            } else {                                    // loc_603B2: e[i-1] = e[i]
                memcpy(tmp, s.buf + base * 3, 3);
                for (int i = 0; i < count - 1; ++i)
                    memcpy(s.buf + (base + i) * 3, s.buf + (base + i + 1) * 3, 3);
                memcpy(s.buf + (base + count - 1) * 3, tmp, 3);
            }
        }
        if (s.counter > 0) {
            if (--s.counter == 0) s.counter = -s.len;   // loc_6037A
        } else {
            if (++s.counter == 0) s.counter = s.len;    // loc_604F7
        }
        wait_retrace(1);                                // sub_5F9AE waits before the DAC write
        pal_load_range(s.start, s.end, s.buf);          // sub_5F9AE
    }
}

// sub_60275: re-read every registered slot's range from the current DAC. sub_197CA calls this
//   after it rewrites the palette for the time of day, so a scroll in progress rotates the dimmed
//   colours instead of uploading its pre-dim snapshot.
void CombatII::pal_scroll_resync() {
    for (auto& s : pal_scroll_slots_) {
        wait_retrace(1);                            // sub_5F945 waits before reading DAC
        pal_snapshot_range(s.start, s.end, s.buf);
    }
}

// sub_602BF: free the last slot (count--, len = 0). The DAC is deliberately not restored; the
//   rotated band stays as-is until another palette write (e.g. sub_197CA) replaces it.
void CombatII::pal_scroll_end() {
    if (pal_scroll_slots_.empty()) return;
    pal_scroll_slots_.back().len = 0;
    pal_scroll_slots_.pop_back();
}

// sub_5FFB5(eax = start, dl = delta): register a unidirectional DAC cycle slot
//   (byte_128568 / unk_1281DC / unk_127A4C). delta > 0 uses [start, start+delta); delta <= 0 uses
//   [start+delta, start). Snapshots the range from DAC with sub_5F945.
void CombatII::pal_cycle_begin(int start, int delta) {
    PalCycleSlot s;
    s.delta = (int)(int8_t)delta;
    const uint8_t st = (uint8_t)(start & 0xFF);
    const uint8_t en = (uint8_t)(st + (int8_t)delta);
    if (s.delta > 0) { s.start = st; s.end = en; }
    else             { s.start = en; s.end = st; }
    wait_retrace(1);                                // sub_5F945 waits before reading DAC
    pal_snapshot_range(s.start, s.end, s.buf);      // sub_5F945
    pal_cycle_slots_.push_back(s);
}

// sub_600D4: for each active cycle slot, rotate its colours by one in a single direction and
//   upload the range with sub_5F9AE. Processing stops at the first slot with delta == 0.
void CombatII::pal_cycle_step() {
    for (size_t n = 0; n < pal_cycle_slots_.size(); ++n) {
        PalCycleSlot& s = pal_cycle_slots_[n];
        if (s.delta == 0) break;                    // `test dl, dl; jz loc_601DC`
        const int count = (s.delta < 0) ? -s.delta : s.delta;
        if (count > 16) break;                      // unk_127A4C holds 16 colours (0x30 bytes) per slot
        if (s.start < s.end && s.start + count <= 256) {
            const int base = s.start;
            uint8_t tmp[3];
            if (s.delta > 0) {                      // loc_60113: e[i] = e[i-1], e[0] = old e[count-1]
                memcpy(tmp, s.buf + (base + count - 1) * 3, 3);
                for (int i = count - 1; i >= 1; --i)
                    memcpy(s.buf + (base + i) * 3, s.buf + (base + i - 1) * 3, 3);
                memcpy(s.buf + base * 3, tmp, 3);
            } else {                                // loc_6014E: e[i-1] = e[i], e[count-1] = old e[0]
                memcpy(tmp, s.buf + base * 3, 3);
                for (int i = 0; i < count - 1; ++i)
                    memcpy(s.buf + (base + i) * 3, s.buf + (base + i + 1) * 3, 3);
                memcpy(s.buf + (base + count - 1) * 3, tmp, 3);
            }
        }
        wait_retrace(1);                            // sub_5F9AE waits before the DAC write
        pal_load_range(s.start, s.end, s.buf);      // sub_5F9AE
    }
}

// sub_6004E: re-read every registered cycle slot's range from the current DAC.
void CombatII::pal_cycle_resync() {
    for (auto& s : pal_cycle_slots_) {
        wait_retrace(1);                            // sub_5F945 waits before reading DAC
        pal_snapshot_range(s.start, s.end, s.buf);
    }
}

// sub_49A3E(eax = start, edx = end, ebx = from, ecx = to, [esp+8] = step): crossfade the DAC
//   from `from` to `to`. The routine copies `to` into unk_FABE0, turns that copy into
//   delta = to - from through sub_5FC21 (`sub [eax], cl`, one byte per channel), then calls
//   sub_5FD1C for pct = 0, step, 2*step, ... <= 100. 5FD1C waits for the vertical retrace, so
//   every step is one visible frame and writes
//     DAC[i] = from[i] + (int8_t)(uint8_t)(to[i] - from[i]) * pct / 100
//   (pct clamped to 100, idiv truncating toward zero, result truncated to a byte).
//   The port presents the backbuffer with the new DAC values because the original programs
//   3C8h/3C9h directly, so the palette change is visible without a new frame.
void CombatII::pal_crossfade(int start, int end, const uint8_t* from, const uint8_t* to, int step_pct) {
    if (!from || !to) return;
    if (start < 0) start = 0;
    if (end > 256) end = 256;
    if (step_pct <= 0) step_pct = 1;
    for (int p = 0; p <= 100; p += step_pct) {
        for (int i = start; i < end; ++i) {
            uint8_t rgb[3];
            for (int c = 0; c < 3; ++c) {
                const uint8_t delta = (uint8_t)(to[i * 3 + c] - from[i * 3 + c]);
                rgb[c] = (uint8_t)(from[i * 3 + c] + (int)(int8_t)delta * p / 100);
            }
            palette_set_entry((uint8_t)i, rgb[0], rgb[1], rgb[2]);
        }
        video_.present();
        if (should_exit_) return;
        wait_retrace(1);                             // sub_5FD1C -> sub_5E943 (vertical retrace)
    }
}

// sub_1E565(eax = mode): full-screen red flash. One (3Fh,0,0) triple is replicated over all 256
//   palette entries and crossfaded with sub_49A3E against the master palette (unk_12824C):
//     mode == 2: master -> red (step 32h), red -> master (step 19h)
//     every mode: master -> red (step 14h), red -> master (step 0Ah)
//   so mode 1 shows 17 steps (6 rising + 11 falling) and mode 2 adds 8 more.
//   Call sites: sub_2EF4A after the damage roll when type > 1 (attack level 2/3), and sub_1E61D
//   with the high word of its var_20 (always 0, so that one never flashes).
void CombatII::battle_fx_1e565(int mode) {
    if ((int16_t)mode == 0) return;
    // sub_1E565 crossfades against unk_12824C itself: the live master palette (weather and
    // time of day included), not the one-time dialogue shade snapshot.
    const uint8_t* master = &battle_master_palette_[0][0];
    uint8_t red[256 * 3];
    for (int i = 0; i < 256; ++i) {
        red[i * 3 + 0] = 0x3F;
        red[i * 3 + 1] = 0x00;
        red[i * 3 + 2] = 0x00;
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: 1E565 red flash mode=%d\n", mode);
    if (mode == 2) {
        pal_crossfade(0, 0x100, master, red, 0x32);
        pal_crossfade(0, 0x100, red, master, 0x19);
    }
    pal_crossfade(0, 0x100, master, red, 0x14);
    pal_crossfade(0, 0x100, red, master, 0x0A);
}

// sub_284A3 (21944/272AD tail) and sub_2FD03 (F1474 idx 18) build the same temporary palette:
//   256 copies of the three bytes at unk_12824F (= unk_12824C + 3, master palette entry 1,
//   white 3Fh/3Fh/3Fh in GENESIS.PAL), then run 49A3E from master to that solid white and back.
//   sub_284A3 uses step 2 on the way in and the finished loop counter 100h on the way out,
//   which 49A3E executes as a single white hold frame before the 5F9AE(master) commit;
//   sub_2FD03 uses 0Ah and 14h with its camera clamp between the two halves.
static void white_palette_284a3(const uint8_t* master, uint8_t* white) {
    for (int i = 0; i < 256; ++i) {
        white[i * 3 + 0] = master[3 + 0];
        white[i * 3 + 1] = master[3 + 1];
        white[i * 3 + 2] = master[3 + 2];
    }
}

void CombatII::battle_fx_white_flash_up(int up_step) {
    // sub_284A3 builds its white from unk_12824F and crossfades against unk_12824C (master).
    const uint8_t* master = &battle_master_palette_[0][0];
    uint8_t white[256 * 3];
    white_palette_284a3(master, white);
    if (battle_trace_) fprintf(stderr, "COMBAT: white flash up=%d\n", up_step);
    pal_crossfade(0, 0x100, master, white, up_step);   // 49A3E(master -> white)
}

void CombatII::battle_fx_white_flash_down(int down_step) {
    // sub_284A3 builds its white from unk_12824F and crossfades against unk_12824C (master).
    const uint8_t* master = &battle_master_palette_[0][0];
    uint8_t white[256 * 3];
    white_palette_284a3(master, white);
    if (battle_trace_) fprintf(stderr, "COMBAT: white flash down=%d\n", down_step);
    pal_crossfade(0, 0x100, white, master, down_step); // 49A3E(white -> master)
    wait_retrace(1);                                   // 5F9AE waits before writing the DAC
    pal_load_range(0, 0x100, master);                  // 5F9AE(0,100h,unk_12824C)
    video_.present();
}

void CombatII::battle_fx_white_flash(int up_step, int down_step) {
    battle_fx_present_454b3();                         // sub_45559 (sub_284A3 only)
    battle_fx_white_flash_up(up_step);
    battle_fx_white_flash_down(down_step);
}

// sub_5FA2C(eax = start, edx = mul<<16, ebx = buffer, ecx = end): DAC[i] = buffer[i] * mul / 100.
//   Leaves the end index in byte_755E8 (the ramp position marker) -> pal_ramp_mark_.
void CombatII::pal_scale_range(int start, int end, const uint8_t* src, int mul_pct) {
    if (!src) return;
    if (start < 0) start = 0;
    if (end > 256) end = 256;
    const int mul = mul_pct;
    for (int i = start; i < end; ++i) {
        palette_set_entry((uint8_t)i,
                          (uint8_t)(src[i * 3 + 0] * mul / 100),
                          (uint8_t)(src[i * 3 + 1] * mul / 100),
                          (uint8_t)(src[i * 3 + 2] * mul / 100));
    }
    pal_ramp_mark_ = end;                       // byte_755E8
}

// sub_5FC21(eax = start, edx = end(exclusive), ebx = source palette, ecx = delta buffer):
//   delta[i] -= source[i]. One-byte subtraction per channel, so negatives wrap into 0..255
//   (the original's `sub [eax], cl`).
void CombatII::pal_sub_range(int start, int end, const uint8_t* src, uint8_t* delta) {
    if (!src || !delta) return;
    if (start < 0) start = 0;
    if (end > 256) end = 256;
    for (int i = start * 3; i < end * 3; ++i) {
        delta[i] = (uint8_t)(delta[i] - src[i]);
    }
}

// loc_27B88 (palette cycle of the 2799A stage 2): advances only the R channel of every buffer
//   entry by `if (value < 0x3E) value + 1 else value - 0x0A`. 2799A runs the master copy taken
//   by 5F945 through this loop and uploads it with 5F9AE, so each frame the red channel walks
//   0..0x3D and then drops ten steps at 0x3E (as in the original).
void CombatII::pal_cycle_entries(uint8_t* buf, int count) {
    if (!buf) return;
    for (int i = 0; i < count; ++i) {
        uint8_t& r = buf[i * 3];
        r = (r >= 0x3E) ? (uint8_t)(r - 0x0A) : (uint8_t)(r + 1);
    }
}

// sub_5623E(eax = colour): the palette colour index the ramp mixes towards (word_11233C).
//   5666E/56896/56BD4 pass 0x14 and 57F18 passes 0x29 - the fire and ice ramp colours.
//   word_11233C is the high word of the dword unk_11233A, so the value sub_56245 reads through
//   `unk_11233A >> 16` is this colour (the ramp palette is filled with that single colour).
//   The port also resets the position to 0 here; the original carries the global over, but
//   starting at 0 per effect is deterministic - every driver restores the palette with
//   pal_ramp_apply(0) when it ends.
void CombatII::pal_ramp_set_width(int color_index) {
    pal_ramp_color_ = color_index & 0xFF;
    pal_ramp_pos_ = 0;                          // word_7550C
    pal_ramp_dir_ = 0;                          // word_7550E
}

// sub_56245(eax = target %, edx = start %, ebx = +-3): mixes the master palette pct% towards
//   colour K and writes it back to the DAC.
//   (1) temp[0..0x100) = master[K] (every entry the same colour)   - 0x100 movsw/movsb
//   (2) sub_5FC21(0, 100h, master, temp) -> temp[i] = master[K] - master[i] (signed byte)
//   (3) sub_5FD1C(0, 0FEh, master, temp, pct) -> DAC[i] = master[i] + temp[i]*pct/100
//   So pct = 0 is the original palette and pct = 100 tints the whole palette with colour K
//   (the fire glow).
void CombatII::pal_ramp_apply(int pct) {
    uint8_t* master = &battle_master_palette_[0][0];    // unk_12824C
    const int k = pal_ramp_color_ & 0xFF;
    const uint8_t kr = master[k * 3 + 0];
    const uint8_t kg = master[k * 3 + 1];
    const uint8_t kb = master[k * 3 + 2];
    uint8_t temp[256 * 3];
    for (int i = 0; i < 256; ++i) {                     // (1) fill with colour K
        temp[i * 3 + 0] = kr;
        temp[i * 3 + 1] = kg;
        temp[i * 3 + 2] = kb;
    }
    for (int i = 0; i < 256 * 3; ++i) {                 // (2) sub_5FC21
        temp[i] = (uint8_t)(temp[i] - master[i]);
    }
    const int p = pct > 100 ? 100 : pct;                // `cmp arg,64h; jle` (no lower clamp)
    for (int i = 0; i < 0xFE; ++i) {                    // (3) sub_5FD1C (end = 0FEh)
        const int dr = (int)(int8_t)temp[i * 3 + 0];     // movsx
        const int dg = (int)(int8_t)temp[i * 3 + 1];
        const int db = (int)(int8_t)temp[i * 3 + 2];
        palette_set_entry((uint8_t)i,
                          (uint8_t)(master[i * 3 + 0] + dr * p / 100),
                          (uint8_t)(master[i * 3 + 1] + dg * p / 100),
                          (uint8_t)(master[i * 3 + 2] + db * p / 100));
    }
}

// Palette stage of 23074/25DFE/276B9/2799A/27C5A/5A267/5A566
//   (5F945 -> delta -> 5FC21 -> 5FD1C). The copy loop always reloads unk_12824F
//   (= unk_12824C + 3 = palette[1]) inside the loop, so every entry of the temporary
//   buffer is palette[1]. 5FC21 then builds delta[i] = palette[1] - palette[i], and
//   5FD1C writes DAC[i] = palette[i] + delta[i]*mul/100.  The end index is not constant:
//   23074, 276B9 and the 27C5A restore pass use 0FEh; 25DFE/2799A/5A267/5A566 use 100h.
//   5FD1C also has no lower clamp for negative pct (23074's first frames pass -8).
void CombatII::pal_shift_fade(int mul_pct, int end) {
    const uint8_t* master = &battle_master_palette_[0][0]; // unk_12824C (read only)
    const int p = mul_pct > 100 ? 100 : mul_pct;          // upper clamp only, as in 5FD1C
    if (end < 0) end = 0;
    if (end > 0x100) end = 0x100;
    const uint8_t br = master[3], bg = master[4], bb = master[5]; // palette[1]
    for (int i = 0; i < end; ++i) {
        const int dr = (int)(int8_t)(uint8_t)(br - master[i * 3 + 0]);
        const int dg = (int)(int8_t)(uint8_t)(bg - master[i * 3 + 1]);
        const int db = (int)(int8_t)(uint8_t)(bb - master[i * 3 + 2]);
        palette_set_entry((uint8_t)i,
                          (uint8_t)(master[i * 3 + 0] + dr * p / 100),
                          (uint8_t)(master[i * 3 + 1] + dg * p / 100),
                          (uint8_t)(master[i * 3 + 2] + db * p / 100));
    }
}

// sub_562C1(eax = base %, edx = descending floor %): advance the ramp by one 3% step.
//   word_7550E (direction) 0 = rising: draw current pos, then pos += 3; at pos > base set
//   pos = base, direction = 1 and return 1.
//   word_7550E 1 = falling: draw current pos, then pos -= 3; at pos < floor set pos = floor,
//   direction = 0 and return 0.
//   Otherwise 2 (still moving). 562C1 calls `56245(eax = current, edx = current +- 3,
//   ebx = +- 3)`, so 56245's loop writes only the current step to the DAC before pos moves.
int CombatII::pal_ramp_step(int base_pct, int dir_start_pct) {
    if (pal_ramp_dir_ == 0) {
        pal_ramp_apply(pal_ramp_pos_);              // 56245 draws one step from the current position
        const int next = pal_ramp_pos_ + 3;
        if (next > base_pct) {
            pal_ramp_pos_ = base_pct;
            pal_ramp_dir_ = 1;
            return 1;
        }
        pal_ramp_pos_ = next;
        return 2;
    }
    if (pal_ramp_dir_ == 1) {
        pal_ramp_apply(pal_ramp_pos_);
        const int next = pal_ramp_pos_ - 3;
        if (next < dir_start_pct) {
            pal_ramp_pos_ = dir_start_pct;
            pal_ramp_dir_ = 0;
            return 0;
        }
        pal_ramp_pos_ = next;
        return 2;
    }
    return 2;
}
