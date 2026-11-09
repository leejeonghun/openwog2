// sub_31283: asura2.nsp (V-weapon F1474 index 1, kang.ssp + f-blast.ssp + slash.ssp)
// ASM: dasm/COMBATII_code.ASM 45691..46913. Entry takes one unit in eax and returns
//   ebx = 1 (success) or -1 (no target) (loc_3257C: `mov eax,ebx`).
//
// Original flow:
//   (1) 45691..45725: clear `dword_F0B9C+2`, then a 12x12 row-major camera sweep calls
//       sub_1A1F8(x, y, ebx=[60h]>>24) to fill F0648. No target returns -1.
//   (2) loc_3130A: zero var_D0[0x32] (slot active) -> save var_28 = [63h] ->
//       61147(eax=1, edx=50h) tint LUT, 103A8 clears the LUT flag -> 1FEA4("asura2.nsp")
//       -> 11EE4 (release caster and target tiles) -> [63h]=0 -> 1033B(5) ->
//       44D6E(1, 28h) dim -> 30824(0) palette mix -> one frame of
//       454B3+12F2A+5E9E8+499E4.
//   (3) var_58 (wave) = 1, var_5C/var_18 save the caster tile, target tiles go to
//       var_2F8[]/var_1CC[], var_C (frame) = 0.
//   (4) loc_3144D main loop (spawns while var_C < 0x64):
//       - 30F61 toggles the 1px camera shake, then 454B3.
//       - frame % 9 == 8 increments var_58.
//       - Activate exactly one free slot (first free index jumps to loc_315F5):
//         var_9C = rand(5)+2 (fall speed), var_230 = var_58 (height seed),
//         var_104 = rand(3)+0x10 (line colour), var_294 = rand(0x19) (x phase),
//         var_168 = 0 (counter).
//       - Update 50 slots: counter < -0x96 deactivates the slot. With
//         var_230+var_168 > 0 draw a vertical line at (caster/target screen x +
//         var_294, y) to the same x at y+var_168; otherwise, when y+var_168 >= 4,
//         draw (x+var_294, y+var_168) down by var_230. Both use 5EB4A(colour = var_104)
//         and 5EB58, then var_168 -= var_9C.
//       - Once var_C > 0x64 the targets and caster sink while 10988(y) > 0.
//       - Each frame ends with 5E9E8+499E4; var_C % 0x0A == 0 plays 4F4CD(34h).
//       - The loop exits at loc_318A0 when var_C > 0x64 and no slot is free.
//   (5) loc_318A0: restore tile coordinates -> 1033B(32h) -> 5FA2C(0, 100h, master, pct)
//       from 32h down to 0 (-4) -> 30F98 (shake + buffer clear) -> one frame -> pct 0..64h
//       (+4) -> 1033B(5).
//   (6) 3196F..: var_14 = 10971(caster x)+0Bh, var_1C = 10988(caster y)+0Eh, var_34 = 3,
//       [64h] = 9. Per target: 1214F(target), var_9C = rand(6)+0Fh, var_104 = 0,
//       var_230 = i*64h+0C8h (i = target index; `imul ebx,edi,64h; add ebx,0C8h`).
//   (7) loc_31A5E falling strike: repeat until at least one slot per target has
//       var_230[i] == 0. Each round plays 4F4CD(33h), then 4 passes x 3 frames:
//         var_34 = (var_34+2 > 8) ? 3 : var_34+2, var_230[k] -= var_9C[k] (floor 0),
//         y = 10988(target y) - var_230[k], x = 10971(target x) + 0Fh.
//         var_230[k] != 0: one 5F305 frame (dword_F0E72 = NSP entry 16).
//         var_230[k] == 0: 3108E (shake target [65h]: 0 -> -2, 1 -> +4, 2 -> -4), then
//           var_9C[k] < 64h promotes it to 64h and draws nothing;
//           < 67h draws F0E7A/F0E7C/F0E7E (entry 17) then entry (var_9C-51h) at +0Ah;
//           < 68h draws F0E86/F0E88/F0E8A (entry 18) then entry 22 at +0Ah;
//           otherwise 30FD1(x, y) (two frames plus a 12-frame column at 10h spacing).
//           Both impact branches then increment var_9C[k].
//         After each target 12F2A, then the caster frame (var_10 == 3 uses F0DAA[0], else
//         var_10) through 5EE51 + 12F2A(caster) + 5E9E8+499E4.
//       Each round ends with 4F4CD(34h).
//   (8) loc_31D6D: 3 rounds x 4 subframes (edi) x 3 frames: 30F98 -> per target
//       30FD1+3108E+12F2A -> 5EE51(F0DAA[edi == 3 ? 0 : edi]) + 12F2A(caster) +
//       5E9E8+499E4; each round ends with 34h.
//   (9) 31EE3: 6 rounds x 3 (esi 9..0Bh) x 3 frames: the same 30FD1/3108E, then
//       5F29D(esi) at (var_48, var_20). The following loc_32019 sets [64h] = 7, lowers the
//       caster y one tile at a time, and drops the D66A8 RLE silhouette (5F517 six-row
//       scan plus 5F3FD full body) 14 times with var_38 from 0x12C down by 64h and
//       var_60 += 14h.
//   (10) 322CB: di = 0Ch..0Fh, 3 frames each, 5F29D(di) at y+61h (di == 0Dh is a buffer
//       memset).
//   (11) Tail (46824..46913): 5E9E8+499E4 -> 1FFCD -> 310DE (0xFA00 buffer screen
//       transition) -> 1033B(64h) -> 5FA2C 64h..0 (-4, 499E4 each step) -> 454B3 + 12F2A
//       for all + 5E9E8+499E4 -> 5FA2C 0..64h (+4) -> 5F9AE -> 44D6E(0) -> BA838 = 0 ->
//       [64h] = 4 -> 11E04 for all -> restore [63h] -> 12124 for all ->
//       2CAF7(caster, ebx=0Ah, edx=3E8h) -> 103A0/61115 -> eax = 1.
//       The port skips only the 310DE 0xFA00 scan transition; both palette ramps run.
//
// Port mapping: 1A1F8->battle_weapon_collect_screen, 1FEA4->battle_nsp_load,
//   1FFCD->battle_nsp_release, 1FFFE->battle_fx_blit, 5EE51->battle_fx_blit,
//   5F305->battle_fx_blit_tinted, 5F29D->battle_fx_blit_tinted_unclipped,
//   5F517->blit_nc2_frame_solid, 5F3FD->blit_nc2_frame,
//   454B3->battle_fx_world, 5E9E8->battle_fx_commit, 454B3+5E9E8->battle_fx_present,
//   1033B->game_timer_set, 499E4->wait_game_tick, 4F4CD->sfx, 499BD->combat_rand,
//   30824->fx_pal_mix(0x60, 0x100, 0, 0), 5FA2C->pal_scale_range,
//   44D6E->battle_dim_row0_, 12F2A->(battle_fx_world draws every unit),
//   12124->reset_unit_pose, 2CAF7->battle_weapon_2caf7_loop,
//   30F61->cam_fine_y_ toggle (1px shake), 30FD1->column_trail(), 3108E->wobble().
//
// Deviations:
//   - 5EB4A/5EB58 (set colour + Bresenham line) are canvas primitives the port lacks, so
//     this file has equivalents (asura_put/asura_line) and draws directly between
//     battle_fx_world and battle_fx_commit, the same window the original uses.
//   - The third argument of 5F305/5F29D is the tint LUT at [entry+8] in the original. The
//     port takes a percentage, fixed at tint 50h to match 61147(1, 50h).
//   - Per-unit 12F2A calls are omitted because battle_fx_world/flush already draw every
//     unit once per frame.
//   - 11EE4/11E04 (release/re-occupy) and 1214F (fixed-pose draw) are replaced by the
//     units_ model and the renderer; reset_unit_pose is called from the tail.
//   - 61147/6104A/61115 (LUT setup and restore) are covered by the port's palette path;
//     103A8/103A0 map to weather_tick_enabled_.
//   - 30F98 clears the visible 200x480 viewport and toggles the 1px shake. The port has no
//     separate cleared canvas, so the shake is kept and the battle world is redrawn, which
//     leaves the map visible behind stages (6)-(10) instead of a cleared field. The one
//     explicit clear in (10) (di == 0Dh, colour 1) is applied to the backbuffer instead.
//   - 310DE was measured as 7 passes (var_4 += 8); each pass memsets the whole display
//     (17700h) and then rebuilds each row from the snapshot via loop A ((200-y)*8/5, left,
//     starting row var_8 = pass*5) and loop B (y*8/5, right). That scan transition is
//     reproduced as-is. Only the high-word offsets read by both loops come from
//     uninitialised stack and are kept at 0. 61115 (release tint LUT) is a no-op because
//     the port caches no LUT; the two palette ramps run through 5FA2C(0..100h) and 499E4.
//   - Frame selection in (7)(8)(9) uses unk_F0DAA entry offsets, and magic_frames_ holds
//     the same order, so it is expressed as frame numbers (0..8, reusing 0). The 12-frame
//     column at 10h spacing in 30FD1 follows the same rule.
//   - The D66A8 RLE silhouette in (9) uses nc2_frame_for(type, 1, 5) (record +0D0h, the
//     16th entry) with 5F517's six-row scan (edi = 5..0) mapped to blit_nc2_frame_solid
//     and 5F3FD to blit_nc2_frame. 5F517's colour argument (var_68) is `[esp+2B2h]>>16`,
//     the low word of var_44 (2Fh), decremented per row to 2Fh..2Ah.
//   - 61115 frees dword_12764C[] (the 0x100-entry tint LUT). The port computes the tint per
//     blit instead of caching it, so there is no equivalent (no-op).
//   - The `[var_C+2]/[var_8+2] >> 16` terms added by loops A/B read the high words of a
//     pointer and a word local, so they depend on the runtime stack. In the game they are
//     effectively 0, and the port pins them to 0.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kAsuraSlots = 0x32;   // size of var_D0/var_9C/var_104/var_230/var_294/var_168

