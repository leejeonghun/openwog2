// sub_35FC2: stormaxe.nsp (axe spin heavy strike, V-weapon index 7)
// ASM: dasm/COMBATII_code.ASM 51301-51720, called from sub_302E6+169. The caller has
// already played the SSP (i-fist.ssp / m-wind.ssp); this body handles the NSP only.
//
// Original flow:
//   (1) dword_F0B9C+2 = 0 -> 1AC08 (caster tile, ebx=2 radius) -> 1AC59/1A1F8 gather
//       hostile units into F0648; returns -1 when the list is empty.
//   (2) 1FEA4("stormaxe.nsp") -> 103A8 (stop weather tick) -> 11EE4 (release caster tile).
//   (3) Save [unit+63h] in var_40 and add all of F0648 to 44D89 (highlight list).
//   (4) 44D6E(1, 28h) dim, 30824(0, 28h) dims palette 0x60..0xFF by 28h%, 1033B(7),
//       [unit+64h] = 9.
//   (5) Anchor si = 10971(caster.x) + 0Bh, sy = 10988(caster.y). The stack words are
//       var_C = sy-2, var_24 = sy+12h, var_38 = sy-0Ch, var_34 = sy-1Bh, var_3C = sy-2Ah
//       and var_1C = sy-165h (trail loop end).
//   (6) Six outer passes (var_30), each with 4F4CD(33h) and then facing var_2C = 3,2,1,0:
//       [unit+63h] = var_2C, 4F4CD(32h), then three sub-frames. Each sub-frame toggles
//       dword_7001C (0Bh/0Dh) and dword_BA838 (-1/1), calls 454B3 and 12F2A(caster),
//       draws three axe frames at (si, sy-2), (si, sy+12h) and (si, sy+12h-0Fh) using
//       frames var_8, var_4+0Ah and var_4+0Dh, then 7 trail copies from var_10 = sy-0Ch,
//       var_28 = sy-1Bh and di = sy-2Ah, stepping 2Dh (45) until di reaches sy-165h.
//       The trail mix depends on var_14 (rem = var_4 % 3, x = si + xo):
//         0: (10h+rem, var_10) (10h+rem, var_28) (13h+rem, di)
//         1: (13h+rem, var_10) (10h+rem, var_28) (10h+rem, di)
//         2: (10h+rem, var_10) (13h+rem, var_28) (13h+rem, di)
//       Then 5E9E8 and 499E4. var_8 wraps 8 -> 1, var_4 wraps 2 -> 0 and var_14 wraps
//       2 -> 0 after each group of three sub-frames.
//   (7) Tail: dword_7001C = 0Ch, dword_BA838 = 0, 1033B(0Ah, 100h), 5F9AE(0,100h)
//       restores the unk_12824C master, 44D75 clears the highlight, 12124 restores the
//       pose of every F0648 unit, 11E04 re-records occupancy, [64h]=3, [63h]=var_40,
//       1FFCD releases the table, 44D6E(0, 0BB8h), 103A0 resumes the weather, then
//       2CAF7(eax=unit, ebx=5, edx=0BB8h) -> eax=1.
//
// Port mapping: 1AC08/1AC59/1A1F8 -> battle_weapon_collect_diamond(2), 1FEA4 ->
//   battle_nsp_load, 1FFCD -> battle_nsp_release, 103A8/103A0 -> weather_tick_enabled_,
//   44D89/44D75 -> battle_highlight_add/clear, 44D6E -> battle_dim_row0_,
//   30824(0,28h) -> fx_pal_mix(0x60,0x100,0,0x28), 1033B -> game_timer_set,
//   454B3 -> battle_fx_world, 12F2A -> battle_draw_unit_now, 5EE51/5EFB9 -> fx_blit_nsp,
//   5E9E8 -> battle_fx_commit, 499E4 -> wait_game_tick, 5F9AE -> pal_load_range,
//   12124 -> reset_unit_pose, 2CAF7 -> battle_weapon_2caf7_loop(5, 0BB8h).
//
// Deviations:
//   - 11EE4/11E04 are omitted; occupancy is modelled by units_ and the caster never
//     moves tiles here.
//   - 5EFB9 maps to fx_blit_nsp, which drops off-screen pixels.
//   - The 44D6E strength/ramp becomes the battle_dim_row0_ scalar.
//   - A missing NSP asset skips the effect but still applies damage (2CAF7).
//   - Frame indices are clamped by the measured stormaxe.nsp frame count (22 frames:
//     0..8 axe, 0Ah..0Ch, 0Dh..0Fh, 10h..12h, 13h..15h); the original does not check.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>
#include <cstdio>

