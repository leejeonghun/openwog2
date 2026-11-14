// sub_59A70: revible.nsp effect (Revible / resurrection)
// ASM: dasm/COMBATII_code.ASM 99600-100198 (598 lines). The only caller is sub_2BC3B+E1
// (row 3, d5 Revible). Arguments: eax = caster (the unit casting Revible), edx = facing
// byte of the revived unit (2BC3B: `mov edx,[ecx+5Dh]; sar edx,18h`, i.e. [revived+60h] >> 24).
//
// Original flow, in order:
//   (1) 103A8 (save palette) -> 1FEA4 ("revible.nsp") -> 1124A (centre on the caster tile) ->
//       11EE4 (release caster occupancy) -> 611AA tint LUT from {01h,30h..35h}, shade 50h.
//   (2) Two palette work buffers:
//       buf1[i] = master[i+31h] (i = 0..0FFh; unk_1282DF = unk_12824C + 93h = entry 31h)
//       buf2 = copy of buf1, except i < 68h, which keeps master[i] (loc_59B1B).
//       5FC21(0, 100h, master, buf1) -> buf1 becomes the colour-shift delta.
//   (3) 16 embers (var_A4 life / var_84 x / var_64 speed / var_44 y):
//       life[i] = -rand(24h) (negative delay); on respawn x = 10971(caster)-6+rand(24h),
//       y = 10988(caster)-0Ah+rand(10h), speed = rand(3)+1. 1033B(10h).
//   (4) 70 frames (`cmp si,46h`):
//       454B3 -> for f > 32h draw table frame 5+(f&1) at
//         (10971+0Ch + xo, 10988-2 + [caster+66h] + yo) through 5EE51.
//       Every odd frame: `dec byte [caster+66h]`.
//       16 embers: if life == 0 respawn; if life > 2Bh skip the whole frame.
//         frame = life<0Ah -> 0 / life>26h -> 0 / life<19h -> (life&1)+1 /
//                 life<23h -> (life&1)+3 / otherwise -> (life&1)+1.
//       If (life >= 0 && y+yo > -1Eh) draw through 5EE51, then y -= speed, life++.
//       5E9E8 -> 5FD1C(60h..100h, buf1, pct = 64h/64h = 1%) -> 499E4.
//       ([var_8] is only a counter incremented by 7Fh and never reaches the palette - skipped.)
//   (5) 5FC21(buf2 shifted by one byte, buf2) -> screen wash delta; 5FD1C(0..100h, delta,
//       0Ah..64h) is applied ten times (0Ah steps) to tint the whole screen.
//   (6) 4F4CD(32h) -> 10 frames: 454B3 -> table (10h+esi&1) at (A0h,64h) via 5EE51,
//       table ((esi&1)+5) at (A0h,4Ch) via 5F29D (tint) -> 5E9E8 ->
//       5FD1C(0..100h, delta, pct 64h,5Ah,...,0Ah) -> 499E4.
//   (7) 4F4CD(33h) -> D66A8 entry (572*facing) -> 36 frames (`cmp di,24h`):
//       454B3 -> table (edi&1)+10h at (A0h,64h), table (edi&1)+7 as tint at (A0h,4Ch),
//       edi<=Ch  -> one more frame ((edi&3)+9) at (A0h,4Ch),
//       edi<=Fh  -> frame edi as tint at (A0h,4Ch),
//       edi>=10h -> unit silhouette (5F517, colour 32h) when edi < 15h, else unit frame
//                   (5F3FD), at (A0h+entry[4], 4Ch+entry[5]).
//       5E9E8 -> 5FD1C(pct = (edi&1)*14h) -> 499E4.
//       ([var_24] countdown and `mov [var_20],40h` are never read - dead code.)
//   (8) 11E04 (restore occupancy) -> 454B3 -> 5FD1C(pct 0..64h, 0Ah steps) ->
//       5FD1C(pct 0..64h, 2 steps) with the "pre-wash copy - master" delta -> 103A0 (restore
//       palette) -> 1FFCD (release table) -> 12124 (caster) -> common epilogue.
//
// Stack slot interpretation (IDA aliases; settled from this file):
//   - var_6A8/var_6C0 are the same slot (`push eax` on entry). sub_2BC3B passes eax = caster
//     and edx = facing, so every record read ([eax], [eax+1], [eax+63h], [eax+66h], [eax+60h])
//     is the caster. The final 12124 uses the same slot -> restore the caster pose.
//   - [caster+66h] is the walk y offset (same `[+66h]` = walk_off_y convention as fx_weapon).
//     `[eax+63h]>>24` addresses the same byte (top byte of the 63h dword = 66h), so the late
//     frames in (4) add walk_off_y - reproduced as-is.
//   - var_6BA>>16 (D66A8 facing) is the second argument -> facing of the revived unit. The
//     sprite type comes from the record, so the caster type is used (D66A8 is a per-record table).
//   - Palette buffers var_6A4/var_3A4/var_3AC/var_3B0/var_3B4/var_3B8 alias one 0x300-byte
//     region four bytes apart. They are split here into named buffers by role (master copy /
//     shift copy / delta / wash copy).
//   - unk_1282DF (master+31h) reads 100h entries, which runs past the palette end (CDh); the
//     port wraps through the palette (&0FFh).
//
// Deviations from the original:
//   1. The 611AA tint LUT is handled by battle_fx_blit_tinted_unclipped(pct=50h) (sub_5F29D
//      does not clip). Palette save/restore (61115/103A8/103A0) is covered by the port.
//   2. 5ECC7/5EDDE/6104A/6106C (clip, dim, LUT setup) and the 45x screen preparation already
//      happen in render/present.
//   3. 11EE4/11E04 (occupancy release/restore) are omitted: the port derives occupancy from
//      the unit tile.
//   4. 5F517 (solid silhouette) / 5F3FD (unit frame) draw to the canvas rather than the magic
//      layer, so this step alone splits present into render_battle_frame() -> overlay ->
//      battle_fx_commit() to keep the original order (background/table -> unit -> present).
//   5. The palette wash base aliases the work buffers in the original, so only the 5FD1C
//      contract "master + delta*pct/100" is kept and the delta is computed into named buffers.
//      The visual result (colour shift, then back to normal) matches the original steps.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>

