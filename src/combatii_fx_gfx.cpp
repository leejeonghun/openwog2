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
#include <cstring>

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
        wait_game_tick();
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
    // unk_12824C is the live master palette (weather included); dlg_shade_pal_ is its snapshot
    // in the port (same reference as the 341C7 tail).
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();
    const uint8_t* master = &dlg_shade_pal_[0][0];
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
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();
    const uint8_t* master = &dlg_shade_pal_[0][0];
    uint8_t white[256 * 3];
    white_palette_284a3(master, white);
    if (battle_trace_) fprintf(stderr, "COMBAT: white flash up=%d\n", up_step);
    pal_crossfade(0, 0x100, master, white, up_step);   // 49A3E(master -> white)
}

void CombatII::battle_fx_white_flash_down(int down_step) {
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();
    const uint8_t* master = &dlg_shade_pal_[0][0];
    uint8_t white[256 * 3];
    white_palette_284a3(master, white);
    if (battle_trace_) fprintf(stderr, "COMBAT: white flash down=%d\n", down_step);
    pal_crossfade(0, 0x100, white, master, down_step); // 49A3E(white -> master)
    pal_load_range(0, 0x100, master);                  // 5F9AE(0,100h,unk_12824C)
    video_.present();
}

void CombatII::battle_fx_white_flash(int up_step, int down_step) {
    battle_fx_present();                               // sub_45559 (sub_284A3 only)
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
    uint8_t* master = &battle_palette_[0][0];           // unk_12824C
    const int k = pal_ramp_color_ & 0xFF;
    const uint8_t kr = battle_palette_[k][0];
    const uint8_t kg = battle_palette_[k][1];
    const uint8_t kb = battle_palette_[k][2];
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
    const uint8_t* master = &battle_palette_[0][0];       // unk_12824C (read only)
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
//   word_7550E (direction) 0 = rising: pos += 3, refresh the DAC, and at pos >= base set
//   pos = base, direction = 1 and return 1.
//   word_7550E 1 = falling: pos -= 3, refresh the DAC, and at pos <= floor set pos = floor,
//   direction = 0 and return 0.
//   Otherwise 2 (still moving). 562C1 calls `56245(eax = current, edx = current +- 3,
//   ebx = +- 3)`, so 56245's loop (si starts at the current value and stops at the target)
//   writes only the current step to the DAC before the position moves - the port keeps that
//   order (draw, then advance).
int CombatII::pal_ramp_step(int base_pct, int dir_start_pct) {
    if (pal_ramp_dir_ == 0) {
        int next = pal_ramp_pos_ + 3;
        if (next > base_pct) next = base_pct;
        pal_ramp_apply(pal_ramp_pos_);              // 56245 draws one step from the current position
        const bool reached = (next >= base_pct);
        pal_ramp_pos_ = reached ? base_pct : next;
        if (reached) { pal_ramp_dir_ = 1; return 1; }
        return 2;
    }
    int next = pal_ramp_pos_ - 3;
    if (next < dir_start_pct) next = dir_start_pct;
    pal_ramp_apply(pal_ramp_pos_);
    const bool reached = (next <= dir_start_pct);
    pal_ramp_pos_ = reached ? dir_start_pct : next;
    if (reached) { pal_ramp_dir_ = 0; return 0; }
    return 2;
}