int CombatII::battle_fx_35fc2_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& A = units_[(size_t)unit_index];

    // ---- (1) collect targets inside a diamond of radius 2 (1AC08/1AC59/1A1F8) ----
    const int count = battle_weapon_collect_diamond(unit_index, 2);
    if (count <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 35fc2 unit=%d empty -> -1\n", A.id);
        return -1;
    }
    // (2) save the caster facing and register the targets for highlighting
    // (original step 3, moved ahead of the NSP load)
    const int8_t saved_facing = A.facing;
    for (int ti : battle_target_list_) battle_highlight_add(ti);

    // (3) load the NSP table and stop the weather tick (original step 2; 11EE4 is
    // omitted because occupancy is modelled by units_)
    if (!battle_nsp_load("stormaxe.nsp") || magic_frames_.empty()) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 35FC2 unit=%d stormaxe.nsp missing (damage only)\n", A.id);
        battle_highlight_clear();
        battle_weapon_2caf7_loop(unit_index, 5, 0x0BB8);
        return 1;
    }
    weather_tick_enabled_ = false;                                      // sub_103A8
    const int nframes = (int)magic_frames_.size();
    auto blit = [&](int frame, int x, int y) {                          // sub_5EE51 / sub_5EFB9
        if (frame < 0 || frame >= nframes) return;
        const MagicFxFrame& fr = magic_frames_[(size_t)frame];
        fx_blit_nsp(frame, x + fr.xo, y + fr.yo);
    };

    // (4) dim, palette, tick period and pose
    battle_dim_row0_ = true;                                            // sub_44D6E(1, 28h)
    fx_pal_mix(0x60, 0x100, 0, 0x28);                                   // sub_30824(0, 28h)
    game_timer_set(7);                                                  // sub_1033B(7)
    A.anim_frame = 9;                                                   // [64h] = 9

    // (5) anchor point
    const int si = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0B;  // 10971+0Bh
    const int sy = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;          // 10988
    const int16_t y_axis = (int16_t)(sy - 2);        // var_C
    const int16_t y_top  = (int16_t)(sy + 0x12);     // var_24
    const int16_t y_a    = (int16_t)(sy - 0x0C);     // var_38 -> var_10
    const int16_t y_b    = (int16_t)(sy - 0x1B);     // var_34 -> var_28
    const int16_t y_c    = (int16_t)(sy - 0x2A);     // var_3C -> di
    const int16_t y_end  = (int16_t)(sy - 0x165);    // var_1C (trail loop end)

    int var_8 = 0;    // axe frame counter (1..8)
    int var_4 = 0;    // sub counter (0..2; axe frames +0Ah/+0Dh and the trail remainder)
    int var_14 = 0;   // trail combination phase (0/1/2)

    // (6) 6 outer passes x 4 facings x 3 sub-frames = 72 frames
    for (int var_30 = 0; var_30 < 6 && !should_exit_; ++var_30) {        // loc_3646B
        sfx(0x33);                                                      // loc_36122: 4F4CD(33h)
        for (int var_2C = 3; var_2C >= 0 && !should_exit_; --var_2C) {   // loc_3645A
            A.facing = (int8_t)var_2C;                                  // loc_3613B: [63h] = var_2C
            sfx(0x32);                                                  // 4F4CD(32h)
            for (int sub = 0; sub < 3 && !should_exit_; ++sub) {         // loc_36155
                // Toggle the camera fine offset (screen shake)
                cam_fine_x_ = (int16_t)((cam_fine_x_ == 0x0B) ? 0x0D : 0x0B);
                cam_fine_y_ = (int16_t)((cam_fine_y_ == 1) ? -1 : 1);
                battle_fx_world();                                      // sub_454B3
                battle_draw_unit_now(unit_index);                       // sub_12F2A
                // Three axe frames (var_8 / var_4+0Ah / var_4+0Dh)
                blit(var_8, si, y_axis);
                blit(var_4 + 0x0A, si, y_top);
                blit(var_4 + 0x0D, si, (int16_t)(y_top - 0x0F));
                // Seven trail copies, 45px apart
                int v10 = y_a, v28 = y_b, vc = y_c;
                do {
                    const int rem = var_4;                              // remainder of idiv 3
                    if (var_14 == 0) {                                  // loc_36253
                        blit(0x10 + rem, si, v10);
                        blit(0x10 + rem, si, v28);
                        blit(0x13 + rem, si, vc);
                    } else if (var_14 == 1) {                           // loc_362CC
                        blit(0x13 + rem, si, v10);
                        blit(0x10 + rem, si, v28);
                        blit(0x10 + rem, si, vc);
                    } else {                                            // loc_36340
                        blit(0x10 + rem, si, v10);
                        blit(0x13 + rem, si, v28);
                        blit(0x13 + rem, si, vc);
                    }
                    v10 = (int16_t)(v10 - 0x2D);
                    v28 = (int16_t)(v28 - 0x2D);
                    vc  = (int16_t)(vc  - 0x2D);
                } while ((int16_t)vc != y_end && !should_exit_);        // `cmp di,bx; jnz`
                // Advance the counters (loc_3640C)
                ++var_8; if (var_8 > 8) var_8 = 1;
                ++var_4; if (var_4 > 2) var_4 = 0;
                battle_fx_commit();                                     // sub_5E9E8
                wait_game_tick();                                       // sub_499E4
            }
            ++var_14; if (var_14 > 2) var_14 = 0;                        // loc_36455
        }
    }

    // ---- (7) tail ----
    cam_fine_x_ = 0x0C;                                                 // dword_7001C = 0Ch
    cam_fine_y_ = 0;                                                    // dword_BA838 = 0
    game_timer_set(0x0A);                                               // sub_1033B(0Ah,100h)
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                   // sub_5F9AE(unk_12824C)
    battle_highlight_clear();                                           // sub_44D75
    for (int ti : battle_target_list_) {                                // 12124 for every target
        if (ti >= 0 && (size_t)ti < units_.size()) reset_unit_pose(units_[(size_t)ti]);
    }
    if ((size_t)unit_index < units_.size()) {
        A.anim_frame = 3;                                               // [64h] = 3
        A.facing = saved_facing;                                        // [63h] = var_40
    }
    battle_nsp_release();                                               // sub_1FFCD
    battle_dim_row0_ = false;                                           // sub_44D6E(0, 0BB8h)
    weather_tick_enabled_ = true;                                       // sub_103A0
    battle_weapon_2caf7_loop(unit_index, 5, 0x0BB8);                   // sub_2CAF7
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 35FC2 unit=%d stormaxe targets=%d frames=72\n",
                units_[(size_t)unit_index].id, count);
    return 1;
}
