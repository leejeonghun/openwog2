#include "combatii.h"
#include "combatii_internal.h"
#include <cstdio>

// ==== sub_2DFC1: madaxe.nsp (axe strike) ====
// ASM: COMBATII_code.ASM 41374..41475. Called from sub_2F98A (F1474 idx 14) as
//   `mov eax,ecx; movsx edx,ax; ... call sub_2DFC1` -> eax = unit, edx = repeat count k.
//
// Original flow:
//   (1) sub_135EB(unit) -> centre (var_4 = x, var_8 = y), then y -= 3.
//   (2) sub_1FEA4("madaxe.nsp"), sub_4F460("i-fist.ssp", 0x32).
//   (3) sub_2D4EB(unit): sub_1033B(0x19), then poses [64h] = 7 and 8 through
//       sub_454B3 -> sub_5E9E8 -> sub_499E4 (edx/ebx coordinates are never used).
//   (4) [64h] = 9, sub_1033B(7), facing [63h] saved in var_C.
//   (5) k*10+10 frames (si = 0 .. k*10+9):
//       si%10 == 0 -> sub_4F4CD(0x32);
//       frame = si%10, and on odd si: [63h] = ([63h]+3) % 4 (idiv ebp = 4);
//       sub_454B3 -> sub_1FFFE(frame, x, y) -> sub_5E9E8 -> sub_499E4.
//   (6) sub_12124 (pose reset) -> restore facing -> sub_1FFCD -> return 1.
//
// Port mapping: 135EB -> unit_screen_center, 1FEA4 -> battle_nsp_load,
//   4F460 -> battle_ssp_play, 1033B -> game_timer_set, 454B3 + 5E9E8 -> battle_fx_present,
//   499E4 -> wait_game_tick, 1FFFE -> battle_fx_blit (adds xo/yo), 4F4CD -> sfx,
//   12124 -> reset_unit_pose, 1FFCD -> battle_nsp_release.
//
// Deviations:
//   - sub_2D4EB only sets poses 7/8 and draws them through 454B3+5E9E8+499E4, so the
//     same order is used here (its coordinate arguments are unused in the original too).
//   - sub_12124 clears the pose to 0, therefore facing is restored after it, as in the
//     original.
int CombatII::battle_fx_2dfc1(int unit_index, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];

    // (1) sub_135EB(unit) -> centre, y -= 3
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);                                  // sub_135EB
    cy -= 3;                                                        // `mov edx,[var_8]; sub edx,3`
    // (2) load the sprite table and play the sound
    if (!battle_nsp_load("madaxe.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    battle_ssp_play("i-fist.ssp", 0x32);                            // sub_4F460("i-fist.ssp", 32h)
    // (3) sub_2D4EB: timer 0x19, poses 7 and 8
    game_timer_set(0x19);                                           // sub_1033B(19h)
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        u.anim_frame = (uint8_t)(i + 7);                            // `mov al,bl; add al,7` -> [64h]
        battle_fx_present();                                        // 454B3 + 5E9E8
        wait_game_tick();                                           // sub_499E4
    }
    if (should_exit_) { battle_nsp_release(); return 1; }

    // (4) pose 9, timer 7, save facing
    const int8_t saved_facing = u.facing;                            // var_C
    u.anim_frame = 9;                                                // `mov byte [ecx+64h],9`
    game_timer_set(7);                                               // sub_1033B(7)

    // (5) k*10+10 frames
    const int nframes = (int)magic_frames_.size();
    const int frames = k * 10 + 10;                                  // `movsx eax,di; imul eax,0Ah; add eax,0Ah`
    for (int si = 0; si < frames && !should_exit_; ++si) {
        if ((si % 10) == 0) sfx(0x32);                               // `idiv 0Ah; test edx,edx` -> sub_4F4CD(32h)
        const int f = si % 10;                                       // frame = si%10 (12-byte entry)
        if (si & 1) {                                                // `test si,1; jz`
            const int nf = ((int)u.facing + 3) % 4;                  // `mov edx,[ecx+60h]>>24; add edx,3; idiv ebp(4)`
            u.facing = (int8_t)nf;                                   // `mov [ecx+63h],dl`
        }
        if (f >= 0 && f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, cx + fr.xo, cy + fr.yo);                // sub_1FFFE
        }
        battle_fx_present();                                         // sub_454B3 + sub_5E9E8
        wait_game_tick();                                            // sub_499E4
    }

    // (6) reset pose -> restore facing -> release table
    reset_unit_pose(u);                                              // sub_12124
    u.facing = saved_facing;                                         // `mov al,[var_C]; mov [ecx+63h],al`
    battle_nsp_release();                                            // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2DFC1 madaxe unit=%d k=%d frames=%d\n", u.id, k, frames);
    return 1;
}

