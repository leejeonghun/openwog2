// sub_2DD7A -- axefly.nsp (axe throw)
// ASM: dasm/COMBATII_code.ASM 41175..41372. Caller: `sub_2F80B+0C7` (F1474 idx 13) does
//   `mov eax,esi; call sub_2DD7A` - only eax = unit and dl = the frame-extension selector (k)
//   are used.
//
// Original structure (read from the ASM and the stack slots):
//   10h-byte frame: [esp+0] = y (var_10), [esp+4] = x (var_C), [esp+8] = dword step (var_8),
//   [esp+0Ch] = k (var_4 byte). Calling convention: 135EB(eax=unit, edx=&x, ebx=&y).
//   (1) 135EB -> (x, y). edi = 0Fh.
//   (2) 1FEA4("axefly.nsp") -> 4F460("f-fall.ssp").
//   (3) 2D4EB(unit, 4_BA83C, [BA83C+2], 0): 1033B(19h), then only the two frames [64h]=7,8
//       with 454B3 + 5E9E8 + 499E4 (the coordinate arguments are unused in the original too).
//   (4) 4F4CD(32h), [64h]=9, var_8 = 0Ch. (5) loop: (k ? 18h : 10h) frames total.
//       - first (k ? 0Ch : 8) frames (loc_2DDE7): move along the facing - off_2DD5A
//           0 south y += var_8(w) / 1 west x -= edi / 2 north y -= var_8(w) / 3 east x += edi
//       - remaining frames (loc_2DE47): return path - off_2DD6A
//           base = k ? 0Ch : 8, rel = cx - base
//           0 south y -= (0Ch - rel) / 1 west x += (0Fh - rel)
//           2 north y += (0Ch - rel) / 3 east x -= (0Fh - rel)
//         Both stages decrement edi per frame (shrinking travel) and var_8 (shrinking step).
//       - each frame: 1033B(5) -> frame = cx % 6 (12-byte table) -> 454B3 -> 1FFFE(x, y)
//         -> 5E9E8 -> 499E4.
//   (6) tail: 12124 (restore pose) -> 45559 -> 499DA(20h). The original leaves eax unspecified.
//
// Port mapping: 135EB -> unit_screen_center, 1FEA4 -> battle_nsp_load, 4F460 -> battle_ssp_play,
//   1033B/499DA -> game_timer_set, 454B3 + 5E9E8 -> battle_fx_present, 499E4 -> wait_game_tick,
//   1FFFE -> battle_fx_blit (adds the table entry's xo/yo), 4F4CD -> sfx, 12124 -> reset_unit_pose,
//   45559 -> present_cutscene_frame, 2D4EB -> the same 2-frame loop.
//
// Approximations:
//   - The camera-register loads before 454B3 (dword_BA83C/BA838) are not ported because the
//     renderer reads cam_tile_x_/cam_fine_y_ directly.
//   - `mov [var_8],edx` after 4F4CD relies on the compiler keeping edx=0Ch alive; the port
//     hardcodes 0Ch (the same value).
//   - The original leaves eax unspecified, so the port returns 1 on success.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>

int CombatII::battle_fx_2dd7a(int unit_index, int tx, int ty, int k) {
    (void)tx;
    (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];

    int x = 0, y = 0;                                                // var_C / var_10
    unit_screen_center(u, x, y);                                     // sub_135EB
    int edi = 0x0F;                                                  // `mov edi,0Fh`
    if (!battle_nsp_load("axefly.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    battle_ssp_play("f-fall.ssp", 0x32);                             // sub_4F460("f-fall.ssp",32h)

    // (3) sub_2D4EB: timer 19h + the two pose frames 7 and 8
    game_timer_set(0x19);                                            // sub_1033B(19h)
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        u.anim_frame = (uint8_t)(i + 7);                             // `mov al,bl; add al,7`
        battle_fx_present();                                         // sub_454B3 + sub_5E9E8
        wait_game_tick();                                            // sub_499E4
    }
    if (should_exit_) { battle_nsp_release(); return 1; }

    // (4) sound + pose 9 + step initial value
    sfx(0x32);                                                       // sub_4F4CD(32h)
    u.anim_frame = 9;                                                // `mov byte [esi+64h],9`
    int step = 0x0C;                                                 // var_8 = dl(0Ch)

    const int nframes = (int)magic_frames_.size();
    const int base = k ? 0x0C : 8;                                   // loc_2DDE7: `var_4 ? 0Ch : 8`
    // loc_2DEF0: `(var_4 ? 8 : 0) + 10h`, i.e. 18h frames when k != 0 and 10h otherwise.
    const int total = (k ? 8 : 0) + 0x10;
    for (int cx = 0; cx < total && !should_exit_; ++cx) {            // loc_2DEF0/loc_2DF98
        const int facing = (int)u.facing & 3;                        // [esi+63h]
        if (cx < base) {
            // ---- stage 1: fly towards the facing (off_2DD5A) ----
            switch (facing) {
                case 0: y += step; break;                            // loc_2DE19 (south)
                case 1: x -= edi; break;                             // loc_2DE26 (west)
                case 2: y -= step; break;                            // loc_2DE30 (north)
                case 3: x += edi; break;                             // loc_2DE3D (east)
                default: break;                                      // `cmp dl,3; ja`
            }
        } else {
            // ---- stage 2: return path (off_2DD6A) ----
            const int rel = cx - base;
            switch (facing) {
                case 0: y -= 0x0C - rel; break;                      // loc_2DE5F (south)
                case 1: x += 0x0F - rel; break;                      // loc_2DF05 (west)
                case 2: y += 0x0C - rel; break;                      // loc_2DF3B (north)
                case 3: x -= 0x0F - rel; break;                      // loc_2DF6C (east)
                default: break;
            }
        }

        // (5) per frame: timer 5, frame = cx % 6, draw + present + wait
        game_timer_set(5);                                           // sub_1033B(5)
        --edi;                                                       // `dec edi`
        const int f = cx % 6;                                        // `idiv 6` remainder
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, x + fr.xo, y + fr.yo);                 // sub_1FFFE(eax=x, edx=y)
        }
        battle_fx_present();                                         // sub_454B3 + sub_5E9E8
        wait_game_tick();                                            // sub_499E4
        --step;                                                      // `mov ebx,[var_8]; dec ebx`
                                                                     //  (read by stage 1 only, so
                                                                     // negatives are never used)
    }

    // (6) tail
    if (should_exit_) { battle_nsp_release(); return 1; }
    reset_unit_pose(units_[(size_t)unit_index]);                     // sub_12124
    present_cutscene_frame();                                        // sub_45559
    game_timer_set(0x20);                                            // sub_499DA(20h)
    wait_game_tick();                                                // 499DA falls through to 499E4
    battle_nsp_release();                                            // sub_1FFCD (loc_2DA2B)
    return 1;
}