// sub_5EB00/5EE12 equivalent: canvas coordinates -> backbuffer pixel (off-screen is dropped).
inline void asura_put(uint8_t* bb, int x, int y, uint8_t px) {
    if (!bb) return;
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    bb[(size_t)y * (size_t)SCREEN_W + (size_t)x] = px;
}

// sub_5EB58(eax=x0, edx=y0, ebx=x1, ecx=y1): Bresenham line; the colour comes from
// 5EB4A(word_755D4).
void asura_line(uint8_t* bb, int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    const int sx = (x0 <= x1) ? 1 : -1;
    const int sy = (y0 <= y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        asura_put(bb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

}  // namespace

int CombatII::battle_fx_31283_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    // ---- (1) 12x12 row-major screen sweep (1A1F8); no target returns -1 ----
    if (battle_weapon_collect_screen(unit_index, 0) <= 0 || battle_target_list_.empty()) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 31283 unit=%d empty -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    if (!battle_nsp_load("asura2.nsp") || magic_frames_.empty()) {   // sub_1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 31283 unit=%d asura2.nsp missing\n", units_[(size_t)unit_index].id);
        return -1;
    }
    const int nframes = (int)magic_frames_.size();
    auto& C = units_[(size_t)unit_index];
    const std::vector<int> targets = battle_target_list_;
    const int count = (int)targets.size();
    auto target_at = [&](int k) -> CombatUnitPlacement* {
        const int ti = targets[(size_t)k];
        if (ti < 0 || (size_t)ti >= units_.size()) return nullptr;
        return &units_[(size_t)ti];
    };
    // sub_10971/sub_10988: tile origin, i.e. TILE_ANCHOR (4,4), not the unit-sprite anchor.
    // The particle lines and NSP blits are positioned from this origin; using
    // UNIT_ANCHOR_X/Y shifts the first effect 12px right and 2px up.
    auto screen_x = [&](int8_t tx) {
        return ((int)tx - (int)cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto screen_y = [&](int8_t ty) {
        return ((int)ty - (int)cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    // sub_30F61/sub_30F98: toggle dword_BA838 between 1 and -1, then redraw (454B3).
    auto shake_world = [&]() {
        cam_fine_y_ = (cam_fine_y_ == (int16_t)1) ? (int16_t)-1 : (int16_t)1;
        battle_fx_world();
    };

    // ---- (2) setup (loc_3130A) ----
    const int8_t saved_facing = C.facing;                  // var_28
    const int16_t saved_caster_x = C.x, saved_caster_y = C.y;  // var_5C / var_18
    weather_tick_enabled_ = false;                         // sub_103A8
    game_timer_set(5);                                     // sub_1033B(5)
    battle_dim_row0_ = true;                               // sub_44D6E(1, 28h)
    fx_pal_mix(0x60, 0x100, 0, 0x28);                      // sub_30824(0, 28h); edx from 44D6E
    C.facing = 0;                                          // mov byte [ebp+63h],0
    if (should_exit_) { battle_dim_row0_ = false; battle_nsp_release(); return 1; }
    battle_fx_present();                                   // 454B3 + 12F2A + 5E9E8 + 499E4
    wait_game_tick();

    std::vector<int16_t> saved_x((size_t)count, 0), saved_y((size_t)count, 0);  // var_2F8/var_1CC
    for (int k = 0; k < count; ++k) {
        if (const CombatUnitPlacement* T = target_at(k)) {
            saved_x[(size_t)k] = T->x;
            saved_y[(size_t)k] = T->y;
        }
    }

    // ---- (3)(4) main loop (loc_3144D) ----
    std::vector<uint8_t> active((size_t)kAsuraSlots, 0);   // var_D0
    std::vector<uint8_t> speed((size_t)kAsuraSlots, 0);    // var_9C
    std::vector<uint8_t> color((size_t)kAsuraSlots, 0);    // var_104
    std::vector<int16_t> height((size_t)kAsuraSlots, 0);   // var_230
    std::vector<int16_t> phase((size_t)kAsuraSlots, 0);    // var_294
    std::vector<int16_t> counter((size_t)kAsuraSlots, 0);  // var_168
    int wave = 1;                                          // var_58
    int frame = 0;                                         // var_C
    const int cast_sx = screen_x(saved_caster_x);
    const int cast_sy = screen_y(saved_caster_y) + 0x0C;

    while (!should_exit_) {
        shake_world();                                     // sub_30F61: shake toggle + 454B3
        // Spawn (var_C < 0x64): one wave every 9 frames, activating the first free slot only
        if (frame < 0x64) {
            if ((frame % 9) == 8) ++wave;
            for (int i = 0; i < kAsuraSlots; ++i) {
                if (active[(size_t)i] != 0) continue;
                active[(size_t)i] = 1;
                speed[(size_t)i] = (uint8_t)(combat_rand(5) + 2);
                height[(size_t)i] = (int16_t)wave;
                color[(size_t)i] = (uint8_t)(combat_rand(3) + 0x10);
                phase[(size_t)i] = (int16_t)combat_rand(0x19);
                counter[(size_t)i] = 0;
                break;                                     // `jmp loc_315F5`
            }
        } else {
            // var_C > 0x64: targets and caster rise one tile while 10988(y) > 0
            for (int k = 0; k < count; ++k) {
                CombatUnitPlacement* T = target_at(k);
                if (!T) continue;
                if (screen_y(T->y) > 0) T->y = (int8_t)(T->y - 1);
            }
            if (screen_y(C.y) > 0) C.y = (int8_t)(C.y - 1);
        }
        // Slot update and vertical lines (loc_31604)
        uint8_t* bb = video_.get_backbuffer();
        for (int i = 0; i < kAsuraSlots && !should_exit_; ++i) {
            if (active[(size_t)i] != 1) continue;
            // loc_3165C: counter < -0x96 deactivates the slot but still draws this frame.
            if (counter[(size_t)i] < -0x96) active[(size_t)i] = 0;
            const int sum = (int)height[(size_t)i] + (int)counter[(size_t)i];
            if (sum > 0) {
                const int x = cast_sx + phase[(size_t)i];
                asura_line(bb, x, cast_sy, x, cast_sy + counter[(size_t)i],
                           color[(size_t)i]);
            } else if (cast_sy + counter[(size_t)i] >= 4) {
                const int x = cast_sx + phase[(size_t)i];
                const int y0 = cast_sy + counter[(size_t)i];
                asura_line(bb, x, y0, x, y0 + height[(size_t)i], color[(size_t)i]);
            }
            // One line per target using the same slot state, anchored on the target screen position
            for (int k = 0; k < count; ++k) {
                const int bx = screen_x(saved_x[(size_t)k]) + phase[(size_t)i];
                const int by = screen_y(saved_y[(size_t)k]) + 0x0C;
                if (sum > 0) {
                    asura_line(bb, bx, by, bx, by + counter[(size_t)i], color[(size_t)i]);
                } else if (by + counter[(size_t)i] >= 4) {
                    const int y0 = by + counter[(size_t)i];
                    asura_line(bb, bx, y0, bx, y0 + height[(size_t)i], color[(size_t)i]);
                }
            }
            counter[(size_t)i] = (int16_t)(counter[(size_t)i] - speed[(size_t)i]);
        }
        battle_fx_commit();                                // sub_5E9E8
        wait_game_tick();                                  // sub_499E4
        // loc_317F7..loc_31850: after frame > 0x64 the loop ends only when the scan finds
        // no active slot at all (`cmp var_D0[i],1; jz found`, exit if var_64 == 32h).
        if (frame > 0x64) {
            bool any_active = false;
            for (int i = 0; i < kAsuraSlots; ++i) {
                if (active[(size_t)i] == 1) { any_active = true; break; }
            }
            if (!any_active) break;
        }
        ++frame;
        if (!should_exit_ && (frame % 0x0A) == 0) sfx(0x34);   // 4F4CD(34h)
    }
    cam_fine_y_ = CAM_FINE_Y_REST;

    // ---- (5) restore tile coordinates, palette fade down and up (loc_318A0) ----
    C.x = saved_caster_x;
    C.y = saved_caster_y;
    for (int k = 0; k < count; ++k) {
        if (CombatUnitPlacement* T = target_at(k)) {
            T->x = (int8_t)saved_x[(size_t)k];
            T->y = (int8_t)saved_y[(size_t)k];
        }
    }
    game_timer_set(1);                                       // sub_1033B(1); 32h is the esi palette start
    for (int pct = 0x32; pct >= 0 && !should_exit_; pct -= 4) {
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);   // sub_5FA2C
        wait_game_tick();
    }
    if (!should_exit_) {                                     // sub_30F98 + one frame
        cam_fine_y_ = (cam_fine_y_ == (int16_t)1) ? (int16_t)-1 : (int16_t)1;
        battle_fx_present();
        wait_game_tick();
    }
    for (int pct = 0; pct <= 0x64 && !should_exit_; pct += 4) {
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }

    // ---- (6)(7) falling strike (loc_3196F / loc_31A5E) ----
    game_timer_set(5);
    const int anchor_x = screen_x(C.x) + 0x0B;               // var_14
    const int anchor_y = screen_y(C.y) + 0x0E;               // var_1C
    C.anim_frame = 9;                                        // mov byte [ebp+64h],9
    int sub_frame = 3;                                       // var_34
    for (int k = 0; k < count; ++k) {
        speed[(size_t)k] = (uint8_t)(combat_rand(6) + 0x0F);
        color[(size_t)k] = 0;
        height[(size_t)k] = (int16_t)(k * 0x64 + 0xC8);      // imul ebx,edi,64h; add ebx,0C8h
    }
    // sub_3108E: shake target [65h] by 0 -> -2, 1 -> +4, 2 -> -4
    std::vector<uint8_t> wob((size_t)count, 0);              // reuses the var_104 slot
    auto wobble = [&](int k) {
        CombatUnitPlacement* T = target_at(k);
        if (!T) return;
        const uint8_t st = wob[(size_t)k];
        if (st == 0) { T->walk_off_x = (int8_t)(T->walk_off_x - 2); wob[(size_t)k] = 1; }
        else if (st == 1) { T->walk_off_x = (int8_t)(T->walk_off_x + 4); wob[(size_t)k] = 2; }
        else { T->walk_off_x = (int8_t)(T->walk_off_x - 4); wob[(size_t)k] = 1; }
    };
    // sub_30FD1(eax=&frame, edx=&target): entry [f] at the target, then a 12-step
    // column of entry [f+1] at 10h spacing from by-19h down to by-C9h. There is no
    // second blit of [f+1] at the target itself.
    auto column_trail = [&](int k, int f) {
        const int bx = screen_x(saved_x[(size_t)k]) + 0x0C;
        const int by = screen_y(saved_y[(size_t)k]) + 0x0C;
        if (f < 0 || f + 1 >= nframes) return;
        const MagicFxFrame& fr = magic_frames_[(size_t)f];
        const MagicFxFrame& fr2 = magic_frames_[(size_t)(f + 1)];
        battle_fx_blit_tinted(f, bx + fr.xo, by + fr.yo, 0x50);
        for (int j = 0; j < 12; ++j) {
            const int y = by - 0x19 - 0x10 * j;
            battle_fx_blit_tinted(f + 1, bx + fr2.xo, y + fr2.yo, 0x50);
        }
    };
    auto blit_entry = [&](int f, int x, int y) {
        if (f >= 0 && f < nframes) battle_fx_blit_tinted(f, x, y, 0x50);   // sub_5F305
    };
    int rounds = 0;
    while (!should_exit_ && rounds++ < 64) {
        int done = 0;
        for (int k = 0; k < count; ++k) if (height[(size_t)k] == 0) ++done;
        if (done >= count) break;
        sfx(0x33);                                           // 4F4CD(33h)
        for (int pass = 0; pass < 4 && !should_exit_; ++pass) {
            sub_frame = (sub_frame + 2 > 8) ? 3 : sub_frame + 2;
            for (int rep = 0; rep < 3 && !should_exit_; ++rep) {
                shake_world();                               // sub_30F98 + 454B3
                for (int k = 0; k < count; ++k) {
                    height[(size_t)k] = (int16_t)std::max(0, (int)height[(size_t)k] - (int)speed[(size_t)k]);
                    const int bx = screen_x(saved_x[(size_t)k]) + 0x0F;
                    const int by = screen_y(saved_y[(size_t)k]) - height[(size_t)k];
                    if (height[(size_t)k] != 0) {
                        // loc_31C79: dword_F0E72 is NSP entry 16; dword_F0E6E supplies
                        // its +4 (x) / +6 (y) offsets.
                        const MagicFxFrame& fr = magic_frames_[16];
                        blit_entry(16, bx + fr.xo, by + fr.yo);
                    } else {
                        wobble(k);                           // 3108E
                        uint8_t& sp = speed[(size_t)k];
                        if (sp < 0x64) {
                            sp = 0x64;                       // promote, nothing drawn this frame
                        } else if (sp < 0x67 && nframes > 18) {
                            // F0E7A/F0E7C/F0E7E = entry 17; the second blit is entry
                            // (sp-51h) = 19..21 at sx+entry.x, by+0Ah+entry.y.
                            const MagicFxFrame& f17 = magic_frames_[17];
                            blit_entry(17, bx + f17.xo, by + f17.yo);
                            const int e = sp - 0x51;
                            if (e < nframes) {
                                const MagicFxFrame& fe = magic_frames_[(size_t)e];
                                blit_entry(e, bx + fe.xo, by + 0x0A + fe.yo);
                            }
                            ++sp;
                        } else if (sp < 0x68 && nframes > 18) {
                            // F0E86/F0E88/F0E8A = entry 18; the second blit is entry 22.
                            const MagicFxFrame& f18 = magic_frames_[18];
                            blit_entry(18, bx + f18.xo, by + f18.yo);
                            const int e = sp - 0x51;
                            if (e < nframes) {
                                const MagicFxFrame& fe = magic_frames_[(size_t)e];
                                blit_entry(e, bx + fe.xo, by + 0x0A + fe.yo);
                            }
                            ++sp;
                        } else {
                            column_trail(k, sub_frame);      // loc_31C64: sub_30FD1
                        }
                    }
                }
                // Caster frame: var_10 (= pass) == 3 uses F0DAA[0], otherwise var_10
                const int cf = (pass == 3) ? 0 : pass;
                if (cf < nframes) {
                    const MagicFxFrame& fr = magic_frames_[(size_t)cf];
                    battle_fx_blit(cf, anchor_x + fr.xo, anchor_y + fr.yo);  // sub_5EE51
                }
                battle_fx_present();
                wait_game_tick();
            }
        }
        sfx(0x34);                                           // 4F4CD(34h)
    }

    // ---- (8) 3 rounds x 4 subframes x 3 frames of column trails (loc_31D6D) ----
    for (int round = 0; round < 3 && !should_exit_; ++round) {
        for (int edi = 0; edi < 4 && !should_exit_; ++edi) {
            sub_frame = (sub_frame + 2 > 8) ? 3 : sub_frame + 2;
            const int f = (edi == 3) ? 0 : edi;
            for (int rep = 0; rep < 3 && !should_exit_; ++rep) {
                shake_world();                               // sub_30F98 + 454B3
                for (int k = 0; k < count; ++k) {            // loc_31DD3
                    column_trail(k, sub_frame);              // 30FD1
                    wobble(k);                               // 3108E
                }
                if (f < nframes) {
                    const MagicFxFrame& fr = magic_frames_[(size_t)f];
                    battle_fx_blit(f, anchor_x + fr.xo, anchor_y + fr.yo);
                }
                battle_fx_present();
                wait_game_tick();
            }
        }
        sfx(0x34);
    }

    // ---- (9) 6 rounds x (esi 9..0Bh) x 3 frames of silhouette drops (loc_31EE3) ----
    C.anim_frame = 7;                                        // mov byte [ebp+64h],7
    const int base_x = screen_x(C.x) + 0x0C;                 // var_48
    int base_y = screen_y(C.y) + 0x16;                       // var_20
    int tail_round = 0;
    while (!should_exit_ && tail_round < 6) {
        for (int esi = 9; esi <= 0x0B && !should_exit_; ++esi) {
            sub_frame = (sub_frame + 2 > 8) ? 3 : sub_frame + 2;
            for (int rep = 0; rep < 3 && !should_exit_; ++rep) {
                shake_world();                               // sub_30F98 + 454B3
                for (int k = 0; k < count; ++k) {            // loc_31F26
                    column_trail(k, sub_frame);              // 30FD1
                    wobble(k);                               // 3108E
                }
                if (esi < nframes) {
                    const MagicFxFrame& fr = magic_frames_[(size_t)esi];
                    battle_fx_blit_tinted_unclipped(esi, base_x + fr.xo, base_y + fr.yo, 0x50); // 5F29D
                }
                battle_fx_present();
                wait_game_tick();
            }
        }
        ++tail_round;
        sfx(0x34);
    }

    // ---- (9b) caster rise and the D66A8 RLE silhouette (loc_32019..loc_322A9) ----
    //   [64h]=7, then the caster tile y descends one step at a time (until 10988 < 0)
    //   running 30F98 + target effects + 12F2A(caster). After that, var_38 = 0x12C stepping
    //   down by 1Eh and var_60 += 14h drop the D66A8 record (+0D0h = facing 1, pose 5)
    //   through 5F517 (solid, six rows) and 5F3FD, incrementing var_24.
    C.anim_frame = 7;                                        // mov byte [ebp+64h],7
    const NC2Frame* sil = nc2_frame_for(C.sprite_type, 1, 5);   // D66A8 + 0D0h
    while (!should_exit_) {
        C.y = (int8_t)(C.y - 1);                             // dec [ebp+1]
        const bool off_top = screen_y(C.y) < 0;              // sub_10988 < 0
        shake_world();                                       // sub_30F98 + 454B3
        for (int k = 0; k < count; ++k) {                    // loc_3204D
            column_trail(k, sub_frame);                      // 30FD1
            wobble(k);                                       // 3108E
        }
        battle_fx_commit();
        wait_game_tick();
        sfx(0x34);
        if (off_top) break;
    }
    int sil_dx = 0x12C;                                      // var_38
    int sil_dy = 0;                                          // var_60
    int sil_n = 0;                                           // var_24
    while (!should_exit_ && sil_dx >= -0x64) {
        shake_world();
        for (int k = 0; k < count; ++k) {                    // loc_32130
            column_trail(k, sub_frame);                      // 30FD1
            wobble(k);                                       // 3108E
        }
        if (sil) {
            if (sil_n > 0) {                                 // var_24 > 0
                int rows = (sil_n < 6) ? 6 : sil_n;           // var_3C = max(var_24,6)
                for (int edi = 5; edi >= 0; --edi) {          // edi = 5..0
                    --rows;
                    // loc_321C0: x = var_38 + 3*rows*edi + rec[+4],
                    //            y = var_60 - 2*rows*edi + rec[+5], colour = 2Fh..2Ah.
                    blit_nc2_frame_solid(sil_dx + 3 * rows * edi + sil->x_off,
                                         sil_dy - 2 * rows * edi + sil->y_off,
                                         *sil, (uint8_t)(0x2A + edi)); // sub_5F517
                }
            }
            blit_nc2_frame(sil_dx + sil->x_off,
                           sil_dy + sil->y_off, *sil);           // sub_5F3FD
        }
        battle_fx_commit();
        wait_game_tick();
        sil_dy += 0x14;                                      // var_60 += 14h
        ++sil_n;
        if (sil_n > 0x64) sfx(0x34);
        sil_dx -= 0x1E;                                      // var_38 -= 1Eh
    }
    sfx(0x35);                                               // 4F4CD(35h)

    // ---- (10) di = 0Ch..0Fh, 3 frames each, 5F29D (loc_322CB) ----
    for (int di = 0x0C; di <= 0x0F && !should_exit_; ++di) {
        for (int rep = 0; rep < 3 && !should_exit_; ++rep) {
            if (di == 0x0D) {
                // loc_32347: memset(dword_755CC+0BB80h, 1, 17700h) clears the visible
                // canvas, then loc_32360 still blits entry 0Dh over it. 30F98 and the
                // target effects are skipped for this frame.
                if (uint8_t* bb = video_.get_backbuffer())
                    std::memset(bb, 1, (size_t)SCREEN_W * SCREEN_H);
            } else {
                shake_world();                           // 30F98 + 454B3
                for (int k = 0; k < count; ++k) {        // loc_322F1
                    column_trail(k, sub_frame);          // 30FD1
                    wobble(k);                           // 3108E
                }
            }
            if (di < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)di];
                battle_fx_blit_tinted_unclipped(di, fr.xo, 0x61 + fr.yo, 0x50); // sub_5F29D
            }
            battle_fx_commit();
            wait_game_tick();
        }
    }

    // ---- (11) tail: screen transition, palette round trip, facing restore, damage ----
    //   ASM 46824..46898: 5E9E8+499E4 -> 1FFCD -> 310DE (0xFA00 buffer screen transition) ->
    //   1033B(64h) -> 5FA2C 64h..0 (-4) -> 454B3 + 12F2A for all + 5E9E8+499E4 ->
    //   5FA2C 0..64h (+4) -> 5F9AE -> 44D6E(0) -> BA838 = 0 -> [64h] = 4 -> 11E04 for all ->
    //   restore [63h] -> 12124 for all -> 2CAF7(0Ah, 3E8h) -> 103A0 -> 61115.
    C.y = saved_caster_y;                                    // loc_32394: mov [ebp+1],al (var_18)
    for (int k = 0; k < count; ++k) {                        // loc_323E2
        column_trail(k, sub_frame);                          // 30FD1
        wobble(k);                                           // 3108E
    }
    battle_fx_present();
    wait_game_tick();
    battle_nsp_release();                                    // sub_1FFCD (before 310DE)
    // ---- 310DE: 0xFA00 snapshot scan transition (ASM 45543..45641) ----
    //     - Snapshot: copy the 200x320 display to the 0xFA00 buffer (row stride 320).
    //     - 7 passes: var_4 = 0, 8, 10h, 18h, 20h, 28h, 30h (cmp bx,32h).
    //     - Each pass memsets the canvas at +0BB80h with 17700h bytes (ebx = offset
    //       loc_17700 = 0x17700, exactly 200 rows x 480), then refills it from the snapshot.
    //       var_8 = var_4/8*5 (0, 5, 10, 15, 20, 25, 30) is loop A's starting row.
    //     - Loop A (loc_31182): ebx = var_8..199, count = (200-ebx)*8/5, source and
    //       destination both start at the left edge of row ebx.
    //     - Loop B (loc_311E4): ebx = 0..199, count = ebx*8/5, source and destination both
    //       start count bytes before the right edge (320-count).
    //       A+B split each row, leaving a 1px seam at the boundary from rounding.
    //     - The `[var_C+2]>>16` / `[var_8+2]>>16` terms read the high words of the var_C
    //       pointer and of the word-only var_8 (an original bug: uninitialised stack). They
    //       are effectively 0 in the game, so the port pins them to 0.
    //     - Each pass runs 5E9E8 (output) + 499E4 (wait).
    {
        uint8_t* bb = video_.get_backbuffer();
        if (bb) {
            std::vector<uint8_t> snap((size_t)SCREEN_W * SCREEN_H);
            std::memcpy(snap.data(), bb, snap.size());
            for (int pass = 0; pass < 7 && !should_exit_; ++pass) {
                const int var8 = pass * 5;                       // var_8 = (var_4/8)*5, loop A start row
                std::memset(bb, 0, (size_t)SCREEN_W * SCREEN_H);  // canvas+0BB80h, 17700h bytes
                // Loop A: top to bottom, from the left edge, (200-y)*8/5 bytes
                for (int y = var8; y < SCREEN_H; ++y) {
                    const int count = (SCREEN_H - y) * 8 / 5;
                    std::memcpy(bb + (size_t)y * SCREEN_W,
                                snap.data() + (size_t)y * SCREEN_W, (size_t)count);
                }
                // Loop B: from the right edge of each row, y*8/5 bytes (A+B cover one row)
                for (int y = 0; y < SCREEN_H; ++y) {
                    const int count = y * 8 / 5;
                    const int col = SCREEN_W - count;
                    std::memcpy(bb + (size_t)y * SCREEN_W + (size_t)col,
                                snap.data() + (size_t)y * SCREEN_W + (size_t)col,
                                (size_t)count);
                }
                battle_fx_commit();                              // sub_5E9E8
                wait_game_tick();                                // sub_499E4
            }
        }
    }
    game_timer_set(1);                                       // sub_1033B(1); 64h is the esi palette start
    for (int pct = 0x64; pct >= 0 && !should_exit_; pct -= 4) {   // 5FA2C fade down
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }
    if (!should_exit_) {
        battle_fx_world();                                   // 454B3 + 12F2A for all
        battle_fx_commit();
        wait_game_tick();
    }
    for (int pct = 0; pct <= 0x64 && !should_exit_; pct += 4) {   // 5FA2C fade up
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }
    pal_load_range(0, 0x100, &battle_palette_[0][0]);        // sub_5F9AE master palette
    battle_dim_row0_ = false;                                // sub_44D6E(0)
    cam_fine_y_ = CAM_FINE_Y_REST;                           // dword_BA838 = 0
    C.anim_frame = 4;                                        // mov byte [ebp+64h],4
    C.facing = saved_facing;                                 // mov al,[var_28]; mov [ebp+63h],al
    for (int k = 0; k < count; ++k) {
        if (CombatUnitPlacement* T = target_at(k)) reset_unit_pose(*T);  // sub_12124(target)
    }
    const int r = battle_weapon_2caf7_loop(unit_index, 0x0A, 0x3E8);  // sub_2CAF7(0Ah, 3E8h)
    weather_tick_enabled_ = true;                            // sub_103A0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 31283 unit=%d asura2 frames=%d targets=%d -> %d\n",
                C.id, frame, count, r);
    return 1;                                                // loc_3257C: eax = ebx = 1
}
