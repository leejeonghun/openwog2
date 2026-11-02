// sub_356DB: sarah.nsp (V-weapon edi 6, single arrow snipe)
// ASM: dasm/COMBATII_code.ASM 50610..51300. Called from sub_302E6 with edi=6
//   (after f-arrow.ssp 32h and growl.ssp 33h, with eax = unit).
//
// Original flow:
//   1. Target
//      - word_F14D8 == 1 (auto): sub_2EA0C(unit, 7) takes the nearest unit on the
//        straight ray, and dword_F0648[0] [0]/[1] becomes the target tile.
//      - Otherwise (player): sub_13F8A -> 1A452 (width 1, length 7) / 1A552 sweep the
//        ray, keep cells whose 1093D (tile attribute) != 0, mark candidates through
//        13D02 and pick one with the 4A483 cursor; it enters F0648 only if it passes
//        1A1F8 (occupant) and 13CBD (E8 reach). A cursor cancel (-1) returns -1.
//   2. Per-facing start offsets and step (local tables at COMBATII.LST 34C1C/34C24/34C30):
//        xoff  = {8, 5, -5, 40}     (var_3C, facing 0..3)
//        yoff  = {32, -5, -30, -5}  (var_34)
//        frame = {20, 0, 30, 10}    (var_44: sarah.nsp frame block start)
//        pose  = {7, 5, 8, 9}       (var_2C: final four poses)
//      facing 1 (west):  step=(-8, 0), x rand 0,   y rand 0Bh,
//                        count = (10971(unit.x) + xoff[1] - 10971(tx)) / 8
//      facing 3 (east):  step=(+8, 0),             y rand 0Bh,
//                        count = (10971(tx) - 10971(unit.x) - xoff[3]) / 8
//      facing 2 (north): step=(0, -8), x rand 0Dh,
//                        count = (10988(unit.y) + yoff[2] - 10988(ty)) / 8
//      facing 0 (south): step=(0, +8), x rand 0Dh,
//                        count = (10988(ty) - 10988(unit.y) - yoff[0]) / 8
//      count += 4 (particle life = frames travelled); the division truncates toward zero,
//      as `sar 3` does.
//   3. Setup: zero the 10 particle slots -> 1FEA4 (sarah.nsp) -> 103A8 (weather freeze) ->
//      save facing (var_18) -> 11EE4 (release the old cell) -> 2CBA1 (register depth) ->
//      1033B(5) -> 44D6E(1) (map dim) -> 200DE(unit, 0) (camera to the caster tile) ->
//      4x { 30824(0, 0Ah); inc ebx; 499E4 } (mix 0x60..0xFF of the palette 10% toward 0).
//   4. Nine passes:
//      - poses 5 and 6 for 3 frames each: 454B3 -> 12F2A (caster) -> 35594 (particles) ->
//        5E9E8 -> 499E4.
//      - 4F4CD(32h) then respawn exactly one empty slot:
//          x = 10971(unit.x) + xoff[f] + 499BD(var_10),
//          y = 10988(unit.y) + yoff[f] + 499BD(var_14), counter = count.
//      - 3 more frames in the current pose.
//   5. Until no slot is empty: toggle dword_BA838 between 5 and -5 and run one frame
//      (particles only).
//   6. 4F4CD(15h) -> poses 7, 5, 8, 9 for 3 frames each (no particles, wobble 5/-5).
//   7. Snipe trail: anchor = 10971(unit.x)+xoff[f], 10988(unit.y)+yoff[f]; base frame =
//      frame[f]. 1033B(0Fh), dword_BA838 = 0.
//      - cx = 3..6: anchor += step, blit frame+cx, 12F2A, 5E9E8, 499E4.
//      - 4F4CD(33h), count -= 3, 1033B(5), cx = 0..count-1: anchor += step, 12F2A,
//        blit frame+7, 5E9E8, 499E4.
//      - 2 frames: toggle dword_BA838 between 2 and -2, anchor += step, blit frame+8+cx.
//   8. Tail: dword_BA838=0 -> 44D6E(0) -> 5F9AE(0,100h, master) -> 44D75 -> 11E04
//      (re-register occupancy) -> restore facing -> 12124 (caster) -> 12124 (target) ->
//      damage (1D770 -> x5 + 0BB8 -> 2CACA -> 45BFB damage number -> [48h] -= fin ->
//      45ED2 knockback) -> 287AF (hit pace) -> 1ED45 -> 1FFCD -> 103A0 -> 1.
//
// Port mapping: 1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release,
//   5EE51 -> fx_blit_nsp, 454B3 -> battle_fx_world, 454D5 -> battle_fx_overlay,
//   5E9E8 -> battle_fx_commit, 12F2A -> battle_draw_unit_now,
//   2CBA1 -> battle_highlight_add_depth, 44D75 -> battle_highlight_clear,
//   44D6E -> battle_dim_row0_, 30824 -> fx_pal_mix(0x60,0x100,0,..),
//   1033B -> game_timer_set, 499E4 -> wait_game_tick, 499BD -> combat_rand,
//   4F4CD -> sfx, 10971/10988 -> TILE_ANCHOR, 200DE(unit,0) -> camera_center_on(caster
//   tile), 103A8/103A0 -> weather_tick_enabled_, 1D770/2CACA/45BFB/45ED2/1ED45 ->
//   battle_damage / battle_weapon_2caca / play_hp_change / run_unit_hp_trigger,
//   287AF -> game_timer_set(6) (same hit-pace approximation as the other weapon bodies).
//
// Approximations:
//   - sub_35594's three frame branches (counter > 4 -> frame, > 2 -> frame+1, else
//     frame+2) and slot advance (x += var_C, y += var_8, counter--) are carried over.
//     Six stacked push arguments make the register labels ambiguous (var_10/var_14 read
//     as both rand range and step), so the step comes from the var_C/var_8 values in
//     step 2, matching the observed per-frame flow along the facing.
//   - sub_3555A always takes the target [65h]=0 path because var_4 is never incremented;
//     carried over as is.
//   - 11EE4/11E04 (release and re-register occupancy) are omitted because the port
//     models occupancy with units_.
//   - 287AF is a 24-frame hit pace; replaced by game_timer_set(6) as in the other bodies
//     (damage and triggers are unchanged).
//   - Damage is applied after the presentation, as in the original.
#include "combatii.h"
#include "combatii_internal.h"

