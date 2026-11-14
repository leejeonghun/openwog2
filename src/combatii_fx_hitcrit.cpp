// Counter / critical hit effects: sub_2D5A3 / sub_2D684
// ASM: dasm/COMBATII_code.ASM 40392..40677. Both routines are called by F1474 weapon
//   handlers (sub_2F56C = idx 2, sub_2F5A7 = idx 4/12/20, sub_2FA1E = idx 15).
//
// Shared sequence (read from the ASM):
//   135EB(unit centre x,y) -> 1FEA4(nsp) -> 4F460(ssp) -> 44D6E(1) (dim the map)
//   -> 44D89(register the caster in the highlight list) -> [unit+64h]=8 -> 4F4CD/1033B
//   -> frame loop -> 44D75(clear the list) -> 44D6E(0) -> 12124(restore pose)
//   -> 1FFCD(release table).
//
// Port mapping: 44D6E -> battle_dim_row0_, 44D89 -> battle_highlight_add,
//   44D75 -> battle_highlight_clear, 454B3 + 5E9E8 -> battle_fx_present,
//   1FFFE -> battle_fx_blit(frame, x+fr.xo, y+fr.yo) (the original 1FFFE adds the entry's
//   xo/yo internally, so the port adds them up front), 499E4 -> wait_game_tick,
//   1033B -> game_timer_set (its ebx/edx arguments are dead: sub_1033B only reads ax),
//   499BD -> combat_rand, 4F460 -> battle_ssp_play, 4F4CD -> sfx.
//
// ---- Shared helper sub_2D48A (40392..40431): full-screen white flash ----
//   al = wait ticks. Fill 768 bytes (256 x RGB) with 0x3F, upload them to the DAC with
//   5F9AE(0,100h) (white), set the period with 1033B(al), wait one 499E4, then restore the
//   master palette with 5F9AE(0,100h,unk_12824C). The port runs the same order as
//   pal_load_range(0,0x100,flash) -> video_.present() -> game_timer_set(delay) ->
//   wait_game_tick() -> pal_load_range(0,0x100,&battle_palette_[0][0]) -> video_.present().
//   2D5A3 passes 0Ah and 2D684 passes 14h (2D684's `mov ecx,5` is dead because sub_2D48A
//   overwrites cl with al).
//   The original programs VGA 3C8h/3C9h directly, so the flash is visible on the current
//   frame; the port must present after the DAC write (SDL converts the palette at present
//   time), otherwise the whole flash is skipped.
#include "combatii.h"
#include "combatii_internal.h"

// ---- sub_2D5A3 (40504..40570): counter.nsp counterattack effect ----
//   Centre from 135EB, then counter.nsp + i-shoot.ssp, dim/highlight registration, pose 8.
//   Frame loop of 10: 1033B(0Fh), frame = i%5, 454B3 + 1FFFE + 5E9E8 + 499E4.
//   On every frame with i%5==4, 4F4CD(32h) + 2D48A(0Ah) add the hit sound and flash.
//   Tail: 44D75 -> 44D6E(0) -> 12124 -> 1FFCD, returns 1.
//   Approximation: the ASM draws the background (454B3) first each frame, then the sprite
//   (1FFFE) and the transfer (5E9E8). The port queues the draws and battle_fx_present emits
//   background, units and sprite at once, producing the same frame content (the same
//   approximation as the other fx drivers).
int CombatII::battle_fx_2d5a3(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("counter.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    auto& u = units_[(size_t)unit_index];
    int fx = 0, fy = 0;
    unit_screen_center(u, fx, fy);                    // sub_135EB (x->[edx], y->[ebx])
    battle_ssp_play("i-shoot.ssp", 0x32);             // sub_4F460
    battle_dim_row0_ = true;                          // sub_44D6E(1)
    battle_highlight_add(unit_index);                 // sub_44D89(eax=unit)
    u.anim_frame = 8;                                 // mov byte [edi+64h],8
    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 0x0A && !should_exit_; ++i) { // cmp cx,0Ah; jl
        game_timer_set(0x0F);                         // sub_1033B(0Fh)
        const int f = i % 5;                          // `idiv 5` remainder -> var_8
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, fx + fr.xo, fy + fr.yo);        // sub_1FFFE
        }
        battle_fx_present();                          // 454B3 + 5E9E8
        if (f == 4) {                                 // `cmp esi,4; jnz`
            sfx(0x32);                                // sub_4F4CD(32h)
            // sub_2D48A(0Ah): cover the palette with 0x3F, restore it 10 ticks later
            uint8_t flash[0x100 * 3];
            for (int p = 0; p < 0x100 * 3; ++p) flash[p] = 0x3F;
            pal_load_range(0, 0x100, flash);           // sub_5F9AE(0,100h,buffer)
            video_.present();                          // DAC write is visible on the current frame
            game_timer_set(0x0A);                     // sub_1033B(0Ah)
            wait_game_tick();                         // sub_499E4
            pal_load_range(0, 0x100, &battle_palette_[0][0]); // 5F9AE(0,100h,unk_12824C)
            video_.present();
        }
        wait_game_tick();                             // sub_499E4
    }
    battle_highlight_clear();                         // sub_44D75
    battle_dim_row0_ = false;                         // sub_44D6E(0)
    reset_unit_pose(units_[(size_t)unit_index]);      // sub_12124
    battle_nsp_release();                             // sub_1FFCD
    return 1;
}