// ==== sub_2F3B2: fistlv5/6.nsp (fist finisher) ====
// ASM: COMBATII_code.ASM 43079..43144. sub_2FFF9 (F1474 idx 23) calls with edx = 0 and
//   sub_30049 (idx 24) with edx = 1; eax = unit, dx = variant.
//
// Original flow:
//   (1) dx == 0 loads "fistlv5.nsp", otherwise "fistlv6.nsp" (sub_1FEA4).
//   (2) sub_44D6E(1) -> sub_1033B(0x0C) -> sub_45559 -> sub_499E4 (commits one frame).
//   (3) [64h] = 6, sub_135EB(unit) -> centre (var_A).
//   (4) three frames (i = 0..2):
//       frame = ([60h]>>24 = facing) * 3 + i (12-byte entry, `lea eax,[eax+eax*2]; add eax,edx`);
//       sub_454B3 -> sub_12F2A(caster) -> sub_12F2A(dword_F0648 = first target) ->
//       sub_1FFFE(frame, x, y) -> sub_5E9E8 -> sub_499E4; i == 0 waits on sub_499E4 once more.
//   (5) sub_44D6E(0) -> sub_1FFCD (the pose reset sub_12124 belongs to callers sub_2FFF9/30049).
//
// Port mapping: 1FEA4 -> battle_nsp_load, 44D6E -> battle_dim_row0_, 1033B -> game_timer_set,
//   45559 -> present_cutscene_frame, 499E4 -> wait_game_tick, 135EB -> unit_screen_center,
//   454B3 + 5E9E8 -> battle_fx_present, 1FFFE -> battle_fx_blit, 1FFCD -> battle_nsp_release,
//   dword_F0648 -> battle_target_list_[0].
//
// Deviations:
//   - The original needs sub_12F2A(caster/target) because sub_454B3 does not draw units; the
//     port's render_battle_map includes them, so battle_fx_present gives the same result
//     (units are not drawn twice).
//   - The two halves of the sub_135EB result ([esp+2]/[var_A]) are x/y word aliases; they map
//     to unit_screen_center's cx/cy as the original calling convention (eax = x, edx = y).
//   - sub_44D6E is a single scalar (dword_FA8B6), so it is cleared to false on exit like the
//     sibling drivers instead of restoring the previous value.
int CombatII::battle_fx_2f3b2(int unit_index, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];

    // (1) per-variant table
    const char* nsp = (k == 0) ? "fistlv5.nsp" : "fistlv6.nsp";     // `test dx,dx; jnz`
    if (!battle_nsp_load(nsp) || magic_frames_.empty()) return 0;   // sub_1FEA4
    // (2) dim, timer, early commit
    battle_dim_row0_ = true;                                         // sub_44D6E(1)
    game_timer_set(0x0C);                                            // sub_1033B(0Ch)
    present_cutscene_frame();                                        // sub_45559
    wait_game_tick();                                                // sub_499E4
    if (should_exit_) { battle_dim_row0_ = false; battle_nsp_release(); return 1; }

    // (3) pose 6 + centre
    u.anim_frame = 6;                                                // `mov byte [esi+64h],6`
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);                                   // sub_135EB

    // (4) three frames
    const int target = battle_target_list_.empty() ? -1 : battle_target_list_[0]; // dword_F0648
    const int nframes = (int)magic_frames_.size();
    const int dir = (int)u.facing & 3;                                // [60h]>>24
    for (int i = 0; i < 3 && !should_exit_; ++i) {
        const int f = dir * 3 + i;                                   // `lea eax,[eax+eax*2]; add eax,edx`
        if (f >= 0 && f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, cx + fr.xo, cy + fr.yo);                // sub_1FFFE
        }
        battle_fx_present();                                         // 454B3 + 5E9E8 (units included)
        wait_game_tick();                                            // sub_499E4
        if (i == 0) wait_game_tick();                                // `test cx,cx; jnz` -> one extra wait
    }

    // (5) clear dim + release table
    battle_dim_row0_ = false;                                        // sub_44D6E(0)
    battle_nsp_release();                                            // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2F3B2 %s unit=%d k=%d dir=%d target=%d\n", nsp, u.id, k, dir, target);
    return 1;
}