int CombatII::battle_fx_356db_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& A = units_[(size_t)unit_index];

    // ---- 1. target selection (battle_weapon_356db gameplay kept as is) ----
    int n = 0;
    if (battle_f14d8_ == 1) {
        n = battle_weapon_collect_nearest(unit_index);            // sub_2EA0C(unit, 7)
    } else {
        battle_target_list_.clear();                              // sub_13F8A
        const int oi = battle_weapon_pick_target(unit_index, true); // 1A452/1A552/4A483
        if (oi >= 0) {
            battle_target_list_.push_back(oi);                    // 1A1F8
            n = 1;
        }
    }
    if (n <= 0 || battle_target_list_.empty()) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 356db unit=%d no target -> -1\n", A.id);
        return -1;
    }
    const int ti = battle_target_list_[0];
    if (ti < 0 || (size_t)ti >= units_.size() || !units_[(size_t)ti].active) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 356db unit=%d target gone -> -1\n", A.id);
        return -1;
    }
    const int tx = (int)(int8_t)units_[(size_t)ti].x;
    const int ty = (int)(int8_t)units_[(size_t)ti].y;
    const int f = (int)A.facing & 3;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 356db unit=%d -> target=%d facing=%d\n",
                A.id, units_[(size_t)ti].id, f);

    // ---- 2. per-facing tables (COMBATII.LST 34C1C / 34C24 / 34C2C / 34C30) ----
    static const int kXOff[4] = {8, 5, -5, 40};              // var_3C
    static const int kYOff[4] = {32, -5, -30, -5};           // var_34
    static const int kFrameBase[4] = {20, 0, 30, 10};        // var_44
    static const uint8_t kPose[4] = {7, 5, 8, 9};            // var_2C
    // sub_10971 / sub_10988
    auto sx_of = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto sy_of = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    const int ux = (int)(int8_t)A.x;
    const int uy = (int)(int8_t)A.y;
    int step_x = 0, step_y = 0;      // var_C / var_8
    int rand_x = 0, rand_y = 0;      // var_10 / var_14
    int count = 0;                   // var_28
    switch (f) {
        case 1:                                          // west
            step_x = -8; rand_y = 0x0B;
            count = (sx_of(ux) + kXOff[1] - sx_of(tx)) / 8;
            break;
        case 3:                                          // east
            step_x = 8; rand_y = 0x0B;
            count = (sx_of(tx) - sx_of(ux) - kXOff[3]) / 8;
            break;
        case 2:                                          // north
            step_y = -8; rand_x = 0x0D;
            count = (sy_of(uy) + kYOff[2] - sy_of(ty)) / 8;
            break;
        default:                                         // south
            step_y = 8; rand_x = 0x0D;
            count = (sy_of(ty) - sy_of(uy) - kYOff[0]) / 8;
            break;
    }
    count += 4;

    // ---- 10 particle slots (var_50 counter / var_78 x / var_64 y) ----
    int px[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int py[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int pcnt[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    // ---- 3. setup ----
    const bool loaded = battle_nsp_load("sarah.nsp");        // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    if (loaded && nframes > 0) {
        weather_tick_enabled_ = false;                       // sub_103A8
        const int saved_facing = (int)A.facing;              // var_18
        battle_highlight_add_depth(unit_index);              // sub_2CBA1
        game_timer_set(5);                                   // sub_1033B(5)
        battle_dim_row0_ = true;                             // sub_44D6E(1)
        camera_center_on(ux, uy);                            // sub_200DE(unit, 0)
        for (int i = 0; i < 4 && !should_exit_; ++i) {       // `cmp bx,4; jl`
            fx_pal_mix(0x60, 0x100, 0, 0x0A);                // sub_30824(0, 0Ah)
            wait_game_tick();                                // sub_499E4
        }

        auto blit = [&](int frame, int x, int y) {           // sub_5EE51
            if (frame < 0 || frame >= (int)magic_frames_.size()) return;
            const MagicFxFrame& e = magic_frames_[(size_t)frame];
            fx_blit_nsp(frame, x + e.xo, y + e.yo);
        };
        // sub_35594: counter > 4 -> frame, > 2 -> frame+1, else frame+2; then counter--.
        auto draw_particles = [&]() {
            for (int i = 0; i < 10; ++i) {
                if (pcnt[i] <= 0) continue;
                const int fb = kFrameBase[f] + (pcnt[i] > 4 ? 0 : (pcnt[i] > 2 ? 1 : 2));
                blit(fb, px[i], py[i]);
                px[i] += step_x;
                py[i] += step_y;
                --pcnt[i];
            }
        };
        // sub_3555A: target [65h] wobble. var_4 is always 0, so only the store-0 path runs.
        auto wobble_target = [&]() {
            if (ti >= 0 && (size_t)ti < units_.size()) units_[(size_t)ti].walk_off_x = 0;
        };
        // 454B3 -> 12F2A (caster) -> particles -> 5E9E8 -> 499E4
        auto unit_frame = [&](int pose) {
            A.anim_frame = (uint8_t)pose;
            battle_fx_world();                               // sub_454B3
            battle_fx_overlay();                             // sub_454D5
            battle_draw_unit_now(unit_index);                // sub_12F2A
            draw_particles();                                // sub_35594
            battle_fx_commit();                              // sub_5E9E8
            wait_game_tick();                                // sub_499E4
        };

        // ---- 4. nine passes ----
        for (int pass = 0; pass < 9 && !should_exit_; ++pass) {   // `cmp di,9; jl`
            for (int pose = 5; pose <= 6 && !should_exit_; ++pose) {
                for (int k = 0; k < 3 && !should_exit_; ++k) {     // `cmp si,3; jl`
                    wobble_target();                            // sub_3555A
                    unit_frame(pose);
                }
            }
            sfx(0x32);                                          // sub_4F4CD(32h)
            // Respawn exactly one empty slot, then leave (loc_35B23 -> loc_35B30).
            for (int i = 0; i < 10 && !should_exit_; ++i) {
                if (pcnt[i] != 0) continue;
                px[i] = sx_of(ux) + kXOff[f] + combat_rand(rand_x);
                py[i] = sy_of(uy) + kYOff[f] + combat_rand(rand_y);
                pcnt[i] = count;
                break;
            }
            for (int k = 0; k < 3 && !should_exit_; ++k) {
                wobble_target();
                unit_frame(6);
            }
        }

        // ---- 5. drain the remaining particles (loc_35BAE) ----
        for (int guard = 0; guard < 0x4000 && !should_exit_; ++guard) {
            cam_fine_y_ = (cam_fine_y_ == 5) ? (int16_t)-5 : (int16_t)5; // toggle dword_BA838
            int i = 0;
            for (; i < 10; ++i) if (pcnt[i] == 0) break;
            if (i == 10) break;                                  // `cmp ax,0Ah; jz`
            wobble_target();
            unit_frame((int)A.anim_frame);
        }
        if (should_exit_) {
            pal_load_range(0, 256, &battle_palette_[0][0]);
            battle_dim_row0_ = false;
            battle_highlight_clear();
            battle_nsp_release();
            weather_tick_enabled_ = true;
            return 1;
        }

        // ---- 6. final four poses (no particles) ----
        sfx(0x15);                                               // sub_4F4CD(15h)
        for (int c = 0; c < 4 && !should_exit_; ++c) {           // `cmp cx,4; jl`
            A.anim_frame = kPose[c];
            for (int k = 0; k < 3 && !should_exit_; ++k) {
                cam_fine_y_ = (cam_fine_y_ == 5) ? (int16_t)-5 : (int16_t)5;
                battle_fx_world();
                battle_fx_overlay();
                if (c == 0) wobble_target();                     // `test cx,cx; jnz` -> sub_3555A
                battle_draw_unit_now(unit_index);
                battle_fx_commit();
                wait_game_tick();
            }
        }

        // ---- 7. snipe trail ----
        int ax = sx_of(ux) + kXOff[f];
        int ay = sy_of(uy) + kYOff[f];
        const int fi = kFrameBase[f];
        game_timer_set(0x0F);                                    // sub_1033B(0Fh)
        cam_fine_y_ = 0;
        for (int c = 3; c <= 6 && !should_exit_; ++c) {          // `cmp cx,6; jle`
            ax += step_x;
            ay += step_y;
            battle_fx_world();                                   // sub_454B3
            battle_fx_overlay();
            blit(fi + c, ax, ay);                                // sub_5EE51, then 12F2A
            battle_draw_unit_now(unit_index);
            battle_fx_commit();
            wait_game_tick();
        }
        sfx(0x33);                                               // sub_4F4CD(33h)
        count -= 3;                                              // `sub ebx,3`
        game_timer_set(5);                                       // sub_1033B(5)
        for (int c = 0; c < count && !should_exit_; ++c) {       // `cmp cx,var_28; jl`
            ax += step_x;
            ay += step_y;
            battle_fx_world();
            battle_fx_overlay();
            battle_draw_unit_now(unit_index);
            blit(fi + 7, ax, ay);
            battle_fx_commit();
            wait_game_tick();
        }
        for (int c = 0; c < 2 && !should_exit_; ++c) {           // `cmp cx,2; jl`
            cam_fine_y_ = (cam_fine_y_ == 2) ? (int16_t)-2 : (int16_t)2;
            ax += step_x;
            ay += step_y;
            battle_fx_world();
            battle_fx_overlay();
            battle_draw_unit_now(unit_index);
            blit(fi + 8 + c, ax, ay);
            battle_fx_commit();
            wait_game_tick();
        }

        // ---- 8. tail ----
        cam_fine_y_ = 0;                                         // dword_BA838 = 0
        battle_dim_row0_ = false;                                // sub_44D6E(0)
        pal_load_range(0, 256, &battle_palette_[0][0]);          // sub_5F9AE(0,100h, master)
        battle_highlight_clear();                                // sub_44D75
        A.facing = (int8_t)saved_facing;                         // `mov al,[var_18]; mov [63h],al`
        reset_unit_pose(A);                                      // sub_12124 (caster)
        if (ti >= 0 && (size_t)ti < units_.size()) reset_unit_pose(units_[(size_t)ti]);
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: warning: 356DB unit=%d sarah.nsp missing (presentation skipped)\n", A.id);
    }

    // ---- damage (tail of step 8: 1D770 -> x5 + 0BB8 -> 2CACA -> 45BFB/45ED2/287AF/1ED45) ----
    if (should_exit_) return 1;
    if (ti < 0 || (size_t)ti >= units_.size() || !units_[(size_t)ti].active) return -1;
    const int base = battle_damage(unit_index, ti, 0, 0, 0);      // sub_1D770(ebx=0, ecx=0)
    const int tmp = 5 * base + 0x0BB8;
    const int fin = battle_weapon_2caca(unit_index, tmp);         // sub_2CACA
    {
        auto& T = units_[(size_t)ti];
        const int old_raw = battle_hp_raw(T);
        int new_raw = old_raw - fin;
        if (new_raw > battle_max_hp_raw(T)) new_raw = battle_max_hp_raw(T);
        const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
        const int delta = new_hp - T.hp;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 356db unit=%d -> target=%d base=%d tmp=%d final=%d hp_raw %d->%d\n",
                    A.id, T.id, base, tmp, fin, old_raw, new_raw);
        if (delta != 0) {
            VmEvent hp; hp.type = VmEventType::Hp; hp.hp_unit = ti;
            hp.hp_delta = delta; hp.hp_dir = A.facing & 3; hp.hp_killer = unit_index;
            play_hp_change(hp);                                   // sub_45BFB
            if (should_exit_) return 1;
        } else {
            run_unit_hp_trigger(ti, unit_index, 1);
        }
    }
    run_unit_hp_trigger(unit_index, -1, 1);                      // sub_1ED45
    battle_nsp_release();                                        // sub_1FFCD (after the damage)
    weather_tick_enabled_ = true;                                // sub_103A0
    reset_unit_pose(units_[(size_t)unit_index]);                 // sub_12124
    game_timer_set(6);                                           // 287AF hit-pace approximation
    battle_highlight_clear();                                    // sub_44D75 (51257)
    return 1;
}
