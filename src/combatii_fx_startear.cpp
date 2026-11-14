// sub_23074 -- startear.nsp (Star Tearer, sub_2A75A kind 1)
// ASM 26961..27501.
//
// Original structure (calling convention eax=x, edx=y - screen coordinates passed by sub_2A75A):
//   1FEA4("startear.nsp") -> 1033B(5) -> 1124A(camera on the caster)
//   (1) 25 frames: x fixed at 0ABh, frame (i%4); fall speed (esi) += 2 every 5 frames,
//       y (ecx) += speed.  (loc_230A6)
//   (2) 4F4CD(32h) -> 15 frames: speed -= 2 every 3 frames, y -= speed (rise); when i%4==0
//       word ptr dword_BA838 (camera fine y) = -4, otherwise 0 (screen shake).
//       Frame is again (i%4).  (loc_23117)
//   (3) lay out 30 particles (loc_231F1): positions [0]=9Ch, [1]=BAh, [2]=0ABh,
//       y[0]=y[1]=current y, y[2]=y+14h, the other 27 at
//       x = 499BD(118h)+3Ch, y = 499BD(32h)+y-19h. Then 5F945(0,100h) snapshots the DAC,
//       overwrites the stack palette from unk_12824F (= master+3) and 5FC21(0,100h,master,delta)
//       builds delta[i] = palette[1]-master[i], which pal_shift_fade(pct) reproduces.
//       4F4CD(33h).
//   (4) 30 frames (loc_23296): each frame pushes the palette with
//       5FD1C(0,0FEh,master,delta,pct) (pct = -8 + 3 per frame), drift (var_C) += 2 every
//       10 frames, y[0] -= 2 / y[1] += 2, each particle x += drift.
//       Drawn frame = i<3 ? i+4 : (x%7)+0Eh.  (loc_23332)
//   (5) 20 frames (loc_234DA): pct += 3 per frame, the first three move x -= 2 / y += 2, the
//       rest pick frame (y%7)+0Eh, the first three draw frames i+7.  (loc_23527/loc_2358F)
//   (6) re-lay 46 particles (loc_23619): x = 499BD(0FAh)+28h, y = 499BD(64h)-3Ch, draw one
//       frame (every third particle x -= 2, y += 2, speed +2 every 10 frames),
//       frame = i<1Eh ? (y%6)+0Eh : (y%7)+16h.  (loc_236BA)
//   (7) re-lay 46 particles (loc_2373D): x = 499BD(122h)+14h, y = 499BD(0C8h), 50 frames:
//       5FD1C (pct -= 3) -> x unchanged, draw frame (y%7)+0Eh at y-1Eh -> y += 1.  (loc_237CB)
//   (8) 1FFCD (release table) -> dword_7001C=0Ch / dword_BA838=0 (restore camera fine) -> 45559.
//
// Port approximations:
//   - The original keeps coordinates and speeds in overlapping dword/word stack slots (IDA var
//     aliases), so the exact byte layout cannot be recovered. The port keeps the observable
//     arithmetic above (values, increments, frame formulas) with the arrays split into separate
//     variables; every constant is the ASM value.
//   - The original's vsync wait (sub_5E943) and 454B3 are handled by battle_fx_present() and
//     wait_game_tick() together with rendering.
//   - pal_shift_fade keeps 5FD1C's upper clamp only; 23074 passes negative pct and end 0FEh.
#include "combatii.h"
#include "combatii_internal.h"

