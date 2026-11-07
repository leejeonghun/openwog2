// sub_21E16: meteo.nsp (Meteor) driver
// ASM: dasm/COMBATII_code.ASM 25464..25703 (called from sub_2A75A+21C after f-meteo1/2.ssp).
//
// Original flow:
//   Copy the four inline 3-word tables into stack copies (1FD38/1FD3E/1FD44/1FD4A):
//     x    = {0x64, 0xC8, 0x12C}   y = {0x78, 0x5A, 0x32}
//     life = {0x14, 0x3C, 0x64}    sfx counter = {0, 0, 0}
//   1124A(tile) -> 1FEA4("meteo.nsp") -> 1033B(5) -> 5F945(0,100h) for the current DAC
//   snapshot. Each frame:
//     (1) Advance only the R channel of the snapshot with `r<3Eh ? r+1 : r-0Ah` over
//         0x100 entries, then push it to the DAC with 5F9AE(0,100h).
//     (2) Three meteors (i=0..2):
//         life<0 respawns: life=0x64, y=-(499BD(6Eh)+1Eh) (-30..-139),
//           x=499BD(12Ch)+0DCh (220..519), sfx counter=0, spawn counter (var_4)++.
//         life>=0Eh (active): frame=(life/5)/3, y+=0Ah, x-=0Ch,
//           draw 1FFFE(frame, x, y) while y>-32h && y<0C8h; life-=5;
//           sfx counter==0 -> 4F4CD(33h) then counter++.
//         life<0Eh (falling/gone): frame=7-life/2, camera shake
//           (dword_BA838 = life even ? -6 : 0), draw 5F570(frame, x+xo-0Ah, y+yo-0Ah,
//           32h, 32h) while y>-32h && y<0FAh; life-=1;
//           sfx counter<4 && life%4==0 -> 4F4CD(32h) then counter++.
//     (3) 454D5 + 5E9E8 + 499E4.
//   The spawn counter reaching 15h (21) ends it: 5F9AE(0,100h, unk_12824C) restores the
//   palette, 1FFCD releases the table, camera fine = (0Ch, 0).
//
// Port mapping: 5F945 -> pal_snapshot_range, 5F9AE -> pal_load_range,
//   the internal R-cycle loop (21EBD..21EDC) -> pal_cycle_entries (same formula),
//   1FFFE -> battle_fx_blit, 5F570 -> battle_fx_blit_scaled(expand_only),
//   454B3+454D5+5E9E8 -> battle_fx_present, 499E4 -> wait_game_tick,
//   1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release.
//
// Approximations:
//   - the original draws 454B3 (background) first and changes the palette afterwards. The port
//     applies the palette first and battle_fx_present draws background + sprites in one pass
//     (same frame result).
//   - the original records the camera shake per meteor and applies it to the NEXT frame's
//     454B3; the port applies the last value to the same frame (one-frame phase shift).
//   - the caller (sub_2A75A) passes a 13x13 sweep coordinate, while the port centres 1124A on
//     the target tile (tx, ty).
#include "combatii.h"
#include "combatii_internal.h"

int CombatII::battle_fx_21e16(int unit_index, int tx, int ty, int k) {
    (void)unit_index;
    (void)k;
    if (!battle_nsp_load("meteo.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                        // sub_1124A
    // Stack copies of the inline tables (1FD38 / 1FD3E / 1FD44 / 1FD4A)
    int16_t mx[3]   = {0x0064, 0x00C8, 0x012C};                      // var_24
    int16_t my[3]   = {0x0078, 0x005A, 0x0032};                      // var_14
    int16_t life[3] = {0x0014, 0x003C, 0x0064};                      // var_1C
    int16_t snd[3]  = {0, 0, 0};                                     // var_C
    int spawned = 0;                                                 // var_4
    uint8_t pal[0x100 * 3];
    pal_snapshot_range(0, 0x100, pal);                               // sub_5F945(0, 100h)
    game_timer_set(5);                                               // sub_1033B(5)
    // The original records the shake in dword_BA838 during the meteor loop and the next
    // frame's 454B3 reads it, so the value trails the meteor update by one frame.
    int16_t pending_shake = cam_fine_y_;
    while (spawned < 0x15 && !should_exit_) {                        // `cmp [var_4],15h; jge`
        cam_fine_y_ = pending_shake;                                 // 454B3 reads the previous frame
        // (1) palette advance (loc_21EBD) + DAC update (sub_5F9AE)
        pal_cycle_entries(pal, 0x100);
        pal_load_range(0, 0x100, pal);
        // (2) three meteors
        for (int i = 0; i < 3 && !should_exit_; ++i) {
            if (life[i] < 0) {                                       // respawn (before loc_21F4C)
                life[i] = 0x64;
                my[i] = (int16_t)(-(combat_rand(0x6E) + 0x1E));      // -(499BD(6Eh)+1Eh)
                mx[i] = (int16_t)(combat_rand(0x12C) + 0xDC);        // 499BD(12Ch)+0DCh
                snd[i] = 0;
                ++spawned;
            }
            if (life[i] >= 0x0E) {                                   // active (loc_2204A)
                const int f = (life[i] / 5) / 3;                     // (life/5)/3
                my[i] = (int16_t)(my[i] + 0x0A);                     // y += 0Ah
                mx[i] = (int16_t)(mx[i] - 0x0C);                     // x -= 0Ch
                const int y = my[i], x = mx[i];
                if (y > -0x32 && y < 0xC8 && f >= 0 && f < nframes) {
                    const MagicFxFrame& fr = magic_frames_[(size_t)f];
                    battle_fx_blit(f, x + fr.xo, y + fr.yo);         // sub_1FFFE
                }
                life[i] = (int16_t)(life[i] - 5);                    // subtract 5
                if (snd[i] == 0) {                                   // `test dx,dx; jnz`
                    sfx(0x33);                                       // 4F4CD(33h)
                    ++snd[i];
                }
            } else {                                                 // falling/gone (loc_21F4C)
                const int f = 7 - life[i] / 2;                       // 7 - life/2
                pending_shake = (int16_t)((life[i] % 2) ? 0 : -6);   // dword_BA838 for the next frame
                const int y = my[i], x = mx[i];
                if (y > -0x32 && y < 0xFA && f >= 0 && f < nframes) {
                    const MagicFxFrame& fr = magic_frames_[(size_t)f];
                    battle_fx_blit_scaled(f, x + fr.xo - 0x0A, y + fr.yo - 0x0A,
                                          0x32, 0x32, true);          // sub_5F570
                }
                life[i] = (int16_t)(life[i] - 1);                    // dec
                if (snd[i] < 4 && (life[i] % 4) == 0) {               // `cmp ax,4` / `idiv 4`
                    sfx(0x32);                                       // 4F4CD(32h)
                    ++snd[i];
                }
            }
        }
        battle_fx_present();                                         // 454B3+454D5+5E9E8
        wait_game_tick();                                            // sub_499E4
    }
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                // 5F9AE(unk_12824C)
    battle_nsp_release();                                            // sub_1FFCD
    cam_fine_x_ = CAM_FINE_X_REST;                                   // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                                   // dword_BA838 = 0
    return 1;
}
