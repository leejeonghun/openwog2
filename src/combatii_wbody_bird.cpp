// sub_308EE: bird.nsp (V-weapon F1474 index 0, the "bird" of fire.ssp)
// ASM: dasm/COMBATII_code.ASM 44861..45404. Called from sub_302E6+2B (index 0 of 13).
//
// Original flow:
//   (1) 169D7(unit): reset the AI grid.
//   (2) Target collection.
//       - word_F14D8 == 1 (auto): 2E0F0(unit, 7, &var_1C, &var_24), up to 7 targets;
//         empty returns -1. Distance edi = 49ADF(unit, endpoint) = Manhattan, where the
//         endpoint is the last empty E8 tile before the first collected hostile.
//       - Player: 44D67(1) starts the cursor, 1A1DE(front tile), then the 1A452/1A552
//         rays (width 1, length 7) fill F0648 through 1A1F8/13CBD/13D02; when the tile
//         chosen by the 4A483 cursor satisfies (13CBD >= 0, 49ADF >= 2, 11DB2 == 0)
//         si is incremented for a second pass. Empty returns -1. The port gates on the
//         clicked tile (battle_pick_tx_/ty_) instead.
//       Both paths return -1 when F0B9C+2 (list count) is 0.
//   (3) 1FEA4("bird.nsp") -> 103A8 (stop weather/animation ticks).
//   (4) 11EE4(unit): release the old tile (skipped; the port models occupancy with units_).
//       10971(tile x) + 0Ch -> var_18 (start x), 10988(tile y) - 2 -> var_20 (start y).
//       Facing bh selects direction code var_C: 3 -> 0, 1 -> 7, 0 -> 0x0E, 2 -> 0x15.
//       South (0) and north (2) shift the start y by edi (distance) in pixels, as written.
//   (5) 44D6E(1) dim -> 30824(palette mix, colour 0, pct 23h) -> 1033B(5).
//       var_8 = edi (distance). Every entry of F0648 is registered with 44D89.
//   (6) Wind-up: i = 0..5 x 3 subframes = 18 frames, frame = var_C + i, position fixed at
//       (var_18, var_20). Each subframe runs 454B3 -> 5EE51 -> 5E9E8 -> 499E4.
//   (7) Main loop: t (var_10) = 0 .. distance+4, so distance+5 frames.
//       - Each main frame with t < distance revives one slot (number = t): life = 0x1C,
//         x = var_18, y = var_20 + 0Fh, subframe = 0 (fixed trail position).
//       - 3 subframes: 454B3, draw the 7 slots, draw the projectile. Only the third runs
//         5E9E8 + 499E4, because the original repaints the screen with 454B3 on every
//         subframe, so only the third is visible (the port clears the queue each subframe
//         and presents only the third).
//         Slot draw: frame = life (before increment); subframe++ past 2 wraps to 0 and
//         increments life; life > 0x23 resets it to 0. Position is the revived (x, y).
//         Draw order is 6 -> 0 (descending, nearest trail on top) only for direction 0x15
//         (north); all others use 0 -> 6.
//         Projectile: t < distance -> frame var_C+6, then move 8px (x) / 5px (y) in the
//         facing direction; t >= distance -> frame var_C + 5 - (t - distance) (impact
//         pose), no movement.
//       - After the 3 subframes, t % 5 == 0 plays 4F4CD(32h); then t++ and 499E4.
//   (8) Tail: move the unit tile by direction code (0: x += dist, 7: x -= dist, 0x0E:
//       y += dist, 0x15: y -= dist) -> 11E04 (re-occupy) -> 44D75 (clear highlight) ->
//       every entry of F0648 gets [1Eh] |= 1, [1Fh] = 5Ah -> 2CAF7(unit, mul=2,
//       add=2710h) -> 5F9AE(0, 100h, master) palette restore -> 44D6E(0) ->
//       5E9E8 + 499E4 -> 103A0 -> 1FFCD -> 1.
//
// Port mapping: 1FEA4->battle_nsp_load, 1FFCD->battle_nsp_release, 5EE51->battle_fx_blit
//   (the renderer does not add xo/yo, so this file does), 454B3+12E01+454D5+5E9E8->
//   battle_fx_present, 499E4->wait_game_tick, 1033B->game_timer_set, 103A8/103A0->
//   weather_tick_enabled_, 4F4CD->sfx, 44D6E->battle_dim_row0_, 44D89/44D75->
//   battle_highlight_add/clear, 30824->fx_pal_mix(0x60, 0x100, colour, pct),
//   5F9AE->pal_load_range, 2CAF7->battle_weapon_2caf7_loop, [1Eh]/[1Fh]->
//   battle_weapon_apply_status, 169D7->battle_ai_169d7_reset, 49ADF->Manhattan distance,
//   10971/10988->tile pixel coordinates relative to TILE_ANCHOR.
//
// Deviations:
//   - IDA reads every `[esp+58h+var_N+2]` as the high word of that local. Measured mapping:
//     var_18+2->var_14(t), var_20+2->var_1C(selected tile y), var_28+2->var_24(tile x),
//     var_10+2->var_C(direction code), var_14+2->var_10(main frame t), var_C+2->var_8(distance),
//     [esp+36h]->var_20(start y), [esp+3Eh]->var_18(start x), [esp+4Ah]->var_C.
//     Entries are 12 bytes, so the frame number is scaled by 0Ch before adding unk_F0DAA.
//   - 11EE4/11E04 (release/re-occupy tile) are skipped: the port models occupancy with units_.
//   - The original repaints the screen with 454B3 on every subframe, so sprites from
//     subframes 1 and 2 never reach the final image. The port advances state (slot lifetime,
//     projectile movement) three times but only queues the draw on the third subframe
//     (same image, same counters).
//   - 5EE51's flat pointer is a frame number in the port; magic_frames_ is the same table.
#include "combatii.h"
#include "combatii_internal.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