int CombatII::battle_fx_23074(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("startear.nsp") || magic_frames_.empty()) return 0;   // sub_1FEA4

    camera_center_on(tx, ty);                          // sub_1124A
    game_timer_set(5);                                 // sub_1033B(5)

    constexpr int kStarX = 0xAB;                       // `mov eax,0ABh`
    int y = 0;                                         // ecx
    int speed = 0;                                     // esi (fall/rise speed)

    // ---- (1) star fall (25 frames) ----
    for (int i = 0; i < 0x19 && !should_exit_; ++i) {
        if (i % 5 == 0) speed += 2;                    // `idiv 5; test edx,edx; add esi,2`
        y += speed;                                    // `add ecx, esi`
        const int f = i % 4;                           // `idiv 4; imul edx,0Ch`
        battle_fx_blit(f, kStarX, y);                  // sub_1FFFE
        battle_fx_present();                           // 454B3 + 5E9E8
        wait_game_tick();                              // sub_499E4
    }
    sfx(0x32);                                         // sub_4F4CD(32h)

    // ---- (2) rise + screen shake (15 frames) ----
    for (int i = 0; i < 0x0F && !should_exit_; ++i) {
        if (i % 3 == 0) speed -= 2;                    // `idiv 3; test edx,edx; sub esi,2`
        y -= speed;                                    // `sub ecx, esi`
        // loc_2314D: word BA838 = 0FFFCh when i%4==0, otherwise 0.
        cam_fine_y_ = (i % 4 == 0) ? -4 : 0;
        const int f = i % 4;
        battle_fx_blit(f, kStarX, y);
        battle_fx_present();
        wait_game_tick();
    }

    // ---- (3) lay out 30 particles (loc_231F1) ----
    constexpr int kN = 30;
    int px[kN], py[kN];
    px[0] = 0x9C; px[1] = 0xBA; px[2] = kStarX;         // var_70 / var_70+2 / var_6C
    py[0] = y;    py[1] = y;    py[2] = y + 0x14;       // var_CC / var_CC+2 / var_C8
    for (int i = 3; i < kN; ++i) {
        px[i] = combat_rand(0x118) + 0x3C;              // sub_499BD(118h)+3Ch
        py[i] = combat_rand(0x32) + y - 0x19;           // sub_499BD(32h)+y-19h
    }
    sfx(0x33);                                          // sub_4F4CD(33h)

    // ---- (4) palette strobe + coordinate advance (30 frames) ----
    int drift = 0;                                      // var_C
    for (int i = 0; i < 0x1E && !should_exit_; ++i) {
        pal_shift_fade(-8 + 3 * i, 0xFE);               // 5F945 -> 5FC21 -> 5FD1C (pct = -8+3i)
        if (i % 10 == 0) drift += 2;                    // loc_23320
        // 1FFFE(eax=x, edx=y): eax addresses the 35Ah (var_74) array and edx the 2FEh
        // (var_D0) array, so the +-2 split moves x and drift accumulates on y (frame
        // selection also uses y).
        px[0] -= 2; px[1] += 2;                         // loc_23296 (var_74 +-2)
        for (int j = 0; j < kN; ++j) {
            py[j] += drift;                             // loc_23332: `add word [..], ax`
            const int f = (j < 3) ? (j + 4) : (((py[j] & 0xFFFF) % 7) + 0x0E);
            battle_fx_blit(f, px[j], py[j]);
        }
        battle_fx_present();
        wait_game_tick();
    }

    // ---- (5) spread (20 frames, loc_234DA) ----
    for (int i = 0; i < 0x14 && !should_exit_; ++i) {
        pal_shift_fade(-8 + 3 * (0x1E + i), 0xFE);      // pct += 3/frame
        for (int j = 0; j < 3; ++j) { px[j] -= 2; py[j] += 2; }   // loc_23527 (first three only)
        if (i % 10 == 0) drift += 2;
        for (int j = 0; j < kN; ++j) {
            py[j] += drift;                             // loc_235C8 (var_D4 array)
            const int f = (j < 3) ? (j + 7) : (((py[j] & 0xFFFF) % 7) + 0x0E);
            battle_fx_blit(f, px[j], py[j]);
        }
        battle_fx_present();
        wait_game_tick();
    }

    // ---- (6) re-lay 46 particles + one frame (loc_23619 / loc_236BA) ----
    constexpr int kM = 0x2E;
    int qx[kM], qy[kM];
    for (int i = 0; i < kM; ++i) {
        qx[i] = combat_rand(0xFA) + 0x28;               // sub_499BD(0FAh)+28h
        qy[i] = combat_rand(0x64) - 0x3C;               // sub_499BD(64h)-3Ch
    }
    for (int i = 0; i < kM; i += 3) { qx[i] -= 2; qy[i] += 2; }   // loc_23669 (`add edx,3`)
    speed += 2;                                         // loc_236B8: the `i%10==0` branch on frame 0
    for (int i = 0; i < kM; ++i) {
        qy[i] += speed;
        const int f = (i < 0x1E) ? (((qy[i] & 0xFFFF) % 6) + 0x0E)
                                 : (((qy[i] & 0xFFFF) % 7) + 0x16);
        battle_fx_blit(f, qx[i], qy[i]);
    }
    battle_fx_present();
    wait_game_tick();

    // ---- (7) rising afterglow (50 frames, loc_2373D / loc_23777) ----
    for (int i = 0; i < kM; ++i) {
        qx[i] = combat_rand(0x122) + 0x14;              // sub_499BD(122h)+14h
        qy[i] = combat_rand(0xC8);                      // sub_499BD(0C8h)
    }
    // Fade out the afterglow by dropping the brightness carried over from (1)-(6) by 3.
    // (100 = full brightness.)
    int pct = 0x64;
    for (int i = 0; i < 0x32 && !should_exit_; ++i) {
        pal_shift_fade(pct, 0xFE);                      // [var_10+2] -> 5FD1C
        if (pct > 0) pct -= 3;                          // `test cx,cx; jle; sub 3`
        for (int j = 0; j < kM; ++j) {
            const int f = (((qy[j] & 0xFFFF) % 7) + 0x0E);
            battle_fx_blit(f, qx[j], qy[j] - 0x1E);     // `sub eax,1Eh`
            qy[j] += 1;                                 // `inc eax` -> [var_DA+2]
        }
        battle_fx_present();
        wait_game_tick();
    }

    battle_nsp_release();                               // sub_1FFCD
    cam_fine_x_ = CAM_FINE_X_REST;                      // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                      // dword_BA838 = 0
    present_cutscene_frame();                           // sub_45559
    return 1;
}