// ==== sub_2FF4B: fistlv4.nsp (F1474 idx 22 finisher) ====
// ASM: COMBATII_code.ASM 44078..44130. The table entry idx 22 calls it with eax = unit.
//
// Original flow:
//   (1) sub_1FEA4("fistlv4.nsp") -> sub_1033B(9) -> sub_44D6E(1) -> sub_1033B(9).
//       The second 1033B(9) is redundant (44D6E only writes FA8B6).
//   (2) sub_135EB(unit) -> centre (x at esp, y at esp+4).
//   (3) 9 frames (`cmp cx,9`): frame = cx%3 (0,1,2,0,1,2,0,1,2) through
//       sub_454B3 -> sub_1FFFE(frame, x, y) -> sub_12F2A(caster) -> sub_5E9E8 -> sub_499E4.
//       The original never changes [unit+64h], so the caster keeps its current pose.
//   (4) sub_44D6E(0) -> sub_1FFCD -> [unit+13h] |= 80h.
//
// Port mapping: 1FEA4 -> battle_nsp_load, 1033B -> game_timer_set, 44D6E ->
//   battle_dim_row0_, 135EB -> unit_screen_center, 454B3 + 5E9E8 -> battle_fx_present
//   (which also runs the 454D5 overlay, like the sibling drivers),
//   1FFFE -> battle_fx_blit (adds the entry's xo/yo), 12F2A -> battle_fx_redraw_unit
//   (queued after the effect so the caster is redrawn on top, as in the original),
//   1FFCD -> battle_nsp_release.
int CombatII::battle_fx_2ff4b(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];
    if (!battle_nsp_load("fistlv4.nsp") || magic_frames_.empty()) {
        u.combat_flags_13 = (uint8_t)(u.combat_flags_13 | 0x80);    // or [13h],80h
        return 0;
    }
    game_timer_set(9);                                               // sub_1033B(9)
    battle_dim_row0_ = true;                                         // sub_44D6E(1)
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);                                   // sub_135EB

    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 9 && !should_exit_; ++i) {                   // cmp cx,9
        const int f = i % 3;                                         // `idiv 3` -> remainder (edx)
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, cx + fr.xo, cy + fr.yo);                // sub_1FFFE
        }
        battle_fx_redraw_unit(unit_index);                           // sub_12F2A(caster)
        battle_fx_present();                                         // 454B3 + 5E9E8
        wait_game_tick();                                            // sub_499E4
        if ((size_t)unit_index >= units_.size()) break;
    }
    battle_dim_row0_ = false;                                        // sub_44D6E(0)
    battle_nsp_release();                                            // sub_1FFCD
    if ((size_t)unit_index < units_.size())
        units_[(size_t)unit_index].combat_flags_13 =
            (uint8_t)(units_[(size_t)unit_index].combat_flags_13 | 0x80);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2FF4B fistlv4 unit=%d frames=9\n", units_[(size_t)unit_index].id);
    return 1;
}
