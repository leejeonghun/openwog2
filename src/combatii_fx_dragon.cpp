// sub_21BE8: b_dragon.nsp (sub_2C8A4 effect==0 meteor)
// ASM: dasm/COMBATII_code.ASM 25291..25463 (sub_2C8A4+0x28); the caller loads
//   dragon.ssp / f-meteo2.ssp first. The routine takes no arguments (the caller does
//   not set eax/edx/ebx before `call sub_21BE8`) and does not move the camera
//   (no sub_1124A - it uses the caller's camera).
//
// Original flow:
//   (1) sub_61147(eax=0, edx=0x32) builds the colour-0 LUT and sets
//       dword_128558 = 100-0x32 = 50 (tint brightness 50%).
//   (2) Copy the 14 bytes at off_1FD2A into the 7-word frame counter table (var_14):
//       {0, 1, 2, 3, 5, 0, 0x64}. The two leading dwords
//       (`dd offset byte_10000` / `dd offset loc_30000+2`) read as words 0, 1, 2, 3,
//       `dd 5` covers entries 4 and 5, and the low word of `dd 0C80064h` is entry 6.
//   (3) sub_1FEA4("b_dragon.nsp") -> sub_1033B(5, 0x100) ->
//       sub_5FA2C(0, 0x100, master, 0x5A = 90%) -> sub_4F4CD(0x32).
//   (4) Fall phase (loc_21C53): var_4 (word) starts at 0xC7 (199) and drops 0x0F per
//       frame, looping while var_4 > -0x64 (`cmp ecx,0FFFFFF9Ch; jle`):
//       sub_454B3; sub_5F305(x = 0x2D, y = var_4, ebx = [unk_F0DAA+8] = flat pointer of
//       frame 0, no table entry so no xo/yo); sub_5E9E8; var_4 -= 0x0F; sub_10364 wait.
//       The dragon (frame 0) rises from below the screen at x = 45 and exits the top.
//   (5) loc_21CA1: sub_5FA2C(0, 0x100, master, 0x64) restores the palette.
//   (6) Spawn 7 particles (loc_21CB9): var_34[i] = sub_499BD(0x91)+0x28 (40..184),
//       var_24[i] = sub_499BD(0x118)+0x14 (20..299). sub_1FFFE takes eax = x, edx = y
//       and the caller passes eax = var_24, edx = var_34, so var_24 is x and var_34 is
//       y. Then sub_1033B(6).
//   (7) Explosion (loc_21CF9): si = 0x50 spawn budget (Watcom keeps esi callee-saved)
//       is > 0, so per frame: sub_454B3; for each of the 7 particles (loc_21D1A):
//       ax = var_14[i] (before increment), var_14[i] = ax+1; when ax == 6 reset to 0,
//       re-roll x/y, --si and play sub_4F4CD(0x33) when si % 5 == 0; toggle
//       dword_BA838 (camera fine y) between 0 and -6; sub_1FFFE(frame = var_14[i]+1,
//       x = var_24[i], y = var_34[i]); then sub_454D5, sub_5E9E8, sub_10364 wait.
//   (8) Exit (loc_21DE8): sub_1FFCD (release table) -> sub_61115 (release LUT) ->
//       dword_7001C = 0x0C, dword_BA838 = 0 -> sub_45559 (present one frame).
//
// Port mapping: sub_1FEA4 -> battle_nsp_load, sub_1FFCD -> battle_nsp_release,
//   sub_1FFFE -> battle_fx_blit (frame xo/yo added here, as sub_1FFFE does),
//   sub_5F305 -> battle_fx_blit_tinted, sub_454B3+sub_454D5+sub_5E9E8 ->
//   battle_fx_present, sub_10364 -> wait_game_tick, sub_1033B -> game_timer_set,
//   sub_4F4CD -> sfx, sub_499BD -> combat_rand,
//   sub_5FA2C -> pal_scale_range(0, 0x100, master, pct),
//   sub_45559 -> present_cutscene_frame; sub_61115/sub_103A8 are omitted.
//
// Deviations:
//   - sub_5F305 takes ebx = the flat pointer of frame 0 (unk_F0DAA+8), not a 12-byte
//     table entry, so no xo/yo is added on that call.
//   - sub_61147 builds the LUT for source colour 0 only, while blit_frame_tinted shades
//     every source colour, so the tint reaches more colours than the original; the 50%
//     background weighting is identical.
//   - sub_5FA2C takes its percentage in ecx (other callers use `movsx ecx,si`). The first
//     call reads the `mov ecx,5Ah` (90) left at the function head, which survives
//     sub_61147/sub_1FEA4/sub_1033B; the restore call passes an explicit 0x64 (100).
//   - The last var_14 entry 0x64 points at frame 101. If b_dragon.nsp has fewer frames
//     the port skips that particle (battle_fx_blit range check) and particle 6 never
//     re-rolls because its counter never reaches 6 - the same word-compare result as
//     the original.
#include "combatii.h"
#include "combatii_internal.h"