// ---- sub_2D527 (40460..40503): restrain.nsp hold ----
//   135EB(target centre) -> 24 frames: 1033B(0Ah), frame = i%8,
//   454B3 + 1FFFE + 5E9E8 + 499E4. The caller (F1474 loc_2E80C) loads restrain.nsp and
//   owns the 2CBA1 registration, 44D6E dim, 197AD queue removal and 1FFCD release.
int CombatII::battle_fx_2d527(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_fx_loaded()) return 0;
    int fx = 0, fy = 0;
    unit_screen_center(units_[(size_t)unit_index], fx, fy);  // sub_135EB
    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 0x18 && !should_exit_; ++i) {        // cmp cx,18h
        game_timer_set(0x0A);                                // sub_1033B(0Ah)
        const int f = i % 8;                                 // `idiv 8` remainder, *0Ch entry stride
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, fx + fr.xo, fy + fr.yo);       // sub_1FFFE
        }
        battle_fx_present();                                 // 454B3 + 5E9E8
        wait_game_tick();                                    // sub_499E4
    }
    return 1;
}

// ---- sub_2D684 (40571..40677): critical.nsp critical hit effect ----
//   (1) 10 frames: frame = i/2 (0..4). From the 135EB centre the coordinates are
//       x+32h-5i, y-46h+7i each frame, so the sprite flows from the upper right to the lower
//       left. (`[var_C]`=x and `[var_4]`=y are read back as dwords and their high words are
//       re-read as `[var_C+2]`/`[var_4]` - exactly the words 135EB writes to [edx]=x and
//       [ebx]=y.)
//   (2) 2D48A(14h) flash -> (3) 6 frames: frames 5..0Ah at the **original centre**.
//   Tail: 44D75 -> 44D6E(0) -> 12124 -> 1FFCD, returns 1.
//   Approximation: (1)'s 454B3 + 1FFFE + 5E9E8 is folded into battle_fx_blit +
//   battle_fx_present, and the 44D89/44D75/44D6E toggles map to the port's highlight and dim
//   state.
int CombatII::battle_fx_2d684(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("critical.nsp") || magic_frames_.empty()) return 0; // sub_1FEA4
    auto& u = units_[(size_t)unit_index];
    int fx = 0, fy = 0;
    unit_screen_center(u, fx, fy);                    // sub_135EB
    battle_ssp_play("i-star2.ssp", 0x32);             // sub_4F460
    battle_dim_row0_ = true;                          // sub_44D6E(1)
    battle_highlight_add(unit_index);                 // sub_44D89(eax=unit)
    u.anim_frame = 8;                                 // mov byte [ebp+64h],8
    sfx(0x32);                                        // sub_4F4CD(32h)
    game_timer_set(7);                                // sub_1033B(7)
    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 0x0A && !should_exit_; ++i) { // cmp cx,0Ah; jl
        const int f = i / 2;                          // `(cx - sign)/2`
        const int dx = (int16_t)(fx + 0x32 - 5 * i);  // var_C(x)+32h-edi, edi += 5
        const int dy = (int16_t)(fy - 0x46 + 7 * i);  // var_4(y)-46h+esi, esi += 7
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, dx + fr.xo, dy + fr.yo);        // sub_1FFFE
        }
        battle_fx_present();                          // 454B3 + 5E9E8
        wait_game_tick();                             // sub_499E4
    }
    // sub_2D48A(14h)
    uint8_t flash[0x100 * 3];
    for (int p = 0; p < 0x100 * 3; ++p) flash[p] = 0x3F;
    pal_load_range(0, 0x100, flash);                  // sub_5F9AE(0,100h,buffer)
    video_.present();                                 // DAC write is visible on the current frame
    game_timer_set(0x14);                             // sub_1033B(14h)
    wait_game_tick();                                 // sub_499E4
    pal_load_range(0, 0x100, &battle_palette_[0][0]); // sub_5F9AE(0,100h,unk_12824C)
    video_.present();
    for (int i = 5; i < 0x0B && !should_exit_; ++i) { // mov ecx,5 -> cmp cx,0Bh; jl
        game_timer_set(0x0F);                         // sub_1033B(0Fh)
        if (i < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)i];
            battle_fx_blit(i, fx + fr.xo, fy + fr.yo);        // sub_1FFFE
        }
        battle_fx_present();                          // 454B3 + 5E9E8
        wait_game_tick();                             // sub_499E4
    }
    battle_highlight_clear();                         // sub_44D75
    battle_dim_row0_ = false;                         // sub_44D6E(0)
    reset_unit_pose(units_[(size_t)unit_index]);      // sub_12124
    battle_nsp_release();                             // sub_1FFCD
    return 1;
}
