// sub_25C88: cure.nsp particles (Cure / MP heal)
// ASM: dasm/COMBATII_code.ASM 30420..30546. Callers: sub_2B1D0(Cure)+11B/+156, sub_3BDC7+4E.
// Flow:
//   1. 1FEA4("cure.nsp") loads the table, 1124A centres the camera on the caster tile.
//   2. The five particles start from dword_1FE50 (`dd 8, 40002h` + `db 6,0` = five words
//      {8, 0, 2, 4, 6}), which are the per-particle ages. Start position is
//      x = 10971(tile x) + 2 + rand(0x15), y = 10988(tile y) - 0x0C + rand(0x15),
//      followed by 4F4CD(0x32) sfx and 1033B(6) timer.
//   3. For 0x32 frames each particle does age[i]++ and, at 9, redraws its position and resets
//      the age to 0. Frame = level*5 + (age >> 1) (`sar ebx,11h` on the age dword), entry =
//      unk_F0DAA + frame*0x0C, drawn with 1FFFE(eax = x, edx = y). Each frame ends with
//      5E9E8 + 499E4, and the routine closes with 1FFCD.
//   4. The third argument (ebx) is the caller's level (0..2) and groups the frames in fives:
//      sub_2B1D0 passes the target level, sub_3BDC7 passes `xor ebx,ebx` = 0.
// Port map: 1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release, 1FFFE -> battle_fx_blit,
//   454B3+5E9E8 -> battle_fx_present, 499E4 -> wait_game_tick, 499BD -> combat_rand,
//   4F4CD -> sfx, 1124A -> camera_center_on, 10971/10988 -> tile coords via TILE_ANCHOR_X/Y.
// Deviation: the original receives tile coordinates only, not a unit record; the port's
//   signature carries unit_index and falls back to that unit's tile only when the passed tile
//   is outside the map.
// 1FFFE takes (eax = x, edx = y): sub_1FFFE adds the entry's [+2]>>16 and [+4]>>16 and calls
//   sub_5EE51(eax = x, edx = y, ebx = entry).
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>
#include <vector>

int CombatII::battle_fx_25c88(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    // Tile coords: the original takes no unit record (callers pass [unit] / [unit+1]).
    int ax = tx, ay = ty;
    if (ax < 0 || ay < 0 || ax >= (int)map_w_ || ay >= (int)map_h_) {
        ax = (int)(int8_t)u.x;
        ay = (int)(int8_t)u.y;
    }
    if (!battle_nsp_load("cure.nsp") || magic_frames_.empty()) return 0;   // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    const int level = (k < 0) ? 0 : k;                                    // ebx (3rd argument)
    camera_center_on(ax, ay);                                             // sub_1124A
    // sub_10971 / sub_10988: tile -> screen coords (TILE_ANCHOR_X/Y = +4).
    auto sx_of = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto sy_of = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    const int base_x = sx_of(ax), base_y = sy_of(ay);

    // dword_1FE50: `dd 8, 40002h` / `db 6,0` -> five words {8, 0, 2, 4, 6}.
    int age[5] = {8, 0, 2, 4, 6};
    int px[5] = {0, 0, 0, 0, 0};
    int py[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i) {                                         // loc_25CBE
        px[i] = base_x + 2 + combat_rand(0x15);                           // 10971+2+rand(21)
        py[i] = base_y - 0x0C + combat_rand(0x15);                        // 10988-0Ch+rand(21)
    }
    sfx(0x32);                                                            // sub_4F4CD(32h)
    game_timer_set(6);                                                    // sub_1033B(6)

    for (int f = 0; f < 0x32 && !should_exit_; ++f) {                     // `cmp di,32h`
        for (int i = 0; i < 5 && !should_exit_; ++i) {                    // loc_25D3F
            if (++age[i] >= 9) {                                          // `inc; cmp 9; jnz`
                px[i] = base_x + 2 + combat_rand(0x15);
                py[i] = base_y - 0x0C + combat_rand(0x15);
                age[i] = 0;
            }
            // Frame = level*5 + (age >> 1); the original builds the entry address with `imul 0Ch`.
            int fr = level * 5 + (age[i] >> 1);
            if (fr < 0) fr = 0;
            if (fr >= nframes) fr = nframes - 1;
            battle_fx_blit(fr, px[i], py[i]);                             // sub_1FFFE
        }
        battle_fx_present();                                              // sub_454B3 + sub_5E9E8
        wait_game_tick();                                                 // sub_499E4
    }
    battle_nsp_release();                                                 // sub_1FFCD
    return 1;
}