int CombatII::battle_fx_21be8(int unit_index, int tx, int ty, int k) {
    (void)unit_index;
    (void)tx;                                   // the original does not move the camera (no sub_1124A)
    (void)ty;
    (void)k;
    if (!battle_nsp_load("b_dragon.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();

    // (1) sub_61147(eax=0, edx=0x32) -> shade = 100-0x32 = 50 (tint brightness)
    constexpr int kTint = 0x32;
    // (3) shorter timer period, 90% palette, sound effect
    game_timer_set(5);                                           // sub_1033B(5, 100h)
    pal_scale_range(0, 0x100, &battle_palette_[0][0], 0x5A);     // sub_5FA2C(ecx=5Ah)
    sfx(0x32);                                                   // sub_4F4CD(32h)

    // (4) dragon (frame 0) rises from below at x = 0x2D (var_4 = 0xC7, -0x0F per frame)
    int16_t fy = 0x00C7;
    while (fy > -0x64 && !should_exit_) {                        // `cmp ecx,0FFFFFF9Ch; jle`
        battle_fx_blit_tinted(0, 0x2D, fy, kTint);               // sub_5F305 (flat pointer of frame 0)
        battle_fx_present();                                     // 454B3 + 5E9E8
        fy = (int16_t)(fy - 0x0F);                               // `sub ebx,0Fh`
        wait_game_tick();                                        // sub_10364 wait
    }

    // (5) restore the palette to 100%
    pal_scale_range(0, 0x100, &battle_palette_[0][0], 0x64);     // sub_5FA2C(ecx=64h)

    // (6) initial placement of 7 particles: var_24 = x (20..299), var_34 = y (40..184)
    int16_t px[7], py[7];
    int16_t pframe[7] = {0, 1, 2, 3, 5, 0, 0x64};                // copy of the 14 bytes at off_1FD2A
    for (int i = 0; i < 7; ++i) {
        py[i] = (int16_t)(combat_rand(0x91) + 0x28);             // var_34: 499BD(91h)+28h
        px[i] = (int16_t)(combat_rand(0x118) + 0x14);            // var_24: 499BD(118h)+14h
    }
    int budget = 0x50;                                           // esi (mov esi,50h)
    game_timer_set(6);                                           // sub_1033B(6)

    // (7) explosion
    while (budget > 0 && !should_exit_) {                        // `test si,si; jle`
        for (int i = 0; i < 7 && !should_exit_; ++i) {           // loc_21D1A
            const int old = pframe[i];                            // var_14[i]
            pframe[i] = (int16_t)(old + 1);
            if (old == 6) {                                       // `cmp ax,6; jnz`
                pframe[i] = 0;
                py[i] = (int16_t)(combat_rand(0x91) + 0x28);     // re-roll (y)
                px[i] = (int16_t)(combat_rand(0x118) + 0x14);    // re-roll (x)
                --budget;                                         // `dec esi`
                if ((budget % 5) == 0) sfx(0x33);                 // `idiv 5` -> sub_4F4CD(0x33)
            }
            // loc_21D7B: toggle camera fine y between 0 and -6
            cam_fine_y_ = (int16_t)((cam_fine_y_ == 0) ? -6 : 0); // dword_BA838
            // loc_21D99: sub_1FFFE(frame = var_14[i]+1, x = var_24[i], y = var_34[i])
            const int f = pframe[i] + 1;
            if (f >= 0 && f < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)f];
                battle_fx_blit(f, px[i] + fr.xo, py[i] + fr.yo);
            }
        }
        battle_fx_present();                                     // 454D5 + 5E9E8
        wait_game_tick();                                        // sub_10364 wait
    }

    // (8) exit
    battle_nsp_release();                                        // sub_1FFCD
    // sub_61115 (LUT release) is omitted: the port has no LUT cache.
    cam_fine_x_ = CAM_FINE_X_REST;                               // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                               // dword_BA838 = 0
    present_cutscene_frame();                                    // sub_45559
    return 1;
}
