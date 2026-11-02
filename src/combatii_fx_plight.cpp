// sub_22D18: p_light.nsp / icedrago.nsp (Ice Dragon)
// ASM: COMBATII_code.ASM 26706..26960. Called from sub_3C3A5+B9 as
//   `mov ebx,32h; mov eax,ebp(unit); mov edx,1770h; call sub_22D18` (damage at 28728/2A500).
//
// Original flow. The routine reads none of its arguments: eax (unit), edx (0x1770) and
//   ebx (0x32) are all unused. Its 0xC4-byte stack frame holds the particle state:
//     var_28 (0x9C) age[5]        - per-particle tick counter, seeded from off_1FD9A
//     var_1C (0xA8) spawn X[5]    - positive
//     var_10 (0xB4) spawn Y[5]    - negative (`neg eax; sub eax,32h`)
//     var_C4 (0x00) point Y[25]   - indexed 10i+2b (i = particle, b = point)
//     var_90 (0x34) point X[25], var_5C (0x68) point live[25]
//     var_4  (0xC0) spawn budget = 0x28
//   IDA shows the same slots as overlapping words (var_C2/var_C0/var_BE/var_BC ...); with
//   these offsets var_C4+2b is point Y[b] and var_90+2b is point X[b].
//   The inline copy (`lea edi,[esp+var_28]; mov esi,off_1FD9A; movsd; movsd; movsw`) is
//   10 bytes = age[5] only: off_1FD9A points into unk_A0005, an uninitialised runtime area
//   with no static value. The spawn loop fills every coordinate/flag array, so nothing is lost.
//
//   (1) sub_61147 (tint shade) -> sub_1FEA4("icedrago.nsp") -> sub_1033B(5).
//   (2) Intro: x fixed at 0x2D; y starts at 0xC7 and drops by 0x0F while y > -0x64,
//       drawing frame 0 tinted through `sub_5F305(x=2Dh, y=y, ebx=table[+8])`, then
//       454B3 + 5E9E8 + 499E4.
//   (3) Seed five particles: Y = -(sub_499BD(82h)+32h) ~ -50..-179, X = sub_499BD(118h)+14h
//       ~ 20..299, age 0, budget var_4 = 0x28 (40). sub_1033B(3).
//   (4) Main loop (var_4 > 0):
//       - per particle: age++; at 20 (0x14) respawn - all five points get
//         Y = spawnY+0Ah, X = spawnX+0Ah, live = 1; age = 0; spawn Y/X are redrawn
//         (Y negative, X positive); budget--.
//       - spawnY += 0Ah; camera shake toggles dword_BA838 (0 <-> -6);
//         when -0x1E < Y < 0xC8 draw the body with `sub_1FFFE(frame 1, X, Y)`.
//       - point velocities (25 points, five per particle): point0 Y-=0A; point1 Y-=0A X-=0A;
//         point2 Y-=0A X+=0A; point3 Y+=0A X-=0A; point4 Y+=0A X+=0A; live points only.
//       - draw all 25 points with `sub_1FFFE(frame (i%5)+2, X[i], Y[i])` (when Y is inside
//         (-0x1E, 0xC8)), then 454D5 + 5E9E8 + 499E4.
//   (5) Tail: sub_1FFCD (release) -> sub_61115 -> dword_7001C = 0Ch, dword_BA838 = 0 ->
//       sub_45559.
//
// Port mapping: 1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release,
//   1FFFE -> battle_fx_blit (the caller adds the entry's xo/yo), 5F305 -> battle_fx_blit_tinted,
//   454B3 + 454D5 + 5E9E8 -> battle_fx_present, 45559 -> present_cutscene_frame,
//   499E4 -> wait_game_tick, 1033B -> game_timer_set, 499BD -> combat_rand,
//   dword_BA838 -> cam_fine_y_, dword_7001C -> cam_fine_x_.
//
// Deviations:
//   - The tint arguments of sub_61147 (eax=18h / edx=32h) remain unresolved after IDA's stack
//     cleanup, so the sibling driver b_dragon's 0x32 (-> 50%) is used.
//   - sub_5F305 takes a flat pointer to a table entry and uses its x/y directly; the port
//     queues a frame number and lets the renderer draw it, so no xo/yo is added for the
//     intro (the 1FFFE path does add them).
//   - The original's 25-point guard compares the same slot (var_C4 = Y) twice (`> -1Eh` and
//     `< 0C8h`). There is no X slot to compare, so X clipping is left to the renderer.
//   - sub_61115 / sub_103A0 (palette LUT restore) have no counterpart in the port and are skipped.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>

