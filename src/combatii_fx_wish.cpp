// sub_3C12D: "Wish to Win!!!" effect (item dx 12 / VM 09 sub 3)
// ASM: dasm/COMBATII_code.ASM 58923..59063. Callers: sub_4CB5E+117 (item use) and
// sub_51FF7+D9 (VM 09 sub 3 -> port play_unit_effect case 3). Both only set eax to the
// unit and never touch edx/ebx, so port argument k is unused.
//
// Original flow:
//   1. [unit+64h]=8 -> 45559 -> 48612(unit, "Wish to Win!!!", 0x11); handled by the caller.
//   2. Mode di: [4Ch]-0C8h >= [48h] selects 0 (HP nearly full); otherwise
//      [2Bh]<=3Ch -> 1 (Heal Wind), else 2 (Sun Light).
//   3. 28422(1) -> 4F460("heal.ssp", 32h, 5) -> 28A97 collect targets -> 253DC(x, y, 1Eh)
//      wide-heal effect -> 2ACCB(2710h) restores 10000 (x100).
//   4. di>0: 28883("Heal Wind!") -> 4F460("heal.ssp" / "cure.ssp") -> 2A75A(edx=2, ebx=3).
//   5. di>1: 28883("Sun Light!") -> 4F460("sun1/sun2/sun.ssp") -> 2A75A(edx=1, ebx=4).
//   6. 28422(0) -> 11EE4 -> 119B6 -> morale +10 -> [1Eh]=0 / [15h]=0 -> 1DAEE/197AD ->
//      1137A/1134F(D6620)/113E4 -> 53E0F -> 53F94 -> 53C12 -> 44DF3 (caster leaves).
//
// Division of work: the gameplay side (steps 2-6, healing and exit) is ported in
// battle_item_3c12d (combatii_battle.cpp); this file owns the visuals, i.e. from the
// 4F460("heal.ssp") call of step 3 through the 2A75A effect of step 5, in the same order.
//
// Port mapping: 45559 -> present_cutscene_frame, 48612 -> battle_unit_label,
// 28422 -> battle_spell_auto_, 4F460 -> battle_ssp_play, 253DC -> battle_fx_253dc
// (wideheal.nsp), 2A75A(ebx=3/4) -> battle_item_2a75a_fx(kind 3/4). 2A75A runs the SSP
// and the NSP driver inside itself, so this function leaves the SSP to it.
//
// Notes:
//   - [64h]=8, 45559 and 48612 ("Wish to Win!!!") are already done by the caller.
//   - 28A97 (target collection) belongs to the gameplay handler; only the centre used by
//     253DC is forced to the caster tile here.
//   - 28883 (spell name announce) is the battle_spell_name_ assignment. The handler sets
//     it right before 2A75A, so overwriting it here would reverse the order.
//   - 253DC's third argument (ebx = 0x1Eh) is unused by the port driver, which takes the
//     period and lifetime from the table; the original value is still passed.
//   - di must be computed before healing: the original tests the pre-heal HP, so call this
//     before battle_spell_heal_2accb to select the same branch.
//   - The healing itself (2A75A edx=2/1, ebx=3/4) is already applied by the handler; this
//     function only plays the effect.
//   - 28422/11EE4/119B6/1DAEE/197AD/1137A/1134F/113E4/53E0F/53F94/53C12/44DF3 (caster
//     exit) are handled by the gameplay handler.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>

int CombatII::battle_fx_3c12d(int unit_index, int tx, int ty, int k) {
    (void)k;                                                     // the caller does not set edx/ebx
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];

    // Step 3 coordinates: movsx edx, byte [esi+1] / movsx eax, byte [esi] = the caster tile.
    //   The original ignores any other tile passed by the caller (253DC's eax/edx are these).
    tx = (int)(int8_t)u.x;
    ty = (int)(int8_t)u.y;

    // Step 2 di: mov eax,[esi+4Ch]; mov edx,[esi+48h]; sub eax,0C8h; cmp eax,edx; jge -> 0;
    //   otherwise cmp byte [esi+2Bh],3Ch; jbe -> 1; else 2.
    int di;
    if (battle_max_hp_raw(u) - 0xC8 >= battle_hp_raw(u)) di = 0;
    else di = (u.combat_level_2b > 0x3C) ? 2 : 1;

    // Step 3: 4F460("heal.ssp", 32h, 5) then the 253DC(x, y, 1Eh) wide-heal effect.
    battle_ssp_play("heal.ssp", 0x32);                           // 4F460("heal.ssp", 32h)
    battle_fx_253dc(unit_index, tx, ty, 0x1E);                   // sub_253DC(eax=x, edx=y, ebx=1Eh)
    if (should_exit_) return 1;

    // Step 4, di>0: Heal Wind effect (battle_item_2a75a_fx plays the SSP and driver at the
    // 2A75A site).
    if (di > 0) {
        battle_item_2a75a_fx(unit_index, 3, 2, tx, ty);          // sub_2A75A(edx=2, ebx=3)
        if (should_exit_) return 1;
    }
    // Step 5, di>1: Sun Light effect.
    if (di > 1) {
        battle_item_2a75a_fx(unit_index, 4, 1, tx, ty);          // sub_2A75A(edx=1, ebx=4)
        if (should_exit_) return 1;
    }
    return 1;
}
