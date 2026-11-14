// sub_3B9E1: swirl.nsp (rotation effect)
// ASM: dasm/COMBATII_code.ASM 58309..58410. Callers: `sub_3F4A1+4C`, `sub_4CB5E+87`
//   (both pass only eax=unit and never read edx/ebx, so k is unused).
//
// Original flow:
//   (1) `mov word ptr dword_F0B9C+2, 0` clears the magic sprite list. 1AC08(unit.x, unit.y,
//       ebx=1) + 1AC59/1A25F collect the units of the radius-1 diamond into F0648. An empty
//       list (dword_F0B9C+2 == 0) ends the routine with eax = -1.
//   (2) 1FEA4("swirl.nsp") -> 135EB(unit centre) -> 1033B(5; ecx=0 is a dead argument).
//   (3) 0Ch-frame loop (cx = 0..0Bh):
//         [unit+63h] = ([unit+63h] + 1) & 3     -- rotate the facing once per frame
//         read dword_BA83C / dword_BA838+2 and draw 454B3 (background + units)
//         frame index = cx % 3 (`idiv 3`, remainder edx -> `imul eax,edx,0Ch`)
//         x = centre x + entry[+2], y = centre y + entry[+4], then 5EE51 (flat blit)
//         5E9E8 -> 499E4 (present + wait)
//   (4) 1FFCD (table release), then 28728(edx=1770h, ebx=14h, ecx=1) for damage ->
//       287AF (hit pose) -> 1ED45 -> eax=1.
//
// Port mapping: 1AC08/1AC59/1A25F -> battle_weapon_collect_dia_at(unit, x, y, 1)
//   (F0648 = battle_target_list_), 1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release,
//   135EB -> unit_screen_center, 1033B -> game_timer_set, 454B3+5E9E8 -> battle_fx_present,
//   5EE51/1FFFE -> battle_fx_blit (frame + xo/yo added), 499E4 -> wait_game_tick.
//
// Approximations:
//   - the damage/pose part of (4) (28728/287AF/1ED45) belongs to the caller, like the other
//     effect drivers; this function only runs the effect. When porting the 3F4A1/4CB5E
//     paths, call battle_apply_spell_hit + battle_spell_pace_287af with base=0x1770,
//     p1=0x14 (=20).
//   - the direct dword_BA83C / dword_BA838+2 reads are absorbed by the background pass of
//     battle_fx_present, which is 454B3 (render_battle_frame) over the same camera globals.
//   - IDA's overlapping `[esp+2]`/`[esp+8+var_A]` (each a dword>>10h) are the two words 135EB
//     stores into [edx]/[ebx], i.e. the unit centre x/y (same reading as the other drivers).
//   - the original never calls 454D5 (overlay) while battle_fx_present includes it, so this
//     effect draws the HUD/markers one extra time (same approximation as its siblings).
#include "combatii.h"
#include "combatii_internal.h"

int CombatII::battle_fx_3b9e1(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;   // the original uses only the unit record
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];

    // (1) radius-1 diamond collection (1AC08 ebx=1). An empty list returns -1 (ASM 3BA2C).
    magic_fx_draws_.clear();                                     // dword_F0B9C+2 = 0
    battle_target_list_.clear();
    const int n = battle_weapon_collect_dia_at(unit_index, (int)(int8_t)u.x, (int)(int8_t)u.y, 1);
    if (n <= 0) return -1;

    // (2) table load + centre + period
    if (!battle_nsp_load("swirl.nsp") || magic_frames_.empty()) return 0;   // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);                               // sub_135EB
    game_timer_set(5);                                           // sub_1033B(5); ecx=0 is dead

    // (3) 12 frames: rotate the facing every frame and draw frame cx%3 at the centre.
    for (int i = 0; i < 0x0C && !should_exit_; ++i) {           // `cmp cx,0Ch; jl`
        if ((size_t)unit_index < units_.size()) {
            auto& uu = units_[(size_t)unit_index];
            uu.facing = (int8_t)(((int)uu.facing + 1) & 3);      // [63h] = ([63h]+1)&3
        }
        const int f = i % 3;                                     // `idiv 3` -> remainder (edx)
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, cx + fr.xo, cy + fr.yo);            // sub_5EE51
        }
        battle_fx_present();                                     // sub_454B3 + sub_5E9E8
        wait_game_tick();                                        // sub_499E4
    }
    battle_nsp_release();                                        // sub_1FFCD
    // (4) 28728(1770h, 14h, 1) + 287AF + 1ED45 are the caller's job (see the header).
    return 1;
}