namespace {

constexpr int kPalLen = 0x100 * 3;

// dword_560DB (COMBATII.LST 109718). sub_59A70 copies 7 bytes and uses them as the 611AA
// colour table: 01h 30h 31h 32h 33h 34h 35h - palette entry 1 plus the 30h..35h gradient.

// Fourth argument of 5F517 (solid colour). The original reads the high word of the table's
// third dword (`mov ecx,[var_24+2]; sar ecx,10h`) and 5F517 uses only cl, which gives 32h
// (the 32h entry of {01,30,31,32,...}).
constexpr uint8_t kSilhouetteColor = 0x32;

} // namespace

int CombatII::battle_fx_59a70(int caster_index, int revived_index) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return 0;
    if (revived_index < 0 || (size_t)revived_index >= units_.size()) return 0;
    auto& A = units_[(size_t)caster_index];                       // eax = caster record
    const int dir = (int)units_[(size_t)revived_index].facing & 3; // edx>>16 = revived facing

    if (!battle_nsp_load("revible.nsp") || magic_frames_.empty()) return 0;   // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    camera_center_on((int)(int8_t)A.x, (int)(int8_t)A.y);                    // sub_1124A

    const uint8_t* master = &battle_palette_[0][0];               // unk_12824C

    // ---- (2) palette work buffers (no sub_5F945; built from a master copy) ----
    uint8_t shifted[kPalLen];                                     // master[i+31h]
    for (int i = 0; i < 0x100; ++i) {
        const int s = (i + 0x31) & 0xFF;                          // the palette end wraps (dev. 5)
        shifted[i * 3 + 0] = master[s * 3 + 0];
        shifted[i * 3 + 1] = master[s * 3 + 1];
        shifted[i * 3 + 2] = master[s * 3 + 2];
    }
    uint8_t hue[kPalLen];                                         // buf1 = shifted - master
    for (int k = 0; k < kPalLen; ++k) hue[k] = (uint8_t)(shifted[k] - master[k]);
    uint8_t wash_pal[kPalLen];                                    // buf2
    for (int i = 0; i < 0x100; ++i) {
        const uint8_t* src = (i < 0x68) ? &master[i * 3] : &shifted[i * 3];
        wash_pal[i * 3 + 0] = src[0];
        wash_pal[i * 3 + 1] = src[1];
        wash_pal[i * 3 + 2] = src[2];
    }

    // sub_5FD1C: DAC[i] = master[i] + delta[i]*pct/100 - only [start,end) is written.
    uint8_t cur[kPalLen];
    auto apply_pal = [&](int start, int end, const uint8_t* delta, int pct) {
        pal_snapshot_range(0, 0x100, cur);
        for (int i = start; i < end; ++i) {
            for (int c = 0; c < 3; ++c) {
                const int base = (int)master[i * 3 + c];
                const int d = (int)(int8_t)delta[i * 3 + c];       // 5FD1C reads with movsx
                cur[i * 3 + c] = (uint8_t)(base + d * pct / 100);
            }
        }
        pal_load_range(0, 0x100, cur);
    };

    // ---- (3) screen coordinates (10971 / 10988) ----
    const int base_x = ((int)(int8_t)A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int base_y = ((int)(int8_t)A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    const int anchor_x = base_x + 0x0C;                            // var_18
    const int anchor_y = base_y - 2;                               // var_C

    int16_t life[16], px[16], py[16], spd[16];
    for (int i = 0; i < 16; ++i) {                                 // loc_59B4F
        life[i] = (int16_t)(-(int)combat_rand(0x24));
        px[i] = 0; py[i] = 0; spd[i] = 0;
    }
    game_timer_set(0x10);                                          // sub_1033B(10h)

    // ---- (4) 70-frame body ----
    for (int f = 0; f < 0x46 && !should_exit_; ++f) {
        // Late "soul" frame (f > 32h): the walk offset is added to y.
        if (f > 0x32) {
            const int fi = 5 + (f & 1);
            if (fi < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fi];
                battle_fx_blit(fi, anchor_x + fr.xo,                     // sub_5EE51
                               anchor_y + (int)(int8_t)A.walk_off_y + fr.yo);
            }
        }
        if (f & 1) A.walk_off_y = (int8_t)(A.walk_off_y - 1);     // dec byte [esi+66h]

        for (int i = 0; i < 16; ++i) {
            if (life[i] == 0) {                                    // `cmp di,si` (respawn)
                px[i] = (int16_t)(base_x - 6 + (int)combat_rand(0x24));
                py[i] = (int16_t)(base_y - 0x0A + (int)combat_rand(0x10));
                spd[i] = (int16_t)((int)combat_rand(3) + 1);
            }
            if (life[i] > 0x2B) continue;                          // `cmp dx,2Bh; jg` (skip update)
            int fi;
            if (life[i] < 0x0A)      fi = 0;
            else if (life[i] > 0x26) fi = 0;
            else if (life[i] < 0x19) fi = (life[i] & 1) + 1;
            else if (life[i] < 0x23) fi = (life[i] & 1) + 3;
            else                     fi = (life[i] & 1) + 1;
            if (life[i] >= 0 && fi < nframes) {                    // `cmp di,life; jg` -> skip
                const MagicFxFrame& fr = magic_frames_[(size_t)fi];
                if (py[i] + fr.yo > -0x1E)                         // `cmp edx,0FFFFFFE2h; jle`
                    battle_fx_blit(fi, px[i] + fr.xo, py[i] + fr.yo);
            }
            py[i] = (int16_t)(py[i] - spd[i]);                     // y -= speed
            ++life[i];
        }
        battle_fx_present();                                       // 454B3 + 5E9E8
        apply_pal(0x60, 0x100, hue, 1);                            // 5FD1C(60h,100h,delta,1%)
        wait_game_tick();                                          // sub_499E4
    }

    // ---- (5) full-screen wash (loc_59E09/59E40) ----
    uint8_t wash[kPalLen];
    for (int k = 0; k < kPalLen; ++k)                              // 5FC21((buf2+1), buf2)
        wash[k] = (uint8_t)(wash_pal[(k + 1) % kPalLen] - wash_pal[k]);
    for (int pct = 0x0A; pct <= 0x64; pct += 0x0A) apply_pal(0, 0x100, wash, pct);

    // ---- (6) 10 frames (light pillar + wash fade) ----
    sfx(0x32);                                                     // sub_4F4CD(32h)
    for (int f = 0; f < 0x0A && !should_exit_; ++f) {
        const int fi = 0x10 + (f & 1);
        if (fi < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)fi];
            battle_fx_blit(fi, 0xA0 + fr.xo, 0x64 + fr.yo);        // sub_5EE51
        }
        const int ft = 5 + (f & 1);
        if (ft < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)ft];
            battle_fx_blit_tinted_unclipped(ft, 0xA0 + fr.xo, 0x4C + fr.yo, 0x50,
                                            {0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35}); // sub_5F29D(611AA)
        }
        battle_fx_present();
        apply_pal(0, 0x100, wash, 0x64 - f * 0x0A);                // 5FD1C(pct 64h,5Ah,...)
        wait_game_tick();
    }

    // ---- (7) 36 frames (the unit returns) ----
    sfx(0x33);                                                     // sub_4F4CD(33h)
    const NC2Frame* unit_frame = nc2_frame_for(A.sprite_type, dir, 0);   // D66A8 (facing, pose 0)
    for (int f = 0; f < 0x24 && !should_exit_; ++f) {
        const int fi = 0x10 + (f & 1);
        if (fi < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)fi];
            battle_fx_blit(fi, 0xA0 + fr.xo, 0x64 + fr.yo);
        }
        const int ft = 7 + (f & 1);
        if (ft < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)ft];
            battle_fx_blit_tinted_unclipped(ft, 0xA0 + fr.xo, 0x4C + fr.yo, 0x50,
                                            {0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35});
        }
        if (f <= 0x0C) {                                            // frame ((edi&3)+9)
            const int fx = (f & 3) + 9;
            if (fx < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fx];
                battle_fx_blit_tinted_unclipped(fx, 0xA0 + fr.xo, 0x4C + fr.yo, 0x50,
                                                {0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35}); // sub_5F29D(611AA)
            }
        } else if (f <= 0x0F) {                                     // frame edi (0Dh..0Fh), tinted
            if (f < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)f];
                battle_fx_blit_tinted_unclipped(f, 0xA0 + fr.xo, 0x4C + fr.yo, 0x50,
                                                {0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35});
            }
        }
        render_battle_frame();                                      // sub_454B3
        if (f >= 0x10 && unit_frame) {                              // 5F517 (silhouette) / 5F3FD
            DrawEntry e{};
            e.x = 0xA0;
            e.y = 0x4C;
            e.foot = 0;
            e.type = (f < 0x15) ? 3 : 2;
            e.idx = kSilhouetteColor;
            e.nc2 = unit_frame;
            blit_draw_entry(e);                                     // x_off/y_off are added here
        }
        battle_fx_commit();                                         // sub_5E9E8
        magic_fx_draws_.clear();
        apply_pal(0, 0x100, wash, (f & 1) * 0x14);                 // 5FD1C(pct (edi&1)*14h)
        wait_game_tick();
    }

    // ---- (8) restore ----
    uint8_t rev[kPalLen];
    for (int k = 0; k < kPalLen; ++k)                              // "swap copy - master"
        rev[k] = (uint8_t)(master[k] - wash_pal[k]);
    for (int pct = 0; pct <= 0x64; pct += 0x0A) apply_pal(0, 0x100, wash, pct);
    for (int pct = 0; pct <= 0x64; pct += 2) apply_pal(0, 0x100, rev, pct);
    pal_load_range(0, 0x100, master);                              // sub_103A0
    battle_nsp_release();                                          // sub_1FFCD
    reset_unit_pose(A);                                            // sub_12124 (caster)
    return 1;
}