int CombatII::battle_fx_308ee_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& A = units_[(size_t)unit_index];

    // ---- (2) target collection and distance ----
    // battle_weapon_collect_2e0f0 runs the sub_169D7 E8 rebuild internally.
    int dist = 0;                                                        // edi
    if (battle_f14d8_ == 1) {
        int end_x = (int)(int8_t)A.x, end_y = (int)(int8_t)A.y;
        if (battle_weapon_collect_2e0f0(unit_index, 7, &end_x, &end_y) <= 0) { // sub_2E0F0(unit, 7)
            if (battle_trace_) fprintf(stderr, "COMBAT: 308ee unit=%d empty(auto) -> -1\n", A.id);
            return -1;
        }
        // 308EE uses the selected ray endpoint, not the first target tile.
        dist = std::abs(end_x - (int)(int8_t)A.x) +
               std::abs(end_y - (int)(int8_t)A.y);                      // sub_49ADF
    } else {
        int fx = 0, fy = 0;
        battle_weapon_front_tile(unit_index, fx, fy);                    // sub_1A1DE
        const int n = battle_weapon_collect_rect(unit_index, fx, fy, 1, 7); // 1A452/1A552/1A1F8
        if (n <= 0 || battle_pick_tx_ < 0) {
            if (battle_trace_) fprintf(stderr, "COMBAT: 308ee unit=%d empty(player) -> -1\n", A.id);
            return -1;
        }
        dist = std::abs(battle_pick_tx_ - (int)(int8_t)A.x) +
               std::abs(battle_pick_ty_ - (int)(int8_t)A.y);            // sub_49ADF
    }
    if (battle_target_list_.empty()) {                                   // dword_F0B9C+2 == 0
        if (battle_trace_) fprintf(stderr, "COMBAT: 308ee unit=%d empty list -> -1\n", A.id);
        return -1;
    }

    // ---- (4) start position and direction code ----
    const int facing = (int)A.facing & 3;
    int dir = 0;                                                         // var_C
    int px = (int)(int8_t)A.x, py = (int)(int8_t)A.y;
    int x0 = (px - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0C; // 10971+0Ch
    int y0 = (py - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 2;    // 10988-2
    switch (facing) {
        case 3: dir = 0; break;                       // east: x += distance
        case 1: dir = 7; break;                       // west: x -= distance
        case 0: dir = 0x0E; y0 += dist; break;        // south: y += distance
        case 2: dir = 0x15; y0 -= dist; break;        // north: y -= distance
        default: dir = 0; break;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 308ee unit=%d dist=%d dir=%d start=(%d,%d) targets=%zu\n",
                A.id, dist, dir, x0, y0, battle_target_list_.size());

    const bool loaded = battle_nsp_load("bird.nsp");                     // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    if (loaded && nframes > 0) {
        weather_tick_enabled_ = false;                                   // sub_103A8
        battle_dim_row0_ = true;                                         // sub_44D6E(1)
        fx_pal_mix(0x60, 0x100, 0, 0x23);                                // sub_30824(0, 23h)
        game_timer_set(5);                                               // sub_1033B(5)
        for (int ti : battle_target_list_) battle_highlight_add(ti);     // sub_44D89

        // 5EE51(entry, x, y). The port queue does not add xo/yo.
        auto blit = [&](int frame, int x, int y) {
            if (frame < 0 || frame >= nframes) return;
            const MagicFxFrame& e = magic_frames_[(size_t)frame];
            battle_fx_blit(frame, x + e.xo, y + e.yo);
        };

        // ---- (6) wind-up: 6 frames x 3 subframes at a fixed position ----
        for (int i = 0; i < 6 && !should_exit_; ++i) {
            for (int sub = 0; sub < 3 && !should_exit_; ++sub) {
                blit(dir + i, x0, y0);
                battle_fx_present();                                     // 454B3 + 5EE51 + 5E9E8
                wait_game_tick();                                        // sub_499E4
            }
        }

        // ---- (7) main loop: (distance + 5) frames x 3 subframes ----
        constexpr int kSlots = 7;
        int life[kSlots] = {0, 0, 0, 0, 0, 0, 0};                        // var_38
        int subf[kSlots] = {0, 0, 0, 0, 0, 0, 0};                        // var_30
        int sx[kSlots] = {0, 0, 0, 0, 0, 0, 0};                          // var_58
        int sy[kSlots] = {0, 0, 0, 0, 0, 0, 0};                          // var_48
        const int span = dist + 5;                                       // var_4
        // Advance one slot: frame = life (before increment); every 3 subframes life+1,
        // wrapping to 0 above 0x23.
        auto step_slot = [&](int i, bool visible) {
            const int frame = life[i];
            if (++subf[i] > 2) { subf[i] = 0; ++life[i]; }
            if (life[i] > 0x23) life[i] = 0;
            if (!visible) return;
            blit(frame, sx[i], sy[i]);
        };
        for (int t = 0; t < span && !should_exit_; ++t) {                // var_10
            // The original indexes the 7-slot array with t directly. The player path is
            // limited to the ray length 7, but auto-collection can be farther and the
            // original then overruns into var_30; the port never creates a slot past 7
            // (such a draw would fall outside the frame range anyway).
            if (t < dist && t < kSlots) {                                // revive one slot
                life[t] = 0x1C;
                sx[t] = x0;
                sy[t] = y0 + 0x0F;
                subf[t] = 0;
            }
            for (int sub = 0; sub < 3 && !should_exit_; ++sub) {
                const bool visible = (sub == 2);                         // only the 3rd reaches the screen
                magic_fx_draws_.clear();
                if (dir == 0x15) {                                       // north: 6 -> 0
                    for (int i = kSlots - 1; i >= 0; --i)
                        if (life[i] > 0) step_slot(i, visible);
                } else {
                    for (int i = 0; i < kSlots; ++i)
                        if (life[i] > 0) step_slot(i, visible);
                }
                if (t < dist) {                                          // projectile = var_C+6, moving
                    if (visible) blit(dir + 6, x0, y0);
                    switch (dir) {
                        case 7: x0 -= 8; break;                          // west
                        case 0: x0 += 8; break;                          // east
                        case 0x0E: y0 += 5; break;                       // south
                        case 0x15: y0 -= 5; break;                       // north
                        default: break;
                    }
                } else if (visible) {                                    // impact pose
                    blit(dir + 5 - (t - dist), x0, y0);
                }
                if (visible) {
                    if (t % 5 == 0) sfx(0x32);                           // 4F4CD(32h)
                    battle_fx_present();                                 // 5E9E8 (includes 454B3)
                }
            }
            wait_game_tick();                                            // sub_499E4
        }
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: warning: 308ee unit=%d bird.nsp missing\n", A.id);
    }

    // ---- (8) tail ----
    switch (dir) {                                                       // move the unit tile
        case 7: px -= dist; break;
        case 0: px += dist; break;
        case 0x0E: py += dist; break;
        case 0x15: py -= dist; break;
        default: break;
    }
    A.x = (decltype(A.x))px;
    A.y = (decltype(A.y))py;
    // 11E04 (re-occupy) is skipped: units_ coordinates define occupancy.
    battle_highlight_clear();                                            // sub_44D75
    for (int ti : battle_target_list_) battle_weapon_apply_status(ti, 0x01, 0x5A); // [1Eh]|=1, [1Fh]=5Ah
    battle_weapon_2caf7_loop(unit_index, 2, 0x2710);                     // sub_2CAF7(mul=2, add=2710h)
    pal_load_range(0, 256, &battle_palette_[0][0]);                      // sub_5F9AE (master restore)
    battle_dim_row0_ = false;                                            // sub_44D6E(0)
    battle_fx_present();                                                 // 5E9E8 (includes 454B3)
    wait_game_tick();                                                    // sub_499E4
    weather_tick_enabled_ = true;                                        // sub_103A0
    battle_nsp_release();                                                // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 308EE unit=%d dist=%d dir=%d frames=%d\n", A.id, dist, dir, dist + 5);
    return 1;
}