int CombatII::battle_fx_22d18(int unit_index, int tx, int ty, int k) {
    (void)unit_index;                                       // the original never reads eax (unit)
    (void)tx; (void)ty; (void)k;                            // edx=0x1770 / ebx=0x32 are unused too
    if (!battle_nsp_load("icedrago.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    const int nframes = (int)magic_frames_.size();

    const int tint = 50;                                    // sub_61147 (0x32 -> 100-0x32)
    game_timer_set(5);                                      // sub_1033B(5)

    // (2) intro: x fixed at 0x2D, y from 0xC7 down to -0x64 in steps of 0x0F
    for (int y = 0xC7; y > -0x64 && !should_exit_; y -= 0x0F) {
        battle_fx_blit_tinted(0, 0x2D, y, tint);             // sub_5F305 (frame 0)
        battle_fx_present();                                 // 454B3 + 5E9E8
        wait_game_tick();                                    // 499E4
    }
    if (should_exit_) {
        battle_nsp_release();
        return 1;
    }

    // ---- stack arrays (offsets in the header) ----
    int16_t point_y[25] = {};    // var_C4 : 10i + 2b
    int16_t point_x[25] = {};    // var_90 : 10i + 2b
    int16_t point_live[25] = {};// var_5C
    int16_t age[5] = {};         // var_28 (original copies 10 bytes from off_1FD9A; unset, so 0)
    int16_t spawn_x[5] = {};     // var_1C
    int16_t spawn_y[5] = {};     // var_10 (negative)
    int budget = 0x28;           // var_4

    // (3) seed particles (loc_22DAB)
    for (int i = 0; i < 5; ++i) {
        spawn_y[i] = (int16_t)(-(int)combat_rand(0x82) - 0x32);   // neg eax; sub eax,32h
        spawn_x[i] = (int16_t)((int)combat_rand(0x118) + 0x14);  // add eax,14h
    }
    game_timer_set(3);                                            // sub_1033B(3)

    // (4) main loop (loc_22DF2)
    while (budget > 0 && !should_exit_) {
        for (int i = 0; i < 5 && !should_exit_; ++i) {
            const int a = age[i] + 1;
            age[i] = (int16_t)a;
            if (a == 0x14) {                                      // `cmp dx,14h`
                const int16_t bx0 = spawn_x[i];
                const int16_t by0 = spawn_y[i];
                for (int b = 0; b < 5; ++b) {                     // loc_22E3B
                    point_y[i * 5 + b] = (int16_t)(by0 + 0x0A);   // var_C4 = var_10 + 0Ah
                    point_x[i * 5 + b] = (int16_t)(bx0 + 0x0A);   // var_90 = var_1C + 0Ah
                    point_live[i * 5 + b] = 1;
                }
                age[i] = 0;
                spawn_y[i] = (int16_t)(-(int)combat_rand(0x82) - 0x32);
                spawn_x[i] = (int16_t)((int)combat_rand(0x118) + 0x14);
                --budget;                                         // `sub [var_4],ecx` (ecx=1)
            }
            spawn_y[i] = (int16_t)(spawn_y[i] + 0x0A);             // var_10 += 0Ah
            cam_fine_y_ = (cam_fine_y_ == 0) ? (int16_t)-6 : (int16_t)0;  // dword_BA838 toggle
            if (spawn_y[i] > -0x1E && spawn_y[i] < 0xC8) {         // -1Eh < Y < 0C8h
                if (1 < nframes) {
                    const MagicFxFrame& fr = magic_frames_[1];    // unk_F0DB6 = frame 1
                    battle_fx_blit(1, spawn_x[i] + fr.xo, spawn_y[i] + fr.yo);  // sub_1FFFE
                }
            }
        }
        if (should_exit_) break;

        // Point velocities (loc_22F21): live points only
        for (int i = 0; i < 5; ++i) {
            const int b0 = i * 5;
            if (point_live[b0 + 0] > 0) point_y[b0 + 0] = (int16_t)(point_y[b0 + 0] - 0x0A);
            if (point_live[b0 + 1] > 0) {
                point_x[b0 + 1] = (int16_t)(point_x[b0 + 1] - 0x0A);
                point_y[b0 + 1] = (int16_t)(point_y[b0 + 1] - 0x0A);
            }
            if (point_live[b0 + 2] > 0) {
                point_x[b0 + 2] = (int16_t)(point_x[b0 + 2] + 0x0A);
                point_y[b0 + 2] = (int16_t)(point_y[b0 + 2] - 0x0A);
            }
            if (point_live[b0 + 3] > 0) {
                point_x[b0 + 3] = (int16_t)(point_x[b0 + 3] - 0x0A);
                point_y[b0 + 3] = (int16_t)(point_y[b0 + 3] + 0x0A);
            }
            if (point_live[b0 + 4] > 0) {
                point_x[b0 + 4] = (int16_t)(point_x[b0 + 4] + 0x0A);
                point_y[b0 + 4] = (int16_t)(point_y[b0 + 4] + 0x0A);
            }
        }

        // Draw the 25 points (loc_22FE9): frame = (i % 5) + 2
        for (int i = 0; i < 0x19 && !should_exit_; ++i) {
            const int f = (i % 5) + 2;                            // `idiv 5` remainder + 2
            if (point_y[i] > -0x1E && point_y[i] < 0xC8 && f < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)f];
                battle_fx_blit(f, point_x[i] + fr.xo, point_y[i] + fr.yo);   // sub_1FFFE
            }
        }
        battle_fx_present();                                     // 454B3 + 454D5 + 5E9E8
        wait_game_tick();                                        // 499E4
    }

    // (5) tail (loc_23045)
    battle_nsp_release();                                        // sub_1FFCD
    cam_fine_x_ = CAM_FINE_X_REST;                               // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                               // dword_BA838 = 0
    present_cutscene_frame();                                    // sub_45559
    return 1;
}