// ==== sub_25DFE: healwind.nsp (Wide Heal / Heal Wind effect) ====
// ASM: dasm/COMBATII_code.ASM 30547..30820. Callers: sub_2A75A+428(kind 3),
//   sub_4426D+3BE(edx = 2).
// Stack frame 0x570 (eax = unit, dl = k):
//   0x000..0x2FF palette buffer (a copy of master+3, turned into a delta by 5FC21)
//   0x300..0x395 Y[0x4B],     0x398..0x42D age/speed[0x4B]
//   0x430..0x4C5 X speed[0x4B], 0x4C8..0x55D X[0x4B]
//   0x560 dword {low: centre sprite y (starts 0x6E), high: frame counter}
//   0x564 dword {low: nTarget (starts 0), high: palette %}
//   0x568 dword palette ramp % (starts 0), 0x56C byte = k+1 (stage count)
//   IDA shows these slots as overlapping +2 views (var_10/var_C/var_8/var_16); the addresses
//   above are the resolved layout.
// Flow:
//   1. 1FEA4("healwind.nsp"), 1124A(caster tile), 1033B(8), then clear both 0x4B-entry arrays.
//   2. Palette: buffer = 100h copies of palette[1] (unk_12824F), delta via
//      5FC21(0,100h,master,buffer); ramp via 5FD1C(0,100h,master,delta,pct) for pct
//      0,20,40,60,80 then 100.
//   3. 4F4CD(0x32) sfx.
//   4. Dim: the same 5FD1C from pct = 100 down by 0x0A while pct > 0 (100..10).
//   5. Main loop while nTarget < 25*(k+1):
//      - 454B3 draws the centre sprite at frame nTarget%3 with 1FFFE(eax = 0xAB, edx = centre y).
//      - Exactly ONE empty slot is revived (loc_25F89): X = 0xAB, Y = centre y,
//        vx = rand(0x15)-0x0A, vy = rand(0x15)-0x0A.
//      - Every slot (loc_26004): a slot with vx == 0 or vy == 0 is skipped as-is (the original
//        leaves it dead for good). Outside Y (5,0xBE) or X (5,0x13B) the slot is respawned at
//        once: vx = vy = 0, X = entry[+4]+0xAB, Y = entry[+6]+0x65, centre y++, and
//        nTarget += 4-(k+1) unless nTarget == 0x60. Otherwise frame = (nTarget+i)%5 + 3 blits
//        with 1FFFE(eax = X, edx = Y), then X += vx and Y += vy.
//        Each slot plays 4F4CD(0x33) when rand(0x0A) == 0.
//      - Frame ends with 5E9E8 + 5E943 + 5FD1C(pct) + 499E4.
//   6. Exit: 45559, then 5FD1C from pct 100 down by 4 while pct > 4, 1FFCD, 1124A(caster).
// Port map: 1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release, 1FFFE -> battle_fx_blit,
//   454B3+5E9E8 -> battle_fx_present, 499E4/5E943 -> wait_game_tick, 499BD -> combat_rand,
//   1033B -> game_timer_set, 4F4CD -> sfx, 45559 -> present_cutscene_frame,
//   5FC21+5FD1C -> pal_shift_fade (same delta and formula), 1124A -> camera_center_on.
// Deviations:
//   - The per-frame 5FD1C of the main loop runs at pct = 0 after step 4, i.e. no change; the
//     original reads the same slot, so the call is omitted.
//   - Slot revive uses the low word of the centre-y dword while the original's overlapping IDA
//     view reads the high word; the port reads the sprite position the particle is born at.
//   - `4 - (k+1)` comes from `mov al, byte [esp+574h+var_8]` with displacement 4, i.e.
//     var_4 = k+1; the revive count is therefore always 25 regardless of the stage count.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdint>
#include <vector>

int CombatII::battle_fx_25dfe(int unit_index, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("healwind.nsp") || magic_frames_.empty()) return 0;   // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    const int cx = (int)(int8_t)caster.x;
    const int cy = (int)(int8_t)caster.y;
    camera_center_on(cx, cy);                                                  // sub_1124A

    const int stages = ((k < 0) ? 0 : k) + 1;                                  // var_4 = dl+1
    const int slot_count = stages * 25;                                        // `imul eax,19h`
    game_timer_set(8);                                                         // sub_1033B(8)

    std::vector<int> vx((size_t)slot_count, 0);                                // 0x430
    std::vector<int> vy((size_t)slot_count, 0);                                // 0x398
    std::vector<int> px((size_t)slot_count, 0);                                // 0x4C8
    std::vector<int> py((size_t)slot_count, 0);                                // 0x300

    // Step 2 palette ramp: 5FC21(delta) + 5FD1C at pct 0,20,40,60,80 (ends at pct = 100).
    for (int pct = 0; pct < 0x64; pct += 0x14) pal_shift_fade(pct);
    sfx(0x32);                                                                 // sub_4F4CD(32h)
    // Step 4 dim: pct 100 -> 10 in steps of 0x0A; the original leaves the loop at 0.
    for (int pct = 0x64; pct > 0; pct -= 0x0A) pal_shift_fade(pct);

    int n_target = 0;                                                          // var_C low
    int sprite_y = 0x6E;                                                       // var_10 low
    for (int guard = 0; guard < 0x4000 && !should_exit_; ++guard) {
        if (n_target >= slot_count) break;                                     // `cmp ecx,eax; jge`
        const int central = n_target % 3;                                      // remainder of `idiv 3`
        // Centre sprite: 1FFFE(eax = 0xAB, edx = centre y).
        if (central < nframes) battle_fx_blit(central, 0xAB, sprite_y);
        const MagicFxFrame& cfr = magic_frames_[(size_t)(central < nframes ? central : 0)];

        // Revive exactly one empty slot (loc_25F89 leaves for loc_25FFD).
        for (int i = 0; i < slot_count; ++i) {
            if (vx[(size_t)i] != 0 || vy[(size_t)i] != 0) continue;
            px[(size_t)i] = 0xAB;                                              // X = 0ABh
            py[(size_t)i] = sprite_y;                                          // Y = centre y
            vx[(size_t)i] = combat_rand(0x15) - 0x0A;
            vy[(size_t)i] = combat_rand(0x15) - 0x0A;
            break;
        }

        // Update and draw (loc_26004).
        for (int i = 0; i < slot_count && !should_exit_; ++i) {
            const size_t si = (size_t)i;
            if (vx[si] == 0 || vy[si] == 0) continue;                          // dead slot
            if (py[si] > 0xBE || py[si] < 5 || px[si] < 5 || px[si] > 0x13B) {
                // loc_260CE: respawn at once - move by the entry's xo/yo and advance nTarget.
                vx[si] = 0;
                vy[si] = 0;
                px[si] = 0xAB + cfr.xo;                                        // [+4]
                py[si] = 0x65 + cfr.yo;                                        // [+6]
                ++sprite_y;                                                    // `inc [var_10]`
                if (n_target != 0x60) n_target += 4 - stages;                   // `4-(k+1)`
            } else {
                const int f = ((n_target + i) % 5) + 3;                        // (nTarget+i)%5 + 3
                if (f < nframes) battle_fx_blit(f, px[si], py[si]);            // 1FFFE(eax=X, edx=Y)
                px[si] += vx[si];
                py[si] += vy[si];
            }
            if (combat_rand(0x0A) == 0) sfx(0x33);                             // 4F4CD(33h)
        }
        battle_fx_present();                                                   // 454B3 + 5E9E8
        wait_game_tick();                                                      // 5E943 + 499E4
    }
    if (should_exit_) {
        battle_nsp_release();
        return 1;
    }

    // Step 6 exit: 45559, palette pct 100..8 (-4), 1FFCD, then restore the camera.
    present_cutscene_frame();                                                   // sub_45559
    for (int pct = 0x64; pct > 4; pct -= 4) pal_shift_fade(pct);
    battle_nsp_release();                                                       // sub_1FFCD
    camera_center_on(cx, cy);                                                   // sub_1124A
    return 1;
}
