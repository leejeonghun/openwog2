// combatii_battle: battle rules (to-hit/damage), ATB scheduler, hit and death effects.
#include "combatii.h"
#include "audio.h"
#include "devutil.h"
#include "combatii_internal.h"
#include "battle_tables.h"
#include "combat_class_names.h"
#include "combatii_item_full_data.h"
#include "combat_form_tables.h"
#include "combatii_progression_tables.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// sub_4C72A tail: read unk_74FA8 after the sub_4BFCA id normalization.
// Returns the 1-based spell id, or 0 for an empty/non-base slot.
static int battle_magic_ring_value(int g, int b, int c) {
    if (g < 0 || g > 6 || b < 0 || b > 7 || c < 0 || c > 7) return 0;
    const int v = (int)kMagicRingMap[g][b][c];
    return (v != 0 && (v - 1) % 3 == 0) ? v : 0;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- VM command timeline playback ----
// The original moves the screen inside the sub_52E55 handlers and waits in
// sub_50339 (camera interpolation), sub_516D0 (move effect) and sub_482A5 (dialogue box).
// The port replays recorded events in order to reproduce the same sequence.

// ==== Event battle effects (0x07 sub_51C74 -> sub_1E61D / 0x0A sub_5211D / death sub_1DB1D) ====

// sub_44AB6: first 0x4C frames of battle.syb (projectiles) / last 0x39 frames (explosions).
// sub_11BB8: wea_effc.syb (0x78 frames) / chr_effc.syb (0x24 frames).
// sub_4F25F builds 12-byte records {w,h,xo,yo,ptr}; the battle.syb bank is stride-corrected
// by sub_10031 for the 320px rows and then drawn by sub_5F3FD.
void CombatII::load_effect_sprites() {
    if (effect_sprites_loaded_) return;
    effect_sprites_loaded_ = true;
    auto load_one = [&](const char* name, int count, std::vector<TerrainOverlayFrame>& out) {
        out.clear();
        std::string p = game_path(name);
        if (!file_exists(p.c_str())) {
            std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", name);
            if (!alt.empty()) p = alt;
        }
        if (!file_exists(p.c_str())) {
            fprintf(stderr, "COMBAT: warning: %s not found - battle effect skipped\n", name);
            return;
        }
        SPRParser spr;
        if (!spr.load(p, false)) {
            fprintf(stderr, "COMBAT: warning: %s parse failed: %s\n", name, spr.error().c_str());
            return;
        }
        const int n = std::min<int>((int)spr.count(), count);
        out.resize((size_t)count);
        for (int i = 0; i < n; i++) {
            const SpriteEntry* se = spr.get(i);
            if (!se || !se->pixel_data || se->data_size <= 0) continue;
            TerrainOverlayFrame& fr = out[(size_t)i];
            fr.w = se->word0; fr.h = se->word2; fr.x = se->x_offset; fr.y = se->y_offset;
            fr.rle.assign(se->pixel_data, se->pixel_data + se->data_size);
        }
        fprintf(stderr, "COMBAT: %s loaded %d/%d frames\n", name, n, count);
    };
    // sub_4F25F is called twice (ebx=0 for 0x4C frames, ebx=0x4C for 0x39): bank 0 holds
    // 0x4C frames, bank 1 0x39 - 0x85 in total. unk_FA400 maps to battle_effects_[0x4C+i].
    load_one("BATTLE.SYB", 0x85, battle_effects_);
    load_one("WEA_EFFC.SYB", 0x78, wea_effc_);
    load_one("CHR_EFFC.SYB", 0x24, chr_effc_);

    // sub_44AB6 calls sub_10031 for each of the first 0x4C BATTLE.SYB entries.
    // This converts the RLE cursor from 320px rows to the 480px scratch-canvas stride:
    // at each 320px boundary, 160 is added to that run's skip.
    // sub_11BB8 (2717-2743) also applies sub_10031 to all 0x78 WEA_EFFC and 0x24 CHR_EFFC
    // frames: the trail (sub_1E2EA -> 5F3FD) is drawn on the 480-wide scratch canvas.
    auto convert_stride = [&](std::vector<TerrainOverlayFrame>& bank) {
        for (TerrainOverlayFrame& fr : bank)
            spr_convert_stride_480(fr.rle.data(), fr.rle.size());
    };
    convert_stride(battle_effects_);
    convert_stride(wea_effc_);
    convert_stride(chr_effc_);
}

// ---- loc_1E8E5 per-class hit effect (sub_47372/47712/4798A/47A96/47E79/47F3C/47FDC) ----
// sub_1E61D dispatches on wc/class through the table at loc_1E8CD..loc_1E984 and calls one of
// seven routines. All of them draw 12-byte {w,h,xo,yo,ptr} records from BATTLE.SYB via
// sub_5F3FD. unk_FA070 holds the first 0x4C frames and unk_FA400 the last 0x39, so the port
// keeps that second bank at battle_effects_[0x4C+i]. Frame index = (VA - 0xFA070)/12, e.g.
// unk_FA160 = 20, unk_FA190 = 24, unk_FA1C0 = 28, unk_FA070+2Ch = 0x2C, unk_FA400 = bank 0x4C.
// Deviation: the original's multi-stage trail coordinates are approximated with frame counts
// and anchors only.
// sub_5F3FD: draw frame (bank, idx) at (x, y), adding the record's [+4]/[+6] offsets.
void CombatII::fx_blit_battle_spr(int bank, int idx, int x, int y) {
    const int fi = (bank == 0) ? idx : 0x4C + idx;
    if (fi < 0 || (size_t)fi >= battle_effects_.size()) return;
    const TerrainOverlayFrame& fr = battle_effects_[(size_t)fi];
    if (fr.rle.empty()) return;
    blit_sprite_480(x + fr.x, y + fr.y, fr.rle.data());
}

// sub_47372 (wc 0Bh: i-piece/i-shoot/shoot family): stamps unk_FA160[dir] and unk_FA190[dir]
//   along the caster's facing at distance/3 steps, then draws the last 6 frames in place.
void CombatII::battle_fx_47372(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    const int dir = (int)A.facing & 3;
    static constexpr int kDx[4] = { 0, -1, 0, 1 };
    static constexpr int kDy[4] = { 1, 0, -1, 0 };
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    int steps = 6;
    if (!battle_target_list_.empty()) {                            // distance/3 approximation
        const auto& T = units_[(size_t)battle_target_list_[0]];
        steps = std::max(2, std::min(0x14, (std::abs((int)T.x - (int)A.x) +
                                            std::abs((int)T.y - (int)A.y)) ));
    }
    A.anim_frame = 5;                                              // loc_47521: [64h]=5
    game_timer_set(0x1E);                                          // sub_1033B(1Eh)
    for (int i = 0; i < 2 && !should_exit_; ++i) {                  // 2 frames (5 -> 6)
        battle_fx_world();
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
        A.anim_frame = (uint8_t)(A.anim_frame + 1);
    }
    for (int i = 0; i < steps && !should_exit_; ++i) {               // body (16px steps)
        battle_fx_world();
        const int px = ax + kDx[dir] * 16 * i;
        const int py = ay + kDy[dir] * 16 * i;
        fx_blit_battle_spr(0, 20 + dir, px, py);                    // unk_FA160 + dir
        if ((i % 3) == 2) fx_blit_battle_spr(0, 24 + dir, px, py);  // unk_FA190 + dir
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
    for (int i = 0; i < 6 && !should_exit_; ++i) {                   // final 6 frames
        battle_fx_world();
        const int px = ax + kDx[dir] * 16 * steps;
        const int py = ay + kDy[dir] * 16 * steps;
        fx_blit_battle_spr(1, 0x0C + i, px, py);                    // unk_FA400+0Ch+i
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
    A.anim_frame = 0;                                              // mov byte [esi+64h],0
    present_cutscene_frame();
}

// sub_47712 (class 0A1): uses the same two tables as 47372, but the anchors are 10971(x)+0Ch
//   and 10988(y)+8, stamps at distance/8 steps and finishes with 6 frames.
void CombatII::battle_fx_47712(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    const int dir = (int)A.facing & 3;
    static constexpr int kDx[4] = { 0, -1, 0, 1 };
    static constexpr int kDy[4] = { 1, 0, -1, 0 };
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    int steps = 6;
    if (!battle_target_list_.empty()) {
        const auto& T = units_[(size_t)battle_target_list_[0]];
        steps = std::max(2, std::min(0x14, (std::abs((int)T.x - (int)A.x) +
                                            std::abs((int)T.y - (int)A.y)) ));
    }
    A.anim_frame = 5;
    game_timer_set(0x1E);
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        battle_fx_world();
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
        A.anim_frame = (uint8_t)(A.anim_frame + 1);
    }
    for (int i = 0; i < steps && !should_exit_; ++i) {               // 8px steps
        battle_fx_world();
        const int px = ax + kDx[dir] * 8 * i;
        const int py = ay + kDy[dir] * 8 * i;
        fx_blit_battle_spr(0, 20 + dir, px, py);
        if ((i % 3) == 2) fx_blit_battle_spr(0, 24 + dir, px, py);
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
    for (int i = 0; i < 6 && !should_exit_; ++i) {
        battle_fx_world();
        fx_blit_battle_spr(1, 0x0C + i, ax, ay);
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
    A.anim_frame = 0;
    present_cutscene_frame();
}

// sub_4798A(eax=unit, edx=mode): 0 = unk_FA070 + 0x2C+facing*8 (8 frames),
//   1 = unk_FA400 + 0x1D+facing*7 (7 frames). Stamps each frame at a fixed anchor and redraws the unit.
void CombatII::battle_fx_4798a(int unit_index, int mode) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    const int dir = (int)A.facing & 3;
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    game_timer_set(0x0C);                                          // sub_1033B(0Ch)
    const int base = (mode == 0) ? (0x2C + dir * 8) : (0x1D + dir * 7);
    const int count = (mode == 0) ? 8 : 7;
    for (int i = 0; i < count && !should_exit_; ++i) {
        battle_fx_world();
        fx_blit_battle_spr(mode == 0 ? 0 : 1, base + i, ax, ay);
        battle_draw_unit_now(unit_index);                           // sub_12F2A
        battle_fx_commit();
        wait_game_tick();
    }
}

// sub_47A96 (wc 0Ch): stamps the unk_FA1C0 + facing frame twice into each of the 4 directions.
//   The original loops var_C (frame count) + 0Ch times and accumulates a coordinate per slot;
//   the port keeps 0x0C iterations with the per-facing frame (28+facing) only.
void CombatII::battle_fx_47a96(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    const int dir = (int)A.facing & 3;
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    A.anim_frame = 5;                                              // mov byte [edi+64h],5
    game_timer_set(0x1E);
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        battle_fx_world();
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
        A.anim_frame = (uint8_t)(A.anim_frame + 1);
    }
    for (int i = 0; i < 0x0C && !should_exit_; ++i) {
        battle_fx_world();
        fx_blit_battle_spr(0, 28 + dir, ax, ay);                    // unk_FA1C0 + facing
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
}

// sub_47E79 (wc 10h): stamps the six unk_FA400 frames 0Ch..11h over every target.
void CombatII::battle_fx_47e79(int unit_index) {
    (void)unit_index;
    game_timer_set(0x10);                                          // sub_1033B(10h)
    for (int i = 0; i < 6 && !should_exit_; ++i) {
        battle_fx_world();
        for (int ti : battle_target_list_) {
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            const auto& T = units_[(size_t)ti];
            const int tx = (T.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
            const int ty = (T.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
            fx_blit_battle_spr(1, 0x0C + i, tx, ty);
        }
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
}

// sub_47F3C (wc 13h): alternates frames 0x28 (facing < 2) / 0x2A at the sub_135EB centre, 9 times.
void CombatII::battle_fx_47f3c(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    int cx = 0, cy = 0;
    unit_screen_center(A, cx, cy);                                  // sub_135EB
    const int base = ((int)A.facing < 2) ? 0x28 : 0x2A;
    game_timer_set(3);
    for (int i = 0; i < 9 && !should_exit_; ++i) {
        battle_fx_world();
        fx_blit_battle_spr(0, base + (i & 1), cx, cy);
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
        present_cutscene_frame();                                   // sub_45559
        wait_game_tick();
    }
}

// sub_47FDC (class 5Ch): after 200DE(4), stamps the 32 unk_FA400 frames 15h..34h at a fixed anchor.
void CombatII::battle_fx_47fdc(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    game_timer_set(0x0C);
    for (int i = 0; i < 0x20 && !should_exit_; ++i) {
        battle_fx_world();
        fx_blit_battle_spr(1, 0x15 + i, ax, ay);                     // unk_FA400+15h+i
        battle_fx_overlay();
        battle_fx_commit();
        wait_game_tick();
    }
}

// x86emu probe_46e27_var8.py (run_2ef4a_call_capture): at the 42854 call the
// uninitialized word at [esp+28Ch-8] is 0. The original's cmp var_8,8 therefore
// takes loc_47059 and selects group 9 for 0x91/0x94..0x98. The event path uses the
// same zeroed caller slot.
constexpr int kSub46E27StackVar8 = 0;

// sub_46E27: interpolates a small BATTLE.SYB projectile from attacker towards the tile in
// dword_FA8B0+2 / word_FA8B4. The 12-byte frame table at unk_FA070 is four direction
// records (BATTLE.SYB frames 0..3); var_8 (the class group) only selects the pose reset,
// the direction override, and the 1/6-tick pacing.
void CombatII::play_weapon_class5_projectile_at(CombatUnitPlacement& attacker,
                                                int tx, int ty, bool reaches_target,
                                                int stack_var8) {
    const bool linked = attacker.code_b >= 1 && attacker.code_b <= 4 && attacker.linked_class >= 0;
    const int effect_class = linked ? attacker.linked_class : attacker.combat_class;
    // sub_46F15..46FE5: frame_group (var_8) selects pose/timer and, for 8/9, the
    // direction override. The 12-byte frame table at unk_FA070 is only the four
    // direction records (BATTLE.SYB frames 0..3) for every class; var_C's high word
    // is the low word of edx (0/1) and therefore never offsets the table base.
    int frame_group = -1;
    bool override_direction = false;
    if (effect_class >= 0x15 && effect_class <= 0x18) {
        frame_group = effect_class - 0x15;
    } else if (effect_class >= 0x35 && effect_class <= 0x38) {
        frame_group = 4;
    } else if (effect_class == 0x8C || effect_class == 0x8D ||
               effect_class == 0x92 || effect_class == 0x93) {
        frame_group = 8;
        override_direction = true;
    } else if (effect_class == 0x90) {
        frame_group = 9;
    } else if (effect_class == 0x91 ||
               (effect_class >= 0x94 && effect_class <= 0x98)) {
        // 0x91/0x94..0x98 enter at loc_46FE5 without a group store; the original
        // tests the inherited var_8 slot against 8. The x86emu probe pins the
        // sub_2EF4A caller slot to 0, so loc_47059 selects group 9.
        frame_group = (stack_var8 == 8) ? 8 : 9;
        override_direction = true;
    } else {
        return;
    }

    // sub_46E27 starts at sub_10971(A.x)+0x0C and uses sub_10988(A.y)-2.
    const int source_x = (attacker.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ +
                         TILE_ANCHOR_X + CENTER_ADD_X;
    const int source_y = (attacker.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ +
                         TILE_ANCHOR_Y - 2;
    int target_x = 0, target_y = 0;
    const int target_oi = battle_unit_index_at(tx, ty); // sub_11DB2
    if (target_oi >= 0 && (size_t)target_oi < units_.size()) {
        unit_screen_center(units_[(size_t)target_oi], target_x, target_y); // sub_135EB: target centre
    } else {
        target_x = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ +
                   TILE_ANCHOR_X + CENTER_ADD_X;                         // 10971 + 0Ch
        target_y = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ +
                   TILE_ANCHOR_Y - 2;                                    // 10988 - 2
    }

    // loc_46EC5 bounds test: [x-16, x-16+0x120) / [y-4, y-4+0xC0).
    if ((uint16_t)(target_x - 0x10) >= 0x120 ||
        (uint16_t)(target_y - 4) >= 0xC0) return;
    const int source_x16 = (int16_t)source_x;
    const int source_y16 = (int16_t)source_y;
    target_x = (int16_t)target_x;
    target_y = (int16_t)target_y;
    if (source_x16 == target_x && source_y16 == target_y) return;

    // loc_46FE5..4704E: classes 0x8C/0x8D/0x91..0x98 replace the facing byte with
    // the larger absolute tile delta (equal |dx|/|dy| falls through to the y axis;
    // an equal coordinate selects north). 0x90 keeps the facing byte.
    int direction = attacker.facing & 3;
    if (override_direction) {
        const int dx = tx - attacker.x;
        const int dy = ty - attacker.y;
        if (std::abs(dx) > std::abs(dy))
            direction = (attacker.x < tx) ? 3 : 1;
        else
            direction = (attacker.y < ty) ? 0 : 2;
    }
    const size_t frame_index = (size_t)direction; // unk_FA070 + direction*12
    if (frame_index >= battle_effects_.size()) return;
    const TerrainOverlayFrame& projectile = battle_effects_[frame_index];
    if (projectile.rle.empty()) return;

    // loc_47085-47157: direction bit 0 selects the matching tile axis; var_18 = 4 * |delta|.
    const int tile_delta = (direction & 1) ? tx - attacker.x : ty - attacker.y;
    const int steps = std::abs(tile_delta) * 4;
    if (steps <= 0 && reaches_target) return;
    // var_1C/var_4 hold signed 16-bit screen coordinates scaled by 100. The ASM's
    // idiv deltas are proportional to (target*100 - source*100); a dodge with a zero
    // facing-axis tile delta still flies until the viewport edge, so keep its divisor nonzero.
    int current_x = (int16_t)(source_x16 * 100);
    int current_y = (int16_t)(source_y16 * 100);
    const int interpolation_divisor = std::max(1, steps);
    const int step_x = ((target_x - source_x16) * 100) / interpolation_divisor;
    const int step_y = ((target_y - source_y16) * 100) / interpolation_divisor;

    const bool old_weather_tick = weather_tick_enabled_;
    weather_tick_enabled_ = false; // sub_103A8/sub_103A0: freeze the weather phase during flight
    if (frame_group < 4) reset_unit_pose(attacker); // 4715E: sub_12124 for classes 15h..18h
    game_timer_set(frame_group == 8 ? 6 : 1);       // 47170..47187: var_8==8 -> 6, else 1
    auto blit_battle_projectile = [&](int x, int y) {
        uint8_t* dest = video_.get_backbuffer();
        if (!dest || projectile.rle.size() < 2) return;
        const std::vector<uint8_t>& rle = projectile.rle;
        const uint16_t chunks = (uint16_t)(rle[0] | (rle[1] << 8));
        size_t pos = 2;
        int64_t rel = 0;
        for (uint16_t i = 0; i < chunks; ++i) {
            if (pos + 4 > rle.size()) break;
            const uint16_t skip = (uint16_t)(rle[pos] | (rle[pos + 1] << 8));
            const size_t payload = (size_t)rle[pos + 2] * 4u + rle[pos + 3];
            pos += 4;
            rel += skip;
            if (pos + payload > rle.size()) break;
            for (size_t b = 0; b < payload; ++b) {
                // sub_5F3FD writes into a 480x400 canvas whose displayed window is
                // columns 0..319 and rows 100..299. The +100-row canvas baseline
                // cancels when cropped, but each RLE row still has a 480px stride.
                const int64_t canvas_x_raw = (int64_t)x + rel + (int64_t)b;
                int64_t row_offset = canvas_x_raw / 480;
                int64_t canvas_x = canvas_x_raw % 480;
                if (canvas_x < 0) { canvas_x += 480; --row_offset; }
                const int64_t screen_y = (int64_t)y + row_offset;
                if (canvas_x < SCREEN_W && screen_y >= 0 && screen_y < SCREEN_H)
                    dest[(size_t)screen_y * SCREEN_W + (size_t)canvas_x] = rle[pos + b];
            }
            rel += (int64_t)payload;
            pos += payload;
        }
    };
    int remaining = steps;
    for (;;) {
        // loc_4718C: hit/guard stops at the target via the distance counter; dodge flies
        // off-screen.
        if (reaches_target && --remaining < 0) break;
        current_x = (int16_t)(current_x + step_x);
        current_y = (int16_t)(current_y + step_y);
        const int draw_x = current_x / 100 + projectile.x;
        const int draw_y = current_y / 100 + projectile.y;

        render_battle_frame(); // the original restores the previous background; the port redraws the world
        blit_battle_projectile(draw_x, draw_y); // sub_5F3FD on 480px rows, cropped to the 320x200 viewport
        render_overlay();
        video_.present();
        wait_game_tick();

        // sub_47289..loc_472A6: stop when the metadata-offset position leaves the screen.
        if (draw_x <= 0 || draw_x >= SCREEN_W || draw_y <= 0 || draw_y >= SCREEN_H ||
            should_exit_ || skip_cutscene_) break;
    }
    weather_tick_enabled_ = old_weather_tick;
    // sub_1E61D loc_1E989 immediately resets the outer attack timer to 0x10 ticks.
}

// Compatibility wrapper for callers that already hold the target unit. sub_46E27 itself
// works from the selected tile (FA8B0/FA8B4), so the unit form only supplies that tile.
void CombatII::play_weapon_class5_projectile(CombatUnitPlacement& attacker,
                                              const CombatUnitPlacement& target,
                                              bool reaches_target) {
    play_weapon_class5_projectile_at(attacker, target.x, target.y, reaches_target,
                                     kSub46E27StackVar8);
}

// sub_499BD(n): random value in 0..n-1. The original uses the C runtime rand, whose sequence
// cannot be reproduced, so the port uses a deterministic LCG.
int CombatII::combat_rand(int n) {
    combat_rng_ = combat_rng_ * 1103515245u + 12345u;
    if (n <= 0) return 0;
    return (int)((combat_rng_ >> 16) % (uint32_t)n);
}

// sub_4F4CD(id) -> sub_5CB97(0x0A, id+1, 1): plays the sample at dword_1124AC[(id+1)*8].
// The sample table (.SSP loading) is not ported, so this only logs.
void CombatII::sfx(int id) {
    if (!battle_sfx_on_) return;                 // word_FDAE6 (system menu SFX toggle)
    if (script_trace_) fprintf(stderr, "COMBAT: sfx %d (sub_4F4CD, sample table not ported)\n", id);
}

// sub_135EB: screen centre of a unit's (facing, pose 0) frame.
//   x = sub_10971(tile x) + 12 + entry[4] + entry[2]/2
//   y = sub_10988(tile y) + 2 + entry[5] + entry[3]/2
// The port keeps the sub_10971/10988 output on the same convention as render_sprites sx/sy
// (without walk_off).
void CombatII::unit_screen_center(const CombatUnitPlacement& u, int& cx, int& cy) {
    // sub_135EB uses sub_10971/sub_10988 (= +4/+4) unchanged; this is not the unit anchor.
    const int sx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int sy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    const NC2Frame* f = nc2_frame_for(u.sprite_type, u.facing & 3, 0);
    if (f) { cx = sx + CENTER_ADD_X + f->x_off + f->w / 2; cy = sy + CENTER_ADD_Y + f->y_off + f->h / 2; }
    else   { cx = sx + CENTER_ADD_X;                       cy = sy + CENTER_ADD_Y + 6; }
}

// sub_174CF: 5 when [+5C] is 1..4 and the linked unit ([+77h]) class is 0x17, else
// unk_72793[[+11h]*3].
int CombatII::weapon_class(const CombatUnitPlacement& u) const {
    if (u.code_b >= 1 && u.code_b <= 4 && u.linked_class == 0x17) return 5;
    return weapon_class_for_type(u.combat_class);
}

// sub_184A4(item, column) = ITEM.DAT[(item-1)*66 + column] as an unsigned byte; item 0 gives 0.
int CombatII::battle_lookup_184a4(int type, int column) const {
    if (type <= 0 || column < 0) return 0;
    const size_t off = (size_t)(type - 1) * 66u + (size_t)column;
    return off < battle_lookup_table_.size() ? battle_lookup_table_[off] : 0;
}

// Narrow condition under which the WC1 scan may use a vector lookup instead of the list/grid walk.
bool CombatII::battle_wc1_static_occupancy_unambiguous() const {
    if (!battle_sce_snapshot_complete_ || !battle_wc1_occupancy_state_known_ ||
        map_w_ == 0 || map_h_ == 0) return false;
    const size_t map_cells = (size_t)map_w_ * (size_t)map_h_;
    if (map_cells > 10000u) return false;
    std::vector<uint8_t> occupied(map_cells, 0);
    for (const CombatUnitPlacement& unit : units_) {
        if (!unit.active) continue;
        if (unit.hp <= 0 || !unit.occupancy_hp_known || !unit.battle_list_membership_modeled ||
            unit.group < 1 || unit.group > 3 || unit.memory_army < 1 || unit.memory_army > 4 ||
            unit.x < 0 || unit.y < 0 || unit.x >= (int)map_w_ || unit.y >= (int)map_h_ ||
            unit.sprite_type < 0 || unit.sprite_type >= 0x12C || unit.linked_class >= 0 ||
            !char_file_name_for_type(unit.sprite_type)) return false;
        // sub_11E04 uses the D66A8 footprint (not the NC2 render frame's tile_w/tile_h).
        // Require the complete NC2 descriptor set for drawing, but derive occupancy from the
        // same raw table entry the original uses. This also covers large WC1 units.
        const auto nc2_it = nc2_chars_.find(unit.sprite_type);
        if (nc2_it == nc2_chars_.end() || !nc2_it->second ||
            nc2_it->second->count() != NC2Parser::DIR_COUNT * NC2Parser::POSE_COUNT) return false;
        int foot_w = 1, foot_h = 1;
        if (!battle_d66a8_footprint(unit.sprite_type, (int)unit.facing & 3,
                                    &foot_w, &foot_h) ||
            foot_w < 1 || foot_h < 1) return false;
        const int ux = (int)(int8_t)unit.x, uy = (int)(int8_t)unit.y;
        // sub_11E04 only writes when the base tile itself passes the padded guard.
        if (ux < 2 || ux >= (int)map_w_ - 2 || uy < 4 || uy >= (int)map_h_ - 4) return false;
        for (int row = 0; row < foot_h; ++row) {
            const int cy = uy - row;
            for (int col = 0; col < foot_w; ++col) {
                const int cx = ux + col;
                if (cx < 0 || cy < 0 || cx >= (int)map_w_ || cy >= (int)map_h_) return false;
                const size_t tile = (size_t)cy * map_w_ + (size_t)cx;
                if (occupied[tile] != 0) return false;
                occupied[tile] = 1;
            }
        }
    }
    return true;
}

// sub_516D0 clears each selected unit with sub_11EE4, then either group-0 sub_16D3E or
// group!=0 sub_50BAF updates occupancy and writes final footprints with sub_11E04. sub_16CF3
// links a mover to an existing occupant on its committed tile; sub_16AEF removes it from the
// old tile while stepping. Therefore routes of different selected movers may share tiles. The
// WC1 shortcut still rejects self-revisits, stationary-unit tiles, and any unproven final
// occupancy, now using the D66A8 footprint instead of a 1x1 base cell.
bool CombatII::battle_wc1_group0_move_preserves_occupancy(const VmEvent& ev, const char** reject_reason) const {
    auto reject = [&](const char* reason) {
        if (reject_reason) *reject_reason = reason;
        return false;
    };
    if (!ev.walk_occupancy_preserving_candidate || ev.walk_units.empty())
        return reject("VM 0x05 is not a complete modeled batch");
    const bool group0 = ev.walk_group == 0;
    if (group0 != ev.walk_sets_start_tile)
        return reject("VM 0x05 walk group/start-tile mismatch");
    if (!battle_sce_snapshot_complete_ || !battle_wc1_occupancy_state_known_)
        return reject("occupancy snapshot/latch is not valid before VM 0x05");
    if (!battle_wc1_static_occupancy_unambiguous())
        return reject("pre-move static occupancy or loaded class-0 descriptors are unproven");

    const size_t map_cells = (size_t)map_w_ * (size_t)map_h_;
    std::vector<int> initial_occupant(map_cells, -1);
    std::vector<int> final_occupant(map_cells, -1);
    std::vector<uint8_t> moving(units_.size(), 0);
    std::vector<uint8_t> vacates_start(units_.size(), 0);
    for (const VmWalkUnit& walk : ev.walk_units) {
        if (walk.unit_index < 0 || (size_t)walk.unit_index >= units_.size())
            return reject("VM 0x05 contains an unresolved unit reference");
        const size_t ui = (size_t)walk.unit_index;
        if (moving[ui] != 0) return reject("VM 0x05 selects the same unit more than once");
        moving[ui] = 1;
        const CombatUnitPlacement& unit = units_[ui];
        if (!unit.active || !unit.battle_list_membership_modeled || unit.group < 1 || unit.group > 3 ||
            unit.memory_army < 1 || unit.memory_army > 4)
            return reject("VM 0x05 unit is outside the modeled ordinary army lists");
        if (group0) {
            if (walk.path.empty() || walk.path.size() > 31 ||
                ev.walk_frames < (int)walk.path.size() + 1)
                return reject("VM 0x05 group-0 path length is outside the proven batch format");
            if (walk.path.front().first != unit.x || walk.path.front().second != unit.y)
                return reject("VM 0x05 path start differs from the current unit tile");
        } else {
            // sub_50BAF reads exactly one destination and keeps the current start tile.
            if (walk.path.size() != 1 || ev.walk_frames < 2)
                return reject("VM 0x05 non-group path is outside the proven batch format");
        }
        if (walk.path.back().first != unit.x || walk.path.back().second != unit.y)
            vacates_start[ui] = 1;
    }
    for (size_t ui = 0; ui < units_.size(); ++ui) {
        const CombatUnitPlacement& unit = units_[ui];
        if (!unit.active) continue;
        int foot_w = 1, foot_h = 1;
        if (!battle_d66a8_footprint(unit.sprite_type, (int)unit.facing & 3,
                                    &foot_w, &foot_h) || foot_w < 1 || foot_h < 1)
            return reject("pre-move unit footprint descriptor is unavailable");
        const int ux = (int)(int8_t)unit.x, uy = (int)(int8_t)unit.y;
        if (ux < 2 || ux >= (int)map_w_ - 2 || uy < 4 || uy >= (int)map_h_ - 4)
            return reject("pre-move unit base is outside the CC998 interior");
        for (int row = 0; row < foot_h; ++row) {
            const int cy = uy - row;
            for (int col = 0; col < foot_w; ++col) {
                const int cx = ux + col;
                if (cx < 0 || cy < 0 || cx >= (int)map_w_ || cy >= (int)map_h_)
                    return reject("pre-move unit footprint leaves the occupancy map");
                const size_t tile = (size_t)cy * map_w_ + (size_t)cx;
                if (initial_occupant[tile] >= 0)
                    return reject("pre-move unit footprints are not unique in the occupancy map");
                initial_occupant[tile] = (int)ui;
            }
        }
    }

    for (const VmWalkUnit& walk : ev.walk_units) {
        std::vector<uint8_t> path_seen(map_cells, 0);
        int previous_x = -1, previous_y = -1;
        for (size_t pi = 0; pi < walk.path.size(); ++pi) {
            const auto& point = walk.path[pi];
            const int x = point.first, y = point.second;
            // SCE and BattleUnit coordinates are signed bytes; also require the exact interior
            // accepted by sub_11DB2/sub_11F74 so no edge clipping or CC998 omission is ambiguous.
            if (x < 0 || x > 127 || y < 0 || y > 127 || x >= (int)map_w_ || y >= (int)map_h_ ||
                x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4)
                return reject("VM 0x05 path leaves the signed-byte CC998 interior");
            if (previous_x >= 0 && std::abs(x - previous_x) + std::abs(y - previous_y) != 1)
                return reject("VM 0x05 path contains a non-adjacent step");
            previous_x = x;
            previous_y = y;

            const size_t tile = (size_t)y * map_w_ + (size_t)x;
            if (path_seen[tile] != 0)
                return reject("VM 0x05 unit revisits a path tile");
            path_seen[tile] = 1;

            int facing = (int)(int8_t)units_[(size_t)walk.unit_index].facing & 3;
            if (group0 && pi + 1 < walk.path.size()) {
                const auto& next = walk.path[pi + 1];
                const int f = walk_facing_toward(x, y, next.first, next.second);
                if (f >= 0) facing = f;
            } else if (pi + 1 == walk.path.size()) {
                facing = walk.load & 3;
            }
            int foot_w = 1, foot_h = 1;
            if (!battle_d66a8_footprint(units_[(size_t)walk.unit_index].sprite_type, facing,
                                        &foot_w, &foot_h) || foot_w < 1 || foot_h < 1)
                return reject("VM 0x05 path footprint descriptor is unavailable");
            for (int row = 0; row < foot_h; ++row) {
                const int cy = y - row;
                for (int col = 0; col < foot_w; ++col) {
                    const int cx = x + col;
                    if (cx < 0 || cy < 0 || cx >= (int)map_w_ || cy >= (int)map_h_)
                        return reject("VM 0x05 path footprint leaves the occupancy map");
                    const size_t cell = (size_t)cy * map_w_ + (size_t)cx;
                    const int occupant = initial_occupant[cell];
                    const bool occupant_will_vacate = occupant >= 0 &&
                        (size_t)occupant < vacates_start.size() && vacates_start[(size_t)occupant] != 0;
                    if (occupant >= 0 && occupant != walk.unit_index && !occupant_will_vacate)
                        return reject("VM 0x05 path crosses a stationary unit's pre-move footprint");
                }
            }
        }
        const auto& endpoint = walk.path.back();
        const int ex = endpoint.first, ey = endpoint.second;
        int foot_w = 1, foot_h = 1;
        if (!battle_d66a8_footprint(units_[(size_t)walk.unit_index].sprite_type,
                                    walk.load & 3, &foot_w, &foot_h) ||
            foot_w < 1 || foot_h < 1)
            return reject("VM 0x05 final footprint descriptor is unavailable");
        if (ex < 2 || ex >= (int)map_w_ - 2 || ey < 4 || ey >= (int)map_h_ - 4)
            return reject("VM 0x05 final base is outside the CC998 interior");
        for (int row = 0; row < foot_h; ++row) {
            const int cy = ey - row;
            for (int col = 0; col < foot_w; ++col) {
                const int cx = ex + col;
                if (cx < 0 || cy < 0 || cx >= (int)map_w_ || cy >= (int)map_h_)
                    return reject("VM 0x05 final footprint leaves the occupancy map");
                const size_t tile = (size_t)cy * map_w_ + (size_t)cx;
                if (final_occupant[tile] >= 0)
                    return reject("VM 0x05 movers have overlapping final footprints");
                final_occupant[tile] = walk.unit_index;
            }
        }
    }
    if (reject_reason) *reject_reason = nullptr;
    return true;
}

void CombatII::battle_wc1_occupancy_unknown(const char* reason, uint32_t offset) {
    if (!battle_wc1_occupancy_state_known_) return;
    battle_wc1_occupancy_state_known_ = false;
    if (battle_trace_ || script_trace_) {
        if (offset == 0xFFFFFFFFu)
            fprintf(stderr, "COMBAT: WC1 occupancy latch closed: %s\n",
                    reason ? reason : "unmodeled mutation");
        else
            fprintf(stderr, "COMBAT: WC1 occupancy latch closed: %s @0x%X\n",
                    reason ? reason : "unmodeled mutation", offset);
    }
}

// Keeps the latch when full static 1x1 uniqueness still holds after a verified 1x1
// add/remove/move: each tile then has a unique occupant regardless of vector order, so the
// unmodeled CC968 ordering does not matter. If the gate breaks, close fail-closed until the
// next complete snapshot.
void CombatII::battle_wc1_occupancy_preserve_or_close(const char* reason, uint32_t offset) {
    if (!battle_wc1_occupancy_state_known_) return;
    if (battle_sce_snapshot_complete_ && battle_wc1_static_occupancy_unambiguous()) {
        if (battle_trace_ || script_trace_) {
            if (offset == 0xFFFFFFFFu)
                fprintf(stderr, "COMBAT: WC1 occupancy preserved after %s\n",
                        reason ? reason : "verified 1x1 mutation");
            else
                fprintf(stderr, "COMBAT: WC1 occupancy preserved after %s @0x%X\n",
                        reason ? reason : "verified 1x1 mutation", offset);
        }
        return;
    }
    if (battle_trace_ || script_trace_) {
        // Gate-failure diagnostics (trace only, for latch analysis after Spawn 1-5).
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& u = units_[i];
            if (!u.active) continue;
            const auto nc2_it = nc2_chars_.find(u.sprite_type);
            const bool nc2_ok = nc2_it != nc2_chars_.end() && nc2_it->second &&
                nc2_it->second->count() == NC2Parser::DIR_COUNT * NC2Parser::POSE_COUNT;
            int bad_dir = -1, bad_pose = -1;
            if (nc2_ok) {
                for (int d = 0; d < NC2Parser::DIR_COUNT && bad_dir < 0; ++d)
                    for (int p = 0; p < NC2Parser::POSE_COUNT; ++p) {
                        const NC2Frame* f = nc2_it->second->frame(d, p);
                        if (!f || f->tile_w != 1 || f->tile_h != 1) { bad_dir = d; bad_pose = p; break; }
                    }
            }
            fprintf(stderr, "COMBAT: WC1 gate check unit idx=%zu id=%d active=%d hp=%d hp_known=%d membership=%d group=%d mem_army=%d pos=(%d,%d) type=%d linked=%d nc2_ok=%d bad=(%d,%d) name=%s\n",
                    i, u.id, u.active ? 1 : 0, u.hp, u.occupancy_hp_known ? 1 : 0,
                    u.battle_list_membership_modeled ? 1 : 0, u.group, u.memory_army,
                    u.x, u.y, u.sprite_type, u.linked_class, nc2_ok ? 1 : 0, bad_dir, bad_pose,
                    char_file_name_for_type(u.sprite_type) ? char_file_name_for_type(u.sprite_type) : "(null)");
        }
    }
    battle_wc1_occupancy_unknown(reason, offset);
}

// sub_11DB2(x, y): the original returns the occupant pointer from CC998; the port scans vectors.
// sub_11CA9 initialises the four CC968 linked-list heads/tails and the 10,000-cell CC998 grid;
// sub_1134F/sub_1137A append and unlink list entries. sub_11FC4 walks the four lists in order and
// writes each unk_D66A8 descriptor footprint into the grid via sub_11E04, clearing it with
// sub_11EE4 on movement. sub_11DB2 is not a flat lookup either: it applies the x-2/y-4 offsets
// and the dword_BA842/word_BA840 guards before reading the grid. The port models the grid by
// scanning active units: each base tile must pass the sub_11E04 guard, then the unit occupies
// its current facing/pose D66A8 footprint (x..x+w-1, y-h+1..y). frame_nt/frame_mt are render
// frames and do not replace this occupancy descriptor.
int CombatII::battle_cc998_occupant(int x, int y, bool apply_guard) const {
    if (map_w_ <= 0 || map_h_ <= 0) return -1;
    if (apply_guard) {
        // sub_11DB2: (x-2) and (y-4) are zero-extended 16-bit values compared against W-4
        // and H-8. This rejects x < 2, y < 4, x >= W-2 and y >= H-4. Large footprints can
        // extend below/left of that padded origin, but sub_11DB2 does not reach those cells.
        if ((uint32_t)(x - 2) >= (uint32_t)(map_w_ - 4)) return -1;
        if ((uint32_t)(y - 4) >= (uint32_t)(map_h_ - 8)) return -1;
    } else if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) {
        return -1;
    }
    // sub_11E04 writes x..x+w-1, y-h+1..y and sub_11EE4 clears the same rectangle. The
    // footprint is selected with the unit's current [63h] facing and [64h] pose; the base
    // must itself pass sub_11E04's x in [2,W-3], y in [4,H-5] guard or nothing was written.
    // Valid final footprints do not overlap, so the last matching entry is only a deterministic
    // fallback for transient/overlapping states.
    int found = -1;
    for (size_t i = 0; i < units_.size(); ++i) {
        const auto& u = units_[i];
        if (!u.active || u.hp <= 0 || u.fx_sprite_hidden) continue;   // sub_11EE4 cleared the cell
        const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
        if (ux < 2 || ux >= (int)map_w_ - 2 || uy < 4 || uy >= (int)map_h_ - 4) continue;
        // sub_11E04 has no 1x1 fallback: a missing descriptor leaves width/height zero and
        // writes nothing, so such a unit is absent from CC998.
        uint8_t raw[13];
        if (!battle_d66a8_raw(u.sprite_type, (int)u.facing & 3, (int)u.anim_frame, raw)) continue;
        const int w = raw[0], h = raw[1];
        if (w < 1 || h < 1) continue;
        if (x >= ux && x < ux + w && y <= uy && y > uy - h) found = (int)i;
    }
    return found;
}

int CombatII::battle_unit_index_at(int x, int y) const {
    return battle_cc998_occupant(x, y, true);
}

int CombatII::battle_unit_index_at_footprint(int x, int y) const {
    return battle_cc998_occupant(x, y, true);
}

// sub_17531: the attacker's damage class (selects a column of unk_72982 / item column 0x3A+class).
//   Map kind 4 (byte_BA84E) maps class 0x86->6, 0x88->4, 0x8A->5 and everything else to -1.
//   unk_73B5A[class*9]==1 gives -1. unk_72793[class*3+1] 1..4 -> 0..3, 5 -> -2, 6+ -> that value.
//   If the result is 0, use [+15] bit 1 -> 0, bit 2 -> 1, bit 4 -> 2, else the weapon-class jump table.
int CombatII::battle_damage_class(const CombatUnitPlacement& u) const {
    const int cls = u.combat_class;
    if (sce_map_kind_ == 4) {
        if (cls == 0x86) return 6;
        if (cls == 0x88) return 4;
        if (cls == 0x8A) return 5;
        return -1;
    }
    if (kBattleClassData[cls][2] == 1) return -1;
    const int wc = weapon_class(u);
    const int table_value = (cls < kWeaponClassCount) ? kWeaponClassTable[cls][1] : 0;   // off_72794[class*3]
    switch (table_value) {
    case 0:  break;
    case 1:  return 0;
    case 2:  return 1;
    case 3:  return 2;
    case 4:  return 3;
    case 5:  return -2;
    default: return table_value;                                   // loc_17628 (eax = value as is)
    }
    if (u.combat_flags_15 & 1) return 0;                           // loc_17628 (eax = 0)
    if (u.combat_flags_15 & 2) return 1;
    if (u.combat_flags_15 & 4) return 2;
    static constexpr int kByWeaponClass[13] = { -1, 4, -1, 5, 6, 7, -1, 4, 5, 6, 0, -1, 7 };  // off_174FD
    return (wc >= 0 && wc <= 12) ? kByWeaponClass[wc] : 0;
}

// sub_1762C(target, damage_class): table value plus the two equipped items' signed-byte (cbw)
//   adjustments. unk_72982[damage_class + class*8], with damage_class a signed 16-bit value: -1
//   would read the previous class's last byte, but sub_1D770 calls this only when
//   damage_class >= 0, so the -1 column (0x39) is never read here.
int CombatII::battle_damage_adjustment(const CombatUnitPlacement& u, int damage_class) const {
    if (u.combat_flags_1e & 0x40) return 10;
    const long index = (long)u.combat_class * 8 + damage_class;
    // sub_3F184 overwrites unk_72982[index] with 0x64 for the duration of a 3EBB4 call.
    // The port substitutes the value locally instead of writing the global const table
    // (the 1762C call path reached from 3EBB4).
    int table_value;
    if (battle_ai_damage_adjust_index_ >= 0 &&
        index == battle_ai_damage_adjust_index_) {
        table_value = battle_ai_damage_adjust_value_;
    } else {
        table_value = (index >= 0 && index < kBattleDamageAdjustSize) ? kBattleDamageAdjustTable[index] : 0;
    }
    const int column = damage_class + 0x3A;
    const int sum = table_value + (int8_t)battle_lookup_184a4(u.combat_type_5b, column) +
                                  (int8_t)battle_lookup_184a4(u.combat_type_5d, column);
    return (int16_t)sum;
}

// sub_1D6B1(attacker) -> critical kind 0 (normal) / 1 / 2; updates the attacker's
//   [+2C]/[+2D] pattern row/column.
//   [+13] bit 0x20: class table unk_73B58[class*9]==1 ? 2 : 1. bit 0x40: 2.
//   Otherwise, while [+2D]==0, set [+2C]=rand(12). If pattern unk_74144[[+2C]*20 + [+2D]]
//   (a signed byte) is greater than (item [+5B] column 0x31) + 3 the result is 0, else
//   (class table==1 ? 2 : 1). Then [+2D] = ([+2D]+1) % 20.
// unk_74144 requires combat_hit_pattern.h (extracted from COMBATII.EXE); without it the pattern
// test is treated as "no critical" and a warning is printed once (not ported).
int CombatII::battle_hit_kind(CombatUnitPlacement& a) {
    const bool class_flag = kBattleClassData[a.combat_class][0] == 1;
    if (a.combat_flags_13 & 0x20) return class_flag ? 2 : 1;
    if (a.combat_flags_13 & 0x40) return 2;
    if (a.hit_seq_2d == 0) { a.hit_seq_2c = (uint8_t)combat_rand(12); a.hit_seq_2d = 0; }
    const int limit = (int8_t)battle_lookup_184a4(a.combat_type_5b, 0x31) + 3;
    const int column = a.hit_seq_2d;
    a.hit_seq_2d = (uint8_t)((column + 1) % 20);
#ifdef G2_HAVE_HIT_PATTERN
    const size_t index = (size_t)a.hit_seq_2c * 20u + (size_t)column;
    const int pattern = index < (size_t)kBattleHitPatternSize ? (int8_t)kBattleHitPattern[index] : 0;
    if (pattern > limit) return 0;
    return class_flag ? 2 : 1;
#else
    static bool warned = false;
    if (!warned) {
        warned = true;
        fprintf(stderr, "COMBAT: warning: combat_hit_pattern.h (unk_74144) missing - critical pattern not ported, always a normal hit\n");
    }
    (void)limit; (void)column;
    return 0;
#endif
}

// sub_1D770(attacker, target, ebx=hit_kind, ecx=area_mode, guard bonus): damage x100
//   (negative = healing). The guard bonus is what sub_1DDF0 adds to [target+46h] temporarily.
//   The byte_E518D inversion is applied inside the sub_44913 AI dispatch. Large-unit occupancy
//   and the contact trigger are handled by the move/attack callers; sub_17E17 runs after the
//   damage is stored (battle_attack_execute), not inside this calculation.
int CombatII::battle_damage(int attacker_index, int target_index, int hit_kind, int area_mode, int def_bonus) {
    if (attacker_index < 0 || target_index < 0 || (size_t)attacker_index >= units_.size() ||
        (size_t)target_index >= units_.size()) return 0;
    const CombatUnitPlacement& att = units_[(size_t)attacker_index];
    const CombatUnitPlacement& tgt = units_[(size_t)target_index];

    // ---- Target-side base value cx ----
    const int tgt_def = (int16_t)(tgt.combat_value_46 + def_bonus);          // [esi+46h]
    int bx = tgt_def;
    if (tgt.combat_flags_1e & 0x40) bx = bx * 70 / 100;
    int spread = (int16_t)bx / 5;
    if ((int16_t)spread <= 0) spread = 1;
    const int bx16 = (int16_t)bx;
    int cx;
    if (att.facing == tgt.facing)                        cx = (combat_rand(spread) >> 1) + (bx16 >> 1);
    else if (((att.facing + tgt.facing) & 1) != 0)       cx = bx16 * 7 / 10 + combat_rand(spread);
    else                                                 cx = bx16 * 9 / 10 + combat_rand(spread);
    cx = (int16_t)cx;
    if (tgt.combat_type_5d == 0x78) cx = tgt_def;
    if (tgt.combat_flags_15 & 0x40) cx += 0x14;
    if (tgt.combat_flags_14 & 2) { cx = (int16_t)cx; cx += cx >> 1; }
    cx = (int16_t)cx;

    // ---- Nearby-unit adjustment (ecx != 0): diamond of radius 1 (5 cells) around the target, ----
    //      excluding the attacker and including the target itself. Same side as the target:
    //      +defense*15%; hostile: -defense*15% (walk sub_1AC08/1AC59, test with sub_1201D).
    if (area_mode != 0) {
        // sub_1D770 ASM 19731: `mov edx, [esi+44h]; sar edx,10h` takes the high word of
        // dword[+44h] = [+46h] (defense). Then `shl eax,4; sub eax,edx; idiv 64h` = defense*15/100.
        const int step = (int16_t)tgt.combat_value_46 * 15 / 100;
        static constexpr int kDx[5] = { 0, -1, 0, 1, 0 };
        static constexpr int kDy[5] = { -1, 0, 0, 0, 1 };
        for (int k = 0; k < 5; ++k) {
            const int other = battle_unit_index_at(tgt.x + kDx[k], tgt.y + kDy[k]);
            if (other < 0 || other == attacker_index) continue;
            if (battle_armies_hostile(battle_effective_army(tgt),
                                      battle_effective_army(units_[(size_t)other]),
                                      battle_hostility_inverted_)) cx -= step;
            else                                                                     cx += step;
        }
        cx = (int16_t)cx;
    }

    // ---- Attacker-side value (word +0x44) ----
    int atk = (int16_t)att.combat_value_44;
    if (att.combat_flags_1e & 0x40) atk = atk * 70 / 100;
    if (att.combat_flags_15 & 8) atk += 0x14;
    if (att.combat_flags_15 & 1)      atk += 0x0F;
    else if (att.combat_flags_15 & 2) atk += 5;
    else if (att.combat_flags_15 & 4) atk += 0x0A;
    if (att.combat_flags_14 & 2) atk += atk >> 1;
    if (area_mode != 0) {                                    // army morale (dword_F0BA0 / +2)
        const int army = battle_effective_army(att); // effective [+0Eh] when modeled; raw+1 fallback for remapped/VM-converted records.
        if (army >= 1 && army <= 2)  atk = (int)morale_lo_ * atk / 100;
        else if (army == 3)          atk = (int)morale_hi_ * atk / 100;
    }
    if (hit_kind == 1 && kBattleClassData[tgt.combat_class][6] == 0) atk = atk * 150 / 100;

    int damage;
    if (kBattleClassData[att.combat_class][2] == 1) {        // class that ignores defense
        damage = (tgt.unit_kind_0d == 2 || tgt.code_b >= 7) ? 0 : atk * 100;
    } else {
        damage = (atk - cx) * 100;
    }
    if (tgt.combat_flags_13 & 1) damage = damage * 70 / 100;

    int damage_class = battle_damage_class(att);                          // sub_17531(attacker)
    if (hit_kind == 1) { if (damage < 1000) damage = 1000; }
    else if (damage < 100) damage = 100;
    if (battle_item_18ef9(att.combat_type_5b)) damage_class = -1;

    int adjust = damage_class;
    if (damage_class >= 0) {
        adjust = battle_damage_adjustment(tgt, damage_class);              // sub_1762C(target)
        damage = adjust * damage / 10;
    }
    if (adjust > 0 && tgt.combat_value_43 < 0x32 && hit_kind == 2 &&
        (tgt.code_b < 7 || tgt.code_b > 0x26)) {                           // critical kind 2 cannot kill
        const uint32_t hp_raw = (uint32_t)battle_hp_raw(tgt);
        if ((uint32_t)damage < hp_raw) damage = (int)hp_raw - 100;
    }
    if (damage < 0) {                                                      // healing is capped at max HP
        const int room = battle_max_hp_raw(tgt) - battle_hp_raw(tgt);
        if (-damage > room) damage = battle_hp_raw(tgt) - battle_max_hp_raw(tgt);
    }
    if (att.combat_class >= 0x86 && att.combat_class <= 0x8B) {            // [edi+11h] = attacker class
        damage *= 3;
        if (damage < 100) damage = 100;
    }
    if (tgt.combat_type_5d == 0x73) damage = damage / 2;                   // [esi+5Dh] = target item
    return damage;
}

// sub_1DDF0 (dx != 1 branch, single target): critical kind (sub_1D6B1) -> dodge/guard/hit roll.
//   Result in word_F0430: -1 hit, 1 guard, 2 dodge. Dodge and guard share one random value
//   (rand(100), var_10). The port keeps the original order: sub_1D6B1 -> rand(100) -> damage
//   (the rand inside sub_1D770).
BattleAttackRoll CombatII::battle_attack_roll(int attacker_index, int target_index, int shared_hit_kind) {
    BattleAttackRoll r;
    if (attacker_index < 0 || target_index < 0 || (size_t)attacker_index >= units_.size() ||
        (size_t)target_index >= units_.size()) return r;
    CombatUnitPlacement& att = units_[(size_t)attacker_index];
    const CombatUnitPlacement& tgt = units_[(size_t)target_index];

    const int wc = weapon_class(att);
    // sub_1DDF0 calls sub_1D6B1 once, before the target loop; with multiple targets the
    // caller passes the shared value in.
    const int hit_kind = shared_hit_kind >= 0 ? shared_hit_kind : battle_hit_kind(att); // word ptr dword_F0B8A+2
    r.area_mode = (wc == 5 || wc == 0x0C || wc == 0x0B) ? 0 : 1;          // var_C (ecx of sub_1D770)
    int area_kind;                                                        // var_8
    if (wc == 0x0C || wc == 0x0B || wc == 0x0D || battle_item_195ae(att.combat_type_5b) ||
        att.combat_class == 0x5C) area_kind = 1;
    else if (wc >= 6 && wc <= 0x0E) area_kind = 2;
    else area_kind = 0;

    // Target kind: sprite type >= 0x12C -> 1; else [+1E] bit 2 -> 1, bit 4 -> 2; otherwise
    // compare facings.
    int kind = 0, behavior = 0;                                           // behavior = var_4
    if (tgt.sprite_type >= 0x12C) {
        kind = 1;
    } else {
        behavior = (tgt.combat_class < kWeaponClassCount) ? kWeaponClassTable[tgt.combat_class][2] : 0;
        if (tgt.combat_flags_1e & 2)      kind = 1;
        else if (tgt.combat_flags_1e & 4) kind = 2;
    }
    if (kind == 0) {
        if (att.facing == tgt.facing) kind = 2;
        else if (((att.facing + tgt.facing) & 1) != 0) kind = 3;
    }
    const int roll = combat_rand(100);                                    // var_10

    int chance = 10;
    if (behavior == 2 && battle_lookup_184a4(tgt.combat_type_5a, 0) == 8)
        chance = (int8_t)battle_lookup_184a4(tgt.combat_type_5a, 0x32) + 10;
    if (kind == 1)      chance = -1;
    else if (kind == 2) chance >>= 1;
    else if (kind == 3) chance = chance * 75 / 100;
    if (area_kind == 1)      chance = 0;
    else if (area_kind == 2) chance = chance * 20 / 100;
    if ((int16_t)chance > roll) { r.result = 2; r.hit_kind = hit_kind; return r; }

    int guard = -1, bonus = 0;
    if (behavior == 1 && battle_lookup_184a4(tgt.combat_type_5a, 0) == 6) {
        const int item = tgt.combat_type_5a;
        guard = ((item - 0x4A) / 4 + 1) * 20 + 10;
        bonus = (int8_t)battle_lookup_184a4(item, 0x2D);
        if (tgt.combat_flags_13 & 1) { bonus += 10; guard += 20; }
        if (tgt.combat_flags_14 & 4) guard = 100;
    }
    if (kind == 1)      guard = -1;
    else if (kind == 2) guard >>= 1;
    else if (kind == 3) guard = guard * 75 / 100;
    if (area_kind == 2) guard = guard * 20 / 100;
    if ((int16_t)guard > roll) {
        r.result = 1;
        r.def_bonus = bonus;
        r.hit_kind = hit_kind != 0 ? 1 : 0;                               // on guard, ebx = (F0B8A+2 != 0)
        return r;
    }
    r.result = -1;
    r.hit_kind = hit_kind;
    return r;
}

bool CombatII::battle_resolve_attack(int attacker_index, int target_index, bool allow_ranged_ai, bool declare,
                                 BattleAttackRoll* out_roll) {
    if (attacker_index < 0 || target_index < 0 || (size_t)attacker_index >= units_.size() ||
        (size_t)target_index >= units_.size()) return false;
    {
        const auto& attacker = units_[(size_t)attacker_index];
        const auto& target = units_[(size_t)target_index];
        const int distance = std::abs(attacker.x - target.x) + std::abs(attacker.y - target.y);
        const int attacker_wc = weapon_class(attacker);
        if (!attacker.active || !target.active ||
            !battle_armies_hostile(battle_effective_army(attacker), battle_effective_army(target),
                                   battle_hostility_inverted_) ||
             (allow_ranged_ai
                 ? (attacker.hp <= 0 || target.hp <= 0 ||
                   !ai_uses_class40dfd_path(attacker_wc) || distance <= 0 ||
                   std::max(std::abs(attacker.x - target.x), std::abs(attacker.y - target.y)) >
                       battle_40dfd_scan_radius(battle_ai_gauge_baseline_, attacker_wc,
                                                attacker.combat_class))
                 : distance != 1)) return false;
    }
    if (allow_ranged_ai) {
        const auto& attacker = units_[(size_t)attacker_index];
        const auto& target = units_[(size_t)target_index];
        camera_center_on((attacker.x + target.x) / 2, (attacker.y + target.y) / 2); // sub_40BFA -> sub_46DF1
    }
    {
        auto& attacker = units_[(size_t)attacker_index];
        const auto& target = units_[(size_t)target_index];
        attacker.facing = (int8_t)((std::abs(target.x - attacker.x) > std::abs(target.y - attacker.y)) ?
            (target.x > attacker.x ? 3 : 1) : (target.y > attacker.y ? 0 : 2));
    }
    // sub_1DDF0 dx=1: sub_53797(mode 0) -> per-target sub_5394D(mode 0, table 5).
    // The counterattack path (sub_1EF9A -> sub_1DDF0 edx=0) rolls without declaring.
    if (declare) {
        run_unit_hp_trigger(attacker_index, target_index, 0);
        if (should_exit_) return true;
        run_contact_trigger(attacker_index, target_index, 0);
    }
    if (should_exit_) return true;
    if (!units_[(size_t)attacker_index].active || !units_[(size_t)target_index].active) return true;

    // sub_1DDF0: critical kind (sub_1D6B1) -> dodge/guard/hit roll -> damage (sub_1D770).
    // All of it is decided before the animation, in the original order.
    const BattleAttackRoll roll = battle_attack_roll(attacker_index, target_index);
    if (out_roll) *out_roll = roll;
    const int result = roll.result;
    int damage = 0;                                              // x100 (negative = healing)
    if (result != 2) damage = battle_damage(attacker_index, target_index, roll.hit_kind, roll.area_mode, roll.def_bonus);
    VmEvent attack; attack.type = VmEventType::Attack; attack.attacker_unit = attacker_index;
    attack.attacker_mode = roll.hit_kind; // dword_F0B8A+2 (1D6B1, 1DEA7) -> 1E2EA s / wind-up
    attack.attack_targets.push_back({target_index, 0, result});
    play_attack_sequence(attack);
    if (should_exit_) return true;
    if (result != 2 && units_[(size_t)target_index].active) {
        // The original does [+48h] -= damage(x100). Port hp is a rounded display value, so
        // hp_deficit keeps the exact x100 remainder.
        auto& tgt = units_[(size_t)target_index];
        int new_raw = battle_hp_raw(tgt) - damage;
        if (new_raw > battle_max_hp_raw(tgt)) new_raw = battle_max_hp_raw(tgt);
        const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
        const int delta = new_hp - tgt.hp;
        tgt.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
        battle_recalc_summon_attack(target_index);            // sub_17E17(target, damage)
        if (battle_trace_)
            fprintf(stderr, "COMBAT: attack unit=%d target=%d result=%d hit_kind=%d area=%d guard_bonus=%d damage_x100=%d hp %d -> %d\n",
                    units_[(size_t)attacker_index].id, tgt.id, result, roll.hit_kind, roll.area_mode, roll.def_bonus,
                    damage, tgt.hp, new_hp);
        if (delta != 0) {                                        // a delta of 0 reads as "death" in play_hp_change, so skip it
            VmEvent hp; hp.type = VmEventType::Hp; hp.hp_unit = target_index;
            hp.hp_delta = delta;
            hp.hp_dir = units_[(size_t)attacker_index].facing;
            hp.hp_killer = attacker_index;                       // if HP <= 0, sub_1DB1D(attacker, target)
            play_hp_change(hp);
        }
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: attack unit=%d target=%d result=dodge\n",
                units_[(size_t)attacker_index].id, units_[(size_t)target_index].id);
    }
    if (should_exit_) return true;
    // sub_1ED45: a surviving target gets the hit event (mode 1). A dead target's death event
    // was already handled by play_unit_death.
    if (units_[(size_t)target_index].active && units_[(size_t)target_index].hp > 0)
        run_unit_hp_trigger(target_index, attacker_index, 1);
    // sub_1E2B1, called by sub_40BFA (ASM 65625) right after sub_1ED45, clears the hit target's
    // sleep/paralysis ([1Eh]&2). battle_resolve_list does this on the player path; the generic
    // AI path has to do it here.
    if (allow_ranged_ai && !should_exit_ && units_[(size_t)target_index].active)
        battle_clear_ailment_flags(target_index, attacker_index);
    return true;
}

bool CombatII::battle_player_attack(int target_index) {
    return battle_resolve_attack(battle_player_unit_, target_index);
}

// sub_19FAB: derive shared dword_F0426+2 from BattleUnit [+54] (or [+52] high word),
// then apply the morale/status/weather modifiers and the original 0x37 lower bound.
void CombatII::battle_prepare_ai_gauge_baseline(CombatUnitPlacement& u) {
    const auto wrap_signed_word = [](int32_t value) -> int32_t {
        const uint16_t word = (uint16_t)value;
        return word < 0x8000 ? (int32_t)word : (int32_t)word - 0x10000;
    };
    int32_t baseline = (u.combat_flags_1e & 0x40)
        ? ((int32_t)u.combat_value_54 * 70) / 100 // signed IDIV of 70 * dword[+52].high / 100
        : (int32_t)u.combat_value_54;
    baseline = wrap_signed_word(baseline); // mov word ptr dword_F0426+2, ax
    const uint8_t flags13_before = u.combat_flags_13;
    if (flags13_before & 0x80) {
        u.combat_flags_13 = (uint8_t)(u.combat_flags_13 & 0x7F); // sub_19FAB consumes this one-shot bit
        baseline = 0xFA;
    }
    if (u.combat_flags_14 & 0x02) baseline = wrap_signed_word(baseline + 0x3C); // 16-bit ADD

    // The original signed shift sequence computes baseline/4 rounded toward zero for either sign.
    switch (weather_) { // dword_BA84A+3, changed by VM 0x71
        case 1:
            baseline = wrap_signed_word(baseline - baseline / 4);
            break;
        case 3: {
            // Original calls rand(3): 0 adds baseline/4, 1/2 subtract it.
            // combat_rand is this port's deterministic RNG, not DOS libc's exact rand sequence.
            const int32_t quarter = baseline / 4;
            baseline = wrap_signed_word(baseline + (combat_rand(3) == 0 ? quarter : -quarter));
            break;
        }
        case 4:
            baseline = wrap_signed_word(baseline - baseline / 3); // signed IDIV rounds toward zero, as in the original
            break;
        default:
            break; // weather 0 or 2 leaves the baseline unchanged
    }
    if (baseline < 0x37) baseline = 0x37; // signed CMP / JGE
    battle_ai_gauge_baseline_ = (int16_t)baseline; // original stores a 16-bit word
    battle_turn_start_tp_ = battle_ai_gauge_baseline_; // mov [F042A],ax (end of sub_19FAB)
    if (DevUtil::ai_trace())
        fprintf(stderr, "COMBAT: id=%d value54=%d flags1e=%02X flags13=%02X->%02X flags14=%02X weather=%u shared=%d\n",
                u.id, (int)u.combat_value_54, (unsigned)u.combat_flags_1e,
                (unsigned)flags13_before, (unsigned)u.combat_flags_13, (unsigned)u.combat_flags_14,
                (unsigned)weather_, (int)battle_ai_gauge_baseline_);
}

// sub_45ED2(target, dl=attacker facing): sets byte_F9C08/F9C0A[i] (knockback velocity).
// Pose and facing are left unchanged.
void CombatII::setup_knockback_hit(CombatUnitPlacement& t, int facing) {
    t.knock_dx = t.knock_dy = 0;
    switch (facing & 3) {
        case 0: t.knock_dy =  5; break;   // loc_45F01
        case 1: t.knock_dx = -6; break;   // loc_45F0B
        case 2: t.knock_dy = -5; break;   // loc_45F15
        case 3: t.knock_dx =  6; break;   // loc_45F1F
    }
}

// sub_4609A (mode 2, word_F0430=1): velocity 3 plus a facing change towards the attacker.
void CombatII::setup_knockback_guard(CombatUnitPlacement& t, int facing) {
    t.knock_dx = t.knock_dy = 0;
    switch (facing & 3) {
        case 0: t.knock_dy =  3; t.facing = 2; break;
        case 1: t.knock_dx = -3; t.facing = 3; break;
        case 2: t.knock_dy = -3; t.facing = 0; break;
        case 3: t.knock_dx =  3; t.facing = 1; break;
    }
}

// sub_461C6 (mode 3, word_F0430=2): diagonal velocity (4,8)/(-8,4)/(-4,-8)/(8,-4) plus facing.
void CombatII::setup_knockback_dodge(CombatUnitPlacement& t, int facing) {
    t.knock_dx = t.knock_dy = 0;
    switch (facing & 3) {
        case 0: t.knock_dx =  4; t.knock_dy =  8; t.facing = 2; break;
        case 1: t.knock_dx = -8; t.knock_dy =  4; t.facing = 3; break;
        case 2: t.knock_dx = -4; t.knock_dy = -8; t.facing = 0; break;
        case 3: t.knock_dx =  8; t.knock_dy = -4; t.facing = 1; break;
    }
}

static inline void knock_decay(int& v) { if (v > 0) v--; else if (v < 0) v++; }

// sub_46108: offset += velocity, decay velocity by 1, sub_1214F (pose = class 0 ? 10 : 0).
void CombatII::apply_knockback_step(CombatUnitPlacement& t) {
    t.walk_off_x += t.knock_dx; t.walk_off_y += t.knock_dy;
    knock_decay(t.knock_dx); knock_decay(t.knock_dy);
    t.anim_frame = (t.sprite_type < 0x12C) ? 10 : 0;
}

// sub_45F29(target, edx=target slot i, bx=frame):
//   frame == rand(2i) plays a hit sound (armor 0x18 plus 0x21/0x17/0x16 by type).
//   Even frame: off += 2*velocity -> hit pose (10).
//   Odd frame: off -= velocity; decay; off -= velocity. If both reach 0 the pose stays,
//   otherwise it becomes the hit pose. The result is a shake that decays in amplitude.
bool CombatII::apply_knockback_hit(CombatUnitPlacement& t, int tidx, int frame) {
    if (frame == combat_rand(2 * tidx)) {
        const int cb = t.code_b;
        if ((cb >= 7 && cb <= 0x1F) || (cb >= 0x33 && cb <= 0x41)) sfx(0x18);
        const int ty = t.sprite_type;
        if ((ty >= 0x3F && ty <= 0x44) || ty == 0x52) sfx(0x21);
        else if (ty < 0x3F) sfx(0x16);   // 0x17 when [unit+0Dh]==1 (faction byte not kept)
    }
    if (frame & 1) {
        t.walk_off_x -= t.knock_dx; t.walk_off_y -= t.knock_dy;
        knock_decay(t.knock_dx); knock_decay(t.knock_dy);
        t.walk_off_x -= t.knock_dx; t.walk_off_y -= t.knock_dy;
        if (t.knock_dx == 0 && t.knock_dy == 0) return false;
    } else {
        t.walk_off_x += 2 * t.knock_dx; t.walk_off_y += 2 * t.knock_dy;
    }
    t.anim_frame = (t.sprite_type < 0x12C) ? 10 : 0;     // sub_1214F
    return true;
}

// sub_12124: pose = (class 0 && [56h] != -1 && not flying) ? 3 : 0, pixel offset 0.
void CombatII::reset_unit_pose(CombatUnitPlacement& u) {
    u.anim_frame = (u.sprite_type < 0x12C && u.battle_gauge != 0xFF && !u.flying) ? 3 : 0;
    u.walk_off_x = u.walk_off_y = 0;
    u.knock_dx = u.knock_dy = 0;
}

// sub_51C74 loc_51E34: with no target and mode >= 2, v = mode-0x32 (10 if negative),
// [63h] = v/11, [64h] = v%11; redraw (sub_4559F/sub_5E9E8) and wait 32 ticks (sub_1033B(0x20) + sub_499E4).
void CombatII::play_unit_pose(const VmEvent& ev) {
    if (ev.attacker_unit < 0 || ev.attacker_unit >= (int)units_.size()) return;
    auto& a = units_[(size_t)ev.attacker_unit];
    int v = ev.attacker_mode - 0x32;
    if (v < 0) v = 10;
    a.facing = (int8_t)(v / 11);
    a.anim_frame = (uint8_t)(v % 11);
            fprintf(stderr, "COMBAT: play unit pose: unit %d -> facing %d pose %d (32 ticks)\n",
                a.id, a.facing, a.anim_frame);
    present_cutscene_frame();
    game_timer_set(0x20);
    wait_game_tick();
}

// sub_1E61D (attacker) + sub_1EF5C: the scripted attack animation.
void CombatII::play_attack_sequence(const VmEvent& ev) {
    if (ev.attacker_unit < 0 || ev.attacker_unit >= (int)units_.size()) return;
    load_effect_sprites();
    auto& A = units_[(size_t)ev.attacker_unit];
    // dword_F0B8A+2 holds hit_kind (0/1/2) for tactical attacks (sub_1DDF0, 20328: sub_1D6B1 -> +2)
    // and attacker_mode (0 normal, 1 critical) for scripted events (sub_51C74, 89092/89111).
    // sub_1E61D's prologue (21054) runs the wind-up when +2 != 0; loc_1E989 (21328) calls 19168
    // only when +2 == 1. sub_1E2EA normalises s to 1 when +2 != 0 (20702-20704).
    const int f0b8a2 = ev.attacker_mode;
    const bool has_crit_anim = (f0b8a2 != 0); // loc_1E677 preliminary motion (non-zero)
    const bool is_crit_19168 = (f0b8a2 == 1); // loc_1E989 19168 (exactly 1)
    const int swing_s = (f0b8a2 != 0) ? 1 : 0; // sub_1E2EA normalises ebx
    const int wclass = weapon_class(A);
    const int afacing = A.facing & 3;

    // sub_51C74 loc_51D63: per-target word_F0430 result and knockback setup.
    struct Tgt { CombatUnitPlacement* u; int result; int idx; };
    std::vector<Tgt> T;
    for (const auto& t : ev.attack_targets) {
        if (t.unit_index < 0 || t.unit_index >= (int)units_.size()) continue;
        auto& u = units_[(size_t)t.unit_index];
        if (t.result == 1)      { setup_knockback_guard(u, afacing); setup_knockback_hit(u, afacing); } // sub_4609A then sub_45ED2 (loc_51DC5 fallthrough)
        else if (t.result == 2) { setup_knockback_dodge(u, afacing); }                                  // sub_461C6
        else                    { setup_knockback_hit(u, afacing); }                                    // sub_45ED2
        T.push_back({&u, t.result, t.unit_index});
    }
            fprintf(stderr, "COMBAT: play attack: unit %d (type %d, wclass %d, facing %d%s) -> %zu target(s)\n",
                A.id, A.sprite_type, wclass, afacing, has_crit_anim ? ", crit" : "", T.size());

    // ---- sub_1E61D prologue ----
    A.anim_frame = 5;                       // [64h] = 5
    A.attacking = true;                     // [6Bh] = 1
    int phases = (A.sprite_type >= 0x12C || A.code_b != 0) ? 2 : 1;   // var_C
    if (wclass == 5 && A.combat_class >= 0x15 && A.combat_class <= 0x18) {
        phases = 0; A.anim_frame = 6;  // sub_1F179 == 0; it tests BattleUnit[+0x11], not sprite type [+0x61]
    }
    // var_20: NC2 head table [0] != -1 means a weapon trail sprite exists (unk_BA8AD).
    const NC2Parser* np = nullptr;
    {
        auto it = nc2_chars_.find(A.sprite_type);
        if (it != nc2_chars_.end()) np = it->second.get();
        else { nc2_frame_for(A.sprite_type, 0, 0); it = nc2_chars_.find(A.sprite_type); if (it != nc2_chars_.end()) np = it->second.get(); }
    }
    const bool has_swing = np && np->head(0) != -1;

    if (has_crit_anim && phases == 1) {              // loc_1E677: critical wind-up (+2 != 0 && var_C == 1)
        game_timer_set(0x18);
        A.anim_frame = 7;
        present_cutscene_frame();           // sub_45559
        wait_game_tick();                   // sub_499E4
        A.anim_frame++;
    }
    game_timer_set(0x10);                   // loc_1E6C6

    // Sound effect selection (var_14 / dx).
    int snd = 0, dx_flag = 0;
    {
        const int ty = A.sprite_type;
        if (wclass == 0x0E || ty == 0x7F || ty == 0x80) snd = 0x22;
        else if (wclass == 1 || wclass == 3 || wclass == 4) { if (sce_map_kind_ == 4) snd = 9; else { snd = 1; dx_flag = 1; } }
        else if (wclass == 5) { if (sce_map_kind_ == 4) snd = 9; else { snd = 3; dx_flag = 1; } }
        else if (ty == 0x33) { snd = 4; dx_flag = 1; }
        else if (wclass == 0) { snd = 2; dx_flag = 1; }
        else if (wclass == 0x0B || wclass == 0x11 || wclass == 0x12) snd = 5;
        else if (wclass == 0x0C) snd = 6;
        else if (wclass == 0x0D) snd = 7;
        else if (wclass == 0x13) snd = 8;
        else snd = 0;
    }
    if (dx_flag == 1 && has_crit_anim) {             // loc_1E7BD: critical shout (+2 != 0, 21154: jz skips)
        const int ty = A.sprite_type;
        if ((ty >= 0x3F && ty <= 0x44) || ty == 0x52) sfx(0x21);
        else if ((ty >= 0x45 && ty <= 0x47) || (ty >= 0x53 && ty <= 0x57) || ty == 0x5F) sfx(0x20);
        else if (ty >= 0x5B && ty <= 0x5D) sfx(0x22);
        else sfx(0x14);                      // 0x15 when [0Dh] == 1
    }

    // Present a frame with the trail/impact sprite layered over the unit.
    auto present_with = [&](const std::function<void()>& extra) {
        render_battle_frame();
        if (extra) extra();
        render_overlay();
        video_.present();
    };

    // ---- Four-phase loop (loc_1E856, var_10 = p, var_8 = p-1) ----
    for (int p = 0; p < 4; p++) {
        const int f8 = p - 1;               // var_8 (ecx starts at -1)
        if (p == 1) {                       // guard/dodge reaction
            for (auto& t : T) {
                if (t.result == 2) { t.u->walk_off_x = t.u->knock_dx; t.u->walk_off_y = t.u->knock_dy; t.u->anim_frame = 2; } // sub_4627E
                else if (t.result == 1) { sfx(0x0A); apply_knockback_step(*t.u); }                                           // sub_46108
            }
        } else if (p == 2) {                // moment of impact
            if (snd) sfx(snd);
            // loc_1E8E5 (ASM 21251-21327): wc 5/0Fh/11h/12h all call sub_46E27.
            // Classes 0x15..0x18, 0x35..0x38, 0x8C/0x8D and 0x90..0x98 are handled;
            // they share the four direction frames and differ only in pose/timer and
            // the direction override (see play_weapon_class5_projectile_at).
            // The remaining branches (class 0A1 -> 47712, wc 0Bh -> 47372, 0Ch -> 47A96,
            // 10h -> 47E79, 13h -> 47F3C, class 5Ch -> 47FDC, 7Fh/80h -> 4798A(1),
            // wc 0Eh -> 4798A(0)) draw BATTLE.SYB frames in the else-if chain below.
            // Class comparisons always test [11h] (combat_class), never [61h] (sprite_type).
            if (phases == 0) A.anim_frame = 0;   // loc_1E8E5: reset the pose before every effect call
            if (wclass == 5 || wclass == 0x0F || wclass == 0x11 || wclass == 0x12) {
                if (!T.empty()) {
                    play_weapon_class5_projectile(A, *T.front().u, T.front().result != 2);
                }
            } else if (A.combat_class == 0xA1) {                               // loc_1E90D -> 47712
                battle_fx_47712(ev.attacker_unit);
            } else if (wclass == 0x0B) {                                       // loc_1E921 -> 47372
                battle_fx_47372(ev.attacker_unit);
            } else if (wclass == 0x0C) {                                       // loc_1E932 -> 47A96
                battle_fx_47a96(ev.attacker_unit);
            } else if (wclass == 0x10) {                                       // loc_1E940 -> 47E79
                battle_fx_47e79(ev.attacker_unit);
            } else if (wclass == 0x13) {                                       // loc_1E94C -> 47F3C
                battle_fx_47f3c(ev.attacker_unit);
            } else if (A.combat_class == 0x5C) {                                // loc_1E95A -> 47FDC
                battle_fx_47fdc(ev.attacker_unit);
            } else if (A.combat_class == 0x7F || A.combat_class == 0x80) {      // loc_1E972 -> 4798A(1)
                battle_fx_4798a(ev.attacker_unit, 1);
            } else if (wclass == 0x0E) {                                       // loc_1E97B -> 4798A(0)
                battle_fx_4798a(ev.attacker_unit, 0);
            }
            if (is_crit_19168) {
                std::vector<int> crit_targets;
                for (const auto& t : T) crit_targets.push_back(t.idx);
                battle_crit_19168(ev.attacker_unit, crit_targets);
                if (should_exit_) return;
            }
            game_timer_set(0x10);
            for (size_t i = 0; i < T.size(); i++) {
                if (T[i].result == 1) apply_knockback_step(*T[i].u);              // sub_46108
                else if (T[i].result == -1) apply_knockback_hit(*T[i].u, (int)i, 0); // sub_45F29(bx=0)
            }
        }
        // loc_1EA0C: redraw, advance the attacker pose and run the target effects.
        std::function<void()> extra;
        if (phases > p) {
            A.anim_frame++;                 // 5 -> 6 (-> 7)
        } else {
            extra = [&, f8]() {
                for (size_t i = 0; i < T.size(); i++) {
                    if (T[i].result == 2) continue;
                    // Result 1: sub_1E460 draws the equipped shield (ITEM class 6) sprite.
                    if (T[i].result == 1) battle_draw_shield_icon(T[i].idx);
                    // sub_1E4E1(A, T, f8): elemental impact sprite CHR_EFFC[((A[15h]&7)>>1)*3 +
                    // 0x15 + f8].
                    //   The SCE data never sets the [unit+15h] element bits, so this is index 0 and
                    //   the original draws nothing either.
                }
                // sub_1E2EA(A, edx=p, ebx=dword_F0B8A): weapon trail (WEA_EFFC).
                // The original (20701 dec edx; 20706 cmp dx,1) uses BA8B3/BA8B4 (+3/+4)
                // only when f8 == 1 (p == 2), otherwise BA8B1/BA8B2 (+1/+2).
                // idx = base + f8 (20817-20819: movsx edx,dx; sar eax,18h; add edx,eax).
                // s is ebx = dword_F0B8A sar 10h (+2, hit_kind/critical) normalised to 0/1.
                if (has_swing && p < 3 && np) {
                    const int d = afacing, s = swing_s;
                    const int base = np->head(d * 10 + s * 5 + 0);
                    const int ox = (f8 == 1) ? np->head(d * 10 + s * 5 + 3) : np->head(d * 10 + s * 5 + 1);
                    const int oy = (f8 == 1) ? np->head(d * 10 + s * 5 + 4) : np->head(d * 10 + s * 5 + 2);
                    const int idx = base + f8;
                    if (base >= 0 && idx >= 0 && idx < (int)wea_effc_.size() && !wea_effc_[(size_t)idx].rle.empty()) {
                        // sub_1E2EA: sub_10971+0Ch - head[...], sub_10988+2 - head[...] ->
                        // sub_5F3FD (480 stride)
                        const int sx = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + CENTER_ADD_X - ox;
                        const int sy = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + CENTER_ADD_Y - oy;
                        blit_sprite_480(sx, sy, wea_effc_[(size_t)idx].rle.data());
                    }
                }
            };
        }
        present_with(extra);
        wait_game_tick();                   // sub_499E4
        // loc_1EAA6: extra ticks selected by code_b.
        if (p == 0 && !(A.code_b >= 1 && A.code_b <= 6)) wait_game_tick();
        if (p == 1 && (A.code_b >= 1 && A.code_b <= 6)) { wait_game_tick(); wait_game_tick(); wait_game_tick(); }
        if (should_exit_ || skip_cutscene_) break;
    }
    // sub_1E565(0): the high word of var_20 is always 0, so there is no palette flash.
    if (A.code_b >= 0x0F && A.code_b <= 0x16) A.anim_frame = 5;

    // ---- Closing 25 frames (loc_1EB3B, period 3): target shake, no damage numbers here ----
    game_timer_set(3);
    for (int f = 1; f < 0x19 && !should_exit_ && !skip_cutscene_; f++) {
        for (size_t i = 0; i < T.size(); i++) {
            if (T[i].result == 2) continue;
            if (T[i].result == 1) apply_knockback_step(*T[i].u);              // sub_46108
            else apply_knockback_hit(*T[i].u, (int)i, f);                    // sub_45F29
        }
        // loc_1EB3B: after sub_454B3, sub_1E460 for every result==1 target (21482), then sub_5E9E8.
        render_battle_frame();
        for (size_t i = 0; i < T.size(); i++)
            if (T[i].result == 1) battle_draw_shield_icon(T[i].idx);
        render_overlay();
        video_.present();
        dump_frame_if_requested();
        wait_game_tick();
    }
    A.attacking = false;                    // [6Bh] = 0
    A.anim_frame = 0;                       // [64h] = 0
    // sub_1EF5C: sub_12124 for every target.
    for (auto& t : T) reset_unit_pose(*t.u);
    present_cutscene_frame();
}

// sub_19168: per-item [5Bh] gameplay effect of a critical hit (dword_F0B8A+2 == 1).
//   The .ssp/camera presentation is replaced by logging; instant death, status and area
//   damage behave as in the original.
//   0x19/0x46 assist-d: instant death for an enemy with [0Dh] == 2.
//   0x1B/0x22/0x24/0x28 death: 1/3 chance of instant death for sprite < 0x12C && [43h] < 0x32.
//   0x1A/0x30 thunder, 0x24 fire, 0x1D atomic/darkzone, 0x2D/0x43 f-blast, 0x3B f-wave:
//     sub_18FC6 -> sub_285E4(attacker, target, base, p1, p2) damage.
//   0x31: poison every target ([1Eh] |= 0x20, [+25h] = 0x3C).
// sub_28596(unit): append to the F0648 list when [15h]&80 == 0 and [5Ch] < 7, without duplicates.
// There is no HP/active check: the sub_11DB2 occupancy grid only ever returns live units.
void CombatII::battle_crit_collect_28596(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& o = units_[(size_t)unit_index];
    if (o.combat_flags_15 & 0x80) return;                       // test byte [eax+15h],80h
    if (o.code_b >= 7) return;                                  // cmp byte [ebx+5Ch],7
    if (std::find(battle_target_list_.begin(), battle_target_list_.end(), unit_index) !=
        battle_target_list_.end())
        return;
    battle_target_list_.push_back(unit_index);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 28596 add target unit=%d (count=%zu)\n",
                o.id, battle_target_list_.size());
}

// sub_19061(x, y, radius, army): walks the sub_1AC08/sub_1AC59 diamond and appends every
// occupant returned by sub_11DB2 whose [0Eh] is hostile to cl (sub_1201D == 1) to F0648
// through sub_28596. No repositioning or animation.
void CombatII::battle_crit_collect_19061(int attacker_index, int radius) {
    if (attacker_index < 0 || (size_t)attacker_index >= units_.size()) return;
    const auto& A = units_[(size_t)attacker_index];
    const int army = battle_effective_army(A);                  // cl = [0Eh]
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    rg.dia_init((int)(int8_t)A.x, (int)(int8_t)A.y, radius);     // sub_1AC08
    int x = 0, y = 0;
    for (int guard = 0; guard < 0x10000 && rg.dia_next(x, y); ++guard) {   // sub_1AC59
        const int oi = rg.occupant_at(x, y);                    // sub_11DB2
        if (oi < 0 || (size_t)oi >= units_.size()) continue;
        if (!units_[(size_t)oi].active || units_[(size_t)oi].hp <= 0) continue;
        if (!battle_armies_hostile(army, battle_effective_army(units_[(size_t)oi]),
                                   battle_hostility_inverted_))  // sub_1201D == 1
            continue;
        battle_crit_collect_28596(oi);
    }
}

// sub_190C3: offset the origin by the loc_1810D facing step {(-1,0),(0,-1),(1,0),(0,1)}, walk
// the sub_1A452 rectangle (count 3, facing, range 0Ah) with sub_1A552 and apply the same gate.
void CombatII::battle_crit_collect_190c3(int attacker_index) {
    if (attacker_index < 0 || (size_t)attacker_index >= units_.size()) return;
    const auto& A = units_[(size_t)attacker_index];
    static constexpr int kOffX[4] = { -1, 0, 1, 0 };            // loc_1810D
    static constexpr int kOffY[4] = { 0, -1, 0, 1 };
    const int facing = A.facing & 3;
    const int army = battle_effective_army(A);
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    rg.rect_init((int)(int8_t)A.x + kOffX[facing], (int)(int8_t)A.y + kOffY[facing],
                 3, facing, 0x0A);                            // sub_1A452
    int x = 0, y = 0;
    for (int guard = 0; guard < 0x10000 && rg.rect_next(x, y); ++guard) {  // sub_1A552
        const int oi = rg.occupant_at(x, y);
        if (oi < 0 || (size_t)oi >= units_.size()) continue;
        if (!units_[(size_t)oi].active || units_[(size_t)oi].hp <= 0) continue;
        if (!battle_armies_hostile(army, battle_effective_army(units_[(size_t)oi]),
                                   battle_hostility_inverted_))  // sub_1201D == 1
            continue;
        battle_crit_collect_28596(oi);
    }
}

void CombatII::battle_crit_19168(int attacker_index, const std::vector<int>& targets) {
    if (attacker_index < 0 || (size_t)attacker_index >= units_.size()) return;
    const uint8_t item = units_[(size_t)attacker_index].combat_type_5b;
    // The damage branches of sub_19168 first extend F0648 through the area collectors
    // (sub_19061/190C3), then damage the whole list with sub_18FC6. The port keeps that list
    // in battle_target_list_.
    battle_target_list_ = targets;
    switch (item) {
        case 0x24: battle_crit_collect_19061(attacker_index, 5); break;   // fire / m-wind / hell
        case 0x1A: battle_crit_collect_19061(attacker_index, 5); break;   // thunder
        case 0x30: battle_crit_collect_19061(attacker_index, 3); break;   // thunder1
        case 0x1D: battle_crit_collect_19061(attacker_index, 3); break;   // atomic / darkzone
        case 0x3B: battle_crit_collect_190c3(attacker_index); break;      // f-wave
        default: break;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: sub_19168 unit=%d item=%02X targets=%zu\n",
                units_[(size_t)attacker_index].id, (unsigned)item, targets.size());
    auto alive = [&](int i) {
        return i >= 0 && (size_t)i < units_.size() && units_[(size_t)i].active && units_[(size_t)i].hp > 0;
    };
    switch (item) {
        case 0x19: case 0x46: { // assist-d: instant death for an enemy with [0Dh] == 2
            for (int t : targets) {
                if (!alive(t) || !alive(attacker_index)) break;
                const auto& tgt = units_[(size_t)t];
                if (!battle_armies_hostile(battle_effective_army(units_[(size_t)attacker_index]),
                                           battle_effective_army(tgt), battle_hostility_inverted_))
                    continue;
                if (tgt.unit_kind_0d != 2) continue;
                play_unit_death(t, attacker_index);
                if (should_exit_) return;
            }
            break;
        }
        case 0x1B: case 0x22: case 0x28: { // death
            for (int t : targets) {
                if (!alive(t) || !alive(attacker_index)) break;
                if (combat_rand(3) != 0) continue;             // sub_499BD(3): 1/3 chance
                const auto& tgt = units_[(size_t)t];
                if (tgt.sprite_type >= 0x12C || tgt.combat_value_43 >= 0x32) continue;
                play_unit_death(t, attacker_index);
                if (should_exit_) return;
            }
            break;
        }
        case 0x24: { // fire / m-wind / hell
            battle_apply_spell_hit(attacker_index, battle_target_list_, 0x61A8, 0x32, 0);
            break;
        }
        case 0x1A: { // thunder (5)
            battle_apply_spell_hit(attacker_index, battle_target_list_, 0x6590, 0x32, 2);
            break;
        }
        case 0x30: { // thunder1 (3)
            battle_apply_spell_hit(attacker_index, battle_target_list_, 0x5DC0, 0x32, 2);
            break;
        }
        case 0x1D: { // atomic / darkzone
            battle_apply_spell_hit(attacker_index, battle_target_list_, 0x7530, 0x46, 3);
            break;
        }
        case 0x2D: case 0x43: { // f-blast (no collection: existing list only)
            battle_apply_spell_hit(attacker_index, battle_target_list_, 0x1770, 0x14, 0);
            break;
        }
        case 0x3B: { // f-wave
            battle_apply_spell_hit(attacker_index, battle_target_list_, 0x3A98, 0x1E, 0);
            break;
        }
        case 0x31: { // poison all targets
            for (int t : targets) {
                if (!alive(t)) continue;
                auto& tgt = units_[(size_t)t];
                tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e | 0x20);
                tgt.ail_25 = 0x3C;
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: 19168 poison unit=%d poison=%u\n",
                            tgt.id, (unsigned)tgt.ail_25);
            }
            break;
        }
        default:
            break;
    }
}

// sub_5211D body (unit alive): HP change presentation.
void CombatII::play_hp_change(const VmEvent& ev) {
    if (ev.hp_unit < 0 || ev.hp_unit >= (int)units_.size()) return;
    auto& U = units_[(size_t)ev.hp_unit];
    const int delta = ev.hp_delta;
    if (delta == 0) {                       // `test edi,edi; jnz; jmp loc_52299` -> sub_1DB1D
        fprintf(stderr, "COMBAT: play hp: unit %d delta 0 -> death\n", U.id);
        play_unit_death(ev.hp_unit, ev.hp_killer);
        return;
    }
    if (delta == 0x7FFF) {                  // sub_462A7(U, (max-hp)*100)
        const int missing_raw = battle_max_hp_raw(U) - battle_hp_raw(U);
        battle_heal_462a7(ev.hp_unit, missing_raw);
        if (battle_trace_) fprintf(stderr, "COMBAT: play hp: unit %d -> full (%d)\n", U.id, U.hp);
        return;
    }
    if (delta > 0) {                        // sub_462A7(U, delta*100)
        battle_heal_462a7(ev.hp_unit, delta * 100);
        if (battle_trace_) fprintf(stderr, "COMBAT: play hp: unit %d +%d -> %d\n", U.id, delta, U.hp);
        return;
    }
    // Damage: sub_45BFB (prepare digits) -> subtract HP -> sub_45ED2 (facing tail) -> period 5 ->
    // 25 frames.
    const int dmg = -delta;
    U.walk_off_x = U.walk_off_y = 0;                       // sub_45BFB prologue: [65h] = [66h] = 0
    char digits[8]; snprintf(digits, sizeof(digits), "%d", dmg);
    const int ndig = (int)strlen(digits);
    // sub_45BFB uses sub_10971(tile x) / sub_10988(tile y) unchanged.
    const int sx0 = (U.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int sy0 = (U.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    int digit_y[5]; for (int k = 0; k < 5; k++) digit_y[k] = sy0 + 10;   // word_F9708[i*10+k*2]
    U.hp -= dmg; if (U.hp < 0) U.hp = 0;
    setup_knockback_hit(U, ev.hp_dir);
            fprintf(stderr, "COMBAT: play hp: unit %d -%d -> %d (knock dir %d)\n", U.id, dmg, U.hp, ev.hp_dir);
    static constexpr uint8_t kDmgColor[8] = {0x04, 0x10, 0x31, 0x51, 0x20, 0x01, 0x00, 0x00};  // unk_70A62[army] == unk_70A5F[army] dword-load sar 24 (0x70A5F+3==0x70A62)
    const int dmg_idx = std::min(std::max(battle_effective_army(U), 0), 7);  // sub_5211D: [esi+0Bh] sar 24 = [+0Eh] victim army
    const uint8_t color = kDmgColor[(size_t)dmg_idx];
    if (!small_font_tried_) load_small_font();
    const bool damage_small_font = small_font_loaded_;     // sub_5211D/sub_45CCB -> sub_608E6 -> TEXT.CHR
    game_timer_set(5);
    for (int f = 0; f < 0x19 && !should_exit_ && !skip_cutscene_; f++) {
        apply_knockback_hit(U, 0, f);                      // sub_45F29(U, 0, f)
        // sub_45CCB(U, f, color): digit y animation - f<5 moves all digits by -2, 5..19 bounces one
        // digit by +2, f>=20 moves all by +2.
        if (f < 5) for (int k = 0; k < 5; k++) digit_y[k] -= 2;
        else if (f >= 0x14) for (int k = 0; k < 5; k++) digit_y[k] += 2;
        else { const int k = (f - 5) / 5; if (k < 5) digit_y[k] += 2; }
        render_battle_frame();
        {
            const int x0 = sx0 + 12 - ndig * 3;
            if (x0 - 16 >= 0 && x0 - 16 < 0x120) {
                uint8_t* bb = video_.get_backbuffer();
                char one[2] = {0, 0};
                for (int k = 0; k < ndig && k < 4; k++) {
                    one[0] = digits[k];
                    const int x = x0 + k * 6, y = digit_y[k];
                    if (y - 5 < 0 || y - 5 >= 0xB2) continue;
                    if (damage_small_font) {
                        // sub_45CCB -> sub_60944 -> sub_608E6: shadow and body both use TEXT.CHR
                        // glyphs.
                        draw_small_text(x + 1, y + 1, one, 0, 1);
                        draw_small_text(x, y, one, color, 1);
                    } else if (bb && font_.is_loaded()) {
                        // Fallback to the dialogue font (as used by the HP display) only when
                        // TEXT.CHR is unavailable.
                        font_.set_text_mode(4, 0x0000, 0xFF);
                        font_.draw_string(bb, SCREEN_W, SCREEN_H, x + 1, y + 1, one);
                        font_.set_text_mode(4, (uint16_t)(color << 8), 0xFF);
                        font_.draw_string(bb, SCREEN_W, SCREEN_H, x, y, one);
                    }
                }
            }
        }
        render_overlay();
        video_.present();
        wait_game_tick();
    }
    reset_unit_pose(U);                                    // sub_12124
    if (U.hp <= 0) play_unit_death(ev.hp_unit, ev.hp_killer);            // `cmp [esi+48h],0; jg; call sub_1DB1D`
}

// sub_197AD(unit) -> sub_19718: remove from the ready queue (dword_F041C) and zero the gauge.
void CombatII::battle_remove_from_ready_queue(int unit_index) {
    battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(), battle_ready_queue_.end(), unit_index),
                              battle_ready_queue_.end());
    if (unit_index >= 0 && (size_t)unit_index < units_.size()) units_[(size_t)unit_index].battle_gauge = 0;
    if (battle_player_unit_ == unit_index) {            // port only: the unit awaiting input disappeared
        battle_player_unit_ = -1;
        battle_attack_cancel();
        battle_move_cancel();
        if (battle_state_ == 1) { battle_state_ = 0; battle_overlay_mode_ = 0; }
    }
}

// sub_1DB1D passes effective BattleUnit [+0x0E]-1 to sub_53F94.
// Use the modeled effective field; remapped group-0/raw-zero spawn/VM Faction still fall back to
// raw army.
// `group` is the SCE outer placement group (dynamic spawns use synthetic group 3), not this trigger
// argument.
int CombatII::battle_unit_group(const CombatUnitPlacement& u) const {
    return battle_effective_army(u) - 1;
}

// Active-unit count fallback for the wiped trigger; the original checks whether the CC968
// descriptor of effective army-1 is empty.
int CombatII::battle_group_alive_count(int group) const {
    int n = 0;
    for (const CombatUnitPlacement& u : units_)
        if (u.active && battle_unit_group(u) == group) ++n;
    return n;
}

// sub_1DB1D loc_1DC0D-loc_1DCF7: uses the real BattleUnit [+0x0E] to pick the dead unit's side
// and shift the grade-based morale (dword_F0BA0).
//   memory army 1/2: ally gauge (lo) -= w (floor 0x32), enemy gauge (hi) += w (cap 0x96)
//   memory army 3  : enemy gauge (hi) -= w (floor 0x32), ally gauge (lo) += w (cap 0x96);
//                    anything else is left unchanged.
// w = word_74234[grade] (extracted from object 2). The helper is modelled for the standard
// outer groups 1..3 and nonzero Spawns; group-0 remaps, raw-zero Spawns and VM Faction use
// the raw+1 fallback and can therefore differ from the original.
void CombatII::battle_apply_death_morale(const CombatUnitPlacement& dead) {
    const int level = dead.combat_value_43;
    const int grade = level > 0x50 ? 3 : level > 0x32 ? 2 : level > 0x1E ? 1 : 0;
    const int w = (int)kBattleDeathMorale[grade];
    const int mem_army = battle_effective_army(dead);
    if (mem_army == 1 || mem_army == 2) {
        morale_lo_ = (int16_t)(morale_lo_ - w);
        if (morale_lo_ < 0x32) morale_lo_ = 0x32;
        morale_hi_ = (int16_t)(morale_hi_ + w);
        if (morale_hi_ > 0x96) morale_hi_ = 0x96;
    } else if (mem_army == 3) {
        morale_hi_ = (int16_t)(morale_hi_ - w);
        if (morale_hi_ < 0x32) morale_hi_ = 0x32;
        morale_lo_ = (int16_t)(morale_lo_ + w);
        if (morale_lo_ > 0x96) morale_lo_ = 0x96;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: morale after death unit=%d raw_army=%d effective_army=%d grade=%d w=%d -> lo=%d hi=%d\n",
                dead.id, dead.army, mem_army, grade, w, (int)morale_lo_, (int)morale_hi_);
}

// sub_5401B (post-move check): area trigger (sub_5361C) -> front-unit contact trigger
// (sub_5394D, table 6) -> region unit-count trigger (sub_53C12).
// An event can remove the unit, so every step re-checks list membership (sub_11FF7 == active).
void CombatII::battle_after_move(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const int saved_target = script_forced_target_;
    VmResult area;
    const bool area_fired = run_area_trigger(units_[(size_t)unit_index], area);
    if (area_fired && !area.events.empty()) {
        fprintf(stderr, "COMBAT: move unit %d -> area trigger (%zu event(s))\n",
                units_[(size_t)unit_index].id, area.events.size());
        // The original runs the VM inline while handling the trigger, so a forced sel>=10
        // target is kept until playback finishes.
        play_vm_events(area.events);
    }
    script_forced_target_ = saved_target;
    if (should_exit_ || !units_[(size_t)unit_index].active) return;
    // sub_5401B: one tile ahead via sub_49ABC -> sub_11DB2 occupant -> sub_5394D(mode 1) if any.
    {
        const auto& u = units_[(size_t)unit_index];
        int fx = (int)(int8_t)u.x, fy = (int)(int8_t)u.y;
        switch (u.facing & 3) {
            case 0: fy += 1; break;
            case 1: fx -= 1; break;
            case 2: fy -= 1; break;
            default: fx += 1; break;
        }
        const int occ = battle_unit_index_at(fx, fy);
        if (occ >= 0) {
            run_contact_trigger(unit_index, occ, 1);            // sub_5394D(mode 1, table 6)
            if (should_exit_) return;
            if (!units_[(size_t)unit_index].active) return;      // sub_11FF7 == 0 -> done
        }
    }
    run_region_count_trigger();
}

// sub_17310 (terrain damage/healing): called after a move by sub_16834+188, sub_40295 and
// friends, right after sub_5401B.
//   Returns 1 (no effect) for [15h]&80h, [14h]&1 and classes 4C/5C/5D. bx is derived from the
//   sub_1093D raw attribute: raw<0x64 -> bx=(raw&0x0F)-9, else bx=((raw-0x64)&0x0F)-5; only
//   bx 0..5 is valid.
//   div = 0x14 for bx 0/3/4 and 0x0A otherwise; esi = [4Ch]/div. For bx>1, sub_1762C(bx-2)
//   scales esi by adjust/10.
//   [48h] -= esi, then sub_53797 (mode 1); the sub_46359/sub_462A7 animation plays only inside
//   the 12x12 viewport. [48h] <= 0 runs sub_1DB1D with no killer and returns 0, else 1.
//   Deviation: the sub_46359/sub_462A7 presentation (timer, knockback, digits) is replaced by
//   logging; HP accounting, triggers and death match. Healing clamps at max HP (loc_462BE) and
//   the viewport is approximated by the 12x12 camera window.
int CombatII::battle_terrain_17310(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 1;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (!u.active) return 1;
    if (u.combat_flags_15 & 0x80) return 1;                       // test byte ptr [eax+15h],80h
    if (u.combat_flags_14 & 1) return 1;                          // test byte ptr [ecx+14h],1
    if (u.combat_class == 0x4C || u.combat_class == 0x5C ||       // cmp dh,4Ch/5Ch/5Dh
        u.combat_class == 0x5D) return 1;
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return 1;
    if (u.x < 0 || u.y < 0 || u.x >= (int)map_w_ || u.y >= (int)map_h_) return 1;
    const size_t ti = (size_t)u.y * map_w_ + (size_t)u.x;
    if (ti >= tile_attr_.size()) return 1;
    const int raw = (int)tile_attr_[ti];                           // sub_1093D (unk_88BD4)
    int bx;
    if (raw < 0x64) bx = (raw & 0x0F) - 9;
    else bx = ((raw - 0x64) & 0x0F) - 5;
    if (bx < 0 || bx > 5) return 1;
    const int div = (bx == 0 || bx == 3 || bx == 4) ? 0x14 : 0x0A;
    const int max_raw = battle_max_hp_raw(u);
    if (max_raw <= 0) return 1;
    // Sar edx,1Fh; idiv esi - equivalent to C++ / for positive values. max 0 already returned.
    int esi = max_raw / div;
    if (bx > 1) {
        const int adjust = battle_damage_adjustment(u, bx - 2);   // sub_1762C
        esi = (esi * adjust) / 10;                                // imul+idiv 0Ah (same signed truncation)
    }
    const int old_raw = battle_hp_raw(u);
    int new_raw = old_raw - esi;
    if (new_raw > max_raw) new_raw = max_raw;                     // equivalent to the sub_462A7 clamp
    if (new_raw <= 0) {
        u.hp = 0; u.hp_deficit = 0;
    } else {
        const int new_hp = (new_raw + 99) / 100;
        u.hp = new_hp; u.hp_deficit = new_hp * 100 - new_raw;
        if (u.hp > u.max_hp) { u.hp = u.max_hp; u.hp_deficit = u.max_hp * 100 - new_raw; }
    }
    run_unit_hp_trigger(unit_index, -1, 1);                       // sub_53797(ebx=1)
    if (should_exit_ || !units_[(size_t)unit_index].active) return 0;
    // Viewport (dword_BA83C/BA838, 12 cells) approximated by a 12x12 camera window; the
    // animation is skipped outside it.
    const bool inside = (u.x >= cam_tile_x_ && u.x < cam_tile_x_ + 12 &&
                         u.y >= cam_tile_y_ && u.y < cam_tile_y_ + 12);
    if (battle_trace_) {
        if (esi > 0) {
            fprintf(stderr, "COMBAT: terrain unit=%d (%d,%d) attr=%02X bx=%d div=%d dmg=%d hp_raw %d->%d%s\n",
                    u.id, u.x, u.y, (unsigned)(raw & 0xFF), bx, div, esi,
                    old_raw, battle_hp_raw(units_[(size_t)unit_index]),
                    inside ? "" : " (outside viewport, sub_46359 skipped)");
        } else if (esi < 0) {
            fprintf(stderr, "COMBAT: terrain unit=%d (%d,%d) attr=%02X bx=%d div=%d heal=%d hp_raw %d->%d%s\n",
                    u.id, u.x, u.y, (unsigned)(raw & 0xFF), bx, div, -esi,
                    old_raw, battle_hp_raw(units_[(size_t)unit_index]),
                    inside ? "" : " (outside viewport, sub_462A7 skipped)");
        }
    }
    // sub_46359/sub_462A7 presentation is replaced by logging (no timer, knockback or digits).
    if (battle_hp_raw(units_[(size_t)unit_index]) <= 0) {
        play_unit_death(unit_index, -1);                           // sub_1DB1D(eax=0)
        return 0;
    }
    return 1;
}

// ---- Tile events (sub_187B1/sub_18B2E, 12-byte F037E records) ----
// Each record from sce_raw_ n12: f0=x, f1=y, f2=item (F0382), f3-4=exp (F0383, word LE),
// f5-8=money (F0385, dword LE), f9=type (F0388+1, 0..3), f10-11=FE04F index (word LE).
// After sub_18539 (clear 10 slots) the sub_4ED18 tail reads n12 records into F037E+2 and sets
// F0382=0xFF when FE04F == 1. The port keeps them in battle_tile_events_ plus
// battle_search_slots_ (the F0382 mirror) and battle_fe04f_.
// sub_4E4DD: reads the whole BLACKBOX.DAT state block.
//   @0x190 FDF87 (0xC8), @0x49D4 FE04F (0x1F4), @0x4BC8 word_FDF85,
//   @0x4158 100 variable records {u32 gold, u16 count, u8 items[count]} -> dword_FE36F[100].
//   byte_FE4FF (new items) is BSS in the original, so it is not read here.
//   A missing or short file leaves the remainder zeroed (the BSS initial state).
void CombatII::battle_load_fe04f() {
    for (size_t i = 0; i < sizeof(battle_fe04f_); ++i) battle_fe04f_[i] = 0;
    for (size_t i = 0; i < sizeof(battle_stash_); ++i) battle_stash_[i] = 0;   // byte_FE4FF is BSS (zero at process start)
    for (size_t i = 0; i < sizeof(battle_fdf87_); ++i) battle_fdf87_[i] = 0;
    for (uint32_t& m : battle_stash_money_) m = 0;
    std::string path = game_path("BLACKBOX.DAT");
    if (!file_exists(path.c_str())) path = game_path("blackbox.dat");
    if (!file_exists(path.c_str())) {
        std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "BLACKBOX.DAT");
        if (!alt.empty() && file_exists(alt.c_str())) path = alt;
    }
    if (!file_exists(path.c_str())) {
        if (battle_trace_) fprintf(stderr, "COMBAT: BLACKBOX load: no BLACKBOX.DAT, all zero\n");
        return;
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: BLACKBOX load: open failed (%s)\n", path.c_str());
        return;
    }
    size_t got = 0;
    if (fseek(fp, 0x190, SEEK_SET) == 0)
        fread(battle_fdf87_, 1, sizeof(battle_fdf87_), fp);           // sub_4E4DD @0x190 0xC8
    if (fseek(fp, 0x49D4, SEEK_SET) == 0)
        got = fread(battle_fe04f_, 1, sizeof(battle_fe04f_), fp);     // @0x49D4 0x1F4
    // @0x4158: 100 entries of {u32 money, u16 count, u8 item[count]}
    if (fseek(fp, 0x4158, SEEK_SET) == 0) {
        for (int i = 0; i < 100; ++i) {
            uint8_t hdr[6];
            if (fread(hdr, 1, 6, fp) != 6) break;
            battle_stash_money_[(size_t)i] =
                (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            const uint16_t cnt = (uint16_t)(hdr[4] | (hdr[5] << 8));
            if (cnt > 0 && fseek(fp, cnt, SEEK_CUR) != 0) break;
        }
    }
    fclose(fp);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: BLACKBOX load: FE04F %zu/500 bytes, FDF87 0xC8, money[%u]=%u (%s)\n",
                got, (unsigned)fdf85_word_,
                (unsigned)battle_stash_money_[fdf85_word_ < 100 ? fdf85_word_ : 0], path.c_str());
}

void CombatII::battle_tile_load_events() {
    battle_tile_events_.clear();
    for (int i = 0; i < 10; ++i) battle_search_slots_[i] = 0; // sub_18539
    // battle_search_cursor_ (F040E) is only cleared in battle_reset; the original's sub_4ED18
    // leaves it alone too.
    if (sce_raw_.size() < 0x19C) return;
    auto rd16 = [&](size_t p) -> uint16_t {
        if (p + 2 > sce_raw_.size()) return 0;
        return (uint16_t)(sce_raw_[p] | (sce_raw_[p + 1] << 8));
    };
    const uint16_t w = rd16(0x196), h = rd16(0x198);
    if (!w || !h) return;
    size_t pos = 0x19A + (size_t)w * h * 2;
    if (pos + 2 > sce_raw_.size()) return;
    for (int g = 0; g < 4; ++g) {
        if (pos + 2 > sce_raw_.size()) return;
        const uint32_t cnt = rd16(pos);
        pos += 2;
        for (uint32_t i = 0; i < cnt; ++i) {
            const size_t span = sce_unit_record_span(sce_raw_.data() + pos, sce_raw_.size() - pos);
            if (span == 0) return;
            pos += span;
        }
    }
    if (pos + 2 > sce_raw_.size()) return;
    const uint32_t n12 = rd16(pos);
    pos += 2;
    if (pos + (size_t)n12 * 12 > sce_raw_.size()) return;
    for (uint32_t i = 0; i < n12 && i < 10; ++i) {
        const uint8_t* r = sce_raw_.data() + pos + (size_t)i * 12;
        BattleTileEvent e;
        e.x = r[0]; e.y = r[1]; e.item = r[2];
        e.exp = (uint16_t)((uint16_t)r[3] | ((uint16_t)r[4] << 8));
        e.money = (uint32_t)r[5] | ((uint32_t)r[6] << 8) | ((uint32_t)r[7] << 16) | ((uint32_t)r[8] << 24);
        e.type = r[9];
        e.fe_index = (uint16_t)((uint16_t)r[10] | ((uint16_t)r[11] << 8));
        e.state = e.item; // byte_F0382 initial value (0=empty, 0xFF=consumed, 0xFE=hidden)
        // sub_4ED18 tail: FE04F[fe] == 1 marks the event consumed (F0382 = 0xFF).
        if (e.fe_index < sizeof(battle_fe04f_) && battle_fe04f_[e.fe_index] == 1)
            e.state = 0xFF;
        battle_tile_events_.push_back(e);
    }
    for (size_t i = 0; i < battle_tile_events_.size() && i < 10; ++i)
        battle_search_slots_[i] = battle_tile_events_[i].state;
    if (battle_trace_)
        for (size_t i = 0; i < battle_tile_events_.size(); ++i) {
            const auto& e = battle_tile_events_[i];
            fprintf(stderr, "COMBAT: tile event[%zu] (%u,%u) item=%u exp=%u money=%u type=%u fe=%u state=%02X\n",
                    i, (unsigned)e.x, (unsigned)e.y, (unsigned)e.item,
                    (unsigned)e.exp, (unsigned)e.money, (unsigned)e.type,
                    (unsigned)e.fe_index, (unsigned)e.state);
        }
}

// sub_187B1: post-move tile-event check. Returns the event index when either the unit's tile
// or the tile one step ahead matches an event tile.
// Gates: skip when F0382 is 0 or 0xFF; for type 2/3 also skip when unk_73B57[class*9+0] == 0
// (only classes 11/12 are 1; 76/9 are 0). Facing deltas are 0=(0,+1) south, 1=(-1,0) west,
// 2=(0,-1) north, 3=(+1,0) east (the off_187A1 inc/dec steps).
// Scans all 10 slots and returns -1 when none match. The chapter 1-7 start areas near (12,53)
// have (5,6)/(59,51), so the trigger does not fire there.
int CombatII::battle_tile_187b1(int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const auto& u = units_[(size_t)unit_index];
    if (!u.active) return -1;
    const int ux = u.x, uy = u.y;
    const int facing = u.facing & 3;
    int dx = 0, dy = 0;
    if (facing == 0) { dx = 0; dy = 1; }
    else if (facing == 1) { dx = -1; dy = 0; }
    else if (facing == 2) { dx = 0; dy = -1; }
    else { dx = 1; dy = 0; }
    for (int idx = 0; idx < 10; ++idx) {
        uint8_t state = 0, type = 0, ex = 0, ey = 0;
        if ((size_t)idx < battle_tile_events_.size()) {
            const auto& e = battle_tile_events_[(size_t)idx];
            state = e.state; type = e.type; ex = e.x; ey = e.y;
        } else {
            state = 0; // empty: outside the range sub_18539 clears
        }
        if (state == 0 || state == 0xFF) continue; // jz/loc_18872 (0xFE still fires)
        if (type == 2 || type == 3) {
            // Lea ebx,[ebx+ebx*8]; cmp byte ptr unk_73B57[ebx],0; jz skip
            if (battle_class_73b57((int)u.combat_class, 0) == 0) continue;
        }
        // Ecx=0: (x,y); ecx=1: (x+dx,y+dy) - the off_187A1 facing step.
        if ((ux == (int)ex && uy == (int)ey) ||
            (ux + dx == (int)ex && uy + dy == (int)ey))
            return idx;
    }
    return -1;
}

// sub_4ECE4(item): increments the count of stash record word_FDF85 and stores (item-1) in that
// slot: inc byte_FE4FF[idx*30]; byte_FE4FF[idx*30 + count] = item-1.
// The original overruns the 30-byte record into the next one; the port ignores items beyond 29.
void CombatII::battle_stash_add(int item) {
    const size_t rec = (size_t)fdf85_word_ * 30u;
    if (fdf85_word_ >= 100 || rec + 29u >= sizeof(battle_stash_)) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: stash add item=%d: word_FDF85=%u out of range - ignored\n", item, (unsigned)fdf85_word_);
        return;
    }
    uint8_t& cnt = battle_stash_[rec];
    if (cnt >= 29) {
        if (battle_trace_) fprintf(stderr, "COMBAT: stash add item=%d: record %u full - ignored\n", item, (unsigned)fdf85_word_);
        return;
    }
    ++cnt;
    battle_stash_[rec + cnt] = (uint8_t)(item - 1);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: stash add item=%d -> record %u count=%u (sub_4ECE4)\n", item, (unsigned)fdf85_word_, (unsigned)cnt);
}

// sub_4E622: writes the whole BLACKBOX.DAT block.
//   @1 word_FDF82 (day), @3 byte_FDF84 (phase), @0x190 unk_FDF87 (0xC8),
//   @0x49D4 unk_FE04F (0x1F4), @0x4158 100 records {u32 dword_FE36F, u16 count, u8 items[count]}.
//   When `sub_19D47() % 900 != 0` the phase advances one step, is folded in half (odd rounded up)
//   and stored; above 7 the day advances and the phase wraps to 0 (the next sub_4E4DD doubles
//   it again). The stash appends the byte_FE4FF new items after the existing file items.
void CombatII::battle_blackbox_save(int open_mode) {
    (void)open_mode;                                          // sub_4E4C4(ax): file open mode
    std::string path = game_path("BLACKBOX.DAT");
    if (!file_exists(path.c_str())) path = game_path("blackbox.dat");
    if (!file_exists(path.c_str())) {
        std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "BLACKBOX.DAT");
        if (!alt.empty() && file_exists(alt.c_str())) path = alt;
    }
    FILE* fp = file_exists(path.c_str()) ? fopen(path.c_str(), "r+b") : fopen(path.c_str(), "w+b");
    if (!fp) { fprintf(stderr, "COMBAT: warning: BLACKBOX save: cannot open file (%s)\n", path.c_str()); return; }

    // ---- Phase/day (sub_4E622 +0x0D..+0x75) ----
    if (battle_time_ % 900u != 0)
        tod_phase_ = (uint8_t)((tod_phase_ + 1) & 15);
    uint8_t stored_phase = (tod_phase_ & 1) ? (uint8_t)(tod_phase_ / 2 + 1)
                                            : (uint8_t)(tod_phase_ / 2);
    if (stored_phase > 7) {
        tod_day_ = (uint16_t)(tod_day_ + 1);
        stored_phase = 0;
    }
    tod_phase_ = stored_phase;                                 // the original also keeps the folded value in FDF84
    const uint8_t day_b[2] = { (uint8_t)(tod_day_ & 0xFF), (uint8_t)(tod_day_ >> 8) };
    bool ok = true;
    if (fseek(fp, 1, SEEK_SET) != 0 || fwrite(day_b, 1, 2, fp) != 2) ok = false;   // word_FDF82 @1
    if (ok && (fseek(fp, 3, SEEK_SET) != 0 || fwrite(&tod_phase_, 1, 1, fp) != 1)) ok = false; // byte_FDF84 @3
    if (ok && (fseek(fp, 0x190, SEEK_SET) != 0 ||
               fwrite(battle_fdf87_, 1, sizeof(battle_fdf87_), fp) != sizeof(battle_fdf87_))) ok = false;
    if (ok && (fseek(fp, 0x49D4, SEEK_SET) != 0 ||
               fwrite(battle_fe04f_, 1, sizeof(battle_fe04f_), fp) != sizeof(battle_fe04f_))) ok = false;

    // ---- Read the existing 0x4158 records ----
    constexpr long kStashBase = 0x4158;
    std::vector<uint8_t> old;
    if (fseek(fp, 0, SEEK_END) == 0) {
        const long sz = ftell(fp);
        if (sz > kStashBase && fseek(fp, kStashBase, SEEK_SET) == 0) {
            old.resize((size_t)(sz - kStashBase));
            if (fread(old.data(), 1, old.size(), fp) != old.size()) old.clear();
        }
    }
    size_t pos = 0;
    bool parsed = true;
    for (int i = 0; i < 100; ++i) {
        if (pos + 6 > old.size()) { parsed = false; break; }
        const uint16_t cnt = (uint16_t)(old[pos + 4] | (old[pos + 5] << 8));
        if (pos + 6u + cnt > old.size()) { parsed = false; break; }
        pos += 6u + cnt;
    }
    if (!parsed) {
        // If the existing records are corrupt, keep no items and write only gold and new items.
        old.clear();
        pos = 0;
    }
    if (ok && fseek(fp, kStashBase, SEEK_SET) == 0) {
        size_t op = 0;
        for (int i = 0; i < 100 && ok; ++i) {
            uint16_t old_cnt = 0;
            const uint8_t* old_items = nullptr;
            if (op + 6 <= old.size()) {
                old_cnt = (uint16_t)(old[op + 4] | (old[op + 5] << 8));
                if (op + 6u + old_cnt <= old.size()) old_items = old.data() + op + 6;
                else old_cnt = 0;
            }
            const uint8_t add = battle_stash_[(size_t)i * 30u];
            const uint16_t new_cnt = (uint16_t)(old_cnt + add);
            const uint32_t money = battle_stash_money_[(size_t)i];
            uint8_t hdr[6] = {
                (uint8_t)(money & 0xFF), (uint8_t)((money >> 8) & 0xFF),
                (uint8_t)((money >> 16) & 0xFF), (uint8_t)((money >> 24) & 0xFF),
                (uint8_t)(new_cnt & 0xFF), (uint8_t)(new_cnt >> 8)
            };
            if (fwrite(hdr, 1, 6, fp) != 6) { ok = false; break; }
            if (old_cnt && fwrite(old_items, 1, old_cnt, fp) != old_cnt) { ok = false; break; }
            if (add) {
                const uint8_t* src = battle_stash_ + (size_t)i * 30u + 1u;
                const size_t n = std::min<size_t>(add, 29u);
                if (fwrite(src, 1, n, fp) != n) { ok = false; break; }
            }
            if (old_items && op + 6u + old_cnt <= old.size()) op += 6u + old_cnt;
        }
        if (ok && op < old.size())                             // preserve the tail after 100 records
            fwrite(old.data() + op, 1, old.size() - op, fp);
    } else {
        ok = false;
    }
    if (ok)
        fprintf(stderr, "COMBAT: BLACKBOX save: day=%u phase=%u fdf87=%zu fe04f=%zu stash=%zu (sub_4E622)\n",
                tod_day_, (unsigned)tod_phase_, sizeof(battle_fdf87_), sizeof(battle_fe04f_), old.size());
    else
        fprintf(stderr, "COMBAT: warning: BLACKBOX save: write failed (%s)\n", path.c_str());
    fclose(fp);
}

// sub_4ECD7: close the BLACKBOX handle. The port closes the file per call, so this is a no-op.
void CombatII::battle_blackbox_close() {}

// ==== sub_4F4EF / sub_4FD25: genesis.qsv quick save/load ====
// The original dumps everything from FDF68 (scenario name) onward: global tables, the unit
// list and the script state. The port stores the battle state needed to resume (0x88 unit
// records, queues, camera, clock, tile events, BLACKBOX memory blocks) in the same file.
namespace {
// The 0x88 record pack/unpack helpers are defined further down (blackbox write).
void battle_pack_unit_record(const CombatUnitPlacement& u, uint8_t rec[0x88]);
CombatUnitPlacement battle_unpack_unit_record(const uint8_t* rec);
std::string battle_qsv_path() {
    std::string p = std::string(GAME_DATA_DIR) + "/genesis.qsv";
    if (!file_exists(p.c_str()))
        p = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "genesis.qsv");
    return p;
}
bool qsv_write_u32(FILE* f, uint32_t v) { return fwrite(&v, sizeof(v), 1, f) == 1; }
bool qsv_write_i32(FILE* f, int32_t v) { return qsv_write_u32(f, (uint32_t)v); }
bool qsv_read_u32(FILE* f, uint32_t& v) { return fread(&v, sizeof(v), 1, f) == 1; }
bool qsv_read_i32(FILE* f, int32_t& v) { return qsv_read_u32(f, (uint32_t&)v); }
} // namespace

bool CombatII::battle_quick_save_4f4ef() {
    // Remove any existing file with different case and create a new one.
    std::string path = battle_qsv_path();
    if (path.empty()) path = std::string(GAME_DATA_DIR) + "/genesis.qsv";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "COMBAT: warning: quick save open failed (%s)\n", path.c_str());
        return false;
    }
    char magic[8] = {'G','2','Q','S','V','2',0,0};
    bool ok = fwrite(magic, 1, sizeof(magic), f) == sizeof(magic);
    // V4 appends the two morale words (dword_F0BA0 / +2): sub_4F4EF writes them right after
    // F0420/422/424 and sub_4F85C reads them back. V5 appends the eight 100-byte trigger fire
    // flag arrays in the exact sub_4F4EF order (they live in genesis.qsv, not BLACKBOX.DAT).
    ok = ok && qsv_write_u32(f, 5);
    const std::string& scen = current_scenario_name_;
    ok = ok && qsv_write_u32(f, (uint32_t)scen.size());
    if (ok && !scen.empty()) ok = fwrite(scen.data(), 1, scen.size(), f) == scen.size();
    ok = ok && qsv_write_u32(f, map_w_) && qsv_write_u32(f, map_h_);
    ok = ok && qsv_write_u32(f, battle_time_);
    ok = ok && qsv_write_u32(f, tod_day_) && qsv_write_u32(f, tod_phase_);
    ok = ok && qsv_write_i32(f, cam_tile_x_) && qsv_write_i32(f, cam_fine_x_);
    ok = ok && qsv_write_i32(f, cam_tile_y_) && qsv_write_i32(f, cam_fine_y_);
    ok = ok && qsv_write_u32(f, battle_state_) && qsv_write_i32(f, battle_player_unit_);
    ok = ok && qsv_write_i32(f, battle_ai_gauge_baseline_);
    ok = ok && qsv_write_u32(f, battle_ai_var4_);
    ok = ok && qsv_write_u32(f, battle_move_var18_);
    ok = ok && qsv_write_u32(f, battle_used_bits_);
    ok = ok && qsv_write_u32(f, battle_var24_action_end_ ? 1 : 0);
    ok = ok && qsv_write_u32(f, battle_free_wait_ ? 1 : 0);
    ok = ok && qsv_write_u32(f, battle_hostility_inverted_ ? 1 : 0);
    ok = ok && qsv_write_u32(f, battle_simple_ring_ ? 1 : 0);
    ok = ok && qsv_write_u32(f, fdf85_word_);
    // V3 stores every slot, including inactive ones, so queue/target/player indices stay valid
    // (v2 stored only active units and therefore compressed the indices).
    ok = ok && qsv_write_u32(f, (uint32_t)units_.size());
    for (const auto& u : units_) {
        if (!ok) break;
        uint8_t rec[0x88];
        battle_pack_unit_record(u, rec);
        ok = fwrite(rec, 1, sizeof(rec), f) == sizeof(rec);
        ok = ok && qsv_write_u32(f, u.list_seq);
        ok = ok && qsv_write_u32(f, u.active ? 1u : 0u);
    }
    auto write_list = [&](const std::vector<int>& v) {
        ok = ok && qsv_write_u32(f, (uint32_t)v.size());
        for (int x : v) ok = ok && qsv_write_i32(f, x);
    };
    write_list(battle_ready_queue_);
    write_list(battle_target_list_);
    for (uint8_t b : battle_search_slots_) ok = ok && qsv_write_u32(f, b);
    ok = ok && qsv_write_u32(f, (uint32_t)battle_tile_events_.size());
    for (const auto& e : battle_tile_events_) {
        ok = ok && qsv_write_u32(f, e.x) && qsv_write_u32(f, e.y) && qsv_write_u32(f, e.item);
        ok = ok && qsv_write_u32(f, e.exp) && qsv_write_u32(f, e.money);
        ok = ok && qsv_write_u32(f, e.type) && qsv_write_u32(f, e.fe_index) && qsv_write_u32(f, e.state);
    }
    if (ok) ok = fwrite(scenario_flags_, 1, sizeof(scenario_flags_), f) == sizeof(scenario_flags_);
    if (ok) ok = fwrite(battle_fe04f_, 1, sizeof(battle_fe04f_), f) == sizeof(battle_fe04f_);
    if (ok) ok = fwrite(battle_fdf87_, 1, sizeof(battle_fdf87_), f) == sizeof(battle_fdf87_);
    if (ok) ok = fwrite(battle_stash_, 1, sizeof(battle_stash_), f) == sizeof(battle_stash_);
    if (ok) ok = fwrite(battle_stash_money_, 1, sizeof(battle_stash_money_), f) == sizeof(battle_stash_money_);
    // V3 appends the battle script/turn/option state after the blocks (v2 files have no tail).
    ok = ok && qsv_write_i32(f, script_seq_);
    ok = ok && qsv_write_i32(f, battle_turn_start_tp_);
    ok = ok && qsv_write_u32(f, battle_scan_group_);
    ok = ok && qsv_write_i32(f, battle_scan_unit_);
    ok = ok && qsv_write_u32(f, battle_scan_active_);
    ok = ok && qsv_write_u32(f, battle_ai_acted_);
    ok = ok && qsv_write_u32(f, battle_search_cursor_);
    ok = ok && qsv_write_u32(f, (uint32_t)hud_time_divisor_);
    ok = ok && qsv_write_u32(f, battle_music_on_ ? 1u : 0u);
    ok = ok && qsv_write_u32(f, battle_sfx_on_ ? 1u : 0u);
    ok = ok && qsv_write_i32(f, battle_scroll_speed_);
    ok = ok && qsv_write_u32(f, battle_overlay_mode_);
    ok = ok && qsv_write_u32(f, (uint32_t)(battle_scan_next_key_ & 0xFFFFFFFFu));
    ok = ok && qsv_write_u32(f, (uint32_t)(battle_scan_next_key_ >> 32));
    ok = ok && qsv_write_u32(f, battle_instant_camera_ ? 1u : 0u);
    ok = ok && qsv_write_u32(f, (uint32_t)(uint16_t)morale_lo_);   // dword_F0BA0
    ok = ok && qsv_write_u32(f, (uint32_t)(uint16_t)morale_hi_);   // dword_F0BA0+2
    // sub_4F4EF: 64h bytes each for unk_11044C, unk_1102BC, unk_110384, unk_110514,
    // unk_110320, unk_1103E8, unk_110254 and byte_1104B0. The hp/region arrays in the
    // original are 100 bytes wide, so only that prefix is serialized.
    if (ok) ok = fwrite(script_fired_, 1, 100, f) == 100;        // unk_11044C
    if (ok) ok = fwrite(area_trigger_fired_, 1, 100, f) == 100;  // unk_1102BC
    if (ok) ok = fwrite(contact_trigger_fired_[0], 1, 100, f) == 100; // unk_110384
    if (ok) ok = fwrite(hp_trigger_fired_[0], 1, 100, f) == 100;     // unk_110514
    if (ok) ok = fwrite(hp_trigger_fired_[1], 1, 100, f) == 100;     // unk_110320
    if (ok) ok = fwrite(contact_trigger_fired_[1], 1, 100, f) == 100; // unk_1103E8
    if (ok) ok = fwrite(region_trigger_fired_, 1, 100, f) == 100;    // unk_110254
    {
        uint8_t morale_flags[100] = {};                           // byte_1104B0
        morale_flags[0] = morale_trigger_fired_;
        if (ok) ok = fwrite(morale_flags, 1, sizeof(morale_flags), f) == sizeof(morale_flags);
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "COMBAT: warning: quick save write failed (%s)\n", path.c_str());
        return false;
    }
    fprintf(stderr, "COMBAT: quick save complete %s units=%zu scen=%s (sub_4F4EF)\n",
            path.c_str(), units_.size(), scen.c_str());
    return true;
}

bool CombatII::battle_quick_load_4fd25() {
    std::string path = battle_qsv_path();
    FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "COMBAT: warning: quick load file missing (%s)\n", path.c_str());
        return false;
    }
    // Read the header into locals first: reloading the scenario makes load_scenario() reset map
    // size, camera, clock and unit state, so the values must be applied after that load.
    struct QsvHeader {
        uint32_t version = 0;
        std::string scen;
        uint32_t map_w = 0, map_h = 0, time = 0;
        uint16_t day = 0;
        uint8_t phase = 0;
        int32_t cam_tx = 0, cam_fx = 0, cam_ty = 0, cam_fy = 0;
        uint8_t state = 0;
        int32_t player_unit = -1;
        int16_t ai_gauge = 0;
        int ai_var4 = 10, move_var18 = 10;
        uint16_t used_bits = 0;
        bool var24 = false, free_wait = false, hostility = false, simple_ring = false;
        uint16_t fdf85 = 0;
    } h;
    char magic[8] = {};
    bool ok = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
              memcmp(magic, "G2QSV2", 6) == 0;
    uint32_t scen_len = 0, uw = 0;
    int32_t i32 = 0;
    ok = ok && qsv_read_u32(f, h.version) &&
         (h.version == 2 || h.version == 3 || h.version == 4 || h.version == 5) &&
         qsv_read_u32(f, scen_len) && scen_len < 256;
    if (ok && scen_len) {
        h.scen.resize(scen_len);
        ok = fread(h.scen.data(), 1, scen_len, f) == scen_len;
    }
    ok = ok && qsv_read_u32(f, h.map_w) && qsv_read_u32(f, h.map_h);
    ok = ok && qsv_read_u32(f, h.time);
    ok = ok && qsv_read_u32(f, uw); h.day = (uint16_t)uw;
    ok = ok && qsv_read_u32(f, uw); h.phase = (uint8_t)uw;
    ok = ok && qsv_read_i32(f, h.cam_tx) && qsv_read_i32(f, h.cam_fx);
    ok = ok && qsv_read_i32(f, h.cam_ty) && qsv_read_i32(f, h.cam_fy);
    ok = ok && qsv_read_u32(f, uw); h.state = (uint8_t)uw;
    ok = ok && qsv_read_i32(f, h.player_unit);
    ok = ok && qsv_read_i32(f, i32); h.ai_gauge = (int16_t)i32;
    ok = ok && qsv_read_u32(f, uw); h.ai_var4 = (int)uw;
    ok = ok && qsv_read_u32(f, uw); h.move_var18 = (int)uw;
    ok = ok && qsv_read_u32(f, uw); h.used_bits = (uint16_t)uw;
    ok = ok && qsv_read_u32(f, uw); h.var24 = uw != 0;
    ok = ok && qsv_read_u32(f, uw); h.free_wait = uw != 0;
    ok = ok && qsv_read_u32(f, uw); h.hostility = uw != 0;
    ok = ok && qsv_read_u32(f, uw); h.simple_ring = uw != 0;
    ok = ok && qsv_read_u32(f, uw); h.fdf85 = (uint16_t)uw;
    if (!ok) { fclose(f); fprintf(stderr, "COMBAT: warning: quick load header corrupt (%s)\n", path.c_str()); return false; }

    // A different scenario reloads map and script; the same scenario keeps the current map.
    const bool same_scen = h.scen.empty() || h.scen == current_scenario_name_;
    if (!same_scen) {
        fclose(f);
        if (!load_scenario(h.scen)) {
            fprintf(stderr, "COMBAT: warning: quick load scenario load failed (%s)\n", h.scen.c_str());
            return false;
        }
        f = fopen(path.c_str(), "rb");
        if (!f) return false;
        // Skip the header again (load_scenario replaced units_/map). The skipped length must
        // match the writer exactly: scenario name + w/h (2) + the 18 u32 fields below.
        char m2[8] = {};
        auto skip_u32 = [&](uint32_t& v) { return qsv_read_u32(f, v); };
        if (fread(m2, 1, 8, f) != 8 || memcmp(m2, "G2QSV2", 6) != 0 ||
            !skip_u32(uw) || (uw != 2 && uw != 3 && uw != 4 && uw != 5) ||
            !skip_u32(uw) || uw >= 256) {
            fclose(f); return false;
        }
        if (uw && fseek(f, (long)uw, SEEK_CUR) != 0) { fclose(f); return false; }
        for (int i = 0; i < 20; ++i) if (!skip_u32(uw)) { fclose(f); return false; } // w/h + time..fdf85
    }

    uint32_t unit_count = 0;
    ok = qsv_read_u32(f, unit_count) && unit_count <= 512;
    std::vector<CombatUnitPlacement> loaded;
    loaded.reserve(unit_count);
    if (ok) {
        for (uint32_t i = 0; i < unit_count; ++i) {
            uint8_t rec[0x88]; uint32_t seq = 0, active = 1;
            if (fread(rec, 1, sizeof(rec), f) != sizeof(rec) || !qsv_read_u32(f, seq) ||
                (h.version >= 3 && !qsv_read_u32(f, active))) { ok = false; break; }
            CombatUnitPlacement u = battle_unpack_unit_record(rec);
            u.list_seq = seq;
            u.active = active != 0;
            loaded.push_back(u);
        }
    }
    auto read_list = [&](std::vector<int>& v) {
        uint32_t n = 0;
        if (!ok || !qsv_read_u32(f, n) || n > 512) { ok = false; return; }
        v.clear(); v.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t x = 0;
            if (!qsv_read_i32(f, x)) { ok = false; return; }
            v.push_back(x);
        }
    };
    std::vector<int> ready, targets;
    read_list(ready); read_list(targets);
    if (ok) for (uint8_t& b : battle_search_slots_) { if (!qsv_read_u32(f, uw)) { ok = false; break; } b = (uint8_t)uw; }
    uint32_t tile_count = 0;
    ok = ok && qsv_read_u32(f, tile_count) && tile_count <= 64;
    std::vector<BattleTileEvent> tiles;
    if (ok) {
        for (uint32_t i = 0; i < tile_count; ++i) {
            BattleTileEvent e; uint32_t v = 0;
            ok = ok && qsv_read_u32(f, v); e.x = (uint8_t)v;
            ok = ok && qsv_read_u32(f, v); e.y = (uint8_t)v;
            ok = ok && qsv_read_u32(f, v); e.item = (uint8_t)v;
            ok = ok && qsv_read_u32(f, v); e.exp = (uint16_t)v;
            ok = ok && qsv_read_u32(f, v); e.money = v;
            ok = ok && qsv_read_u32(f, v); e.type = (uint8_t)v;
            ok = ok && qsv_read_u32(f, v); e.fe_index = (uint16_t)v;
            ok = ok && qsv_read_u32(f, v); e.state = (uint8_t)v;
            if (ok) tiles.push_back(e);
        }
    }
    if (ok) ok = fread(scenario_flags_, 1, sizeof(scenario_flags_), f) == sizeof(scenario_flags_);
    if (ok) ok = fread(battle_fe04f_, 1, sizeof(battle_fe04f_), f) == sizeof(battle_fe04f_);
    if (ok) ok = fread(battle_fdf87_, 1, sizeof(battle_fdf87_), f) == sizeof(battle_fdf87_);
    if (ok) ok = fread(battle_stash_, 1, sizeof(battle_stash_), f) == sizeof(battle_stash_);
    if (ok) ok = fread(battle_stash_money_, 1, sizeof(battle_stash_money_), f) == sizeof(battle_stash_money_);
    uint32_t tail[17] = {};
    bool tail_ok = true;
    if (ok && h.version >= 3) {
        const int tail_n = (h.version >= 4) ? 17 : 15;   // v4 adds the two morale words
        for (int i = 0; i < tail_n; ++i)
            if (!qsv_read_u32(f, tail[i])) { tail_ok = false; break; }
    }
    if (ok && tail_ok && h.version >= 5) {
        // V5 trigger fire flags (sub_4F85C reads the same eight 100-byte blocks).
        auto read_flags = [&](uint8_t* dst, size_t n) {
            if (!ok) return;
            ok = fread(dst, 1, n, f) == n;
        };
        read_flags(script_fired_, 100);        // unk_11044C
        read_flags(area_trigger_fired_, 100);  // unk_1102BC
        read_flags(contact_trigger_fired_[0], 100); // unk_110384
        read_flags(hp_trigger_fired_[0], 100);      // unk_110514
        read_flags(hp_trigger_fired_[1], 100);      // unk_110320
        read_flags(contact_trigger_fired_[1], 100); // unk_1103E8
        read_flags(region_trigger_fired_, 100);     // unk_110254
        uint8_t morale_flags[100] = {};             // byte_1104B0
        read_flags(morale_flags, sizeof(morale_flags));
        if (ok) morale_trigger_fired_ = morale_flags[0];
    }
    fclose(f);
    if (!ok || !tail_ok) { fprintf(stderr, "COMBAT: warning: quick load body corrupt (%s)\n", path.c_str()); return false; }

    // Apply the saved header values only when the body parsed cleanly, so a corrupt file
    // cannot break the running battle.
    map_w_ = h.map_w; map_h_ = h.map_h;
    battle_time_ = h.time;
    battle_hud_time_ = battle_time_;   // the HUD clock follows immediately (first 44EB0 after sub_4F85C)
    tod_day_ = h.day; tod_phase_ = h.phase;
    cam_tile_x_ = h.cam_tx; cam_fine_x_ = h.cam_fx;
    cam_tile_y_ = h.cam_ty; cam_fine_y_ = h.cam_fy;
    battle_state_ = h.state;
    battle_player_unit_ = h.player_unit;
    battle_ai_gauge_baseline_ = h.ai_gauge;
    battle_ai_var4_ = h.ai_var4;
    battle_move_var18_ = h.move_var18;
    battle_used_bits_ = h.used_bits;
    battle_var24_action_end_ = h.var24;
    battle_free_wait_ = h.free_wait;
    battle_free_cycle_ = 0; // the original cannot save inside the blocking 5580F loop
    battle_hostility_inverted_ = h.hostility;
    battle_simple_ring_ = h.simple_ring;
    fdf85_word_ = h.fdf85;
    battle_tick_next_us_ = 0.0;

    units_ = std::move(loaded);
    battle_ready_queue_ = std::move(ready);
    battle_target_list_ = std::move(targets);
    battle_tile_events_ = std::move(tiles);
    uint32_t max_seq = 0;
    for (const auto& u : units_) if (u.list_seq > max_seq) max_seq = u.list_seq;
    battle_list_seq_counter_ = max_seq;
    battle_attack_cancel();
    battle_move_cancel();
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    battle_magic_pending_r_ = -1;
    battle_item_pending_dx_ = -1;
    if (battle_state_ == 1) battle_free_wait_ = true;
    if (h.version >= 3) {
        script_seq_ = (int32_t)tail[0];
        battle_turn_start_tp_ = (int16_t)(int32_t)tail[1];
        battle_scan_group_ = (uint16_t)tail[2];
        battle_scan_unit_ = (int32_t)tail[3];
        battle_scan_active_ = (uint16_t)tail[4];
        battle_ai_acted_ = (uint16_t)tail[5];
        battle_search_cursor_ = (int)tail[6];
        hud_time_divisor_ = (int)tail[7];
        if (hud_time_divisor_ < 2 || hud_time_divisor_ > 30) hud_time_divisor_ = 10;
        battle_turn_interval_ = hud_time_divisor_;
        battle_music_on_ = tail[8] != 0;
        battle_sfx_on_ = tail[9] != 0;
        battle_scroll_speed_ = ((int32_t)tail[10] == 8) ? 8 : 4;
        battle_overlay_mode_ = (uint16_t)tail[11];
        battle_scan_next_key_ = ((uint64_t)tail[12]) | ((uint64_t)tail[13] << 32);
        battle_instant_camera_ = tail[14] != 0;
        battle_scroll_speed_ = battle_instant_camera_ ? 8 : 4;
    }
    if (h.version >= 4) {
        // sub_4F85C reads F0BA0 / F0BA0+2 directly (the morale gauges and the damage modifier
        // input).
        morale_lo_ = (int16_t)(uint16_t)tail[15];
        morale_hi_ = (int16_t)(uint16_t)tail[16];
    }
    present_cutscene_frame();
    video_.present();                       // screen refresh after sub_4A321
    fprintf(stderr, "COMBAT: quick load complete %s units=%zu scen=%s (sub_4FD25)\n",
            path.c_str(), units_.size(), current_scenario_name_.c_str());
    return true;
}

// sub_4E0EE (the system menu) lives in combatii_sysmenu.cpp (MENUSTAT sprite mouse menu).

// Kept for existing callers: forwards to the full sub_4E622 save.
void CombatII::battle_stash_save() {
    battle_blackbox_save(2);
}

// sub_1888D(unit, new_item): two-choice dialog shown when both bag slots are full.
//   (1) "Send the item to the stash" (y=0x92): stash the new item (sub_4ECE4(new item)).
//   (2) "Send the bag item to the stash and take this one" (y=0xA2): stash [5Fh] and put the
//       new item in that slot.
//   Input: Up/Down toggles the selection (redraw), Enter confirms, Esc/right click cancels (0),
//   left click confirms when it hits the already-selected row, otherwise it only moves the
//   selection (and does not redraw, because the original clears var_10).
//   Any other key or mouse motion changes neither the selection nor the screen.
bool CombatII::battle_item_full_select(int unit_index, int new_item) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const std::string kMsgFull(kItemFullMsgFull), kChoiceNew(kItemFullChoiceNew), kChoiceOld(kItemFullChoiceOld);
    int sel = 0;                       // edi (selected row)
    bool redraw = true;                // var_10 (needs redraw)
    int last_mx = input_.get_mouse_x(), last_my = input_.get_mouse_y();

    auto draw = [&]() {
        const auto& u = units_[(size_t)unit_index];
        render_battle_frame();                                      // sub_454B3
        render_dialog_dimming(0x10, 0x78, 0x130 - 0x10, 0xBD - 0x78);   // sub_6104A(0) + sub_5EDDE(0x10,0x78,0x130,0xBD)
        render_dialog_dimming(0x10, 0x7D, 0x130 - 0x10, 0xB8 - 0x7D);   // sub_5EDDE(0x10,0x7D,0x130,0xB8)
        if (uint8_t* bb = video_.get_backbuffer()) {                // sub_6104A(1) + sub_5EDDE(0x18,y,0x12A,y+0x10): highlight the selected row
            const uint8_t* lut = battle_move_shade_lut();
            const int y0 = 0x92 + sel * 16;
            for (int y = y0; y < y0 + 0x10; ++y) {
                if (y < 0 || y >= SCREEN_H) continue;
                for (int x = 0x18; x < 0x12A; ++x) {
                    if (x < 0 || x >= SCREEN_W) continue;
                    uint8_t& p = bb[(size_t)y * SCREEN_W + (size_t)x];
                    p = lut[p];
                }
            }
        }
        if (u.face >= 0) show_portrait(u.face, 0x48, 0x78);        // sub_48132(0x48, 0x78, [unit+10h], 0)
        int nx = 0x48 - 4 * (int)u.name.size();                    // sub_5E048(0x48-4*strlen, 0x5D, "%s", [+2])
        draw_text_wrapped(nx, 0x5D, u.name);
        draw_text_wrapped(0x20, 0x82, kMsgFull);
        draw_text_wrapped(0x20, 0x92, kChoiceNew);
        draw_text_wrapped(0x20, 0xA2, kChoiceOld);
    };

    int result = -1;                   // 1 = Enter (confirm), 0 = cancel
    while (result < 0) {
        if (redraw) draw();
        video_.present();                                          // sub_5D41E + sub_5E9E8
        dump_frame_if_requested();
        // loc_18A24: wait for a getch_ key or a mouse event; keys take priority over mouse events.
        bool got = false;
        while (!got) {
            input_.update();
            if (input_.quit_requested()) { should_exit_ = true; return false; }
            const MouseState& m = input_.get_mouse();
            if (input_.any_getch_key_pressed()) {
                got = true;
                if (input_.is_key_pressed(SDLK_UP) || input_.is_key_pressed(SDLK_DOWN)) {   // 4800h / 5000h scan codes
                    sel = (sel == 0) ? 1 : 0;                      // test di,di; setz
                    redraw = true;
                } else {
                    redraw = false;                                // loc_18ACA
                    if (input_.is_key_pressed(SDLK_RETURN) || input_.is_key_pressed(SDLK_KP_ENTER)) result = 1;
                    else if (input_.is_key_pressed(SDLK_ESCAPE)) result = 0;
                }
            } else if (m.right_clicked) {                          // event 3: right press -> cancel
                got = true; redraw = false; result = 0;
            } else if (m.left_clicked) {                           // event 2: left press
                got = true;
                const int y = m.y;
                if (y > 0x92 && y < 0xB2) {                        // cmp 92h/0B2h: both choice rows only
                    const int row = (y - 0x92) >> 4;
                    if (row == sel) result = 1;                    // same row = Enter (var_28 = 0Dh)
                    else sel = row;                                // mov edi, eax (var_10 unchanged)
                }
            } else if (m.x != last_mx || m.y != last_my) {         // event 1: mouse motion -> var_10 = 0
                got = true; redraw = false;
            }
            last_mx = m.x; last_my = m.y;
            if (!got) SDL_Delay(1);
        }
    }
    present_cutscene_frame();                                      // loc_18AEA: sub_45559
    if (result != 1) return false;
    const int slot_item = (int)units_[(size_t)unit_index].inv_5f;
    if (sel == 0) {
        battle_stash_add(new_item);                                // sub_4ECE4(new item)
    } else {
        battle_stash_add(slot_item);                               // sub_4ECE4([unit+5Fh])
        units_[(size_t)unit_index].inv_5f = (uint8_t)new_item;     // [unit+5Fh] = new item
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: item full unit=%d new=%d choice=%d (sub_1888D)\n", units_[(size_t)unit_index].id, new_item, sel);
    return true;
}

// sub_18B2E: execute a tile event. The index from sub_187B1 grants the item/exp/money and
// marks the event consumed.
//   [0x77] link x/y copy (the port only checks linked_class and skips; chapters 1-7 have no links).
//   Item (state<0x96): fill [0x5E] then [0x5F]; when both are full sub_1888D asks the player and
//   a cancel leaves the event unconsumed.
//     Shows EDD88[(item-1)*66+1] name + "] ..." through sub_482A5 (the port logs and draws UI text).
//   exp (F0383): [0x3D] += exp; at 100 or more sub_17A20 levels up and resets it to 0.
//     Applied to the linked unit as well (skipped when there is no link).
//   money (F0385): display only ("ELD ..."); the original changes no state either.
//   Consume: F0382 = 0xFF and FE04F[fe] = 1. The sub_482A5 presentation (camera/dim/wait) is
//   replaced by logging.
void CombatII::battle_tile_18b2e(int unit_index, int event_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (event_index < 0 || (size_t)event_index >= battle_tile_events_.size()) return;
    auto& u = units_[(size_t)unit_index];
    if (!u.active) return;
    BattleTileEvent& e = battle_tile_events_[(size_t)event_index];
    const uint8_t item = e.state; // byte_F0382 (sub_187B1 filters 0/FF)
    // [0x77] link copy: the port only checks linked_class and skips when one exists.
    if (u.linked_class >= 0 && battle_trace_)
        fprintf(stderr, "COMBAT: tile event[%d] unit=%d linked copy (sub_18B2E [77h] Tripwire, skipped)\n",
                event_index, u.id);
    // ---- Item (dl < 0x96) ----
    if (item < 0x96) {
        if (u.inv_5e == 0) {
            u.inv_5e = item; // mov [edx+5Eh],al
        } else if (u.inv_5f == 0) {
            u.inv_5f = item; // mov [edx+5Fh],al
        } else {
            // sub_1888D: full-bag two-choice dialog. A cancel (0) leaves the event unconsumed (jz
            // loc_18D2E).
            if (!battle_item_full_select(unit_index, (int)item)) {
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: tile event[%d] unit=%d item=%u slots full - cancelled, not consumed (sub_1888D)\n",
                            event_index, u.id, (unsigned)item);
                return;
            }
        }
        // sub_18B2E+0x86: buffer = unk_7707F ("\n[") + EDD88[(item-1)*66+1] (Johab name) + aBcb_1
        //   ("] acquired!") -> sub_482A5(unit, buffer). The name is passed as the NUL-terminated
        //   Johab byte string unchanged (strcat_).
        std::string iname_raw;
        if (!battle_lookup_table_.empty() && item >= 1) {
            const size_t off = ((size_t)item - 1) * 66 + 1;
            for (size_t k = off; k < battle_lookup_table_.size() && battle_lookup_table_[k] != 0; ++k)
                iname_raw.push_back((char)battle_lookup_table_[k]);
        }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: tile event[%d] unit=%d got item=%u (%s) inv=(%u,%u) (sub_18B2E item)\n",
                    event_index, u.id, (unsigned)item, Font::johab_to_utf8(iname_raw).c_str(),
                    (unsigned)u.inv_5e, (unsigned)u.inv_5f);
        std::string msg = "\n[";                                  // unk_7707F
        msg += iname_raw;
        msg += "] ";                                              // aBcb_1 (0x77082)
        msg += "\xD2\x42\x97\x62";
        msg += "!\n";
        battle_unit_message(unit_index, msg);                     // sub_482A5
    }
    // ---- exp/money display and exp gain (F0383/F0385) ----
    {
        if (battle_trace_ && (e.exp != 0 || e.money != 0))
            fprintf(stderr, "COMBAT: tile event[%d] unit=%d exp=%u money=%u (sub_18B2E)\n",
                    event_index, u.id, (unsigned)e.exp, (unsigned)e.money);
        // The buffer [FA784] starts empty; when exp (word F0383) != 0 it appends unk_7708B +
        // itoa(exp) + unk_77093, and when money (dword F0385) != 0 it appends ltoa(money) +
        // aEldBcb_0 (money is display only, no state change).
        std::string msg;
        if (e.exp != 0) {
            msg += "\x89\x77\xD0\xF1\xC3\xA1 ";                    // unk_7708B
            msg += std::to_string((unsigned)e.exp);
            msg += " \xB8\xF1 \xD2\x42\x97\x62!\n";                // unk_77093
        }
        if (e.money != 0) {
            msg += std::to_string((unsigned)e.money);
            msg += "ELD \xD2\x42\x97\x62!\n";                      // aEldBcb_0 (0x7709E)
        }
        if (!msg.empty()) battle_unit_message(unit_index, msg);   // sub_482A5
    }
    if (e.exp != 0) {
        // Mov al, byte ptr [var_4]: only the LOW byte of the F0383 word is added.
        const uint8_t gain = (uint8_t)e.exp;
        const uint8_t sum = (uint8_t)(u.exp_3d + gain);       // add [ecx+3Dh],al
        u.exp_3d = sum;
        if (u.stored_77) u.stored_77->exp_3d = sum;           // [ecx+77h] -> [+3Dh]
        if (battle_trace_)
            fprintf(stderr, "COMBAT: tile event[%d] unit=%d exp %u+%u -> %u\n",
                    event_index, u.id, (unsigned)(uint8_t)(sum - gain), (unsigned)gain, (unsigned)sum);
        if (u.exp_3d >= 0x64) {
            // sub_18B2E: [3Dh]=100 -> sub_17A20(unit, 0) -> [3Dh]=0. At most one level-up per
            // event.
            u.exp_3d = 0x64;
            battle_gain_exp(unit_index, -1);                  // sub_17A20
            if (should_exit_) return;
            if ((size_t)unit_index >= units_.size()) return;
            units_[(size_t)unit_index].exp_3d = 0;
        }
        if ((size_t)unit_index < units_.size()) {
            auto& nu = units_[(size_t)unit_index];
            if (nu.stored_77) nu.stored_77->exp_3d = nu.exp_3d; // [ecx+77h][3Dh] = [ecx+3Dh]
        }
    }
    // ---- Consume (F0382 = 0xFF, FE04F[fe] = 1) ----
    e.state = 0xFF; // mov byte_F0382[eax*4],0FFh
    if ((size_t)event_index < 10) battle_search_slots_[event_index] = 0xFF;
    if (e.fe_index < sizeof(battle_fe04f_))
        battle_fe04f_[e.fe_index] = 1; // mov FE04F[eax],1 (written to file by the sub_4E622 block at battle end)
    if (battle_trace_)
        fprintf(stderr, "COMBAT: tile event[%d] consumed unit=%d fe=%u (F0382=FF)\n",
                event_index, u.id, (unsigned)e.fe_index);
}

// sub_1DB1D(killer, victim): the full unit-death sequence.
//   sub_197AD -> HP=0 -> sub_53797(mode 1, the death event; the script may revive or remove the
//   unit right here) -> if it is still in the army list with HP<=0, the real death runs
//   (camera -> animation -> exp/gold -> morale -> list removal) -> sub_53E0F (morale trigger)
//   -> sub_53C12 (region trigger) -> sub_53F94 (army-wiped trigger) when the army list empties.
//   [+12h] >= 0Ah (summons) leaves the list after the animation/exp only and skips morale,
//   summon removal and the wiped trigger.
//   Ordinary units also run sub_1DAEE (clear [+73h] references) and sub_29EE4 (remove summons
//   whose [+12h] == id+10).
void CombatII::play_unit_death(int unit_index, int killer_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (!units_[(size_t)unit_index].active) { units_[(size_t)unit_index].hp = 0; return; }
    const int group = battle_unit_group(units_[(size_t)unit_index]);
    const bool is_summon = units_[(size_t)unit_index].item_use_12 >= 0x0A; // cmp byte [ecx+12h],0Ah

    battle_remove_from_ready_queue(unit_index);                 // sub_197AD
    units_[(size_t)unit_index].hp = 0;                          // mov dword ptr [edx+48h], 0
    run_unit_hp_trigger(unit_index, killer_index, 1);           // sub_53797(victim, killer, 1)
    if (should_exit_) return;

    const CombatUnitPlacement& v = units_[(size_t)unit_index];
    if (v.active && v.hp <= 0) {                                // sub_11FF7 != 0 && hp <= 0
        camera_center_on(v.x, v.y);                             // sub_1124A
        play_unit_death_anim(unit_index);                       // sub_463FB / sub_46B2C / sub_46A11
        if (should_exit_) return;
        // sub_1DB1D loc_1DBC5: exp/gold when a killer exists (summon deaths included).
        if (killer_index >= 0 && (size_t)killer_index < units_.size() && killer_index != unit_index)
            battle_gain_exp(killer_index, unit_index);          // sub_17A20(killer, victim)
        if (should_exit_) return;
        // sub_1DAEE: clear [+73h] on every unit that targets this one. The port models it as a
        // units_ index.
        for (auto& o : units_)
            if (o.ai_target_unit == unit_index) o.ai_target_unit = -1;
        if (is_summon) {
            // sub_1DB1D loc_1DBE3 -> loc_1DDE8: summons are removed from the army list only.
        } else {
            battle_apply_death_morale(units_[(size_t)unit_index]);
            run_morale_trigger();                               // sub_53E0F
            if (should_exit_) return;
            run_region_count_trigger();                         // sub_53C12
            if (should_exit_) return;
            // sub_1DB1D loc_1DCF7: when a [77h] stored record (transform/mount) exists and
            // byte_BA84E (map kind) < 4, sub_1808A restores that record, sub_11F74 re-occupies
            // the current tile, only [60h] = sub_1198D([61h]) is refreshed and the code jumps to
            // loc_1DDAD. The unit is never added to the army/death lists, so it survives in its
            // original form. Map kind >= 4 uses the normal death path.
            const bool revived_from_stored_77 = units_[(size_t)unit_index].stored_77 && sce_map_kind_ < 4;
            if (revived_from_stored_77) {
                battle_restore_stored_1808a(unit_index, false, false);   // sub_1808A
                if ((size_t)unit_index < units_.size()) {
                    units_[(size_t)unit_index].slot_hint =
                        (int)((uint32_t)units_[(size_t)unit_index].sprite_type & 0xFF); // [60h] = 1198D([61h])
                    reset_unit_pose(units_[(size_t)unit_index]);
                }
                battle_wc1_occupancy_preserve_or_close("death revive (sub_11F74)");
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: death revive unit=%d -> class=%u type=%d hp=%d (sub_1DB1D loc_1DCF7)\n",
                            units_[(size_t)unit_index].id, (unsigned)units_[(size_t)unit_index].combat_class,
                            units_[(size_t)unit_index].sprite_type, units_[(size_t)unit_index].hp);
            } else {
            // sub_1DB1D loc_1DD65: clear the dead unit's status/timed flags.
            units_[(size_t)unit_index].combat_flags_1e = 0;     // mov byte [ecx+1Eh],0
            units_[(size_t)unit_index].combat_flags_15 = 0;     // mov [ecx+15h],al
            }
            // sub_29EE4: remove the dead unit's summons ([+12h] == id+0Ah) from the same army list.
            // The original also runs the camera/death animation (sub_463FB) for each summon.
            const uint8_t tag = (uint8_t)((unsigned)units_[(size_t)unit_index].id + 10u);
            const int dead_army = battle_effective_army(units_[(size_t)unit_index]);
            std::vector<int> summons;
            for (size_t i = 0; i < units_.size(); ++i) {
                if ((int)i == unit_index) continue;
                const auto& o = units_[i];
                if (o.active && o.item_use_12 == tag && battle_effective_army(o) == dead_army)
                    summons.push_back((int)i);
            }
            for (int si : summons) {
                if (si < 0 || (size_t)si >= units_.size()) continue;
                battle_remove_from_ready_queue(si);             // sub_197AD
                for (auto& o : units_)
                    if (o.ai_target_unit == si) o.ai_target_unit = -1; // sub_1DAEE
                if (!units_[(size_t)si].active) continue;
                camera_center_on(units_[(size_t)si].x, units_[(size_t)si].y); // sub_1124A
                play_unit_death_anim(si);                       // sub_463FB
                if (should_exit_) return;
            }
        }
    }
    if (!is_summon && group >= 0 && battle_group_alive_count(group) == 0) // cmp dword_CC968[(army-1)*8],0
        run_army_wiped_trigger(group);                       // sub_53F94
}

// sub_46B2C(unit): death animation for large classes 0x86..0x91, following dasm 85171-85366.
//   (1) Copy the inline 12-entry table at 0x44A1C {particle count, BATTLE.SYB back-bank index}
//       to the stack and pick the row for [class-0x86].
//   (2) The anchor is sub_135EB (unit centre) plus the x_off/y_off of the dir0/pose0 frame
//       (byte_D66AC/D66AD). Per particle: vx=rand(4)+2, vy=rand(4)+8, rand(4)==0 adds 4 to vx,
//       rand(4)==2 adds 4 to vy, rand(2)==0 negates vx, gravity 1, and
//       x=anchor+rand(0x18)+record[+4], y=anchor+rand(0x10)+record[+6].
//   (3) sub_1033B(5) -> sub_12124 (pose reset) -> sub_44D89 (highlight list) -> 16 frames of
//       facing rotation (sub_45559 + sub_499E4) -> sub_44D81 -> sub_11EE4 (release occupancy)
//       -> sub_45559 -> sub_1033B(5).
//   (4) 30 frames: sub_454B3 (map/units/weather), then draw only the live particles with
//       sub_5F3FD (screen x-10h < 120h, y-5 < B2h) and advance through sub_5E9E8 + sub_499E4
//       with x+=vx, y-=vy, vy-=gravity.
//   The particle sprite is the RLE of the unk_FA400 record (BATTLE.SYB 0x4C+idx); the record's
//   [+4]/[+6] are added once at creation and never again while drawing.
void CombatII::play_unit_death_46b2c(int unit_index) {
    if (unit_index < 0 || unit_index >= (int)units_.size()) return;
    auto& U = units_[(size_t)unit_index];
    load_effect_sprites();                                  // sub_44AB6 BATTLE.SYB table (loaded lazily in the port)
    // Inline 48 bytes at 0x44A1C (IDA renders the first dword as a link-time symbol).
    static constexpr int kLargeDeath[12][2] = {
        {14, 17}, {14, 18}, {10, 17}, {10, 18}, {16, 17}, {16, 18},
        {12, 17}, {12, 18}, {10, 19}, {10, 20}, {10, 17}, {10, 18},
    };
    const int cls = (int)U.combat_class;
    if (cls < 0x86 || cls > 0x91) return;                 // the caller (sub_1DB1D) already filters this
    const int count = kLargeDeath[cls - 0x86][0];
    const int bank_idx = 0x4C + kLargeDeath[cls - 0x86][1]; // unk_FA400 = BATTLE.SYB 0x4C+
    const TerrainOverlayFrame* spr =
        (bank_idx >= 0 && (size_t)bank_idx < battle_effects_.size() && !battle_effects_[(size_t)bank_idx].rle.empty())
            ? &battle_effects_[(size_t)bank_idx] : nullptr;
    const int rec_xo = spr ? spr->x : 0;                   // unk_FA404 (record +4)
    const int rec_yo = spr ? spr->y : 0;                   // unk_FA406 (record +6)

    int base_x = 0, base_y = 0;
    unit_screen_center(U, base_x, base_y);                 // sub_135EB
    if (const NC2Frame* f0 = nc2_frame_for(U.sprite_type, 0, 0)) { // byte_D66AC/AD[slot*572]
        base_x += f0->x_off;
        base_y += f0->y_off;
    }

    // word_F8D88/F8B88/F8F88/F8328/F8528 are all 16-bit, so every update wraps at 16 bits.
    struct LargeParticle { uint16_t x = 0, y = 0, vx = 0, vy = 0, g = 1; };
    std::vector<LargeParticle> ps((size_t)count);
    for (LargeParticle& p : ps) {
        p.vx = (uint16_t)(combat_rand(4) + 2);
        p.vy = (uint16_t)(combat_rand(4) + 8);
        const int r = combat_rand(4);
        if (r == 0) p.vx = (uint16_t)(p.vx + 4);
        else if (r == 2) p.vy = (uint16_t)(p.vy + 4);
        if (combat_rand(2) == 0) p.vx = (uint16_t)(-(int)(int16_t)p.vx);
        p.g = 1;                                            // word_F8F88 = 1
        p.x = (uint16_t)(base_x + combat_rand(0x18) + rec_xo);
        p.y = (uint16_t)(base_y + combat_rand(0x10) + rec_yo);
    }

    game_timer_set(5);                                      // sub_1033B(5)
    reset_unit_pose(U);                                     // sub_12124 (ecx=4 is unused)
    battle_highlight_add(unit_index);                       // sub_44D89
    for (int b = 0; b < 0x10 && !should_exit_ && !skip_cutscene_; ++b) {
        U.facing = (int8_t)((U.facing + 1) & 3);            // 16-frame rotation
        present_cutscene_frame();                           // sub_45559
        wait_game_tick();                                   // sub_499E4
    }
    battle_highlight_pop();                                 // sub_44D81
    U.active = false;                                       // sub_11EE4
    battle_wc1_occupancy_preserve_or_close("unit death/removal");
    present_cutscene_frame();                               // sub_45559
    game_timer_set(5);                                      // sub_1033B(5)
    for (int step = 0; step < 0x1E && !should_exit_ && !skip_cutscene_; ++step) {
        battle_fx_world();                                  // sub_454B3
        if (spr) {
            for (const LargeParticle& p : ps) {
                if ((uint16_t)(p.x - 0x10) >= 0x120) continue;   // cmp dx,120h / jnb
                if ((uint16_t)(p.y - 5) >= 0xB2) continue;       // cmp dx,0B2h / jnb
                blit_sprite_480((int16_t)p.x, (int16_t)p.y, spr->rle.data()); // sub_5F3FD
            }
        }
        battle_fx_commit();                                 // sub_5E9E8 (+454D5 overlay)
        wait_game_tick();                                   // sub_499E4
        for (LargeParticle& p : ps) {
            p.x = (uint16_t)(p.x + p.vx);
            p.y = (uint16_t)(p.y - p.vy);
            p.vy = (uint16_t)(p.vy - p.g);
        }
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 46B2C large death unit=%d class=%02X particles=%d frames=%d\n",
                U.id, (unsigned)cls, count, 0x1E);
}

// ==== sub_46710 / sub_468BC / sub_46A11: removal effects ====
// All three build a W x H strip from the (facing, pose 0) D66A8 entry with sub_10083. The port
// uses the decoded NC2 pixels; 0xFF is the transparent value the original strip carries.

// sub_46A11 (dasm 35195..35390): 16-frame pixel dissolve used by large-unit deaths and by
// sub_50AA7 mode 4. The caller has already released the occupancy (sub_11EE4), so the unit is
// drawn only through this strip. Per frame it erases (int16)(W*H-2)/12 pixels at
// rand(|(int16)(W*H-2)|) (sub_499BD) and redraws through sub_5EE51.
void CombatII::battle_fx_46a11(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& U = units_[(size_t)unit_index];
    const NC2Frame* f0 = nc2_frame_for(U.sprite_type, U.facing & 3, 0);
    if (!f0 || f0->pixels.empty()) return;
    const int strip_w = f0->w, strip_h = f0->h;
    const long total = (long)strip_w * (long)strip_h;
    const int span = (int)(int16_t)(total - 2);          // 10083 returns bytes+2, caller -4
    const int per_frame = span / 12;                     // idiv 0Ch
    const int erase_range = span < 0 ? -span : span;     // sub_499BD |(int16)|
    U.death_dissolve = 1;
    U.death_w = strip_w;
    U.death_h = strip_h;
    U.death_mask.assign((size_t)total, 0);
    game_timer_set(9);                                   // sub_1033B(9)
    for (int i = 0; i < 16 && !should_exit_; ++i) {
        for (int k = 0; k < per_frame; ++k) {
            if (erase_range <= 0) break;
            const size_t idx = (size_t)combat_rand(erase_range); // sub_499BD
            if (idx < U.death_mask.size()) U.death_mask[idx] = 1; // unk_F1BAC[idx] = 0FFh
        }
        battle_fx_world();                               // sub_454B3 (5EE51 draws the strip)
        video_.present();                                // sub_5E9E8
        wait_game_tick();                                // sub_499E4
    }
    U.death_dissolve = 0;
    U.death_mask.clear();
    U.death_w = U.death_h = 0;
    present_cutscene_frame();                            // sub_45559
}

// sub_46710 (dasm 34672..34895): 8-pass wipe. sub_10083 builds the strip (W = entry[2],
// H = entry[3]); the unit leaves the occupancy grid; then, for each of eight bands of
// edi = (int16)(bytes+2)/8 pixels, it erases `divisor + edi` random pixels (divisor =
// max(edi/2, 2)), blanks the band, and redraws the strip through sub_5EE51. Drawing happens
// every `divisor`-th erasure as well. The strip is drawn at sub_10971+0Ch+entry[4] /
// sub_10988-2+entry[5], which is exactly the renderer's UNIT_ANCHOR position plus the NC2
// x_off/y_off, so the port reuses death_dissolve's draw path.
void CombatII::battle_fx_46710(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& U = units_[(size_t)unit_index];
    const NC2Frame* f0 = nc2_frame_for(U.sprite_type, U.facing & 3, 0);
    if (!f0 || f0->pixels.empty()) return;
    const int w = f0->w, h = f0->h;
    const long total = (long)w * (long)h;
    const int ret = (int)(int16_t)(total + 2);           // sub_10083 return value
    const int band = ret / 8;                            // movsx/sar 3 (truncates toward 0)
    int divisor = band / 2;                              // movsx/sar 1
    if (divisor < 2) divisor = 2;                        // cmp ax,2; jge
    const int erasures = divisor + band;                 // var_C
    U.active = false;                                    // sub_11EE4
    U.death_dissolve = 1;
    U.death_w = w;
    U.death_h = h;
    U.death_mask.assign((size_t)total, 0);
    game_timer_set(5);                                   // sub_1033B(5)
    int si = 0;
    for (int pass = 0; pass < 8 && !should_exit_; ++pass) {
        for (int ecx = erasures; ecx >= 0 && !should_exit_; --ecx) {
            const int idx = si + (band > 0 ? combat_rand(band) : 0); // sub_499BD(edi)
            if (idx >= 0 && (size_t)idx < U.death_mask.size()) U.death_mask[idx] = 1;
            // mov ebx,[esp+2] >> 16 -> divisor; idiv + test edx -> draw every divisor-th step
            if (divisor > 0 && (ecx % divisor) == 0) {
                battle_fx_world();                       // sub_454B3
                video_.present();                        // sub_5E9E8
                wait_game_tick();                        // sub_499E4
            }
        }
        for (int k = 0; k < band; ++k) {                 // blank the band
            const int idx = si + k;
            if (idx >= 0 && (size_t)idx < U.death_mask.size()) U.death_mask[idx] = 1;
        }
        battle_fx_world();                               // sub_454B3 + sub_5EE51
        video_.present();
        wait_game_tick();
        si += band;
    }
    U.death_dissolve = 0;
    U.death_mask.clear();
    U.death_w = U.death_h = 0;
    present_cutscene_frame();                            // sub_45559
}

// sub_468BC (dasm 35058..35217): horizontal shrink wipe (sub_50AA7 mode 3).
//   x = 10971+0Ch+entry[4], y = 10988-2+entry[5]; 11EE4 releases the occupancy.
//   W1 = W - W%2 + 1; for si = 1,3,5,... < W1+4, with di = 1,2,4,... after si > 4:
//     scale += 2 while si < W1; y -= di; then 5F611(x + scale/2, y, strip, scale, 1).
//   dst_h is 1, so 5F611 performs no vertical shrink and draws the full strip height.
void CombatII::battle_fx_468BC(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& U = units_[(size_t)unit_index];
    const NC2Frame* f0 = nc2_frame_for(U.sprite_type, U.facing & 3, 0);
    if (!f0 || f0->pixels.empty()) return;
    const int W = f0->w, H = f0->h;
    if (W <= 0 || H <= 0) return;
    const int sx = ((int)(int8_t)U.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + f0->x_off;
    const int sy = ((int)(int8_t)U.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y + f0->y_off;
    // Build the sub_10083 strip layout ([w][h] + pixels) for sub_5F611.
    std::vector<uint8_t> strip((size_t)4 + (size_t)W * (size_t)H);
    strip[0] = (uint8_t)(W & 0xFF); strip[1] = (uint8_t)(W >> 8);
    strip[2] = (uint8_t)(H & 0xFF); strip[3] = (uint8_t)(H >> 8);
    std::memcpy(strip.data() + 4, f0->pixels.data(), (size_t)W * (size_t)H);
    U.active = false;                                    // sub_11EE4
    const int W1 = W - (W & 1) + 1;                      // idiv 2; sub remainder; inc
    game_timer_set(9);                                   // sub_1033B(9)
    int scale = 1;                                       // [esp+10h]
    int si = 1;
    int di = 1;
    int y = sy;                                          // [esp+8]
    const int limit = W1 + 4;                            // [esp]
    while (si < limit && !should_exit_) {
        battle_fx_world();                               // sub_454B3
        if (si < W1) scale += 2;
        if (si > 4) { y -= di; di += di; }
        if (y > -0x64)                                   // cmp ebp,-64h; jle skip
            blit_scale_shrink(sx + scale / 2, y, strip.data(), scale, 1); // sub_5F611
        video_.present();                                // sub_5E9E8
        wait_game_tick();                                // sub_499E4
        si += 2;
    }
    present_cutscene_frame();                            // sub_45559
}

// Presentation only: inside sub_1DB1D(0, U), class 0 uses sub_463FB (16-frame rotation plus
// 30 frames of sprite-pixel debris), classes 0x86..0x91 use sub_46B2C and other large units
// use sub_46A11. Tile occupancy release (sub_11EE4), army-list removal and the morale
// (dword_F0BA0) adjustment are covered by removing the unit.
void CombatII::play_unit_death_anim(int unit_index) {
    if (unit_index < 0 || unit_index >= (int)units_.size()) return;
    auto& U = units_[(size_t)unit_index];
    U.hp = 0;
    if (!U.active) return;
    if (U.sprite_type >= 0x12C) {
        // sub_1DB1D loc_1DBA8: among large units, classes 0x86..0x91 use sub_46B2C.
        if (U.combat_class >= 0x86 && U.combat_class <= 0x91) {
            play_unit_death_46b2c(unit_index);
            return;
        }
        // sub_46A11 (large units outside 0x86..0x91), using the D66A8 entry (facing, POSE 0):
        //   (1) sub_10083 builds a W x H strip in unk_F1BA8 (0xFF transparent),
        //   (2) sub_11EE4 clears the occupancy, then
        //   (3) for 16 frames it erases `(int16)(W*H-2)/12` pixels per frame at
        //       `rand % (int16)(W*H-2)` by writing 0xFF and redraws with sub_454B3 + sub_5EE51.
        // sub_10083 is not palette work but a "span list -> row-major strip" expander (header
        // [0]=W, [2]=H, then W*H pixels, 0xFF=transparent). sub_11575 already patched each span's
        // x_delta through sub_10031 at load time (+0xA0 past 0x140), so combined with the 0x1E0
        // wrap in sub_10083 it matches the nc2_parser.h decode (0x140 wrap, fill=cursor). The port
        // therefore uses the NC2 pixels directly as the strip and keeps only a per-pixel erase
        // mask.
        const NC2Frame* f0 = nc2_frame_for(U.sprite_type, U.facing & 3, 0);
        // The original builds the strip with sub_10083 before calling sub_11EE4, so the strip
        // size is fixed here first. f0 is read only at this point because the occupancy-release
        // path may touch the NC2 cache.
        const int strip_w = f0 ? f0->w : 0;
        const int strip_h = f0 ? f0->h : 0;
        const bool have_strip = f0 && !f0->pixels.empty();
        U.active = false;                                   // sub_11EE4
        battle_wc1_occupancy_preserve_or_close("unit death/removal");
        if (!have_strip) {
            present_cutscene_frame();
            return;
        }
        const long total = (long)strip_w * (long)strip_h;
        // sub_10083 returns (bytes written + 2) and the caller subtracts 4, so (int16)(W*H-2)
        // is both the erase count and the index range; both truncate to 16 bits as in the original.
        const int span = (int)(int16_t)(total - 2);
        const int per_frame = span / 12;                    // idiv 0Ch (truncates toward 0)
        const int erase_range = span < 0 ? -span : span;    // sub_499BD: |(int16)| range
        U.death_dissolve = 1;
        U.death_w = strip_w;
        U.death_h = strip_h;
        U.death_mask.assign((size_t)total, 0);
        game_timer_set(9);                                  // sub_1033B(9)
        for (int i = 0; i < 16 && !should_exit_; ++i) {
            for (int k = 0; k < per_frame; ++k) {
                combat_rng_ = combat_rng_ * 1103515245u + 12345u;   // sub_499BD
                const size_t idx =
                    (size_t)((combat_rng_ >> 16) % (uint32_t)erase_range); // movsx eax,cx
                if (idx < U.death_mask.size()) U.death_mask[idx] = 1;       // unk_F1BAC[idx] = 0FFh
            }
            present_cutscene_frame();                       // sub_454B3 + sub_5EE51 + sub_5E9E8
            wait_game_tick();                               // sub_499E4
        }
        U.death_dissolve = 0;
        U.death_mask.clear();
        U.death_w = U.death_h = 0;
        if (script_trace_)
            fprintf(stderr, "COMBAT: 46A11 dissolve done unit=%d strip=%dx%d per_frame=%d\n",
                    U.id, strip_w, strip_h, per_frame);
        present_cutscene_frame();                           // sub_45559
        return;
    }
    // ---- sub_463FB ----
    const NC2Frame* f0 = nc2_frame_for(U.sprite_type, 0, 0);
    // sub_10194(unk_F8A88, dword_D66B1[slot*572] = the facing-0/pose-0 sprite) decodes the RLE,
    // counts the PIXELS and stores each decoded pixel value in order in unk_F8A88. When the total
    // would pass 0x100 it rewinds that run and stops (hence the 255 cap). The returned di is the
    // particle count, and particle i's colour is unk_F8A88[i] (loc_465CD: `mov al,
    // unk_F8A88[ecx]`).
    // The `cmp ax,100h / jg loc_46708` particle-skip branch is unreachable because of that clamp.
    std::vector<uint8_t> colors;
    if (f0) {
        for (uint8_t c : f0->pixels) {
            if (c == 0xFF) continue;              // 0xFF = transparent in the decoded strip
            if (colors.size() >= 0xFF) break;     // the sub_10194 0x100 accumulation clamp
            colors.push_back(c);
        }
    }
    const bool particles = !colors.empty();
    fprintf(stderr, "COMBAT: play death: unit %d (type %d) particles=%zu\n",
            U.id, U.sprite_type, colors.size());
    struct P { int x, y, vx, vy, g; uint8_t bg; };
    std::vector<P> ps;
    if (particles) {
        int cx, cy; unit_screen_center(U, cx, cy);        // sub_135EB
        cx += f0->x_off; cy += f0->y_off;                  // byte_D66AC/D66AD[slot*572]
        ps.resize(colors.size());
        for (size_t i = 0; i < ps.size(); i++) {
            P& p = ps[i];
            p.vx = combat_rand(6); p.vy = combat_rand(12);
            const int r = combat_rand(4);
            if (r == 0) p.vx += 4; else if (r == 2) p.vy += 8;
            if (combat_rand(2) == 0) { p.vx = -p.vx; p.g = 1; }   // loc_464DE sets the sign flip and
            else p.g = 0;                                          // gravity 1 in the same branch
            p.x = combat_rand(0x18) + cx; p.y = combat_rand(0x18) + cy;
            p.bg = 0;
        }
    }
    game_timer_set(5);
    reset_unit_pose(U);                                    // sub_12124
    // G2_DUMP_DEATH=<path>: dump the first rotation frame, the first debris frame and the final
    // debris frame as PGM for verification.
    const char* dump_death = getenv("G2_DUMP_DEATH");
    auto dump_death_frame = [&](const char* tag) {
        if (!dump_death || !*dump_death) return;
        uint8_t* buf = video_.get_backbuffer();
        if (!buf) return;
        char name[512];
        snprintf(name, sizeof(name), "%s.%s.pgm", dump_death, tag);
        if (!DevUtil::dump_pgm_gray(name, buf, SCREEN_W, SCREEN_H)) return;
        fprintf(stderr, "COMBAT: death dump %s -> %s\n", tag, name);
    };
    // sub_44D89(U) registers the unit in the dword_F9F08 highlight list, so sub_44DCC in sub_454D5
    // redraws the rotating unit every frame (above spell effects and dimming, below the HUD).
    // sub_44D81 removes it after the loop.
    battle_highlight_add(unit_index);
    for (int b = 0; b < 0x10 && !should_exit_ && !skip_cutscene_; b++) {
        U.facing = (int8_t)((U.facing + 1) & 3);           // rotation
        present_cutscene_frame();                          // sub_45559
        wait_game_tick();
        if (b == 0x0C) sfx(0x1B);
        if (b == 0) dump_death_frame("rot");
    }
    // sub_44D81 + sub_11EE4(x,y,U): leave the highlight list and release the tile occupancy, so
    // the unit is no longer drawn.
    battle_highlight_pop();
    U.active = false;
    battle_wc1_occupancy_preserve_or_close("unit death/removal");
    present_cutscene_frame();
    if (!particles) return;
    game_timer_set(3);
    for (int step = 0; step < 0x1E && !should_exit_ && !skip_cutscene_; step++) {
        render_battle_frame();
        uint8_t* bb = video_.get_backbuffer();
        int drawn = 0;
        for (size_t i = 0; i < ps.size(); i++) {
            P& p = ps[i];
            if (p.x - 0x10 < 0 || p.x - 0x10 >= 0x120 || p.y - 5 < 0 || p.y - 5 >= 0xB2) continue;
            bb[p.y * SCREEN_W + p.x] = colors[i];          // sub_5EB1F(x, y, unk_F8A88[i])
            drawn++;
        }
        if (step == 0) {
            fprintf(stderr, "COMBAT: death particles on screen: %d/%zu\n", drawn, ps.size());
            dump_death_frame("parts");
        }
        render_overlay();
        video_.present();
        wait_game_tick();
        for (P& p : ps) { p.x += p.vx; p.y -= p.vy; p.vy -= p.g; }
    }
    present_cutscene_frame();
    dump_death_frame("after");
}

// ==== Battle tick engine: sub_19652 / sub_19D53 / sub_19F5D / sub_197CA ====

// sub_19652: reset the battle state. sub_4FD25 calls it BEFORE loading the scenario
// (sub_4F85C).
//   loc_19658 empties the dword_F041C list through _nfree_ (the player wait queue).
//   dword_F0414 = 0 (queue tail)   dword_F0418 = 0 (battle time)   dword_74134 = -1 (sequence trigger off)
//   byte_74138 = 0x0A (turn length) word_F0424 = 0                 dword_F0410 = 0
//   word_F0420 = 0                  word ptr dword_F0426 = 0        word_F0422 = 0
//   byte_F042E = 0
// dword_75008 (overlay), word_789A4 (pause cooldown) and byte_FDF84/word_FDF82 (clock) are
// left untouched.
void CombatII::battle_reset() {
    battle_load_item_table();   // sub_18115 (once before battle entry)
    battle_ready_queue_.clear();
    battle_attack_cancel();
    battle_move_cancel();
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    battle_magic_pending_r_ = -1;
    battle_item_pending_dx_ = -1;
    // sub_18539 (F0382 clear) runs when the scenario loads (sub_4ED18), so battle_reset
    // (sub_19652) leaves it alone; battle_tile_load_events() fills tile events from
    // sce_raw_. Only the cursor (F040E) is reset - slots/events stay with the loader.
    battle_search_cursor_ = 0;
    battle_player_diffusion_.clear();
    battle_player_diffusion_valid_ = false;
    battle_range_e8_.clear();
    battle_ai_gauge_baseline_ = 0;
    battle_hostility_inverted_ = false;
    for (CombatUnitPlacement& u : units_) u.battle_gauge = 0;
    battle_time_ = 0;
    battle_hud_time_ = 0;      // bottom time gauge value (sub_44EB0)
    script_seq_ = -1;
    hud_time_divisor_ = 0x0A;
    battle_scan_group_ = 0;
    battle_scan_unit_ = -1;
    battle_scan_next_key_ = ~0ull;
    battle_list_seq_counter_ = 0;
    for (CombatUnitPlacement& lu : units_) lu.list_seq = 0;
    battle_ai_acted_ = 0;
    battle_scan_active_ = 0;
    battle_state_ = 0;
    battle_free_wait_ = false;
    battle_free_hover_ = -1;
    battle_free_cycle_ = 0;
    battle_simple_ring_ = false;
    battle_move_var18_ = 10;
    battle_player_unit_ = -1;
    battle_used_bits_ = 0;
    battle_var24_action_end_ = false;
    // sub_4FE74 (55B59+3A, battle init): reads the four game.cfg bytes in order:
    //   read(1) -> word_FDAEA (instant camera) -> sub_10D9A(FDAEA != 0 ? 8 : 4)
    //   read(1) -> word_FDAE8 (music), read(1) -> word_FDAE6 (sound effects)
    //   read(1) -> speed (low byte of dword var_10), clamped to 2..0x1E by
    //   `cmp bx,2 jl -> 2` / `cmp bx,1Eh jle keep`, then sub_1A116 -> byte_74138.
    // byte_74138 is both the turn period ((time+1) % byte_74138) and the divisor of the
    // bottom time gauge (sub_44EB0). The shipped game.cfg is all zero, so the original
    // clamps to 2: the gauge moves between 0 and 1 pip and a turn comes every 2 ticks.
    {
        battle_instant_camera_ = false;
        int cfg_speed = 0;
        std::string cfg = game_path("game.cfg");
        if (!file_exists(cfg.c_str())) cfg = game_path("GAME.CFG");
        if (FILE* fp = cfg.empty() ? nullptr : fopen(cfg.c_str(), "rb")) {
            uint8_t b[4] = {};
            const size_t got = fread(b, 1, sizeof(b), fp);
            // Each field is a 1-byte read (a word read of the first two bytes misreads FDAEA).
            if (got >= 1) battle_instant_camera_ = b[0] != 0;                 // word_FDAEA
            // word_FDAE8 (music) / word_FDAE6 (sfx) keep the system-menu state: the
            // original assigns the cfg values here, but sub_4FF8E (game.cfg write) is not
            // ported, so applying them would reset the user's menu toggles every battle.
            if (got >= 4) cfg_speed = b[3];                                  // speed
            fclose(fp);
        }
        if (std::getenv("G2_INSTANT_CAMERA") != nullptr) battle_instant_camera_ = true;
        battle_scroll_speed_ = battle_instant_camera_ ? 8 : 4; // sub_10D9A(ax)
        if (cfg_speed < 2 || cfg_speed > 0x1E) cfg_speed = 2;   // sub_4FE74 loc_4FEF9 clamp
        hud_time_divisor_ = cfg_speed;                          // sub_1A116 -> byte_74138
        battle_turn_interval_ = cfg_speed;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: reset (sub_19652) time=0 seq=-1 divisor=%d state=0 instant_cam=%d\n",
                (int)hud_time_divisor_, battle_instant_camera_ ? 1 : 0);
}

// sub_18115: reads the first 0x1FF8 bytes of ITEM.DAT into unk_EDD88 (124 records of
// 66 bytes = the 0x7C entries of item.spr). The table is not stored in COMBATII.EXE
// (object 2 BSS), so it is read at runtime. Without the file the table stays empty and
// battle_lookup_184a4 returns 0 (no equipment bonus).
void CombatII::battle_load_item_table() {
    if (battle_item_table_tried_) return;
    battle_item_table_tried_ = true;
    constexpr size_t kBytes = 0x1FF8;
    std::string path = game_path("ITEM.DAT");
    if (!file_exists(path.c_str())) path = game_path("item.dat");
    if (!file_exists(path.c_str())) path = file_find_case_insensitive(GAME_DATA_DIR, "ITEM.DAT");
    FILE* fp = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "COMBAT: warning: ITEM.DAT not found - continuing without equipment bonuses (unk_EDD88)\n");
        return;
    }
    battle_lookup_table_.assign(kBytes, 0);
    const size_t got = fread(battle_lookup_table_.data(), 1, kBytes, fp);
    fclose(fp);
    fprintf(stderr, "COMBAT: ITEM.DAT loaded (%zu bytes, %zu records)\n", got, got / 66);
}

// SCE/VM classification needs the original's effective [+0x0E]. Outer groups 1..3 and
// non-zero Spawn values are kept in memory_army; group-0/raw-zero remaps and the VM
// Faction path fall back to raw+1, so equality with the original is not guaranteed there.
bool CombatII::battle_unit_is_player(const CombatUnitPlacement& u) const {
    return battle_effective_army(u) == 1 && u.auto_battle == 0 && (u.combat_flags_1e & 0x10) == 0;
}

bool CombatII::battle_unit_is_ai_controlled(const CombatUnitPlacement& u) const {
    // ASM uses effective memory army>1, auto byte!=current scheduler BL, or [+0x1E]&0x10.
    // The port uses modeled effective army and retains its coarse auto-byte test; unknown remaps
    // fall back to raw+1.
    return battle_effective_army(u) > 1 || u.auto_battle != 0 || (u.combat_flags_1e & 0x10) != 0;
}

// sub_19892: buff/ailment duration timer. When [+15h] != 0 it walks the +16h/+18h/+1Ah/
// +1Ch words, and when [+1Eh] != 0 the five words +1Fh..+27h. A word that reaches 0
// clears the matching bit (unk_74139 for +15h, unk_7413D for +1Eh) and becomes 0xFFFF;
// a positive value is decremented. When the barrier ([+1Eh]&40h) expires, the original
// restores the [+60h] image slot from the [+5Fh] high word (character type); the port
// keys its NC2 cache by type, so it only restores slot_hint.
void CombatII::battle_status_timer_19892(CombatUnitPlacement& u) {
    if (u.combat_flags_15 != 0) {
        uint16_t* words[4] = { &u.buff_16, &u.buff_18, &u.buff_1a, &u.buff_1c };
        for (int i = 0; i < 4; ++i) {
            uint16_t v = *words[i];
            if (v == 0) {
                u.combat_flags_15 = (uint8_t)(u.combat_flags_15 & kStatusBuffClearMask[i]);
                *words[i] = 0xFFFF;
            } else if ((int16_t)v > 0) {
                *words[i] = (uint16_t)(v - 1);
            }
        }
    }
    if (u.combat_flags_1e != 0) {
        uint16_t* words[5] = { &u.ail_1f, &u.ail_21, &u.ail_23, &u.ail_25, &u.ail_27 };
        for (int i = 0; i < 5; ++i) {
            uint16_t v = *words[i];
            if (v == 0) {
                if (u.combat_flags_1e & 0x40) {
                    // sub_1198D([+5Fh]>>16): restore the original image slot. The port keys
                    // its NC2 cache by type, so only the slot number returns to that type.
                    u.slot_hint = (int)((uint32_t)u.sprite_type & 0xFF);
                }
                u.combat_flags_1e = (uint8_t)(u.combat_flags_1e & kStatusAilClearMask[i]);
                *words[i] = 0xFFFF;
            } else if ((int16_t)v > 0) {
                *words[i] = (uint16_t)(v - 1);
            }
        }
    }
}

// sub_1992D: ATB/damage handling for poison and slow. Returns 1 to add the hit to the
// on-screen damage list (F0648).
//   [+25h] % 10 == 0 with [+1Eh]&20h (poison) re-rolls the status with sub_499BD(6).
//   [+1Eh]&2 (sleep/paralysis) forces [+56h]=0 every tick; otherwise &4 with [+1Fh]&1
//   and 0<[+56h]<FFh decrements it by 1. [+56h] != FFh (not ready) returns -1.
//   When ready, &1/&4 damage is (100-[+59h])*1500/100 times the sub_1762C adjustment,
//   and &80h adds 800. Damage is subtracted from [+48h] at once and, if inside the
//   viewport, queued for the damage numbers (deaths go through sub_1ED90).
int CombatII::battle_status_ailment_1992d(int unit_index, int slot, int* out_damage) {
    if (out_damage) *out_damage = 0;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    // [+23h]>>16 = [+25h] (high word of the dword at +23h)
    if ((u.ail_25 % 10) == 0 && (u.combat_flags_1e & 0x20)) {
        const int r = combat_rand(6);
        if (r < 3) {
            u.ail_1f = 0x32;
            u.combat_flags_1e = (uint8_t)(u.combat_flags_1e & 0xF8);
        } else if (r == 5) {
            u.ail_27 = 0x32;
        } else {
            // [+1Bh + ax*2]: ax=3 -> +21h, ax=4 -> +23h
            if (r == 3) u.ail_21 = 0x32; else u.ail_23 = 0x32;
        }
        u.combat_flags_1e = (uint8_t)(u.combat_flags_1e | kStatusAilReapplyMask[r & 7]);
    }
    if (u.combat_flags_1e & 0x02) {
        u.battle_gauge = 0;                              // sleep/paralysis: gauge 0 every tick
    } else if ((u.combat_flags_1e & 0x04) && (u.ail_1f & 0x01) &&
               u.battle_gauge > 0 && u.battle_gauge < 0xFF) {
        --u.battle_gauge;                                // slow: gauge decremented by 1
    }
    if (u.battle_gauge != 0xFF) return -1;               // not ready to act

    int damage_class = -1;
    int damage = 0;
    if (u.combat_flags_1e & 0x01) {
        damage_class = 0;
        damage = (100 - (int)u.combat_def59) * 0x5DC / 100;
    } else if (u.combat_flags_1e & 0x04) {
        damage_class = 2;
        damage = (100 - (int)u.combat_def59) * 0x5DC / 100;
    }
    if (damage_class != -1) {
        const int adjust = battle_damage_adjustment(u, damage_class); // sub_1762C
        damage = damage * adjust / 10;
    }
    if (u.combat_flags_1e & 0x80) damage += 0x320;       // curse: extra damage
    if (damage == 0) return -1;

    const int old_raw = battle_hp_raw(u);
    const int new_raw = old_raw - damage;
    if (new_raw <= 0) { u.hp = 0; u.hp_deficit = 0; }
    else {
        const int new_hp = (new_raw + 99) / 100;
        u.hp = new_hp;
        u.hp_deficit = new_hp * 100 - new_raw;
    }
    if (out_damage) *out_damage = damage;
    // ASM 14282..14296: `add eax,0Dh; cmp edx,eax; jg` -> x,y <= cam+13 (14 tiles, both ends
    // included).
    const bool inside = (u.x >= cam_tile_x_ && u.x <= cam_tile_x_ + 13 &&
                         u.y >= cam_tile_y_ && u.y <= cam_tile_y_ + 13);
    if (battle_trace_) {
        fprintf(stderr, "COMBAT: status damage unit=%d id=%d class=%d raw %d->%d dmg=%d slot=%d%s\n",
                unit_index, u.id, damage_class, old_raw, new_raw, damage, slot,
                inside ? "" : " (outside viewport)");
    }
    if (!inside) return -1;
    // sub_45ED2: hit direction comes from [+63h] (facing).
    setup_knockback_hit(u, u.facing & 3);
    return 1;
}

// sub_19AEF second half: when the F0648 damage list is non-empty, plays the 25-frame
// damage number/shake animation and then lets sub_1ED90 kill units with HP <= 0.
// The damage itself was already applied by sub_1992D.
void CombatII::battle_status_damage_anim(const std::vector<std::pair<int,int>>& targets) {
    if (targets.empty()) return;
    if (!small_font_tried_) load_small_font();
    static constexpr uint8_t kDmgColor[8] = {0x04, 0x10, 0x31, 0x51, 0x20, 0x01, 0x00, 0x00};
    struct Popup {
        int idx;
        int x0, y0;
        char digits[8];
        int ndig;
        int digit_y[5];
        uint8_t color;
    };
    std::vector<Popup> pops;
    for (const auto& t : targets) {
        const int ti = t.first;
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        const auto& u = units_[(size_t)ti];
        Popup p{};
        p.idx = ti;
        // sub_45BFB draws [48h] x100 damage in displayed HP units (rounded up).
        const int shown = std::max(1, (t.second + 99) / 100);
        p.x0 = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        p.y0 = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 10;
        snprintf(p.digits, sizeof(p.digits), "%d", shown);
        p.ndig = (int)strlen(p.digits);
        for (int j = 0; j < 5; ++j) p.digit_y[j] = p.y0;
        const int army = std::min(std::max(battle_effective_army(u), 0), 7);
        p.color = kDmgColor[(size_t)army];
        pops.push_back(p);
    }
    game_timer_set(3);
    for (int f = 0; f < 0x19 && !should_exit_ && !skip_cutscene_; ++f) {
        for (auto& p : pops) {
            if ((size_t)p.idx >= units_.size()) continue;
            auto& u = units_[(size_t)p.idx];
            apply_knockback_hit(u, 0, f);                 // sub_45F29
            if (f < 5) for (int j = 0; j < 5; ++j) p.digit_y[j] -= 2;
            else if (f >= 0x14) for (int j = 0; j < 5; ++j) p.digit_y[j] += 2;
            else { const int j = (f - 5) / 5; if (j < 5) p.digit_y[j] += 2; }
        }
        render_battle_frame();
        uint8_t* bb = video_.get_backbuffer();
        for (const auto& p : pops) {
            if (p.x0 - 16 < 0 || p.x0 - 16 >= 0x120) continue;
            char one[2] = { p.digits[0], 0 };
            const int y = p.digit_y[0];
            if (y - 5 < 0 || y - 5 >= 0xB2) continue;
            if (small_font_loaded_) {
                draw_small_text(p.x0 + 1, y + 1, one, 0, 1);
                draw_small_text(p.x0, y, one, p.color, 1);
            } else if (bb && font_.is_loaded()) {
                font_.set_text_mode(4, 0x0000, 0xFF);
                font_.draw_string(bb, SCREEN_W, SCREEN_H, p.x0 + 1, y + 1, one);
                font_.set_text_mode(4, (uint16_t)(p.color << 8), 0xFF);
                font_.draw_string(bb, SCREEN_W, SCREEN_H, p.x0, y, one);
            }
        }
        render_overlay();
        video_.present();
        wait_game_tick();
    }
    for (const auto& t : targets) {
        if (t.first < 0 || (size_t)t.first >= units_.size()) continue;
        reset_unit_pose(units_[(size_t)t.first]);            // sub_12124
    }
    // sub_1ED90: per army list, HP <= 0 calls sub_1DB1D (killer 0), else sub_53797 (mode 1).
    for (const auto& t : targets) {
        if (should_exit_) return;
        const int ti = t.first;
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& u = units_[(size_t)ti];
        if (!u.active) continue;
        if (battle_hp_raw(u) <= 0) play_unit_death(ti, -1);
        else run_unit_hp_trigger(ti, -1, 1);
    }
}

// ---- sub_1767E / 176F1 / 1796B / 184F9 / 17A20: level-up and kill rewards ----
// sub_1767E(unit, stat): growth test. For stat 0..3 it returns 1 when the level is a
// multiple of the table value; stat 4/5 returns 15-3*(value-1); stat >= 6 returns 0.
int CombatII::battle_growth_1767e(const CombatUnitPlacement& u, int stat) const {
    if (u.combat_class >= 0x3F) return 1;                    // cmp [eax+11h],3Fh; jb
    if (stat < 0 || stat > 9) return 0;
    const int cls = (int)u.combat_class;
    if (cls >= (int)(sizeof(kLevelGrowthTable) / sizeof(kLevelGrowthTable[0]))) return 0;
    const int val = (int)kLevelGrowthTable[cls][stat];
    if (val == 0) return 0;                                  // test ax,ax; jz loc_176DA
    if (stat > 3) return 15 - 3 * (val - 1);                 // dec eax; imul 3; 0Fh-
    const int level = (int)u.combat_level_2b;
    return (level % val) == 0 ? 1 : 0;                       // idiv; test edx,edx; jz 1
}

// sub_184F9(unit, column): signed-byte ITEM.DAT column sum over the three equipment
// slots ([5Bh]/[5Ah]/[5Dh]).
int CombatII::battle_item_col_sum(const CombatUnitPlacement& u, int column) const {
    const int b = (int)(int8_t)battle_lookup_184a4((int)u.combat_type_5b, column);
    const int a = (int)(int8_t)battle_lookup_184a4((int)u.combat_type_5a, column);
    const int d = (int)(int8_t)battle_lookup_184a4((int)u.combat_type_5d, column);
    return d + b + a;                                        // ASM: [5Bh]+[5Ah]+[5Dh]
}

// sub_176F1(unit, stat): weapon/magic growth counter. Returning false stops the
// 6..0x11 loop in sub_17A20.
//   stat 0..9     : [+3F] += 10 when unk_72090[class*10+stat] != 0.
//   stat 0x0A     : [+3F] += 10 for class 0x2F/0x30 only; otherwise the magic-charge
//                   path below with a table index of -1.
//   stat 0xB..0x11: [+3E] += 10 when unk_72310[class*7+slot]-1 > [+2E+slot].
//   [+3F] >= 100  : stat 6..0x0A raises one weapon count (+38/+3A/+39/+3B/+3C), resets
//                   [+3F] and returns true. Otherwise, when [+3E] >= 100, raises one
//                   magic charge (+2E+slot), resets [+3E] and returns false.
bool CombatII::battle_growth_176f1(CombatUnitPlacement& u, int stat) {
    if (u.combat_class >= 0x3F) return false;                // loc_17723
    const int cls = (int)u.combat_class;
    if (stat <= 9) {
        if (cls >= (int)(sizeof(kLevelGrowthTable) / sizeof(kLevelGrowthTable[0]))) return true;
        if (kLevelGrowthTable[cls][stat] == 0) return true;  // loc_178A2
        u.growth_3f = (uint8_t)(u.growth_3f + 10);           // loc_17751
    } else if (stat == 0x0A && (cls == 0x2F || cls == 0x30)) {
        u.growth_3f = (uint8_t)(u.growth_3f + 10);           // loc_17751
    } else if (stat >= 0x0A) {
        const int slot = stat - 0x0B;                        // -1..6
        int cap = 0;
        if (cls < (int)(sizeof(kSpellGrowthTable) / sizeof(kSpellGrowthTable[0]))) {
            cap = (slot >= 0) ? (int)kSpellGrowthTable[cls][slot]
                              : (cls > 0 ? (int)kSpellGrowthTable[cls - 1][6] : 0);
        }
        const int have = (slot >= 0)
                             ? (int)u.spell_charge_2e[slot]
                             : (int)u.hit_seq_2d;            // [+2Dh] (-1 index of stat 0x0A)
        if (cap - 1 > have) {                                // dec ebx; cmp ebx,eax; jle
            u.combat_stat_3e = (uint8_t)(u.combat_stat_3e + 10);
            // ASM 11057..11065: `add cl,0Ah; mov [ebp+3Eh],cl; cmp cl,64h; jb loc_17723`
            // -> below 100 it returns eax=0 here (breaks the 6..0x11 loop in sub_17A20).
            if (u.combat_stat_3e < 100) return false;
        }
    }
    // loc_177AD
    if (u.growth_3f >= 100) {
        bool raised = false;
        switch (stat - 6) {                                  // off_176DD (stat 6..0Ah)
            case 0: if (u.weap_cnt_38 < 7) { u.weap_cnt_38++; raised = true; } break;
            case 1: if (u.weap_cnt_3a < 4) { u.weap_cnt_3a++; raised = true; } break;
            case 2: if (u.weap_cnt_39 < 4) { u.weap_cnt_39++; raised = true; } break;
            case 3: if (u.weap_cnt_3b < 4) { u.weap_cnt_3b++; raised = true; } break;
            case 4: if (u.weap_cnt_3c < 6) { u.weap_cnt_3c++; raised = true; } break;
            default: break;
        }
        if (raised && battle_trace_)
            fprintf(stderr, "COMBAT: weapon level up unit=%d stat=%d wc=(%u,%u,%u,%u,%u)\n",
                    u.id, stat, (unsigned)u.weap_cnt_38, (unsigned)u.weap_cnt_39,
                    (unsigned)u.weap_cnt_3a, (unsigned)u.weap_cnt_3b, (unsigned)u.weap_cnt_3c);
        u.growth_3f = 0;                                     // loc_1789E
        return true;                                         // loc_178A2
    }
    // loc_178B0
    if (u.combat_stat_3e < 100) return true;
    const int slot = stat - 0x0B;
    if (slot >= 0 && slot < 7)
        u.spell_charge_2e[slot] = (uint8_t)(u.spell_charge_2e[slot] + 1);
    else
        u.hit_seq_2d = (uint8_t)(u.hit_seq_2d + 1);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: magic charge up unit=%d stat=%d slots=%u,%u,%u,%u,%u,%u,%u\n",
                u.id, stat, (unsigned)u.spell_charge_2e[0], (unsigned)u.spell_charge_2e[1],
                (unsigned)u.spell_charge_2e[2], (unsigned)u.spell_charge_2e[3],
                (unsigned)u.spell_charge_2e[4], (unsigned)u.spell_charge_2e[5],
                (unsigned)u.spell_charge_2e[6]);
    u.combat_stat_3e = 0;
    return false;                                            // loc_17723
}

// sub_1796B(unit): recompute attack/defence/TP/def59 after a level-up. Does nothing when
// [77h] is set.
void CombatII::battle_recalc_stats_1796b(CombatUnitPlacement& u) {
    if (u.stored_77) return;                                 // cmp dword ptr [eax+77h],0
    const int cls = (int)u.combat_class;
    u.combat_value_44 = (int16_t)(uint16_t)u.combat_str_40;  // mov [44h],[40h]
    const int wc = weapon_class(u);
    if (wc == 0 || wc == 6) {
        u.combat_value_44 = (int16_t)(u.combat_value_44 * 2); // add eax,eax
    } else {
        u.combat_value_44 = (int16_t)(u.combat_value_44 +
            (int8_t)battle_lookup_184a4((int)u.combat_type_5b, 0x2C));
    }
    const int def_base = (cls >= 0 && cls < (int)(sizeof(kLevelDefBase) / sizeof(kLevelDefBase[0])))
                             ? (int)kLevelDefBase[cls] : 0;
    u.combat_value_46 = (int16_t)(def_base + battle_item_col_sum(u, 0x2D));
    const int speed_base = (cls >= 0 && cls < 192)
                               ? (int)(kClass71B68[cls][4] | (kClass71B68[cls][5] << 8)) : 0;
    int v54 = (int)(uint8_t)(u.combat_value_42 & 0xFF) + speed_base;
    if ((int16_t)v54 < 0x3C) v54 = 0x3C;
    u.combat_value_54 = (int16_t)v54;
    int def59 = ((cls >= 0 && cls < (int)(sizeof(kLevelDef59Base) / sizeof(kLevelDef59Base[0])))
                     ? (int)kLevelDef59Base[cls] : 0) + battle_item_col_sum(u, 0x2F);
    u.combat_def59 = (uint8_t)def59;                         // add al,dl (byte wrap)
    if (battle_trace_)
        fprintf(stderr, "COMBAT: recalc unit=%d class=%d atk=%d def=%d speed54=%d def59=%u (sub_1796B)\n",
                u.id, cls, (int)u.combat_value_44, (int)u.combat_value_46,
                (int)u.combat_value_54, (unsigned)u.combat_def59);
}

// sub_17A20(unit, victim): kill EXP/money and level-up. victim == -1 only runs the
// level-up check.
//   EXP = (victim[+2B]-killer[+2B]+1)*10, or 5 for a negative difference, capped by the
//   remaining EXP (100-[+3D]). Level 99 gains neither EXP nor money. The message and the
//   money are granted only when [+0E] == 1 (player army). Reaching [+3D] == 100 raises
//   the level and runs sub_1767E(0..5) / sub_176F1(6..0x11) / sub_1796B.
void CombatII::battle_gain_exp(int unit_index, int victim_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (u.combat_class == 0xA1) return;                      // cmp byte [eax+11h],0A1h
    // With a stored [77h] record the progress accumulates there and only EXP/level are
    // copied back to the real unit.
    CombatUnitPlacement* prog = u.stored_77 ? u.stored_77.get() : &u;
    if (u.stored_77) {                                       // 17A30..17A4C: [77h][0]=[0], [77h][1]=[1]
        prog->x = u.x;                                       // mov al,[eax]; mov [edx],al
        prog->y = u.y;                                       // mov al,[eax+1]; mov [edx+1],al
    }
    const CombatUnitPlacement* victim = nullptr;
    if (victim_index >= 0 && (size_t)victim_index < units_.size() &&
        victim_index != unit_index)
        victim = &units_[(size_t)victim_index];

    int gained = 0;
    if (victim) {
        int diff = (int)victim->combat_level_2b - (int)prog->combat_level_2b; // [2Bh] difference
        gained = diff < 0 ? 5 : (diff + 1) * 10;
        const int room = 100 - (int)prog->exp_3d;            // 100-[+3D]
        if (gained > room) gained = room;                    // cmp eax,64h; jle
        if (prog->combat_level_2b == 99) {                   // cmp [2Bh],63h
            prog->exp_3d = 0;
            gained = 0;
        }
        if (battle_effective_army(*prog) == 1) {             // cmp byte [ebp+0Eh],1 - ebp already points at [77h]
            const unsigned money = (unsigned)victim->reward_money_85;
            // word_FA784 = unk_76E94 + itoa(exp) + aBcb + ltoa(money) + aEldBcb
            // (dasm 17AB7..17B32):
            //   unk_76E94 = 0A 89 77 D0 F1 C3 A1 20 00           "\n<EXP label> "
            //   aBcb      = 20 D2 42 97 62 21 0A 00               " <gained>!\n"
            //   aEldBcb   = 20 45 4C 44 20 D2 42 97 62 21 00       " ELD <gained>!"
            std::string msg = "\n\x89\x77\xD0\xF1\xC3\xA1 ";
            msg += std::to_string((unsigned)gained);         // itoa_(gained, 10)
            msg += " \xD2\x42\x97\x62!\n";
            msg += std::to_string(money);                    // ltoa_(victim[85h], 10)
            msg += " ELD \xD2\x42\x97\x62!";
            battle_unit_message(*prog, msg);                 // sub_482A5(ebp, word_FA784)
            if (should_exit_) return;
            if (fdf85_word_ < 100)                           // dword_FE36F[word_FDF85] += ax
                battle_stash_money_[fdf85_word_] += money;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: kill reward unit=%d victim=%d exp=%d money=%u stash=%u total=%u msg='%s'\n",
                        u.id, victim->id, gained, money, (unsigned)fdf85_word_,
                        (unsigned)battle_stash_money_[fdf85_word_ < 100 ? fdf85_word_ : 0],
                        Font::johab_to_utf8(msg).c_str());
        }
    }
    prog->exp_3d = (uint8_t)(prog->exp_3d + gained);         // add [ebp+3Dh],al
    if (prog->exp_3d != 100) {
        if (prog && prog != &u) u.exp_3d = prog->exp_3d;     // [77h] -> copy back to the unit
        return;
    }

    // ---- level up ----
    prog->combat_level_2b = (uint8_t)(prog->combat_level_2b + 1); // inc byte [2Bh]
    prog->exp_3d = 0;                                        // mov byte [3Dh],0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: LEVEL UP unit=%d victim=%d -> level=%u\n",
                u.id, victim ? victim->id : -1, (unsigned)prog->combat_level_2b);
    // ASM 11404..11425: "\n LEVEL " + tens digit (a space below 10) + ones digit +
    // " rose." in JOHAB - the original writes the digits into bytes 8/9 of the 27-byte
    // template (word_FA784).
    {
        const int lv = (int)prog->combat_level_2b;
        std::string lvmsg = "\n LEVEL ";
        lvmsg += (lv >= 10) ? (char)('0' + (lv / 10) % 10) : ' ';
        lvmsg += (char)('0' + lv % 10);
        lvmsg += "\x9D\xA1 \xACw\xAFw\xD0\x96\xAFs\x93\xA1\x94" "a.";   // " rose."
        battle_unit_message(*prog, lvmsg);                               // sub_482A5(ebp, word_FA784)
        if (should_exit_) return;
    }
    // sub_1793D: collects the stat-up strings and opens the dialogue box every 3rd one
    // (cx==2 -> sub_482A5, then clear the buffer).
    std::string grow_buf;
    int grow_n = 0;
    auto grow_msg = [&](const char* what) {
        if (battle_trace_) fprintf(stderr, "COMBAT: growth unit=%d %s\n", u.id, what);
        grow_buf += what;
        if (grow_n == 2) {
            battle_unit_message(*prog, grow_buf);
            grow_buf.clear();
            grow_n = 0;
        } else {
            ++grow_n;
        }
    };
    {   // stat 0..3: +1 point - strings at COMBATII.EXE 0x76ECB/0x76ED7/0x76EE4/0x76EF0
        static const char* const kStatNames[4] = {
            "\xB5\xC5\x9D\x62 \xACw\xAFw!\n",                          // strength rose
            "\xA3\xA5\xC0\xF3\x95\xA1 \xACw\xAFw!\n",                // agility rose
            "\xBB\xA1\x9D\x62 \xACw\xAFw!\n",                          // wisdom rose
            "\x8A\xA5\xB6\xE1 \xACw\xAFw!\n",                          // health rose
        };
        for (int s = 0; s < 4; ++s) {
            if (battle_growth_1767e(*prog, s) != 1) continue;
            switch (s) {
                case 0:
                    if (prog->combat_str_40 < 0xFF) { prog->combat_str_40++; grow_msg(kStatNames[s]); }
                    break;
                case 1: {   // [+42h] low byte
                    if ((uint8_t)(prog->combat_value_42 & 0xFF) < 0xFF) {
                        const uint8_t lo = (uint8_t)((uint8_t)(prog->combat_value_42 & 0xFF) + 1);
                        prog->combat_value_42 = (int16_t)(uint16_t)(lo | (prog->combat_value_43 << 8));
                        grow_msg(kStatNames[s]);
                    }
                    break;
                }
                case 2:
                    if (prog->combat_mag41 < 0xFF) { prog->combat_mag41++; grow_msg(kStatNames[s]); }
                    break;
                case 3:
                    if (prog->combat_value_43 < 0xFF) {
                        prog->combat_value_43++;
                        const uint8_t lo = (uint8_t)(prog->combat_value_42 & 0xFF);
                        prog->combat_value_42 = (int16_t)(uint16_t)(lo | (prog->combat_value_43 << 8));
                        grow_msg(kStatNames[s]);
                    }
                    break;
            }
        }
    }
    {   // stat 4: max HP += growth*100 (raw); stat 5: max MP += growth
        const int hp_gain = battle_growth_1767e(*prog, 4);
        if (hp_gain > 0) {
            prog->max_hp += hp_gain;                         // [+4Ch] += 100*growth
            grow_msg("\xC1\x41\x9D\x62 \xACw\xAFw!\n");               // "max HP rose"
        }
        const int mp_gain = battle_growth_1767e(*prog, 5);
        prog->combat_mp_max_52 = (uint16_t)(prog->combat_mp_max_52 + mp_gain); // [+52h] += ax
        grow_msg("\xA0\x61\xA4\xF3\x9D\x62 \xACw\xAFw!\n");         // "max MP rose"
    }
    // A non-zero sub_1793D return (fewer than 3 collected) flushes the remaining buffer once.
    if (grow_n != 0 && !grow_buf.empty()) {
        battle_unit_message(*prog, grow_buf);
        if (should_exit_) return;
    }
    for (int s = 6; s < 0x12; ++s) {
        if (!battle_growth_176f1(*prog, s)) break;
        if (should_exit_) return;
    }
    battle_recalc_stats_1796b(*prog);
    if (prog != &u) {                                        // [77h] unit: copy back EXP/level only
        u.exp_3d = prog->exp_3d;
        u.combat_level_2b = prog->combat_level_2b;
    }
}

// sub_17E17: recompute a summon's (code_b 0x27..0x32) [+44h] attack from the summoner's
// HP ratio: base = dword_73108[(code_b-0x27)*5*2] >> 17,
// [+44h] = base + base*(hp/100)/1000.
void CombatII::battle_recalc_summon_attack(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    if (u.code_b < 0x27 || u.code_b > 0x32) return;
    const int idx = u.code_b - 0x27;
    const int base = kSummonAttackBase[idx];
    const int hp_display = battle_hp_raw(u) / 100;
    u.combat_value_44 = (int16_t)(base + base * hp_display / 1000);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: sub_17E17 unit=%d code_b=%d hp=%d attack=%d\n",
                u.id, u.code_b, hp_display, (int)u.combat_value_44);
}

void CombatII::battle_atb_step() {
    // sub_19AEF (ASM 14313..): walks the CC968 lists 1..4 in list order (each list is
    // linked through [7Bh]). The port keeps the per-unit order (gauge -> rage -> status
    // -> timer -> terrain) but rebuilds the walk in ascending (effective army 1..4,
    // list_seq) so random draws and ready-queue insertion match the original list order.
    // Units outside 1..4 (group 0 / unmapped remap) have no list in the original; they
    // are appended at the end because the port still has to run them.
    std::vector<int> order;
    order.reserve(units_.size());
    for (int army = 1; army <= 4; ++army) {
        std::vector<int> bucket;
        for (size_t i = 0; i < units_.size(); ++i)
            if (battle_effective_army(units_[i]) == army) bucket.push_back((int)i);
        std::stable_sort(bucket.begin(), bucket.end(), [&](int a, int b) {
            return units_[(size_t)a].list_seq < units_[(size_t)b].list_seq;
        });
        order.insert(order.end(), bucket.begin(), bucket.end());
    }
    for (size_t i = 0; i < units_.size(); ++i) {
        const int a = battle_effective_army(units_[i]);
        if (a < 1 || a > 4) order.push_back((int)i);
    }
    std::vector<std::pair<int,int>> damage_list; // (unit index, raw damage)
    int damage_slot = 0;
    for (int oi2 : order) {
        const size_t i = (size_t)oi2;
        if (i >= units_.size()) continue;
        CombatUnitPlacement& u = units_[i];
        if (!u.active || u.combat_class == 0xA2) continue;   // cmp [ebx+11h],0A2h

        if (u.battle_gauge != 0xFF) {
            u.battle_gauge = (uint8_t)(u.battle_gauge + 1); // `inc byte ptr [ebx+56h]`
            int threshold = (int)kBattleSpeedTable[u.combat_class];
            if (u.combat_flags_15 & 0x10) threshold = threshold * 2 / 3 + 1;
            else if (u.combat_flags_14 & 0x02) threshold >>= 1;
            if ((int)u.battle_gauge >= threshold) {
                u.combat_flags_14 = (uint8_t)(u.combat_flags_14 & 0xFB); // and [14h],0FBh
                u.battle_gauge = 0xFF;
                u.combat_flags_13 = 0;                       // [+13h] = 0
                u.anim_frame = 0;                            // [+64h] = 0
                // ---- rage: class table [7] == 1 and HP < max/2 ----
                // ASM 14396..14410: `call sub_499BD(3); test ax,ax; jz loc_19BEB`. With
                // rand == 0, [15h] |= 0xE0 only when sprite < 0x12C; otherwise [15h] |= 0x40.
                if (kBattleClassData[u.combat_class][7] == 1 &&
                    (battle_max_hp_raw(u) / 2) > battle_hp_raw(u)) {
                    u.buff_18 = 0x1E;
                    u.buff_1a = 0x1E;
                    u.combat_flags_15 = (uint8_t)(u.combat_flags_15 | 0x18);
                    if (combat_rand(3) == 0) {
                        if (u.sprite_type < 0x12C) {             // loc_19BEB
                            u.buff_1c = 0x1E;
                            u.combat_flags_15 = (uint8_t)(u.combat_flags_15 | 0xE0);
                        }
                    } else {
                        u.buff_1c = 0x1E;
                        u.combat_flags_15 = (uint8_t)(u.combat_flags_15 | 0x40);
                    }
                }
                const bool manual_player = battle_unit_is_player(u); // [+0Eh]==1 && [+83h]==0 && !([+1E]&10h)
                if (manual_player) {
                    battle_ready_queue_.push_back((int)i);       // sub_196D0: FIFO tail append
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: ATB ready player unit=%zu id=%d class=%u threshold=%d queue=%zu\n",
                                i, u.id, (unsigned)u.combat_class, threshold, battle_ready_queue_.size());
                } else if (battle_trace_) {
                    fprintf(stderr, "COMBAT: ATB ready AI unit=%zu id=%d class=%u army=%d auto=%u flags1e=%02X threshold=%d\n",
                            i, u.id, (unsigned)u.combat_class, u.army, (unsigned)u.auto_battle,
                            (unsigned)u.combat_flags_1e, threshold);
                }
            }
        }
        // loc_19C1D: during a barrier, force the sleep/paralysis bits off.
        if (u.combat_flags_15 & 0x40) u.combat_flags_1e = (uint8_t)(u.combat_flags_1e & 0xF8);
        // loc_19C27: ailment handling.
        if (u.combat_flags_1e != 0) {
            int status_damage = 0;
            if (battle_status_ailment_1992d((int)i, damage_slot, &status_damage) == 1) {
                damage_list.push_back({(int)i, status_damage});
                ++damage_slot;
            }
        }
        battle_status_timer_19892(u);
        // loc_19C5C: ready units take terrain damage or healing (sub_17310).
        if (u.battle_gauge == 0xFF) {
            battle_terrain_17310((int)i);
            if (should_exit_) return;
        }
    }
    if (!damage_list.empty()) battle_status_damage_anim(damage_list);
}

// sub_4E4DD + sub_4ED18: initial time of day.
//   sub_4E4DD reads word_FDF82 (day) and byte_FDF84 (phase) from BLACKBOX.DAT offset 1
//   and doubles the phase. If SCE header +5 (var_4) is non-negative, byte_FDF84 is then
//   overwritten with that value.
void CombatII::battle_load_time_of_day() {
    tod_day_ = 0;
    tod_phase_ = 0;
    std::string path = game_path("BLACKBOX.DAT");
    if (!file_exists(path.c_str())) path = game_path("blackbox.dat");
    if (file_exists(path.c_str())) {
        if (FILE* fp = fopen(path.c_str(), "rb")) {
            uint8_t b[3] = {};
            if (fseek(fp, 1, SEEK_SET) == 0 && fread(b, 1, 3, fp) == 3) {
                tod_day_ = (uint16_t)(b[0] | (b[1] << 8));
                tod_phase_ = (uint8_t)(b[2] * 2);            // `add ah, ah`
            }
            fclose(fp);
        }
    }
    if (sce_time_byte_ >= 0) tod_phase_ = (uint8_t)sce_time_byte_;
    fprintf(stderr, "COMBAT: time of day day=%u phase=%u (SCE byte=%d)\n",
            tod_day_, tod_phase_, (int)sce_time_byte_);
}

// sub_197CA: advance the time-of-day phase by one and dim the palette.
//   byte_FDF84 = (byte_FDF84 + 1) % 16; on reaching 8, word_FDF82++ (a new day)
//   byte_BA84E == 3 stops here (loc_196CA - the palette is untouched)
//   phase < 8 : brightness % = 100 - phase*6      (100 .. 58)
//   phase >= 8: brightness % = 0x34 + (phase-8)*6 (52 .. 94) -> word ptr dword_F042A+2
//   sub_5FA2C(0, 0x100, ..., brightness %) writes unk_12824C <- unk_F9188 (the palette
//   before dimming) to the DAC as c' = c * brightness% / 100 (idiv, still 6-bit), and
//   sub_5F945 reads the DAC back to settle unk_12824C. sub_6004E/sub_60275 re-read the
//   palette rotation ranges (byte_128564/byte_128568 entries) from the DAC; the port has
//   no DAC rotation ranges, so g_palette is enough.
void CombatII::battle_time_of_day_step() {
    tod_phase_ = (uint8_t)((tod_phase_ + 1) % 16);
    if (tod_phase_ == 8) tod_day_ = (uint16_t)(tod_day_ + 1);
    if (sce_map_kind_ == 3) {
        if (battle_trace_) fprintf(stderr, "COMBAT: sub_197CA phase=%u day=%u (map kind 3: palette kept)\n", tod_phase_, tod_day_);
        return;
    }
    tod_factor_ = (tod_phase_ < 8) ? (uint16_t)(100 - tod_phase_ * 6)
                                   : (uint16_t)(0x34 + (tod_phase_ - 8) * 6);
    for (int i = 0; i < 256; i++)
        for (int ch = 0; ch < 3; ch++)
            g_palette[i][ch] = (uint8_t)((int)weather_cur_palette_[i][ch] * (int)tod_factor_ / 100);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: sub_197CA phase=%u day=%u brightness=%u%%\n", tod_phase_, tod_day_, tod_factor_);
}

// sub_19F5D (called every tick by the INT8 handler):
//   `call sub_10392` (word_789A4 == 0?) -> `cmp byte_F042E,0` -> `sub_5D827(0x39)`
//   (Space pressed table). When all three hold: byte_F042E = 2, sub_1038B(0x32) (50-tick
//   cooldown), sub_44DEC(1) (overlay on).
// Port: during cutscenes Space advances dialogue (port-only input), so the ISR hook runs
// only from the battle viewer loop.
void CombatII::battle_pause_isr_hook() {
    if (!battle_loop_active_) return;
    if (battle_pause_cooldown_ != 0) return;
    if (battle_state_ != 0) return;
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    if (!keys || !keys[SDL_SCANCODE_SPACE]) return;
    battle_state_ = 2;
    battle_pause_cooldown_ = 0x32;
    battle_overlay_mode_ = 1;
    if (battle_trace_) fprintf(stderr, "COMBAT: pause requested (sub_19F5D) time=%u\n", battle_time_);
}

// sub_5580F loc_55B19 (Space key): when F042E != 0 and the cooldown expired, back to 0.
//   sub_19F9D(0); sub_1038B(0x32); sub_44DEC(0)
void CombatII::battle_resume_key() {
    if (battle_state_ == 0) return;
    if (battle_pause_cooldown_ != 0) return;
    battle_state_ = 0;
    battle_pause_cooldown_ = 0x32;
    battle_overlay_mode_ = 0;
    if (battle_trace_) fprintf(stderr, "COMBAT: resume (Space) time=%u\n", battle_time_);
}

// ---- pacing (port only) ----
// The original sub_5580F calls sub_19D53 on every loop iteration with no gating
// (byte_789A8/byte_789A6 belong to drawing and the UI), so how fast dword_F0418 rises
// depends on CPU speed. The port needs a fixed period, and the perceived speed of
// P1 (gauge) / P6 (AI) depends on it. Default is a half tick (30 game ticks = 171 ms =
// 5.85 Hz); G2_BATTLE_TICK = free | frame | half overrides it.
bool CombatII::battle_tick_due() {
    if (battle_tick_period_ <= 0) return true;                 // no gating (as the original)
    const double now = timer_now_us();
    const double period_us = (double)battle_tick_period_ * 1e6 / kTimerHz;
    if (battle_tick_next_us_ == 0.0) {                         // first call / just resumed: set baseline only
        battle_tick_next_us_ = now + period_us;
        return false;
    }
    if (now < battle_tick_next_us_) return false;
    battle_tick_next_us_ += period_us;
    if (now - battle_tick_next_us_ > 4.0 * period_us) battle_tick_next_us_ = now + period_us;  // avoid runaway catch-up
    return true;
}

// sub_19D53 loc_19E02..loc_19F56: walks slots 0..3 of word_F0424 (the CC968 linked lists
// of effective army 1..4) in list order (sub_1134F tail insert = load, reinforcement,
// faction change order) and saves the next node (edx = [+7Bh]) before acting, so the walk
// continues even if the unit leaves the list. Returning 2 preserves F0424/F0410 for the
// next call to resume (word_F0422).
int CombatII::battle_turn_scheduler() {
    constexpr uint64_t kNull = ~0ull;          // slot start (F0410 == 0, read the head)
    constexpr uint64_t kEnd = ~0ull - 1;       // saved next is null: slot end
    if (!battle_scan_active_) {
        battle_scan_group_ = 0;
        battle_scan_next_key_ = kNull;
        battle_ai_acted_ = 0;
    }
    battle_scan_active_ = 1;
    auto key_of = [&](size_t i) -> uint64_t {
        return ((uint64_t)units_[i].list_seq << 32) | (uint64_t)i;
    };
    // Smallest active node of the slot list with key >= from (strict: > from), or -1.
    auto find_node = [&](int army, uint64_t from, bool strict) -> int {
        int best = -1;
        uint64_t best_key = kNull;
        for (size_t i = 0; i < units_.size(); ++i) {
            const CombatUnitPlacement& c = units_[i];
            if (!c.active || battle_effective_army(c) != army) continue;
            const uint64_t k = key_of(i);
            if (from != kNull && (strict ? k <= from : k < from)) continue;
            if (k < best_key) { best_key = k; best = (int)i; }
        }
        return best;
    };
    while (battle_scan_group_ < 4) {
        const int army = (int)battle_scan_group_ + 1;
        const int idx = (battle_scan_next_key_ == kEnd) ? -1
            : (battle_scan_next_key_ == kNull)
                ? find_node(army, kNull, false)                   // sub_113DD(CC968[slot]) = list head
                : find_node(army, battle_scan_next_key_, false);
        if (idx < 0) {                                             // loc_19EA2: end of slot
            ++battle_scan_group_;
            battle_scan_next_key_ = kNull;
            continue;
        }
        const size_t i = (size_t)idx;
        battle_scan_unit_ = idx;
        const uint64_t cur_key = key_of(i);
        const int next = find_node(army, cur_key, true);           // edx = [+7Bh]
        // Last node: a node appended during the action is not visited in this slot
        // (saved next = null).
        battle_scan_next_key_ = (next < 0) ? kEnd : key_of((size_t)next);
        CombatUnitPlacement& u = units_[i];

        // loc_19E43: AI when effective army > 1, the auto byte != BL (0), or status bit 0x10.
        const bool ai_candidate = battle_unit_is_ai_controlled(u);
        if (ai_candidate && u.battle_gauge == 0xFF) {
            battle_ai_acted_ = 1;
            // battle_ai_step may run the event VM (reinforcements reallocate units_), so u
            // is stale afterwards.
            const int unit_id = u.id;
            const unsigned unit_class = (unsigned)u.combat_class;
            const int unit_weapon_class = weapon_class(u);
            if (!battle_ai_step((int)i) && battle_trace_)
                fprintf(stderr, "COMBAT: AI ready unit=%zu id=%d class=%u weapon_class=%d (AI action unavailable)\n",
                        i, unit_id, unit_class, unit_weapon_class);
            if (should_exit_) return 1;
        }
        if (battle_state_ == 2) {
            battle_state_ = 3;
            return 2;
        }
    }

    const int ret = battle_ai_acted_ ? 3 : 1;
    battle_scan_active_ = 0;
    battle_scan_group_ = 0;
    battle_ai_acted_ = 0;
    battle_scan_unit_ = -1;

    // `inc eax; div byte_74138`: every 9, 19, ... a time the last queued unit starts.
    if (((battle_time_ + 1) % (uint32_t)hud_time_divisor_) != 0 || battle_ready_queue_.empty()) return ret;
    const int unit_index = battle_ready_queue_.back(); // sub_1979F(F0426 - 1)
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return ret;

    // End of sub_19D53 (14655..14675): sfx 0x1D + sub_1124A + F042E=1 + sub_44DEC(1) +
    // sub_1A0F0(1), then return 2. No ring opens; play enters free wait (5580F). The
    // gauge/TP/diffusion work starts on entering sub_4D674 (click).
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    battle_player_unit_ = -1; // free wait: sub_4D674 not entered yet (set once a unit is clicked)
    battle_free_wait_ = true;
    battle_free_hover_ = -1;
    battle_free_cycle_ = 0; // loc_558E2: sub_19D53 returning 2 does `xor ecx,ecx`
    battle_attack_cancel();
    battle_move_cancel();
    sfx(0x1D); // mov eax,1Dh; call sub_4F4CD
    camera_center_on(u.x, u.y); // sub_1124A (selection box + HP drawn as free hover)
    battle_state_ = 1; // mov byte_F042E,1
    // This tick's sub_44EB0 redraws the bottom gauge with this value (the original does it
    // every tick).
    battle_hud_time_ = battle_time_;
    battle_overlay_mode_ = 1; // call sub_44DEC(1): banner on
    battle_set_420f(1); // mov eax,1; call sub_1A0F0 (14673-14674)
    ring_opened_this_turn_ = false;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: player wait unit=%d id=%d queue=%zu (free, click to enter 4D674)\n",
                unit_index, u.id, battle_ready_queue_.size());
    return 2;
    return 2;
}

// sub_4D674 + sub_4D442: player turn state machine. Instead of the original ring
// (4B767/4BA7F) the port drives the same dispatch with hotkeys and clicks:
// click = move (13/4D98E), A = attack (0), M = magic (6), I = item (7), W = wait (8),
// G = guard (10), D = defend 40 (9), F = defend 60 (12), N = info (11), V = weapon
// (1..5), Esc = cancel. Returning true means Esc was consumed, so the module does not exit.
bool CombatII::battle_player_input() {
    // Free wait (5580F): the left-click branch checks no state; only the right-click cycle
    // tests 19FA3()==1. Ring input is state 1 only.
    if (battle_free_wait_) return battle_free_input();
    if (battle_state_ != 1) return false;
    if (battle_player_unit_ < 0 ||
        (size_t)battle_player_unit_ >= units_.size()) return false;
    CombatUnitPlacement& u = units_[(size_t)battle_player_unit_];
    if (!u.active) { battle_player_end_turn(false); return true; }
    const int ui = battle_player_unit_;

    auto has_menu = [&](int id) {
        return std::find(battle_menu_ids_.begin(), battle_menu_ids_.end(), id) != battle_menu_ids_.end();
    };
    // Back to the ring (si==0/-2): end the turn when TP < var18 (4DA7B), else redraw the
    // menu (4D7EA). No move diffusion is laid down here; E8 is the sub_1CC09 range.
    auto back_to_ring = [&]() {
        battle_attack_cancel();
        battle_move_cancel();
        battle_magic_select_ = false;
        battle_item_select_ = false;
        battle_weapon_select_ = false;
        ring_sub_close();
        if (battle_tp() < battle_move_var18_) {
            if (battle_trace_) fprintf(stderr, "COMBAT: ring TP=%d <var18=%d, turn ends (4DA7B)\n", battle_tp(), battle_move_var18_);
            battle_player_end_turn(false);
            return;
        }
        if (battle_var24_action_end_) {
            battle_player_end_turn(true);
            return;
        }
        battle_player_show_menu(ui); // 1CC09 + redraw the ring (4D7EA)
    };

    // Attack target phase (cursor loop of sub_1F738): Esc, right click and left click are
    // all handled here.
    if (battle_atk_phase_ != 0) {
        const bool esc_pressed = input_.is_key_pressed(SDLK_ESCAPE);
        battle_attack_input(ui);
        return esc_pressed;
    }

    // Move phase (sub_4A483 destination loop -> sub_16357 facing loop).
    if (battle_move_phase_ != 0) return battle_move_input(ui);

    // ---- sub-ring input (sub_4C72A/4CDBD/4CAA3 + sub_4D06E/4CE1F) ----
    // The original blocks in a loop; the port is a per-frame state machine. An open
    // sub-ring is handled before the main ring (hit -> select, miss -> back/move,
    // right click/Esc -> back).
    if (ring_subkind_ != SubRingKind::None && ring_sub_open_) {
        // 4CE1F target selection goes through the map click path.
        if (ring_subkind_ == SubRingKind::AITarget) {
            const bool esc_now = input_.is_key_pressed(SDLK_ESCAPE);
            battle_ai_target_input(ui);
            return esc_now;
        }
        // Hover update (the [ecx+5] of sub_4AC8A).
        ring_sub_compute_layout(ui);
        {
            const int hx = input_.get_mouse_x(), hy = input_.get_mouse_y();
            if (hx >= 0 && hy >= 0) ring_sub_set_hover(ring_sub_hit_test(hx, hy));
        }
        const MouseState& mst0 = input_.get_mouse();
        const bool esc0 = input_.is_key_pressed(SDLK_ESCAPE);
        // Right click/Esc: back (sub_4BA7F -1; per-level meaning below).
        if (mst0.right_clicked || esc0) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: cancel kind=%d unit=%d (equivalent to sub_4BA7F -1)\n",
                        (int)ring_subkind_, u.id);
            // Per-level cancel (4C72A: level 1 -> main (-2), later -> back / AI: level 2 -> 1,
            // level 1 -> manual).
            switch (ring_subkind_) {
                case SubRingKind::MagicG: {
                    // loc_4C886 (var_8==0, <0) -> eax=-2 -> 4D442 returns 0 (back to the ring).
                    battle_magic_select_ = false;
                    back_to_ring();
                    return true;
                }
                case SubRingKind::MagicD:
                    ring_sub_open(SubRingKind::MagicG, ui);
                    return true;
                case SubRingKind::MagicK:
                    ring_sub_open(SubRingKind::MagicD, ui);
                    return true;
                case SubRingKind::Item:
                case SubRingKind::Weapon: {
                    // 4CDBD/4CAA3 <0 -> -1 -> 4D442 returns 0 (back to the ring).
                    battle_item_select_ = false;
                    battle_weapon_select_ = false;
                    back_to_ring();
                    return true;
                }
                case SubRingKind::AI1: {
                    // Level-1 cancel (<0) -> -1 after 19718 (pass ends). [83h] stays 1.
                    // loc_4D3A6: end_turn removes the unit from the queue.
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI1 cancel -> pass (equivalent to sub_4D06E -1)\n");
                    ring_sub_close();
                    ring_close_anim();
                    battle_player_end_turn(false);
                    return true;
                }
                case SubRingKind::AI2: {
                    // Level-2 miss (>0x0C) -> loc_4D0D0 (back to level 1).
                    ring_subkind_ = SubRingKind::AI1;
                    ring_sub_open_ = true;
                    ring_sub_compute_layout(ui);
                    return true;
                }
                case SubRingKind::AIMagic:
                    ring_subkind_ = SubRingKind::AI2;
                    ring_sub_open_ = true;
                    // AIMagic per-level back: k (d>=0) -> d (d=-2), d (d==-2) -> g (g=-1),
                    // g -> AI2.
                    if (ring_sub_magic_g_ >= 0 && ring_sub_magic_d_ >= 0) {
                        ring_sub_magic_d_ = -2;
                        ring_subkind_ = SubRingKind::AIMagic;
                        ring_sub_compute_layout(ui);
                    } else if (ring_sub_magic_g_ >= 0) {
                        ring_sub_magic_g_ = -1;
                        ring_sub_magic_d_ = -1;
                        ring_subkind_ = SubRingKind::AIMagic;
                        ring_sub_compute_layout(ui);
                    } else {
                        ring_subkind_ = SubRingKind::AI2;
                        ring_sub_compute_layout(ui);
                    }
                    return true;
                case SubRingKind::AIWeapon:
                    ring_subkind_ = SubRingKind::AI2;
                    ring_sub_open_ = true;
                    ring_sub_compute_layout(ui);
                    return true;
                default: break;
            }
        }
        if (input_.was_left_clicked()) {
            const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
            ring_sub_compute_layout(ui);
            int opt = -1;
            const bool hit = ring_sub_handle_click(ui, mx, my, opt);
            if (!hit) {
                // Missed left click: the 4BA7F proximity/FDAF2 gate. The port backs out
                // unconditionally. MagicG level-1 miss -> main (-2), later levels -> back,
                // AI2 -> level 1.
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: miss click kind=%d (equivalent to a sub_4BA7F miss)\n",
                            (int)ring_subkind_);
                switch (ring_subkind_) {
                    case SubRingKind::MagicG:
                        battle_magic_select_ = false;
                        back_to_ring();
                        return false;
                    case SubRingKind::MagicD:
                        ring_sub_open(SubRingKind::MagicG, ui);
                        return false;
                    case SubRingKind::MagicK:
                        ring_sub_open(SubRingKind::MagicD, ui);
                        return false;
                    case SubRingKind::Item:
                    case SubRingKind::Weapon:
                        battle_item_select_ = false;
                        battle_weapon_select_ = false;
                        back_to_ring();
                        return false;
                    case SubRingKind::AI2:
                        ring_subkind_ = SubRingKind::AI1;
                        ring_sub_open_ = true;
                        ring_sub_compute_layout(ui);
                        return false;
                    case SubRingKind::AIMagic:
                        ring_subkind_ = SubRingKind::AI2;
                        ring_sub_open_ = true;
                        if (ring_sub_magic_g_ >= 0 && ring_sub_magic_d_ >= 0) {
                            ring_sub_magic_d_ = -2;
                            ring_subkind_ = SubRingKind::AIMagic;
                            ring_sub_compute_layout(ui);
                        } else if (ring_sub_magic_g_ >= 0) {
                            ring_sub_magic_g_ = -1;
                            ring_sub_magic_d_ = -1;
                            ring_subkind_ = SubRingKind::AIMagic;
                            ring_sub_compute_layout(ui);
                        } else {
                            ring_subkind_ = SubRingKind::AI2;
                            ring_sub_compute_layout(ui);
                        }
                        return false;
                    case SubRingKind::AIWeapon:
                        ring_subkind_ = SubRingKind::AI2;
                        ring_sub_open_ = true;
                        ring_sub_compute_layout(ui);
                        return false;
                    default: break;
                }
            }
            // Hit: confirm per level.
            switch (ring_subkind_) {
                case SubRingKind::MagicG: {
                    // sub_4BFCA stage 0 returns id 1..7; g = id-1.
                    ring_sub_magic_g_ = opt - 1;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: magic a=%d g=%d (4C72A level 1)\n", opt, ring_sub_magic_g_);
                    ring_sub_open(SubRingKind::MagicD, ui);
                    return false;
                }
                case SubRingKind::MagicD: {
                    // sub_4C72A: id 0 -> b=7, ids 8..14 -> b=0..6.
                    ring_sub_magic_d_ = (opt == 0) ? 7 : opt - 8;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: magic g=%d id=%d b=%d (4C72A level 2)\n",
                                ring_sub_magic_g_, opt, ring_sub_magic_d_);
                    ring_sub_open(SubRingKind::MagicK, ui);
                    return false;
                }
                case SubRingKind::MagicK: {
                    // sub_4C72A: id 0 -> c=7, ids 15..21 -> c=0..6; then unk_74FA8.
                    const int g = ring_sub_magic_g_, b = ring_sub_magic_d_;
                    const int c = (opt == 0) ? 7 : opt - 15;
                    const int v = battle_magic_ring_value(g, b, c);
                    const int kind = (v != 0) ? battle_spell_kind(u, (v - 1) / 21) : -1;
                    const int r = (v != 0) ? (v - 1) + kind : -1;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: magic g=%d b=%d c=%d v=%d kind=%d R=%d (4C72A level 3)\n",
                                g, b, c, v, kind, r);
                    // Empty/non-base slot: sub_4C72A returns -1 and sub_4D442 passes it to
                    // sub_4C605 (Wrong Spell). For a valid slot, run 4C605's kind gate now;
                    // the cursor opens only when it passes.
                    if (v == 0 || r < 0 || battle_spell_kind(u, r / 21) != r % 3) {
                        battle_cast_magic(ui, -1, -1, -1);
                        back_to_ring();
                        return false;
                    }
                    battle_magic_pending_r_ = r;
                    // After R is fixed, wait for the target click: close the sub-ring and
                    // let the map click path take over (the original's 4C605 opens the
                    // cursor once 4C72A returns R).
                    ring_sub_close();
                    // battle_magic_select_ stays set (waiting for a target).
                    return false;
                }
                case SubRingKind::Item: {
                    battle_item_pending_dx_ = opt;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: item dx=%d (4CDBD)\n", opt);
                    // Warp (dx 0) and mount/dismount (dx 5) open the sub_4A483 destination cursor.
                    if (opt == 0 || opt == 5) {
                        ring_sub_close();
                        return false;
                    }
                    // Immediate items run right away, as on the number-key path.
                    ring_sub_close();
                    battle_item_select_ = false;
                    battle_item_pending_dx_ = -1;
                    {
                        const int si = battle_use_item(ui, opt, -1, -1);
                        if (si == -4 || si == -3 || si == -1) {
                            battle_var24_action_end_ = (si == -4);
                            battle_player_end_turn(si == -4);
                        } else {
                            if (si > 0)
                                battle_used_bits_ = (uint16_t)(battle_used_bits_ | (1u << (si - 1)));
                            if (battle_tp_changed() && si != -3)
                                units_[(size_t)ui].combat_acc57 = 0;
                            back_to_ring();
                        }
                    }
                    return false;
                }
                case SubRingKind::Weapon: {
                    if (battle_trace_) fprintf(stderr, "COMBAT: weapon si=%d (4CAA3)\n", opt);
                    ring_sub_close();
                    battle_weapon_select_ = false;
                    battle_use_weapon(ui, opt);
                    if (battle_tp_changed()) units_[(size_t)ui].combat_acc57 = 0;
                    // 4A483 target cursor pending: do not reopen the ring.
                    if (battle_weapon_pending_si_ >= 0) return false;
                    back_to_ring();
                    return false;
                }
                case SubRingKind::AI1: {
                    if (battle_trace_) fprintf(stderr, "COMBAT: AI1 pick id=%d\n", opt);
                    if (opt == 13) {
                        // loc_4D20F (var_8==0x0D): [83h]=0; on [56h]==FF call 196D0; return 1 ->
                        // manual.
                        u.auto_battle = 0;
                        if (battle_saved_gauge_ == 0xFF) {
                            if (std::find(battle_ready_queue_.begin(), battle_ready_queue_.end(), ui) ==
                                battle_ready_queue_.end())
                                battle_ready_queue_.push_back(ui);
                        }
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: first=13 [83h]=0 -> manual (equivalent to sub_4D06E returning 1)\n");
                        battle_used_bits_ = (uint16_t)(battle_used_bits_ | 0x01); // var_28 |= 1
                        ring_sub_close();
                        back_to_ring();
                        return false;
                    }
                    ring_sub_ai_first_ = opt;
                    ring_subkind_ = SubRingKind::AI2;
                    ring_sub_open_ = true;
                    ring_sub_compute_layout(ui);
                    return false;
                }
                case SubRingKind::AI2: {
                    if (battle_trace_) fprintf(stderr, "COMBAT: AI2 pick id=%d\n", opt);
                    if (opt == 2) {
                        battle_ai_enter_target(ui);
                        return false;
                    }
                    if (opt == 10) {
                        // loc_4D337: 4C72A -> [80h] = R/3 (= g*7 + d). The original
                        // has no learned-group gate here; sub_4C72A shows ids 1..7.
                        ring_sub_magic_g_ = -1;
                        ring_sub_magic_d_ = -1;
                        ring_subkind_ = SubRingKind::AIMagic;
                        ring_sub_open_ = true;
                        ring_sub_compute_layout(ui);
                        return false;
                    }
                    if (opt == 12) {
                        // loc_4D37B: 4CAA3 -> [82h] = si.
                        battle_weapon_options_.clear();
                        battle_weapon_list_4c03e(u, battle_weapon_options_);
                        if (battle_weapon_options_.empty()) {
                            sfx(0x02);
                            if (battle_trace_)
                                fprintf(stderr, "COMBAT: AI weapon empty (4CAA3 buzz)\n");
                            return false;
                        }
                        ring_subkind_ = SubRingKind::AIWeapon;
                        ring_sub_open_ = true;
                        ring_sub_compute_layout(ui);
                        return false;
                    }
                    battle_ai_apply_second(ui, opt);
                    // Back to level 1 after confirming (the original jumps to loc_4D0D0).
                    ring_subkind_ = SubRingKind::AI1;
                    ring_sub_open_ = true;
                    ring_sub_compute_layout(ui);
                    return false;
                }
                case SubRingKind::AIMagic: {
                    // AIMagic level 3 (group -> b -> c): sub_4BFCA ids and unk_74FA8.
                    if (ring_sub_magic_g_ < 0) {
                        ring_sub_magic_g_ = opt - 1; // stage-0 id 1..7
                        ring_sub_magic_d_ = -2;
                        ring_sub_compute_layout(ui);
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI magic a=%d g=%d\n", opt, ring_sub_magic_g_);
                    } else if (ring_sub_magic_d_ == -2) {
                        ring_sub_magic_d_ = (opt == 0) ? 7 : opt - 8;
                        ring_sub_compute_layout(ui);
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI magic g=%d id=%d b=%d\n",
                                    ring_sub_magic_g_, opt, ring_sub_magic_d_);
                    } else {
                        const int g = ring_sub_magic_g_, b = ring_sub_magic_d_;
                        const int c = (opt == 0) ? 7 : opt - 15;
                        const int v = battle_magic_ring_value(g, b, c);
                        const int r = (v == 0) ? -1
                            : (v - 1) + battle_spell_kind(u, (v - 1) / 21);
                        // loc_4D337: R < 0 leaves [80h] untouched and returns to AI1;
                        // otherwise [80h] = R/3 (= g*7+d).
                        if (r >= 0) u.ai_field_80 = (uint8_t)(r / 3);
                        if (battle_trace_) {
                            if (r >= 0)
                                fprintf(stderr, "COMBAT: AI magic g=%d b=%d c=%d v=%d R=%d -> [80h]=%d\n",
                                        g, b, c, v, r, r / 3);
                            else
                                fprintf(stderr, "COMBAT: AI magic g=%d b=%d c=%d v=%d R=%d -> [80h] unchanged\n",
                                        g, b, c, v, r);
                        }
                        ring_sub_magic_g_ = -1;
                        ring_sub_magic_d_ = -1;
                        ring_subkind_ = SubRingKind::AI1;
                        ring_sub_open_ = true;
                        ring_sub_compute_layout(ui);
                    }
                    return false;
                }
                case SubRingKind::AIWeapon: {
                    if (battle_trace_) fprintf(stderr, "COMBAT: AI weapon si=%d -> [82h]\n", opt);
                    u.ai_mode = (int8_t)(opt & 0xFF);
                    ring_subkind_ = SubRingKind::AI1;
                    ring_sub_open_ = true;
                    ring_sub_compute_layout(ui);
                    return false;
                }
                default: break;
            }
        }
        // Without a click this falls through to the hotkey path (M/I/V/digits). The text
        // path and shortcuts stay available even with a sub-ring open, for verification.
        // (A handled left click has already returned above.)
    }

    // Ring hover update (the [ecx+5] of sub_4AC8A): relayout every frame so camera moves track.
    if (ring_open_ && !battle_magic_select_ && !battle_item_select_ && !battle_weapon_select_) {
        // sub_455BD (83284): the ring display loop (loc_4D88F) checks for HUD panel overlap
        // before 4B8EB on every iteration - when the mouse covers the panel it moves to
        // another slot.
        battle_hud_panel_relocate(input_.get_mouse_x(), input_.get_mouse_y());
        ring_compute_layout(ui);
        const int hx = input_.get_mouse_x(), hy = input_.get_mouse_y();
        if (hx >= 0 && hy >= 0) ring_set_hover(ring_hit_test(hx, hy));
    }

    // Right click is also a ring cancel (-1) (sub_4BA7F: 3/0x1B -> 0xFFFFFFFF) -> free wait,
    // with time still stopped. The 4A483 target cursor is cancelled the same way
    // (right click/Esc); the original also closes the cursor and returns to the ring.
    if (battle_weapon_pending_si_ >= 0 &&
        (input_.get_mouse().right_clicked || input_.is_key_pressed(SDLK_ESCAPE))) {
        if (battle_trace_) fprintf(stderr, "COMBAT: weapon cursor cancelled unit=%d\n", u.id);
        battle_weapon_pending_si_ = -1;
        battle_weapon_pending_kind_ = 0;
        battle_pick_tx_ = battle_pick_ty_ = -1;
        back_to_ring();
        return true;
    }
    {
        const MouseState& mst = input_.get_mouse();
        if (mst.right_clicked && !battle_magic_select_ && !battle_item_select_ && !battle_weapon_select_) {
            if (battle_trace_) fprintf(stderr, "COMBAT: ring right-cancel unit=%d (si=-1, free)\n", u.id);
            battle_ring_cancel_to_free(ui);
            return true;
        }
    }

    // Esc: cancels a selection back to the ring, or cancels the menu (-1) to free wait.
    if (input_.is_key_pressed(SDLK_ESCAPE)) {
        if (battle_attack_select_ || battle_magic_select_ || battle_item_select_ ||
            battle_weapon_select_) {
            if (battle_trace_) fprintf(stderr, "COMBAT: select cancelled unit=%d\n", u.id);
            back_to_ring();
            return true;
        }
        if (battle_trace_) fprintf(stderr, "COMBAT: ring cancelled unit=%d (si=-1, free)\n", u.id);
        battle_ring_cancel_to_free(ui); // 4B842 + free wait (queue kept, gauge restored, state stays 1)
        return true;
    }

    // sub_5580F converts click coordinates to tiles via sub_109A3/sub_109CE after the camera shift.
    if (input_.was_left_clicked()) {
        const int mx = input_.get_mouse_x();
        const int my = input_.get_mouse_y();
        // Ring icon click first (sub_4BA7F: hit -> id, miss -> move (-2, only when
        // FDAF2 == 0 and within +-12 px)).
        // Skip the icon test while a submenu (magic/item/weapon) selection is open.
        if (!battle_magic_select_ && !battle_item_select_ && !battle_weapon_select_ &&
            battle_weapon_pending_si_ < 0) {
            int menu_id = -1;
            // The layout was already computed by the hover update above, but the camera
            // may have moved on the click frame, so recompute once.
            ring_compute_layout(ui);
            if (ring_handle_click(ui, mx, my, menu_id)) {
                if (std::find(battle_menu_ids_.begin(), battle_menu_ids_.end(), menu_id) ==
                    battle_menu_ids_.end()) {
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: click id %d not offered, ignored (4BA7F)\n", menu_id);
                    return false;
                } else {
                    // Banner off/on around the sub_4D674 dispatch (83309/83316, sub_44DEC).
                    battle_overlay_mode_ = 0; // sub_44DEC(0)
                    battle_ring_dispatch(ui, menu_id);
                    // Turn the banner back on when the dispatch returned to the ring
                    // (4D7EA/4DA7B). On turn end, move or attack targeting, that state
                    // manages the banner.
                    if (battle_player_unit_ == ui && !battle_free_wait_ &&
                        battle_move_phase_ == 0 && battle_atk_phase_ == 0 &&
                        ring_subkind_ == SubRingKind::None)
                        battle_overlay_mode_ = 1; // sub_44DEC(1)
                    return false;
                }
            }
            // Miss: -2 (move) only within +-12 px of the unit centre and when FDAF2 == 0
            // (80698..80725). Other clicks outside the ring are ignored.
            if (battle_simple_ring_) {
                if (battle_trace_) fprintf(stderr, "COMBAT: miss click ignored (FDAF2=1)\n");
                return false;
            }
            int ucx = 0, ucy = 0;
            unit_screen_center(u, ucx, ucy);
            if (mx < ucx - 12 || mx > ucx + 12 || my < ucy - 12 || my > ucy + 12) {
                if (battle_trace_) fprintf(stderr, "COMBAT: miss click outside +/-12px ignored (4BA7F)\n");
                return false;
            }
            // Within +-12 px -> enter move (the 5CF3E mouse warp is skipped in SDL).
        }
        const int tx = cam_tile_x_ + (mx - cam_fine_x_ - TILE_ANCHOR_X) / TOWN_TILE_W;
        const int ty = cam_tile_y_ + (my - cam_fine_y_ - TILE_ANCHOR_Y) / TOWN_TILE_H;
        if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return false;
        // 4A483 target cursor of a weapon special: when the picked tile is valid, that unit
        // becomes the target and the attack runs.
        if (battle_weapon_pending_si_ >= 0) {
            const int si = battle_weapon_pending_si_;
            const int kind = battle_weapon_pending_kind_;
            battle_pick_tx_ = tx;
            battle_pick_ty_ = ty;
            if (!battle_weapon_pick_valid(ui, kind)) {
                battle_pick_tx_ = battle_pick_ty_ = -1;
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: warning: weapon si=%d kind=%d cursor tile=(%d,%d) invalid, wait\n",
                            si, kind, tx, ty);
                return false;   // keep the 4A483 cursor
            }
            battle_weapon_pending_si_ = -1;
            battle_weapon_pending_kind_ = 0;
            battle_use_weapon(ui, si);
            battle_pick_tx_ = battle_pick_ty_ = -1;
            if (battle_tp_changed() && (size_t)ui < units_.size())
                units_[(size_t)ui].combat_acc57 = 0; // 83317
            back_to_ring();
            return false;
        }
        // Magic target selection (manual sub_28982 path: click after R is fixed).
        if (battle_magic_select_) {
            if (battle_magic_pending_r_ < 0) return false; // waiting for a spell number (ignore click)
            const int rc = battle_magic_pending_r_;
            const int si = battle_cast_magic(ui, rc, tx, ty);
            if (si == 0) {
                battle_magic_select_ = false;
                battle_magic_pending_r_ = -1;
                if (battle_tp_changed() && (size_t)ui < units_.size()) units_[(size_t)ui].combat_acc57 = 0; // 83317
                back_to_ring();
            } else if (si == 3) {
                // Retarget: the magic target cursor (sub_28982 -> sub_4A483) continues.
            } else {
                battle_magic_pending_r_ = -1; // keep the failure message, reselect the spell
            }
            return false;
        }
        // Item target selection (only warp dx=0 uses a target).
        if (battle_item_select_) {
            if (battle_item_pending_dx_ < 0) return false; // waiting for an item number
            const int dx = battle_item_pending_dx_;
            battle_item_select_ = false;
            battle_item_pending_dx_ = -1;
            const int si = battle_use_item(ui, dx, tx, ty);
            if (si == 3) {
                // 4A483 cursor loop: keep the cursor when the tile is not a destination
                // (warp / mount / dismount).
                battle_item_select_ = true;
                battle_item_pending_dx_ = dx;
                return false;
            }
            if (si == -4 || si == -3 || si == -1) { battle_var24_action_end_ = (si == -4); battle_player_end_turn(si == -4); }
            else {
                if (si > 0) battle_used_bits_ = (uint16_t)(battle_used_bits_ | (1u << (si - 1)));
                if (battle_tp_changed() && si != -3 && (size_t)ui < units_.size()) units_[(size_t)ui].combat_acc57 = 0; // 83317
                back_to_ring();
            }
            return false;
        }
        // Map click on the ring = move (sub_4BA7F -2 -> loc_4D98E): destination (sub_4A483)
        // then facing (sub_16357).
        battle_move_begin(ui);
        return false;
    }

    // ---- sub-ring digit fallback (the original is mouse-only; kept for verification) ----
    // With a sub-ring open, digits and M/I/V are ignored (select with click or Esc).
    // The M/I/V paging and digit paths apply only while the sub-ring is closed (hotkey
    // entry). Click entry and hotkey entry are separate paths; both converge on R/dx/si
    // and then a target click.
    if (ring_subkind_ != SubRingKind::None && ring_sub_open_) {
        for (int k = 0; k < 9; ++k) {
            if (!input_.is_test_key_pressed((SDL_Keycode)(SDLK_1 + k))) continue;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: number %d ignored (kind=%d, click used)\n",
                        k + 1, (int)ring_subkind_);
            return false;
        }
        // With a sub-ring open, M/I/V page changes are ignored (select with click or
        // digits). The original sub-ring is mouse-only too.
        if (input_.is_test_key_pressed(SDLK_m) || input_.is_test_key_pressed(SDLK_i) ||
            input_.is_test_key_pressed(SDLK_v))
            return false;
        // The main ring hotkeys (A/W/G/D/F/N/C) are ignored too; confirming or cancelling
        // the sub-ring comes first.
        if (input_.is_test_key_pressed(SDLK_a) || input_.is_test_key_pressed(SDLK_w) ||
            input_.is_test_key_pressed(SDLK_g) || input_.is_test_key_pressed(SDLK_d) ||
            input_.is_test_key_pressed(SDLK_f) || input_.is_test_key_pressed(SDLK_n) ||
            input_.is_test_key_pressed(SDLK_c) || input_.is_test_key_pressed(SDLK_RETURN))
            return false;
    }
    // ---- ring hotkeys (sub_4D442 dispatch; menu drawing in battle_player_show_menu) ----
    // A = attack (0): requires sub_1D124 and TP >= 40.
    if (input_.is_test_key_pressed(SDLK_a)) {
        if (!has_menu(0)) { if (battle_trace_) fprintf(stderr, "COMBAT: attack not offered unit=%d\n", u.id); return false; }
        battle_attack_begin(ui);                 // sub_4D442 id 0 -> sub_1F738
        return false;
    }
    // M = magic (6): group paging plus number selection, replacing the 4C72A 3-level
    // ring. The first M opens the first learned group; M while selecting advances a group
    // (a single group stays on the same page).
    if (input_.is_test_key_pressed(SDLK_m)) {
        if (!has_menu(6)) { if (battle_trace_) fprintf(stderr, "COMBAT: magic not offered unit=%d\n", u.id); return false; }
        if (!battle_magic_select_) {
            battle_magic_groups_.clear();
            for (int g = 0; g < 7; ++g)
                if (battle_spell_kind(u, g) >= 0) battle_magic_groups_.push_back(g);
            if (battle_magic_groups_.empty()) return false;
            battle_magic_page_ = 0;
            battle_magic_select_ = true;
            battle_magic_pending_r_ = -1;
            battle_attack_cancel();
            battle_item_select_ = false;
            battle_weapon_select_ = false;
        } else if (!battle_magic_groups_.empty()) {
            battle_magic_page_ = (battle_magic_page_ + 1) % (int)battle_magic_groups_.size();
            battle_magic_pending_r_ = -1;
        }
        battle_magic_show_page(ui);
        return false;
    }
    // I = item (7): pages of 7 plus number selection; I while selecting advances a page.
    if (input_.is_test_key_pressed(SDLK_i)) {
        if (!has_menu(7)) { if (battle_trace_) fprintf(stderr, "COMBAT: item not offered unit=%d\n", u.id); return false; }
        if (!battle_item_select_) {
            battle_item_full_options_ = battle_item_menu_ids(ui);
            if (battle_item_full_options_.empty()) return false;
            battle_item_page_ = 0;
            battle_item_select_ = true;
            battle_item_pending_dx_ = -1;
            battle_attack_cancel();
            battle_magic_select_ = false;
            battle_weapon_select_ = false;
        } else {
            const int npages = ((int)battle_item_full_options_.size() + 6) / 7;
            if (npages > 0) battle_item_page_ = (battle_item_page_ + 1) % npages;
            battle_item_pending_dx_ = -1;
        }
        battle_item_options_.clear();
        for (size_t s = (size_t)battle_item_page_ * 7;
             s < battle_item_full_options_.size() && s < (size_t)battle_item_page_ * 7 + 7; ++s)
            battle_item_options_.push_back(battle_item_full_options_[s]);
        return false;
    }
    // V = weapon submenu (1..5): 4C03E list plus 4C98A execution; 4CAA3 buzzes on an
    // empty list.
    if (input_.is_test_key_pressed(SDLK_v)) {
        bool offered = false;
        for (int id = 1; id <= 5; ++id) if (has_menu(id)) offered = true;
        if (!offered) { if (battle_trace_) fprintf(stderr, "COMBAT: weapon submenu not offered unit=%d\n", u.id); return false; }
        battle_weapon_options_.clear();
        battle_weapon_list_4c03e(u, battle_weapon_options_);
        if (battle_weapon_options_.empty()) {
            sfx(0x02); // sub_4CAA3 empty-list buzz
            if (battle_trace_) fprintf(stderr, "COMBAT: weapon submenu empty unit=%d (sub_4CAA3 buzz)\n", u.id);
            back_to_ring(); // si=0
            return false;
        }
        battle_weapon_select_ = true;
        battle_attack_cancel();
        battle_magic_select_ = false;
        battle_item_select_ = false;
        return false;
    }
    // W/Return = wait (8): heal via 4D3BA, then spend all TP.
    if (input_.is_test_key_pressed(SDLK_w) || input_.is_test_key_pressed(SDLK_RETURN)) {
        if (!has_menu(8)) { if (battle_trace_) fprintf(stderr, "COMBAT: wait not offered unit=%d\n", u.id); return false; }
        battle_wait_heal(ui); // sub_4D3BA (loc_4D4EB)
        battle_consume_tp(ui, battle_tp()); // 45B4E (all TP)
        if (battle_trace_) fprintf(stderr, "COMBAT: wait unit=%d\n", u.id);
        back_to_ring(); // si=0 -> TP < 10 -> end turn
        return false;
    }
    // G = guard (10): [57h] += 20 (cap 500), TP = 0, end turn (-3).
    if (input_.is_test_key_pressed(SDLK_g)) {
        if (!has_menu(10)) { if (battle_trace_) fprintf(stderr, "COMBAT: guard not offered unit=%d\n", u.id); return false; }
        if (u.combat_acc57 < 500) {
            u.combat_acc57 = (uint16_t)(u.combat_acc57 + 20);
            if (u.combat_acc57 > 500) u.combat_acc57 = 500;
        }
        battle_tp_set(0); // sub_1A0BF(0)
        if (battle_trace_) fprintf(stderr, "COMBAT: guard unit=%d acc57=%u (si=-3)\n", u.id, (unsigned)u.combat_acc57);
        battle_player_end_turn(false); // TP < 10 -> 4D95A end (keeps [57h])
        return false;
    }
    // D = defend 40 (9): 45B4E(40), returns 1 (bit 0).
    if (input_.is_test_key_pressed(SDLK_d)) {
        if (!has_menu(9)) { if (battle_trace_) fprintf(stderr, "COMBAT: defend40 not offered unit=%d\n", u.id); return false; }
        battle_consume_tp(ui, 0x28);
        battle_used_bits_ = (uint16_t)(battle_used_bits_ | 0x01); // si=1
        if (battle_tp_changed()) u.combat_acc57 = 0;
        if (battle_trace_) fprintf(stderr, "COMBAT: defend40 unit=%d\n", u.id);
        back_to_ring();
        return false;
    }
    // F = defend 60 (12): 45B4E(60), returns 2 (bit 1 = counter stance).
    if (input_.is_test_key_pressed(SDLK_f)) {
        if (!has_menu(12)) { if (battle_trace_) fprintf(stderr, "COMBAT: defend60 not offered unit=%d\n", u.id); return false; }
        battle_consume_tp(ui, 0x3C);
        battle_used_bits_ = (uint16_t)(battle_used_bits_ | 0x02); // si=2
        if (battle_tp_changed()) u.combat_acc57 = 0;
        if (battle_trace_) fprintf(stderr, "COMBAT: defend60 unit=%d\n", u.id);
        back_to_ring();
        return false;
    }
    // N = info (11).
    if (input_.is_test_key_pressed(SDLK_n)) {
        battle_show_info(ui); // sub_493A3 text version (si=0)
        return false;
    }
    // C = AI setting (13): switches [83h] to 1 (auto) - minimal sub_4D06E, for verification.
    if (input_.is_test_key_pressed(SDLK_c)) {
        if (!has_menu(13)) { if (battle_trace_) fprintf(stderr, "COMBAT: ai-config not offered unit=%d\n", u.id); return false; }
        ring_handle_ai_config(ui);
        return false;
    }
    // Digits 1..7: magic/item/weapon sub-selection.
    for (int k = 0; k < 7; ++k) {
        if (!input_.is_test_key_pressed((SDL_Keycode)(SDLK_1 + k))) continue;
        if (battle_magic_select_ && (size_t)k < battle_magic_options_.size()) {
            battle_magic_pending_r_ = battle_magic_options_[(size_t)k];
            const int r = battle_magic_pending_r_;
            if (battle_trace_) fprintf(stderr, "COMBAT: magic pick unit=%d R=%d %s\n", u.id, r, kSpellNames[r]);
            return false;
        }
        if (battle_item_select_ && (size_t)k < battle_item_options_.size()) {
            const int dx = battle_item_options_[(size_t)k];
            battle_item_pending_dx_ = dx;
            if (dx == 0 || dx == 5) {
                // Warp (3C7D1) and mount/dismount (3B7CE/3B614) open the target click cursor.
                return false;
            }
            battle_item_select_ = false;
            battle_item_pending_dx_ = -1;
            const int si = battle_use_item(ui, dx, -1, -1);
            if (si == -4 || si == -3 || si == -1) { battle_var24_action_end_ = (si == -4); battle_player_end_turn(si == -4); }
            else {
                if (si > 0) battle_used_bits_ = (uint16_t)(battle_used_bits_ | (1u << (si - 1)));
                if (battle_tp_changed() && si != -3 && (size_t)ui < units_.size())
                    units_[(size_t)ui].combat_acc57 = 0;
                back_to_ring();
            }
            return false;
        }
        if (battle_weapon_select_ && (size_t)k < battle_weapon_options_.size()) {
            // 4C98A(si = 4C03E menu id): gate, execute, back to the ring (TP charged only on
            // success). The digit selects the list position, the id is what 4C98A expects.
            battle_weapon_select_ = false;
            battle_use_weapon(ui, battle_weapon_options_[(size_t)k]);
            if (battle_tp_changed() && (size_t)ui < units_.size())
                units_[(size_t)ui].combat_acc57 = 0; // 83317
            // 4A483 target cursor pending: do not reopen the ring.
            if (battle_weapon_pending_si_ >= 0) return false;
            back_to_ring();
            return false;
        }
    }
    return false;
}

void CombatII::render_player_move_overlay() {
    if (battle_state_ != 1) return;
    // Free wait: hover box + HP only (49FE1 + 4A106); no ring or range.
    if (battle_free_wait_) {
        battle_render_free_hover();
        return;
    }
    if (battle_player_unit_ < 0 ||
        (size_t)battle_player_unit_ >= units_.size()) return;
    const CombatUnitPlacement& u = units_[(size_t)battle_player_unit_];
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    if (battle_move_phase_ != 0) { render_move_overlay(); return; }
    // sub_4D674 loc_4D88F: 4B9D7 (hide) then 4D442 -> 1F738, so the attack target phase
    // (1F19C/1F466/4A815) has no ring - only the range shading remains. battle_attack_begin
    // already ran ring_close_anim, so no ring is drawn here either.
    if (battle_atk_phase_ != 0) {
        // Return with just the attack cursor/hover marker (no ring or unit/panel over the dim).
    } else if (ring_subkind_ != SubRingKind::None && ring_sub_open_) {
        render_sub_ring();
    } else if (!battle_magic_select_ && !battle_item_select_ && !battle_weapon_select_) {
        render_ring();
    } else if (ring_subkind_ == SubRingKind::None) {
        // During a text selection without a sub-ring (G2_NO_RING fallback), hide only the main
        // ring.
    } else {
        render_sub_ring();
    }
    // Active unit (12F2A) + HP (13498) over the dim layer, drawn only while the ring dim is
    // active; draw_speaker_frame covers both the box and the HP.
    if (battle_ring_dim_active()) {
        draw_speaker_frame(battle_player_unit_);
        battle_render_ring_panels(battle_player_unit_);
    }
    if (battle_atk_phase_ != 0) {
        // render_attack_range() draws the range shading behind the tiles; only the cursor
        // and hover marker are added here.
        auto box = [&](int tx, int ty, int tw, int th, uint8_t color) {
            const int x0 = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
            const int y0 = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
            const int x1 = x0 + tw * TOWN_TILE_W - 1, y1 = y0 + th * TOWN_TILE_H - 1;
            auto put = [&](int x, int y) {
                if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) bb[y * SCREEN_W + x] = color;
            };
            for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
            for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
        };
        // sub_1F19C: box on the attacker first (type 5, 49E4E) - always drawn.
        box(u.x, u.y, 1, 1, 0x0C);
        if (battle_atk_phase_ == 3) {
            box(battle_line_cx_, battle_line_cy_, 2, 2, 0x0C);   // sub_49EEE 2x2 cursor
        } else if (battle_atk_phase_ == 4) {
            box(battle_atk_tx_ - 1, battle_atk_ty_ - 1, 3, 3, 0x0C);
        } else if (battle_atk_phase_ == 1) {
            // sub_1F19C(edx=1) single-frame hover: when the unit on the hovered tile is
            // hostile (1201D) only that target gets 49FE1, otherwise it falls back to the
            // attacker.
            const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
            int htx = -1, hty = -1;
            if (mx >= 0 && my >= 0) {
                htx = cam_tile_x_ + (mx - cam_fine_x_ - TILE_ANCHOR_X + 24 * 64) / TOWN_TILE_W - 64;
                hty = cam_tile_y_ + (my - cam_fine_y_ - TILE_ANCHOR_Y + 16 * 64) / TOWN_TILE_H - 64;
            }
            bool hover_ok = false;
            if (htx >= 0 && hty >= 0 && htx < (int)map_w_ && hty < (int)map_h_ &&
                (size_t)hty * map_w_ + (size_t)htx < battle_range_e8_.size() &&
                battle_range_e8_[(size_t)hty * map_w_ + (size_t)htx] >= 0) {
                const int occ = battle_unit_index_at(htx, hty);
                const bool in_guard = (unsigned)(htx - 2) < (unsigned)((int)map_w_ - 4) &&
                                      (unsigned)(hty - 4) < (unsigned)((int)map_h_ - 8);
                if (in_guard && occ >= 0 &&
                    battle_armies_hostile(battle_effective_army(u),
                                          battle_effective_army(units_[(size_t)occ]), battle_hostility_inverted_))
                    hover_ok = true;
            }
            if (hover_ok) box(htx, hty, 1, 1, 0x0C); // frame on the target only
            // Otherwise keep the attacker box drawn above
        } else if (battle_atk_phase_ == 2) {
            const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
            const int hx = cam_tile_x_ + (mx - cam_fine_x_ - TILE_ANCHOR_X + 24 * 64) / TOWN_TILE_W - 64;
            const int hy = cam_tile_y_ + (my - cam_fine_y_ - TILE_ANCHOR_Y + 16 * 64) / TOWN_TILE_H - 64;
            if (hx >= 0 && hy >= 0 && hx < (int)map_w_ && hy < (int)map_h_ &&
                (size_t)hy * map_w_ + (size_t)hx < battle_range_e8_.size() &&
                battle_range_e8_[(size_t)hy * map_w_ + (size_t)hx] >= 0)
                box(hx, hy, 1, 1, 0x0F);
        }
        return;
    }
    // render_player_move_range() draws the move range behind the base tiles and in front
    // of the units (sub_454B3 order). The original has no radius-3 dot fallback: the ring
    // phase (4D674) paints the 1CC09 range, and the move phase shows only the cursor when
    // the diffusion gate was not met.
    return;
}

// Diffusion step of sub_4D674 -> sub_16834: sub_14032 (blocker marks) plus sub_143F3
// (budget propagation). The budget is the AI's var_18*4 (var_18 = trunc((shared-10)/10),
// sub_40295 ECX); battle_ai_build_diffusion() reproduces the sub_14032 + sub_143F3
// decision step, so it is reused here.
void CombatII::battle_player_begin_overlay(int unit_index) {
    battle_player_diffusion_.clear();
    battle_player_diffusion_valid_ = false;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (map_w_ == 0 || map_h_ == 0) return;
    // loc_4D98E: budget = ((TP-var18)/10)*4 (83478..83489); var18 is refreshed by 167C9
    // during the turn.
    const int tp = (int)battle_ai_gauge_baseline_; // sub_1A0E9
    int budget = ((tp - battle_move_var18_) / 10) * 4;
    if (budget < 0) budget = 0;
    std::vector<int> grid = battle_ai_build_diffusion(unit_index, budget);
    if (grid.empty()) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: player overlay unit=%d no diffusion (mobility gate), radius-3 fallback\n",
                    units_[(size_t)unit_index].id);
        return;
    }
    // sub_16834 player diffusion (14032+143F3) leaves actor at budget (1430E).
    // battle_ai_build_diffusion bakes the 3CA3C-head refresh (+1) for AI scoring,
    // so restore the base value here; otherwise sub_15523 (E8==EDD72 arrival)
    // never hits and every destination is rejected.
    {
        const auto& au = units_[(size_t)unit_index];
        const int ax = (int)(int8_t)au.x, ay = (int)(int8_t)au.y;
        if (ax >= 0 && ay >= 0 && ax < (int)map_w_ && ay < (int)map_h_ &&
            (size_t)ay * (size_t)map_w_ + (size_t)ax < grid.size())
            grid[(size_t)ay * (size_t)map_w_ + (size_t)ax] = budget;
    }
    battle_player_diffusion_ = std::move(grid);
    battle_player_diffusion_valid_ = true; // sub_44D67(1) = word_FA8BC
    if (battle_trace_) {
        int nreach = 0;
        for (int v : battle_player_diffusion_) if (v >= 0) nreach++;
        fprintf(stderr, "COMBAT: player overlay unit=%d diffusion budget=%d (TP=%d var18=%d) reachable=%d\n",
                units_[(size_t)unit_index].id, budget, tp, battle_move_var18_, nreach);
    }
}

// sub_1547F: called by sub_454B3 (-> render_battle_map) behind the base tiles when
// word_FA8BC != 0. For every reached tile of the 13x13 viewport (E8 >= 0,
// `cmp word,0; jge`) it runs the whole 24x16 tile through LUT row 1 (sub_6104A(1)) via
// sub_10130.
// ---- TP (turn points) gauge arithmetic (sub_1A0E9/1A0BF/1A0C6/45B4E/1A0AD) ----
//   shared word (F0426+2) = battle_ai_gauge_baseline_, snapshot (F042A) =
//   battle_turn_start_tp_. sub_45B4E(unit, amount) clears [unit+57h] and then subtracts
//   amount in 5-point animation steps; only the net result matters for drawing, so the
//   port applies TP -= amount (floored at 0) with [57h] = 0.
void CombatII::battle_tp_consume(int n) {
    int v = (int)battle_ai_gauge_baseline_ - n;
    if (v < 0) v = 0;
    battle_ai_gauge_baseline_ = (int16_t)v; // sub_1A0C6 (0 clamp)
}

// sub_1A0F0 (14840..14854): edx = eax (new value). When F042F is 1 it plays sub_45ACF,
// otherwise sub_45A41, then stores the new value with `mov F042F, dl`. The transition is
// chosen from the previous value only (setting 1 while already 1 still runs 45ACF, as in
// the original):
//   previous 1 (clock) -> 45ACF: TIME_GRA #3 -> #2 -> #1 (the clock folds away)
//   anything else (TP gauge 0/2) -> 45A41: TIME_GRA #1 -> #2 -> #3 (unfolds)
// Original call sites: end of 19D53 (line 1914, entering the next unit's wait), just
// before the 4D674 ring display (2), and the 4DB4E exit (1).
void CombatII::battle_set_420f(int v) {
    battle_gauge_transition(battle_flag_420f_ != 1);
    battle_flag_420f_ = (uint8_t)(v & 0xFF);
}

// Body of sub_45A41 (ascending: cx=1..3) / sub_45ACF (descending: cx=3..1)
// (72234..72314):
//   word_7500E = 0 (hide the gauge/clock body) -> sub_1033B(0Ah)
//   do { sub_499E4 (wait 10 ticks); sub_454B3 (camera BA83C/BA838: map + units + full
//        sub_454D5 HUD/banner); sub_5EE51 (flat transparent blit of TIME_GRA #cx at
//        (75008>>16 + xo[cx], 7500C + yo[cx])); sub_5E9E8 (present) } while cx advances
//   word_7500E = 1; sub_45559 (redraw the map and present - F042F is still the old value).
// Deviation: the final sub_45559 frame still shows the old state, which the original
// immediately overwrites by the next draw; with vsync present that flickers for one frame,
// so the port skips the present and only restores word_7500E.
void CombatII::battle_gauge_transition(bool ascending) {
    if (!time_gra_tried_) load_time_gra();
    battle_gauge_visible_ = false; // mov word_7500E, 0
    game_timer_set(10);            // mov eax,0Ah; call sub_1033B
    for (int cx = ascending ? 1 : 3; ascending ? (cx < 4) : (cx > 0); cx += ascending ? 1 : -1) {
        wait_game_tick();          // call sub_499E4
        render_battle_frame();     // call sub_454B3 (falls into sub_454D5; gauge/clock skipped while 7500E=0)
        render_overlay();          // port-only text hint (not in the original) to avoid inter-frame flicker
        if (time_gra_loaded_) {
            // Movsx eax,cx; imul eax,0Ch -> FA6EA/FA6EC/FA6F0[cx*12] = xo/yo/ptr of TIME_GRA entry
            // cx
            if (const SpriteEntry* sp = time_gra_.get(cx)) {
                blit_flat_transparent(battle_gauge_x_ + (int)sp->x_offset,
                                      battle_gauge_y_ + (int)sp->y_offset, sp->pixel_data);
            } else if (battle_trace_) {
                fprintf(stderr, "COMBAT: warning: sub_45%s TIME_GRA entry %d missing (count=%d)\n",
                        ascending ? "A41" : "ACF", cx, time_gra_.count());
            }
        }
        video_.present();          // call sub_5E9E8
    }
    battle_gauge_visible_ = true;  // mov word_7500E, 1
    if (battle_trace_)
        fprintf(stderr, "COMBAT: sub_1A0F0 transition %s (F042F=%d)\n",
                ascending ? "45A41 #1->#3" : "45ACF #3->#1", (int)battle_flag_420f_);
}

void CombatII::battle_consume_tp(int unit_index, int amount) {
    if (unit_index >= 0 && (size_t)unit_index < units_.size())
        units_[(size_t)unit_index].combat_acc57 = 0; // mov [eax+57h],0
    battle_tp_consume(amount);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: consume TP unit=%d amount=%d remain=%d\n",
                unit_index >= 0 && (size_t)unit_index < units_.size() ?
                    units_[(size_t)unit_index].id : -1,
                amount, (int)battle_ai_gauge_baseline_);
}

// sub_167C9: 10 + 30 x (hostile units in the self+four-neighbour cross).
// (Walk of sub_1AC08/1AC59 + sub_11DB2 validity + sub_1201D hostility.)
// sub_167C9 calls sub_1AC08 with ebx=1, so only the radius-1 cross of 1AC08 is walked:
// (0,-1), (-1,0), (0,0), (1,0), (0,1) in oracle.diamond order. Emulator runs confirmed
// that south d2, diagonal d3 and east d8/d9 are not counted - only d1 adds 30.
int CombatII::battle_move_again_threshold(int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 10;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    static constexpr int kDx[5] = { 0, -1, 0, 1, 0 };
    static constexpr int kDy[5] = { -1, 0, 0, 0, 1 };
    int n = 0;
    for (int i = 0; i < 5; ++i) {
        const int x = ax + kDx[i], y = ay + kDy[i];
        if (x < 2 || y < 4 || x >= (int)map_w_ - 2 || y >= (int)map_h_ - 4) continue; // 11DB2 guard
        const int oi = battle_unit_index_at(x, y);
        if (oi < 0 || (size_t)oi >= units_.size()) continue;
        const auto& other = units_[(size_t)oi];
        if (!other.active || other.hp <= 0) continue;
        if (battle_armies_hostile(battle_effective_army(actor),
                                  battle_effective_army(other))) n++;
    }
    return 10 + 30 * n; // esi = 0Ah + 1Eh per hostile unit
}

// loc_4D98E cost: consume = var_18 + ((budget - E8[dest]) / 4) * 10.
// (16834 returns the remaining E8[dest] in eax; ebx = var_18 and ecx = 10 both survive the
// calls. `sar eax,2` rounds down. var_18 is the 167C9 refresh (10 + 30 per adjacent hostile),
// not the initial 10, so a re-move next to enemies costs more.)
int CombatII::battle_move_tp_cost(int budget, int e8dest) const {
    const int diff = budget - e8dest;
    return battle_move_var18_ + (diff >> 2) * 10;
}

// The sub_1D124 gate lives in combatii_player.cpp (battle_player_can_attack -> g2range).

// sub_4BD26: enumerate the main ring options in code order: 0, 1..5 by weapon class,
// 6, 7, 9, 12, 10, 11, 8, 13. The compact ring (word_FDAF2 == 1) has only {0x0D, 0x0B}
// (80868..80876): 0x0D = 13 (AI setting), 0x0B = 11 (info).
std::vector<int> CombatII::battle_player_menu_ids(int unit_index) {
    if (battle_simple_ring_) return std::vector<int>{13, 11};
    std::vector<int> ids;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return ids;
    const auto& u = units_[(size_t)unit_index];
    const int tp = battle_tp();
    const int cls = (int)u.combat_class;
    const int wc = weapon_class(u);
    const int kind = (int)sce_map_kind_;
    const int hp_raw = battle_hp_raw(u);
    const int max_raw = battle_max_hp_raw(u);
    // Id 0 (attack): 1D124 and TP >= 40 (0x28).
    if (battle_player_can_attack(unit_index) && tp >= 0x28) ids.push_back(0);
    // Ids 1..5 by weapon class: TP >= 70, code_b == 0, type < 0x12C, HP > 500, kind < 4.
    if (tp >= 0x46 && u.code_b == 0 && u.sprite_type < 0x12C && hp_raw > 500 && kind < 4) {
        // off_4BD0E jump table (80858..80870): index 0 -> class 2Fh/30h adds 5,
        // 1 -> [38h] adds 1, 2 -> nothing, 3 -> [39h] adds 2, 4 -> [3Ah] adds 3,
        // 5 -> [3Bh] adds 4. The counts sit one weapon class higher than the id.
        int sub = -1;
        if (wc == 0 && (u.combat_class == 0x2F || u.combat_class == 0x30)) sub = 5;
        else if (wc == 1 && u.weap_cnt_38 > 0) sub = 1;
        else if (wc == 3 && u.weap_cnt_39 > 0) sub = 2;
        else if (wc == 4 && u.weap_cnt_3a > 0) sub = 3;
        else if (wc == 5 && u.weap_cnt_3b > 0) sub = 4;
        if (sub >= 0) ids.push_back(sub);
    }
    // Id 6 (magic): MP > 0, TP >= 50, [1Eh]&8 == 0, kind < 4 and some g with kind >= 0.
    if (u.combat_mp_50 > 0 && tp >= 0x32 && !(u.combat_flags_1e & 8) && kind < 4) {
        for (int g = 0; g < 7; ++g)
            if (battle_spell_kind(u, g) >= 0) { ids.push_back(6); break; }
    }
    // Id 7 (item): TP >= 60, [12h] == 0, kind < 4, max/10 < cur, and type < 0x12C or code_b.
    if (tp >= 0x3C && u.item_use_12 == 0 && kind < 4 && max_raw / 10 < hp_raw &&
        (u.sprite_type < 0x12C || (u.code_b >= 1 && u.code_b <= 4) ||
         (u.code_b >= 0x0F && u.code_b <= 0x16))) ids.push_back(7);
    // Id 9 (defend 40): TP >= 40 and kind < 4.
    if (tp >= 0x28 && kind < 4) ids.push_back(9);
    // Id 12 (defend 60): TP >= 60, kind < 4, WC < 5 and 73B59[class] == 0.
    if (tp >= 0x3C && kind < 4 && wc < 5 && battle_class_73b59(cls, 0) == 0) ids.push_back(12);
    // Id 10 (guard): TP == [54h], type < 0x12C and kind < 4.
    if (tp == (int)u.combat_value_54 && u.sprite_type < 0x12C && kind < 4) ids.push_back(10);
    // Id 11 (info): always.
    ids.push_back(11);
    // Id 8 (wait/heal): TP > 20.
    if (tp > 0x14) ids.push_back(8);
    // Id 13 (move setting): always.
    ids.push_back(13);
   (void)cls;
    return ids;
}

// sub_28429: tier + charge -> kind (-1 excluded; 0..2).
int CombatII::battle_spell_kind(const CombatUnitPlacement& u, int group) const {
    if (group < 0 || group > 6) return -1;
    const int cls = (int)u.combat_class;
    const int tier = (cls >= 0 && cls < 165) ? (int)kSpellLearn[cls][group] : 0;
    int v = (tier ? 1 : 0) + (int)u.spell_charge_2e[group];
    if (v > 3) v = 3;
    return v - 1;
}

// sub_28480: TP threshold (T[d] for WC 2, otherwise 2*T[d]).
int CombatII::battle_spell_thr(const CombatUnitPlacement& u, int d) const {
    if (d < 0 || d > 6) return 0x7FFF;
    const int t = (int)kSpellTP[d];
    return weapon_class(u) == 2 ? t : t * 2;
}

// Ring display: the sub_4B767/4B8EB graphic ring plus the overlay text (fallback and
// verification). With the ring loaded it draws the icon circle (final layout of
// sub_4B0FB); the text stays as a hotkey hint, since the original is mouse-only.
void CombatII::battle_player_show_menu(int unit_index, bool first_entry) {
    battle_menu_ids_ = battle_player_menu_ids(unit_index);
    if (battle_trace_) {
        fprintf(stderr, "COMBAT: ring unit=%d ids=",
                unit_index >= 0 && (size_t)unit_index < units_.size() ?
                    units_[(size_t)unit_index].id : -1);
        for (int id : battle_menu_ids_) fprintf(stderr, "%d ", id);
        fprintf(stderr, "(TP=%d simple=%d)\n", battle_tp(), battle_simple_ring_ ? 1 : 0);
    }
    // The 4D674 ring-phase shading is the attack range (1CC09); the move range appears
    // only after 16834.
    battle_range_mark(unit_index); // the sub_1CC09 range (shared E8 grid, per facing/weapon)
    // First entry into sub_4D674: after 1CC09 -> 4B0F5 -> 4BD26 -> 4AD69 and before
    // 44D67(1)/4B767 (ring open), sub_1A0F0(2) plays the clock -> TP gauge transition
    // (45ACF/45A41) and sets F042F = 2. The 4D7EA redraw loop (ring returns after a move
    // or attack) does not call 1A0F0.
    if (first_entry) battle_set_420f(2);
    ring_open_anim(unit_index); // the sub_4B767/4B8EB ring open (sfx 0x1F + FDAEE=1 + layout)
}

// sub_4D582 (83023..83124): converts the sprite centre plus the wc5/0x0B facing offset
// back to a tile. Original order: D66A8 pose-0 entry (entry[4]/[5] + w/h/2 + 12/-2) -> store
// in [ecx]/[arg] -> 174CF (ecx=3 when wc is 5/0x0B, else 0) -> facing [63h] (0 south y+,
// 1 west x-, 2 north y-, 3 east x+) added to the *unit tile* -> 10971/10988 (current cam)
// -> compose the centre -> 109A3/109CE (current cam) -> tile -> 1124A. The +-3 is in tiles.
// 1-7 are all wc1 1x1, so off = 0 and the result is the unit tile or one tile north of it
// (the visual centre sits 3 px above the tile top and crosses the boundary). Y follows the
// visual centre (sy-2, draw+half); the +2 of unit_screen_center is relative to the trail
// (1E2EA) and 4 px lower.
static int floor_div_4d582(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
std::pair<int,int> CombatII::battle_4d582_tile(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return {cam_tile_x_ + 6, cam_tile_y_ + 6};
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (u.x < 0 || u.y < 0 || u.x >= (int)map_w_ || u.y >= (int)map_h_) return {u.x, u.y};
    // Tile top-left for the current cam (sub_10971/10988, TILE_ANCHOR 4/4).
    const int sx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int sy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    // sub_4D582 indexes D66A8 + 572*slot + 143*facing: the pose-0 entry, not [64h].
    const NC2Frame* fr = nc2_frame_for(u.sprite_type, u.facing & 3, 0);
    if (!fr) return {u.x, u.y}; // no NC2: the CITIZEN convention is unknown, use the unit tile
    // D66A8 raw footprint; large sprites use the same formula (the +-3 branch is only a wc gate).
    if (battle_trace_) {
        uint8_t raw[13];
        // battle_d66a8_raw shares the nc2_frame_for cache, so large sprites report measured w/h.
        int foot_w = fr->tile_w, foot_h = fr->tile_h;
        if (foot_w != 1 || foot_h != 1)
            fprintf(stderr, "COMBAT: 4D582 large footprint type=%d %dx%d (D66A8 raw table, tile derived from the visual centre)\n",
                    u.sprite_type, foot_w, foot_h);
        (void)raw;
    }
    // Visual centre: X = sx+12+x_off+w/2, Y = sy-2+y_off+h/2 (draw(sx+12,sy-2) + half).
    const int cx = sx + 12 + (int)fr->x_off + fr->w / 2;
    const int cy = sy - 2 + (int)fr->y_off + fr->h / 2;
    // Screen -> tile (sub_109A3/109CE, current cam, floor).
    int tx = cam_tile_x_ + floor_div_4d582(cx - cam_fine_x_ - TILE_ANCHOR_X, TOWN_TILE_W);
    int ty = cam_tile_y_ + floor_div_4d582(cy - cam_fine_y_ - TILE_ANCHOR_Y, TOWN_TILE_H);
    // 4D611..4D633: the wc 5/0Bh offset is added to the unit tile (bx/dx) and then fed through
    // sub_10971/10988 (tile -> screen) before 109A3/109CE, so it is 3 *tiles*, not 3 pixels.
    const int wc = weapon_class(u);
    const int off = (wc == 5 || wc == 0x0B) ? 3 : 0;
    switch (u.facing & 3) {
        case 0: ty += off; break; // south: y+
        case 1: tx -= off; break; // west: x-
        case 2: ty -= off; break; // north: y-
        default: tx += off; break; // east: x+
    }
    if (battle_trace_) {
        // Log only wc5/0x0B, large sprites and tile changes (1-7 are wc1 1x1 and match
        // the unit tile).
        if (off != 0 || tx != u.x || ty != u.y)
            fprintf(stderr, "COMBAT: 4D582 unit=%d tile=(%d,%d) center=(%d,%d) wc=%d off=%d(tiles)->tile=(%d,%d)\n",
                    u.id, u.x, u.y, cx, cy, wc, off, tx, ty);
    }
    return {tx, ty};
}
void CombatII::battle_4d582_center(int unit_index) {
    auto t = battle_4d582_tile(unit_index);
    camera_center_on(t.first, t.second); // sub_1124A (the later 49FE1 + 4A106 is drawn as hover/ring)
}

// Entering sub_4D674 (83125..83226): 4D582 + 1124A + 455BD + 1CC09 + ring + 44D67(1) +
// 4B767. The FDAF2 branch (83160..83167) takes the compact ring when [56h] != FF, or
// [56h] == FF and 19FA3 == 3. Called from free wait by clicking an ally or cycling with
// the right button; false rejects entry (info paths and so on).
bool CombatII::battle_player_enter_ring(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (!u.active || u.hp <= 0) return false;
    // sub_4D674 prologue (83130..83155):
    //   test byte [ebx+1Eh],10h -> loc_4DB76 (exit, no ring)
    //   test byte [eax+14h],2   -> loc_4DB76
    //   cmp byte [eax+83h],1 / jb -> 4D6B8; otherwise sub_4D06E(x,y) runs first and only a
    //   return 1 continues into the ring.
    if (u.combat_flags_1e & 0x10) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 4D674 reject unit=%d id=%d [1Eh]&10\n", unit_index, u.id);
        return false;
    }
    if (u.combat_flags_14 & 2) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 4D674 reject unit=%d id=%d [14h]&2\n", unit_index, u.id);
        return false;
    }
    const bool ai_screen_first = u.auto_battle >= 1; // [83h] >= 1
    // [56h] FDAF2 branch: ([56h] != FF) or ([56h] == FF and F042E == 3).
    const bool simple = (u.battle_gauge != 0xFF) || (u.battle_gauge == 0xFF && battle_state_ == 3);
    battle_simple_ring_ = simple;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 4D674 enter unit=%d id=%d gauge=%u state=%u FDAF2=%d\n",
                unit_index, u.id, (unsigned)u.battle_gauge, (unsigned)battle_state_, simple ? 1 : 0);
    battle_player_unit_ = unit_index;
    battle_free_wait_ = false;
    battle_free_hover_ = -1;
    battle_attack_cancel();
    battle_move_cancel();
    // sub_19FAB shared gauge - the TP baseline.
    battle_prepare_ai_gauge_baseline(u);
    if (!simple) {
        // FDAF2 == 0: save [56h] then clear it (4D6D7); the compact ring leaves the gauge alone.
        battle_saved_gauge_ = u.battle_gauge;
        u.battle_gauge = 0;
    } else {
        battle_saved_gauge_ = u.battle_gauge;
    }
    // var_28 (83198..83238): 2 when 73B59[class*9+0] == 1, else 0. It is ORed into [13h] on
    // the 197AD exit and picks the pose branch ([64h]=0 vs sub_12124).
    battle_used_bits_ = (battle_class_73b59((int)u.combat_class, 0) == 1) ? 2 : 0;
    battle_var24_action_end_ = false; // var_24=0
    battle_move_var18_ = 10; // var_18=0Ah (83243)
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    battle_magic_pending_r_ = -1;
    battle_item_pending_dx_ = -1;
    // sub_455BD: when the mouse covers the HUD panel at ring entry, move the panel to
    // another slot. Original call sites: 78827 (sub_4A483 destination cursor loop -
    // battle_move_input), 83210 (entry after 4D582), 83284 (ring display loop - the ring
    // hover block below) and 83407 (target cursor loop - battle_attack_input).
    battle_hud_panel_relocate(input_.get_mouse_x(), input_.get_mouse_y());
    // sub_4D582 + 1124A: D66A8 visual centre + wc5/0x0B facing offset -> tile; large
    // sprites use the raw footprint.
    battle_4d582_center(unit_index); // sub_1124A (the later 49FE1 + 4A106 is drawn as hover/ring)
    if (should_exit_) return true;
    // No diffusion in the ring phase; E8 is the 1CC09 range.
    battle_player_diffusion_.clear();
    battle_player_diffusion_valid_ = false;
    if (ai_screen_first) {
        // [83h] >= 1: sub_4D06E runs before the ring. Its only "return 1" is the 0Dh (AI off)
        // choice, which resets [83h] and reopens the ring (the AI1 handler's 13 branch calls
        // back_to_ring); cancel exits like sub_4D06E returning -1.
        if (battle_trace_) fprintf(stderr, "COMBAT: 4D674 [83h]=%u -> sub_4D06E first\n", (unsigned)u.auto_battle);
        ring_handle_ai_config(unit_index);
        return true;
    }
    battle_player_show_menu(unit_index, true); // 1CC09+4B0F5+4BD26+4AD69+1A0F0(2)+44D67(1)+4B767
    // sub_44D67(1): range shading on while the ring is shown - battle_range_mark lays it
    // down and the renderer draws it. sub_44DEC is turned off in the dispatch and back on
    // on return (handled in battle_ring_dispatch).
    return true;
}

// Ring cancel -1 goes loc_4D96F -> loc_4DAAF (the same settlement as 4DA7B), not to a
// separate free-wait path:
//   TP changed (1A0AD == 1) -> 4D3BA heal when TP >= 40, 197AD (queue removed, [56h]=0),
//   [13h] |= var_28 (plus class 30h -> bit 2) and the var_28 pose branch.
//   TP unchanged -> loc_4DB44 restores [56h] and keeps the queue; only the 4DB4E tail runs
//   (ring close, 1A0F0(1), free wait).
// word_FDAF2 != 0 short-cuts to loc_4DB4E, which battle_player_end_turn already models.
void CombatII::battle_ring_cancel_to_free(int unit_index) {
    if (unit_index >= 0 && (size_t)unit_index < units_.size()) {
        if (battle_tp_changed()) {
            battle_player_unit_ = unit_index;
            battle_player_end_turn(false);
            return;
        }
        units_[(size_t)unit_index].battle_gauge = battle_saved_gauge_;
    }
    // Ring close is a 9-frame hide (36 -> 0) from 4B9D7 right after a command; distinct
    // from the turn-end 4B842 (36 -> 276).
    ring_close_anim();
    ring_sub_close();
    battle_attack_cancel();
    battle_move_cancel();
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    battle_magic_pending_r_ = -1;
    battle_item_pending_dx_ = -1;
    battle_used_bits_ = 0;
    battle_var24_action_end_ = false;
    battle_menu_ids_.clear();
    battle_player_diffusion_.clear();
    battle_player_diffusion_valid_ = false;
    battle_range_e8_.clear();
    // loc_4DB4E: every exit from 4D674 (cancel included) closes the ring (4B842), settles
    // the gauge and then runs the TP gauge -> clock transition (45A41) once via
    // sub_1A0F0(1).
    battle_set_420f(1);
    battle_player_unit_ = -1;
    battle_free_wait_ = true; // enter free wait (F042E stays 1, time frozen)
    battle_state_ = 1;
    battle_overlay_mode_ = 1; // keep the banner on (44DEC(1))
    if (battle_trace_) fprintf(stderr, "COMBAT: ring cancel -> free wait (4B842+5580F)\n");
}

// MENUSTAT.SYB (sub_4E448: loads 0x37 sprites from `.\\menustat.syb` with sub_4F25F, then
// sub_10031). The file is 320-stride RLE, so the port blits it with blit_sprite_rle
// unchanged; the original sub_10031 converts it for the 480-wide canvas.
bool CombatII::load_menustat() {
    menustat_tried_ = true;
    menustat_loaded_ = false;
    std::string cand = game_path("MENUSTAT.SYB");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "MENUSTAT.SYB");
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: MENUSTAT.SYB not found (info panel skipped)\n");
        return false;
    }
    if (!menustat_.load(cand) || menustat_.count() <= 36) {
        fprintf(stderr, "COMBAT: warning: MENUSTAT.SYB load failed or too few sprites (%d)\n", menustat_.count());
        return false;
    }
    menustat_loaded_ = true;
    return true;
}

// sub_4868A (dasm 75758..): draws a vsprintf'd string in the TEXT.CHR small font
// (sub_608E6, spacing 1) at (x-1,y) (x+1,y) (x,y-1) (x,y+1) in colour 6, then (x,y) in
// the requested colour.
void CombatII::draw_small_text_outlined(int x, int y, const char* s, uint8_t color) {
    draw_small_text(x - 1, y, s, 6, 1);
    draw_small_text(x + 1, y, s, 6, 1);
    draw_small_text(x, y - 1, s, 6, 1);
    draw_small_text(x, y + 1, s, 6, 1);
    draw_small_text(x, y, s, color, 1);
}

// sub_48734 (dasm 75990..76110): non-ally info window. There is no frame of its own; the
// items below are drawn in order over the dimmed background:
//   1) sub_48132(0x4C, 0x78, [unit+10h], 0)      portrait (x centred, y = bottom)
//   2) sub_5E048(0x4C - 4*len, 0x5D, "%s", [+2]) name - the only line in the Johab
//                                                dialogue font
//   3) sub_5F3FD(FD7AC+0x4C, FD7AE+0xA4, FD7B0)  MENUSTAT.SYB #36 (xo/yo from +4/+6)
//   4) sub_4868A outlined TEXT.CHR small-font text in colour 0x11:
//        class name (0x4C - 3*len, 0x7E)  aKnight + [+11h]*16
//        level      (0x60, 0x92) "%3d"      [+2Bh]
//        HP         (0x3C, 0x9D) "%4d/%4d"  current = ceil([+48h]/100) (sub_13853),
//                                           max = [+4Ch]/100 (floor)
//        MP         (0x3C, 0xA7) "%4d/%4d"  current = word[+50h], max = word[+52h]
//        experience (0x60, 0xB1) "%3d"      [+3Dh]
void CombatII::battle_draw_unit_info_panel(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    if (!small_font_tried_) load_small_font();
    if (!menustat_tried_) load_menustat();

    // 1) portrait
    if (u.face >= 0) show_portrait(u.face, 0x4C, 0x78);
    // 2) name
    if (!u.name.empty()) {
        int nx = 0x4C - 4 * (int)u.name.size();
        if (nx < 0) nx = 0;
        draw_text_wrapped(nx, 0x5D, u.name);
    }
    // 3) MENUSTAT #36
    if (menustat_loaded_) {
        const SpriteEntry* sp = menustat_.get(36);
        if (sp && sp->pixel_data) blit_sprite_rle(0x4C + sp->x_offset, 0xA4 + sp->y_offset, sp->pixel_data);
    }
    // 4) small-font numbers
    const char* cls_name = combat_class_name(u.combat_class);
    draw_small_text_outlined(0x4C - 3 * (int)strlen(cls_name), 0x7E, cls_name, 0x11);

    char buf[64];
    snprintf(buf, sizeof(buf), "%3d", (int)u.combat_level_2b);
    draw_small_text_outlined(0x60, 0x92, buf, 0x11);

    const int hp_cur = u.hp;                                          // ceil(raw/100) = sub_13853
    const int hp_max = (u.max_hp * 100 - u.max_hp_deficit) / 100;     // [+4Ch]/100 floored
    snprintf(buf, sizeof(buf), "%4d/%4d", hp_cur, hp_max);
    draw_small_text_outlined(0x3C, 0x9D, buf, 0x11);

    snprintf(buf, sizeof(buf), "%4d/%4d", (int)(int16_t)u.combat_mp_50, (int)(int16_t)u.combat_mp_max_52);
    draw_small_text_outlined(0x3C, 0xA7, buf, 0x11);

    snprintf(buf, sizeof(buf), "%3d", (int)u.exp_3d);
    draw_small_text_outlined(0x60, 0xB1, buf, 0x11);
}

// sub_48083(fmt): centred text band. Dims (0,0x50)-(0x140,0x78) twice and prints the text
// at (0xA0 - 4*len, 0x5C). The original draws straight onto the backbuffer and presents
// with sub_5E9E8; the caller does the subsequent 499EF wait.
void CombatII::battle_text_band(const std::string& text, bool redraw_frame) {
    if (redraw_frame) render_battle_frame();         // previous frame (the original reuses the backbuffer)
    render_dialog_dimming(0, 0x50, 0x13F, 0x78 - 0x50);   // first pass: sub_5EDDE(0,0x50,0x13F,0x78), width = x2-x1 = 0x13F
    render_dialog_dimming(0, 0x50, 0x13F, 0x78 - 0x50);   // second pass (same arguments again)
    int x = 0xA0 - 4 * (int)text.size();             // mov ebx,0A0h; sub ebx,len*4; movsx ebx,bx
    draw_text_wrapped(x, 0x5C, text);                // sub_5E048(x, 0x5C, "%s")
    video_.present();                                // sub_5E9E8
    dump_frame_if_requested();
}

// sub_482A5(unit, Johab string): dialogue window with the unit's portrait and name. At the
// end sub_499EF(0x2BC) -> sub_1033B(2) -> sub_4A321.
void CombatII::battle_unit_message(int unit_index, const std::string& johab_text) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    show_dialogue_line(johab_text, u.face, u.name, u.x, u.y, unit_index);
    video_.present();
    dump_frame_if_requested();
    wait_info_dismiss(0x2BC);                        // mov eax,2BCh; call sub_499EF
    game_timer_set(2);                               // sub_1033B(2)
}

// sub_482A5(eax = unit record, edx = Johab string): takes the portrait [10h], name [2..]
// and tile [0],[1] from the record (sub_49FE1 finds the on-screen actor from the tile
// again). sub_17A20 switches ebp to the stored [77h] record when one exists, so a
// transformed or mounted unit shows its original portrait and name in kill/level-up
// messages.
void CombatII::battle_unit_message(const CombatUnitPlacement& rec, const std::string& johab_text) {
    show_dialogue_line(johab_text, rec.face, rec.name, rec.x, rec.y, -1);
    video_.present();
    dump_frame_if_requested();
    wait_info_dismiss(0x2BC);                        // mov eax,2BCh; call sub_499EF
    game_timer_set(2);                               // sub_1033B(2)
}

// sub_18D36 success path: move the camera to the event tile, draw only a marker there and
// wait 0x1F4 ticks.
void CombatII::battle_search_marker(int unit_index, int tx, int ty) {
    (void)unit_index;
    camera_center_on(tx, ty);                        // sub_1124A
    battle_dim_row0_ = true;                         // sub_44D6E(1)
    render_battle_frame();                           // sub_454B3
    battle_dim_row0_ = false;
    battle_draw_tile_marker(tx, ty);                 // sub_49EEE (49E4E dl=5: 24x16)
    video_.present();                                // sub_5E9E8
    dump_frame_if_requested();
    wait_info_dismiss(0x1F4);                        // sub_44D6E(0) then sub_499EF(0x1F4)
}

// The sub_499EF wait: returns on any key (getch_, modifiers excluded), a left click, or the
// tick timeout. Unlike wait_key_or_ticks, Esc does not set skip_cutscene_, so battle input
// cannot leak into a cutscene skip.
bool CombatII::wait_info_dismiss(int ticks) {
    game_timer_set(ticks);
    while (true) {
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return true; }
        // sub_499EF: polls mouse events and keys (getch_) through sub_5D22E/sub_5D839 and
        // returns on any key or a left click.
        if (input_.was_left_clicked() || input_.any_getch_key_pressed()) {
            // The original pops the event (getch_/5D22E) before returning, so this click or
            // key is not seen again by the caller in the same frame (free wait, ring,
            // system menu).
            input_.consume_event();
            return false;
        }
        if (game_timer_poll()) return false;
        SDL_Delay(auto_advance_ ? 0 : 1);
    }
}

// sub_493A3 (dasm 77001..77035): click info for a non-ally.
//   sub_103A8 (weather tick off) -> 44DEC(0) (banner off) -> 44D6E(1) (dim on)
//   -> (48E90 interactive equipment when [0Eh]==1; not used for enemies)
//   454B3 (frame with the dim) -> 48734 -> 5E9E8 -> 499EF(0x4B0)
//   -> 44D6E(0) -> 44DEC(1) -> sub_103A0 (weather tick on)
// The original blocks in 499EF, so the dim and the info window stay up for the whole wait.
void CombatII::battle_show_info_modal(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    weather_tick_enabled_ = false;                // sub_103A8
    battle_overlay_mode_ = 0;                     // sub_44DEC(0)
    battle_dim_row0_ = true;                      // sub_44D6E(1)
    const int saved_hover = battle_free_hover_;   // the 454B3 frame has no hover box
    battle_free_hover_ = -1;
    render_battle_frame();                        // sub_454B3 (including dim)
    battle_dim_row0_ = false;                     // the dim is baked into the frame (same as show_scenario_condition)
    battle_draw_unit_info_panel(unit_index);      // sub_48734
    video_.present();                             // sub_5E9E8
    dump_frame_if_requested();
    if (battle_trace_) fprintf(stderr, "COMBAT: info modal unit=%d (493A3/48734)\n", units_[(size_t)unit_index].id);
    wait_info_dismiss(0x4B0);                     // sub_499EF(4B0h)
    battle_free_hover_ = saved_hover;
    battle_overlay_mode_ = 1;                     // sub_44DEC(1)
    weather_tick_enabled_ = true;                 // sub_103A0
}

// sub_4B744(eax = unit, ebx = kind): 1 -> sub_4B18F (magic list panel), 2 -> sub_4B4CA
// (AI setting summary band), 0 -> nothing. The base, item and special rings (ebx=0) have
// none; only magic level 1 uses kind 1 and AI levels 1/2 use kind 2. The active unit
// (12F2A) + HP (13498) over the dim layer are drawn by the renderer before this call.
void CombatII::battle_render_ring_panels(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (ring_subkind_ == SubRingKind::MagicG && ring_sub_open_) {
        battle_panel_magic_4b18f(unit_index);
        return;
    }
    if ((ring_subkind_ == SubRingKind::AI1 || ring_subkind_ == SubRingKind::AI2) && ring_sub_open_) {
        battle_panel_ai_4b4ca(unit_index);
        return;
    }
}

// One label line drawn by calling sub_484F9 directly with frame = 0x24 (36); shared by
// sub_4B18F and sub_4B4CA.
//   For frame >= 8 sub_484F9 starts from the leftover in stack slot var_4
//   ([esp+14h+var_4]); only frame < 8 initialises it to 8-frame. When the leftover di as a
//   signed word is > 0x11-6 the argument colour is used; when it is <= 7 the low byte of
//   the leftover becomes the colour.
//   The residue is whatever last touched that stack depth; each caller derives it below.
//   Returns the slot value after the call, which the next sub_484F9 call at the same depth
//   inherits.
int16_t CombatII::battle_panel_label(int x, int y, const char* text, int color_arg, int16_t residue) {
    int16_t var4 = residue;
    battle_label_draw_frame(x, y, text, 0x24, color_arg, var4);
    return var4;
}

// sub_4B18F: magic list panel (dasm 79873..80145).
//   Colour by kind (sub_28429): 0 -> 0x11, 1 -> 0x31, 2 -> 0x50. Insufficient MP
//   (word[+50h] < word_753B6[d]) or TP (sub_28480(d) > word[+57h] + sub_1A0E9()) makes the
//   line grey 0x24.
//   A line is the name (off_7423C[(g*7+d)*3+kind], centred) plus the MP cost
//   (word_753B6[d] = kSpellCost[d], decimal, centred at x+0x2D). The number is 0x24 when the
//   name is grey, else 1. Line spacing 8 px.
//   - Normal: 2 levels x 3 columns. Level 1 g=0,1,2 at y 9; level 2 g=3,4 and the last
//     column g=6 at y 0x8C. Columns start at x 0x3C and step +0x5C (an unlearned column,
//     kind < 0, draws nothing and steps +0x5A).
//   - Special (class [+11h] 0x29..0x2B): one column for group 5, name x 0x98, number
//     x 0xC5, y 9 + 8*d.
//   sub_484F9 residue: between calls itoa_/sub_28429/sub_28480 (-> sub_174CF) overwrite the
//   same stack depth, so the original value cannot be pinned down statically. It is set to
//   a large positive value, matching the intent that the argument colour is used. For the
//   special path the original derives the number's y from edx after itoa_ with
//   `sar edx,10h`; itoa_ is not in this listing, so the port reuses the name's y.
void CombatII::battle_panel_magic_4b18f(int unit_index) {
    const auto& u = units_[(size_t)unit_index];
    if (!small_font_tried_) load_small_font();
    constexpr int16_t kResidue = 0x100;

    auto entry = [&](int xname, int xnum, int y, int g, int d, int kind) {
        const bool gray_mp = (int16_t)u.combat_mp_50 < (int)kSpellCost[d];
        const bool gray_tp = !gray_mp &&
            battle_spell_thr(u, d) > (int)(int16_t)u.combat_acc57 + (int)(int16_t)battle_tp();
        int color = 0x24;
        if (!gray_mp && !gray_tp) color = (kind == 0) ? 0x11 : (kind == 1) ? 0x31 : 0x50;
        const int r = (g * 7 + d) * 3 + kind;
        const char* name = (r >= 0 && r < 147) ? kSpellNames[r] : "";
        battle_panel_label(xname, y, name, color, kResidue);
        char num[16];
        snprintf(num, sizeof(num), "%d", (int)kSpellCost[d]);
        battle_panel_label(xnum, y, num, color == 0x24 ? 0x24 : 1, kResidue);
    };

    if (u.combat_class == 0x29 || u.combat_class == 0x2A || u.combat_class == 0x2B) {
        const int kind = battle_spell_kind(u, 5);
        if (kind < 0) return;
        for (int d = 0; d < 7; ++d) entry(0x98, 0xC5, 9 + 8 * d, 5, d, kind);
        if (battle_trace_) fprintf(stderr, "COMBAT: magic panel special unit=%d kind=%d (4B18F)\n", u.id, kind);
        return;
    }
    int g = 0;
    for (int tier = 0; tier < 2; ++tier) {
        const int y0 = tier == 0 ? 9 : 0x8C;
        int x = 0x3C;
        for (int c = 0; c < 3; ++c) {
            if (tier == 1 && c == 2) g = 6;
            const int kind = battle_spell_kind(u, g);
            if (kind < 0) { x += 0x5A; ++g; continue; }
            for (int d = 0; d < 7; ++d) entry(x, x + 0x2D, y0 + 8 * d, g, d, kind);
            x += 0x5C;
            ++g;
        }
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: magic panel unit=%d (4B18F)\n", u.id);
}

// sub_4B4CA: AI setting summary band (dasm 80146..80369), in the original draw order.
//   y = unit.y > cam.y+6 ? 8 : 0xA8. sub_5EDDE(0, y-2, 0x13F, y+0x1A) dims row 0
//   (width 0x13F = 319, 28 rows).
//   Left ([73h] target pointer, else [6Fh] != FF, else the [71h]/[72h] watch target, else
//   "No Target."):
//     target unit : sub_5F3FD(0x18, y, D66A8[slot*572 + 9] = facing 0, pose 0 frame) +
//                   sub_5E048(0x38, y+4, "%s", name)
//     [6Fh] != FF : sub_5E048(0x18, y+4, "<3 Johab chars> [%d %d]", [6Fh], [70h])
//     otherwise   : sub_5E048(0x38, y+4, "No Target.")
//   Centre: [80h]=a != FF -> sub_484F9(0xC0, y+6, "[AI Magic]", 0x24, 0x11) +
//            sub_484F9(0xC0, y+0xE, off_7423C[3a+kind], 0x24, dword_4AE4A[kind])
//            (kind = sub_28429(unit, a/7))
//           otherwise -> sub_484F9(0xC0, y+0xC, "[Normal Sequence]", 0x24, 0x11)
//   Right: [82h]=v != FF -> sub_4AC28(MAINRING #category, 0x104, y+0xC) +
//          sub_4AC28(SPECIAL #v, 0x118, y+0xC), where category = v<7 -> 1, <0xB -> 2,
//          <0xF -> 3, <0x13 -> 4, else 5.
//   dword_4AE4A = {0x11, 0x31, 0x50}, read straight from the EXE.
//   sub_484F9 residue (var_4 slot = this function's esp-24) is left by sub_5E048, which last
//   touched that depth:
//     - 4-argument call (name, slot = first pushed ebx): low word of ebx. On the target-unit
//       path sub_5F3FD just ran `dec bx; jnz` down to 0, so it is 0.
//     - 3-argument call ("No Target.", slot = second pushed edx): low word of edx. edx is 0
//       on the [73h]==0 path and [72h] on the sub_1379A failure path (dl untouched).
//     - 5-argument call ("[%d %d]", slot = return address): low word of 0x4B581, the address
//       after `call sub_5E048`. This assumes the load base matches the IDA VA; the DOS/4G
//       load address cannot be pinned down from the EXE.
//   A residue of 0 draws with body colour 0 (black) and outline 6, as the original does.
void CombatII::battle_panel_ai_4b4ca(int unit_index) {
    const auto& u = units_[(size_t)unit_index];
    if (!small_font_tried_) load_small_font();
    const int y = ((int)(int8_t)u.y > cam_tile_y_ + 6) ? 8 : 0xA8;
    render_dialog_dimming(0, y - 2, 0x13F, 0x1C);           // sub_5EDDE(0, y-2, 0x13F, y+0x1A)

    constexpr int16_t kRetAddrLow4B581 = (int16_t)0xB581;
    static const int kKindColor[3] = {0x11, 0x31, 0x50};     // dword_4AE4A
    int16_t residue = 0;

    auto unit_portrait_and_name = [&](const CombatUnitPlacement& t) {
        if (const NC2Frame* fr = nc2_frame_for(t.sprite_type, 0, 0)) blit_nc2_frame(0x18, y, *fr);   // sub_5F3FD
        draw_text_wrapped(0x38, y + 4, t.name);                                                       // sub_5E048(0x38, y+4, "%s")
        residue = 0;                                          // sub_5F3FD leaves bx at 0
    };

    if (u.ai_target_unit >= 0 && (size_t)u.ai_target_unit < units_.size()) {
        unit_portrait_and_name(units_[(size_t)u.ai_target_unit]);
    } else if (u.ai_home_x != 0xFF) {
        char buf[64];
        snprintf(buf, sizeof(buf), "\xA1\xA2\xB8\xE2\xBB\xA1 [%d %d]", (int)u.ai_home_x, (int)u.ai_home_y);
        draw_text_wrapped(0x18, y + 4, buf);                  // format string at EXE 0x77EA4 (3 Johab chars + " [%d %d]")
        residue = kRetAddrLow4B581;
    } else {
        int found = -1;
        if (u.ai_watch_sel != 0xFF)   // sub_1379A(sel, id): CC960[sel] linked list
            found = battle_ai_cc960_member((int)u.ai_watch_sel, (int)u.ai_watch_id);
        if (found >= 0) {
            unit_portrait_and_name(units_[(size_t)found]);
        } else {
            draw_text_wrapped(0x38, y + 4, "No Target.");
            residue = (u.ai_watch_sel == 0xFF) ? (int16_t)0 : (int16_t)u.ai_watch_id;
        }
    }

    if (u.ai_field_80 != 0xFF) {
        const int a = u.ai_field_80;
        const int kind = battle_spell_kind(u, a / 7);           // sub_28429(unit, a/7)
        const int r = 3 * a + kind;
        const int color = (r >= 0) ? kKindColor[r % 3] : kKindColor[0];
        const char* name = (r >= 0 && r < 147) ? kSpellNames[r] : "";
        residue = battle_panel_label(0xC0, y + 6, "[AI Magic]", 0x11, residue);
        battle_panel_label(0xC0, y + 0xE, name, color, residue);
    } else {
        battle_panel_label(0xC0, y + 0xC, "[Normal Sequence]", 0x11, residue);
    }

    const uint8_t v = (uint8_t)u.ai_mode;
    if (v != 0xFF && ring_load_icons()) {
        const int cat = v < 7 ? 1 : v < 0xB ? 2 : v < 0xF ? 3 : v < 0x13 ? 4 : 5;
        ring_draw_icon_4ac28(ring_main_, cat, 0x104, y + 0xC);      // unk_FD9A8 (mainring.ico)
        ring_draw_icon_4ac28(ring_special_, v, 0x118, y + 0xC);     // unk_FDA12 (special.ico)
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI band y=%d unit=%d target=%d home=(%d,%d) watch=%d/%d a=%d mode=%d (4B4CA)\n",
                y, u.id, u.ai_target_unit, (int)u.ai_home_x, (int)u.ai_home_y,
                (int)u.ai_watch_sel, (int)u.ai_watch_id, (int)u.ai_field_80, (int)v);
}

// Free-wait hover box + HP (49FE1 + 4A106; 5580F 94296..94310, 94333..94340). Draws the
// box and HP for the unit on the mouse tile (y+8), ally or enemy.
void CombatII::battle_render_free_hover() {
    if (!battle_free_wait_ || battle_state_ != 1) return;
    if (battle_free_hover_ < 0 || (size_t)battle_free_hover_ >= units_.size()) return;
    draw_speaker_frame(battle_free_hover_); // the 49FE1 + 4A106 + 13498 box and HP
}

// sub_5580F free wait (94275..94538): hover, left click, right click, Space.
// Hover runs 49FE1 + 4A106 on every new tile (tile y+8); a left click with [0Eh]==1 enters
// 4D674, otherwise 493A3; a right click with 19FA3==1 does 1979F + 4D674; Space with
// 19FA3 != 0 and 10392 == 1 does 19F9D(0) + 1038B + 44DEC(0). Returns true when the input
// was consumed.
bool CombatII::battle_free_input() {
    if (!battle_free_wait_) return false;
    const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
    // Esc in free wait is not consumed; the caller (the 5580F loop) opens the system menu
    // (4E0EE).
    if (input_.is_key_pressed(SDLK_ESCAPE)) return false;
    // Screen -> tile: sub_109A3(mouse x) uses `idiv 24` (truncate toward zero), while
    // sub_109CE(mouse y + 8) uses `sar 4` (floor). Free wait maps y+8 (the sprite body);
    // 1F19C does not add 8.
    auto free_tile = [&](int px, int py, int& tx, int& ty) {
        const int divx = px - cam_fine_x_ - TILE_ANCHOR_X;
        const int divy = (py + 8) - cam_fine_y_ - TILE_ANCHOR_Y;
        tx = cam_tile_x_ + divx / TOWN_TILE_W;
        ty = cam_tile_y_ + (divy >= 0 ? divy / TOWN_TILE_H : -((-divy + TOWN_TILE_H - 1) / TOWN_TILE_H));
    };
    int htx = -1, hty = -1;
    if (mx >= 0 && my >= 0) free_tile(mx, my, htx, hty);
    // Hover: 49FE1 + 4A106 on every new tile (ally or enemy, inside the 11DB2 guard).
    int hover = -1;
    if (htx >= 2 && hty >= 4 && htx < (int)map_w_ - 2 && hty < (int)map_h_ - 4) {
        hover = battle_unit_index_at(htx, hty);
    } else if (htx >= 0 && hty >= 0 && htx < (int)map_w_ && hty < (int)map_h_) {
        // Outside the guard 11DB2 = 0, so no hover (the original returns 0 off-grid too).
        hover = -1;
    }
    battle_free_hover_ = hover;
    // Right click: 19FA3()==1 -> sub_1979F(cx) + 4D674 (loc_55AC7). cx is the loop-local
    // index into the ready list: pick entry cx, advance cx, and when cx runs past the end
    // fall back to entry 0 and set cx=1 (sub_19779 walks dword_F041C and returns 0 at the
    // end). The list order itself never changes; resetting to 0 belongs to the 19D53==2 path.
    const MouseState& mst = input_.get_mouse();
    if (mst.right_clicked) {
        // 19FA3()==1 gate (only this branch tests the state).
        if (battle_state_ != 1) return false;
        const int n = (int)battle_ready_queue_.size();
        if (n > 0) {
            auto entry = [&](int idx) -> int {
                if (idx < 0 || idx >= n) return -1;
                const int v = battle_ready_queue_[(size_t)idx];
                return (v >= 0 && (size_t)v < units_.size()) ? v : -1;
            };
            const int cx = battle_free_cycle_;
            int nxt = entry(cx);
            ++battle_free_cycle_;
            if (nxt < 0) {
                nxt = entry(0);
                battle_free_cycle_ = 1;
            }
            if (battle_trace_)
                fprintf(stderr, "COMBAT: free right-click cx=%d -> unit=%d (1979F+4D674)\n", cx, nxt);
            if (nxt >= 0) battle_player_enter_ring(nxt);
            return false;
        }
        return false;
    }
    // Left click (loc_55A6F): sub_109A3(mouse x) + sub_109CE(mouse y + 8) is the only
    // candidate tile, the same one the hover pass uses. sub_11DB2 returns 0 outside its guard
    // or on an empty tile, so those clicks are ignored; otherwise [0Eh]==1 -> 4D674, else
    // 493A3.
    if (input_.was_left_clicked()) {
        if (mx < 0 || my < 0) return false;
        const int occ = hover;
        if (occ < 0) return false;
        const auto& u = units_[(size_t)occ];
        const int eff = battle_effective_army(u);
        if (eff == 1) {
            if (battle_trace_) fprintf(stderr, "COMBAT: free left-click ally unit=%d id=%d -> 4D674\n", occ, u.id);
            battle_player_enter_ring(occ);
            return false;
        } else {
            if (battle_trace_) fprintf(stderr, "COMBAT: free left-click non-ally unit=%d id=%d -> 493A3\n", occ, u.id);
            battle_show_info_modal(occ);
            return false;
        }
    }
    return false;
}

// sub_4D3BA + sub_462A7: wait healing. With HP < 80% and TP > 10 it heals
// (TP-10)*30 raw (x100 units) capped at 80%. kind == 3 (YOU01 lava map) or weather == 3
// halves it.
bool CombatII::battle_wait_heal(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    auto& u = units_[(size_t)unit_index];
    const int cur = battle_hp_raw(u), maxv = battle_max_hp_raw(u);
    if (cur >= maxv * 80 / 100) return false;      // cmp cap,[48h]; jle skip
    const int tp = battle_tp();
    if (tp <= 10) return false;                    // sub ecx,0Ah; jle skip
    int heal = (tp - 10) * 30;
    const int cap = maxv * 80 / 100;
    if (cur + heal > cap) heal = cap - cur;        // clamp
    if (heal <= 0) return false;
    if (sce_map_kind_ == 3 || weather_ == 3) heal /= 2; // sub_462A7 halving
    const int new_raw = cur + heal;
    const int new_hp = (new_raw + 99) / 100;
    u.hp_deficit = new_hp * 100 - new_raw;
    u.hp = new_hp;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: wait-heal unit=%d +%d_raw hp=%d (sub_4D3BA)\n", u.id, heal, new_hp);
    return true;
}

// sub_493A3 (ring id 11 -> sub_4D442 loc_4D53B, dasm 77001..77035):
//   sub_103A8 (weather tick off) -> 44DEC(0) (banner off) -> 44D6E(1) (dim on)
//   -> for [0Eh]==1 (ally) sub_48E90 (equipment drag-and-drop screen,
//      battle_equipment_screen); otherwise 454B3 -> 48734 info window -> 5E9E8 ->
//      499EF(0x4B0)
//   -> 44D6E(0) -> 44DEC(1) -> sub_103A0 (weather tick on)
void CombatII::battle_show_info(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    // sub_493A3: `cmp byte ptr [ebx+0Eh],1` tests the original's effective army ([0Eh]).
    // Confusing it with the raw SCE army (0) sends 1-7 allies to the info panel and loses
    // the equipment/weapon slots.
    if (battle_effective_army(u) == 1) {
        weather_tick_enabled_ = false;                // sub_103A8
        battle_overlay_mode_ = 0;                     // sub_44DEC(0)
        battle_equipment_screen(unit_index);          // sub_48E90
        battle_overlay_mode_ = 1;                     // sub_44DEC(1)
        weather_tick_enabled_ = true;                 // sub_103A0
        return;
    }
    battle_show_info_modal(unit_index);
}

// sub_4D674 loc_4DAAF: turn end (4B842 36 -> 276 fly-out over 15 frames + 4AD69 +
// 197AD/restore + 1A0F0(1) + 19FA3/19792/19F9D).
// action_end (var_24 != 0) jumps straight to loc_4DB4E: the action routine already removed
// the unit and zeroed the gauge, so 197AD, [13h], the pose branch and the heal are skipped.
// word_FDAF2 != 0 (compact ring) takes the same short exit.
// Otherwise 1A0AD decides: unchanged TP restores [56h] and keeps the unit queued (loc_4DB44);
// changed TP heals (4D3BA when TP >= 40) and settles through 197AD, [13h] |= var_28 and the
// var_28 pose branch.
// When the queue empties afterwards time resumes (state 0, 19F9D); otherwise the port stays
// in free wait (state 1, 4DB71).
void CombatII::battle_player_end_turn(bool action_end) {
    const int ui = battle_player_unit_;
    if (ui >= 0 && (size_t)ui < units_.size() && units_[(size_t)ui].active) {
        auto& u = units_[(size_t)ui];
        if (action_end || battle_simple_ring_) {
            // loc_4DAAF: var_24 != 0 or word_FDAF2 != 0 -> loc_4DB4E without the settlement.
            if (battle_trace_)
                fprintf(stderr, "COMBAT: end-turn short exit unit=%d (var24=%d FDAF2=%d)\n",
                        u.id, action_end ? 1 : 0, battle_simple_ring_ ? 1 : 0);
        } else if (!battle_tp_changed()) {
            // loc_4DB44: unchanged TP restores [56h] and skips 197AD (queue kept).
            u.battle_gauge = battle_saved_gauge_;
            // sub_1A0F0(1) runs exactly once on every exit, because loc_4DB44 falls through to
            // loc_4DB4E; the common call after the ring close (4DB4E) handles it, so it is not
            // called here.
            if (battle_trace_)
                fprintf(stderr, "COMBAT: end-turn pass unit=%d (gauge restored %u, stays queued)\n",
                        u.id, (unsigned)u.battle_gauge);
        } else {
            // 4DAAF -> 1A0AD == 1: sub_4D3BA(unit, TP) when TP >= 40 (before 197AD).
            if (battle_tp() >= 0x28) battle_wait_heal(ui);
            // sub_197AD: remove from the queue and clear [56h].
            battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                                  battle_ready_queue_.end(), ui),
                                      battle_ready_queue_.end());
            u.battle_gauge = 0;
            // [13h] |= var_28, then the class 30h bit (4DB0A..4DB29).
            u.combat_flags_13 = (uint8_t)(u.combat_flags_13 | (battle_used_bits_ & 0xFF));
            if (u.combat_class == 0x30) u.combat_flags_13 = (uint8_t)(u.combat_flags_13 | 4);
            if (battle_used_bits_ != 0) {
                u.anim_frame = 0; // var_28 != 0 -> [64h] = 0
            } else {
                reset_unit_pose(u); // var_28 == 0 -> sub_12124
            }
            if (battle_trace_)
                fprintf(stderr, "COMBAT: end-turn unit=%d (queue removed, gauge 0, bits=%02X)\n",
                        u.id, (unsigned)battle_used_bits_);
        }
    } else {
        battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                              battle_ready_queue_.end(), ui),
                                  battle_ready_queue_.end());
    }
    battle_player_unit_ = -1;
    // loc_4DAAF: 4B842 (close 36 -> 276); turn end is a fly-out, not a hide.
    ring_close_anim_full(); // the sub_4B842 close (sfx + 36 -> 276 over 15 frames)
    ring_sub_close(); // close the sub-ring too (4B9D7)
    ring_sub_magic_g_ = -1;
    ring_sub_magic_d_ = -1;
    ring_sub_ai_first_ = -1;
    battle_attack_cancel();
    battle_move_cancel();
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_magic_pending_r_ = -1;
    battle_item_pending_dx_ = -1;
    battle_used_bits_ = 0;
    battle_var24_action_end_ = false;
    battle_menu_ids_.clear();
    battle_magic_options_.clear();
    battle_magic_groups_.clear();
    battle_magic_page_ = 0;
    battle_item_options_.clear();
    battle_item_full_options_.clear();
    battle_item_page_ = 0;
    battle_weapon_options_.clear();
    battle_weapon_select_ = false;
    battle_player_diffusion_.clear();
    battle_player_diffusion_valid_ = false;
    battle_range_e8_.clear();
    battle_simple_ring_ = false;
    battle_set_420f(1); // mov eax,1; call sub_1A0F0 (4DB4E)
    // loc_4DB71: 19FA3==1 and 19792()==0 (empty queue) -> 19F9D(0) resumes time; a
    // non-empty queue stays in free wait. 19792 returns 1 when dword_F041C != 0 (the ready
    // queue is non-empty).
    if (battle_state_ == 1 && battle_ready_queue_.empty()) {
        battle_state_ = 0; // call sub_19F9D(0)
        battle_overlay_mode_ = 0; // time resumes: the original only calls 19F9D; the next tick runs 44DEC(0)
        battle_free_wait_ = false;
        if (battle_trace_) fprintf(stderr, "COMBAT: end-turn queue empty -> resume (19F9D)\n");
    } else {
        battle_state_ = 1;
        battle_overlay_mode_ = 1;
        battle_free_wait_ = true; // wait for the next unit click
        battle_free_hover_ = -1;
        if (battle_trace_) fprintf(stderr, "COMBAT: end-turn queue=%zu -> free wait\n", battle_ready_queue_.size());
    }
}

// sub_1E2B1: for each F0648 target, clears [1Eh]&2 and sets [1Fh] = FFFFh (clears the low
// three status bits). sub_1EF9A (counterattack) is battle_counterattack_list in
// combatii_player.cpp.
void CombatII::battle_clear_ailment_flags(int a_index, int b_index) {
    const int idx[2] = {a_index, b_index};
    for (int k = 0; k < 2; ++k) {
        if (idx[k] < 0 || (size_t)idx[k] >= units_.size()) continue;
        auto& u = units_[(size_t)idx[k]];
        if (u.combat_flags_1e & 2) {
            u.ail_1f = 0xFFFF;
            u.combat_flags_1e = (uint8_t)(u.combat_flags_1e & 0xF8);
        }
    }
}

// sub_1EBFE: physical-hit ailment. The original walks the F0648 list (attacker + target);
// the port handles a single pair. Its only XREF is sub_1F738 (player attack), so it is not
// attached to the AI attack path.
//   hit_kind (dword_F0B8A+2) == 0 skips everything. cx = sub_18F52(attacker [+5Bh]); -1
//   falls back to sub_17531(attacker), and another -1 skips.
//   A dodge (result == 2) and targets with sprite_type >= 0x12C are skipped.
//   cx==0: 1762C(tgt,0) > 0 -> [1Eh] = ([1Eh]&F8)|1, [1Fh] = 100.
//   cx==1: 1762C(tgt,1) > 0 -> |2, [1Fh] = 100, 197AD(tgt: queue removed, [56h] = 0).
//   cx==2: 1762C(tgt,2) > 0 -> |4, [1Fh] = 100.
//   cx==3: [1Eh] |= 0x10, [23h] = 100; with [0Eh]==1 (raw) and [56h]==0xFF -> 19718 (queue
//   removal only).
//   cx==-2: [1Eh] |= 0x80, [27h] = 100 (no 1762C gate).
// Deviation: the original writes these fields even for targets whose death has not been
// processed yet (before 1ED45); the port resolves death first and skips inactive or HP 0
// targets, which would not be observable after the removal anyway.
void CombatII::battle_apply_1ebfe(int attacker_index, int target_index, int hit_kind, int result) {
    if (hit_kind == 0) {
        if (battle_trace_ && attacker_index >= 0 && (size_t)attacker_index < units_.size() &&
            target_index >= 0 && (size_t)target_index < units_.size())
            fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (hit_kind 0)\n",
                    units_[(size_t)attacker_index].id, units_[(size_t)target_index].id);
        return; // `cmp dword_F0B8A+2,0; jz` (skip everything)
    }
    if (attacker_index < 0 || target_index < 0 ||
        (size_t)attacker_index >= units_.size() || (size_t)target_index >= units_.size()) return;
    const auto& att = units_[(size_t)attacker_index];
    auto& tgt = units_[(size_t)target_index];
    if (!att.active || !tgt.active || tgt.hp <= 0) return;
    if (result == 2) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (dodge)\n", att.id, tgt.id);
        return; // word_F0430 == 2 (dodge: skip)
    }
    if (tgt.sprite_type >= 0x12C) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (type>=0x12C)\n", att.id, tgt.id);
        return;
    }
    int cx = battle_item_18f52((int)att.combat_type_5b); // sub_18F52
    if (cx == -1) cx = battle_damage_class(att);        // sub_17531 fallback
    if (cx == -1) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (class -1)\n", att.id, tgt.id);
        return;
    }
    switch (cx) {
        case 0: {
            if (battle_damage_adjustment(tgt, 0) <= 0) {
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (1762C class0<=0)\n", att.id, tgt.id);
                return; // sub_1762C(edx=0)
            }
            tgt.combat_flags_1e = (uint8_t)((tgt.combat_flags_1e & 0xF8) | 1);
            tgt.ail_1f = 100; // 0x64
            break;
        }
        case 1: {
            if (battle_damage_adjustment(tgt, 1) <= 0) {
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (1762C class1<=0)\n", att.id, tgt.id);
                return;
            }
            tgt.combat_flags_1e = (uint8_t)((tgt.combat_flags_1e & 0xF8) | 2);
            tgt.ail_1f = 100;
            // sub_197AD: remove from the queue and clear [56h].
            battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                                  battle_ready_queue_.end(), target_index),
                                      battle_ready_queue_.end());
            tgt.battle_gauge = 0;
            break;
        }
        case 2: {
            if (battle_damage_adjustment(tgt, 2) <= 0) {
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (1762C class2<=0)\n", att.id, tgt.id);
                return;
            }
            tgt.combat_flags_1e = (uint8_t)((tgt.combat_flags_1e & 0xF8) | 4);
            tgt.ail_1f = 100;
            break;
        }
        case 3: {
            tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e | 0x10);
            tgt.ail_23 = 100;
            // [0Eh]==1 (raw army) && [56h]==0xFF (ready) -> sub_19718 (dequeue, keep the gauge).
            if (tgt.army == 1 && tgt.battle_gauge == 0xFF) {
                battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                                      battle_ready_queue_.end(), target_index),
                                          battle_ready_queue_.end());
            }
            break;
        }
        default: {
            if (cx != -2) {
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: 1ebfe skip unit=%d target=%d (class %d unhandled)\n", att.id, tgt.id, cx);
                return;
            }
            tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e | 0x80);
            tgt.ail_27 = 100;
            break;
        }
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 1ebfe unit=%d target=%d class=%d flags1e=%02X ail1f=%u ail23=%u ail27=%u\n",
                att.id, tgt.id, cx, (unsigned)tgt.combat_flags_1e,
                (unsigned)tgt.ail_1f, (unsigned)tgt.ail_23, (unsigned)tgt.ail_27);
}

// ---- summon (sub_29F4D / sub_299DA / sub_29EE4) ----
// Used by dword_F13B0 rows 0..2 d4 and row 5. Arguments: eax = caster, edx = k (low level),
// ebx = template row (0..9), stack = monster sprite ([61h]). sub_29F4D picks an empty tile
// with 29C5A/29D88 (the presentation and facing table are skipped), removes summons holding
// the same [12h] = caster id+10 through sub_29EE4, then creates them with sub_299DA.
// sub_299DA copies the dword_28359 template (15 bytes x 10 rows) and fills [0Fh]=0xFF,
// [12h]=caster id+10, [0Eh]=caster army, [61h]=stack argument, [64h]=0, [63h]=caster facing,
// [48h]=[4Ch], [50h]=[52h]; the stats come from the row (the port reads [0]=str, [1]=mag,
// [2]=[42h] low, [3]=[43h], [4]=max HP/1000, [5]=max MP/10 - the byte offsets of 299DA).
// The summon count is low level + 1 (1..3).
namespace {
// Reads dword_28359 as 15-byte rows (sub_299DA: `mov eax,[esp+0C6h]; sar 16; imul 0Fh`).
inline int summon_tbyte(int row, int off) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(kSummonTemplate);
    const int i = row * 15 + off;
    return (i >= 0 && i < (int)sizeof(kSummonTemplate)) ? (int)b[i] : 0;
}
} // namespace

// sub_4EAF3 record write: 0x88 bytes at BLACKBOX.DAT 0x4E26 + (id-1)*0x88.
//   The port keeps no raw SCE record, so it reads the existing record and overwrites only
//   the fields it models; unmodelled fields such as name and equipment keep their file
//   values (the original writes the whole in-memory record).
namespace {
void battle_pack_unit_record(const CombatUnitPlacement& u, uint8_t rec[0x88]) {
    std::memset(rec, 0, 0x88);
    auto w16 = [&](int o, int v) { rec[o] = (uint8_t)(v & 0xFF); rec[o + 1] = (uint8_t)((v >> 8) & 0xFF); };
    auto w32 = [&](int o, int v) {
        const uint32_t x = (uint32_t)v;
        rec[o] = (uint8_t)(x & 0xFF); rec[o + 1] = (uint8_t)((x >> 8) & 0xFF);
        rec[o + 2] = (uint8_t)((x >> 16) & 0xFF); rec[o + 3] = (uint8_t)((x >> 24) & 0xFF);
    };
    const size_t n = std::min<size_t>(u.name.size(), 13u);
    if (n) memcpy(rec + 2, u.name.data(), n);
    rec[0] = (uint8_t)u.x;
    rec[1] = (uint8_t)u.y;
    rec[0x0D] = u.unit_kind_0d;
    rec[0x0E] = (uint8_t)battle_effective_army(u);
    rec[0x0F] = (uint8_t)u.id;
    rec[0x10] = (uint8_t)u.face;
    rec[0x11] = u.combat_class;
    rec[0x12] = u.item_use_12;
    rec[0x13] = u.combat_flags_13;
    rec[0x14] = u.combat_flags_14;
    rec[0x15] = u.combat_flags_15;
    w16(0x16, u.buff_16); w16(0x18, u.buff_18); w16(0x1A, u.buff_1a); w16(0x1C, u.buff_1c);
    rec[0x1E] = u.combat_flags_1e;
    w16(0x1F, u.ail_1f); w16(0x21, u.ail_21); w16(0x23, u.ail_23); w16(0x25, u.ail_25); w16(0x27, u.ail_27);
    rec[0x29] = u.ai_random_29; rec[0x2A] = u.ai_random_2a;
    rec[0x2B] = u.combat_level_2b;
    rec[0x2C] = u.hit_seq_2c; rec[0x2D] = u.hit_seq_2d;
    for (int i = 0; i < 7; ++i) rec[0x2E + i] = u.spell_charge_2e[i];
    rec[0x35] = u.weap_35; rec[0x36] = u.weap_36; rec[0x37] = u.weap_37;
    rec[0x38] = u.weap_cnt_38; rec[0x39] = u.weap_cnt_39; rec[0x3A] = u.weap_cnt_3a;
    rec[0x3B] = u.weap_cnt_3b; rec[0x3C] = u.weap_cnt_3c;
    rec[0x3D] = u.exp_3d; rec[0x3E] = u.combat_stat_3e; rec[0x3F] = u.growth_3f;
    rec[0x40] = u.combat_str_40; rec[0x41] = u.combat_mag41;
    w16(0x42, (int)(uint16_t)u.combat_value_42);
    rec[0x43] = u.combat_value_43;
    w16(0x44, (int)(uint16_t)u.combat_value_44);
    w16(0x46, (int)(uint16_t)u.combat_value_46);
    w32(0x48, battle_hp_raw(u));
    w32(0x4C, battle_max_hp_raw(u));
    w16(0x50, u.combat_mp_50); w16(0x52, u.combat_mp_max_52);
    w16(0x54, (int)(uint16_t)u.combat_value_54);
    rec[0x56] = u.battle_gauge;
    w16(0x57, u.combat_acc57);
    rec[0x59] = u.combat_def59;
    rec[0x5A] = u.combat_type_5a; rec[0x5B] = u.combat_type_5b; rec[0x5C] = u.code_b;
    rec[0x5D] = u.combat_type_5d; rec[0x5E] = u.inv_5e; rec[0x5F] = u.inv_5f;
    rec[0x60] = (uint8_t)u.slot_hint;
    rec[0x61] = (uint8_t)(u.sprite_type & 0xFF); rec[0x62] = (uint8_t)((u.sprite_type >> 8) & 0xFF);
    rec[0x63] = (uint8_t)(u.facing & 3);
    rec[0x64] = u.anim_frame;
    rec[0x6C] = u.ai_decision_state;
    rec[0x6D] = u.ai_field_6d; rec[0x6E] = u.ai_field_6e;
    rec[0x6F] = u.ai_home_x; rec[0x70] = u.ai_home_y;
    rec[0x71] = u.ai_watch_sel; rec[0x72] = u.ai_watch_id;
    w32(0x73, 0);
    w32(0x77, 0);
    w32(0x7B, 0);
    rec[0x7F] = u.ai_field_7f; rec[0x80] = u.ai_field_80; rec[0x81] = u.ai_field_81;
    rec[0x82] = (uint8_t)u.ai_mode; rec[0x83] = u.auto_battle; rec[0x84] = u.flags_84;
    w16(0x85, u.reward_money_85);
    rec[0x87] = 0; // owner keeps its existing value on the BLACKBOX write
}

CombatUnitPlacement battle_unpack_unit_record(const uint8_t* rec) {
    CombatUnitPlacement p;
    p.x = (int8_t)rec[0];
    p.y = (int8_t)rec[1];
    for (int i = 2; i < 0x0F && rec[i]; ++i) p.name.push_back((char)rec[i]);
    p.unit_kind_0d = rec[0x0D];
    p.army = rec[0x0E];
    p.memory_army = rec[0x0E];
    p.group = 2;
    p.id = rec[0x0F];
    p.owner_87 = rec[0x87];
    p.face = rec[0x10];
    p.combat_class = rec[0x11];
    p.item_use_12 = rec[0x12];
    p.combat_flags_13 = rec[0x13];
    p.combat_flags_14 = rec[0x14];
    p.combat_flags_15 = rec[0x15];
    p.combat_flags_1e = rec[0x1E];
    auto r16 = [&](int o) { return (uint16_t)(rec[o] | (rec[o + 1] << 8)); };
    p.buff_16 = r16(0x16); p.buff_18 = r16(0x18); p.buff_1a = r16(0x1A); p.buff_1c = r16(0x1C);
    p.ail_1f = r16(0x1F); p.ail_21 = r16(0x21); p.ail_23 = r16(0x23);
    p.ail_25 = r16(0x25); p.ail_27 = r16(0x27);
    p.ai_random_29 = rec[0x29]; p.ai_random_2a = rec[0x2A];
    p.combat_level_2b = rec[0x2B];
    p.hit_seq_2c = rec[0x2C]; p.hit_seq_2d = rec[0x2D];
    for (int i = 0; i < 7; ++i) p.spell_charge_2e[i] = rec[0x2E + i];
    p.weap_35 = rec[0x35]; p.weap_36 = rec[0x36]; p.weap_37 = rec[0x37];
    p.weap_cnt_38 = rec[0x38]; p.weap_cnt_39 = rec[0x39]; p.weap_cnt_3a = rec[0x3A];
    p.weap_cnt_3b = rec[0x3B]; p.weap_cnt_3c = rec[0x3C];
    p.exp_3d = rec[0x3D]; p.combat_stat_3e = rec[0x3E]; p.growth_3f = rec[0x3F];
    p.combat_str_40 = rec[0x40]; p.combat_mag41 = rec[0x41];
    p.combat_value_42 = (int16_t)r16(0x42); p.combat_value_43 = rec[0x43];
    p.combat_value_44 = (int16_t)r16(0x44); p.combat_value_46 = (int16_t)r16(0x46);
    p.combat_mp_50 = r16(0x50); p.combat_mp_max_52 = r16(0x52);
    p.combat_value_54 = (int16_t)r16(0x54);
    p.battle_gauge = rec[0x56];
    p.combat_acc57 = r16(0x57);
    p.combat_def59 = rec[0x59];
    p.combat_type_5a = rec[0x5A]; p.combat_type_5b = rec[0x5B]; p.code_b = rec[0x5C];
    p.combat_type_5d = rec[0x5D]; p.inv_5e = rec[0x5E]; p.inv_5f = rec[0x5F];
    p.slot_hint = rec[0x60];
    p.sprite_type = (int)(uint16_t)(rec[0x61] | (rec[0x62] << 8));
    p.facing = (int8_t)(rec[0x63] & 3);
    p.anim_frame = rec[0x64];
    p.ai_decision_state = rec[0x6C];
    p.ai_field_6d = rec[0x6D]; p.ai_field_6e = rec[0x6E];
    p.ai_home_x = rec[0x6F]; p.ai_home_y = rec[0x70];
    p.ai_watch_sel = rec[0x71]; p.ai_watch_id = rec[0x72];
    p.ai_field_7f = rec[0x7F]; p.ai_field_80 = rec[0x80]; p.ai_field_81 = rec[0x81];
    p.ai_mode = (int8_t)rec[0x82]; p.auto_battle = rec[0x83]; p.flags_84 = rec[0x84];
    p.reward_money_85 = r16(0x85);
    p.ai_target_unit = -1;
    apply_record_hp(p, rec);
    p.active = true;
    p.occupancy_hp_known = battle_hp_raw(p) > 0;
    return p;
}
} // namespace

// sub_4E81F: re-reads the 0x88 record at BLACKBOX.DAT 0x4E26+(ID-1)*0x88 and overwrites
// the unit outright, then restores only the coordinates [0]/[1], facing [63h] and the AI
// runtime fields ([6Ch..70h], [73h], [71h], [72h], [80h], [82h]). The [+87h] owner is
// remapped through word_FDAF6[owner] to an army (ASM 84484..84672).
//   - An owner of 0/0x63 is replaced by the low word_FDF85 value (loc_4E8FF).
//   - When army = FDAF6[owner]-1 is 0, CC990 is searched for a record with [0Eh]==1 and
//     [0Fh]==ID and its [56h] is cleared; if none exists, a copy of the record
//     ([0Eh]=1, [56h]=0) is inserted at the front of the list.
//   - When the saved [5Ch] (code_b at call time) is non-zero, the current unit is copied
//     into the [77h] stash and transformed into that form with sub_17E6D, then [48h]=[4Ch]
//     (full HP).
//   The caller (sub_50D9F loc_50EDD) increments [0Eh] and [[77h]+0Eh] right after this
//   call, so that is applied here as well.
void CombatII::battle_reload_record_4e81f(CombatUnitPlacement& u) {
    if (u.id <= 0) return;
    std::string path = game_path("BLACKBOX.DAT");
    if (!file_exists(path.c_str())) path = game_path("blackbox.dat");
    if (!file_exists(path.c_str())) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 4E81F id=%d: BLACKBOX missing - keeping the inline record\n", u.id);
        return;
    }
    uint8_t rec[0x88] = {};
    bool ok = false;
    if (FILE* fp = fopen(path.c_str(), "rb")) {
        const long base = 0x4E26L + (long)(u.id - 1) * 0x88L;
        ok = fseek(fp, base, SEEK_SET) == 0 && fread(rec, 1, sizeof(rec), fp) == sizeof(rec);
        fclose(fp);
    }
    if (!ok) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 4E81F id=%d: record read failed - keeping the inline one\n", u.id);
        return;
    }

    // ---- saved temporaries (var_28/var_4/var_2C/var_14/var_3C/var_8..var_18) ----
    const int saved_x = u.x, saved_y = u.y;                  // [0]/[1]
    const int8_t saved_facing = u.facing;                    // [63h]
    const int saved_form = u.code_b;                         // [5Ch]
    auto saved_77 = u.stored_77;                             // [77h]
    const uint8_t s6c = u.ai_decision_state, s6d = u.ai_field_6d, s6e = u.ai_field_6e;
    const uint8_t s6f = u.ai_home_x, s70 = u.ai_home_y;
    const int s73 = u.ai_target_unit;
    const uint8_t s71 = u.ai_watch_sel, s72 = u.ai_watch_id, s80 = u.ai_field_80;
    const int8_t s82 = u.ai_mode;
    // Port-only runtime state (the original keeps these outside the 0x88 record)
    const CombatUnitPlacement keep = u;

    // ---- read_ 0x88: overwrite the whole record ----
    u = battle_unpack_unit_record(rec);
    u.x = saved_x; u.y = saved_y; u.facing = saved_facing;
    u.stored_77 = saved_77;
    u.ai_decision_state = s6c; u.ai_field_6d = s6d; u.ai_field_6e = s6e;
    u.ai_home_x = s6f; u.ai_home_y = s70; u.ai_target_unit = s73;
    u.ai_watch_sel = s71; u.ai_watch_id = s72; u.ai_field_80 = s80; u.ai_mode = s82;
    u.linked_class = keep.linked_class;
    u.ai_target_marker = keep.ai_target_marker;
    u.active = keep.active;
    u.group = keep.group;
    u.list_seq = keep.list_seq;
    u.battle_list_membership_modeled = keep.battle_list_membership_modeled;
    u.anim_counter = keep.anim_counter; u.anim_phase = keep.anim_phase;
    u.walk_arrived = keep.walk_arrived;
    u.walk_off_x = keep.walk_off_x; u.walk_off_y = keep.walk_off_y;
    u.drop_step = keep.drop_step; u.drop_wait = keep.drop_wait; u.drop_sil = keep.drop_sil;
    u.knock_dx = keep.knock_dx; u.knock_dy = keep.knock_dy; u.attacking = keep.attacking;
    u.frame_nt = keep.frame_nt; u.frame_mt = keep.frame_mt;
    u.sprite_id = (uint8_t)(u.sprite_type % 222);
    u.team = (u.army == 0) ? 0 : 1;
    // apply_record_hp follows the SCE record convention and zeroes the accumulated TP in
    // [57h], but 4E81F reads the persistent BLACKBOX sheet, so [57h] must come from the file.
    u.combat_acc57 = (uint16_t)(rec[0x57] | (rec[0x58] << 8));

    // ---- [87h] owner -> word_FDAF6 remap (loc_4E8FF..loc_4E982) ----
    u.owner_87 = rec[0x87];
    int owner = rec[0x87];
    if (owner == 0 || owner == 0x63) owner = (int)fdf85_lo_;
    int fdaf6 = sce_fdaf6_lookup(owner);                     // word_FDAF6[owner] low
    if (fdaf6 < 0) fdaf6 = 0;
    int army_byte = (int)(uint8_t)(fdaf6 - 1);               // dec al
    u.army = army_byte;
    u.memory_army = army_byte;
    u.team = (army_byte == 0) ? 0 : 1;
    if (army_byte == 0) {
        bool found = false;
        for (auto& r : battle_roster_cc990_) {
            if (r.id != u.id) continue;
            if ((uint8_t)battle_effective_army(r) != 1) continue;
            r.battle_gauge = 0;                              // mov byte [eax+56h],0
            found = true;
            break;
        }
        if (!found) {                                        // loc_4E942: sub_112FD + copy 0x88
            CombatUnitPlacement copy = u;                    // (army 0 -> inc -> 1)
            copy.army = 1;
            copy.memory_army = 1;
            copy.battle_gauge = 0;
            copy.stored_77.reset();
            battle_roster_cc990_.push_back(copy);                // sub_1134F: link at the end of the list
            if (battle_trace_)
                fprintf(stderr, "COMBAT: 4E81F id=%d army0 -> CC990 copy (army 1)\n", u.id);
        }
    }

    // ---- stored [5Ch] != 0: [77h] copy + sub_17E6D + [48h] = [4Ch] ----
    if (saved_form != 0 && saved_77) {
        *saved_77 = u;                                       // repne movsd: ebp -> [77h]
        saved_77->stored_77.reset();                         // the original leaves a pointer to itself behind
        u.stored_77 = saved_77;
        battle_form_17e6d(u, saved_form);                    // sub_17E6D(unit, stored [5Ch])
        u.hp = u.max_hp;                                     // mov eax,[4Ch]; mov [48h],eax
        u.hp_deficit = u.max_hp_deficit;
    }

    // sub_50D9F loc_50EDD: `inc byte ptr [unit+0Eh]` / `inc byte ptr [[77h]+0Eh]`. The caller
    // adds 1 to the army 4E81F stored ([87h] remap minus 1), so the final effective army is
    // FDAF6[owner].
    const int eff = (int8_t)(uint8_t)(army_byte + 1);
    u.army = eff;
    u.memory_army = eff;
    u.team = (eff == 0) ? 0 : 1;
    if (u.stored_77) {
        const int eff77 = (int8_t)(uint8_t)(u.stored_77->army + 1);
        u.stored_77->army = eff77;
        u.stored_77->memory_army = eff77;
    }

    if (battle_trace_)
        fprintf(stderr, "COMBAT: 4E81F id=%d owner=%d fdaf6=%d army=%d class=%u lv=%u hp=%d/%d (BLACKBOX reload)\n",
                u.id, owner, fdaf6, eff, (unsigned)u.combat_class, (unsigned)u.combat_level_2b,
                u.hp, u.max_hp);
}

void CombatII::battle_write_blackbox_record(const CombatUnitPlacement& u) {
    if (u.id <= 0) return;
    std::string path = game_path("BLACKBOX.DAT");
    if (!file_exists(path.c_str())) path = game_path("blackbox.dat");
    if (!file_exists(path.c_str())) return;
    FILE* fp = fopen(path.c_str(), "r+b");
    if (!fp) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: cleanup record unit=%d: BLACKBOX open failed\n", u.id);
        return;
    }
    const long base = 0x4E26L + (long)(u.id - 1) * 0x88L;
    uint8_t rec[0x88] = {};
    if (fseek(fp, base, SEEK_SET) == 0) fread(rec, 1, sizeof(rec), fp);
    const uint8_t old_owner = rec[0x87];
    battle_pack_unit_record(u, rec);
    rec[0x87] = old_owner; // keep the [+87h] owner
    if (fseek(fp, base, SEEK_SET) == 0 && fwrite(rec, 1, sizeof(rec), fp) == sizeof(rec)) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: BLACKBOX record unit=%d id=%d hp_raw=%d (sub_4EAF3)\n",
                    u.id, u.id, battle_hp_raw(u));
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: warning: BLACKBOX record write failed id=%d\n", u.id);
    }
    fclose(fp);
}

// sub_4EC51 + sub_4EAF3: after battle, remove the battle units from the army list and
// restore the persistent fields.
//   - HP: when current != max, heal by max/10. For owner == low word_FDF85, raise anything
//     below max/3 to max/3 and clamp above max; other owners are restored to full HP.
//   - MP current = max, [+13h] = 0; [+14h]&1 is cleared with [+61h]--, &2 clears [+83h].
//   - [+14h]/[+1Eh]/[+15h] = 0, buff/ailment words = FFFF, gauge/TP/AI state reset.
//   - Regular units with [+12h]==0 write their 0x88 record back to BLACKBOX.DAT.
//   The original removes them from the army list (sub_113E4) and walks all of CC990 with
//   sub_4EAF3.
void CombatII::battle_battle_cleanup_4ec51() {
    const int party_owner = (int)(fdf85_word_ & 0xFF);
    auto cleanup_one = [&](CombatUnitPlacement& u) {
        int hp_raw = battle_hp_raw(u);
        const int max_raw = battle_max_hp_raw(u);
        int owner = -1;
        if (u.item_use_12 == 0) {
            owner = blackbox_owner_for_id(u.id);              // [+87h] of the record
            if (owner < 0) owner = party_owner;
        }
        if (hp_raw != max_raw) {
            hp_raw += max_raw / 10;                           // add eax(max/10)
            if (owner == party_owner && party_owner >= 0) {
                if (max_raw / 3 > hp_raw) hp_raw = max_raw / 3; // below a third: raise to max/3
                if (hp_raw > max_raw) hp_raw = max_raw;
            } else {
                hp_raw = max_raw;                             // loc_4EB93: other owners are restored to full
            }
            u.hp = hp_raw <= 0 ? 0 : (hp_raw + 99) / 100;
            u.hp_deficit = u.hp > 0 ? u.hp * 100 - hp_raw : 0;
        }
        u.combat_mp_50 = u.combat_mp_max_52;                  // mov ax,[52h]; mov [50h],ax
        u.combat_flags_13 = 0;                                // mov byte [13h],0
        if (u.combat_flags_14 & 1) {                          // mount/transform bit
            u.combat_flags_14 = (uint8_t)(u.combat_flags_14 & 0xFE);
            if (u.sprite_type > 0) --u.sprite_type;           // dec word [61h]
        }
        if (u.combat_flags_14 & 2) u.auto_battle = 0;         // mov byte [83h],0
        u.combat_flags_14 = 0;
        u.combat_flags_1e = 0;
        u.combat_flags_15 = 0;
        u.buff_16 = u.buff_18 = u.buff_1a = u.buff_1c = 0xFFFF;
        u.ail_1f = u.ail_21 = u.ail_23 = u.ail_25 = u.ail_27 = 0xFFFF;
        u.battle_gauge = 0;                                   // mov byte [56h],0
        u.combat_acc57 = 0;                                   // mov word [57h],0
        u.auto_battle = 0;                                    // mov byte [83h],0
        u.ai_field_80 = 0xFF;                                 // mov byte [80h],0FFh
        u.ai_mode = -1;                                       // mov byte [82h],0FFh
        u.flags_84 = 0;                                       // mov byte [84h],0
        u.ai_target_unit = -1;
        reset_unit_pose(u);                                   // sub_12124
        if (u.item_use_12 == 0) {
            battle_write_blackbox_record(u);                  // sub_4EAF3 write_
        } else if (battle_trace_) {
            fprintf(stderr, "COMBAT: cleanup summon unit=%d tag=%u (record not written)\n",
                    u.id, (unsigned)u.item_use_12);
        }
        u.active = false;                                     // sub_113E4 (remove from the army list)
    };
    for (auto& u : units_) {
        if (!u.active) continue;                              // already gone from the CC990 list
        cleanup_one(u);
    }
    // 4EC51 runs 4EAF3 over all of CC990, including party records that never made it onto
    // the map ([56h] != 0x80). Records placed on the map are handled by the units_ loop
    // above, so roster entries with [56h]==0x80 are skipped here.
    for (auto& r : battle_roster_cc990_) {
        if (!r.deploy_roster || r.active || r.battle_gauge == 0x80) continue;
        cleanup_one(r);
    }
    battle_ready_queue_.clear();
    battle_target_list_.clear();
}

// sub_29EE4: drops existing summons with [12h] == id+10 (army list, occupancy, queue).
void CombatII::battle_remove_old_summons(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const uint8_t tag = (uint8_t)((unsigned)units_[(size_t)unit_index].id + 10u);
    for (auto& o : units_) {
        if (!o.active || o.item_use_12 != tag) continue;
        o.hp = 0;
        o.active = false;
    }
    battle_wc1_occupancy_preserve_or_close("summon replaced");
}

int CombatII::battle_spell_summon(int unit_index, int k, int row, int sprite, int combat_class) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    if (!units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0) return 4;
    // sub_29F4D entry: count = k+1; rows 4..6 (large) take the 2x2 footprint path (sub_29D88).
    const int count = k + 1;                                          // var_14 = dx+1
    const bool large = (row >= 4 && row <= 6);                        // var_10
    // Facing table filled by sub_29C5A/29D88: dirs[0] = facing, dirs[1] = (facing+1)%4,
    // dirs[2] = (facing+3)%4. The original only marks the E8F46 grid with 3E8/1/-1 to drive
    // the 4A483 cursor display; these values have no gameplay effect.
    const int caster_facing = (int)units_[(size_t)unit_index].facing & 3;
    int dirs[3] = { caster_facing, (caster_facing + 1) & 3, (caster_facing + 3) & 3 };
    // 29F4D: remove old summons -> 28889 announce -> 299DA per tile.
    battle_remove_old_summons(unit_index);
    battle_spell_announce(unit_index, (battle_spell_name_ && *battle_spell_name_) ? battle_spell_name_ : "Summon");
    if (should_exit_) return 0;
    int spawned = 0;
    for (int n = 0; n < count && !should_exit_; ++n) {
        if ((size_t)unit_index >= units_.size()) break;
        const CombatUnitPlacement& caster = units_[(size_t)unit_index];
        const int cx0 = (int)(int8_t)caster.x, cy0 = (int)(int8_t)caster.y;
        // 29F4D loc_2A02A: one step along the facing table (sub_49ABC). For large sprites
        // even indices use table[n/2] and odd indices use table[n].
        const int dir_index = large ? ((n & 1) ? n : (n >> 1)) : n;
        const int dir = dirs[dir_index < 3 ? dir_index : 2];
        int px = cx0, py = cy0;
        switch (dir & 3) {                                            // sub_49ABC
            case 0: py += 1; break;
            case 1: px -= 1; break;
            case 2: py -= 1; break;
            default: px += 1; break;
        }
        // 11DB2 occupancy: skip this tile entirely (no relocation); the 13CBD guard is the same.
        if (px < 2 || py < 4 || px >= (int)map_w_ - 2 || py >= (int)map_h_ - 4) continue;
        if (battle_unit_index_at(px, py) >= 0) continue;
        // ---- sub_299DA: copy the caster's 0x88 record and overwrite only the fields below ----
        CombatUnitPlacement nu = caster;
        nu.stored_77.reset();                                         // [77h]=0
        nu.linked_class = -1;
        nu.x = px;                                                    // [0],[1]
        nu.y = py;
        nu.sprite_type = sprite;                                      // [61h]
        nu.combat_class = (uint8_t)combat_class;                      // [11h]
        nu.combat_flags_13 = 0;                                       // [13h]=0
        nu.combat_flags_14 = (uint8_t)(row == 6 ? 1 : 0);             // [14h] = [13h]; row 6 -> |=1
        nu.combat_flags_15 = 0;                                       // [15h] = [1Eh]
        nu.combat_flags_1e = 0;
        nu.unit_kind_0d = (row == 0x0A) ? 2 : 1;                      // [0Dh]=1, row 0x0A -> 2
        nu.flying = (row == 6);
        nu.anim_frame = 0;                                            // [64h]=0
        nu.facing = 0;                                                // [63h] = [64h]
        nu.hit_seq_2c = 0;                                            // [67h] = [77h]
        nu.hit_seq_2d = 0;
        nu.ai_field_81 = 0;                                           // [81h]=0
        nu.ai_field_7f = 0;                                           // [7Fh] = [81h]
        nu.ai_mode = (int8_t)0xFF;                                    // [82h]=0FFh
        nu.ai_field_80 = 0xFF;                                        // [80h] = [82h]
        nu.ai_field_6d = 0;                                           // [6Dh] = [6Eh]
        nu.ai_field_6e = 0;                                           // [6Eh]=0
        nu.ai_decision_state = 0;                                     // [6Ch]
        nu.ai_watch_sel = (uint8_t)0xFF;                              // [71h]=0FFh
        nu.ai_home_x = 0xFF;                                          // [6Fh] = [71h]
        nu.ai_home_y = 0xFF;
        nu.ai_target_unit = -1;                                       // [73h]=0
        nu.battle_gauge = 0;                                          // [56h]=0
        nu.combat_acc57 = 0;                                          // [57h]=0
        // 15-byte template row: [0]=[40h] [1]=[41h] [2]=[42h] [3]=[43h] [4]=HP/100
        //   [5]=MP/10 [6..11] -> [2Eh..33h] (spell charges)
        nu.combat_str_40 = (uint8_t)summon_tbyte(row, 0);
        nu.combat_mag41 = (uint8_t)summon_tbyte(row, 1);
        nu.combat_value_42 = (int16_t)summon_tbyte(row, 2);
        nu.combat_value_43 = (uint8_t)summon_tbyte(row, 3);
        nu.combat_value_44 = (int16_t)(2 * summon_tbyte(row, 0));     // [44h] = 2*[40h]
        nu.max_hp = summon_tbyte(row, 4) * 10;                        // [4Ch] = tmpl[4]*1000 (raw)
        nu.max_hp_deficit = 0;
        nu.hp = nu.max_hp; nu.hp_deficit = 0;                         // [48h]=[4Ch]
        nu.combat_mp_max_52 = (uint16_t)(summon_tbyte(row, 5) * 10);  // [52h] = tmpl[5]*10
        nu.combat_mp_50 = nu.combat_mp_max_52;                        // [50h]=[52h]
        for (int i = 0; i < 6; ++i)
            nu.spell_charge_2e[i] = (uint8_t)summon_tbyte(row, 6 + i); // [2Eh..33h]
        {
            const int cls = (combat_class >= 0 && combat_class < 256) ? combat_class : 0;
            nu.combat_value_46 = (int16_t)(kClass71B68[cls][0] | (kClass71B68[cls][1] << 8));
            const int w71b6c = (int)(kClass71B68[cls][4] | (kClass71B68[cls][5] << 8));
            nu.combat_value_54 = (int16_t)((int)nu.combat_value_42 + w71b6c); // no lower clamp
        }
        nu.item_use_12 = (uint8_t)((unsigned)caster.id + 10u);         // [12h]=[0Fh]+0Ah
        nu.id = caster.id;                                            // [0Fh] keeps the copied value
        if ((int)battle_effective_army(caster) == 1) nu.auto_battle = 2; // [0Eh]==1 -> [83h]=2
        nu.slot_hint = sprite & 0xFF;                                 // [60h] = sub_1198D([61h])
        nu.active = true;
        nu.occupancy_hp_known = true;
        reset_unit_pose(nu);
        nu.list_seq = battle_next_list_seq();                          // [7Bh]
        units_.push_back(nu);
        battle_wc1_occupancy_preserve_or_close("summon");
        // sub_299DA: 1124A -> 10971/10988(+0Ch) -> sub_21944(recall.nsp), same table as
        // 3C548/4426D.
        if (battle_nsp_load("recall.nsp")) {                           // sub_1FEA4
            battle_ssp_play("regen.ssp");                              // 4F460(32h)
            battle_ssp_play("summon.ssp");                             // 4F460(33h)
            const int spawn_index = (int)units_.size() - 1;
            battle_fx_21944(spawn_index, (int)(int8_t)units_.back().x,
                            (int)(int8_t)units_.back().y, 0);
            battle_nsp_release();                                      // sub_1FFCD
            if (should_exit_) return 0;
        }
        ++spawned;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: summon unit=%d id=%d class=%d row=%d sprite=%d at=(%d,%d) hp=%d (sub_29F4D/299DA)\n",
                    units_[(size_t)unit_index].id, units_.back().id, (int)units_.back().combat_class, row,
                    sprite, px, py, units_.back().hp);
    }
    if (!should_exit_ && unit_index < (int)units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: summon done unit=%d row=%d spawned=%d/%d\n",
                units_[(size_t)unit_index].id, row, spawned, count);
    return 0;
}

// sub_137E7(al = army): picks one record that moved to dword_D6620 (the corpse list) on
// death. al != 0 takes the first corpse not hostile to that army; al == 0 picks any corpse
// at random. The port keeps the unit records, so it looks for
// [active == false && hp <= 0 && [0Dh] != 2 && sprite < 0x12C].
int CombatII::battle_corpse_take(int unit_index, bool random_pick) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int army = battle_effective_army(units_[(size_t)unit_index]);
    std::vector<int> cand;
    for (size_t i = 0; i < units_.size(); ++i) {
        const auto& o = units_[i];
        if (o.active || o.hp > 0) continue;
        if (o.sprite_type < 0 || o.sprite_type >= 0x12C) continue;   // 1DB1D: large sprites leave no corpse
        if (o.unit_kind_0d == 2) continue;                          // undead never enter the corpse list
        if (!random_pick &&
            battle_armies_hostile(battle_effective_army(o), army, battle_hostility_inverted_))
            continue;
        cand.push_back((int)i);
    }
    if (cand.empty()) return -1;
    if (random_pick) return cand[(size_t)combat_rand((int)cand.size())];
    return cand.front();
}
// ---- magic (sub_4C72A/4C605 + F13B0) ----
// sub_285E4: spell damage formula (x100 raw).
//   A = clamp([41h]*100/p1, 100, 200), B = A*base/100,
//   def = [59h] + ([15h]&0x20 ? 20 : 0), C = (p2 == 3 ? 100 : 100-def), B = C*B/100,
//   D = signed-byte sum of 184A4(5B/5A/5D, 0x37) plus the weather affinity (+50),
//   B += D*B/100; when p2 != -1 (not terrain), B = B*adj/10 with sub_1762C(target, p2).
int CombatII::battle_spell_damage(const CombatUnitPlacement& caster, const CombatUnitPlacement& target,
                                  int base, int p1, int p2) {
    int t = (int)caster.combat_mag41 * 100;
    int f = (p1 == 0) ? 100 : t / p1;
    if (f < 100) f = 100;
    if (f > 200) f = 200;
    int dmg = f * base / 100;
    const int def = (int)target.combat_def59 + ((target.combat_flags_15 & 0x20) ? 20 : 0);
    const int m = (p2 == 3) ? 100 : (100 - def);
    dmg = dmg * m / 100;
    // ASM 33613..33637: movsx cx,al / cbw / cbw - all three sign-extend and the sum truncates to a
    // word.
    int bonus = (int)(int8_t)battle_lookup_184a4(caster.combat_type_5b, 0x37) +
                (int)(int8_t)battle_lookup_184a4(caster.combat_type_5a, 0x37) +
                (int)(int8_t)battle_lookup_184a4(caster.combat_type_5d, 0x37);
    if ((weather_ == 1 && p2 == 2) || (weather_ == 2 && p2 == 1) || (weather_ == 3 && p2 == 0)) bonus += 50;
    bonus = (int16_t)bonus;                                             // movsx edx,cx
    dmg = dmg + dmg * bonus / 100;
    // ASM 33649..33665: when `mov edx,[esp+0Eh]>>16` (= p2) is not -1, scale by the
    // sub_1762C(target, p2) resistance.
    if (p2 != -1) dmg = dmg * battle_damage_adjustment(target, p2) / 10;
    return dmg;
}

// sub_29437 equivalent: attach a status to the F0648 list. dx = status, bx = chance base,
// ecx = edi (duration index).
//   Status 2 lowers the chance base by 20 (`sub ebx,14h`). The 28947 gate (clamp 10..70,
//   rand(100) < c) is unchanged.
//   Duration: status 1/4 sets `[1Fh]=word_7448A[edi]` (= 74488[edi+1]); status 2 sets
//   `[1Fh]=word_74488[edi]` plus 197AD (queue removed, gauge 0); other statuses only set the
//   `[1Eh]` bit.
//   edi turned out to be the dword-overlap slot ([var_8+2]>>16 = kind), so field/wave pass
//   kind; Ice passes ecx=2 explicitly and stays at 2 as the ASM confirms.
//   RNG order: ASM 0x2945E..0x2947A rolls 28947 for every list member first and only then
//   applies the `[61h]>=12Ch` sprite gate, so a large sprite still consumes a roll. (The
//   F0648 list is not cleared after 28728 damage, so dead or inactive targets are rolled and
//   written too.)
void CombatII::battle_spell_status_29437(int caster_index, const std::vector<int>& targets, int status, int chance,
                                         int edi) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    const auto& caster = units_[(size_t)caster_index];
    if (status == 2) chance -= 20; // `cmp dx,2; jnz; sub ebx,14h`
    for (int ti : targets) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& tgt = units_[(size_t)ti];
        const int roll_base = chance + ((int)caster.combat_level_2b - (int)tgt.combat_level_2b);
        int c = roll_base < 10 ? 10 : (roll_base > 70 ? 70 : roll_base); // sub_28947 clamp
        if (combat_rand(100) >= c) continue;
        if (tgt.sprite_type >= 0x12C) continue;                     // 0x29479 cmp [61h],12Ch
        tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e & 0xF8);
        tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e | (status & 0xFF));
        if (status == 1 || status == 4) {
            const int di = edi + 1 < 0 ? 0 : (edi + 1 > 4 ? 4 : edi + 1); // word_7448A[edi]
            tgt.ail_1f = kAilDuration[di];
        } else if (status == 2) {
            const int di = edi < 0 ? 0 : (edi > 4 ? 4 : edi); // word_74488[edi]
            tgt.ail_1f = kAilDuration[di];
            // sub_197AD: remove from the queue and clear [56h].
            battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                                  battle_ready_queue_.end(), ti),
                                      battle_ready_queue_.end());
            tgt.battle_gauge = 0;
        }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: status unit=%d st=%d dur=%u (sub_29437)\n", tgt.id, status, (unsigned)tgt.ail_1f);
    }
}

// sub_2A5BE: Thunder spread status sweep - applies the same status to hostile units outside
// the F0648 list. The original walks dword_CC960[pass*8] for passes 1..4, in list order.
// Frame aliasing confirms: [esp-3]>>24 = var_C (pass number), used for the 1201D gate and
// the CC960 selector, and [esp+6]>>16 = var_4 (chance). The pass gate reads
// `[0Bh]>>24`, which overlaps the effective army byte at [+0Eh], so it is effective-based.
// Duration: status 1/4 -> word_7448C[edi] (= kAilDuration[edi+2]); status 2 ->
// word_7448A[edi] (= kAilDuration[edi+1]) plus 197AD.
void CombatII::battle_thunder_sweep_2a5be(int caster_index, const std::vector<int>& victims,
                                          int status, int chance, int edi) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    if (!units_[(size_t)caster_index].active || units_[(size_t)caster_index].hp <= 0) return;
    const int caster_eff = battle_effective_army(units_[(size_t)caster_index]);
    // The four lists are CC960[1..4]. list_seq is the modeled [+7Bh] tail-insert order; the
    // initial load shares seq 0, so units_ index breaks that tie just like the original list.
    for (int pass = 1; pass <= 4; ++pass) {
        // sub_1201D(effective army, pass), including byte_E518D's xor.
        if (!battle_armies_hostile(caster_eff, pass, battle_hostility_inverted_)) continue;
        std::vector<int> list;
        list.reserve(units_.size());
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& u = units_[i];
            if (!u.active || u.hp <= 0) continue;
            if (battle_effective_army(u) != pass) continue;
            list.push_back((int)i);
        }
        std::sort(list.begin(), list.end(), [&](int a, int b) {
            const uint32_t sa = units_[(size_t)a].list_seq, sb = units_[(size_t)b].list_seq;
            return sa != sb ? sa < sb : a < b;
        });
        for (int i : list) {
            auto& tgt = units_[(size_t)i];
            if (!tgt.active || tgt.hp <= 0) continue;
            bool listed = false;
            for (int v : victims) if (v == i) { listed = true; break; }
            if (listed) continue;                 // F0648 members are handled by 29437
            if (tgt.combat_flags_15 & 0x80) continue;
            if (tgt.sprite_type >= 0x12C) continue;
            const int roll_base = chance + ((int)units_[(size_t)caster_index].combat_level_2b -
                                            (int)tgt.combat_level_2b);
            int c = roll_base < 10 ? 10 : (roll_base > 70 ? 70 : roll_base); // sub_28947 clamp
            if (combat_rand(100) >= c) continue;
            tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e & 0xF8);
            tgt.combat_flags_1e = (uint8_t)(tgt.combat_flags_1e | (status & 0xFF));
            if (status == 1 || status == 4) {
                const int di = edi + 2 < 0 ? 0 : (edi + 2 > 4 ? 4 : edi + 2); // word_7448C[edi]
                tgt.ail_1f = kAilDuration[di];
            } else if (status == 2) {
                const int di = edi + 1 < 0 ? 0 : (edi + 1 > 4 ? 4 : edi + 1); // word_7448A[edi]
                tgt.ail_1f = kAilDuration[di];
                battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                                      battle_ready_queue_.end(), i),
                                          battle_ready_queue_.end());
                tgt.battle_gauge = 0;
            }
            if (battle_trace_)
                fprintf(stderr, "COMBAT: sweep pass=%d unit=%d st=%d dur=%u (sub_2A5BE)\n",
                        pass, tgt.id, status, (unsigned)tgt.ail_1f);
        }
    }
}

// sub_28728 equivalent: apply spell_damage to every list member (subtracting x100 raw), then
// post-process death / mode 1. The 45BFB popup and 45ED2 knockback are presentation and are
// skipped, with a trace line instead.
void CombatII::battle_apply_spell_hit(int caster_index, const std::vector<int>& targets,
                                      int base, int p1, int p2) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    const auto& caster = units_[(size_t)caster_index];
    for (int ti : targets) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& tgt = units_[(size_t)ti];
        if (!tgt.active || tgt.hp <= 0) continue;
        const int dmg = battle_spell_damage(caster, tgt, base, p1, p2);
        const int new_raw = battle_hp_raw(tgt) - dmg;
        const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
        const int delta = new_hp - tgt.hp;
        tgt.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: spell unit=%d target=%d base=%d dmg_x100=%d hp -> %d\n",
                    caster.id, tgt.id, base, dmg, new_hp);
        if (delta != 0) {
            VmEvent hp; hp.type = VmEventType::Hp; hp.hp_unit = ti;
            hp.hp_delta = delta;
            hp.hp_dir = caster.facing;
            hp.hp_killer = caster_index;
            play_hp_change(hp);
        }
        if (should_exit_) return;
    }
    for (int ti : targets) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        if (units_[(size_t)ti].active && units_[(size_t)ti].hp > 0)
            run_unit_hp_trigger(ti, caster_index, 1); // the sub_1ED45 else branch
        if (should_exit_) return;
    }
}

// sub_462A7(unit, amount_raw): HP healing. Clamp to max HP, halve on map kind 3 / weather 3,
// then require at least 100 (`cmp ecx,64h; jl loc_46356`) and add it in 100-raw steps. Each
// step draws the world, speaker frame and HP bar, presents, adds the chunk and waits one
// timer tick; the final frame gets a fresh 30-tick hold (loc_46342/46356).
void CombatII::battle_heal_462a7(int unit_index, int amount_raw) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    const int cur = battle_hp_raw(u), maxv = battle_max_hp_raw(u);
    int c = amount_raw;
    if (cur + amount_raw > maxv) c = maxv - cur;       // clamp before loc_462BE
    if (sce_map_kind_ == 3 || weather_ == 3) c /= 2;   // sar/sub/sar 1 (truncates toward 0)
    if (c < 100) return;                               // jl loc_46356
    game_timer_set(3);                                 // mov eax,3; call sub_1033B
    int applied = 0;
    int remaining = c;
    while (remaining > 0 && !should_exit_ && !skip_cutscene_) {
        // sub_454B3 -> sub_4A106 -> sub_5E9E8: draw the pre-chunk HP, present, then add.
        render_battle_frame();
        draw_speaker_frame(unit_index);
        video_.present();
        const int chunk = std::min(100, remaining);
        applied += chunk;
        remaining -= chunk;
        const int new_raw = cur + applied;
        const int new_hp = (new_raw + 99) / 100;
        u.hp_deficit = new_hp * 100 - new_raw;
        u.hp = new_hp;
        wait_game_tick();                              // sub_499E4
    }
    if (should_exit_) return;
    // Esc during the loop skips the presentation; apply the chunks that were not drawn yet
    // so the heal is not lost (the original cannot be interrupted).
    if (remaining > 0) {
        applied += remaining;
        remaining = 0;
        const int new_raw = cur + applied;
        const int new_hp = (new_raw + 99) / 100;
        u.hp_deficit = new_hp * 100 - new_raw;
        u.hp = new_hp;
    }
    // loc_46342: sub_45559 (454B3 + 5E9E8) redraws the map only, then a 30-tick hold.
    if (!skip_cutscene_) {
        present_cutscene_frame();                      // sub_45559
        game_timer_set(0x1E);
        wait_game_tick();
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: heal unit=%d +%d_raw hp=%d (sub_462A7)\n", u.id, c, u.hp);
}

// sub_2ACCB(caster, edx = base, bx = p1): handles the F0648 list. [0Dh]==2 takes damage
// (285E4*2); everything else is healed by the 184A4(0x37) bonus percentage, clamped to max
// HP, through 462A7 when inside the 12x12 viewport and added directly when outside.
void CombatII::battle_spell_heal_2accb(int caster_index, const std::vector<int>& targets, int base, int p1) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    game_timer_set(0x1E);                                // mov eax,1Eh; call sub_1033B
    bool killflag = false;
    for (int ti : targets) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& t = units_[(size_t)ti];
        const auto& caster = units_[(size_t)caster_index];
        if (t.unit_kind_0d == 2) {
            const int dmg = battle_spell_damage(caster, t, base, p1, -1) * 2;
            const int new_raw = battle_hp_raw(t) - dmg;
            const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
            const int delta = new_hp - t.hp;
            t.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
            t.hp = new_hp;
            if (delta != 0) {
                VmEvent hp; hp.type = VmEventType::Hp; hp.hp_unit = ti;
                hp.hp_delta = delta; hp.hp_dir = caster.facing; hp.hp_killer = caster_index;
                play_hp_change(hp);
            }
            killflag = true;
            continue;
        }
        if (t.code_b > 4) continue;                      // cmp byte [esi+5Ch],4; ja
        int bonus = (int)(int8_t)battle_lookup_184a4(caster.combat_type_5b, 0x37) +
                    (int)(int8_t)battle_lookup_184a4(caster.combat_type_5a, 0x37) +
                    (int)(int8_t)battle_lookup_184a4(caster.combat_type_5d, 0x37);
        bonus = (int)(int16_t)bonus;
        int amount = bonus * base / 100 + base;
        const int cur = battle_hp_raw(t), maxv = battle_max_hp_raw(t);
        if (cur + amount > maxv) amount = maxv - cur;
        const bool inside = t.x >= cam_tile_x_ && t.x <= cam_tile_x_ + 11 &&
                            t.y >= cam_tile_y_ && t.y <= cam_tile_y_ + 11;
        if (inside) {
            battle_heal_462a7(ti, amount);
        } else {
            const int new_raw = cur + amount;
            const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
            t.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
            t.hp = new_hp;
        }
        if (should_exit_) return;
    }
    if (killflag) {                                      // sub_287AF + sub_1ED45
        for (int ti : targets) {
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            if (units_[(size_t)ti].active && units_[(size_t)ti].hp > 0 && units_[(size_t)ti].unit_kind_0d == 2)
                run_unit_hp_trigger(ti, caster_index, 1);
            if (should_exit_) return;
        }
    }
}

// sub_2A500(caster, base, p1, p2): walks the units of every army hostile to the caster
// (1201D) in the N=1..4 chains, and for those not in the F0648 list that pass the sub_28596
// gate ([15h]&80 == 0 and [5Ch] < 7) subtracts sub_285E4 damage from [48h]. No presentation
// or death handling here - the later sub_1ED90 settles deaths. Returns the damaged units.
std::vector<int> CombatII::battle_spell_2a500_extras(int caster_index, const std::vector<int>& listed,
                                                      int base, int p1, int p2) {
    std::vector<int> extra;
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return extra;
    const int caster_army = battle_effective_army(units_[(size_t)caster_index]);
    for (size_t i = 0; i < units_.size(); ++i) {
        auto& o = units_[i];
        if ((int)i == caster_index || !o.active || o.hp <= 0) continue;
        if ((o.combat_flags_15 & 0x80) || o.code_b >= 7) continue;                 // sub_28596 gate
        if (!battle_armies_hostile(caster_army, battle_effective_army(o))) continue;   // sub_1201D (raw army, N)
        if (std::find(listed.begin(), listed.end(), (int)i) != listed.end()) continue; // list members are handled by 28728
        const int dmg = battle_spell_damage(units_[(size_t)caster_index], o, base, p1, p2);
        const int new_raw = battle_hp_raw(o) - dmg;
        const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
        o.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
        o.hp = new_hp;
        extra.push_back((int)i);
    }
    return extra;
}

// sub_1ED90 (off-list settlement): HP <= 0 -> sub_1DB1D (death), otherwise sub_53797 (hit
// trigger, mode 1).
void CombatII::battle_spell_1ed90_settle(int caster_index, const std::vector<int>& units) {
    for (int ei : units) {
        if (ei < 0 || (size_t)ei >= units_.size() || !units_[(size_t)ei].active) continue;
        if (units_[(size_t)ei].hp <= 0) play_unit_death(ei, caster_index);
        else run_unit_hp_trigger(ei, caster_index, 1);
        if (should_exit_) return;
    }
}

// sub_2A75A (rows 0..2 d6 / row 3 d4,d6 / 3BDC7,3C12D): 13x13 area effect.
//   eax = unit, edx = k (low level), ebx = kind (0 Meteor / 1 Star Tearer / 2 Mega Thunder /
//   3 Heal Wind / 4 Sun Light).
//   kind 0..2: centre = sub_2A6C0 (per-army highest [43h]) or the caster -> 13x13 hostile
//     list -> 28728(dword_283FE[kind], 70, kind) -> 29437/2A5BE (status 1<<kind, 30,
//     edi = k) -> 2A500 (off-list) -> 1ED90. With k >= 1 an allied heal follows
//     (base = dword_283FE[kind], 50).
//   kind 3: allies (armies 1 and 2 when the caster's army < 3, otherwise its own side) with
//     [0Dh] != 2 -> 2ACCB(5000, 50).
//   kind 4: 13x13 hostile list -> 28728(9000, 70, -1) -> 2A500 -> 1ED90; with k >= 1 an
//     allied heal of 8000.
//   dword_283FE sits next to dword_2840A, so [3] = 0x1388 (5000) and [4] = 0x1F40 (8000)
//   (ASM 33364).
void CombatII::battle_item_2a75a(int unit_index, int kind, int sub) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const int caster_army = battle_effective_army(units_[(size_t)unit_index]);
    auto addable = [&](const CombatUnitPlacement& o) { // sub_28596 gate
        return o.active && !(o.combat_flags_15 & 0x80) && o.code_b < 7;
    };
    battle_spell_announce(unit_index, battle_spell_name_);                 // sub_28889
    if (should_exit_ || (size_t)unit_index >= units_.size()) return;
    const int k = sub;
    auto heal_own_side = [&](int base, int p1) {
        std::vector<int> list;
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& o = units_[i];
            if (!addable(o) || o.unit_kind_0d == 2) continue;
            const int ea = battle_effective_army(o);
            const bool member = caster_army < 3 ? (ea == 1 || ea == 2) : (ea == caster_army);
            if (member) list.push_back((int)i);
        }
        // 2A75A loc_2AC03..2AC64: 2ACCB(base, 0x32) without 197AD.
        battle_spell_heal_2accb(unit_index, list, base, p1);
    };
    if (kind == 3) {
        // The 25DFE wide-heal presentation is skipped; only the healing.
        heal_own_side(5000, 0x32);                                         // dword_283FE[3]
        if (!should_exit_ && (size_t)unit_index < units_.size())
            reset_unit_pose(units_[(size_t)unit_index]);
        return;
    }
    if (kind == 4) {
        const auto& u0 = units_[(size_t)unit_index];
        int x0 = (int)(int8_t)u0.x - 6, y0 = (int)(int8_t)u0.y - 6;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 > (int)map_w_ - 12) x0 = (int)map_w_ - 12;
        if (y0 > (int)map_h_ - 12) y0 = (int)map_h_ - 12;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        std::vector<int> victims;
        for (int y = y0; y < y0 + 13; ++y) {
            for (int x = x0; x < x0 + 13; ++x) {
                if (x >= (int)map_w_ || y >= (int)map_h_) continue;
                const int oi = battle_unit_index_at(x, y);
                if (oi < 0 || (size_t)oi >= units_.size()) continue;
                const auto& o = units_[(size_t)oi];
                if (!battle_armies_hostile(caster_army, battle_effective_army(o), battle_hostility_inverted_))
                    continue;
                if (!addable(o)) continue;
                if (std::find(victims.begin(), victims.end(), oi) == victims.end()) victims.push_back(oi);
            }
        }
        battle_apply_spell_hit(unit_index, victims, 9000, 70, -1);         // sub_28728
        if (should_exit_) return;
        const std::vector<int> extra = battle_spell_2a500_extras(unit_index, victims, 9000, 70, -1);
        battle_spell_1ed90_settle(unit_index, extra);
        if (should_exit_) return;
        if (sub < 1) {
            reset_unit_pose(units_[(size_t)unit_index]);
            return;
        }
        heal_own_side(8000, 0x32);                                         // dword_283FE[4]
        if (!should_exit_ && (size_t)unit_index < units_.size())
            reset_unit_pose(units_[(size_t)unit_index]);
        return;
    }
    // Kind 0..2: the centre chosen by sub_2A6C0 - kind 0 sees all four armies, 1/2 see
    // armies 3 and 4, kind 3 excludes army 3 and kind 4 excludes army 4. Hostility is not
    // checked, as in the ASM.
    int cx = (int)(int8_t)units_[(size_t)unit_index].x;
    int cy = (int)(int8_t)units_[(size_t)unit_index].y;
    {
        int best = -1;
        for (const auto& o : units_) {
            if (!o.active || o.hp <= 0) continue;
            const int ea = battle_effective_army(o);
            if (ea < 1 || ea > 4) continue;
            if ((kind == 1 || kind == 2) && ea < 3) continue;
            if (kind == 3 && ea == 3) continue;
            if (kind == 4 && ea == 4) continue;
            if ((int)o.combat_value_43 > best) {
                best = (int)o.combat_value_43;
                cx = (int)(int8_t)o.x; cy = (int)(int8_t)o.y;
            }
        }
    }
    int x0 = cx - 6, y0 = cy - 6;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 > (int)map_w_ - 12) x0 = (int)map_w_ - 12;
    if (y0 > (int)map_h_ - 12) y0 = (int)map_h_ - 12;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    std::vector<int> victims;
    for (int y = y0; y < y0 + 13; ++y) {
        for (int x = x0; x < x0 + 13; ++x) {
            if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) continue;
            const int oi = battle_unit_index_at(x, y);
            if (oi < 0 || (size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!addable(o)) continue;
            if (!battle_armies_hostile(caster_army, battle_effective_army(o), battle_hostility_inverted_))
                continue;
            if (std::find(victims.begin(), victims.end(), oi) == victims.end()) victims.push_back(oi);
        }
    }
    const int base = (kind == 0) ? 2000 : (kind == 1 ? 4000 : 7000);       // dword_283FE[kind]
    battle_apply_spell_hit(unit_index, victims, base, 70, kind);           // sub_28728
    if (should_exit_) return;
    // 29437 + 2A5BE: status 1<<kind, chance 30, duration index = k.
    battle_spell_status_29437(unit_index, victims, 1 << kind, 30, k);
    battle_thunder_sweep_2a5be(unit_index, victims, 1 << kind, 30, k);
    if (should_exit_) return;
    // 2A500: the same damage to every off-list hostile unit, then 1ED90 settles.
    const std::vector<int> extra = battle_spell_2a500_extras(unit_index, victims, base, 70, kind);
    battle_spell_1ed90_settle(unit_index, extra);
    if (should_exit_) return;
    reset_unit_pose(units_[(size_t)unit_index]);
    if (k >= 1) heal_own_side(base, 0x32);
}
// sub_3BDC7 (dx 8): heal / Sun Light / MP +20. Pose 8, delay 0x5A, then it fires when
// rand(10) < (5 for class 0x25, else 3). rand(10) >= 4 gives "MP +20!" (MP += 20, clamped to
// [52h]); otherwise rand(10) < 5 picks Sun Light (4) and the rest Heal Wind (3).
void CombatII::battle_item_3bdc7(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    units_[(size_t)unit_index].anim_frame = 8;           // mov byte [eax+64h],8
    present_cutscene_frame();                            // sub_45559
    game_timer_set(0x5A);                                // sub_1033B(0x5A) + sub_499E4
    wait_game_tick();
    const int thr = (units_[(size_t)unit_index].combat_class == 0x25) ? 5 : 3;
    if (thr > combat_rand(10)) {
        if (combat_rand(10) < 4) {
            const bool auto_saved = battle_spell_auto_;
            battle_spell_auto_ = true;                   // sub_28422(1)
            const int kind = (combat_rand(10) < 5) ? 4 : 3;
            // loc_3BE70/3BE98: sub_28883 (eax = name) only fills dword_F13AC; the on-screen
            // announcement is sub_28889 inside sub_2A75A. (Loading sun/heal/cure.ssp via
            // sub_4F460 is skipped because the effect sprite engine is not ported.)
            battle_spell_name_ = (kind == 4) ? "Sun Light!" : "Heal Wind!";   // sub_28883
            battle_item_2a75a(unit_index, kind, 0);                           // sub_2A75A(edx=0, ebx=kind)
            if (!should_exit_) battle_item_2a75a_fx(unit_index, kind, 0,
                                                    (int)(int8_t)units_[(size_t)unit_index].x,
                                                    (int)(int8_t)units_[(size_t)unit_index].y);
            battle_spell_auto_ = auto_saved;             // sub_28422(0)
            if (should_exit_ || (size_t)unit_index >= units_.size()) return;
        } else {
            battle_unit_label(unit_index, "MP +20!");    // loc_3BEAF: sub_48612(unit, "MP +20!", 0x11)
            if (should_exit_ || (size_t)unit_index >= units_.size()) return;
            // loc_3BEAF: cure.ssp + sub_25C88 (cure.nsp particles, 50 frames).
            battle_ssp_play("cure.ssp");                     // 4F460("cure.ssp", 32h)
            auto& u = units_[(size_t)unit_index];
            battle_fx_25c88(unit_index, (int)(int8_t)u.x, (int)(int8_t)u.y, 0);
            int mp = (int)u.combat_mp_50 + 0x14;
            if ((int)u.combat_mp_max_52 < mp) mp = (int)u.combat_mp_max_52;
            u.combat_mp_50 = (uint16_t)mp;
            if (battle_trace_) fprintf(stderr, "COMBAT: item MP+20 unit=%d mp=%d\n", u.id, mp);
        }
    }
    reset_unit_pose(units_[(size_t)unit_index]);         // sub_12124
}

// ---- form change (sub_17D82 / sub_18262 / sub_17E6D) ----
// sub_17D82(unit, dx): looks up the form table unk_72E9A[[5Ch]*16]. 3 -> min([40h], w0)+w3,
// 4 -> min([42h], w1)+dword_71B6C[class]+w4, anything else -> w2.
int CombatII::battle_form_17d82(const CombatUnitPlacement& u, int sel) const {
    const int f = u.code_b;
    if (f >= 0x30) return 0;
    const uint16_t* r = kFormStat72E9A[f];
    const int cls = u.combat_class;
    const int w71b6c = cls < 0xC0 ? (int)(kClass71B68[cls][4] | (kClass71B68[cls][5] << 8)) : 0;
    if (sel == 3) {
        int dx = u.combat_str_40;
        const int bx = (int16_t)r[0];
        if (dx > bx) dx = bx;
        return (int16_t)(dx + (int16_t)r[3]);
    }
    if (sel == 4) {
        int dx = (int)(u.combat_value_42 & 0xFF);
        const int bx = (int16_t)r[1];
        if (dx > bx) dx = bx;
        return (int16_t)(dx + w71b6c + (int16_t)r[4]);
    }
    return (int16_t)r[2];
}

// sub_18262(unit, slot 0/1/2 = [5Ah]/[5Bh]/[5Dh]): removes the equipment and subtracts the
// ITEM.DAT bonuses again.
void CombatII::battle_form_18262(CombatUnitPlacement& u, int slot) const {
    int item = 0;
    if (slot == 0) { item = u.combat_type_5a; u.combat_type_5a = 0; }
    else if (slot == 1) { item = u.combat_type_5b; u.combat_type_5b = 0; }
    else if (slot == 2) { item = u.combat_type_5d; u.combat_type_5d = 0; }
    // Dec esi: item 0 would read (0-1)*66 from before the table, but callers only pass a
    // non-zero item.
    if (item <= 0) return;
    auto col = [&](int c) { return battle_lookup_184a4(item, c); };
    u.combat_str_40 = (uint8_t)(u.combat_str_40 - col(0));
    u.combat_mag41 = (uint8_t)(u.combat_mag41 - col(1));
    const uint8_t v42 = (uint8_t)((u.combat_value_42 & 0xFF) - col(2));
    u.combat_value_43 = (uint8_t)(u.combat_value_43 - col(3));
    u.combat_value_42 = (int16_t)(v42 | (u.combat_value_43 << 8));
    u.combat_value_44 = (int16_t)(u.combat_value_44 - (int8_t)col(4));
    if (slot != 0) u.combat_value_46 = (int16_t)(u.combat_value_46 - (int8_t)col(5));
    u.combat_def59 = (uint8_t)(u.combat_def59 - col(7));
    const int cls = u.combat_class;
    const int w71b6c = cls < 0xC0 ? (int)(kClass71B68[cls][4] | (kClass71B68[cls][5] << 8)) : 0;
    int v54 = (int)v42 + w71b6c;
    if ((int16_t)v54 < 0x3C) v54 = 0x3C;
    u.combat_value_54 = (int16_t)v54;
}

// sub_17E6D(unit, form): sets the form code [5Ch] = form, sprite 0x12B+form and class
// form+0x5F, then recomputes the stats from the tables.
void CombatII::battle_form_17e6d(CombatUnitPlacement& u, int form) const {
    form &= 0xFF;
    u.code_b = (uint8_t)form;
    u.sprite_type = (int)(uint16_t)(form + 0x12B);
    u.combat_class = (uint8_t)(form + 0x5F);
    if (u.combat_type_5a != 0) battle_form_18262(u, 0);
    if (u.combat_type_5b != 0) battle_form_18262(u, 1);
    if (u.combat_type_5d != 0) battle_form_18262(u, 2);
    const int cl = (int)(uint8_t)(form + 0x5F);
    u.inv_5e = 0;
    u.inv_5f = 0;
    const int cls = u.combat_class;
    const int w71b68 = cls < 0xC0 ? (int)(kClass71B68[cls][0] | (kClass71B68[cls][1] << 8)) : 0;
    const int b71b6a = cls < 0xC0 ? (int)kClass71B68[cls][2] : 0;
    const int w71b6c = cls < 0xC0 ? (int)(kClass71B68[cls][4] | (kClass71B68[cls][5] << 8)) : 0;
    auto w73108 = [&](int byte_off) -> int { // word at 73108 + byte_off (0 outside the table)
        const int idx = byte_off / 2;
        if (byte_off < 0 || idx < 0 || idx >= (int)(sizeof(kForm73108) / sizeof(kForm73108[0]))) return 0;
        return (int)(int16_t)kForm73108[idx];
    };
    auto set_max_raw = [&](int raw) {
        u.max_hp = raw <= 0 ? 0 : (raw + 99) / 100;
        u.max_hp_deficit = u.max_hp * 100 - raw;
    };
    const int esi = w73108(6 * (cl - 0x86) + 2) >> 1;   // dword_73108[..] sar 11h
    if (cl < 0x86) {
        u.combat_value_44 = (int16_t)battle_form_17d82(u, 3);
        u.combat_value_46 = (int16_t)w71b68;
        u.combat_def59 = (uint8_t)b71b6a;
        int v54 = (int16_t)battle_form_17d82(u, 4);
        if (v54 < 0x3C) v54 = 0x3C;
        u.combat_value_54 = (int16_t)v54;
        const int x = battle_form_17d82(u, 2);
        u.combat_mp_max_52 = 0;
        set_max_raw(x * 100);                           // ((x*3)*8+x)*4
        u.combat_mp_50 = u.combat_mp_max_52;
    } else {
        const int ch = cl - 0x86;
        if (cl < 0x92) {
            const int raw = w73108(6 * ch + 4) * (int)u.combat_value_43;
            set_max_raw(raw);
            const int e = raw / 100;
            u.combat_value_44 = (int16_t)(esi * e / 1000 + esi);
            u.combat_value_46 = (int16_t)w71b68;
            u.combat_def59 = (uint8_t)b71b6a;
            u.combat_value_54 = (int16_t)w73108(6 * ch + 6);
        } else {
            int edx, edi;
            if (cl < 0x99) { edx = (int)u.combat_value_43 * esi; edi = 100; }
            else { edx = (int)(u.combat_value_42 & 0xFF) * esi; edi = 200; }
            u.combat_value_44 = (int16_t)(edx / edi + esi);
            u.combat_value_46 = (int16_t)w71b68;
            u.combat_def59 = (uint8_t)b71b6a;
            u.combat_value_54 = (int16_t)w73108(6 * ch + 6);
            set_max_raw(w73108(6 * ch + 4) * 100);
        }
    }
    (void)w71b6c;
    if (u.code_b == 5 || u.code_b == 6 || u.code_b == 0x20) {   // cmp cl,5/6/20h -> [14h] |= 1
        u.combat_flags_14 = (uint8_t)(u.combat_flags_14 | 1);
        u.flying = true;
    }
}

// sub_3C548(unit, form): when the tile to the east is free, stashes the current record in
// [77h] and summons through a form change (large sprite). 1 = success, 0 = failure.
int CombatII::battle_item_3c548(int unit_index, int form) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    std::vector<int> grid;
    battle_ai_169d7_reset(unit_index, grid);                              // sub_169D7
    const int nx = (int)(int8_t)units_[(size_t)unit_index].x + 1;          // sub_49ABC dir 3 (x+1)
    const int ny = (int)(int8_t)units_[(size_t)unit_index].y;
    const bool padded = nx >= 2 && nx < (int)map_w_ - 2 && ny >= 4 && ny < (int)map_h_ - 4;
    if (padded && battle_unit_index_at(nx, ny) >= 0) return 0;             // sub_11DB2 != 0
    if (battle_ai_wc5_diff_read(grid, nx, ny) != -1) return 0;             // sub_13CBD != -1
    auto& u = units_[(size_t)unit_index];
    auto stored = std::make_shared<CombatUnitPlacement>(u);                 // sub_112FD + copy 0x88
    stored->list_seq = 0;                                                   // [edx+7Bh]=0
    u.stored_77 = stored;                                                   // [ebp+77h]=node
    u.facing = 0;                                                           // [63h]=0
    battle_form_17e6d(u, form);                                             // sub_17E6D
    u.combat_flags_1e = 0;
    u.hp = u.max_hp; u.hp_deficit = u.max_hp_deficit;                       // [48h]=[4Ch]
    u.combat_flags_15 = 0;
    u.ail_1f = u.ail_23 = u.ail_25 = u.ail_27 = 0xFFFF;                      // [16h..27h] = FFFF
    camera_center_on((int)(int8_t)u.x, (int)(int8_t)u.y);                   // sub_1124A
    battle_wc1_occupancy_preserve_or_close("recall transform");             // sub_11EE4/119B6/1198D/11E04 (large-sprite occupancy)
    // Presentation: 61147(1) -> 1124A -> 10971/10988(+0Ch) -> 1FEA4("recall.nsp") ->
    //   4F460("regen.ssp")/4F460("summon.ssp") -> 21944(x, y, [60h]) -> 11E04 ->
    //   61115 -> 1FFCD -> 12124 -> 45559. The shared 21944 driver is battle_fx_21944;
    //   61147/61115 are the tint-LUT setup/restore, covered by its tinted blits in the port.
    if (battle_nsp_load("recall.nsp")) {                                    // sub_1FEA4
        battle_ssp_play("regen.ssp");                                       // sub_4F460(32h)
        battle_ssp_play("summon.ssp");                                      // sub_4F460(33h)
        // 21944 receives [60h] (image slot); the port driver derives the D66A8 entry from
        // sprite_type, so this argument is informational.
        battle_fx_21944(unit_index, (int)(int8_t)u.x, (int)(int8_t)u.y,
                        (int)u.slot_hint);                                  // sub_21944
        battle_nsp_release();                                               // sub_1FFCD
        if (should_exit_) return 1;
    }
    reset_unit_pose(u);                                                     // sub_12124
    present_cutscene_frame();                                               // sub_45559
    if (battle_trace_)
        fprintf(stderr, "COMBAT: recall unit=%d form=%d class=%u type=%d hp=%d (sub_3C548)\n",
                u.id, form, (unsigned)u.combat_class, u.sprite_type, u.hp);
    return 1;
}

// sub_1808A: restores from the [77h] stash. The current coordinates, AI fields and list link
// ([7Bh]) are copied into the stash, then 0x88 bytes are copied back, the stash [14h] bit 0
// is cleared and [5Ch] set to 0, and the stash is released (sub_1134A).
// preserve_facing = true overwrites [63h] before the call as 3C70C does; false keeps the
// facing stored with the stash, as 3B614 does. present covers only the sub_45559 of 3C70C.
void CombatII::battle_restore_stored_1808a(int unit_index, bool preserve_facing, bool present) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& cur = units_[(size_t)unit_index];
    if (!cur.stored_77) {
        if (battle_trace_) fprintf(stderr, "COMBAT: revert unit=%d has no stored record (SCE link record unmodeled)\n", cur.id);
        return;
    }
    const int8_t saved_facing = cur.facing;                                 // 3C70C var_4 = [63h]
    CombatUnitPlacement restored = *cur.stored_77;
    restored.x = cur.x; restored.y = cur.y;                                 // [esi]=[ecx], [esi+1]=[ecx+1]
    restored.ai_home_x = cur.ai_home_x; restored.ai_home_y = cur.ai_home_y; // [6Fh]/[70h]
    restored.ai_watch_sel = cur.ai_watch_sel; restored.ai_watch_id = cur.ai_watch_id; // [71h]/[72h]
    restored.ai_target_unit = cur.ai_target_unit;                           // [73h]
    restored.list_seq = cur.list_seq;                                       // [7Bh]
    restored.combat_flags_14 = (uint8_t)(restored.combat_flags_14 & 0xFE);
    restored.flying = false;
    restored.code_b = 0;                                                    // [5Ch]=0
    restored.stored_77.reset();                                             // release the node (sub_1134A)
    restored.linked_class = -1;                                             // [77h]==0 -> [[77h]+11h] projection released too
    if (preserve_facing) restored.facing = saved_facing;                    // [63h]=var_4
    cur = restored;
    battle_wc1_occupancy_preserve_or_close(present ? "recall revert" : "dismount revert");
    reset_unit_pose(cur);                                                   // sub_12124
    if (present) present_cutscene_frame();                                  // sub_45559
    if (battle_trace_)
        fprintf(stderr, "COMBAT: revert unit=%d class=%u type=%d hp=%d (sub_1808A)\n",
                cur.id, (unsigned)cur.combat_class, cur.sprite_type, cur.hp);
}

// sub_3C70C: wipes the current form (sub_46710), restores the stored record through sub_1808A
// keeping [63h], reloads the image slot and redraws.
void CombatII::battle_item_3c70c(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const int8_t saved_facing = units_[(size_t)unit_index].facing;   // [esp+4+var_4]
    battle_fx_46710(unit_index);                                     // sub_46710
    if (should_exit_ || (size_t)unit_index >= units_.size()) return;
    units_[(size_t)unit_index].active = true;                        // sub_11E04 (re-occupy)
    units_[(size_t)unit_index].death_dissolve = 0;
    battle_restore_stored_1808a(unit_index, true, true);             // sub_1808A + 12124 + 45559
    if ((size_t)unit_index >= units_.size()) return;
    units_[(size_t)unit_index].facing = saved_facing;                // mov [ecx+63h],al
}

// ---- sub_3B7CE / sub_3B614 (4CB5E dx = 5, mount/dismount) ----
// Both run a cursor loop that takes the destination tile from sub_4A483. In the port the map
// click arrives as tx/ty, so a click that would keep 4A483 running is returned as 3
// (reselect). The ESC/right-click (-1) cancel of 4A483 is handled by the port's item-cancel
// path.

namespace {
// sub_13D53 + sub_13F08: clamps a rectangle into the map and fills the E8F46-equivalent grid
// with a word value. sub_13D53 shrinks the width and moves x to 2 when x < 2, shrinks to
// W-2-x when x+w > W-2, and applies the same rule to y (H-2). sub_13F08 writes the clamped
// rectangle without the (x-2,y-4) guard of 13D02.
void battle_wc5_fill_e8_rect(std::vector<int>& grid, int map_w, int map_h,
                             int x, int y, int w, int h, int value) {
    if (x < 2) { w += x - 2; x = 2; }
    else if (x + w > map_w - 2) w = map_w - 2 - x;
    if (y < 2) { h += y - 2; y = 2; }
    else if (y + h > map_h - 2) h = map_h - 2 - y;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const int gx = x + col, gy = y + row;
            if (gx < 0 || gy < 0 || gx >= map_w || gy >= map_h) continue;
            grid[(size_t)gy * (size_t)map_w + (size_t)gx] = value;
        }
    }
}
} // namespace

// sub_3B7CE(unit): mounting when [5Ch]==0.
//   Clears E8F46 with sub_13F8A, then marks the three tiles above the caster (sub_13F08
//   x,y-1,1,3) and the left/right neighbours (sub_13D02 x+-1,y) with 1. The clicked tile
//   needs 13CBD >= 0 and 11DB2 occupancy, and that unit must have [61h] >= 0x12C,
//   [77h] == 0 and [5Ch] != 0 (a mount).
//   On success: release the caster's occupancy (11F74 ebx=0) -> release the target footprint
//   (11EE4) -> remove the target from the list (1137A) -> hand back the caster's old image
//   slot (119B6) -> copy the caster's name/[10h] into the (discarded) target record -> the
//   new record is a copy of the caster (stash, [7Bh]=0), the caster record becomes the target
//   copy, the caster's [77h] points at the new record, [0Eh]/[6Fh..73h]/[7Bh] keep the
//   caster's values -> sub_17E6D(caster, [5Ch]) for the large form -> sub_11B77 sets the
//   stash [60h] to the rider's image slot -> sub_11E04 occupies the caster footprint.
//   Returns the caster pointer (the caller spends 40 TP, swaps [ecx] and returns -2).
//   The port keys its NC2 cache by type and does not model image slots
//   (1198D/119B6/11B77).
int CombatII::battle_item_3b7ce(int unit_index, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (map_w_ == 0 || map_h_ == 0) return 0;
    if (tx < 0 || ty < 0) return 3;                       // no click -> keep the cursor
    const int caster_id = units_[(size_t)unit_index].id;
    const int ux = (int)(int8_t)units_[(size_t)unit_index].x;
    const int uy = (int)(int8_t)units_[(size_t)unit_index].y;
    std::vector<int> e8((size_t)map_w_ * (size_t)map_h_, -1);          // sub_13F8A
    battle_wc5_fill_e8_rect(e8, (int)map_w_, (int)map_h_, ux, uy - 1, 1, 3, 1); // sub_13F08
    battle_ai_wc5_diff_write(e8, ux - 1, uy, 1);                      // sub_13D02
    battle_ai_wc5_diff_write(e8, ux + 1, uy, 1);
    if (battle_ai_wc5_diff_read(e8, tx, ty) < 0) return 3;             // sub_13CBD
    const int ti = battle_unit_index_at_footprint(tx, ty);             // sub_11DB2
    if (ti < 0 || ti == unit_index || (size_t)ti >= units_.size()) return 3;
    {
        const auto& target = units_[(size_t)ti];
        if ((int16_t)target.sprite_type < 0x12C || target.stored_77 || target.code_b == 0) {
            if (battle_trace_) fprintf(stderr, "COMBAT: mount unit=%d click (%d,%d) unit=%d rejected type=%d link=%d b=%d\n",
                                        caster_id, tx, ty, target.id, target.sprite_type,
                                        target.stored_77 ? 1 : 0, (int)target.code_b);
            return 3;
        }
    }
    // Copy of the caster (rider). [7Bh] becomes 0 in the new stash record; the merged unit
    // keeps its own value.
    CombatUnitPlacement saved = units_[(size_t)unit_index];
    const uint32_t caster_seq = saved.list_seq;
    saved.list_seq = 0;                                  // [edx+7Bh]=0
    CombatUnitPlacement merged = units_[(size_t)ti];      // copy of the mount
    // sub_1137A + sub_11EE4: the target leaves the list and its occupancy is cleared. The port
    // expresses the list exit as inactive and derives occupancy from units_. The original
    // leaves the caster's name/[10h] in the discarded target record, so the same values are
    // written here.
    units_[(size_t)ti].active = false;
    units_[(size_t)ti].stored_77.reset();
    units_[(size_t)ti].name = saved.name;
    units_[(size_t)ti].face = saved.face;
    merged.name = saved.name;
    merged.face = saved.face;
    merged.army = saved.army;
    merged.memory_army = saved.memory_army;
    merged.group = saved.group;
    merged.list_seq = caster_seq;
    merged.ai_home_x = saved.ai_home_x; merged.ai_home_y = saved.ai_home_y;
    merged.ai_watch_sel = saved.ai_watch_sel; merged.ai_watch_id = saved.ai_watch_id;
    merged.ai_target_unit = saved.ai_target_unit;
    merged.stored_77 = std::make_shared<CombatUnitPlacement>(saved); // sub_112FD + copy 0x88
    merged.linked_class = saved.combat_class;            // sub_174CF: [[77h]+11h] = the stash record's class
    units_[(size_t)unit_index] = merged;
    battle_form_17e6d(units_[(size_t)unit_index], units_[(size_t)unit_index].code_b); // sub_17E6D
    // sub_11B77: the stash [60h] is the rider image slot (forms 1..4 -> word_706D4[61h],
    // 5/6 -> word_70896[61h], +0x1F4). The port keys its NC2 cache by type and uses no slots.
    battle_wc1_occupancy_preserve_or_close("mount (3B7CE)");          // sub_11E04
    present_cutscene_frame();                                         // sub_45559
    if (battle_trace_)
        fprintf(stderr, "COMBAT: mount unit=%d id=%d form=%d type=%d hp=%d target=%d (sub_3B7CE)\n",
                caster_id, units_[(size_t)unit_index].id, (int)units_[(size_t)unit_index].code_b,
                units_[(size_t)unit_index].sprite_type, units_[(size_t)unit_index].hp, ti);
    return 1;
}

// sub_3B614(unit): dismounting when [5Ch] != 0.
//   Using the w/h of the D66A8 pose-0 record it marks x..x+w-1 by y-h..y+1 (13F08, height
//   h+2) and the side columns x-1 / x+w (13F08, height h) with 1. The click needs 13CBD >= 0
//   and no 11DB2 occupancy.
//   On success: a new record (a copy of the current mounted unit with [0Eh]=5, [77h]=0,
//   [10h]=0x64) is appended to the list (1134F, [7Bh]=0), its footprint is occupied (11E04),
//   and sub_1808A restores the caster from the stashed rider record. The caster coordinates
//   then become the clicked tile, with [5Ch]=0, [77h]=0, [60h]=1198D([61h]) and the clicked
//   tile occupied by the caster. [63h] stays the rider facing written by sub_1808A; unlike
//   3C70C, 3B614 does not rewrite it.
//   The link record SCE wrote is never loaded by the port, so a missing stored_77 returns 0.
int CombatII::battle_item_3b614(int unit_index, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (map_w_ == 0 || map_h_ == 0) return 0;
    if (!units_[(size_t)unit_index].stored_77) {
        if (battle_trace_) fprintf(stderr, "COMBAT: dismount unit=%d has no stored record (SCE link record unmodeled)\n",
                                    units_[(size_t)unit_index].id);
        return 0;
    }
    if (tx < 0 || ty < 0) return 3;                       // no click -> keep the cursor
    CombatUnitPlacement cur = units_[(size_t)unit_index]; // the mounted unit
    const int caster_id = cur.id;
    const int ux = (int)(int8_t)cur.x, uy = (int)(int8_t)cur.y;
    int fw = 1, fh = 1;
    battle_d66a8_footprint(cur.sprite_type, (int)cur.facing & 3, &fw, &fh); // unk_D66A8 pose 0
    std::vector<int> e8((size_t)map_w_ * (size_t)map_h_, -1);          // sub_13F8A
    battle_wc5_fill_e8_rect(e8, (int)map_w_, (int)map_h_, ux, uy - fh, fw, fh + 2, 1); // #1
    battle_wc5_fill_e8_rect(e8, (int)map_w_, (int)map_h_, ux - 1, uy - fh + 1, 1, fh, 1); // #2
    battle_wc5_fill_e8_rect(e8, (int)map_w_, (int)map_h_, ux + fw, uy - fh + 1, 1, fh, 1); // #3
    if (battle_ai_wc5_diff_read(e8, tx, ty) < 0) return 3;             // sub_13CBD
    if (battle_unit_index_at_footprint(tx, ty) >= 0) return 3;          // sub_11DB2 != 0
    // sub_112FD + copy 0x88: the mount record left behind. The original writes only the [0Eh]
    // byte as 5, overwrites [10h] with 0x64 and clears [77h]; the rest stays as it is.
    CombatUnitPlacement fresh = cur;
    fresh.army = 5;
    fresh.memory_army = 5;                               // [0Eh]=5
    fresh.stored_77.reset();                             // [eax+77h]=0
    fresh.linked_class = -1;
    fresh.face = 0x64;                                   // [eax+10h]=64h
    fresh.list_seq = battle_next_list_seq();             // sub_1134F tail insert ([7Bh]=0)
    units_.push_back(fresh);
    // sub_11E04: occupy the new record's footprint - the port derives this from units_.
    battle_restore_stored_1808a(unit_index, /*preserve_facing=*/false, /*present=*/false); // sub_1808A
    auto& u = units_[(size_t)unit_index];
    u.x = (int8_t)tx; u.y = (int8_t)ty;                  // [ebp] = the clicked tile
    u.code_b = 0;                                        // [5Ch]=0
    u.stored_77.reset();                                 // [ebp+77h]=0
    // sub_1198D([61h]) -> [60h]: the port stores slot_hint on the stash record instead.
    battle_wc1_occupancy_preserve_or_close("dismount (3B614)");       // sub_11F74
    present_cutscene_frame();                                         // sub_45559
    if (battle_trace_)
        fprintf(stderr, "COMBAT: dismount unit=%d id=%d type=%d hp=%d at=(%d,%d) fresh=%zu (sub_3B614)\n",
                caster_id, u.id, u.sprite_type, u.hp, tx, ty, units_.size() - 1);
    return 1;
}

// sub_3BC67(unit): clone - duplicates its own record onto each free tile at facing +1/+3
// (auto battle, [12h] = id+10). TP/[84h] are handled by the caller.
void CombatII::battle_clone_3bc67(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
        std::vector<int> grid;
        battle_ai_169d7_reset(unit_index, grid);                  // sub_169D7
        const int caster_id = u.id;
        const int caster_index = unit_index;
        for (int v = 1; v < 4 && !should_exit_; v += 2) {          // var_8 = 1, 3
            if (caster_index >= (int)units_.size()) break;
            const CombatUnitPlacement& src = units_[(size_t)caster_index];
            const int f = ((int)src.facing + v) & 3;
            int nx = (int)(int8_t)src.x, ny = (int)(int8_t)src.y;
            switch (f) { case 0: ++ny; break; case 1: --nx; break; case 2: --ny; break; default: ++nx; break; } // sub_49ABC
            const bool padded = nx >= 2 && nx < (int)map_w_ - 2 && ny >= 4 && ny < (int)map_h_ - 4;
            if (padded && battle_unit_index_at(nx, ny) >= 0) continue;       // sub_11DB2
            if (battle_ai_wc5_diff_read(grid, nx, ny) != -1) continue;       // sub_13CBD != -1
            CombatUnitPlacement nu = src;                                      // copy 0x88 bytes
            nu.auto_battle = 2;                                                // [83h]=2
            nu.item_use_12 = (uint8_t)(src.id + 10);                           // [12h]=[0Fh]+10
            nu.x = nx; nu.y = ny;
            nu.list_seq = battle_next_list_seq();                              // sub_1134F tail insert
            nu.battle_gauge = src.battle_gauge;
            nu.active = true;
            units_.push_back(nu);
            const size_t ci = units_.size() - 1;
            battle_wc1_occupancy_preserve_or_close("clone");
            camera_center_on(nx, ny);                                          // sub_1124A
            game_timer_set(3);
            for (int s = 0; s < 12 && !should_exit_; ++s) {                    // [63h]+1 &3, 12 times
                units_[ci].facing = (int8_t)((units_[ci].facing + 1) & 3);
                present_cutscene_frame();
                wait_game_tick();
            }
            if (battle_trace_)
                fprintf(stderr, "COMBAT: clone unit=%d id=%d at=(%d,%d) facing=%d (sub_3BC67)\n",
                        caster_id, units_[ci].id, nx, ny, (int)units_[ci].facing);
        }
}

// sub_3C09F(unit): Morale Down. Pose 8, delay 0x10, morale -2 (army 1..2 -> F0BA0+2,
// 3..4 -> F0BA0, floor 0x32), then restore the pose.
void CombatII::battle_item_3c09f(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    game_timer_set(0x10);                                // sub_1033B(0x10)
    u.anim_frame = 8;                                    // mov byte [ecx+64h],8
    present_cutscene_frame();                            // sub_45559
    wait_game_tick();                                    // sub_499E4
    battle_unit_label(unit_index, "Morale Down!");       // sub_48612(unit, "Morale Down!", 0x11), 36 frames
    if (should_exit_ || (size_t)unit_index >= units_.size()) return;
    const int eff = battle_effective_army(u);
    if (eff >= 1 && eff <= 2) {
        morale_hi_ = (int16_t)(morale_hi_ - 2);
        if (morale_hi_ < 0x32) morale_hi_ = 0x32;
    } else if (eff >= 3 && eff <= 4) {
        morale_lo_ = (int16_t)(morale_lo_ - 2);
        if (morale_lo_ < 0x32) morale_lo_ = 0x32;
    }
    reset_unit_pose(u);                                  // sub_12124
    if (battle_trace_) fprintf(stderr, "COMBAT: morale down unit=%d lo=%d hi=%d\n",
                                u.id, (int)morale_lo_, (int)morale_hi_);
}

// sub_3C3A5(unit): Ice Dragon - collect enemies in the 13x12 camera view, then 28728,
// four 2A500 passes and freeze. [84h]/TP are handled by the caller.
void CombatII::battle_item_3c3a5(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
        std::vector<int> victims;
        for (int r = 0; r < 12; ++r) {
            for (int c = 0; c < 13; ++c) {
                const int vx = cam_tile_x_ + c, vy = cam_tile_y_ + r;
                if (vx < 0 || vy < 0 || vx >= (int)map_w_ || vy >= (int)map_h_) continue;
                const int oi = battle_unit_index_at(vx, vy);
                if (oi < 0 || (size_t)oi >= units_.size()) continue;
                const auto& other = units_[(size_t)oi];
                if (!other.active || other.hp <= 0) continue;
                if (other.combat_flags_15 & 0x80) continue;
                if (other.code_b >= 7) continue;
                if (!battle_armies_hostile(battle_effective_army(u), battle_effective_army(other))) continue;
                bool dup = false;
                for (int v : victims) if (v == oi) dup = true;
                if (!dup) victims.push_back(oi);
            }
        }
        battle_unit_label(unit_index, "Ice Dragon!");  // sub_48612(unit, "Ice Dragon!", 0x11), 36 frames
        if (should_exit_ || (size_t)unit_index >= units_.size()) return;
        // sub_22D18(unit, edx=1770h, ebx=32h): p_light/icedrago presentation (the body reads
        // none of the three arguments).
        battle_fx_22d18(unit_index, 0, 0, 0);
        if (should_exit_) return;
        battle_apply_spell_hit(unit_index, victims, 6000, 50, 1); // sub_28728 (list damage)
        if (should_exit_) return;
        // sub_2A500(ecx=1, ebx=0x32, edx=0x1770): the same damage to every off-list hostile
        // unit, walking the N=1..4 army chains.
        const std::vector<int> extra = battle_spell_2a500_extras(unit_index, victims, 6000, 50, 1);
        reset_unit_pose(units_[(size_t)unit_index]);                        // sub_12124
        // The sub_287AF 25-frame hit flash and pose restore are presentation and skipped.
        battle_spell_status_29437(unit_index, victims, 2, 30, 2);           // sub_29437(edx=2, ebx=0x1E, ecx=2): freeze
        battle_thunder_sweep_2a5be(unit_index, victims, 2, 30, 2);          // sub_2A5BE(edx=2, ebx=0x1E, ecx=2)
        battle_spell_1ed90_settle(unit_index, extra);                       // sub_1ED90: settle (HP <= 0 dies, otherwise 53797)
}

// VM 0x09 (sub_51FF7): centres the camera on the unit, then runs subcommands 0..6:
//   0 Berserk (3BC17), 1 "Oh! My Lord!" (rand(2) ? Heal Wind : Sun Light, 2A75A sub 0),
//   2 Morale Down (3C09F), 3 Wish to Win (3C12D), 4 Ice Dragon (3C3A5),
//   5 transform summon (3C548, form = extra+0x0F), 6 clone (3BC67).
void CombatII::play_unit_effect(const VmEvent& ev) {
    const int ui = ev.hp_unit;
    if (ui < 0 || (size_t)ui >= units_.size() || !units_[(size_t)ui].active) return;
    camera_center_on((int)(int8_t)units_[(size_t)ui].x, (int)(int8_t)units_[(size_t)ui].y); // sub_1124A
    switch (ev.value) {
        case 0: battle_berserk_3bc17(ui); break;
        case 1: {
            const bool auto_saved = battle_spell_auto_;
            battle_spell_auto_ = true;                                   // sub_28422(1)
            battle_spell_name_ = "Oh! My Lord!";                         // sub_28883(eax = "Oh! My Lord!"); the announcement is sub_28889 in 2A75A
            const int kind = combat_rand(2) != 0 ? 3 : 4;
            battle_item_2a75a(ui, kind, 0);                              // sub_2A75A(edx=0, ebx=kind)
            if (!should_exit_) battle_item_2a75a_fx(ui, kind, 0,
                                                    (int)(int8_t)units_[(size_t)ui].x,
                                                    (int)(int8_t)units_[(size_t)ui].y);
            battle_spell_auto_ = auto_saved;                             // sub_28422(0)
            break;
        }
        case 2: { // sub_3C09F: morale down (same body as 4CB5E dx 11; the caller spends TP)
            battle_item_3c09f(ui);
            break;
        }
        case 3: battle_item_3c12d(ui); break;
        case 4: battle_item_3c3a5(ui); break;
        case 5: battle_item_3c548(ui, ev.army + 0x0F); break;
        case 6: battle_clone_3bc67(ui); break;
        default: break;
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: vm evt09 unit=%d sub=%d done\n", units_[(size_t)ui].id, ev.value);
}

// sub_3C12D (dx 12 / VM 09 sub 3): Wish to Win!!! - heals every ally (caster included) in the
// radius-5 diamond by 10000 (x100); when HP exceeds max-200 (nearly full) it also casts Heal
// Wind (sub 2), and Sun Light (sub 1) when level [2Bh] > 0x3C. It then removes the caster
// from the field (occupancy, image slot, morale +10, queue/list removal, then the
// morale/region/wipe triggers). The 253DC wideheal presentation is replaced by trace output.
void CombatII::battle_item_3c12d(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    units_[(size_t)unit_index].anim_frame = 8;           // mov byte [eax+64h],8
    present_cutscene_frame();                            // sub_45559
    battle_unit_label(unit_index, "Wish to Win!!!");     // sub_48612(unit, "Wish to Win!!!", 0x11), 36 frames
    if (should_exit_ || (size_t)unit_index >= units_.size()) return;
    int di;
    {
        const auto& u = units_[(size_t)unit_index];
        if (battle_max_hp_raw(u) - 0xC8 >= battle_hp_raw(u)) di = 0;       // jge -> xor edi,edi
        else di = (u.combat_level_2b > 0x3C) ? 2 : 1;                      // cmp byte [esi+2Bh],3Ch; jbe
    }
    const bool auto_saved = battle_spell_auto_;
    battle_spell_auto_ = true;                           // sub_28422(1)
    // 3C12D: 4F460(heal.ssp) -> 253DC -> (di>0) 2A75A kind 3 -> (di>1) 2A75A kind 4.
    battle_fx_3c12d(unit_index, (int)(int8_t)units_[(size_t)unit_index].x,
                    (int)(int8_t)units_[(size_t)unit_index].y, 0);
    if (should_exit_) { battle_spell_auto_ = auto_saved; return; }
    // sub_28A97(x, y, army, radius=5, ecx=0): adds the non-hostile units (1201D == 0) in the
    // diamond that pass the sub_28596 gate to the list.
    {
        std::vector<int> list;
        const int cx = (int)(int8_t)units_[(size_t)unit_index].x, cy = (int)(int8_t)units_[(size_t)unit_index].y;
        const int army = battle_effective_army(units_[(size_t)unit_index]);
        for (int y = cy - 5; y <= cy + 5; ++y) {
            for (int x = cx - 5; x <= cx + 5; ++x) {
                if (std::abs(x - cx) + std::abs(y - cy) > 5) continue;
                if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) continue;
                const int oi = battle_unit_index_at(x, y);
                if (oi < 0 || (size_t)oi >= units_.size()) continue;
                const auto& o = units_[(size_t)oi];
                if (battle_armies_hostile(army, battle_effective_army(o))) continue;
                if (!o.active || (o.combat_flags_15 & 0x80) || o.code_b >= 7) continue;
                if (std::find(list.begin(), list.end(), oi) == list.end()) list.push_back(oi);
            }
        }
        if (battle_trace_) fprintf(stderr, "COMBAT: wish-to-win unit=%d di=%d targets=%zu\n",
                                   units_[(size_t)unit_index].id, di, list.size());
        battle_spell_heal_2accb(unit_index, list, 10000, 0x1E);              // sub_2ACCB(0x2710)
    }
    if (should_exit_) { battle_spell_auto_ = auto_saved; return; }
    if (di > 0) {
        battle_spell_name_ = "Heal Wind!";                                   // sub_28883(eax="Heal Wind!")
        battle_item_2a75a(unit_index, 3, 2);                                 // sub_2A75A(edx=2, ebx=3)
        if (should_exit_) { battle_spell_auto_ = auto_saved; return; }
    }
    if (di > 1) {
        battle_spell_name_ = "Sun Light!";                                   // sub_28883(eax="Sun Light!")
        battle_item_2a75a(unit_index, 4, 1);                                 // sub_2A75A(edx=1, ebx=4)
        if (should_exit_) { battle_spell_auto_ = auto_saved; return; }
    }
    battle_spell_auto_ = auto_saved;                     // sub_28422(0)
    if ((size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    const int group = battle_unit_group(u);
    const int ea = battle_effective_army(u);
    if (ea == 1 || ea == 2) morale_lo_ = (int16_t)(morale_lo_ + 10);         // add word F0BA0,0Ah (no cap)
    else if (ea == 3) morale_hi_ = (int16_t)(morale_hi_ + 10);               // add word F0BA0+2,0Ah
    u.combat_flags_1e = 0;                               // mov byte [esi+1Eh],0
    u.combat_flags_15 = 0;                               // mov [esi+15h],al(=0)
    for (auto& o : units_) if (o.ai_target_unit == unit_index) o.ai_target_unit = -1; // sub_1DAEE: drop [+73h] references
    battle_remove_from_ready_queue(unit_index);          // sub_197AD
    u.anim_frame = 0;                                    // mov byte [esi+64h],0
    u.active = false;                                    // sub_11EE4 + sub_1137A + sub_1134F(D6620) + 119B6
    battle_wc1_occupancy_preserve_or_close("wish to win removal");
    present_cutscene_frame();
    run_morale_trigger();                                // sub_53E0F
    if (should_exit_) return;
    if (group >= 0 && battle_group_alive_count(group) == 0) run_army_wiped_trigger(group); // sub_53F94
    if (should_exit_) return;
    run_region_count_trigger();                          // sub_53C12
}

// ==== F13B0 spell handlers (dword_F13B0[q1], q1 = g*7+d) ====
// Target collection (28982/28A97/28B24/2AE44/2B031) and the bodies
// (28ED6/29273/2A1C6/2B1D0/2B391/2B9C8/2BA8E/2BC3B/2C00E/2C28F/2C36A/2C468/2C566) are
// ported. SSP/NSP/camera/wait presentation is skipped as elsewhere, and the gameplay
// (targets, damage, healing, status, summon, death) matches the original.

// sub_28596 gate: [15h]&0x80 (barrier/exit) and [5Ch] >= 7 (transformed) never enter the list.
bool CombatII::battle_spell_can_target(const CombatUnitPlacement& o) const {
    return o.active && o.hp > 0 && !(o.combat_flags_15 & 0x80) && o.code_b < 7;
}

// sub_28982 / sub_2AE44 manual path: one unit on the clicked tile.
//   mode 1 = hostile (28982 push 1), 0 = ally (28982 push 0), 2 = ally or hostile undead
//   (2AE44).
int CombatII::battle_spell_pick_single(int unit_index, int tx, int ty, int range_add, int mode) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (map_w_ == 0 || map_h_ == 0) return -1;
    if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return -1;
    const auto& u = units_[(size_t)unit_index];
    const int range = battle_lookup_184a4(u.combat_type_5b, 0x36) + range_add; // 28465
    const int caster_eff = battle_effective_army(u);
    const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
    for (size_t i = 0; i < units_.size(); ++i) {
        const auto& o = units_[i];
        if (!battle_spell_can_target(o)) continue;
        if ((int)(int8_t)o.x != tx || (int)(int8_t)o.y != ty) continue;
        if (std::abs((int)(int8_t)o.x - ux) + std::abs((int)(int8_t)o.y - uy) > range) continue;
        const bool hostile = battle_armies_hostile(caster_eff, battle_effective_army(o),
                                                   battle_hostility_inverted_);
        if (mode == 1 && !hostile) continue;
        if (mode == 0 && hostile) continue;
        if (mode == 2 && hostile && o.unit_kind_0d != 2) continue;
        return (int)i;
    }
    return -1;
}

// sub_28A97 / sub_2AF53 / sub_2AFE0: units inside the Manhattan diamond of radius around
// (cx,cy). mode 1 = hostile, 0 = ally, -1 = no hostility filter; undead_only keeps [0Dh]==2.
std::vector<int> CombatII::battle_spell_diamond(int unit_index, int cx, int cy, int radius,
                                                int mode, bool undead_only) {
    std::vector<int> out;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return out;
    const int caster_eff = battle_effective_army(units_[(size_t)unit_index]);
    if (radius < 0) radius = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        const int half = radius - (dy < 0 ? -dy : dy);
        for (int dx = -half; dx <= half; ++dx) {
            const int vx = cx + dx, vy = cy + dy;
            if (vx < 0 || vy < 0 || vx >= (int)map_w_ || vy >= (int)map_h_) continue;
            const int oi = battle_unit_index_at(vx, vy);
            if (oi < 0 || (size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!battle_spell_can_target(o)) continue;
            if (undead_only) {
                if (o.unit_kind_0d != 2) continue;                 // 2AFE0
            } else if (mode >= 0) {
                const bool hostile = battle_armies_hostile(caster_eff, battle_effective_army(o),
                                                           battle_hostility_inverted_);
                if (mode == 1 && !hostile) continue;
                if (mode == 0 && hostile) continue;
            }
            if (std::find(out.begin(), out.end(), oi) == out.end()) out.push_back(oi);
        }
    }
    return out;
}

// sub_28ED6 (rows 0/1/2 d0): Fire Arrow/Fire Ball/Fire Blast, Ice Missile/C.Missile/Diamond
// Dust, L.Bolt/D.L.Bolt/T.L.Bolt. 28982(push 1) picks one hostile unit, then
// 28728(3000+1500*k, 20, 2 for row 2 only). Range = 184A4(5B,0x36) + (k==0 ? 5 : 7).
int CombatII::battle_spell_28ed6(int unit_index, int row, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, k == 0 ? 5 : 7, 1);
    if (target < 0) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: spell %s target rejected unit=%d tile=(%d,%d)\n",
                    battle_spell_name_ ? battle_spell_name_ : "?",
                    units_[(size_t)unit_index].id, tx, ty);
        return 3;
    }
    battle_spell_announce(unit_index, battle_spell_name_);       // 28982 -> 28889
    if (should_exit_) return 0;
    battle_apply_spell_hit(unit_index, std::vector<int>{target}, 3000 + 1500 * k, 20,
                           (row == 2) ? 2 : 0);
    if (should_exit_) return 0;
    reset_unit_pose(units_[(size_t)unit_index]);                 // sub_12124
    run_unit_hp_trigger(unit_index, -1, 1);                      // sub_1ED45
    return 0;
}

// sub_29273 (rows 0/1/2 d1): Fire/Ice/T.Blade. 28982(push 0 = ally) picks one unit, then the
// 2914A enchant: [15h] = ([15h]&0xF8) | bit (bit = 1/2/4) with enchant duration
// [16h] = word_74488[k]. When code_b >= 7 the buff is skipped entirely (2914A
// `cmp byte ptr [eax+5Ch],7; jnb`).
int CombatII::battle_spell_29273(int unit_index, int bit, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, k <= 1 ? 5 : 7, 0);
    if (target < 0) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: blade target rejected unit=%d tile=(%d,%d)\n",
                    units_[(size_t)unit_index].id, tx, ty);
        return 3;
    }
    battle_spell_announce(unit_index, battle_spell_name_);       // 29273 -> 28889
    if (should_exit_) return 0;
    auto& tgt = units_[(size_t)target];
    if (tgt.code_b < 7) {                                        // 2914A `cmp [eax+5Ch],7; jnb`
        // ASM 34619..34634: when [15h]&7 is already set, the shipped code still prints the
        // debug text "AI Check! %x", waits for a key with getch_ and then continues.
        if (tgt.combat_flags_15 & 0x07) {
            char dbg[32];
            snprintf(dbg, sizeof(dbg), "AI Check! %x", (unsigned)tgt.combat_flags_15);
            draw_small_text_outlined(0, 0, dbg, 1);              // sub_60944(0,0,1,1)
            video_.present();
            wait_info_dismiss(0x40);                             // call getch_
        }
        tgt.combat_flags_15 = (uint8_t)((tgt.combat_flags_15 & 0xF8) | (bit & 0x07));
        // 2914A: after `add dl,2`, word_74488[dl] -> duration = kAilDuration[k+2]; [16h] is
        // shared by bits 1/2/4.
        const int di = (k + 2 > 4) ? 4 : (k + 2);
        tgt.buff_16 = kAilDuration[di];                          // [16h] = word_74488[k+2]
        // 2914A loc_29230: 8 silhouette frames, colours selected by the bit (the stored word
        // 0/1/2 in the original indexes dword_28332 at 0x00/0x08/0x10), then 12124. [64h] is
        // explicitly 0 during the flash.
        static const uint8_t kEnchantSil[3][8] = {
            {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17},   // bit 1
            {0x1F, 0x1E, 0x1D, 0x1D, 0x1B, 0x1A, 0x19, 0x18},   // bit 2
            {0x0F, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01},   // bit 4
        };
        const int sil = (bit == 1) ? 0 : (bit == 2) ? 1 : 2;
        tgt.anim_frame = 0;                                      // [64h] = 0
        game_timer_set(0x0C);                                    // sub_1033B(0Ch)
        present_cutscene_frame();                                // sub_45559
        for (int f = 0; f < 8 && !should_exit_; ++f) {
            battle_fx_silhouette_13669(target, kEnchantSil[sil][f]);
            battle_fx_commit();                                  // sub_5E9E8
            wait_game_tick();                                    // sub_499E4
        }
        reset_unit_pose(tgt);                                    // sub_12124
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: blade skip unit=%d target=%d (code_b>=7)\n",
                units_[(size_t)unit_index].id, tgt.id);
    }
    return 0;
}

// sub_2A1C6 (rows 0/1/2 d5, row 3 d3, row 4 d5, row 6 d6): automatic diamond.
//   Radius = 184A4(5B,0x36) + (2*k+3); kind 4 (Barrier) is 2 smaller and picks allies.
//   kind 0/1/2 = Explosion (25000) / Leopard (20000) / P.Lighting (24000+2000k), p1 = 50,
//     p2 = kind, status 1/2/4 (chance 30, edi = k). kind 3 = Turn Undead (hostile [0Dh]==2
//     dies outright). kind 4 = Barrier: [15h] |= 0xE0, [1Eh] = 0, [1Ch] = word_7448A[k].
//   kind 5 = Dark Zone / Atomic Blast / Amageddon: 30000/70/3.
int CombatII::battle_spell_2a1c6(int unit_index, int kind, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const auto& u = units_[(size_t)unit_index];
    const int cx = (int)(int8_t)u.x, cy = (int)(int8_t)u.y;
    int radius = battle_lookup_184a4(u.combat_type_5b, 0x36) + (2 * k + 3); // 28465 + 2*k+3
    if (kind == 4) radius -= 2;
    const int mode = (kind == 4) ? 0 : 1;                        // 28A97 filter (1201D result)
    std::vector<int> targets = battle_spell_diamond(unit_index, cx, cy, radius, mode);
    if (targets.empty()) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);       // sub_28889
    if (should_exit_) return 0;
    switch (kind) {
        case 0: battle_apply_spell_hit(unit_index, targets, 25000, 50, 0); break;
        case 1: battle_apply_spell_hit(unit_index, targets, 20000, 50, 1); break;
        case 2: battle_apply_spell_hit(unit_index, targets, 24000 + 2000 * k, 50, 2); break;
        case 3: {                                                // Turn Undead
            const int caster_eff = battle_effective_army(u);
            for (int ti : targets) {
                if (ti < 0 || (size_t)ti >= units_.size()) continue;
                auto& o = units_[(size_t)ti];
                if (!o.active || o.hp <= 0 || o.unit_kind_0d != 2) continue;
                if (!battle_armies_hostile(caster_eff, battle_effective_army(o),
                                           battle_hostility_inverted_)) continue;
                play_unit_death(ti, unit_index);                 // sub_1DB1D
                if (should_exit_) return 0;
            }
            if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
            return 0;
        }
        case 4: {                                                // Barrier
            for (int ti : targets) {
                if (ti < 0 || (size_t)ti >= units_.size()) continue;
                auto& o = units_[(size_t)ti];
                if (!o.active || o.hp <= 0 || o.sprite_type >= 0x12C) continue;
                o.combat_flags_15 = (uint8_t)(o.combat_flags_15 | 0xE0);
                o.combat_flags_1e = 0;
                const int di = (k + 1 > 4) ? 4 : (k + 1);        // word_7448A[k]
                o.buff_1c = kAilDuration[di];                    // [1Ch]
            }
            if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
            return 0;
        }
        default: battle_apply_spell_hit(unit_index, targets, 30000, 70, 3); break;
    }
    if (should_exit_) return 0;
    reset_unit_pose(units_[(size_t)unit_index]);                 // sub_12124
    if (kind < 3) {
        const int status = (kind == 2) ? 4 : (kind + 1);
        battle_spell_status_29437(unit_index, targets, status, 30, k); // sub_29437
    }
    if (!should_exit_) run_unit_hp_trigger(unit_index, -1, 1);   // sub_1ED45
    return 0;
}

// sub_2B1D0 (row 3 d2 Cure / row 6 d0 Confuse): 28B24 manual single target.
//   kind 0: [1Eh] = 0 (clears every status). If the unit was army 1 with gauge FF, not
//   auto-controlled and confused, 196D0 requeues it.
//   kind 1: 28947 roll at 30% -> [1Eh] |= 0x10, [23h] = word_7448C[k]; army 1 with gauge FF
//   is removed from the queue with 19718.
int CombatII::battle_spell_2b1d0(int unit_index, int kind, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, 5, kind == 0 ? 0 : 1);
    if (target < 0) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    auto& t = units_[(size_t)target];
    const auto& c = units_[(size_t)unit_index];
    if (t.sprite_type >= 0x12C) return 0;
    // sub_25C88: 50-frame cure.nsp particle. The original calls it before applying the
    //   status (ecx = target, eax/edx = target type x/y, ebx = caster level).
    battle_fx_25c88(target, (int)(int8_t)t.x, (int)(int8_t)t.y, k);
    if (kind == 0) {
        const bool had_confuse = (t.combat_flags_1e & 0x10) != 0;
        const bool ready_unit = battle_effective_army(t) == 1 && t.battle_gauge == 0xFF &&
                                t.auto_battle == 0;
        t.combat_flags_1e = 0;
        if (had_confuse && ready_unit) {                         // sub_196D0: re-add to queue tail
            if (std::find(battle_ready_queue_.begin(), battle_ready_queue_.end(), target) ==
                battle_ready_queue_.end())
                battle_ready_queue_.push_back(target);
        }
    } else {
        const int roll_base = 0x1E + ((int)c.combat_level_2b - (int)t.combat_level_2b);
        const int chance = roll_base < 10 ? 10 : (roll_base > 70 ? 70 : roll_base); // 28947
        if (combat_rand(100) < chance) {
            t.combat_flags_1e = (uint8_t)(t.combat_flags_1e | 0x10);
            const int di = (k + 2 > 4) ? 4 : (k + 2);
            t.ail_23 = kAilDuration[di];                          // word_7448C[k] (= 74488[k+2])
            if (battle_effective_army(t) == 1 && t.battle_gauge == 0xFF) { // sub_19718
                battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(),
                                                      battle_ready_queue_.end(), target),
                                          battle_ready_queue_.end());
            }
        }
    }
    reset_unit_pose(t);
    return 0;
}

// sub_2B391 (row 4 d0..d4,d6): one-shot diamond buff for allies and hostiles.
//   bit 0x08/0x10/0x20/0x40 = Power UP/Speed UP/Magic Armor/Bless -> [15h] |= bit,
//   [16h+2*k] = word_7448A[k]. 0x20 is skipped when 0x40 is already set.
//   bit 0x800 (Silence) / 0x2000 (Curse) = hostile targets -> [1Eh] |= 8 / 0x20,
//   duration in [1Fh+2*k].
int CombatII::battle_spell_2b391(int unit_index, int bit, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const auto& u = units_[(size_t)unit_index];
    int cx = tx, cy = ty;
    if (cx < 0 || cy < 0 || cx >= (int)map_w_ || cy >= (int)map_h_) {
        cx = (int)(int8_t)u.x; cy = (int)(int8_t)u.y;
    }
    const bool on_hostiles = ((bit & 0x2800) != 0);              // `test byte [var_4+1],28h`
    const int radius = battle_lookup_184a4(u.combat_type_5b, 0x36) + 5;
    std::vector<int> targets = battle_spell_diamond(unit_index, cx, cy, radius,
                                                     on_hostiles ? 1 : 0);
    if (targets.empty()) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    const int di = (k < 0) ? 0 : (k > 4 ? 4 : k);
    const int di_a = (di + 1 > 4) ? 4 : di + 1;              // word_7448A[i] = T[i+1]
    for (int ti : targets) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& o = units_[(size_t)ti];
        if (!o.active || o.hp <= 0 || o.sprite_type >= 0x12C) continue;
        switch (bit) {
            case 0x08: o.combat_flags_15 = (uint8_t)(o.combat_flags_15 | 0x08); o.buff_16 = kAilDuration[di_a]; break;
            case 0x10: o.combat_flags_15 = (uint8_t)(o.combat_flags_15 | 0x10); o.buff_18 = kAilDuration[di_a]; break;
            case 0x20:
                if (o.combat_flags_15 & 0x40) continue;          // `test byte [ecx+15h],40h`
                o.combat_flags_15 = (uint8_t)(o.combat_flags_15 | 0x20);
                o.buff_1a = kAilDuration[di_a];
                break;
            case 0x40: o.combat_flags_15 = (uint8_t)(o.combat_flags_15 | 0x40); o.buff_1c = kAilDuration[di_a]; break;
            case 0x800: {                                          // loc_2B558 Silence
                o.combat_flags_1e = (uint8_t)(o.combat_flags_1e | 0x08);
                // `add ecx, 2*k` -> [1Fh+2k] = word_7448A[di]
                if (k <= 0) o.ail_1f = kAilDuration[di_a];
                else if (k == 1) o.ail_21 = kAilDuration[di_a];
                else o.ail_23 = kAilDuration[di_a];
                break;
            }
            case 0x2000: {                                         // loc_2B58F Curse
                o.combat_flags_1e = (uint8_t)(o.combat_flags_1e | 0x20);
                if (k <= 0) o.ail_1f = kAilDuration[di];           // [1Fh+2k] = word_74488[di]
                else if (k == 1) o.ail_21 = kAilDuration[di];
                else o.ail_23 = kAilDuration[di];
                break;
            }
            default: break;
        }
    }
    if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
    return 0;
}

// sub_2B9C8 (row 3 d0 Heal): 2AE44 picks one target (ally or hostile undead) ->
//   2ACCB((k+1)*5000, 30). Range = 184A4(5B, 0x36) + (k == 0 ? 5 : 7).
int CombatII::battle_spell_2b9c8(int unit_index, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, k <= 1 ? 5 : 7, 2);
    if (target < 0) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    battle_spell_heal_2accb(unit_index, std::vector<int>{target}, (k + 1) * 5000, 0x1E);
    if (!should_exit_ && (size_t)unit_index < units_.size())
        reset_unit_pose(units_[(size_t)unit_index]);
    return 0;
}

// sub_2BA8E (row 3 d1 Wide Heal): 2B031 manual centre -> 2AF53 heals allies
//   ([0Dh] != 2) with dword_2840A[k] = 5000/8000/12000, p1 = 30, then 2AFE0 damages
//   undead ([0Dh] == 2) with dword_2840A[k+2] = 12000/15000/18000, p1 = 30.
//   Radius = 184A4(5B, 0x36) + k + 2.
int CombatII::battle_spell_2ba8e(int unit_index, int k, int tx, int ty) {
    static constexpr int kWide[3] = {5000, 8000, 12000};   // dword_2840A (only 3 dwords copied)
    const int wk = (k < 0) ? 0 : (k > 2 ? 2 : k);
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const auto& u = units_[(size_t)unit_index];
    if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return 3;
    const int base_range = battle_lookup_184a4(u.combat_type_5b, 0x36);
    if (std::abs(tx - (int)(int8_t)u.x) + std::abs(ty - (int)(int8_t)u.y) > base_range + 5)
        return 3;
    const int radius = base_range + k + 2;
    std::vector<int> friends = battle_spell_diamond(unit_index, tx, ty, radius, 0);
    friends.erase(std::remove_if(friends.begin(), friends.end(), [&](int i) {
        return units_[(size_t)i].unit_kind_0d == 2;              // 2AF53: skip undead
    }), friends.end());
    std::vector<int> undead = battle_spell_diamond(unit_index, tx, ty, radius, -1, true);
    if (friends.empty() && undead.empty()) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    // ASM 38222..38233: only 3 dwords of dword_2840A are copied, and both the ally heal
    // and the undead damage use arr[k] (the original has no upper 3 values).
    if (!friends.empty()) battle_spell_heal_2accb(unit_index, friends, kWide[wk], 0x1E);
    if (should_exit_) return 0;
    if (!undead.empty()) battle_spell_heal_2accb(unit_index, undead, kWide[wk], 0x1E);
    if (!should_exit_ && (size_t)unit_index < units_.size())
        reset_unit_pose(units_[(size_t)unit_index]);
    return 0;
}

// sub_2BC3B (row 3 d5 Revible): picks a same-side corpse from the corpse list (137E7)
//   and raises it in the front tile. HP/MP = 30%/60%/100% for k 0/1/2
//   (2BC3B loc_2BD55/2BD91/2BDC1). An empty or unreachable tile re-selects.
int CombatII::battle_spell_2bc3b(int unit_index, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int ci = battle_corpse_take(unit_index, false);        // sub_137E7(legion)
    if (ci < 0) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: revible unit=%d no corpse\n", units_[(size_t)unit_index].id);
        return 4;
    }
    const auto& caster = units_[(size_t)unit_index];
    int nx = (int)(int8_t)caster.x, ny = (int)(int8_t)caster.y;
    switch (caster.facing & 3) {                                 // sub_49ABC
        case 0: ++ny; break;
        case 1: --nx; break;
        case 2: --ny; break;
        default: ++nx; break;
    }
    std::vector<int> grid;
    battle_ai_169d7_reset(unit_index, grid);                     // sub_169D7
    if (battle_ai_wc5_diff_read(grid, nx, ny) != -1) return 4;    // sub_13CBD
    if (battle_unit_index_at(nx, ny) >= 0) return 4;              // sub_11DB2
    battle_spell_announce(unit_index, battle_spell_name_);        // sub_28889
    if (should_exit_ || (size_t)unit_index >= units_.size()) return 0;
    // 2BC3B: heal.ssp + sun2.ssp -> sub_59A70(revible.nsp). The original plays this
    //   before placing the unit ([0]/[1]/[60h] are written after the effect ends).
    battle_ssp_play("heal.ssp");                                  // 4F460(32h)
    battle_ssp_play("sun2.ssp");                                  // 4F460(33h)
    battle_fx_59a70(unit_index, ci);                              // sub_59A70(eax=caster, edx=revived)
    if (should_exit_) return 0;
    auto& z = units_[(size_t)ci];
    z.active = true;
    z.x = (int8_t)nx; z.y = (int8_t)ny;
    z.stored_77.reset();
    z.linked_class = -1;
    const int pct = (k >= 2) ? 100 : (k == 1 ? 60 : 30);
    const int hp_raw = battle_max_hp_raw(z) * pct / 100;          // [48h] = [4Ch]*pct/100
    z.hp = hp_raw <= 0 ? 0 : (hp_raw + 99) / 100;
    z.hp_deficit = z.hp * 100 - hp_raw;
    z.combat_mp_50 = (uint16_t)((int)z.combat_mp_max_52 * pct / 100); // [50h]
    reset_unit_pose(z);                                          // sub_12124
    battle_wc1_occupancy_preserve_or_close("revible");            // sub_11E04
    present_cutscene_frame();                                    // sub_45559
    if (battle_trace_)
        fprintf(stderr, "COMBAT: revible unit=%d corpse=%d id=%d at=(%d,%d) hp=%d pct=%d\n",
                caster.id, ci, z.id, nx, ny, z.hp, pct);
    return 0;
}

// sub_19600: items exempt from unequip/destroy (0x16, 0x1A, 0x1D, 0x20..0x24, 0x3C, 0x46, 0x67,
// 0x78).
static bool battle_item_protected_19600(int id) {
    if (id == 0x16) return true;
    if (id >= 0x19) {
        if (id == 0x1A) return true;
        if (id == 0x1D) return true;
    }
    if (id >= 0x20 && id <= 0x24) return true;
    if (id == 0x3C) return true;
    if (id == 0x46) return true;
    if (id == 0x67) return true;
    return id == 0x78;
}

// sub_2C00E (row 6 d1 Melt Item): 28982(push 1) picks one hostile; a 1/(8>>k) roll
//   destroys one item. Check order [5Eh] -> [5Fh] -> [5Dh] -> [5Ah] -> [5Bh];
//   [5A/5B/5D] roll back stat bonuses with 18262.
int CombatII::battle_spell_2c00e(int unit_index, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, 5, 1);
    if (target < 0) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    auto& o = units_[(size_t)target];
    if (combat_rand(8 >> k) != 0) {                              // `mov eax,8; sar eax,cl; 499BD`
        reset_unit_pose(o);
        return 0;
    }
    bool destroyed = false;
    int destroyed_item = 0;
    auto take = [&](uint8_t& slot, int slot_kind) {
        if (destroyed || slot == 0) return;
        if (battle_item_protected_19600((int)slot)) return;
        if (slot_kind == 2) battle_form_18262(o, 2);             // [5Dh]
        else if (slot_kind == 0) battle_form_18262(o, 0);        // [5Ah]
        else if (slot_kind == 1) battle_form_18262(o, 1);        // [5Bh]
        destroyed_item = (int)slot;
        slot = 0;
        destroyed = true;
    };
    take(o.inv_5e, -1);                                          // [5Eh]
    take(o.inv_5f, -1);                                          // [5Fh]
    take(o.combat_type_5d, 2);                                   // [5Dh]
    take(o.combat_type_5a, 0);                                   // [5Ah]
    take(o.combat_type_5b, 1);                                   // [5Bh]
    if (destroyed) {
        // ASM 38889..38930: 184CA yields the destroyed item name, then an icon is drawn
        // with sub_5E048(x, 5Ch, "[%s] ..."). Deviation: the port puts the name in the
        // label instead of the icon popup (sub_5F3FD / EFDB4 table).
        std::string iname_raw;
        if (!battle_lookup_table_.empty() && destroyed_item >= 1) {
            const size_t off = ((size_t)destroyed_item - 1) * 66 + 1;
            for (size_t k = off; k < battle_lookup_table_.size() && battle_lookup_table_[k] != 0; ++k)
                iname_raw.push_back((char)battle_lookup_table_[k]);
        }
        const std::string label = iname_raw.empty() ? std::string("Melt Item!")
                                                    : ("[" + Font::johab_to_utf8(iname_raw) + "]");
        battle_unit_label(unit_index, label.c_str());
        if (battle_trace_)
            fprintf(stderr, "COMBAT: melt unit=%d target=%d item=%d (sub_2C00E)\n",
                    units_[(size_t)unit_index].id, o.id, destroyed_item);
    }
    if (!should_exit_ && (size_t)target < units_.size()) reset_unit_pose(units_[(size_t)target]);
    return 0;
}

// sub_2C28F (row 6 d2 Death): 28982(push 1) picks one hostile; a 1/(8>>k) roll plus
//   sprite < 0x12C and [43h] < 32 kills it through sub_1DB1D.
int CombatII::battle_spell_2c28f(int unit_index, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, 5, 1);
    if (target < 0) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    const auto& o = units_[(size_t)target];
    if (combat_rand(8 >> k) == 0 && o.sprite_type >= 0 && o.sprite_type < 0x12C &&
        (int)o.combat_value_43 < 0x32) {
        play_unit_death(target, unit_index);                     // sub_1DB1D
        if (should_exit_) return 0;
    }
    if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
    return 0;
}

// sub_2C36A + sub_2B5C4 (row 6 d3 Soul Recall): raises one corpse per front/side tile
//   (29C5A direction table) as [0Dh]=2 undead. [12h] = caster id + 10, id = 0xFF,
//   full HP, MP 0, gauge 0.
int CombatII::battle_spell_2c36a(int unit_index, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int count = k + 1;                                     // 29C5A count = edx+1
    const int facing = (int)units_[(size_t)unit_index].facing & 3;
    int dirs[3] = {facing, (facing + 1) & 3, (facing + 3) & 3};  // 29C5A var_C[0..2]
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    battle_remove_old_summons(unit_index);                       // sub_29EE4
    std::vector<int> grid;
    battle_ai_169d7_reset(unit_index, grid);                     // sub_169D7
    const int ux = (int)(int8_t)units_[(size_t)unit_index].x;
    const int uy = (int)(int8_t)units_[(size_t)unit_index].y;
    int raised = 0;
    for (int i = 0; i < count && !should_exit_; ++i) {
        int nx = ux, ny = uy;
        switch (dirs[i]) {                                       // sub_49ABC
            case 0: ++ny; break;
            case 1: --nx; break;
            case 2: --ny; break;
            default: ++nx; break;
        }
        if (battle_unit_index_at(nx, ny) >= 0) continue;         // sub_11DB2
        if (battle_ai_wc5_diff_read(grid, nx, ny) < 0) continue;  // sub_13CBD
        const int ci = battle_corpse_take(unit_index, true);      // sub_137E7(al=0): random corpse
        if (ci < 0) break;
        auto& z = units_[(size_t)ci];
        auto& c = units_[(size_t)unit_index];
        z.active = true;
        z.x = (int8_t)nx; z.y = (int8_t)ny;
        z.unit_kind_0d = 2;                                      // [0Dh]=2
        z.id = 0xFF;                                             // [0Fh]=0xFF
        z.item_use_12 = (uint8_t)((unsigned)c.id + 10u);         // [12h]=[0Fh]+10
        z.army = c.army; z.memory_army = c.memory_army;          // [0Eh]
        z.facing = c.facing;                                     // [63h]
        z.anim_frame = 3;                                        // [64h]=3
        z.combat_flags_14 = 0;                                   // [14h]=0
        z.combat_mp_50 = 0;                                      // [50h]=0
        z.battle_gauge = 0;                                      // [56h]=0
        z.combat_acc57 = 0;                                      // [57h]=0
        z.ai_field_81 = 0;                                       // [81h]=0
        z.ai_mode = (int8_t)0xFF;                                // [82h]=0xFF
        z.combat_flags_1e = 0;
        z.auto_battle = (battle_effective_army(c) == 1) ? 2 : 0;  // [83h]=2
        z.hp = z.max_hp; z.hp_deficit = z.max_hp_deficit;        // [48h]=[4Ch]
        z.stored_77.reset();
        z.linked_class = -1;
        reset_unit_pose(z);
        battle_wc1_occupancy_preserve_or_close("soul recall");
        ++raised;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: soul recall unit=%d corpse=%d at=(%d,%d) (sub_2B5C4)\n",
                    c.id, ci, nx, ny);
    }
    if (!should_exit_ && (size_t)unit_index < units_.size())
        reset_unit_pose(units_[(size_t)unit_index]);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: soul recall done unit=%d raised=%d/%d\n",
                units_[(size_t)unit_index].id, raised, count);
    return 0;
}

// sub_2C468 (row 6 d4 DNA Change): 28982(push 1) picks one hostile. With neither
//   status 0x20 nor 0x40 set it applies [1Eh] |= 0x40, sprite slot 64h and
//   [25h] = word_7448C[k] (duration). 275A4(dna.nsp) is presentation only and skipped.
int CombatII::battle_spell_2c468(int unit_index, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const int target = battle_spell_pick_single(unit_index, tx, ty, 5, 1);
    if (target < 0) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    auto& o = units_[(size_t)target];
    if (o.sprite_type >= 0 && o.sprite_type < 0x12C &&
        !(o.combat_flags_1e & 0x20) && !(o.combat_flags_1e & 0x40)) {
        o.combat_flags_1e = (uint8_t)(o.combat_flags_1e | 0x40);
        o.slot_hint = 0x64;                                      // [60h]=64h
        const int di = (k + 2 > 4) ? 4 : (k + 2);
        o.ail_25 = kAilDuration[di];                             // [25h] = word_7448C[k]
    }
    if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
    return 0;
}

// sub_2C566 (row 6 d5 Black Hole): 28B24 centre (clicked tile), radius 184A4+5, hostile
//   roster. Each unit dies when the 1/(8>>k) roll, sprite < 0x12C and [43h] < 32 pass.
int CombatII::battle_spell_2c566(int unit_index, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    const auto& u = units_[(size_t)unit_index];
    if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return 3;
    const int base_range = battle_lookup_184a4(u.combat_type_5b, 0x36);
    if (std::abs(tx - (int)(int8_t)u.x) + std::abs(ty - (int)(int8_t)u.y) > base_range + 5)
        return 3;
    // 2C566: 28B24 push radius = k+3 (unlike 2k+3 in 2A1C6).
    std::vector<int> victims = battle_spell_diamond(unit_index, tx, ty, k + 3, 1);
    if (victims.empty()) return 3;
    battle_spell_announce(unit_index, battle_spell_name_);
    if (should_exit_) return 0;
    for (int ti : victims) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        const auto& o = units_[(size_t)ti];
        if (!o.active || o.hp <= 0 || o.sprite_type < 0 || o.sprite_type >= 0x12C) continue;
        if ((int)o.combat_value_43 >= 0x32) continue;
        if (combat_rand(8 >> k) != 0) continue;
        play_unit_death(ti, unit_index);                         // sub_1DB1D
        if (should_exit_) return 0;
    }
    if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
    return 0;
}

// sub_29514 (row 0/1/2 d2): Fire Field / Ice Fog / E.Field.
//   ASM: dx = k (level), bx = row (0/1/2 - sub_2B75A/2B848/2B919 pass 0/1/2).
//   bx == 1 (Ice Fog) -> 22AF9(icewind/icestor/icetypho) with 1000 damage.
//   Otherwise -> dx branch k0 = 20620(f_field) / k1 = 28C8A / k2 = 20DDF(energy_f) with
//   2000 damage. Collection: k == 1 uses the 9x3 slab of sub_28C8A (1A452 count=9 rows=3),
//   otherwise 28B24 -> 28A97 diamond of radius k+3. Range = 184A4 + (k == 1 ? 5 : k+5).
//   28728(edx=base, ebx=0x1E, ecx=bx) and 29437(edx=bx==2?4:bx+1, ebx=0x32, ecx=k).
int CombatII::battle_spell_field(int unit_index, int g, int k, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    auto& u = units_[(size_t)unit_index];
    const int bx = g;                                   // SCE row = ebx of sub_2B75A/848/919
    const int sel = (bx == 2) ? 4 : bx + 1;             // loc_296D2/loc_296FE
    // ASM 29552/295DA: collection radius = k+3, reach = sub_28465(=184A4[5Bh][36]) + (k+5).
    // ASM 2952C: k == 1 goes through sub_28C8A(ecx=5), so only +5 is added
    //   (28C8A var_10 = 5 -> 28465).
    const int range = battle_lookup_184a4(u.combat_type_5b, 0x36) + (k == 1 ? 5 : k + 5);
    if (std::abs(tx - u.x) + std::abs(ty - u.y) > range) {
        if (battle_trace_) fprintf(stderr, "COMBAT: field epicenter rejected unit=%d tile=(%d,%d)\n", u.id, tx, ty);
        return 3;
    }
    std::vector<int> victims;
    auto add_victim = [&](int vx, int vy) {
        if (vx < 0 || vy < 0 || vx >= (int)map_w_ || vy >= (int)map_h_) return;
        const int oi = battle_unit_index_at(vx, vy);
        if (oi < 0 || (size_t)oi >= units_.size()) return;
        const auto& other = units_[(size_t)oi];
        if (!battle_spell_can_target(other)) return;              // sub_28596 gate
        if (!battle_armies_hostile(battle_effective_army(u), battle_effective_army(other),
                                   battle_hostility_inverted_)) return;
        for (int v : victims) if (v == oi) return;
        victims.push_back(oi);
    };
    if (k == 1) {
        // sub_29514 local 2953E -> sub_28C8A: build a 3-row x 9-column slab around the
        //   clicked tile. 28C8A starts at +/-4 columns and row 0, while sub_1A452(push 3,
        //   ecx=9) steps back (row length - 1) per row, giving an axis-aligned 9x3 slab.
        //   Rows advance along the caster facing, the first row being the clicked row.
        int fx = 0, fy = 0;
        switch (u.facing & 3) {
            case 0: fy = 1; break;      // south
            case 1: fx = -1; break;     // west
            case 2: fy = -1; break;     // north
            default: fx = 1; break;     // east
        }
        const int px = -fy, py = fx;
        for (int row = 0; row < 3; ++row) {
            const int cx = tx + fx * row, cy = ty + fy * row;
            for (int w = -4; w <= 4; ++w) add_victim(cx + px * w, cy + py * w);
        }
        if (victims.empty()) {
            if (battle_trace_) fprintf(stderr, "COMBAT: field slab empty unit=%d tile=(%d,%d)\n", u.id, tx, ty);
            return 3;                                        // 28C8A -> eax=-1
        }
    } else {
        // 28B24 -> 28A97: Manhattan diamond of radius k+3 around the clicked tile.
        const int rad = k + 3;
        for (int dy = -rad; dy <= rad; ++dy) {
            const int half = rad - (dy < 0 ? -dy : dy);
            for (int dx = -half; dx <= half; ++dx) add_victim(tx + dx, ty + dy);
        }
    }
    battle_spell_announce(unit_index, battle_spell_name_);        // 29514 -> sub_28889
    // loc_2963E(7D0h) / loc_296A1(3E8h) - only Ice Fog (bx == 1) uses 1000.
    battle_apply_spell_hit(unit_index, victims, bx == 1 ? 1000 : 2000, 0x1E, bx);
    battle_spell_status_29437(unit_index, victims, sel, 0x32, k);
    if (!should_exit_ && (size_t)unit_index < units_.size())
        reset_unit_pose(units_[(size_t)unit_index]);              // sub_12124
    if (battle_trace_) fprintf(stderr, "COMBAT: field done unit=%d victims=%zu\n",
                                units_[(size_t)unit_index].id, victims.size());
    return 0;
}

// sub_29722 (row 0/1/2 d3): Fire Gazer / Diamond Fist / E.Wave. Frontal cone of the
//   caster (k 0/1/2). dx = k (level), bx = row (0/1/2). Damage =
//   dword_28352 word table {15000,10000,12000}[bx] (`mov ecx,[esp+1Eh]>>16` = bx,
//   `mov edx,[esp+ecx*2+6]>>16`).
//   28728(ebx=0x1E, ecx=bx) and 29437(edx=bx==2?4:bx+1, ebx=0x1E, ecx=k).
int CombatII::battle_spell_wave(int unit_index, int g, int k, int tx, int ty,
                                bool play_visual, bool* out_had_targets) {
    (void)tx; (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    auto& u = units_[(size_t)unit_index];
    const int bx = g;
    const int sel = (bx == 2) ? 4 : bx + 1;             // loc_29968/loc_299AE
    std::vector<int> victims;
    {
        int fx = 0, fy = 0;
        switch (u.facing & 3) {
            case 0: fy = 1; break;
            case 1: fx = -1; break;
            case 2: fy = -1; break;
            default: fx = 1; break;
        }
        // sub_1A452(+1A552) covers k=0/2 and sub_1AF22(+1B00D) covers k=1.
        //   k=0: push 5, ecx 1  -> 5 rows x width 1 (straight line)
        //   k=2: push 10, ecx 3 -> 10 rows x width 3
        //   k=1: push 4, ecx 3  -> 4 rows; 1B00D adds 2 to the width until one row is left,
        //        then drops 2 -> widths 3,5,7,5 (half widths 1,2,3,2). Row start also shifts
        //        by 1 with each width change to keep the cone centred.
        static constexpr int kWaveHw1[4] = { 1, 2, 3, 2 };
        const int len = (k == 0) ? 5 : (k == 1 ? 4 : 10);
        for (int s = 1; s <= len; ++s) {
            const int cx = (int)(int8_t)u.x + fx * s, cy = (int)(int8_t)u.y + fy * s;
            const int px = -fy, py = fx;
            const int hw = (k == 1) ? kWaveHw1[s - 1] : (k == 2 ? 1 : 0);
            for (int w = -hw; w <= hw; ++w) {
                const int vx = cx + px * w, vy = cy + py * w;
                if (vx < 0 || vy < 0 || vx >= (int)map_w_ || vy >= (int)map_h_) continue;
                const int oi = battle_unit_index_at(vx, vy);
                if (oi < 0 || (size_t)oi >= units_.size()) continue;
                const auto& other = units_[(size_t)oi];
                // 29722 loc_29844: after 11DB2/1201D, append when sub_28596 passes
                // (active + [15h]&80h + [5Ch]<7 + no duplicate).
                if (!battle_spell_can_target(other)) continue;
                if (!battle_armies_hostile(battle_effective_army(u), battle_effective_army(other),
                                           battle_hostility_inverted_)) continue;
                bool dup = false;
                for (int v : victims) if (v == oi) dup = true;
                if (!dup) victims.push_back(oi);
            }
        }
    }
    if (out_had_targets) *out_had_targets = !victims.empty();
    if (victims.empty()) return 4;                                // 29722 loc_2976F -> -1
    battle_spell_announce(unit_index, battle_spell_name_);       // 29722 -> sub_28889
    if (should_exit_) return 0;
    if (play_visual) {
        // AI F13D8 has no separate battle_magic_fx call: run the original row-3 handler's
        // 4F460 + 21114/2132B/215FE limb here, after the announcement and before damage.
        const int r_code = g * 21 + 9 + k; // wave row is d=3 (q1 3/10/17)
        battle_ssp_play(battle_spell_ssp(r_code));
        battle_fx_driver(r_code, unit_index, (int)(int8_t)u.x, (int)(int8_t)u.y);
        if (should_exit_) return 0;
    }
    static constexpr int kWaveDamage[3] = { 15000, 10000, 12000 };
    battle_apply_spell_hit(unit_index, victims, kWaveDamage[bx < 0 ? 0 : (bx > 2 ? 2 : bx)], 0x1E, bx);
    if (should_exit_) return 0;
    if (play_visual) battle_spell_pace_287af();                  // 287AF before 29437
    battle_spell_status_29437(unit_index, victims, sel, 0x1E, k);
    if (!should_exit_ && (size_t)unit_index < units_.size())
        reset_unit_pose(units_[(size_t)unit_index]);
    if (battle_trace_) fprintf(stderr, "COMBAT: wave done unit=%d victims=%zu\n",
                                units_[(size_t)unit_index].id, victims.size());
    return 0;
}

// ==== Spell effects (sub_4F460 SSP / sub_1FEA4 NSP / sub_287AF / sub_5EE51) ====
// The original handler (1) loads and plays MAGICSPR\<spell>.ssp into the sound slot via
// 4F460, (2) has a driver (2016A/203CC/22172/.../inside 2A1C6) put the <spell>.nsp frames
// into the table with 1FEA4 and drop the timer period to 3 so each frame draws with
// (454B3+5E9E8) and waits with 499E4, then (3) runs 287AF for 24 frames of [64h] hit poses,
// stands the victims up with 12124, restores the tick period with 499DA(0x24) and clears
// the table with 1FFCD.
// The port keeps (1) and (2) as-is and folds (3) into a 24-frame loop. The 37 drivers are
// dispatched by the battle_fx_driver table (one file each); only a missing or failed file
// falls back to the shared player (caster -> target interpolation, evenly split frames).

// Loads one NSP file into the unk_F0DAA table (frame array).
//   File: 0x42 = u16 frame count, entries start at 0x80. A 32-byte entry header is
//   {u32 size, u16 w, u16 h, u32 unused, i16 x, i16 y, 16 bytes unused} followed by
//   size bytes (size = 4 + w*h; the payload is a [w][h] header plus a flat w*h pixel
//   block, matching the sub_5EE51 input).
bool CombatII::battle_nsp_load(const char* name) {
    if (!name || !*name) return false;
    if (magic_frames_name_ == name && !magic_frames_.empty()) return true;
    battle_nsp_release();
    const std::string path = game_path((std::string("MAGICSPR/") + name).c_str());
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: cannot open '%s'\n", path.c_str());
        return false;
    }
    uint16_t count = 0;
    if (fseek(fp, 0x42, SEEK_SET) != 0 || fread(&count, 2, 1, fp) != 1 || count == 0 || count > 512) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0x80, SEEK_SET) != 0) { fclose(fp); return false; }
    magic_frames_.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t hdr[32];
        if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) break;
        const uint32_t size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                              ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        const uint16_t w = (uint16_t)(hdr[4] | (hdr[5] << 8));
        const uint16_t h = (uint16_t)(hdr[6] | (hdr[7] << 8));
        const int16_t xo = (int16_t)(hdr[12] | (hdr[13] << 8));
        const int16_t yo = (int16_t)(hdr[14] | (hdr[15] << 8));
        if (size == 0 || size > 0x100000 || w == 0 || h == 0 || (int)w * (int)h > 250000) break;
        std::vector<uint8_t> raw(size);
        if (fread(raw.data(), 1, size, fp) != size) break;
        MagicFxFrame fr;
        fr.w = w; fr.h = h; fr.xo = xo; fr.yo = yo; fr.xo0 = xo; fr.yo0 = yo;
        if (size >= 4 && size >= 4u + (uint32_t)w * h) {
            fr.flat.assign(raw.begin() + 4, raw.begin() + 4 + (size_t)w * h); // skip the [w][h] header
        } else {
            fr.flat.assign(raw.begin(), raw.end());
            fr.flat.resize((size_t)w * h, 0xFF);
        }
        magic_frames_.push_back(std::move(fr));
    }
    fclose(fp);
    magic_frames_name_ = name;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: nsp '%s' frames=%zu/%u\n", name, magic_frames_.size(), (unsigned)count);
    return !magic_frames_.empty();
}

// sub_1FFCD: clears the magic sprite table (unk_F0DAA, word_F0DA8).
void CombatII::battle_nsp_release() {
    magic_frames_.clear();
    magic_frames_name_.clear();
    magic_fx_frame_ = -1;
}

// sub_4F460(name, slot): loads MAGICSPR\<name> into the sound slot and plays it. The
//   original clears slots 2..7 with 5CD0D, loads the file with 5CA62(slot+1, path) and the
//   driver stops it with 4F4CD(0x32). Audio is single-channel here (play_voc stops before
//   playing), which covers that stop. 35 of the files carry a VOC header and the rest are
//   raw PCM behind a 0x40 header, so Audio::play_voc's three branches (VOC/WAV/raw) apply.
void CombatII::battle_ssp_play(const char* name) {
    if (!name || !*name) return;
    const std::string path = game_path((std::string("MAGICSPR/") + name).c_str());
    if (battle_trace_) fprintf(stderr, "COMBAT: ssp '%s'\n", name);
    Audio::play_voc(path);
}

// SSP name loaded by the handler through 4F460 (q1 = g*7+d; k variants are picked here).
const char* CombatII::battle_spell_ssp(int r_code) const {
    if (r_code < 0 || r_code >= 147) return "";
    const int q1 = r_code / 3, k = r_code % 3;
    switch (q1) {
        case 0:  return (k == 2) ? "f-blast.ssp" : "f-arrow.ssp";   // Fire Arrow/Ball/Blast
        case 1:  case 8:  case 15: return "enchant.ssp";            // X.Blade
        case 2:  case 16: return "f-field.ssp";                      // Field
        case 3:  return "f-wave.ssp";                                // Fire Gazer
        case 4:  case 11: case 18:                                   // Salamanda/Marid/Djinni
        case 35: case 36: case 37: case 38: case 39: case 40: case 41:
        case 45: return "regen.ssp";                                 // summon / Soul Recall
        case 5:  return "fire.ssp";                                  // Explosion series
        case 6:  return "f-meteo2.ssp";                              // Meteor series
        case 7:  return (k >= 2) ? "i-piece.ssp" : "i-shoot.ssp";   // Ice Missile series
        case 9:  return "m-wind.ssp";                                // Ice Fog
        case 10: return "i-fist.ssp";                                // Diamond Fist
        case 12: return "f-blast.ssp";                               // Leopard
        case 13: return "f-meteo2.ssp";                              // Star Tearer
        case 14: return (k == 0) ? "bolt1.ssp" : (k == 1 ? "bolt2.ssp" : "bolt3.ssp");
        case 17: return "bolt2.ssp";                                 // E.Wave
        case 19: return "thunder1.ssp";                              // P.Lighting
        case 20: return "f-meteo2.ssp";                              // Mega/Giga/Pow Thunder
        case 21: case 22: case 25: case 26: return "heal.ssp";       // Heal/Wide Heal/Heal Wind/Revible
        case 23: return "cure.ssp";                                  // Cure
        case 24: return "assist-d.ssp";                              // Turn Undead
        case 27: return "sun1.ssp";                                  // Sun Light
        case 28: case 29: case 30: case 31: case 33: return "assist-u.ssp";  // Power/Speed/M.Armor/Bless/Barrier
        case 32: case 34: case 43: case 46: return "assist-d.ssp";   // Silence/Curse/Melt/DNA Change
        case 42: return "poison.ssp";                                // Confuse
        case 44: return "death1.ssp";                                // Death
        case 47: return "b-hole.ssp";                                // Black Hole
        case 48: return (k == 1) ? "f-meteo2.ssp" : "atomic.ssp";   // Dark Zone/Atomic/Amageddon
        default: return "";
    }
}

// NSP loaded by the drivers through 1FEA4. Only confirmed entries are listed
// (2016A/203CC/22172/22486/23882/5666E/57766/57AC4/57F18/241F7/24466); an empty string
// means sound and hit poses only.
const char* CombatII::battle_spell_nsp(int r_code) const {
    if (r_code < 0 || r_code >= 147) return "";
    const int q1 = r_code / 3, k = r_code % 3;
    switch (q1) {
        case 0:  return (k == 2) ? "f_blast.nsp" : "f_arrow.nsp";   // 2016A / 203CC
        case 5:  return (k == 0) ? "explosio.nsp" : (k == 1 ? "firestor.nsp" : "hellfire.nsp");
        case 7:  return (k == 2) ? "diamondd.nsp" : "icemiss.nsp";  // 22172 / 22486
        case 12: return (k == 0) ? "leopard.nsp" : (k == 1 ? "icestorm.nsp" : "blizzard.nsp");
        case 14: return "lbolt.nsp";                                 // 23882
        case 19: return "p_light.nsp";                               // 241F7/24466
        default: return "";
    }
}

// sub_287AF: hit pacing after damage and status are applied.
//   Drops the period to 3 (1033B(3,1)) and runs 24 times
//   { draw each unit with 45F29/45CCB -> 454B3+5E9E8 -> 499E4 }, then stands the collected
//   units up with 12124 and restores the period with 499DA(0x24).
void CombatII::battle_spell_pace_287af() {
    game_timer_set(3);                                    // sub_1033B(3, 1)
    for (int i = 1; i <= 0x18 && !should_exit_; ++i) {
        present_cutscene_frame();                         // sub_454B3 + sub_5E9E8
        wait_game_tick();                                 // sub_499E4
    }
    // The 12124 loop of 287AF (restore collected poses) already happens in each handler
    // through the final reset_unit_pose call.
    game_timer_set(0x24);                                 // sub_499DA(0x24)
    wait_game_tick();                                     // 499DA falls through to 499E4
}

// sub_2016A: f_arrow.nsp projectile (Fire Arrow / Fire Ball, q1 0 d0 k0/k1).
//   eax = caster, edx = sub-frame (k), the target tile is a separate argument. Loads
//   "f_arrow.nsp" with 1FEA4, plays 200DE(caster,3), then each frame moves the screen
//   position "remaining distance / 5" of the way from the caster tile centre (+12,+2) to
//   the target unit centre (135EB). Frame layout (24):
//     0..3 = north, 4..7 = south, 8..11 = west, 12..15 = east, 16..23 = impact.
//   Sub-frame = k%4 (one NSP, per-step flight art). It stops at |dy| <= 6 for vertical
//   travel or |dx| <= 6 for horizontal travel (arrival included), then after 1033B(9) and
//   4F4CD(0x33) draws the impact frame over the target and frees the table with 1FFCD.
bool CombatII::battle_fx_projectile_2016a(int unit_index, int tx, int ty, int subframe) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (!battle_nsp_load("f_arrow.nsp") || magic_frames_.empty()) return false;
    int sx = 0, sy = 0;
    unit_screen_center(units_[(size_t)unit_index], sx, sy);         // 10971/10988
    sx += 12; sy += 2;                                             // +0Ch / +2
    int ex = sx, ey = sy;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);             // sub_135EB target centre
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 2;
    }
    // Direction block: port facing 0=south 1=west 2=north 3=east -> first frame 4/8/0/12
    // (off_2015A order).
    static const int kDirBlock[4] = {4, 8, 0, 12};
    int dir = 0;
    {
        const int dx = ex - sx, dy = ey - sy;
        if (std::abs(dx) > std::abs(dy)) dir = (dx > 0) ? 3 : 1;
        else dir = (dy > 0) ? 0 : 2;
    }
    int frame = kDirBlock[dir & 3] + (subframe & 3);
    if (frame >= (int)magic_frames_.size()) frame = (int)magic_frames_.size() - 1;
    magic_fx_frame_ = frame;
    game_timer_set(3);                                             // sub_1033B(3)
    int x = sx, y = sy;
    for (int guard = 0; guard < 0x100 && !should_exit_; ++guard) {
        const int nx = x + (ex - x) / 5;                            // remaining distance / 5
        const int ny = y + (ey - y) / 5;
        magic_fx_x_ = nx; magic_fx_y_ = ny;
        present_cutscene_frame();                                   // sub_454B3 + sub_5E9E8
        const bool reached = (nx == ex && ny == ey);
        const bool near = ((dir == 0 || dir == 2) ? std::abs(ny - ey) : std::abs(nx - ex)) <= 6;
        if (reached || near) { x = nx; y = ny; break; }
        x = nx; y = ny;
        wait_game_tick();                                           // sub_499E4
    }
    if (should_exit_) { battle_nsp_release(); return true; }
    game_timer_set(9);                                             // sub_1033B(9)
    for (int i = 16; i < (int)magic_frames_.size() && !should_exit_; ++i) {
        magic_fx_frame_ = i;                                        // impact frame
        magic_fx_x_ = ex; magic_fx_y_ = ey;
        present_cutscene_frame();
        wait_game_tick();
    }
    magic_fx_frame_ = -1;
    battle_nsp_release();                                           // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2016A f_arrow unit=%d dir=%d sub=%d target=(%d,%d)\n",
                units_[(size_t)unit_index].id, dir, subframe, ex, ey);
    return true;
}

// sub_203CC: f_blast.nsp screen-shake effect (Fire Blast, q1 0 d0 k2).
//   Centres the camera on the caster (1124A), picks the target centre, then runs 50
//   frames at timer 5: the low word of [BA838] (camera fine y) alternates odd = 0 /
//   even = -2 to shake the screen, the sound is cut with 4F4CD(0x32) every 10 frames,
//   and the frame index (i%10)/2 cycles 0,0,1,1,2,2,3,3,4,4 (F_BLAST.NSP is 5 frames
//   per 10-frame cycle). The final frame (49, odd) restores fine y to 0, so the camera
//   returns to its original spot.
bool CombatII::battle_fx_blast_203cc(int unit_index, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (!battle_nsp_load("f_blast.nsp") || magic_frames_.empty()) return false;
    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                     (int)(int8_t)units_[(size_t)unit_index].y);     // sub_1124A
    int ex = 0, ey = 0;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);             // sub_135EB
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    } else {
        unit_screen_center(units_[(size_t)unit_index], ex, ey);
    }
    ey -= 0x10;                                                     // sub edx, 10h
    game_timer_set(5);                                              // sub_1033B(5)
    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 0x32 && !should_exit_; ++i) {
        cam_fine_y_ = (int16_t)((i & 1) ? 0 : -2);                  // low word of dword_BA838
        int f = (i % 10) / 2;                                       // `idiv 10; sar edx,1` -> 5 frames per 10
        if (f >= nframes) f = nframes - 1;
        // 203CC builds dword_12764C with 611AA(table, 5, 0x40) and draws with 5F305: x is the
        // target centre unchanged, y adds only the frame's [6] (yo).
        battle_fx_blit_tinted(f, ex, ey + magic_frames_[(size_t)f].yo, 0x40);
        present_cutscene_frame();                                   // sub_454B3 + sub_5E9E8
        wait_game_tick();
    }
    cam_fine_y_ = 0;
    magic_fx_frame_ = -1;
    battle_nsp_release();                                           // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 203CC f_blast unit=%d target=(%d,%d)\n",
                units_[(size_t)unit_index].id, ex, ey);
    return true;
}

// R_code -> per-driver dispatch table. Each handler calls these effect routines by level
//   k (0..2); r_code = 3*q1 + k and the names match kSpellNames in battle_tables.h.
//   Call sites in the original (ASM):
//     28ED6 (x = row 0/1/2) - row0 k0/k1 -> 2016A, k2 -> 203CC; row1 k0,k1 -> 22172(ecx=k),
//       k2 -> 22486; row2 all -> 23882. (kDirBlock and the frame layout live in the 2016A port)
//     29273 (bit 1/2/4 = fire/ice/thunder) - fire: k2 -> 5637D, else 204E8(ebx=k+8);
//       ice: k0 -> 2262C(ebx=5), k1 -> 22767, k2 -> 229CC; thunder: k2 -> 2262C(ebx=7),
//       else 2262C(ebx=6).
//     2A1C6 (kind di) - kind0 Explosion/Fire Storm/Hell Fire -> 5666E/56896/56BD4,
//       kind1 Leopard/Ice Storm/Blizzard -> 57766/57AC4/57F18,
//       kind2 P.Lighting/T.Storm/G.Thunder -> 241F7(edx=0)/241F7(edx=1)/24466,
//       kind3 Turn Undead -> 59675 on each listed unit, kind4 Barrier -> 2690B,
//       kind5 Dark Zone/Atomic/Amageddon -> 276B9/2799A/27C5A.
//     2B9C8 Heal -> 2476E(k0)/25047(k2); 2BA8E Wide Heal -> 25254/253DC/25591.
//     2B391 (bit) - 8 Power UP -> 261FC, 10 Speed UP -> 26391, 20 Magic Armor -> 26584,
//       40 Bless -> 26672, 800 Silence/Steal MP -> 26810, 2000 Curse -> 26B1F.
//     2C00E Melt Item -> 204E8(ebx=0Bh); 2C28F Death -> 26C9A; 2C36A Soul Recall -> 272AD;
//     2C468 DNA Change -> 275A4; 2C566 Black Hole -> 26E1F.
//   A driver that returns 0 (missing file and so on) makes the caller fall back to the
//   shared player.
//
// sub_2A75A's kind branch (off_2A746) runs 4F460 + the driver inside the routine itself.
// The port separates gameplay handlers from presentation, so this function runs those
// five arms instead:
//   kind 0 Meteor      : k<=1 -> 21E16(meteo)   / k==2 -> 570A1(promi)
//   kind 1 Star Tearer : k<=1 -> 23074(i-star)  / k==2 -> 58619(moon)
//   kind 2 Mega Thunder: 58C87 / 58E95 / 592B0
//   kind 3 Heal Wind   : 25DFE(k)
//   kind 4 Sun Light   : k==0 -> 5A267(sunl)    / else -> 5A566(sunl2)
// This function also plays the SSP at the original 4F460 spot (battle_magic_fx skips the
// SSP for these codes).
bool CombatII::battle_item_2a75a_fx(int unit_index, int kind, int k, int tx, int ty) {
    const int lv = (k < 0) ? 0 : (k > 2 ? 2 : k);
    switch (kind) {
        case 0:
            if (lv <= 1) {
                battle_ssp_play("f-meteo1.ssp");                        // 4F460(32h)
                battle_ssp_play("f-meteo2.ssp");                        // 4F460(33h)
                return battle_fx_21e16(unit_index, tx, ty, lv) != 0;    // sub_21E16
            }
            battle_ssp_play("promi-1.ssp");
            battle_ssp_play("promi-2.ssp");
            return battle_fx_570a1(unit_index, tx, ty, lv) != 0;        // sub_570A1
        case 1:
            if (lv <= 1) {
                battle_ssp_play("cure.ssp");
                battle_ssp_play("i-star.ssp");
                return battle_fx_23074(unit_index, tx, ty, lv) != 0;    // sub_23074
            }
            battle_ssp_play("regen.ssp");
            battle_ssp_play("i-fist.ssp");
            return battle_fx_58619(unit_index, tx, ty, lv) != 0;        // sub_58619
        case 2:
            battle_ssp_play("thunder3.ssp");                            // 4F460(32h)
            if (lv == 0) {
                battle_ssp_play("bolt1.ssp");                           // 4F460(33h)
                return battle_fx_58c87(unit_index, tx, ty, lv) != 0;    // sub_58C87
            }
            if (lv == 1) {
                battle_ssp_play("elect.ssp");                           // 4F460(34h)
                return battle_fx_58e95(unit_index, tx, ty, lv) != 0;    // sub_58E95
            }
            battle_ssp_play("thunder1.ssp");
            return battle_fx_592b0(unit_index, tx, ty, lv) != 0;        // sub_592B0
        case 3:
            battle_ssp_play("heal.ssp");                                // 2BC05: 4F460(32h)
            battle_ssp_play("cure.ssp");                                // 2BC05: 4F460(33h)
            return battle_fx_25dfe(unit_index, lv) != 0;                // sub_25DFE
        case 4:
            battle_ssp_play("sun1.ssp");                                // 2BDF9: 4F460(32h)
            battle_ssp_play("sun2.ssp");                                // 2BDF9: 4F460(33h)
            battle_ssp_play("sun.ssp");                                 // 2BDF9: 4F460(34h)
            if (lv == 0) return battle_fx_5a267(unit_index, tx, ty, lv) != 0;  // sub_5A267
            return battle_fx_5a566(unit_index, tx, ty, lv) != 0;               // sub_5A566
        default: return false;
    }
}

bool CombatII::battle_fx_driver(int r_code, int unit_index, int tx, int ty) {
    const int k = r_code % 3;
    switch (r_code) {
        // ---- Fire ----
        // ASM 34421/34444/34468: all three sites `xor ecx,ecx` -> sub-frame fixed at 0
        // (not the level k).
        case 0: case 1: return battle_fx_projectile_2016a(unit_index, tx, ty, 0);   // sub_2016A
        case 2:         return battle_fx_blast_203cc(unit_index, tx, ty);           // sub_203CC
        case 3: case 4: return battle_fx_204e8(unit_index, tx, ty, k) != 0;         // sub_204E8 (ebx=k+8)
        case 5:         return battle_fx_5637d(unit_index, tx, ty, k) != 0;         // sub_5637D
        // ---- Field (q1=2/9/16, sub_29514) ----
        case 6:         return battle_fx_20620(unit_index, tx, ty, 0) != 0;          // 29514 k0
        case 7:         return battle_fx_2095a(unit_index, tx, ty, 0) != 0;          // 29514 k1 (loc_29652)
        case 8:         return battle_fx_20ddf(unit_index, tx, ty, 0) != 0;          // 29514 k2
        case 27: case 28: case 29:                                                   // Ice Fog(bx=1): 22AF9
                        return battle_fx_22af9(unit_index, tx, ty, k) != 0;
        case 48:        return battle_fx_20620(unit_index, tx, ty, 2) != 0;          // E.Field(bx=2) 20620
        case 49:        return battle_fx_2095a(unit_index, tx, ty, 2) != 0;          // E.Field k1 (loc_29652)
        case 50:        return battle_fx_20ddf(unit_index, tx, ty, 2) != 0;          // E.Field 20DDF
        // ---- Wave (q1=3/10/17, sub_29722) ----
        case 9:         return battle_fx_21114(unit_index, tx, ty, 0) != 0;          // Fire Gazer k0
        case 10:        return battle_fx_2132b(unit_index, tx, ty, 0) != 0;
        case 11:        return battle_fx_215fe(unit_index, tx, ty, 0) != 0;
        case 30:        return battle_fx_21114(unit_index, tx, ty, 1) != 0;          // Diamond Fist(bx=1)
        case 31:        return battle_fx_2132b(unit_index, tx, ty, 1) != 0;
        case 32:        return battle_fx_215fe(unit_index, tx, ty, 1) != 0;
        // E.Wave (bx=2) is the thunder branch of 29722+1C6: use the thundwav family, not boom.
        case 51:        return battle_fx_23aba(unit_index, tx, ty, 0) != 0;          // loc_2994A (k0)
        case 52:        return battle_fx_23c36(unit_index, tx, ty, 1) != 0;          // loc_29958 (k1)
        case 53:        return battle_fx_23f3a(unit_index, tx, ty, 2) != 0;          // loc_29961 (k2)
        case 15:        return battle_fx_5666e(unit_index, tx, ty, k) != 0;
        case 16:        return battle_fx_56896(unit_index, tx, ty, k) != 0;
        case 17:        return battle_fx_56bd4(unit_index, tx, ty, k) != 0;
        // ---- Ice ----
        case 21: case 22: return battle_fx_22172(unit_index, tx, ty, k) != 0;       // ecx=0 / ecx=1
        case 23:          return battle_fx_22486(unit_index, tx, ty, 0) != 0;
        case 24:          return battle_fx_2262c(unit_index, tx, ty, 0) != 0;       // ebx=5
        case 25:          return battle_fx_22767(unit_index, tx, ty, 0) != 0;
        case 26:          return battle_fx_229cc(unit_index, tx, ty, 0) != 0;
        case 36:          return battle_fx_57766(unit_index, tx, ty, k) != 0;
        case 37:          return battle_fx_57ac4(unit_index, tx, ty, k) != 0;
        case 38:          return battle_fx_57f18(unit_index, tx, ty, k) != 0;
        // ---- Thunder (blade uses bit 4 = ebx 6/7) ----
        case 42: case 43: case 44: return battle_fx_23882(unit_index, tx, ty, k) != 0;
        case 45: case 46: return battle_fx_2262c(unit_index, tx, ty, 1) != 0;       // ebx=6
        case 47:          return battle_fx_2262c(unit_index, tx, ty, 2) != 0;       // ebx=7
        case 57: case 58: return battle_fx_241f7(unit_index, tx, ty, k) != 0;       // edx=0 / edx=1
        case 59:          return battle_fx_24466(unit_index, tx, ty, 0) != 0;
        // ---- Heal ----
        case 63:          return battle_fx_2476e(unit_index, tx, ty, k) != 0;
        case 65:          return battle_fx_25047(unit_index, tx, ty, k) != 0;
        case 66:          return battle_fx_25254(unit_index, tx, ty, k) != 0;
        case 67:          return battle_fx_253dc(unit_index, tx, ty, k) != 0;
        case 68:          return battle_fx_25591(unit_index, tx, ty, k) != 0;
        case 72: case 73: case 74: {                                    // Turn Undead: each listed unit
            const int ti = battle_unit_index_at(tx, ty);
            return ti >= 0 && (size_t)ti < units_.size() && battle_fx_59675(ti) != 0;
        }
        // ---- Buff / status (2B391) ----
        case 84: case 85: case 86: return battle_fx_261fc(unit_index, tx, ty, k) != 0;   // Power UP
        case 87: case 88: case 89: return battle_fx_26391(unit_index, tx, ty, k) != 0;   // Speed UP
        case 90: case 91: case 92: return battle_fx_26584(unit_index, tx, ty, k) != 0;   // Magic Armor
        case 93: case 94: case 95: return battle_fx_26672(unit_index, tx, ty, k) != 0;   // Bless
        case 96: case 97: case 98: return battle_fx_26810(unit_index, tx, ty, k) != 0;   // Silence / Steal MP
        case 99: case 100: case 101: return battle_fx_2690b(unit_index, tx, ty, k) != 0; // Barrier
        case 102: case 103: case 104: return battle_fx_26b1f(unit_index, tx, ty, k) != 0;// Curse
        // ---- Row 6: item / instant death / summon ----
        case 129: case 130: case 131: return battle_fx_204e8(unit_index, tx, ty, 3) != 0; // Melt Item (ebx=0Bh)
        case 132: case 133: case 134: return battle_fx_26c9a(unit_index, tx, ty, k) != 0; // Death
        case 135: case 136: case 137: return battle_fx_272ad(unit_index, tx, ty, k) != 0; // Soul Recall
        case 138: case 139: case 140: return battle_fx_275a4(unit_index, tx, ty, k) != 0; // DNA Change
        case 141: case 142: case 143: return battle_fx_26e1f(unit_index, tx, ty, k) != 0; // Black Hole
        // ---- sub_2A75A kind 0..4 (off_2A746 branch inside the routine) ----
        case 18: case 19: case 20: return battle_item_2a75a_fx(unit_index, 0, k, tx, ty); // Meteor
        case 39: case 40: case 41: return battle_item_2a75a_fx(unit_index, 1, k, tx, ty); // Star Tearer
        case 60: case 61: case 62: return battle_item_2a75a_fx(unit_index, 2, k, tx, ty); // Mega Thunder
        case 75: case 76: case 77: return battle_item_2a75a_fx(unit_index, 3, k, tx, ty); // Heal Wind
        case 81: case 82: case 83: return battle_item_2a75a_fx(unit_index, 4, k, tx, ty); // Sun Light
        case 144: return battle_fx_276b9(unit_index, tx, ty, k) != 0;               // Dark Zone
        case 145: return battle_fx_2799a(unit_index, tx, ty, k) != 0;               // Atomic
        case 146: return battle_fx_27c5a(unit_index, tx, ty, k) != 0;               // Amageddon
        default: return false;
    }
}

// sub_2C8A4 (spell >= 0x64 of VM 0x08/0x6C): damages EVERY unit inside the 12x12 screen
//   box by (100 - (def + ([15h]&20h ? 20 : 0))) * 10000 / 100 raw HP, friend or foe.
//   ASM 39400..39510: walks the four CC968 lists and, for units inside the camera box
//   [camX,camX+0Ch) x [camY,camY+0Ch), calls 45BFB (damage digit), 45ED2 (knockback) and
//   then `sub [ecx+48h], esi`.
//   effect == 0 plays dragon.ssp/f-meteo2.ssp plus the sub_21BE8(meteo) effect; otherwise
//   sub_2C863 shakes the screen for 0x40 frames with dword_BA838 = rand(5)-2.
void CombatII::battle_spell_2c8a4(int effect) {
    if (effect == 0) {
        battle_ssp_play("dragon.ssp");
        battle_ssp_play("f-meteo2.ssp");
        // sub_21BE8 (b_dragon.nsp): takes no arguments (the call site clears eax/edx/ebx).
        battle_fx_21be8(0, 0, 0, 0);
        if (should_exit_) return;
    } else {
        // sub_2C863: camera-shake variant without dragon.nsp.
        game_timer_set(3);                                   // sub_1033B(3)
        for (int i = 0; i < 0x40 && !should_exit_; ++i) {    // `cmp dx,40h; jl`
            cam_fine_y_ = (int16_t)(combat_rand(5) - 2);     // 499BD(5)-2 -> dword_BA838
            present_cutscene_frame();                        // sub_45559
            wait_game_tick();                                // sub_499E4
        }
        cam_fine_y_ = CAM_FINE_Y_REST;
    }
    const int left = cam_tile_x_, top = cam_tile_y_;
    const int right = left + 12, bottom = top + 12;
    int hit = 0;
    for (size_t i = 0; i < units_.size(); ++i) {
        CombatUnitPlacement& t = units_[i];
        if (!t.active || t.hp <= 0) continue;
        const int tx = (int)(int8_t)t.x, ty = (int)(int8_t)t.y;
        if (tx < left || tx >= right || ty < top || ty >= bottom) continue;
        int def = (int)t.combat_def59;
        if (t.combat_flags_15 & 0x20) def += 20;             // `test dl,20h; add eax,14h`
        const int dmg = (100 - def) * 10000 / 100;           // imul 2710h; idiv 64h
        const int new_raw = battle_hp_raw(t) - dmg;
        const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
        const int delta = new_hp - t.hp;
        t.hp = new_hp;
        t.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
        ++hit;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 2C8A4 meteor unit=%d def=%d dmg=%d hp=%d (%d,%d)\n",
                    t.id, def, dmg, new_hp, tx, ty);
        (void)delta;
    }
    // 287AF (hit pacing) -> 12124 on the list -> 1ED90. The port uses the shared pace.
    if (hit > 0) battle_spell_pace_287af();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2C8A4 effect=%d targets=%d camera=(%d,%d)\n", effect, hit, left, top);
}

// F13B0 driver replacement: play SSP -> NSP frame loop (period 3) -> 287AF pacing ->
// 1FFCD release.
void CombatII::battle_magic_fx(int r_code, int unit_index, int tx, int ty) {
    if (r_code < 0 || r_code >= 147) return;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    // For the sub_2A75A family (kind 0..4, the original calls 4F460 inside the routine)
    // the driver plays the SSP too.
    const int q0 = r_code / 3;
    const bool ssp_in_driver = (q0 == 6 || q0 == 13 || q0 == 20 || q0 == 25 || q0 == 27);
    if (!ssp_in_driver) battle_ssp_play(battle_spell_ssp(r_code));  // sub_4F460
    // Only the hit handlers (28ED6/29514/29722/2A1C6/2A75A/2C00E/2C28F/2C566) call 287AF.
    const int q1 = r_code / 3, k = r_code % 3;
    auto pace = [&]() {
        switch (q1) {
            // Routines that call 287AF: 28ED6(d0)/29514(d2)/29722(d3)/2A1C6(d5)/2A75A(d6).
            case 0: case 2: case 3: case 5: case 6:
            case 7: case 9: case 10: case 12: case 13:
            case 14: case 16: case 17: case 19: case 20:
            case 24: case 33: case 48:                    // 2A1C6: Turn Undead / Barrier / Dark Zone
            case 25: case 27:                            // 2A75A: Heal Wind / Sun Light
                battle_spell_pace_287af();                // sub_287AF
                break;
            default: break;
        }
    };
    // Try the individually ported drivers first (2016A, 203CC, 22172 ... 27C5A,
    // 5EE51/5F305) and fall back to the shared player below when they fail
    // (missing NSP file and so on).
    (void)q1; (void)k;
    if (battle_fx_driver(r_code, unit_index, tx, ty)) {
        pace();
        return;
    }
    // Only when the driver could not open its NSP: play the skipped SSP here.
    if (ssp_in_driver) battle_ssp_play(battle_spell_ssp(r_code));
    magic_fx_draws_.clear();                              // draws left behind by a failed driver
    const bool loaded = battle_nsp_load(battle_spell_nsp(r_code));   // sub_1FEA4
    int sx0 = 0, sy0 = 0;
    unit_screen_center(units_[(size_t)unit_index], sx0, sy0);        // sub_10971/10988
    int sx1 = sx0, sy1 = sy0;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], sx1, sy1);            // sub_135EB
    } else if (tile_ok) {
        sx1 = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        sy1 = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    } // with no tile (AI auto target) the effect stays on the caster
    const int total = 0x18;
    if (loaded && !magic_frames_.empty()) {
        game_timer_set(3);                                // 1033B(3) of the drivers
        const int nframes = (int)magic_frames_.size();
        for (int i = 1; i <= total && !should_exit_; ++i) {
            const int f = std::min(nframes - 1, (i - 1) * nframes / total);
            magic_fx_frame_ = f;
            magic_fx_x_ = sx0 + (sx1 - sx0) * i / total + magic_frames_[(size_t)f].xo;
            magic_fx_y_ = sy0 + (sy1 - sy0) * i / total + magic_frames_[(size_t)f].yo;
            present_cutscene_frame();
            wait_game_tick();
        }
        magic_fx_frame_ = -1;
    }
    pace();
    battle_nsp_release();                                 // sub_1FFCD
}

// ==== Common drawing (used by every driver) ====
// Drivers queue the sprites to draw here and emit one frame with battle_fx_present().
// render_magic_effect_layer draws them in order over map and units (later entries on
// top) and battle_fx_present clears the list after present.
void CombatII::battle_fx_blit(int frame, int x, int y) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    magic_fx_draws_.push_back(MagicFxDraw{frame, x, y, -1, 0, 0});       // sub_5EE51
}

void CombatII::battle_fx_blit_clipped(int frame, int x, int y) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    magic_fx_draws_.push_back(MagicFxDraw{frame, x, y, -1, 0, 0, false, false, true}); // sub_5EFB9
}

void CombatII::battle_fx_blit_tinted(int frame, int x, int y, int tint_pct) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    magic_fx_draws_.push_back(MagicFxDraw{frame, x, y, tint_pct, 0, 0}); // sub_5F305
}

void CombatII::battle_fx_blit_tinted_unclipped(int frame, int x, int y, int tint_pct) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    magic_fx_draws_.push_back(MagicFxDraw{frame, x, y, tint_pct, 0, 0, false, true}); // sub_5F29D
}

// sub_5F093(eax=x, edx=y, ebx=[w][h]+pixels, cl=colour): solid silhouette. Every source pixel
//   that is not 0FFh is replaced by the colour byte, with no palette lookup and no clip. The
//   original walks the 480-stride framebuffer without bounds tests; the port keeps the same
//   linear walk but drops writes outside the buffer.
void CombatII::battle_fx_blit_silhouette(int frame, int x, int y, int color) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    magic_fx_draws_.push_back(MagicFxDraw{frame, x, y, -1, 0, 0, false, false, false, false,
                                          color & 0xFF}); // sub_5F093
}

// sub_5F570 / sub_5F611: draw a frame scaled up or down to the target size (the
// Barrier/Black Hole/Amagedon family changes size over the 0Fh/0Eh timers). A target at
// least as large as the source uses expand (5F570) and a smaller one shrink (5F611); the
// original also calls them separately.
// expand_only covers call sites where the original only calls 5F570 (276B9/2799A/27C5A):
// a smaller target then still goes through 5F570 instead of the shrink routine.
void CombatII::battle_fx_blit_scaled(int frame, int x, int y, int dst_w, int dst_h,
                                     bool expand_only, bool shrink_only) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    if (dst_w <= 0 || dst_h <= 0) return;
    magic_fx_draws_.push_back(MagicFxDraw{frame, x, y, -1, dst_w, dst_h, expand_only, false,
                                          false, shrink_only});
}

void CombatII::battle_fx_present() {
    present_cutscene_frame();                                     // sub_454B3 + sub_5E9E8
    magic_fx_draws_.clear();
}

// sub_5F305(x=eax, y=edx, flat=ebx): same [w][h]+flat input as sub_5EE51, but pixels go
//   through a colour LUT (dword_12764C[source colour]) instead of being copied. sub_611AA
//   builds that LUT once per source colour:
//     effect colour = sub_6098B(source) = RGB(source) * shade / 100  (shade = 100 - ebx)
//     LUT[dest colour] = sub_609F9: nearest palette entry to
//                        RGB(dest)*alpha/100 + effect colour (L1 distance, lowest error
//                        wins, black on failure)
//   611AA/6098B/609F9: the argument is the effect-colour weight (ebx). 6098B multiplies the
//   source colour by it, 609F9 weights the destination by (100 - it) and adds 9 to each
//   channel. Both sums are stored as bytes, so they wrap modulo 256 exactly like `mov [var],
//   al`. The nearest palette entry is chosen by L1 distance, first match wins.
void CombatII::blit_frame_tinted(int x, int y, const MagicFxFrame& fr, int tint_pct, bool clip) {
    if (fr.flat.empty() || fr.w <= 0 || fr.h <= 0) return;
    uint8_t* dst = video_.get_backbuffer();
    if (!dst) return;
    // Drop the cache when the palette changes (it is fixed in battle, but be safe).
    static std::unordered_map<uint32_t, std::vector<uint8_t>> cache;
    static uint32_t cache_pal = 0;
    uint32_t pal_hash = 2166136261u;
    for (int i = 0; i < 256; ++i)
        pal_hash = (pal_hash ^ (uint32_t)(battle_palette_[i][0] | (battle_palette_[i][1] << 8))) * 16777619u;
    if (pal_hash != cache_pal) { cache.clear(); cache_pal = pal_hash; }

    const int arg = std::max(0, std::min(100, tint_pct));   // 611AA ebx
    const int dest_weight = 100 - arg;                      // dword_128558
    const long base = (long)y * SCREEN_W + x;               // sub_5EB00 destination
    const long limit = (long)SCREEN_W * SCREEN_H;
    for (int row = 0; row < fr.h; ++row) {
        for (int col = 0; col < fr.w; ++col) {
            long off;
            if (clip) {
                const int dy = y + row;
                if (dy < 0 || dy >= 0xC7) continue;       // 5F305 clips at C7h
                const int dx = x + col;
                if (dx < 0 || dx >= 0x13F) continue;      // 5F305 clips at 13Fh
                off = (long)dy * SCREEN_W + dx;
            } else {
                // sub_5F29D advances the framebuffer pointer by the stride with no bounds
                // test. Keep the same linear walk but drop writes outside the buffer.
                off = base + (long)row * SCREEN_W + col;
                if (off < 0 || off >= limit) continue;
            }
            const uint8_t px = fr.flat[(size_t)row * (size_t)fr.w + (size_t)col];
            if (px == 0xFF) continue;                               // transparent
            const uint32_t key = ((uint32_t)px << 8) | (uint32_t)arg;
            auto it = cache.find(key);
            if (it == cache.end()) {
                // 6098B: effect colour = palette[px] * arg%.
                const int er = battle_palette_[px][0] * arg / 100;
                const int eg = battle_palette_[px][1] * arg / 100;
                const int eb = battle_palette_[px][2] * arg / 100;
                std::vector<uint8_t> lut(256);
                for (int d = 0; d < 256; ++d) {
                    // 609F9: palette[d] * (100-arg)% + effect + 9, stored through al.
                    const int r = (uint8_t)(battle_palette_[d][0] * dest_weight / 100 + er + 9);
                    const int g = (uint8_t)(battle_palette_[d][1] * dest_weight / 100 + eg + 9);
                    const int b = (uint8_t)(battle_palette_[d][2] * dest_weight / 100 + eb + 9);
                    int best = 0, best_d = 0x7FFFFFFF;
                    for (int c = 0; c < 256; ++c) {
                        const int dr = std::abs(r - (int)battle_palette_[c][0]);
                        const int dg = std::abs(g - (int)battle_palette_[c][1]);
                        const int db = std::abs(b - (int)battle_palette_[c][2]);
                        const int dist = dr + dg + db;              // L1 (12824C sums the 3 deltas)
                        if (dist < best_d) { best_d = dist; best = c; if (!dist) break; }
                    }
                    lut[(size_t)d] = (uint8_t)best;
                }
                it = cache.emplace(key, std::move(lut)).first;
            }
            dst[(size_t)off] = it->second[dst[(size_t)off]];
        }
    }
}

// sub_5EE51: blit a [w][h]+flat block (0xFF transparent) to the back buffer.
void CombatII::render_magic_effect_layer() {
    for (const MagicFxDraw& d : magic_fx_draws_) {
        if (d.frame < 0 || (size_t)d.frame >= magic_frames_.size()) continue;
        const MagicFxFrame& fr = magic_frames_[(size_t)d.frame];
        if (fr.flat.empty()) continue;
        static std::vector<uint8_t> dbuf;
        dbuf.resize(4 + fr.flat.size());
        dbuf[0] = (uint8_t)(fr.w & 0xFF); dbuf[1] = (uint8_t)(fr.w >> 8);
        dbuf[2] = (uint8_t)(fr.h & 0xFF); dbuf[3] = (uint8_t)(fr.h >> 8);
        std::memcpy(dbuf.data() + 4, fr.flat.data(), fr.flat.size());
        if (d.dst_w > 0) {
            // sub_5F570 / sub_5F611: expand when the target is at least the source size;
            // explicit 5F611 call sites force shrink even for a larger target.
            if (d.expand_only || (!d.shrink_only && d.dst_w >= fr.w && d.dst_h >= fr.h))
                blit_scale_expand(d.x, d.y, dbuf.data(), d.dst_w, d.dst_h);
            else
                blit_scale_shrink(d.x, d.y, dbuf.data(), d.dst_w, d.dst_h);
        } else if (d.silhouette >= 0) {
            // sub_5F093: fill every non-transparent pixel with the colour byte.
            const long base = (long)d.y * SCREEN_W + d.x;
            const long limit = (long)SCREEN_W * SCREEN_H;
            for (int row = 0; row < fr.h; ++row) {
                for (int col = 0; col < fr.w; ++col) {
                    if (fr.flat[(size_t)row * (size_t)fr.w + (size_t)col] == 0xFF) continue;
                    const long off = base + (long)row * SCREEN_W + col;
                    if (off < 0 || off >= limit) continue;
                    video_.get_backbuffer()[off] = (uint8_t)d.silhouette;
                }
            }
        } else if (d.clip319) {
            blit_flat_clip319(d.x, d.y, dbuf.data());                       // sub_5EFB9
        } else if (d.tint_pct < 0) {
            blit_flat_transparent(d.x, d.y, dbuf.data());                   // sub_5EE51
        } else {
            blit_frame_tinted(d.x, d.y, fr, d.tint_pct, !d.unclipped);      // sub_5F305 / 5F29D
        }
    }
    magic_fx_draws_.clear();
    if (magic_fx_frame_ < 0 || (size_t)magic_fx_frame_ >= magic_frames_.size()) return;
    const MagicFxFrame& fr = magic_frames_[(size_t)magic_fx_frame_];
    if (fr.flat.empty()) return;
    static std::vector<uint8_t> buf;
    buf.resize(4 + fr.flat.size());
    buf[0] = (uint8_t)(fr.w & 0xFF); buf[1] = (uint8_t)(fr.w >> 8);
    buf[2] = (uint8_t)(fr.h & 0xFF); buf[3] = (uint8_t)(fr.h >> 8);
    std::memcpy(buf.data() + 4, fr.flat.data(), fr.flat.size());
    blit_flat_transparent(magic_fx_x_, magic_fx_y_, buf.data());
}

// sub_4C605 + F13B0: validate R -> pick target -> run -> spend MP -> drain TP.
// Returns 0 = done (back to the ring), 3 = pick another target, 4 = show a failure label
// and stay in selection.
// The handler table follows the order sub_2C643 writes into dword_F13B0[49]:
//   q1 = g*7+d -> {row 0: 2B73F,2B74A,2B75A,2B79C,2B7C0,2B7DC,2B822},
//   {row 1: 2B82D,2B83D,2B848,2B86C,2B890,2B8A6,2B8F3},
//   {row 2: 2B903,2B90E,2B919,2B95B,2B97F,2B995,2B9BD},
//   {row 3: 2B9C8,2BA8E,2BBB7,2BBDB,2BC05,2BC3B,2BDF9},
//   {row 4: 2BE3B,2BE69,2BE90,2BEB7,2BEDE,2BF08,2BF2F},
//   {row 5: 2BF59,2BF6C,2BF82,2BF95,2BFA8,2BFBE,2BFD4},
//   {row 6: 2BFEA,2C00E,2C28F,2C36A,2C468,2C566,2C62C}.
int CombatII::battle_cast_magic(int unit_index, int r_code, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 4;
    auto& u = units_[(size_t)unit_index];
    if (r_code == -1) {                                  // sub_4C605: cmp ebx,-1 -> "Wrong Spell!" (sub_48612)
        battle_unit_label(unit_index, "Wrong Spell!");
        return 4;
    }
    const int g = r_code / 21, q1 = r_code / 3, d = q1 % 7, k = r_code % 3;
    if (g < 0 || g > 6 || d < 0 || d > 6 || k < 0 || k > 2) return 4;
    // dword_74492 == 1 (sub_28422 AI auto target): the caller sub_432A0 handles the gates
    // and the MP/TP cost.
    const bool ai = battle_spell_auto_;
    // 4C605 gates, in order.
    if (!ai && battle_spell_kind(u, g) != k) {
        battle_unit_label(unit_index, "Wrong Spell!");   // sub_4C605 -> sub_48612
        if (battle_trace_) fprintf(stderr, "COMBAT: magic Wrong Spell! unit=%d R=%d\n", u.id, r_code);
        return 4;
    }
    const int thr = battle_spell_thr(u, d);
    if (!ai && (int)u.combat_acc57 + battle_tp() < thr) {
        battle_unit_label(unit_index, "No TP!");         // sub_4C605 -> sub_48612
        if (battle_trace_) fprintf(stderr, "COMBAT: magic No TP! unit=%d R=%d\n", u.id, r_code);
        return 4;
    }
    if (!ai && (int)u.combat_mp_50 < (int)kSpellCost[d]) {
        battle_unit_label(unit_index, "No MP!");         // sub_4C605 -> sub_48612
        if (battle_trace_) fprintf(stderr, "COMBAT: magic No MP! unit=%d R=%d\n", u.id, r_code);
        return 4;
    }
    battle_spell_name_ = kSpellNames[r_code];   // sub_28883: dword_F13AC = off_7423C[R]
    if (battle_trace_) fprintf(stderr, "COMBAT: magic cast unit=%d R=%d %s\n", u.id, r_code, kSpellNames[r_code]);
    int rc = 4;
    // Effect target coordinates (the handler's tile once chosen, otherwise the click = -1)
    const int fx_tx = tx, fx_ty = ty;
    switch (q1) {
        // ---- Row 0: Fire (Fire Arrow/Blade/Field/Gazer/Salamanda/Explosion/Meteor) ----
        case 0: rc = battle_spell_28ed6(unit_index, 0, k, tx, ty); break;
        case 1: rc = battle_spell_29273(unit_index, 0x01, k, tx, ty); break;
        case 2: rc = battle_spell_field(unit_index, 0, k, tx, ty); break;
        case 3: rc = battle_spell_wave(unit_index, 0, k, tx, ty); break;
        case 4: rc = battle_spell_summon(unit_index, k, 7, 0x17A, 0x5A); break;   // Salamanda
        case 5: rc = battle_spell_2a1c6(unit_index, 0, k); break;           // Explosion
        case 6: battle_item_2a75a(unit_index, 0, k); rc = 0; break;         // Meteor
        // ---- Row 1: Ice (Ice Missile/Blade/Fog/Fist/Marid/Leopard/Star Tearer) ----
        case 7:  rc = battle_spell_28ed6(unit_index, 1, k, tx, ty); break;
        case 8:  rc = battle_spell_29273(unit_index, 0x02, k, tx, ty); break;
        case 9:  rc = battle_spell_field(unit_index, 1, k, tx, ty); break;
        case 10: rc = battle_spell_wave(unit_index, 1, k, tx, ty); break;
        case 11: rc = battle_spell_summon(unit_index, k, 8, 0x176, 0x59); break;  // Marid
        case 12: rc = battle_spell_2a1c6(unit_index, 1, k); break;          // Leopard
        case 13: battle_item_2a75a(unit_index, 1, k); rc = 0; break;        // Star Tearer
        // ---- Row 2: Thunder (L.Bolt/T.Blade/E.Field/E.Wave/Djinni/P.Lighting/Mega Thunder) ----
        case 14: rc = battle_spell_28ed6(unit_index, 2, k, tx, ty); break;
        case 15: rc = battle_spell_29273(unit_index, 0x04, k, tx, ty); break;
        case 16: rc = battle_spell_field(unit_index, 2, k, tx, ty); break;
        case 17: rc = battle_spell_wave(unit_index, 2, k, tx, ty); break;
        case 18: rc = battle_spell_summon(unit_index, k, 9, 0x16E, 0x58); break;  // Djinni
        case 19: rc = battle_spell_2a1c6(unit_index, 2, k); break;          // P.Lighting
        case 20: battle_item_2a75a(unit_index, 2, k); rc = 0; break;        // Mega/Giga/Pow Thunder
        // ---- Row 3: Heal family (q1 21..27) ----
        //   dword_F13B0 holds 49 entries (sub_2C643 initialises 7 rows x 7 columns,
        //   q1 = 7g+d). k (= R%3) is a stage variant of the same spell, so one handler
        //   per q1. The name table off_7423C[R] also has 147 (= 49x3) entries, so R >= 147
        //   does not exist.
        case 21: rc = battle_spell_2b9c8(unit_index, k, tx, ty); break;         // 2B9C8 Heal
        case 22: rc = battle_spell_2ba8e(unit_index, k, tx, ty); break;         // 2BA8E Wide Heal
        case 23: rc = battle_spell_2b1d0(unit_index, 0, k, tx, ty); break;      // 2BBB7 Cure -> 2B1D0
        case 24: rc = battle_spell_2a1c6(unit_index, 3, k); break;              // 2BBDB Turn Undead
        case 25: battle_item_2a75a(unit_index, 3, k); rc = 0; break;            // 2BC05 Heal Wind
        case 26: rc = battle_spell_2bc3b(unit_index, k); break;                 // 2BC3B Revible
        case 27: battle_item_2a75a(unit_index, 4, k); rc = 0; break;            // 2BDF9 Sun Light
        // ---- Row 4: buff / debuff (q1 28..34) ----
        case 28: rc = battle_spell_2b391(unit_index, 0x08, k, tx, ty); break;   // 2BE3B Power UP
        case 29: rc = battle_spell_2b391(unit_index, 0x10, k, tx, ty); break;   // 2BE69 Speed UP
        case 30: rc = battle_spell_2b391(unit_index, 0x20, k, tx, ty); break;   // 2BE90 Magic Armor
        case 31: rc = battle_spell_2b391(unit_index, 0x40, k, tx, ty); break;   // 2BEB7 Bless
        case 32: rc = battle_spell_2b391(unit_index, 0x800, k, tx, ty); break;  // 2BEDE Silence / Steal MP
        case 33: rc = battle_spell_2a1c6(unit_index, 4, k); break;              // 2BF08 Barrier
        case 34: rc = battle_spell_2b391(unit_index, 0x2000, k, tx, ty); break; // 2BF2F Curse
        // ---- Row 5: summon (q1 35..41, Bat/Slime/Goblin/Orc/Ogre/Golem/Gargoyle) ----
        case 35: rc = battle_spell_summon(unit_index, k, 0, 0x72, 0x4C); break;  // 2BF59
        case 36: rc = battle_spell_summon(unit_index, k, 1, 0x80, 0x48); break;  // 2BF6C
        case 37: rc = battle_spell_summon(unit_index, k, 2, 0x6D, 0x3F); break;  // 2BF82
        case 38: rc = battle_spell_summon(unit_index, k, 3, 0x6F, 0x41); break;  // 2BF95
        case 39: rc = battle_spell_summon(unit_index, k, 4, 0x177, 0x43); break; // 2BFA8
        case 40: rc = battle_spell_summon(unit_index, k, 5, 0x173, 0x53); break; // 2BFBE
        case 41: rc = battle_spell_summon(unit_index, k, 6, 0x16F, 0x52); break; // 2BFD4
        // ---- Row 6: status / instant death (q1 42..48) ----
        case 42: rc = battle_spell_2b1d0(unit_index, 1, k, tx, ty); break;      // 2BFEA Confuse
        case 43: rc = battle_spell_2c00e(unit_index, k, tx, ty); break;         // 2C00E Melt Item
        case 44: rc = battle_spell_2c28f(unit_index, k, tx, ty); break;         // 2C28F Death
        case 45: rc = battle_spell_2c36a(unit_index, k); break;                 // 2C36A Soul Recall
        case 46: rc = battle_spell_2c468(unit_index, k, tx, ty); break;         // 2C468 DNA Change
        case 47: rc = battle_spell_2c566(unit_index, k, tx, ty); break;         // 2C566 Black Hole
        case 48: rc = battle_spell_2a1c6(unit_index, 5, k); break;              // 2C62C Dark Zone/Atomic/Amageddon
        default:
            // Q1 only ranges 0..48 (all of dword_F13B0[49] is handled above). Defensive only.
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: magic q1=%d out of range unit=%d\n", q1, u.id);
            return 4;
    }
    if (rc != 0) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: magic handler q1=%d rc=%d unit=%d\n", q1, rc, u.id);
        return rc;
    }
    if (should_exit_ || (size_t)unit_index >= units_.size()) return 0;
    // SSP + NSP frames + 287AF hit pacing (the original runs these inside the handler).
    battle_magic_fx(r_code, unit_index, fx_tx, fx_ty);
    if (should_exit_ || (size_t)unit_index >= units_.size()) return 0;
    auto& uc = units_[(size_t)unit_index];
    if (!ai) { // MP/TP cost (tail of 4C605); AI (sub_432A0) is settled by the caller.
        uc.combat_mp_50 = (uint16_t)((int)uc.combat_mp_50 - (int)kSpellCost[d]);
        battle_consume_tp(unit_index, thr);
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: magic done unit=%d MP=%u TP=%d\n",
                                uc.id, (unsigned)uc.combat_mp_50, battle_tp());
    return 0;
}

// ==== Items (sub_4C19D/4CDBD/4CB5E) ====
// sub_4C19D: builds the dx list in code order. cap = kClassCapFlat with stride 15.
std::vector<int> CombatII::battle_item_menu_ids(int unit_index) {
    std::vector<int> ids;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return ids;
    const auto& u = units_[(size_t)unit_index];
    const int cls = (int)u.combat_class;
    auto cap = [&](int off) { return battle_class_cap15(cls, off); };
    // [5Ch] 1..4 -> 5, 0Fh..16h -> 16: append that one entry and end the list (jmp loc_4C4EB).
    if (u.code_b >= 1 && u.code_b <= 4) { ids.push_back(5); return ids; }
    if (u.code_b >= 0x0F && u.code_b <= 0x16) { ids.push_back(16); return ids; }
    // HP: item 0 only while max/10 < cur (x100 raw) (`cmp eax,[48h]; jge skip`).
    if (battle_max_hp_raw(u) / 10 < battle_hp_raw(u)) ids.push_back(0);
    if (cap(0)) ids.push_back(5);
    // With byte_BA84E == 0, id/[5Bh]/word_FD8Bx == 0 adds 16 (one-shot summon).
    if (sce_map_kind_ == 0 &&
        ((u.id == 2 && u.combat_type_5b == 0x20 && battle_fd8bc_ == 0) ||
         (u.id == 0x16 && u.combat_type_5b == 0x3B && battle_fd8be_ == 0) ||
         (u.id == 0x1D && u.combat_type_5b == u.id && battle_fd8c0_ == 0))) ids.push_back(16);
    if (cap(1) == 1 && sce_map_kind_ < 2) {
        // loc_4C281 (81410..81430): unk_731AD == 1, kind < 2, class 26h needs [0Dh] == 1,
        // then sub_1093D(x,y): raw > 100 subtracts 100 and the low nibble must not be 8.
        const size_t ti = (size_t)(int8_t)u.y * map_w_ + (size_t)(int8_t)u.x;
        int attr = (!tile_attr_.empty() && ti < tile_attr_.size()) ? (int)tile_attr_[ti] : 0;
        if (attr > 0x64) attr -= 0x64;
        if ((attr & 0xF) != 8 && !(cls == 0x26 && u.unit_kind_0d != 1)) ids.push_back(1);
    }
    if (cap(2)) ids.push_back(3);
    if (cap(3)) ids.push_back(6);
    if (cap(4)) ids.push_back(4);
    if (cap(5)) {
        const size_t ti = (size_t)(int8_t)u.y * map_w_ + (size_t)(int8_t)u.x;
        int attr = (!tile_attr_.empty() && ti < tile_attr_.size()) ? (int)tile_attr_[ti] : 0;
        if (attr > 0x64 && (((attr - 0x64) & 0xF) == 2 || ((attr - 0x64) & 0xF) == 3)) ids.push_back(2);
    }
    if (cap(6)) ids.push_back(7);
    if (cap(7)) ids.push_back(8);
    if (cap(8) != 0) ids.push_back(9);                        // unk_731B4 != 0 (cmp ..,0; jz)
    if (cap(9) && !(u.flags_84 & 2)) ids.push_back(0x0A);  // unk_731B5==1 && !([84h]&2)
    if (cap(10)) ids.push_back(0x0B);
    if (cap(11)) ids.push_back(0x0C);
    if (cap(12)) ids.push_back(0x0D);
    if (battle_class_capflat(cls, 13) && !(u.flags_84 & 4)) ids.push_back(0x0E);
    if (battle_class_capflat(cls, 14)) ids.push_back(0x0F);
    if (u.combat_value_43 > 0x32) ids.push_back(0x11);
    return ids;
}

// Test hook (G2_INPUT_SCRIPT KEY m) for magic selection: fills battle_magic_options_ with
// the 7 spells of the current group page (R = g*21+d*3+k) and draws nothing (the original
// UI is the 4C72A graphic ring).
void CombatII::battle_magic_show_page(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    battle_magic_options_.clear();
    if (!battle_magic_groups_.empty()) {
        if (battle_magic_page_ < 0 || (size_t)battle_magic_page_ >= battle_magic_groups_.size())
            battle_magic_page_ = 0;
        const int g = battle_magic_groups_[(size_t)battle_magic_page_];
        const int k = battle_spell_kind(u, g);
        if (k >= 0)
            for (int d = 0; d < 7; ++d) battle_magic_options_.push_back(g * 21 + d * 3 + k);
    }
}

// sub_2CACA: ITEM.DAT 0x39 percentage bonus. eax = unit (only [5Bh] is used),
//   edx = incoming damage.
//   out = in + in*184A4([5Bh],0x39)/100 (the 184A4 byte is signed, idiv 100).
int CombatII::battle_weapon_2caca(int attacker_index, int damage) {
    if (attacker_index < 0 || (size_t)attacker_index >= units_.size()) return damage;
    const auto& a = units_[(size_t)attacker_index];
    const int adj = (int8_t)battle_lookup_184a4(a.combat_type_5b, 0x39);
    return damage + damage * adj / 100;
}

// sub_2CC3A: runs a V normal attack (2F47F/2F68C call it with edx=5/7/0x0C, ecx=1 and
//   ebx = fixed damage, a dead store). Returns -1 when the 1D124 gate fails. The tail
//   si is 0x14 (20) for variant 5, 0x19 (25) for 7 and 0x1E (30) for 0x0C.
//   Targets are the whole F0648 list (= battle_target_list_) filled by 1D124; per target it
//   computes base = 1D770(attacker, target, ebx=0, ecx=0), scaled = si*base/10 and
//   final = 2CACA, then applies 45BFB / HP loss / 45ED2, finishes with the 287AF/1ED45/12124
//   equivalents and returns 1.
//   Deviation: presentation (454B3/5F517/5F3FD/5EECF/5EE51/5E9E8/44D6E family, the large
//   0x90-frame sequence) is replaced by log output; gameplay (targets, damage, death,
//   triggers) is equivalent. Extra F0648 collection (28B24 and so on) is approximated by
//   1D124.
int CombatII::battle_weapon_2cc3a(int unit_index, int variant, int sfx_id) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    int si = -1;
    if (variant == 5) si = 0x14;
    else if (variant == 7) si = 0x19;
    else if (variant == 0x0C) si = 0x1E;
    if (si < 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 2cc3a unit=%d variant=%d unknown -> -1\n",
                                    units_[(size_t)unit_index].id, variant);
        return -1;
    }
    const int n = battle_range_collect(unit_index);
    if (n <= 0 || battle_target_list_.empty()) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 2cc3a unit=%d variant=%d 1D124 empty -> -1\n",
                                    units_[(size_t)unit_index].id, variant);
        return -1;
    }
    const std::vector<int> targets = battle_target_list_;
    if (battle_trace_) fprintf(stderr, "COMBAT: 2cc3a unit=%d variant=%d si=%d targets=%zu\n",
                                units_[(size_t)unit_index].id, variant, si, targets.size());
    auto& A = units_[(size_t)unit_index];
    // Original sub_2CC3A (ASM 39805..39806): `sub_11F74(x, y, ebx=0)` is not a camera move
    // but a call that clears the unit's own cell in the occupancy map (dword_CC998); the
    // restore is sub_11E04 at the end (ASM 40313). Then `[+64h]=5` and sub_10971/10988 give
    // the screen position to draw at. There is no camera call inside this routine; that is
    // the caller's job (AI: sub_3FFD4 calls sub_1124A right before the handler; player:
    // sub_4C98A does not, because entering the ring already centred the unit).
    // The port models occupancy through units_ positions, so no separate record is needed.
    // The presentation (44D89 all targets -> [+64h]=5 -> 5 swing poses -> dim -> afterimage,
    // shake, cleanup) lives in combatii_fx_weapon.cpp; the damage loop follows it.
    battle_fx_2cc3a_anim(unit_index, variant, sfx_id);
    std::vector<BattleDamageFx> fx;
    fx.reserve(targets.size());
    for (size_t slot = 0; slot < targets.size(); ++slot) {
        const int ti = targets[slot];
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        if (!T.active || T.hp <= 0) continue;
        const int base = battle_damage(unit_index, ti, 0, 0, 0); // sub_1D770(ebx=0, ecx=0)
        const int scaled = si * base / 10;
        const int fin = battle_weapon_2caca(unit_index, scaled);
        if (battle_trace_) fprintf(stderr, "COMBAT: 2cc3a unit=%d -> target=%d base=%d scaled=%d final=%d\n",
                                    A.id, T.id, base, scaled, fin);
        battle_damage_fx_prepare(unit_index, ti, (int)slot, fin, fx); // 45BFB + [48h] -= fin + 45ED2
    }
    battle_damage_fx_play(fx, targets);                              // sub_287AF
    game_timer_set(6);                                               // sub_1033B(6)
    battle_damage_fx_triggers(targets, unit_index);                  // sub_1ED45(caster)
    if (!should_exit_ && (size_t)unit_index < units_.size())
        reset_unit_pose(units_[(size_t)unit_index]);                 // sub_12124(caster)
    return 1;
}

// sub_2CA40: V-weapon 1..3 branch (first step of the si != 0 path). Picks a threshold pair
// from [+35], compares unk_74144[[+36]*20+[+37]] against it and returns 3/2/1.
// With [+37] == 0 it first sets [+36] = rand(12); at the end [+37] = ([+37]+1)%20.
// The threshold table is 20 bytes at EXE file offset 0x32DFA (off_2C9FA; IDA mislabels the
// first 2 dwords as offsets - the low/high words are the thresholds):
// (6,16),(2,12),(0,10),(0,6),(0,2). All static SCE values of [35] are <= 4, so only 0..4 is
// supported and anything else trips a warning and returns 1. The port LCG differs from DOS
// rand() in general.
int CombatII::battle_weapon_2ca40(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 1;
    auto& u = units_[(size_t)unit_index];
    static constexpr int kThresh[5][2] = {{6,16},{2,12},{0,10},{0,6},{0,2}};
    const int sel = (int)u.weap_35;
    if (sel < 0 || sel > 4) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 2ca40 unit=%d sel35=%d out of range (Tripwire, return 1)\n",
                                    u.id, sel);
        return 1;
    }
    if (u.weap_37 == 0) {
        u.weap_36 = (uint8_t)combat_rand(12);
        if (battle_trace_) fprintf(stderr, "COMBAT: 2ca40 unit=%d randomize row36=%u ([37]==0)\n",
                                    u.id, (unsigned)u.weap_36);
    }
    const int row = (int)u.weap_36, col = (int)u.weap_37;
    int pattern = 0;
    if (row >= 0 && row < kBattleHitPatternRows && col >= 0 && col < kBattleHitPatternCols)
        pattern = (int)(int8_t)kBattleHitPattern[(size_t)row * 20u + (size_t)col];
    else if (battle_trace_)
        fprintf(stderr, "COMBAT: warning: 2ca40 unit=%d row/col out of range (%d,%d, Tripwire pattern=0)\n",
                u.id, row, col);
    const int lo = kThresh[sel][0], hi = kThresh[sel][1];
    int ret = 1;
    if (pattern < lo) ret = 3;
    else if (pattern < hi) ret = 2;
    else ret = 1;
    u.weap_37 = (uint8_t)(((int)u.weap_37 + 1) % 20);
    if (battle_trace_) fprintf(stderr, "COMBAT: 2ca40 unit=%d sel=%d row=%d col=%d pat=%d lo=%d hi=%d -> %d (next col=%u)\n",
                                u.id, sel, row, col, pattern, lo, hi, ret, (unsigned)u.weap_37);
    return ret;
}

// sub_301BB: V-weapon self-damage plus presentation. eax = unit, edx = weapon idx (0..12),
//   ebx = 0 (si != 0) / 1 (si == 0).
//   ecx = (bx == 0 ? 753F4 : 75440)[idx] >> 16, skipped when <= 0.
//   dmg = min(fixed, [48h]-100) and [48h] -= dmg.
//   1124A -> 45BFB -> 45ED2(facing) -> 1033B(3) -> 25 frames
//   (454B3/45F29/45CCB/5E9E8/499E4) -> 12124 -> 53797(mode 1) -> 499DA(0x24).
//   Self-damage always leaves at least 100 raw HP (1 displayed), so it cannot kill. The
//   digits follow the same convention as play_hp_change (45BFB/45CCB).
void CombatII::battle_weapon_301bb(int unit_index, int weapon_idx, int is_leader) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (weapon_idx < 0 || weapon_idx > 24) return;
    if (is_leader && weapon_idx > 12) return; // dword_75440 covers si == 0 (0..12) only
    auto& u = units_[(size_t)unit_index];
    const int fixed = is_leader ? (int)battle_weapon_fixed75440(weapon_idx)
                                : (int)battle_weapon_fixed753f4(weapon_idx);
    if (fixed <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 301bb unit=%d idx=%d leader=%d fixed=%d <=0 skip\n",
                                    u.id, weapon_idx, is_leader, fixed);
        return;
    }
    const int old_raw = battle_hp_raw(u);
    int dmg = old_raw - 100;
    if (dmg > fixed) dmg = fixed;
    if (dmg < 0) dmg = 0;
    if (dmg == 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 301bb unit=%d idx=%d fixed=%d hp_raw=%d -> no damage (clamp)\n",
                                    u.id, weapon_idx, fixed, old_raw);
        return;
    }
    camera_center_on(u.x, u.y); // sub_1124A
    // sub_45BFB: prepare the digits. walk_off stays 0; digit layout matches play_hp_change.
    u.walk_off_x = u.walk_off_y = 0;
    char digits[8]; snprintf(digits, sizeof(digits), "%d", dmg);
    const int ndig = (int)strlen(digits);
    const int sx0 = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int sy0 = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    int digit_y[5]; for (int k = 0; k < 5; k++) digit_y[k] = sy0 + 10;
    const int new_raw = old_raw - dmg;
    const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
    u.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
    u.hp = new_hp;
    setup_knockback_hit(u, u.facing & 3); // sub_45ED2 edx=[60h]>>24==[63h] facing
    if (battle_trace_) fprintf(stderr, "COMBAT: 301bb unit=%d idx=%d leader=%d fixed=%d dmg=%d hp_raw %d->%d (hp %d)\n",
                                u.id, weapon_idx, is_leader, fixed, dmg, old_raw, new_raw, new_hp);
    static constexpr uint8_t kDmgColor[8] = {0x04, 0x10, 0x31, 0x51, 0x20, 0x01, 0x00, 0x00};
    const int dmg_idx = std::min(std::max(battle_effective_army(u), 0), 7);
    const uint8_t color = kDmgColor[(size_t)dmg_idx];
    if (!small_font_tried_) load_small_font();
    const bool damage_small_font = small_font_loaded_;
    game_timer_set(3); // mov eax,3; call sub_1033B (edx=1 is ignored)
    // The original loops 1..24 (starts at bx=1 and exits at 0x19); the 45F29/45CCB counter
    // also starts at 1.
    for (int f = 1; f < 0x19 && !should_exit_ && !skip_cutscene_; f++) {
        render_battle_frame(); // sub_454B3
        apply_knockback_hit(u, 0, f); // sub_45F29(U, 0, f)
        if (f < 5) for (int k = 0; k < 5; k++) digit_y[k] -= 2;
        else if (f >= 0x14) for (int k = 0; k < 5; k++) digit_y[k] += 2;
        else { const int k = (f - 5) / 5; if (k < 5) digit_y[k] += 2; }
        {
            const int x0 = sx0 + 12 - ndig * 3;
            if (x0 - 16 >= 0 && x0 - 16 < 0x120) {
                uint8_t* bb = video_.get_backbuffer();
                char one[2] = {0, 0};
                for (int k = 0; k < ndig && k < 4; k++) {
                    one[0] = digits[k];
                    const int x = x0 + k * 6, y = digit_y[k];
                    if (y - 5 < 0 || y - 5 >= 0xB2) continue;
                    if (damage_small_font) {
                        draw_small_text(x + 1, y + 1, one, 0, 1);
                        draw_small_text(x, y, one, color, 1);
                    } else if (bb && font_.is_loaded()) {
                        font_.set_text_mode(4, 0x0000, 0xFF);
                        font_.draw_string(bb, SCREEN_W, SCREEN_H, x + 1, y + 1, one);
                        font_.set_text_mode(4, (uint16_t)(color << 8), 0xFF);
                        font_.draw_string(bb, SCREEN_W, SCREEN_H, x, y, one);
                    }
                }
            }
        }
        render_overlay(); // sub_5E9E8
        video_.present();
        wait_game_tick(); // sub_499E4
    }
    reset_unit_pose(u); // sub_12124 (eax=esi, ebx=1)
    run_unit_hp_trigger(unit_index, -1, 1); // sub_53797(eax=esi, edx=0, ebx=1)
    if (should_exit_) return;
    game_timer_set(0x24);
    wait_game_tick();     // mov eax,24h; call sub_499DA -> falls through to 499E4
}

// sub_2F47F family (F1474 vector idx = si-1, sub_300A0 0..12). 1 on success, -1 on failure.
//   idx0=2F47F, idx8=2F68C: dx1 -> 5, dx2 -> 7, dx3 -> 0x0C passed to 2CC3A, else -1.
//   idx2=2F56C: 2D5A3 plus the [13h] bits (and E3h, or 4<<(dx-1)), always 1.
//   idx4/12=2F5A7: 2D684 and [13h] &= 9Fh, then or 20h when dx < 3 else or 40h, always 1.
//   idx11=2F71E: front single 1D124 plus 2CAF7(mul dx==1?0:dx, add 3000).
//   The remaining vectors (2F592/2F5D7/2F5EF/loc_2F4DE/2F68C/2F6EB/2F706/2F80B/2F98A/
//   2FA1E/2FA41/2FA6E/2FD03/2FED7/2FF36/2FF4B/2FFF9/30049) are handled in
//   battle_weapon_f1474 / battle_weapon_f1474_ext above; the ext dispatcher is entered for
//   idx >= 13, and the fall-through below is only reached for an out-of-table idx.
//   4C98A returns to the ring without spending on -1, and on 1 runs 301BB plus the cost when
//   class != 0x32.
int CombatII::battle_weapon_f1474(int unit_index, int idx, int dx) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& u = units_[(size_t)unit_index];
    if (idx == 0 || idx == 8 || idx == 19) { // idx19=2FED7: same dx 1/2/3 -> 5/7/0xC.
        //   ebx=3000 is a dead store; ecx survives as the 4th argument (cx) of 2CC3A and
        //   becomes the switch hit-sound id (1 vs 2).
        int variant = -1;
        if (dx == 1) variant = 5;
        else if (dx == 2) variant = 7;
        else if (dx == 3) variant = 0x0C;
        if (variant < 0) {
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: f1474[%d]=%s unit=%d dx=%d invalid -> -1\n",
                                        idx, idx == 19 ? "2FED7" : (idx == 8 ? "2F68C" : "2F47F"), u.id, dx);
            return -1;
        }
        const int sfx_id = (idx == 19) ? 2 : 1;   // sub_2FED7 does `mov ecx,2`, the rest `mov ecx,1`
        const int r = battle_weapon_2cc3a(unit_index, variant, sfx_id);
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=%s unit=%d dx=%d -> 2CC3A edx=%d -> %d\n",
                                    idx, idx == 0 ? "2F47F" : "2F68C", u.id, dx, variant, r);
        return r;
    }
    if (idx == 2) { // sub_2F56C
        battle_fx_2d5a3(unit_index);                             // sub_2D5A3(counter.nsp)
        if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
        u.combat_flags_13 = (uint8_t)((u.combat_flags_13 & 0xE3) | (4 << (dx - 1)));
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[2]=2F56C unit=%d dx=%d -> [13h]=%02X (return 1)\n",
                                    u.id, dx, (unsigned)u.combat_flags_13);
        return 1;
    }
    if (idx == 4 || idx == 12 || idx == 20) { // sub_2F5A7 (duplicated by F1484/F14A4/F14C4)
        battle_fx_2d684(unit_index);                             // sub_2D684(critical.nsp)
        if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
        u.combat_flags_13 = (uint8_t)(u.combat_flags_13 & 0x9F);
        if (dx < 3) u.combat_flags_13 = (uint8_t)(u.combat_flags_13 | 0x20);
        else u.combat_flags_13 = (uint8_t)(u.combat_flags_13 | 0x40);
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2F5A7 unit=%d dx=%d -> [13h]=%02X (return 1)\n",
                                    idx, u.id, dx, (unsigned)u.combat_flags_13);
        return 1;
    }
    if (idx == 11) { // sub_2F71E: front single 1D124 + 2CAF7 mul 0/2/3 + 3000
        if (dx < 1 || dx > 3) {
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: f1474[11]=2F71E unit=%d dx=%d invalid -> -1\n",
                                        u.id, dx);
            return -1;
        }
        const int n = battle_weapon_collect_front(unit_index);
        if (n <= 0) {
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[11]=2F71E unit=%d dx=%d front empty -> -1\n",
                                        u.id, dx);
            return -1;
        }
        const int mul = (dx == 1) ? 0 : dx; // emu: dx1->0, dx2->2, dx3->3
        const int r = battle_weapon_2caf7_loop(unit_index, mul, 3000);
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[11]=2F71E unit=%d dx=%d -> 2CAF7(%d,3000) -> %d\n",
                                    u.id, dx, mul, r);
        return r;
    }
    if (idx == 1 || idx == 7) { // loc_2F4DE (restrain.nsp, F14D8 auto approximation)
        // The original fills F0648 with a 1AC08 diamond of radius 2D4EB+2CBFB
        // (ITEM[5Bh][0x38]+dx) through 1AC59/1A1F8 and then runs 2D527 (restrain.nsp)
        // plus 197AD (queue removal) per target, with no damage. The port matches the
        // diamond collection, 2CBA1/dim, the per-target 2D527 loop and 197AD. An empty
        // collection returns -1.
        if (dx < 1 || dx > 3) {
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: f1474[%d]=2F4DE unit=%d dx=%d invalid -> -1\n",
                                        idx, u.id, dx);
            return -1;
        }
        const int base_r = (int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38) + dx; // sub_2CBFB
        const int radius = base_r < 0 ? 0 : base_r;
        const int n = battle_weapon_collect_diamond(unit_index, radius);
        if (n <= 0) {
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2F4DE unit=%d dx=%d radius=%d empty -> -1\n",
                                        idx, u.id, dx, radius);
            return -1;
        }
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2F4DE unit=%d dx=%d radius=%d targets=%zu (restrain)\n",
                                    idx, u.id, dx, radius, battle_target_list_.size());
        // loc_2E80C: 2CBA1 register -> 44D6E(1,0) -> per target {4F4CD(32h), 2D527, 197AD}
        // -> 44D6E(0) -> 44D75 -> 12124 -> 1FFCD. 2D527 itself is 24 frames of restrain.nsp.
        battle_highlight_add_depth(unit_index);                 // sub_2CBA1
        battle_dim_row0_ = true;                                // sub_44D6E(1,0)
        const bool have_restrain = battle_nsp_load("restrain.nsp"); // sub_1FEA4
        for (int ti : battle_target_list_) {
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            if (!units_[(size_t)ti].active) continue;
            sfx(0x32);                                         // sub_4F4CD(32h)
            if (have_restrain) battle_fx_2d527(ti);             // sub_2D527
            if (should_exit_) break;
            battle_remove_from_ready_queue(ti);                // sub_197AD
            if (should_exit_) break;
        }
        battle_dim_row0_ = false;                               // sub_44D6E(0)
        battle_highlight_clear();                               // sub_44D75
        battle_target_list_.clear();
        if (!should_exit_ && (size_t)unit_index < units_.size())
            reset_unit_pose(units_[(size_t)unit_index]);        // sub_12124
        if (have_restrain) battle_nsp_release();                 // sub_1FFCD
        return 1;
    }
    if (idx == 3 || idx == 9 || idx == 21) { // sub_2F592/2F6EB/2FF36 -> sub_2EB61 (single 1D770+2CACA, add 3000/3000/0)
        const int edi2eb = (idx == 3) ? 0 : (idx == 9 ? 1 : 2); // 2EB61 edx: radius +6+edx; edx<2 adds 2E896 knockback
        // Entry state: ecx=3000, edx=0/1, ebx=dx (1..3). The original picks one target
        // through F14D8 automatic (2EA0C) or manual (4A483), doubles [44h] when dx > 1,
        // then applies base = 1D770, tmp = 3000+base, fin = 2CACA(tmp) to that target.
        // Automatic collection is the lowest scoring unit among 17531/1762C along the front
        // ray at distance 2..R (R = 184A4+6 with 2EB61 edx 0/1). The manual path (4A483
        // cursor) is approximated by the automatic one.
        if (dx < 1 || dx > 3) {
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: f1474[%d]=2EB61 unit=%d dx=%d invalid -> -1\n",
                                        idx, u.id, dx);
            return -1;
        }
        const int base_r = (int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38); // sub_2CBFB
        const int n = battle_weapon_collect_2ea0c(unit_index, (int16_t)(base_r + 6 + edi2eb));
        // Put the player's manual target (4A483) at the head of F0648; proceed even when
        // the automatic collection is empty as long as a manual target exists.
        const bool picked = battle_weapon_apply_pick(unit_index);
        if (n <= 0 && !picked) {
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2EB61 unit=%d dx=%d no target -> -1\n",
                                        idx, u.id, dx);
            return -1;
        }
        const int ti = battle_target_list_[0];
        auto& A = units_[(size_t)unit_index];
        const int16_t orig44 = A.combat_value_44;
        if (dx > 1) A.combat_value_44 = (int16_t)((int32_t)orig44 * 2); // [44h] *= 2 when dx > 1
        const int base = battle_damage(unit_index, ti, 0, 0, 0);
        A.combat_value_44 = orig44;
        const int add2eb = (idx == 21) ? 0 : 3000; // 2F6FB: ecx = high word of 744A0+2/744BE (idx21 = 744C0 = 0)
        const int tmp = add2eb + base;
        const int fin = battle_weapon_2caca(unit_index, tmp);
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2EB61 unit=%d dx=%d -> target=%d base=%d tmp=%d final=%d (2DA5E)\n",
                                    idx, u.id, dx, units_[(size_t)ti].id, base, tmp, fin);
        battle_highlight_add_depth(unit_index);                 // sub_2CBA1 (2EB61 loc_2ECB5)
        A.anim_frame = 5;
        // 2EB61 loc_2ECB5: 49ADF/2CBA1 -> 44D6E(1) -> sub_2DA5E(slight/spearfly/fistlv3) ->
        //   44D6E(0) -> 44D75 -> 1D770/2CACA damage.
        battle_dim_row0_ = true;                                 // sub_44D6E(1)
        battle_fx_2da5e(unit_index, (int)(int8_t)A.x, (int)(int8_t)A.y, edi2eb);
        battle_dim_row0_ = false;                                // sub_44D6E(0)
        battle_highlight_clear();                                // sub_44D75
        if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
        // 2EB61 after loc_2ED0D: right after [48h] -= fin, edx < 2 runs sub_2E896 (target,
        // attacker facing) knockback before 46359.
        if (edi2eb < 2) battle_knockback_2e896(ti, A.facing & 3);
        {
            auto& T = units_[(size_t)ti];
            const int old_raw = battle_hp_raw(T);
            int new_raw = old_raw - fin;
            if (new_raw > battle_max_hp_raw(T)) new_raw = battle_max_hp_raw(T);
            const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
            const int delta = new_hp - T.hp;
            if (delta != 0) {
                VmEvent hp; hp.type = VmEventType::Hp; hp.hp_unit = ti;
                hp.hp_delta = delta; hp.hp_dir = A.facing & 3; hp.hp_killer = unit_index;
                play_hp_change(hp);
                if (should_exit_) return 1;
            } else {
                run_unit_hp_trigger(ti, unit_index, 1);
            }
        }
        run_unit_hp_trigger(unit_index, -1, 1);
        reset_unit_pose(units_[(size_t)unit_index]);
        game_timer_set(6);
        battle_target_list_.clear();
        return 1;
    }
    if (idx == 5 || idx == 10) { // sub_2F5D7/2F706 -> sub_2E25B (hon.nsp slash + 2CAF7)
        // Entry state: idx5 ecx=7449C>>16 edx=0, idx10 ecx=744A4>>16 edx=1, ebx=dx.
        // R = base+5 (idx5) / base+6 (idx10) with base = 184A4(5Bh, 0x38).
        if (dx < 1 || dx > 3) {
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: f1474[%d]=2E25B unit=%d dx=%d invalid -> -1\n",
                                        idx, u.id, dx);
            return -1;
        }
        const int add = (idx == 5) ? 5000 : 3000; // emu ecx
        const int base_r = (int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38); // sub_2CBFB
        const int radius = base_r + (idx == 5 ? 5 : 6);
        const int n = battle_weapon_collect_2e0f0(unit_index, radius);
        const bool picked = battle_weapon_apply_pick(unit_index);
        if (n <= 0 && !picked) {
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2E25B unit=%d dx=%d R=%d empty -> -1\n",
                                        idx, u.id, dx, radius);
            return -1;
        }
        const std::vector<int> targets = battle_target_list_;

        // loc_2E40F: 44D6E(1) -> 2CBA1 -> hon.nsp + death/bomb -> 200DE(unit,3).
        battle_dim_row0_ = true;                                  // sub_44D6E(1)
        battle_highlight_add_depth(unit_index);                   // sub_2CBA1 (41768)
        const bool hon_ok = battle_nsp_load("hon.nsp");           // sub_1FEA4
        battle_ssp_play("death.ssp");                             // sub_4F460
        battle_ssp_play("bomb.ssp");                              // sub_4F460
        {
            int ccx = (int)(int8_t)u.x, ccy = (int)(int8_t)u.y;
            switch (u.facing & 3) {
                case 0: ccy += 3; break;                          // 200DE facing 0: +y
                case 1: ccx -= 3; break;                          // 200DE facing 1: -x
                case 2: ccy -= 3; break;                          // 200DE facing 2: -y
                default: ccx += 3; break;                         // 200DE facing 3: +x
            }
            camera_center_on(ccx, ccy);                           // sub_200DE(unit,3)
        }

        // loc_2E49A: pose 6, 1033B(1Eh), then 3 present/wait frames (6 -> 7 -> 8 -> 9).
        u.anim_frame = 6;
        game_timer_set(0x1E);
        for (int i = 0; i < 3 && !should_exit_; ++i) {
            battle_fx_present();                                  // 454B3 + 5E9E8
            ++u.anim_frame;
            wait_game_tick();                                     // sub_499E4
        }
        battle_highlight_clear();                                 // sub_44D75
        for (int ti : targets) battle_highlight_add(ti);          // sub_44D89 per target

        // loc_2E54D..loc_2E69D: 12-frame caster NC2 trail (poses 5..9). Each step moves
        // var_20/var_18 (the caster) by (target - caster)/5, so index 11 is the first
        // afterimage and index 0 is the final position near the target. The draw loop
        // then stamps pose 9 silhouettes plus the pose 9/pose 6 main frame at the target.
        const NC2Frame* poses[5] = {};
        for (int i = 0; i < 5; ++i)
            poses[i] = nc2_frame_for(u.sprite_type, u.facing & 3, 5 + i);
        const int ax = ((int)u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0C;
        const int ay = ((int)u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 2;
        int ex = ax, ey = ay;
        if (!targets.empty()) {
            const auto& T = units_[(size_t)targets[0]];           // far end of the 2E0F0 ray
            ex = ((int)T.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0C;
            ey = ((int)T.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 2;
        }
        int trail_x[12], trail_y[12];
        int tx = ax, ty = ay;                                     // var_20/var_18 start at the caster
        for (int i = 11; i >= 0; --i) {
            tx += (ex - tx) / 5;                                  // var_20 += (var_1C-var_20)/5
            ty += (ey - ty) / 5;                                  // var_18 += (var_8-var_18)/5
            trail_x[i] = tx; trail_y[i] = ty;
        }
        // sub_5F517 reads the 5-dword table copied from loc_2CA23+2. The dword load is
        // [var_4C-2 + 4*(si/3)], so `sar 10h` yields the first word of entry si/3:
        // 3D3Dh, 3C3Ch, 3A3Ah, 3939h (the 5th entry 3838h is past si=11).
        static const uint8_t kHonTrailColor[4] = {0x3D, 0x3C, 0x3A, 0x39};
        sfx(0x32);                                                // sub_4F4CD(32h)
        for (int f = 11; f >= 0 && !should_exit_; --f) {
            game_timer_set((0x0F - f) >> 1);                      // sub_1033B((0Fh-f)>>1)
            battle_fx_world();                                    // sub_454B3
            for (int s = 0; s < f; ++s) {
                if (!poses[4]) break;
                blit_nc2_frame_solid(trail_x[s] + poses[4]->x_off,
                                     trail_y[s] + poses[4]->y_off,
                                     *poses[4], kHonTrailColor[s / 3]); // sub_5F517
            }
            const NC2Frame* mainf = (f == 11) ? poses[4] : poses[1]; // var_38[4] then var_38[1]
            if (mainf)
                blit_nc2_frame(ex + mainf->x_off, ey + mainf->y_off, *mainf); // sub_5F3FD
            battle_fx_overlay();                                  // sub_454D5
            if (f == 11) {
                // sub_2D48A(14h): full-palette 3Fh flash, restored 14h ticks later.
                uint8_t flash[0x100 * 3];
                for (int p = 0; p < 0x100 * 3; ++p) flash[p] = 0x3F;
                pal_load_range(0, 0x100, flash);
                video_.present();
                game_timer_set(0x14);
                wait_game_tick();
                pal_load_range(0, 0x100, &battle_palette_[0][0]);
                video_.present();
                game_timer_set(3);                                // 2D48A -> 1033B(3)
            }
            battle_fx_commit();                                   // sub_5E9E8 after 2D48A
            wait_game_tick();                                     // sub_499E4
        }

        // loc_2E6B5: after the last trail frame the caster lands on the tile of its final
        // interpolated position: sub_109A3(var_20), sub_109CE(var_8+4), then sub_11E04
        // occupies the new tile (the port's units_ position is the occupancy).
        // trail_x[0]/trail_y[0] are that final var_20/var_18 (the 12th step).
        // 109A3 subtracts only the 4px base (the trail's +0C centre offset stays inside
        // the idiv), while 109CE(var_8+4) cancels its own -4 before the sar 4.
        if (!targets.empty() && (size_t)unit_index < units_.size()) {
            auto& M = units_[(size_t)unit_index];
            const int nx = (trail_x[0] - cam_fine_x_ - TILE_ANCHOR_X) / TOWN_TILE_W + cam_tile_x_;
            const int ny = ((trail_y[0] - cam_fine_y_) >> 4) + cam_tile_y_;
            M.x = nx;
            M.y = ny;
            M.anim_frame = 6;                                     // [ebp+64h] = 6
        }

        // loc_2E6FD/loc_2E726: 44D75 -> 2CBA1 -> 1214F targets, then 1033B(9), sfx(33)
        // and the 5-frame hon.nsp impact loop (1FFFE stamped at the trail points).
        battle_highlight_clear();                                 // sub_44D75
        battle_highlight_add_depth(unit_index);                   // sub_2CBA1 (41979)
        for (int ti : targets) {
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            auto& T = units_[(size_t)ti];
            T.anim_frame = (T.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00; // sub_1214F
        }
        game_timer_set(9);                                        // sub_1033B(9)
        sfx(0x33);                                                // sub_4F4CD(33h)
        if (hon_ok) {
            const int stamps = std::min<int>(12, (int)targets.size());
            // 2E74A reads [var_8+2]>>16, which aliases the low word of var_4: the same
            // 0..4 frame counter loaded into the magic frame index, so f is exact.
            for (int f = 0; f < 5 && !should_exit_; ++f) {
                battle_fx_world();                                // sub_454B3
                for (int s = 0; s < stamps; ++s) {
                    if (f >= (int)magic_frames_.size()) break;
                    const MagicFxFrame& fr = magic_frames_[(size_t)f];
                    battle_fx_blit(f, trail_x[s] + fr.xo, trail_y[s] + fr.yo); // sub_1FFFE
                }
                battle_fx_overlay();                              // sub_454D5
                battle_fx_commit();                               // sub_5E9E8
                wait_game_tick();                                 // sub_499E4
            }
        }
        battle_highlight_clear();                                 // sub_44D75
        battle_dim_row0_ = false;                                 // sub_44D6E(0)
        if (should_exit_) { battle_nsp_release(); return 1; }
        const int r = battle_weapon_2caf7_loop(unit_index, dx, add); // sub_2CAF7
        if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]); // 12124
        if (!should_exit_) battle_fx_present();                   // 454B3 + 5E9E8 (before 1FFCD)
        battle_nsp_release();                                     // sub_1FFCD
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d]=2E25B unit=%d dx=%d R=%d -> 2CAF7(%d,%d) -> %d\n",
                                    idx, u.id, dx, radius, dx, add, r);
        if (!should_exit_ && (size_t)unit_index < units_.size() && units_[(size_t)unit_index].active)
            battle_after_move(unit_index);                        // sub_5401B
        return 1;
    }
    if (idx == 6) { // sub_2F5EF (r5 diamond + 2CBA1 + 2D7D2 + 2CAF7 add 4000)
        // The original fills F0648 with a 1AC08 r5 diamond through 1A1F8, goes through
        // 2CBA1/2D7D2 (presentation stub) and applies the damage in one go with 2CAF7.
        // add = 7449C+2>>16 = 4000 (static dd). mul is approximated by dx (1..3); the exact
        // [esp+2]>>16 value is unknown. An empty collection returns -1.
        if (dx < 1 || dx > 3) {
            if (battle_trace_) fprintf(stderr, "COMBAT: warning: f1474[6]=2F5EF unit=%d dx=%d invalid -> -1\n",
                                        u.id, dx);
            return -1;
        }
        // sub_2F5EF: after collecting the r5 diamond (sub_1AC08/1A1F8) it still runs
        // 2CBA1/44D6E/2D7D2/44D6E/44D75 + sub_2CAF7 with an EMPTY list and returns eax=1.
        const int n = battle_weapon_collect_diamond(unit_index, 5);
        if (n <= 0 && battle_trace_)
            fprintf(stderr, "COMBAT: f1474[6]=2F5EF unit=%d dx=%d empty list (still returns 1)\n",
                    u.id, dx);
        // 2CBA1 -> 44D6E(1) -> 2D7D2(swstr.nsp, dl=dx) -> 44D6E(0) -> 44D75 -> 2CAF7.
        battle_highlight_add_depth(unit_index);                  // sub_2CBA1
        battle_dim_row0_ = true;                                 // sub_44D6E(1)
        battle_fx_2d7d2(unit_index, dx);                         // sub_2D7D2
        battle_dim_row0_ = false;                                // sub_44D6E(0)
        battle_highlight_clear();                                // sub_44D75
        if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
        const int r = battle_weapon_2caf7_loop(unit_index, dx, 4000);
        if (battle_trace_) fprintf(stderr, "COMBAT: f1474[6]=2F5EF unit=%d dx=%d -> 2CAF7(%d,4000) -> %d\n",
                                    u.id, dx, dx, r);
        return 1;                                               // sub_2F5EF: mov eax,1
    }
    if (idx >= 13 && idx <= 24) return battle_weapon_f1474_ext(unit_index, idx, dx);
    if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d] unit=%d dx=%d out of vector range (-1)\n",
                                idx, u.id, dx);
    return -1;
}

// Shared collector for the 13 sub_302E6 routines (approximates 1A1F8/1A452/1AC08/49ABC; the
// 4A483 cursor is approximated automatically).
// The original screen sweep covers 12x12 tiles from dword_BA83C/BA838 with 1A1F8
// (11DB2 + 1201D + dedup); the port sweeps the same 12x12 from cam_tile_. The F14D8 == 1
// automatic and 4A483 cursor branches are both handled by automatic collection here.
// The X start is caller-specific: sub_341C7 (edi 4) increments esi before the 1A1F8 call,
// so its columns are camX+1..camX+12 (sub_341C7+0x6F/0x78), while sub_31283 increments after
// the call and starts at camX. Y always starts at camY (the outer loop's first check is before
// the first loc_3420F row). x_shift reproduces that difference without changing other callers.
int CombatII::battle_weapon_collect_screen(int unit_index, int cap, int x_shift) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    const int ax_eff = battle_effective_army(A);
    for (int dy = 0; dy < 12; ++dy) {
        for (int dx = 0; dx < 12; ++dx) {
            const int tx = cam_tile_x_ + x_shift + dx, ty = cam_tile_y_ + dy;
            const int oi = battle_unit_index_at(tx, ty);
            if (oi < 0 || oi == unit_index) continue;
            if ((size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!o.active || o.hp <= 0) continue;
            if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
            bool dup = false;
            for (int v : battle_target_list_) if (v == oi) { dup = true; break; }
            if (dup) continue;
            battle_target_list_.push_back(oi);
            if (cap > 0 && (int)battle_target_list_.size() >= cap) return (int)battle_target_list_.size();
        }
    }
    return (int)battle_target_list_.size();
}

// sub_1AC08/1AC59 diamond approximation: collect hostiles within the Manhattan radius of
// the attacker, excluding itself.
int CombatII::battle_weapon_collect_diamond(int unit_index, int radius) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    const int ax_eff = battle_effective_army(A);
    for (size_t i = 0; i < units_.size(); ++i) {
        if ((int)i == unit_index) continue;
        const auto& o = units_[i];
        if (!o.active || o.hp <= 0) continue;
        if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
        const int d = std::abs(o.x - A.x) + std::abs(o.y - A.y);
        if (d < 1 || d > radius) continue;
        battle_target_list_.push_back((int)i);
    }
    return (int)battle_target_list_.size();
}

// sub_49ABC+1A1F8 front single tile (32589): put only the hostile on the front tile in
// F0648[0].
int CombatII::battle_weapon_collect_front(int unit_index) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    int fx = A.x, fy = A.y;
    switch (A.facing & 3) {
        case 0: fy += 1; break; // south y+1 (49ABC dir 0)
        case 1: fx -= 1; break; // west x-1
        case 2: fy -= 1; break; // north y-1
        default: fx += 1; break; // east x+1
    }
    const int oi = battle_unit_index_at(fx, fy);
    if (oi < 0 || (size_t)oi >= units_.size()) return 0;
    const auto& o = units_[(size_t)oi];
    if (!o.active || o.hp <= 0) return 0;
    if (!battle_armies_hostile(battle_effective_army(A), battle_effective_army(o), battle_hostility_inverted_)) return 0;
    battle_target_list_.push_back(oi);
    return 1;
}

// F1474 handlers that use the 4A483/1F19C target cursor - 2EB61 (idx3/9/21),
// 2E25B (idx5/10) and 2FD03 (idx18). With F14D8 == 1 (AI/script) the original also collects
// automatically without a cursor, so those are not listed here.
bool CombatII::battle_weapon_needs_cursor(int idx) const {
    return idx == 3 || idx == 5 || idx == 9 || idx == 10 || idx == 18 || idx == 21;
}

// Kind of 4A483 cursor used by the si == 0 path (sub_302E6 routines).
//   308EE (edi 0): with F14D8 != 1, picks an EMPTY tile (distance >= 2) on the 7-tile front
//     ray as the landing spot (ASM 44960..45040).
//   356DB (edi 6): with F14D8 != 1, picks an occupied tile on the 7-tile front ray
//     (ASM 50716..50830).
//   39A49 (edi 11): with F14D8 != 1, picks any tile and collects the radius-2 diamond
//     around it (ASM 56113..56200).
//   The other routines (31283/341C7/34C48/377A6/37FA4 and so on) never look at F14D8 and
//   collect the screen or ray automatically.
int CombatII::battle_weapon_cursor_kind(int unit_index, int si) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (battle_f14d8_ == 1) return 0;                 // AI(sub_3FFD4)/script(loc_51F86) auto target
    if (si > 0) return battle_weapon_needs_cursor(si - 1) ? 1 : 0;
    const int edi = battle_weapon_match_4bc3c(units_[(size_t)unit_index].id);
    if (edi == 0) return 2;                           // 308EE (bird.nsp)
    if (edi == 6) return 3;                           // 356DB (f-arrow/growl)
    if (edi == 11) return 4;                          // 39A49 (f-wave/rain)
    return 0;
}

// Whether the tile chosen in battle_pick_tx_/ty_ fits that cursor rule.
//   1 = target unit (regular F1474 path), 2 = 308EE landing (empty tile, front 7, distance >= 2),
//   3 = 356DB (hostile-occupied tile on the front 7), 4 = 39A49 (tile with a hostile in the
//   radius-2 diamond).
bool CombatII::battle_weapon_pick_valid(int unit_index, int kind) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (battle_pick_tx_ < 0 || battle_pick_ty_ < 0) return false;
    const int tx = battle_pick_tx_, ty = battle_pick_ty_;
    if (tx >= (int)map_w_ || ty >= (int)map_h_) return false;
    if (kind == 1) return battle_weapon_pick_target(unit_index, true) >= 0;
    const auto& A = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)A.x, ay = (int)(int8_t)A.y;
    // sub_13CBD guard (2..w-2, 4..h-4)
    if (tx < 2 || tx >= (int)map_w_ - 2 || ty < 4 || ty >= (int)map_h_ - 4) return false;
    // sub_1A146/1A452 front ray test (count 1, range 7).
    auto in_front_ray = [&](int range) {
        const g2range::Env env = battle_range_env_make();
        g2range::Range rg(env);
        const g2range::Unit au = battle_range_unit(unit_index);
        int fx = 0, fy = 0;
        const auto pt = rg.front_tile(au, au.facing);             // sub_1A1DE
        fx = pt.x; fy = pt.y;
        rg.rect_init(fx, fy, 1, au.facing, range);                // sub_1A452
        int x = 0, y = 0;
        for (int guard = 0; guard < 0x1000 && rg.rect_next(x, y); ++guard) {   // sub_1A552
            if (x == tx && y == ty) return true;
        }
        return false;
    };
    if (kind == 2) {
        if (std::abs(ax - tx) + std::abs(ay - ty) < 2) return false;          // sub_49ADF < 2 -> repick
        const int oi = battle_unit_index_at(tx, ty);                          // sub_11DB2
        if (oi >= 0 && (size_t)oi < units_.size() && units_[(size_t)oi].active &&
            units_[(size_t)oi].hp > 0) return false;                          // must be empty
        return in_front_ray(7);
    }
    if (kind == 3) {
        if (!in_front_ray(7)) return false;
        return battle_weapon_pick_target(unit_index, true) >= 0;              // occupant added by 1A1F8
    }
    if (kind == 4) {
        const int ax_eff = battle_effective_army(A);
        bool found = false;
        const g2range::Env env = battle_range_env_make();
        g2range::Range rg(env);
        rg.dia_init(tx, ty, 2);                                               // sub_1AC08(centre, 2)
        int x = 0, y = 0;
        for (int guard = 0; guard < 0x1000 && !found && rg.dia_next(x, y); ++guard) { // sub_1AC59
            const int oi = battle_unit_index_at(x, y);
            if (oi < 0 || (size_t)oi >= units_.size() || oi == unit_index) continue;
            const auto& o = units_[(size_t)oi];
            if (!o.active || o.hp <= 0) continue;
            if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
            found = true;
        }
        return found;
    }
    return false;
}

// Unit on the tile the player clicked (the cell 4A483 picks inside the 11DB2 guard).
int CombatII::battle_weapon_pick_target(int unit_index, bool hostile_only) const {
    if (battle_pick_tx_ < 0 || battle_pick_ty_ < 0) return -1;
    const int ti = battle_unit_index_at(battle_pick_tx_, battle_pick_ty_);
    if (ti < 0 || (size_t)ti >= units_.size() || ti == unit_index) return -1;
    const CombatUnitPlacement& T = units_[(size_t)ti];
    if (!T.active || T.hp <= 0) return -1;
    if (hostile_only && unit_index >= 0 && (size_t)unit_index < units_.size() &&
        !battle_armies_hostile(battle_effective_army(units_[(size_t)unit_index]),
                               battle_effective_army(T), battle_hostility_inverted_)) return -1;
    return ti;
}

// Move the manual target to the head of F0648, which 2CAF7/1D770 read first. When it is
// missing from the automatic list (a pick outside the screen collection, as with 2FD03) it is
// inserted at the head.
bool CombatII::battle_weapon_apply_pick(int unit_index) {
    if (battle_f14d8_ == 1) return false;
    const int ti = battle_weapon_pick_target(unit_index, true);
    if (ti < 0) return false;
    auto it = std::find(battle_target_list_.begin(), battle_target_list_.end(), ti);
    if (it == battle_target_list_.end()) battle_target_list_.insert(battle_target_list_.begin(), ti);
    else if (it != battle_target_list_.begin()) std::rotate(battle_target_list_.begin(), it, it + 1);
    return true;
}

// sub_2EA0C/4A483 single-target approximation (356DB): nearest hostile (49ADF Manhattan,
// first one wins a tie).
int CombatII::battle_weapon_collect_nearest(int unit_index) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    const int ax_eff = battle_effective_army(A);
    int best = -1, best_d = INT_MAX;
    for (size_t i = 0; i < units_.size(); ++i) {
        if ((int)i == unit_index) continue;
        const auto& o = units_[i];
        if (!o.active || o.hp <= 0) continue;
        if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
        const int d = std::abs(o.x - A.x) + std::abs(o.y - A.y); // sub_49ADF
        if (d < best_d) { best_d = d; best = (int)i; }
    }
    if (best < 0) return 0;
    battle_target_list_.push_back(best);
    return 1;
}

// sub_2EA0C single automatic target (emulator-confirmed):
//   hostiles on the 1A452+1A552 front ray (R+1 tiles; two tile pre-consumption at the origin
//   means only dist 2..R count) are collected in ascending distance the way 1A1F8 does
//   (occupancy + 1201D hostility, no duplicates on a ray), then the lowest
//   "17531/1762C chain plus facing correction" score goes into F0648.
//   di = 17531(actor); for each candidate di = 1762C(tgt, di) when di >= 0, else di = 10;
//   ax = [tgt+46h] (signed); a facing tie halves ax (ax >>= 1), otherwise ax -= ax >> 2 when
//   (af+tf)&1. score = 2*ax - trunc(ax*di/10) compared strictly as 16-bit. No candidate -> 0.
int CombatII::battle_weapon_collect_2ea0c(int unit_index, int radius) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    if (radius < 2) return 0;
    const int ax_eff = battle_effective_army(A);
    int dx = 0, dy = 0;
    switch (A.facing & 3) {
        case 0: dy = 1; break; // south y+1 (1A452 bx=0)
        case 1: dx = -1; break; // west x-1 (bx=1)
        case 2: dy = -1; break; // north y-1 (bx=2)
        default: dx = 1; break; // east x+1 (bx=3)
    }
    int di = battle_damage_class(A); // sub_17531(attacker)
    int best16 = 0x7FFF, best = -1;
    for (int s = 2; s <= radius; ++s) { // start at 2, matching the pre-consumed dist 0..1
        const int oi = battle_unit_index_at(A.x + dx * s, A.y + dy * s);
        if (oi < 0 || (size_t)oi >= units_.size()) continue;
        const auto& o = units_[(size_t)oi];
        if (!o.active || o.hp <= 0) continue;
        if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
        if (di >= 0) di = battle_damage_adjustment(o, di); // sub_1762C chain
        else di = 10;
        int ax = (int16_t)o.combat_value_46; // mov ax,[46h]+cwde
        if (A.facing == o.facing) ax >>= 1; // sar eax,1
        else if ((((A.facing & 3) + (o.facing & 3)) & 1) != 0) ax -= (ax >> 2); // sar edx,2
        const int score = 2 * ax - (ax * di) / 10; // imul+idiv(trunc)+add/sub
        const int16_t s16 = (int16_t)score; // cmp ax (16-bit)
        if (s16 < best16) { best16 = s16; best = oi; }
    }
    if (best < 0) return 0;
    battle_target_list_.push_back(best);
    return 1;
}

// sub_37FA4 front 7-tile line, single target (approximates 1A452+1A552+1D124/165C2+49ABC):
// the first hostile along the facing line.
int CombatII::battle_weapon_collect_line(int unit_index, int range) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    const int ax_eff = battle_effective_army(A);
    int dx = 0, dy = 0;
    switch (A.facing & 3) {
        case 0: dy = 1; break;
        case 1: dx = -1; break;
        case 2: dy = -1; break;
        default: dx = 1; break;
    }
    for (int s = 1; s <= range; ++s) {
        const int tx = A.x + dx * s, ty = A.y + dy * s;
        const int oi = battle_unit_index_at(tx, ty);
        if (oi < 0) continue;
        if ((size_t)oi >= units_.size()) continue;
        const auto& o = units_[(size_t)oi];
        if (!o.active || o.hp <= 0) continue;
        if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
        battle_target_list_.push_back(oi);
        return 1;
    }
    return 0;
}

// sub_2E0F0 automatic (emulator-confirmed):
//   eax = actor, edx = R (2CBFB base+5/6; 308EE fixes it at 7). 169D7 lays down E8
//   (empty -1 / blocked -2) and the front ray (R tiles, 49ABC straight) is scanned far to
//   near. After seeing an empty tile (E8 == -1) it records var_4 = 1; a following hostile
//   (1201D == 1 plus 11DB2 occupancy) then breaks with success. Exhausting the ray (cx == di)
//   fails with 0. On success 1A25F collects every hostile on the ray far to near into F0648.
//   An empty ray or a side/rear target fails, unlike the screen sweep. The far point
//   (dist == R) is always treated as empty and excluded (only dist < R counts; the emulator
//   fails for dist 7 / R 7).
//   The port skips the E8 gate and uses hostile presence on the ray instead (equivalent on
//   open terrain; a blocked ray remains an open question). Collection order stays far to near.
int CombatII::battle_weapon_collect_2e0f0(int unit_index, int radius) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& A = units_[(size_t)unit_index];
    if (!A.active || A.hp <= 0) return 0;
    if (radius < 2) return 0; // dist < R needs at least one tile, so R < 2 finds nothing (emulator fails at R=2)
    const int ax_eff = battle_effective_army(A);
    int dx = 0, dy = 0;
    switch (A.facing & 3) {
        case 0: dy = 1; break; // south y+1 (49ABC bx=0)
        case 1: dx = -1; break; // west x-1 (bx=1)
        case 2: dy = -1; break; // north y-1 (bx=2)
        default: dx = 1; break; // east x+1 (bx=3)
    }
    for (int s = radius - 1; s >= 1; --s) { // far to near, far point excluded (emulator order)
        const int tx = A.x + dx * s, ty = A.y + dy * s;
        const int oi = battle_unit_index_at(tx, ty);
        if (oi < 0 || (size_t)oi >= units_.size()) continue;
        const auto& o = units_[(size_t)oi];
        if (!o.active || o.hp <= 0) continue;
        if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
        battle_target_list_.push_back(oi);
    }
    return (int)battle_target_list_.size();
}

// sub_2CBFB(eax=unit, edx=delta): 16-bit (signed ITEM[5Bh][0x38] byte + delta).
int CombatII::battle_weapon_2cbfb(int unit_index, int delta) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const auto& u = units_[(size_t)unit_index];
    const int b = (int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38);
    return (int16_t)(b + delta);
}

// sub_1A452/1A552 + sub_1A1F8 rect line collection: from (x,y) along the facing for range
// tiles with the given width, add the hostile occupant of each tile to F0648
// (= battle_target_list_) without duplicates. last_x/last_y hold the last tile returned
// (1A552 leaves its outputs untouched on false, so the original local variable keeps the
// last tile - the start of the second ray in 2F80B).
int CombatII::battle_weapon_collect_rect(int unit_index, int x, int y, int count, int range,
                                         int* last_x, int* last_y) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    const g2range::Unit au = battle_range_unit(unit_index);
    rg.rect_init(x, y, count, au.facing, range);                  // sub_1A452
    int px = x, py = y;
    if (last_x) *last_x = x;
    if (last_y) *last_y = y;
    for (int guard = 0; guard < 0x10000 && rg.rect_next(px, py); ++guard) { // sub_1A552
        rg.add_by_army(px, py, au.army);                          // sub_1A1F8
        if (last_x) *last_x = px;
        if (last_y) *last_y = py;
    }
    battle_target_list_ = rg.list;
    return (int)battle_target_list_.size();
}

// sub_1AC08/1AC59 + sub_1A1F8 diamond collection around (x,y,r). Rebuilds F0648.
int CombatII::battle_weapon_collect_dia_at(int unit_index, int x, int y, int r) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    const g2range::Unit au = battle_range_unit(unit_index);
    rg.dia_init(x, y, r);                                         // sub_1AC08
    int px = 0, py = 0;
    for (int guard = 0; guard < 0x10000 && rg.dia_next(px, py); ++guard) // sub_1AC59
        rg.add_by_army(px, py, au.army);                          // sub_1A1F8
    battle_target_list_ = rg.list;
    return (int)battle_target_list_.size();
}

// sub_1A1DE+1A146: the attacker's front tile.
void CombatII::battle_weapon_front_tile(int unit_index, int& fx, int& fy) {
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    const g2range::Unit au = battle_range_unit(unit_index);
    const auto pt = rg.front_tile(au, au.facing);
    fx = pt.x; fy = pt.y;
}

// sub_2E896(eax=target, edx=facing): knockback. Does nothing when [61h] >= 0x12C or
//   [5Ch] != 0.
//   Sets [64h] = 0Ah, builds the 169D7 selector grid (impassable/occupied -2), then walks up
//   to 3 tiles along the facing: a 13CBD result of -2 takes one tile back and stops (the
//   out-of-range -128 passes), and zero moved tiles leaves only pose 0Ah. A move records
//   occupancy through 11EE4/11E04 and sets the [65h]/[66h] pixel offsets
//   (facing 0: [66h] = -16n, 1: [65h] = 24n, 2: [66h] = +16n, 3: [65h] = -24n), then slides
//   over 9 frames (45559) shrinking each by *0x46/100 (truncating toward 0) under the
//   sub_1033B(7) timer, and ends with 12124 + 45559.
void CombatII::battle_knockback_2e896(int target_index, int dir) {
    if (target_index < 0 || (size_t)target_index >= units_.size()) return;
    auto& T = units_[(size_t)target_index];
    if (T.sprite_type >= 0x12C) return;
    if (T.code_b != 0) return;
    T.anim_frame = 0x0A;
    std::vector<int> grid;
    battle_ai_169d7_reset(target_index, grid);                   // sub_169D7
    dir &= 3;
    static constexpr int kStepX[4] = { 0, -1, 0, 1 };           // sub_49ABC 0=S 1=W 2=N 3=E
    static constexpr int kStepY[4] = { 1, 0, -1, 0 };
    int cx = (int)(int8_t)T.x, cy = (int)(int8_t)T.y;
    int count = 0;
    for (int si = 0; si < 3; ++si) {
        cx += kStepX[dir]; cy += kStepY[dir];
        if (battle_ai_wc5_diff_read(grid, cx, cy) == -2) {      // sub_13CBD
            const int back = (dir + 2) & 3;
            cx += kStepX[back]; cy += kStepY[back];
            break;
        }
        ++count;
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: knockback 2E896 target=%d dir=%d steps=%d\n", T.id, dir, count);
    if (count == 0) return;
    T.x = (decltype(T.x))cx; T.y = (decltype(T.y))cy;           // 11EE4 -> [0]/[1] -> 11E04
    switch (dir) {
        case 0: T.walk_off_y = -(count * 16); break;
        case 1: T.walk_off_x = count * 24; break;
        case 2: T.walk_off_y = count * 16; break;
        default: T.walk_off_x = -(count * 24); break;
    }
    game_timer_set(7);                                           // sub_1033B(7)
    for (int f = 0; f < 9 && !should_exit_ && !skip_cutscene_; ++f) {
        present_cutscene_frame();                                // sub_45559
        auto& U = units_[(size_t)target_index];
        U.walk_off_x = U.walk_off_x * 0x46 / 100;                // imul 46h; idiv 64h (truncating)
        U.walk_off_y = U.walk_off_y * 0x46 / 100;
        wait_game_tick();                                        // sub_499E4
    }
    reset_unit_pose(units_[(size_t)target_index]);               // sub_12124
    present_cutscene_frame();                                    // sub_45559
}

// sub_2ED9A(eax=attacker, edx=radius): automatic centre selection. Walks the 1AF22 triangle
// iterator (cx=1, facing, radius) from the front tile, discarding the first step's result,
// and returns the position of the hostile with the strictly lowest per-[6Ch] score.
//   [6Ch] 0/1/5/6: 49ADF distance; 2: [48h]/100; 3: 0BB8-(12C-b)*sub_17531 with the
//   facing-adjusted defence b; 4: 0C8-[attacker+43h]; > 6 is skipped.
//   Compared as signed 16-bit (cmp ax,cx; jge skip) starting from 7FFF.
bool CombatII::battle_weapon_2ed9a_pick(int unit_index, int radius, int& out_x, int& out_y) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const auto& A = units_[(size_t)unit_index];
    int fx = 0, fy = 0;
    battle_weapon_front_tile(unit_index, fx, fy);               // sub_1A1DE
    const int ax_eff = battle_effective_army(A);
    const int di = battle_damage_class(A);                       // sub_17531 (ecx=1 preserved)
    Wc5Iter it;
    battle_ai_wc5_iter_init(it, fx, fy, 1, A.facing & 3, (int16_t)radius); // sub_1AF22
    int px = 0, py = 0;
    battle_ai_wc5_iter_next(it, px, py);                         // the first 1B00D result is discarded
    int best16 = 0x7FFF, best = -1;
    int guard = 0;
    while (guard++ < 0x10000 && battle_ai_wc5_iter_next(it, px, py)) {
        const int oi = battle_unit_index_at(px, py);             // sub_11DB2
        if (oi < 0) continue;
        const auto& o = units_[(size_t)oi];
        if (!battle_armies_hostile(ax_eff, battle_effective_army(o), battle_hostility_inverted_)) continue;
        const int state = A.ai_decision_state;
        if (state > 6) continue;
        int score;
        switch (state) {
            case 2: score = battle_hp_raw(o) / 100; break;
            case 3: {
                int b;
                const int def = (int16_t)o.combat_value_46;
                if ((o.facing & 0xFF) == (A.facing & 0xFF)) b = def >> 1;
                else if ((((o.facing & 3) + (A.facing & 3)) & 1) != 0) b = def - (def >> 2);
                else b = def;
                score = 0xBB8 - (0x12C - b) * di;
                break;
            }
            case 4: score = 0xC8 - (int)A.combat_value_43; break;
            default: {
                const int d = std::abs((int)(int8_t)o.x - (int)(int8_t)A.x) + std::abs((int)(int8_t)o.y - (int)(int8_t)A.y);
                score = d; // sub_49ADF (Manhattan)
                break;
            }
        }
        const int s16v = (int16_t)score;
        if (s16v < best16) { best16 = s16v; best = oi; }
    }
    if (best < 0) return false;
    out_x = (int)(int8_t)units_[(size_t)best].x;
    out_y = (int)(int8_t)units_[(size_t)best].y;
    return true;
}

// sub_2EF4A(eax=attacker, edx=di, ebx=type(dx 1..3), ecx=add): diamond area damage around
//   the automatic centre (2ED9A).
//   Radius = 2CBFB(1AEF3) (5/7). Collects the hostiles in the diamond of radius
//   (type == 3 && di ? 1 : 0) around the centre, then:
//   di == 0 (idx15): for type 3, type becomes 2 when the target level >= attacker level;
//     base = 1D770(hit_kind = type-1) + add.
//   di != 0 (idx16): base = 1D770(0,0), then add+base for type 1 or add+2*base otherwise,
//     corrected by 2CACA.
//   Every listed unit takes the same damage. Always 1 (or -1 when there is no centre).
int CombatII::battle_weapon_2ef4a(int unit_index, int di, int type, int add) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& A = units_[(size_t)unit_index];
    const int saved_pose = A.anim_frame;
    const int radius = battle_weapon_2cbfb(unit_index,
        battle_ai_wc5_aef3(A.combat_class)); // 2CBFB(unit, 1AEF3)
    int cx = 0, cy = 0;
    if (battle_f14d8_ == 1 || battle_pick_tx_ < 0) {
        if (!battle_weapon_2ed9a_pick(unit_index, radius, cx, cy)) {
            if (battle_trace_) fprintf(stderr, "COMBAT: 2ef4a unit=%d no center (radius=%d) -> -1\n",
                                        A.id, radius);
            return -1;
        }
    } else {
        cx = battle_pick_tx_; cy = battle_pick_ty_;          // 1F19C player centre
    }
    battle_atk_tx_ = cx;                                     // sub_2ED9A/1F19C -> dword_FA8B0+2
    battle_atk_ty_ = cy;                                     // word_FA8B4
    const int r = (type == 3 && di != 0) ? 1 : 0;
    const int n = battle_weapon_collect_dia_at(unit_index, cx, cy, r);
    const std::vector<int> targets = battle_target_list_;
    if (battle_trace_) fprintf(stderr, "COMBAT: 2ef4a unit=%d di=%d type=%d center=(%d,%d) r=%d targets=%d\n",
                                A.id, di, type, cx, cy, r, n);

    // ASM 2EF4A loc_2EFC0: automatic path only, 1F179 == 0 (class 15h..18h) runs
    // 1033B(20h), pose 5, 45559, 499E4, pose 6, 45559 before the collection effect.
    if (battle_f14d8_ == 1 && A.combat_class >= 0x15 && A.combat_class <= 0x18) {
        game_timer_set(0x20);
        A.anim_frame = 5;
        present_cutscene_frame();                            // sub_45559
        wait_game_tick();                                    // sub_499E4
        A.anim_frame = 6;
        present_cutscene_frame();
    }

    // loc_2F0BC: per-target 1214F -> 2CBA1 -> 44D6E(1) -> 46DF1 -> 4F4CD(3) -> 46E27.
    for (int ti : targets) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        T.anim_frame = (T.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00; // sub_1214F
    }
    battle_highlight_add_depth(unit_index);                   // sub_2CBA1 (42840)
    battle_dim_row0_ = true;                                  // sub_44D6E(1)
    camera_center_on((int)A.x + ((cx - (int)A.x) >> 1),
                     (int)A.y + ((cy - (int)A.y) >> 1));      // sub_46DF1 midpoint
    sfx(3);                                                    // sub_4F4CD(3)
    play_weapon_class5_projectile_at(A, cx, cy, true, kSub46E27StackVar8); // sub_46E27 (42854, edx=1)

    int tmp = 0;
    int flash_mode = 0;                                       // sub_1E565(var_10 - 1)
    if (di == 0) {
        if (n > 0) {
            const int t0 = targets[0];
            int ty = type;
            if (ty == 3 && units_[(size_t)t0].combat_level_2b >= A.combat_level_2b) ty = 2;
            tmp = battle_damage(unit_index, t0, ty - 1, 0, 0) + add; // 1D770(hit_kind = ty-1)
            if (ty > 1) flash_mode = ty - 1;
        }
        // loc_2F2C1: 44D75 -> 44D6E(0) -> 1E565(type-1) when type > 1.
        battle_highlight_clear();                             // sub_44D75
        battle_dim_row0_ = false;                             // sub_44D6E(0)
        if (flash_mode) battle_fx_1e565(flash_mode);          // sub_1E565
    } else {
        if (n > 0) {
            const int t0 = targets[0];
            const int base = battle_damage(unit_index, t0, 0, 0, 0); // 1D770(0,0)
            tmp = add + (type == 1 ? base : 2 * base);
            tmp = battle_weapon_2caca(unit_index, tmp);       // sub_2CACA
        }
        // loc_2F199: type 2 -> 4 frames, type 3 -> 0Fh frames, type 1 -> 1 frame.
        const int count = (type == 2) ? 4 : (type == 3 ? 0x0F : 1);
        game_timer_set(2);                                    // sub_1033B(2)
        for (int i = 0; i < count && !should_exit_; ++i) {
            A.anim_frame = 5;                                 // pose 5
            present_cutscene_frame();                         // sub_45559
            A.anim_frame = 6;                                 // pose 6
            present_cutscene_frame();
            // loc_2F261: 4F4CD(3) -> 46E27(unit, edi != 0) -> facing nudge on [65h]/[66h].
            // loc_2F1D6: on the 0Fh-frame type-3 finisher each frame rerolls the
            // centre target tile: 499BD(2) selects the axis and 499BD(3)-1 the -1..1
            // offset. 11DB2/1201D keep the occupant only when it is hostile, and
            // 46E19 records the jittered tile even when it is empty.
            int nudge_ti = targets.empty() ? -1 : targets[0];
            if (type == 3 && !targets.empty()) {
                const auto& T0 = units_[(size_t)targets[0]];
                int jx = (int)(int8_t)T0.x, jy = (int)(int8_t)T0.y;
                if (combat_rand(2) != 0) jx += combat_rand(3) - 1;
                else jy += combat_rand(3) - 1;
                battle_atk_tx_ = jx;                         // sub_46E19
                battle_atk_ty_ = jy;
                nudge_ti = battle_unit_index_at(jx, jy);      // sub_11DB2
                if (nudge_ti >= 0 && (size_t)nudge_ti < units_.size()) {
                    const auto& O = units_[(size_t)nudge_ti];
                    if (!O.active || O.hp <= 0 ||
                        !battle_armies_hostile(battle_effective_army(A),
                                               battle_effective_army(O),
                                               battle_hostility_inverted_))
                        nudge_ti = -1;                       // sub_1201D != 1 -> edi = 0
                } else {
                    nudge_ti = -1;
                }
            }
            sfx(3);                                           // sub_4F4CD(ebx=3)
            // loc_2F261: 46E27 re-runs on every frame. Type 3 uses the jittered
            // FA8B0/FA8B4 tile; types 1/2 keep the selected centre. edx is 1 only
            // when edi (the first target or the jittered hostile) is present.
            const int ptx = (type == 3) ? battle_atk_tx_ : cx;
            const int pty = (type == 3) ? battle_atk_ty_ : cy;
            play_weapon_class5_projectile_at(A, ptx, pty, nudge_ti >= 0,
                                             kSub46E27StackVar8); // sub_46E27
            if (nudge_ti >= 0 && (size_t)nudge_ti < units_.size()) {
                auto& T = units_[(size_t)nudge_ti];
                switch (A.facing & 3) {                       // off_2EF3A
                    case 0: T.walk_off_y -= 1; break;
                    case 1: T.walk_off_y += 1; break;
                    case 2: T.walk_off_x -= 1; break;
                    default: T.walk_off_x += 1; break;
                }
            }
            wait_game_tick();                                 // sub_499E4
        }
        battle_highlight_clear();                             // sub_44D75
        battle_dim_row0_ = false;                             // sub_44D6E(0)
    }

    std::vector<BattleDamageFx> fx;
    fx.reserve(targets.size());
    if (n > 0) {
        for (size_t slot = 0; slot < targets.size(); ++slot) {
            const int ti = targets[slot];
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            auto& T = units_[(size_t)ti];
            if (!T.active || T.hp <= 0) continue;
            battle_damage_fx_prepare(unit_index, ti, (int)slot, tmp, fx); // 45BFB + [48h] -= tmp + 45ED2
        }
    }
    battle_damage_fx_play(fx, targets);                            // sub_287AF + 12124
    battle_damage_fx_triggers(targets, unit_index);                // sub_1ED45(caster)
    if ((size_t)unit_index < units_.size())
        units_[(size_t)unit_index].anim_frame = (uint8_t)saved_pose;
    return 1;
}

// F1474 idx 13..24 (sub_2F80B/2F98A/2FA1E/2FA41/2FA6E/2FD03/2FED7/2FF36/2FF4B/2FFF9/30049).
//   add comes from COMBATII.EXE object 2: word 744AE=4000(idx13), 744B0=6000(idx14),
//   744B2=1000/744B4=2000 (idx15/16 add), 744B8=3000(idx17). idx 19/20/21 are handled by the
//   0/8, 4/12 and 3/9 branches above.
int CombatII::battle_weapon_f1474_ext(int unit_index, int idx, int dx) {
    auto& u = units_[(size_t)unit_index];
    switch (idx) {
        case 13: { // sub_2F80B: front line damage, R=2CBFB(5+(dx>1)) (add 4000, mul dx);
                   //   dx > 2 fires a second line from the last tile
            const int R = battle_weapon_2cbfb(unit_index, 5 + (dx > 1 ? 1 : 0));
            int fx = 0, fy = 0;
            battle_weapon_front_tile(unit_index, fx, fy);
            int lx = fx, ly = fy;
            int n = battle_weapon_collect_rect(unit_index, fx, fy, 1, R, &lx, &ly);
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[13]=2F80B unit=%d dx=%d R=%d targets=%d\n", u.id, dx, R, n);
            // 2F80B: 2CBA1 -> 44D6E(1) -> 2DD7A(axefly.nsp, edx = dx>1 ? 1 : 0) -> 44D6E(0) ->
            // 44D75.
            battle_highlight_add_depth(unit_index);                  // sub_2CBA1
            battle_dim_row0_ = true;                                 // sub_44D6E(1)
            battle_fx_2dd7a(unit_index, 0, 0, dx > 1 ? 1 : 0);       // sub_2DD7A
            battle_dim_row0_ = false;                                // sub_44D6E(0)
            battle_highlight_clear();                                // sub_44D75
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            if (n > 0) battle_weapon_2caf7_loop(unit_index, dx, 4000);
            if (should_exit_ || (size_t)unit_index >= units_.size() || !units_[(size_t)unit_index].active) return 1;
            if (dx > 2) {
                n = battle_weapon_collect_rect(unit_index, lx, ly, 1, R, nullptr, nullptr);
                if (n > 0) battle_weapon_2caf7_loop(unit_index, dx, 4000);
            }
            // The 2DD7A tail (ASM 41367) restores the pose with sub_12124 even with no target.
            if (!should_exit_ && (size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
            return 1;
        }
        case 14: { // sub_2F98A: radius-1 diamond around the caster -> 2CBA1 -> 44D6E(1) ->
                   //   2DFC1(unit, dx-1) -> 44D6E(0) -> 44D75 -> 2CAF7(mul dx+2, add 6000). Always 1.
            const int n = battle_weapon_collect_dia_at(unit_index, (int)(int8_t)u.x, (int)(int8_t)u.y, 1);
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[14]=2F98A unit=%d dx=%d targets=%d\n", u.id, dx, n);
            battle_highlight_add_depth(unit_index);                  // sub_2CBA1
            battle_dim_row0_ = true;                                 // sub_44D6E(1)
            battle_fx_2dfc1(unit_index, dx - 1);                     // sub_2DFC1(madaxe.nsp)
            battle_dim_row0_ = false;                                // sub_44D6E(0)
            battle_highlight_clear();                                // sub_44D75
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            if (n > 0) battle_weapon_2caf7_loop(unit_index, (int16_t)(dx + 2), 6000);
            else {
                run_unit_hp_trigger(unit_index, -1, 1);
                reset_unit_pose(units_[(size_t)unit_index]);
            }
            return 1;
        }
        case 15: { // sub_2FA1E: 2D684(critical.nsp) -> 2EF4A(di=0, add 1000)
            battle_fx_2d684(unit_index);
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            return battle_weapon_2ef4a(unit_index, 0, dx, 1000);
        }
        case 16: return battle_weapon_2ef4a(unit_index, 1, dx, 2000); // sub_2FA41 (2EF4A di=1)
        case 17: { // sub_2FA6E: t_arrow line of R=2CBFB(9); automatic returns -1 with no target.
                   //   Presentation: 2CBA1 depth highlight -> 44D6E(1) -> t_arrow.nsp +
                   //   bolt2.ssp -> poses 7..9 -> flight loop -> 2CAF7(mul dx, add 3000) -> 12124.
                   //   The ASM's final 29437 call passes eax=4. sub_29437 forwards eax to
                   //   sub_28947, which reads [eax+2Bh], so eax is a BattleUnit*, not an
                   //   effect id or unit index. 4 is a low DGROUP offset; 2CAF7/287AF/1ED45
                   //   do not clear F0648, so the original performs a garbage-pointer status
                   //   roll rather than a no-op. The port does not reproduce that UB.
            const int R = battle_weapon_2cbfb(unit_index, 9);
            // sub_200DE(unit, 5, &var_14): centre the camera five tiles ahead.
            {
                int ccx = (int)(int8_t)u.x, ccy = (int)(int8_t)u.y;
                switch (u.facing & 3) {
                    case 0: ccy += 5; break;
                    case 1: ccx -= 5; break;
                    case 2: ccy -= 5; break;
                    default: ccx += 5; break;
                }
                camera_center_on(ccx, ccy);                     // sub_200DE(unit, 5)
            }
            int fx = 0, fy = 0, last_x = 0, last_y = 0;
            battle_weapon_front_tile(unit_index, fx, fy);
            const int n = battle_weapon_collect_rect(unit_index, fx, fy, 1, R, &last_x, &last_y);
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[17]=2FA6E unit=%d dx=%d R=%d targets=%d last=(%d,%d)\n",
                                       u.id, dx, R, n, last_x, last_y);
            // sub_2FA6E: only `word_F14D8 == 1 && count == 0` returns -1. On the player path
            // (F14D8 != 1) the presentation/journal runs even with an empty target list and the
            // routine still returns 1.
            if (battle_f14d8_ == 1 && n <= 0) return -1;

            battle_highlight_add_depth(unit_index);          // sub_2CBA1
            battle_dim_row0_ = true;                         // sub_44D6E(1)
            const bool nsp_ok = battle_nsp_load("t_arrow.nsp"); // sub_1FEA4
            battle_ssp_play("bolt2.ssp");                    // sub_4F460
            // off_2FA5E: 0 -> +4x, 1 -> -4y, 2 -> -4x, 3 -> +4y.
            int step_x = 0, step_y = 0;
            switch (u.facing & 3) {
                case 0: step_x = 4; break;
                case 1: step_y = -4; break;
                case 2: step_x = -4; break;
                default: step_y = 4; break;
            }
            // sub_49ADF(caster, last ray tile) - 1, scaled by 6 for E/W and 4 for N/S facing.
            // At 4 px/frame this is 24 px per E/W tile and 16 px per N/S tile.
            int frames = (std::abs((int)u.x - last_x) + std::abs((int)u.y - last_y) - 1) *
                         ((u.facing & 1) ? 6 : 4);
            if (frames < 0) frames = 0;

            // ASM 43738..43780: 1033B(20h), then poses 7,8,9 each presented once.
            u.anim_frame = 7;                                // [64h]=7
            game_timer_set(0x20);                            // sub_1033B(20h)
            for (int i = 0; i < 3 && !should_exit_; ++i) {
                battle_fx_present();                         // sub_45559
                ++u.anim_frame;                              // 7 -> 8 -> 9 -> 10
                wait_game_tick();                            // sub_499E4
            }
            // ASM 43780..43795: 1033B(10h), [64h]=9, 1F179 decides whether to reset.
            game_timer_set(0x10);                            // sub_1033B(10h)
            u.anim_frame = 9;
            if (u.combat_class >= 0x15 && u.combat_class <= 0x18)
                reset_unit_pose(u);                          // sub_12124 (1F179 == 0)

            // ASM 437FA..43815: anchor = 10971(caster x)+0Ch, 10988(caster y)-2.
            int px = ((int)u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0C;
            int py = ((int)u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 2;
            game_timer_set(3);                               // sub_1033B(3)
            sfx(0x32);                                       // sub_4F4CD(32h)
            const int facing_base = (u.facing & 3) * 5;      // [unit+60h]>>24 == [63h] facing
            for (int i = 0; i < frames && !should_exit_; ++i) {
                const int frame = facing_base + (i % 5);
                if (nsp_ok && frame < (int)magic_frames_.size()) {
                    const MagicFxFrame& fr = magic_frames_[(size_t)frame];
                    battle_fx_blit(frame, px + fr.xo, py + fr.yo); // sub_1FFFE
                }
                battle_fx_present();                         // 454B3 + 5E9E8
                wait_game_tick();                            // sub_499E4
                px += step_x;                                // var_C / var_10
                py += step_y;
            }
            battle_highlight_clear();                        // sub_44D75
            battle_dim_row0_ = false;                        // sub_44D6E(0)
            if (should_exit_) { battle_nsp_release(); return 1; }
            battle_weapon_2caf7_loop(unit_index, dx, 3000);  // sub_2CAF7
            if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]); // 12124
            battle_nsp_release();                            // sub_1FFCD
            return 1;
        }
        case 18: { // sub_2FD03: with F14D8 == 1 (AI/script) there is no target (-1). The player
            // Picks one with the 1F19C cursor, then 2CAF7(mul=dx, add=[744B6]=4000) -> 12124.
            // battle_use_weapon passes the 4A483 cursor tile in battle_pick_tx_/ty_ and
            // this case moves that unit to the head of F0648 (the other candidates stay as the
            // 12x12 screen collection).
            if (battle_f14d8_ == 1) {
                if (battle_trace_) fprintf(stderr, "COMBAT: f1474[18]=2FD03 unit=%d dx=%d auto(no target) -> -1\n", u.id, dx);
                return -1;
            }
            const int n = battle_weapon_collect_screen(unit_index, 0);
            // Put the 1F19C cursor target first (2CAF7 mul=dx, add=4000).
            battle_weapon_apply_pick(unit_index);
            if (battle_trace_)
                fprintf(stderr, "COMBAT: f1474[18]=2FD03 unit=%d dx=%d player cursor tile=(%d,%d) targets=%d\n",
                        u.id, dx, battle_pick_tx_, battle_pick_ty_, n);
            if (battle_target_list_.empty()) return -1;
            // ASM 43922..43944: 1124A(caster tile) -> 1033B(18h) -> 1F179 branch.
            camera_center_on((int)u.x, (int)u.y);            // sub_1124A
            game_timer_set(0x18);                            // sub_1033B(18h)
            // sub_1F179 returns 0 for [unit+11h] in 15h..18h: two waits, pose 0, one present.
            if (u.combat_class >= 0x15 && u.combat_class <= 0x18) {
                wait_game_tick();                            // sub_499E4 x2
                wait_game_tick();
                u.anim_frame = 0;
                battle_fx_present();                         // sub_45559
            } else {
                for (int f = 7; f < 0x0A && !should_exit_; ++f) { // poses 7,8,9
                    u.anim_frame = (uint8_t)f;
                    battle_fx_present();                     // sub_45559
                    wait_game_tick();                        // sub_499E4
                }
            }
            if (should_exit_) return 1;
            sfx(3);                                          // sub_4F4CD(3)
            // ASM 43948~44016: 49A3E master -> white (step 0Ah), camera clamp, 49A3E white ->
            // master (step 14h), 5F9AE(master).
            battle_fx_white_flash_up(0x0A);
            int cam_x = battle_atk_tx_ - 6;                   // 43965..43983
            if (cam_x < 0) cam_x = 0;
            else if (cam_x > (int)map_w_ - 12) cam_x = (int)map_w_ - 12;
            int cam_y = battle_atk_ty_ - 6;                   // 43986..44004
            if (cam_y < 0) cam_y = 0;
            else if (cam_y > (int)map_h_ - 12) cam_y = (int)map_h_ - 12;
            cam_tile_x_ = cam_x;
            cam_tile_y_ = cam_y;
            battle_fx_white_flash_down(0x14);
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            // ASM 44017~44022: ebx=[esp+2FEh]>>16 = dx(mul), edx=unk_744B4>>16 = [744B6]=4000(add).
            battle_weapon_2caf7_loop(unit_index, dx, 4000);
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            reset_unit_pose(units_[(size_t)unit_index]);         // sub_12124
            return 1;
        }
        case 22: // sub_2FF4B: fistlv4 presentation then [13h] |= 80h, always 1
            u.combat_flags_13 = (uint8_t)(u.combat_flags_13 | 0x80);
            if (battle_trace_) fprintf(stderr, "COMBAT: f1474[22]=2FF4B unit=%d -> [13h]=%02X (return 1)\n",
                                        u.id, (unsigned)u.combat_flags_13);
            return 1;
        case 23: { // sub_2FFF9: 1D124 gate -> 2F3B2(0) presentation -> 2E896 knockback on the
                   //   first target -> 12124, always 1
            if (battle_range_collect(unit_index) <= 0 || battle_target_list_.empty()) return -1;
            const int ti = battle_target_list_[0];
            battle_fx_2f3b2(unit_index, 0);               // sub_2F3B2(edx=0 -> fistlv5.nsp)
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            battle_knockback_2e896(ti, units_[(size_t)unit_index].facing & 3);
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            reset_unit_pose(units_[(size_t)unit_index]);
            return 1;
        }
        case 24: { // sub_30049: 1D124 gate -> 2F3B2(1) presentation -> 2CAF7(mul dx, add 7D00)
                   //   -> 12124, always 1
            if (battle_range_collect(unit_index) <= 0 || battle_target_list_.empty()) return -1;
            battle_fx_2f3b2(unit_index, 1);               // sub_2F3B2(edx=1 -> fistlv6.nsp)
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            battle_weapon_2caf7_loop(unit_index, dx, 0x7D00);
            if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
            reset_unit_pose(units_[(size_t)unit_index]);
            return 1;
        }
        default: break;
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: f1474[%d] unit=%d dx=%d unported (-1)\n", idx, u.id, dx);
    return -1;
}

void CombatII::battle_weapon_apply_status(int target_index, int bit, int duration) {
    if (target_index < 0 || (size_t)target_index >= units_.size()) return;
    auto& t = units_[(size_t)target_index];
    t.combat_flags_1e = (uint8_t)(t.combat_flags_1e | (uint8_t)bit); // or [1Eh],bit
    t.ail_1f = (uint16_t)duration; // mov [1Fh],dur (word)
}

// ---- batched damage presentation shared by sub_2CAF7/2CC3A/2EF4A/34C48 ----
// sub_45BFB prepares one target's number and start position, subtracts the raw damage from
// [+48h] and sets the 45ED2 knockback. The whole list is prepared before sub_287AF animates
// it in parallel (45F29/45CCB), so every caller uses this same entry point.
void CombatII::battle_damage_fx_prepare(int caster_index, int ti, int slot, int fin,
                                        std::vector<BattleDamageFx>& fx) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    if (ti < 0 || (size_t)ti >= units_.size()) return;
    auto& A = units_[(size_t)caster_index];
    auto& T = units_[(size_t)ti];
    if (!T.active || T.hp <= 0) return;
    const int old_raw = battle_hp_raw(T);
    int new_raw = old_raw - fin;
    if (new_raw > battle_max_hp_raw(T)) new_raw = battle_max_hp_raw(T);
    const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
    if (battle_trace_) fprintf(stderr, "COMBAT: damage fx unit=%d -> target=%d final=%d hp_raw %d->%d\n",
                                A.id, T.id, fin, old_raw, new_raw);
    BattleDamageFx d{};
    d.unit = ti;
    d.slot = slot;
    d.sx0 = (T.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int sy0 = (T.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    for (int k = 0; k < 5; k++) d.digit_y[k] = sy0 + 10;   // word_F9708
    // sub_45BFB: shown = fin / 100 (idiv 64h), the display damage, not the HP difference.
    // No value is hidden: above 999 the width index is 0 and the same 4-cell window is
    // filled left to right, and fin <= 0 leaves the buffer empty for the "miss" branch.
    const int shown = fin / 100;
    if (fin > 0) {
        d.ndig = snprintf(d.digits, sizeof(d.digits), "%d", shown);
        if (d.ndig > 4) d.ndig = 4;          // 45CCB reads cells 0..3 only
    }
    static constexpr uint8_t kDmgColor[8] = {0x04, 0x10, 0x31, 0x51, 0x20, 0x01, 0x00, 0x00};
    d.color = kDmgColor[std::min(std::max(battle_effective_army(T), 0), 7)];
    T.walk_off_x = T.walk_off_y = 0;                       // sub_45BFB prologue
    T.hp = new_hp;
    T.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0; // exact [+48h] after the subtract
    setup_knockback_hit(T, A.facing & 3);                  // sub_45ED2
    fx.push_back(d);
}

// sub_287AF: period 3, 24 frames with every prepared target animated together (45F29/45CCB),
// then sub_12124 for all targets and sub_499DA(0x24).
void CombatII::battle_damage_fx_play(std::vector<BattleDamageFx>& fx,
                                     const std::vector<int>& targets) {
    if (!small_font_tried_) load_small_font();
    const bool damage_small_font = small_font_loaded_;
    game_timer_set(3);
    for (int f = 1; f <= 0x18 && !should_exit_ && !skip_cutscene_; ++f) {
        for (BattleDamageFx& d : fx) {
            if (d.unit < 0 || (size_t)d.unit >= units_.size()) continue;
            auto& U = units_[(size_t)d.unit];
            apply_knockback_hit(U, d.slot, f);                 // sub_45F29(U, slot, frame)
            // sub_45CCB (loc_45D15..loc_45D66): only cell 0 (f<5), cell 3 (f>=14h) or a
            // pair of adjacent cells moves; the 24-frame wave travels through the digits.
            if (d.ndig <= 0) {
                // loc_45E21: "miss" alternates between y[1] and y[0] - (14h-frame)/2.
                if (f & 1) d.digit_y[0] = d.digit_y[1];
                else d.digit_y[0] -= (0x14 - f) / 2;
            } else if (f < 5) {
                d.digit_y[0] -= 2;                             // sub word_F9708[ebx],2
            } else if (f >= 0x14) {
                d.digit_y[3] += 2;                             // add unk_F970E[ebx],2
            } else {
                const int k = (f - 5) / 5;                     // 0,1,2
                d.digit_y[k] += 2;                             // add dx,2
                d.digit_y[k + 1] -= 2;                         // sub cx,2
            }
        }
        render_battle_frame();
        uint8_t* bb = video_.get_backbuffer();
        for (const BattleDamageFx& d : fx) {
            // loc_45D6E/loc_45E4D: one cull for the whole number (x-10h, y[0]-5).
            if (d.sx0 - 16 < 0 || d.sx0 - 16 >= 0x120) continue;
            const int y0 = d.digit_y[0];
            if (y0 - 5 < 0 || y0 - 5 >= 0xB2) continue;
            if (d.ndig <= 0) {
                // loc_45E78: an empty buffer shows "miss" (shadow at +1,+1).
                if (damage_small_font) {
                    draw_small_text(d.sx0 + 1, y0 + 1, "miss", 0, 1);
                    draw_small_text(d.sx0, y0, "miss", d.color, 1);
                } else if (bb && font_.is_loaded()) {
                    font_.set_text_mode(4, 0x0000, 0xFF);
                    font_.draw_string(bb, SCREEN_W, SCREEN_H, d.sx0 + 1, y0 + 1, "miss");
                    font_.set_text_mode(4, (uint16_t)(d.color << 8), 0xFF);
                    font_.draw_string(bb, SCREEN_W, SCREEN_H, d.sx0, y0, "miss");
                }
                continue;
            }
            // loc_45DA2: four 6-px cells at sx0 + 6*i; the digits are right-aligned into
            // cells 0..3 (width index 4-ndig in sub_45BFB).
            const int first = 4 - d.ndig;
            char one[2] = {0, 0};
            for (int k = 0; k < d.ndig; k++) {
                one[0] = d.digits[k];
                const int x = d.sx0 + 6 * (first + k), y = d.digit_y[first + k];
                if (damage_small_font) {
                    draw_small_text(x + 1, y + 1, one, 0, 1);
                    draw_small_text(x, y, one, d.color, 1);
                } else if (bb && font_.is_loaded()) {
                    font_.set_text_mode(4, 0x0000, 0xFF);
                    font_.draw_string(bb, SCREEN_W, SCREEN_H, x + 1, y + 1, one);
                    font_.set_text_mode(4, (uint16_t)(d.color << 8), 0xFF);
                    font_.draw_string(bb, SCREEN_W, SCREEN_H, x, y, one);
                }
            }
        }
        render_overlay();
        video_.present();
        wait_game_tick();                                      // sub_499E4
    }
    if (!should_exit_) {
        for (int ti : targets) {                               // 12124 loop of sub_287AF
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            reset_unit_pose(units_[(size_t)ti]);
        }
    }
    game_timer_set(0x24);                                      // sub_499DA(0x24)
    wait_game_tick();                                          // 499DA falls through to 499E4
}

// sub_1ED45(caster): a dead target runs sub_1DB1D and leaves F0648, a survivor gets
// sub_53797(victim, caster, 1).
void CombatII::battle_damage_fx_triggers(const std::vector<int>& targets, int caster_index) {
    if (should_exit_) return;
    for (size_t slot = 0; slot < targets.size(); ++slot) {
        const int ti = targets[slot];
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        if (units_[(size_t)ti].hp <= 0) {
            play_unit_death(ti, caster_index);                 // sub_1DB1D
            if (should_exit_) break;
            if (slot < battle_target_list_.size()) battle_target_list_[slot] = -1;
        } else {
            run_unit_hp_trigger(ti, caster_index, 1);          // sub_53797
            if (should_exit_) break;
        }
    }
}

// sub_2CAF7 gameplay core (eax=unit, ebx=mul, dx=add).
//   [44h] = orig*mul (low 16 bits) -> per target sub_1D770(0,0) -> dmg = add+base ->
//   45BFB (damage number) / HP -= dmg / 45ED2 (knockback). The whole list is prepared before
//   any animation: sub_287AF then runs 24 frames at period 3 with every target animated in
//   parallel (45F29/45CCB), stands all of them up (12124) and restores the period (499DA 24h).
//   sub_1ED45(caster) follows: a dead target runs sub_1DB1D and leaves F0648, a survivor gets
//   sub_53797(victim, caster, 1). The caller owns the caster's pose.
int CombatII::battle_weapon_2caf7_loop(int unit_index, int mul, int add) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (battle_target_list_.empty()) return -1;
    const std::vector<int> targets = battle_target_list_;
    auto& A = units_[(size_t)unit_index];
    const int16_t orig44 = A.combat_value_44;
    const int32_t prod = (int32_t)orig44 * (int32_t)mul;
    A.combat_value_44 = (int16_t)(prod & 0xFFFF); // imul + mov [44h],ax
    if (battle_trace_) fprintf(stderr, "COMBAT: 2caf7 unit=%d mul=%d add=%d orig44=%d temp44=%d targets=%zu\n",
                                A.id, mul, add, (int)orig44, (int)A.combat_value_44, targets.size());

    // sub_45BFB/45ED2 prepare every target before sub_287AF animates the whole list at once.
    std::vector<BattleDamageFx> fx;
    fx.reserve(targets.size());
    for (size_t slot = 0; slot < targets.size(); ++slot) {
        const int ti = targets[slot];
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        if (!T.active || T.hp <= 0) continue;
        const int base = battle_damage(unit_index, ti, 0, 0, 0); // sub_1D770(ebx=0, ecx=0)
        const int fin = add + base;
        if (battle_trace_) fprintf(stderr, "COMBAT: 2caf7 unit=%d -> target=%d base=%d final=%d\n",
                                    A.id, T.id, base, fin);
        battle_damage_fx_prepare(unit_index, ti, (int)slot, fin, fx);
    }
    A.combat_value_44 = orig44; // restore [44h]

    battle_damage_fx_play(fx, targets);                        // sub_287AF
    battle_damage_fx_triggers(targets, unit_index);            // sub_1ED45(caster)
    return 1;
}

// sub_308EE (edi 0, fire.ssp+bird.nsp): front 7-tile ray + bird landing + [1E] |= 1 /
//   [1F] = 5Ah + 2CAF7(2,10000).
//   With F14D8 == 1 (AI/script) it collects targets with 2E0F0(edx=7) and uses the distance
//   to the first target (ASM 44917..44940).
//   With F14D8 == 0 (player) it scans 1A452 (width 1, length 7)/1A552 from the 1A1DE front
//   tile, collects hostiles with 1A1F8, then picks an empty tile as the landing spot with
//   the 4A483 cursor (sub_49ADF distance >= 2, no 11DB2 occupant) (ASM 44946..45040).
//   An empty collection returns -1 from 30A97. The other edi values (1/3/4/8/11) use the
//   screen sweep.
//   Common tail: after the bird.nsp presentation the caster moves that distance along its
//   facing (ASM 45320..45345, 11E04 occupancy), every target gets [1E] |= 1 / [1F] = 5Ah,
//   then 2CAF7(mul 2, add 2710h).
//   The port approximates the bird.nsp frames by distance interpolation (the original is a
//   trail of 6 packets x 4 frames).
// battle_fx_308ee_anim owns the whole gameplay and presentation of sub_308EE (a direct
// transcription of the routine).
int CombatII::battle_weapon_308ee(int unit_index) {
    return battle_fx_308ee_anim(unit_index);
}

// sub_31283 (edi 1, kang+f-blast+slash+asura2.nsp): 12x12 screen sweep + 2CAF7(10,1000).
//   The 3108E [65h] shuffle and 12124 reset are presentation/pose work, covered by
//   play_hp_change plus reset.
// battle_fx_31283_anim owns the whole gameplay and presentation of sub_31283 (a direct
// transcription of the routine).
int CombatII::battle_weapon_31283(int unit_index) {
    return battle_fx_31283_anim(unit_index);
}

// sub_32589 (edi 2, chang+slash+arcion.nsp): front adjacent single target, then instant death
// (1DB1D) after the 129-frame arcion presentation.
int CombatII::battle_weapon_32589(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int n = battle_weapon_collect_front(unit_index);
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 32589 unit=%d no front target -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    const int ti = battle_target_list_[0];
    if (battle_trace_) fprintf(stderr, "COMBAT: 32589 unit=%d -> target=%d arcion 1DB1D\n",
                                units_[(size_t)unit_index].id, units_[(size_t)ti].id);
    auto& A = units_[(size_t)unit_index];
    camera_center_on(A.x, A.y);
    A.anim_frame = 5;
    // 46914..47381 presentation: 129 arcion.nsp frames (27+15+45+6+36). The target's instant
    // death (1DB1D) happens at the tail of the presentation.
    battle_fx_32589_anim(unit_index, ti);
    if (should_exit_) return 1;
    reset_unit_pose(units_[(size_t)unit_index]);
    game_timer_set(6);
    return 1;
}

// sub_32BCB (edi 3, i-star2+slash+ice+icer.nsp): screen sweep (cap 50) + 2CAF7(4,0).
int CombatII::battle_weapon_32bcb(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int n = battle_weapon_collect_screen(unit_index, 50);
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 32bcb unit=%d empty -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    // 429-frame presentation (33C31/33E06 ice particle engine + icer.nsp); the trailing
    // 2CAF7(4,0) sits inside original frame 428, so the anim calls it and this does not.
    if (!battle_fx_32bcb_anim(unit_index)) {
        // Without the NSP asset, skip only the presentation and still apply the damage.
        return battle_weapon_2caf7_loop(unit_index, 4, 0);
    }
    return 1;
}

// sub_341C7 (edi 4, thunder1+f-blast+thunder3+boom+f_quake.nsp): screen sweep + f_quake
// presentation + 2CAF7(2,20000).
int CombatII::battle_weapon_341c7(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    // sub_341C7's inner loop increments esi before sub_1A1F8, so it tests camX+1..camX+12.
    const int n = battle_weapon_collect_screen(unit_index, 0, 1);
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 341c7 unit=%d empty -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    battle_fx_341c7_anim(unit_index);   // 44D89 registers all, 5E9D0/305A2/30824 for 10 frames, f_quake 6+7 frames
    reset_unit_pose(units_[(size_t)unit_index]); // sub_12124(caster), ASM 49658 before 2CAF7
    return battle_weapon_2caf7_loop(unit_index, 2, 0x4E20);
}

// sub_34C48 (edi 5, ju+shak2.nsp): 1A1DE front tile -> 1A452(1 wide, 3 deep) -> 1A552/1A1F8
// (ASM 496B4..496CF), shak2 presentation (106 frames) + per target base*4+6000 through 2CACA.
int CombatII::battle_weapon_34c48(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    int front_x = 0, front_y = 0;
    battle_weapon_front_tile(unit_index, front_x, front_y);              // sub_1A1DE
    const int n = battle_weapon_collect_rect(unit_index, front_x, front_y, 1, 3); // sub_1A452/1A552/1A1F8
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 34c48 unit=%d empty -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    const std::vector<int> targets = battle_target_list_;
    auto& A = units_[(size_t)unit_index];
    if (battle_trace_) fprintf(stderr, "COMBAT: 34c48 unit=%d targets=%zu\n", A.id, targets.size());
    camera_center_on(A.x, A.y);
    A.anim_frame = 5;
    // 49681..50380 presentation: 106 shak2.nsp frames (3 waves x 24 plus a 33-frame tail).
    // Damage follows the presentation.
    battle_fx_34c48_anim(unit_index);
    if (should_exit_) return 1;
    if (targets.empty()) return -1;
    std::vector<BattleDamageFx> fx;
    fx.reserve(targets.size());
    for (size_t slot = 0; slot < targets.size(); ++slot) {
        const int ti = targets[slot];
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        if (!T.active || T.hp <= 0) continue;
        const int base = battle_damage(unit_index, ti, 0, 0, 0);
        const int tmp = 4 * base + 0x1770;
        const int fin = battle_weapon_2caca(unit_index, tmp);
        if (battle_trace_) fprintf(stderr, "COMBAT: 34c48 unit=%d -> target=%d base=%d tmp=%d final=%d\n",
                                    A.id, T.id, base, tmp, fin);
        battle_damage_fx_prepare(unit_index, ti, (int)slot, fin, fx); // 45BFB + [48h] -= fin + 45ED2
    }
    battle_damage_fx_play(fx, targets);                     // sub_287AF + 12124
    battle_damage_fx_triggers(targets, unit_index);         // sub_1ED45(caster)
    battle_nsp_release();                                   // sub_1FFCD
    weather_tick_enabled_ = true;                           // sub_103A0
    return 1;
}

// sub_356DB (edi 6, f-arrow+growl+sarah.nsp): single target, base*5+3000 through 2CACA.
//   With F14D8 == 1 it collects with 2EA0C(7). With F14D8 == 0 it scans the 1A452
//   (width 1, length 7)/1A552 ray, picks a tile with the 4A483 cursor and puts its occupant
//   in F0648 with 1A1F8; an empty collection keeps the cursor open (ASM 50716..50830).
//   The port uses the hostile occupant of the player's tile as the single target.
// battle_fx_356db_anim owns the whole gameplay and presentation of sub_356DB (a direct
// transcription of the routine).
int CombatII::battle_weapon_356db(int unit_index) {
    return battle_fx_356db_anim(unit_index);
}

// sub_35FC2 (edi 7, i-fist+m-wind+stormaxe.nsp): radius-2 diamond + 2CAF7(5,3000).
// battle_fx_35fc2_anim owns the whole gameplay and presentation of sub_35FC2 (a direct
// transcription of the routine).
int CombatII::battle_weapon_35fc2(int unit_index) {
    return battle_fx_35fc2_anim(unit_index);
}

// sub_367EE (edi 8, shoot+f-arrow+bomb1+nang.nsp): screen sweep + nang.nsp presentation
// (40-shot wave) + 2CAF7(4,6000).
int CombatII::battle_weapon_367ee(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int n = battle_weapon_collect_screen(unit_index, 0);
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 367ee unit=%d empty -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    battle_fx_367ee_anim(unit_index);   // nang.nsp; the anim calls the trailing 2CAF7(4,1770h)
    return 1;
}

// sub_377A6 (edi 9, pazizik+f-blast+shu.nsp): front 3 rows x 7 tiles + [1E] |= 4 /
// [1F] = 90 + 2CAF7(2,10000).
// battle_fx_377a6_anim owns the whole gameplay and presentation of sub_377A6 (a direct
// transcription of the routine).
int CombatII::battle_weapon_377a6(int unit_index) {
    return battle_fx_377a6_anim(unit_index);
}

// sub_37FA4 (edi 10, elect+irus.nsp): front 7-tile single target, 10 hits of
// base(ecx=1)+5000 applied directly. Returns 0 on the normal path.
int CombatII::battle_weapon_37fa4(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int n = battle_weapon_collect_line(unit_index, 7);
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 37fa4 unit=%d no line target -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    int ti = battle_target_list_[0];
    auto& A = units_[(size_t)unit_index];
    if (battle_trace_) fprintf(stderr, "COMBAT: 37fa4 unit=%d -> target=%d\n",
                                A.id, units_[(size_t)ti].id);
    camera_center_on(A.x, A.y);
    A.anim_frame = 5;
    // Lightning presentation ((1) clear the list, (2) 44D89 in facing order, (3)-(7)
    // dim/305A2/30824/irus for 9 frames, (8) dash).
    battle_fx_37fa4_anim(unit_index);
    // (9) 10 hits (loc_383A2..loc_385E6). One turn of the original is
    //     [A] 1 frame (pose 5) -> 4F4CD(1) -> [B] 4 frames (pose 6) -> var_C++ ->
    //     [48h] -= var_20 -> 4F4CD(14h) -> [C] 2 frames (pose 7,8) -> 4F4CD(1) ->
    //     [D] 4 frames (pose 9) -> shake (target [65h]/[66h] +/-1 along the facing) ->
    //     var_C++ -> [48h] -= var_20 -> repeat while [48h] >= 0,
    //   and var_C >= 0Ah jumps to loc_3860F before another A/B block.  So there are 10
    //   damage applications and the frames are A1+B4 + C2+D4 = 11 per turn over 5 turns.
    //   B and D start their loop at di = 1 and run while di < 5 (loc_38426/loc_38546), i.e.
    //   four frames, not five.
    //   The original computes var_20 once before the loop (38124: 1D770 + 1388h) and only
    //   recomputes it after a retarget (38715).  It subtracts [48h] directly: no 45BFB/
    //   45ED2 number animation and no per-hit 53797 trigger.
    int hit_damage = 0;                                 // var_20
    auto refresh_hit_damage = [&]() {
        const int base = battle_damage(unit_index, ti, 0, 1, 0); // sub_1D770(ebx=0, ecx=1)
        hit_damage = base + 0x1388;
        if (battle_trace_) fprintf(stderr, "COMBAT: 37fa4 unit=%d target=%d base=%d dmg=%d (cached)\n",
                                    units_[(size_t)unit_index].id, units_[(size_t)ti].id, base, hit_damage);
    };
    refresh_hit_damage();
    // var_C: the hit counter compared at loc_383A2 and loc_3860F. It is incremented after
    // block B and again after block D, before the damage of each pair is applied.
    int hit = 0;
    // Returns 0 to stop the strike, 1 to continue the current block, 2 when the dead target was
    // replaced and the original jumps back to loc_381EF (dash + lightning) before loc_383A2.
    auto apply_hit = [&](int hit_no) -> int {           // [48h] -= var_20
        if (ti < 0 || (size_t)ti >= units_.size() || !units_[(size_t)ti].active) return 0;
        auto& T = units_[(size_t)ti];
        const int old_raw = battle_hp_raw(T);
        const int new_raw = old_raw - hit_damage;
        T.hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
        T.hp_deficit = T.hp > 0 ? T.hp * 100 - new_raw : 0;
        if (battle_trace_) fprintf(stderr, "COMBAT: 37fa4 unit=%d -> target=%d hit=%d dmg=%d hp_raw %d->%d\n",
                                    units_[(size_t)unit_index].id, T.id, hit_no, hit_damage, old_raw, new_raw);
        if (new_raw >= 0) return 1;                       // cmp [48h],0 / jge loc_383A2
        // loc_3860F: 44D75, 44D89(caster), 13F08 (grid mark), 1DB1D, then F0648 = 0.
        // 463FB adds the dying unit itself and removes it again (44D81), leaving the caster.
        battle_highlight_clear();
        battle_highlight_add(unit_index);
        play_unit_death(ti, unit_index);
        ti = -1;
        if (should_exit_ || hit >= 0x0A) return 0;        // var_C >= 0Ah -> loc_3873C
        // loc_386A1..38715: rescan the seven-tile line; a new target recomputes var_20 and
        // resumes at loc_381EF. loc_38676 clears the list and rebuilds it as [caster, target]
        // for facing 0 and [target, caster] otherwise, before the re-entry.
        if (battle_weapon_collect_line(unit_index, 7) <= 0) return 0;
        ti = battle_target_list_[0];
        battle_highlight_clear();
        const bool front_first = ((units_[(size_t)unit_index].facing & 3) == 0);
        if (front_first) battle_highlight_add(unit_index);
        battle_highlight_add(ti);
        if (!front_first) battle_highlight_add(unit_index);
        refresh_hit_damage();
        if (battle_trace_) fprintf(stderr, "COMBAT: 37fa4 unit=%d retarget -> %d (hit %d)\n",
                                    units_[(size_t)unit_index].id, units_[(size_t)ti].id, hit_no);
        return 2;
    };
    while (!should_exit_) {
        if (hit >= 0x0A) break;                           // cmp var_C,0Ah / jge loc_3860F
        const int ent = hit % 3;                         // [esp+12h]>>16 % 3 (F0DAA entry)
        fx_37fa4_hit_block(unit_index, ent, 5, 1);       // [A] 1 frame
        sfx(0x01);                                        // 4F4CD(1)
        fx_37fa4_hit_block(unit_index, ent, 6, 4);       // [B] 4 frames (di=1..4)
        ++hit;                                             // var_C++
        int r = apply_hit(1);                              // damage 1
        if (r == 0) break;
        if (r == 2) { battle_fx_37fa4_dash_and_lightning(unit_index, false); continue; }
        sfx(0x14);                                        // 4F4CD(14h)
        fx_37fa4_hit_block(unit_index, ent, 7, 1);       // [C] di=7
        fx_37fa4_hit_block(unit_index, ent, 8, 1);       // [C] di=8
        sfx(0x01);                                        // 4F4CD(1)
        fx_37fa4_hit_block(unit_index, ent, 9, 4);       // [D] 4 frames (di=1..4)
        // Shake (loc_385C0..loc_385DE): shift the target [+65h]/[+66h] by +/-1 along the
        // caster facing.
        if (ti >= 0 && (size_t)ti < units_.size()) {
            auto& T = units_[(size_t)ti];
            switch (units_[(size_t)unit_index].facing & 3) {
                case 0: T.walk_off_y += 1; break;         // inc [66h]
                case 1: T.walk_off_x -= 1; break;         // dec [65h]
                case 2: T.walk_off_y -= 1; break;         // dec [66h]
                default: T.walk_off_x += 1; break;        // inc [65h]
            }
        }
        ++hit;                                            // var_C++
        r = apply_hit(2);                                  // damage 2
        if (r == 0) break;
        if (r == 2) battle_fx_37fa4_dash_and_lightning(unit_index, false);
    }
    // Tail (54199..54220): 44D75, 44D6E(0), 5F9AE (master palette restore), 12124, 1FFCD.
    battle_highlight_clear();                           // 44D75
    battle_dim_row0_ = false;                           // 44D6E(0, 100h)
    pal_load_range(0, 256, &battle_palette_[0][0]);     // 5F9AE
    reset_unit_pose(units_[(size_t)unit_index]);        // 12124 (caster)
    if (ti >= 0 && (size_t)ti < units_.size()) reset_unit_pose(units_[(size_t)ti]); // 12124 (F0648)
    game_timer_set(6);
    battle_nsp_release();       // 1FFCD
    return 0; // the normal elect path returns 0 (4C98A skips 301BB and the cost)
}

// sub_39A49 (edi 11, fire+effect2+f-wave+rain.nsp): diamond collection + [1E] |= 1 /
// [1F] = 5Ah + 2CAF7(2,7000).
//   F14D8 == 1 uses the screen sweep. F14D8 == 0 collects the radius-2 1AC08 diamond around
//   the 4A483 cursor tile with 1AC59/1A1F8; an empty collection keeps the cursor open
//   (ASM 56113..56200).
// battle_fx_39a49_anim owns the whole gameplay and presentation of sub_39A49 (a direct
// transcription of the routine).
int CombatII::battle_weapon_39a49(int unit_index) {
    return battle_fx_39a49_anim(unit_index);
}

// sub_3AEE8 (edi 12, f-blast+io.nsp): screen sweep + [1E] |= 2 / [1F] = 90 + 197AD +
// 2CAF7(3,7000).
int CombatII::battle_weapon_3aee8(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int n = battle_weapon_collect_screen(unit_index, 0);
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 3aee8 unit=%d empty -> -1\n", units_[(size_t)unit_index].id);
        return -1;
    }
    // The status ([1E] |= 2 / [1F] = 5Ah), 197AD and 2CAF7(3,1B58h) all live in the 3AEE8 tail.
    battle_fx_3aee8_anim(unit_index);
    return 1;
}

// sub_302E6 (V-leader dispatch over 13 routines, si == 0 with edi = 4BC3C result). Each case
// is an SSP (4F460, presentation only) plus the matching routine (308EE..3AEE8). All 13 are
// ported: dx > 0x0C returns -1, edi == 10 (elect + 37FA4) returns 0 on the normal path, and
// the other 12 return 1 normally or -1 with no target.
// 4C98A runs 301BB and the cost only for ax == 1, so 0/-1 returns to the ring without
// spending. SSP playback is log-only here.
int CombatII::battle_weapon_302e6(int unit_index, int edi) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int id = units_[(size_t)unit_index].id;
    static constexpr const char* kCases[13] = {
        "fire.ssp+308EE", "kang+f-blast+slash+31283", "chang+slash+32589",
        "i-star2+slash+ice+32BCB", "thunder1+f-blast+thunder3+boom+341C7",
        "ju+34C48", "f-arrow+growl+356DB", "i-fist+m-wind+35FC2",
        "shoot+f-arrow+bomb1+367EE", "pazizik+f-blast+377A6",
        "elect+37FA4", "fire+effect2+f-wave+39A49", "f-blast+3AEE8"};
    if (edi < 0 || edi > 12) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 302e6 unit=%d edi=%d out of range -> -1 (Tripwire)\n", id, edi);
        return -1;
    }
    // ASM loc_30300..loc_30501: each branch first loads and plays MAGICSPR\<name>.ssp with
    //   4F460 (32h/33h/34h/35h are the sound slots). Audio here is single-channel, so the files
    //   play in order.
    static const char* const kSsp[13][5] = {
        {"fire.ssp", nullptr, nullptr, nullptr, nullptr},                             // 0
        {"kang.ssp", "f-blast.ssp", "slash.ssp", nullptr, nullptr},                    // 1
        {"chang.ssp", "slash.ssp", nullptr, nullptr, nullptr},                          // 2
        {"i-star2.ssp", "slash.ssp", "ice.ssp", nullptr, nullptr},                     // 3
        {"thunder1.ssp", "f-blast.ssp", "thunder3.ssp", "boom.ssp", nullptr},          // 4
        {"ju.ssp", nullptr, nullptr, nullptr, nullptr},                                  // 5
        {"f-arrow.ssp", "growl.ssp", nullptr, nullptr, nullptr},                        // 6
        {"i-fist.ssp", "m-wind.ssp", nullptr, nullptr, nullptr},                        // 7
        {"shoot.ssp", "f-arrow.ssp", "bomb1.ssp", nullptr, nullptr},                    // 8
        {"pazizik.ssp", "f-blast.ssp", nullptr, nullptr, nullptr},                      // 9
        {"elect.ssp", nullptr, nullptr, nullptr, nullptr},                               // 10
        {"fire.ssp", "effect2.ssp", "f-wave.ssp", nullptr, nullptr},                    // 11
        {"f-blast.ssp", nullptr, nullptr, nullptr, nullptr},                             // 12
    };
    for (int si = 0; si < 5; ++si)
        if (kSsp[edi][si]) battle_ssp_play(kSsp[edi][si]);            // sub_4F460
    if (battle_trace_) fprintf(stderr, "COMBAT: 302e6 unit=%d edi=%d %s begin\n",
                                id, edi, kCases[edi]);
    int ret = -1;
    switch (edi) {
        case 0: ret = battle_weapon_308ee(unit_index); break;
        case 1: ret = battle_weapon_31283(unit_index); break;
        case 2: ret = battle_weapon_32589(unit_index); break;
        case 3: ret = battle_weapon_32bcb(unit_index); break;
        case 4: ret = battle_weapon_341c7(unit_index); break;
        case 5: ret = battle_weapon_34c48(unit_index); break;
        case 6: ret = battle_weapon_356db(unit_index); break;
        case 7: ret = battle_weapon_35fc2(unit_index); break;
        case 8: ret = battle_weapon_367ee(unit_index); break;
        case 9: ret = battle_weapon_377a6(unit_index); break;
        case 10: ret = battle_weapon_37fa4(unit_index); break;
        case 11: ret = battle_weapon_39a49(unit_index); break;
        case 12: ret = battle_weapon_3aee8(unit_index); break;
        default: ret = -1; break;
    }
    if (battle_trace_) fprintf(stderr, "COMBAT: 302e6 unit=%d edi=%d %s -> %d\n",
                                id, edi, kCases[edi], ret);
    return ret;
}

// sub_4C98A: runs a V weapon. si = list position (0 = leader, 1+ = regular). The 4D442
// caller ignores the return value and always yields -2 (stay in the ring), so the port also
// returns 0 and leaves the caller in the ring. The leader position always takes the 4BC3C
// path regardless of a match (4BC3C only looks at the ID, so the gate is well defined even
// for an unmatched ID like 18; only an ID with no mapping at all is a tripwire).
//   si == 0: edi = 4BC3C(id) match -> gate = 75426[edi] vs TP+[57h] -> run 302E6.
//          ax == 1 runs 301BB(bx=1, edx=edi) + consume(gate-[57h]) and sets [57h] = 0.
//          ax != 1 returns to the ring without spending ([57h] kept).
//   si != 0: i = si-1, gate = 753C2[i] vs TP+[57h] -> 2CA40(dx=1..3) + F1474[i](edx=dx).
//          ax == -1 returns to the ring without spending ([57h] kept).
//          class == 0x32 skips 301BB and only consumes, otherwise 301BB(bx=0, edx=i) +
//          consume. The cost is gate-[57h]; when negative it skips 45B4E and only clears
//          [57h].
//   All 13 sub_302E6 routines are ported: 12 return 1 (301BB runs) or -1 with no target,
//   elect (edi 10) returns 0 on the normal path (301BB and the cost are skipped) or -1 with
//   no target. The 2CC3A family (idx0/8/19) is implemented in battle_weapon_2cc3a and
//   returns 1 after the hit sequence. The small F1474 cases (idx2/4/12) return 1, so 301BB
//   and the cost run there.
int CombatII::battle_use_weapon(int unit_index, int si) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];
    const int tp = battle_tp(), acc = (int)u.combat_acc57;
    if (si == 0) {
        const int edi = battle_weapon_match_4bc3c(u.id);
        if (edi < 0 || edi > 12) {
            if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=0 no 4BC3C match (Tripwire)\n", u.id);
                return 0;
        }
        const int gate = (int)battle_weapon_tp75426(edi);
        if (tp + acc < gate) {
            battle_unit_label(unit_index, "No TP!");   // sub_4C98A -> sub_48612
            if (battle_trace_) fprintf(stderr, "COMBAT: weapon No TP! unit=%d si=0 gate=%d\n", u.id, gate);
            return 0;
        }
            // 308EE/356DB/39A49 open the 4A483 cursor on the player path (TP not spent yet).
        if (battle_pick_tx_ < 0 && battle_weapon_cursor_kind(unit_index, 0) != 0) {
            battle_weapon_pending_si_ = 0;
            battle_weapon_pending_kind_ = battle_weapon_cursor_kind(unit_index, 0);
            if (battle_trace_)
                fprintf(stderr, "COMBAT: weapon unit=%d si=0 entry=%d -> 4A483 cursor kind=%d (no consume)\n",
                        u.id, edi, battle_weapon_pending_kind_);
            return 0;
        }
        const int w302e6 = battle_weapon_302e6(unit_index, edi);
        if (w302e6 == 0) {
            // Normal elect (37FA4): the routine already handled damage and death, so return to
            // the ring without 301BB and the cost.
            if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=0 entry=%d gate=%d 302e6=0 elect done (no consume, ring)\n",
                                        u.id, edi, gate);
            return 0;
        }
        if (w302e6 != 1) {
            if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=0 entry=%d gate=%d 302e6=%d -> no consume (ring)\n",
                                        u.id, edi, gate, w302e6);
            return 0;
        }
        battle_weapon_301bb(unit_index, edi, 1);
        if (should_exit_) return 0;
        const int amount = gate - acc;
        if (amount < 0) {
            u.combat_acc57 = 0;
            if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=0 consume skipped (amount=%d<0, acc cleared)\n",
                                        u.id, amount);
        } else {
            battle_consume_tp(unit_index, amount);
        }
        u.combat_acc57 = 0; // mov [ecx+57h],0 (45B4E clears it too, as does the jl path)
        if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=0 entry=%d gate=%d 302e6=1 301bb done consume=%d\n",
                                    u.id, edi, gate, amount);
        return 0;
    }
    const int i = si - 1;
    if (i < 0 || i > 24) {
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: weapon unit=%d si=%d out of range\n", u.id, si);
        return 0;
    }
    const int gate = (int)battle_weapon_tp_any(i); // 753C2[i] = word_753C4[i] (0..24)
    if (tp + acc < gate) {
        battle_unit_label(unit_index, "No TP!");   // sub_4C98A -> sub_48612
        if (battle_trace_) fprintf(stderr, "COMBAT: weapon No TP! unit=%d si=%d gate=%d\n", u.id, si, gate);
        return 0;
    }
    const int w2340 = battle_weapon_2ca40(unit_index);
    battle_f14d8_ = 0;                                     // player ring path (sub_4C98A)
    // With F14D8 == 0 the original picks a target with the 4A483 cursor. While no tile is
    // picked it spends no TP and waits on the cursor (battle_player_input takes the click and
    // calls this function again).
    if (battle_weapon_needs_cursor(i) && battle_pick_tx_ < 0) {
        battle_weapon_pending_si_ = si;
        battle_weapon_pending_kind_ = 1;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: weapon unit=%d si=%d -> 4A483 target cursor (no consume)\n", u.id, si);
        return 0;
    }
    const int w1474 = battle_weapon_f1474(unit_index, i, w2340);
    if (w1474 == -1) {
        if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=%d gate=%d 2ca40=%d f1474=-1 -> no consume (ring)\n",
                                    u.id, si, gate, w2340);
        return 0;
    }
    if (u.combat_class != 0x32) {
        battle_weapon_301bb(unit_index, i, 0);
        if (should_exit_) return 0;
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: weapon unit=%d si=%d class=0x32 skip 301bb (consume only)\n", u.id, si);
    }
    const int amount = gate - acc;
    if (amount < 0) {
        u.combat_acc57 = 0;
        if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=%d consume skipped (amount=%d<0, acc cleared)\n",
                                    u.id, si, amount);
    } else {
        battle_consume_tp(unit_index, amount);
    }
    u.combat_acc57 = 0;
    if (battle_trace_) fprintf(stderr, "COMBAT: weapon unit=%d si=%d gate=%d 2ca40=%d f1474=%d 301bb done consume=%d\n",
                                u.id, si, gate, w2340, w1474, amount);
    return 0;
}

// sub_4CB5E: dispatches on dx and returns si (0 = continue, -2 = recompute budget,
// -3/-4 = finish). Only the reachable cases 1-7 are implemented; the rest trip a warning.
int CombatII::battle_use_item(int unit_index, int dx, int tx, int ty) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];
    if (battle_trace_) fprintf(stderr, "COMBAT: item unit=%d dx=%d\n", u.id, dx);
    switch (dx) {
        case 0: { // sub_3C7D1: warp item (5% HP self-damage + sub_3C769 relocation + sub_5401B).
            //   Returns -2 on success.
            if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return 3;
            // sub_169D7 (-1 = walkable, empty) then sub_49ABC for the front tile -> sub_1A452
            //   (count 1, range 7)/sub_1A552 ray.
            //   Per tile: a sub_13CBD result != -1 ends the ray (nothing marked). Otherwise it
            //   checks the 4 neighbours for a hostile (1201D == 1) unit and marks the tile with
            //   sub_13D02(..., 1) either way. A hostile neighbour also ends the ray (cx < 4
            //   stops the loop).
            //   Selectable tiles = sub_13CBD >= 0 (marked tiles). Any other click goes back to
            //   sub_4A483 (port: 3 = repick).
            std::vector<int> grid;
            battle_ai_169d7_reset(unit_index, grid);
            if (grid.empty()) return 3;
            static constexpr int kDxy[4][2] = { {0, 1}, {-1, 0}, {0, -1}, {1, 0} };   // sub_49ABC facing 0..3
            const int fdir = (int)((uint8_t)u.facing) & 3;
            int rx = (int)(int8_t)u.x + kDxy[fdir][0], ry = (int)(int8_t)u.y + kDxy[fdir][1];
            bool selectable = false;
            for (int step = 0; step < 7; ++step, rx += kDxy[fdir][0], ry += kDxy[fdir][1]) {
                if (battle_ai_wc5_diff_read(grid, rx, ry) != -1) break;       // sub_13CBD != -1 -> stop
                bool hostile_adjacent = false;
                for (int c = 0; c < 4 && !hostile_adjacent; ++c)
                    if (battle_ai_hostile_unit_at(rx + kDxy[c][0], ry + kDxy[c][1], unit_index) >= 0) hostile_adjacent = true;
                if (rx == tx && ry == ty) selectable = true;                  // sub_13D02(rx, ry, 1)
                if (hostile_adjacent) break;
            }
            if (!selectable) return 3;
            const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
            // sub_3C769: sub_46DF1 (camera on the midpoint; sar 1) -> sub_11EE4 (release the old
            // tile) -> swap coordinates -> sub_11E04. No label and no rotation.
            camera_center_on(ux + ((tx - ux) >> 1), uy + ((ty - uy) >> 1));
            u.x = tx; u.y = ty;
            battle_wc1_occupancy_preserve_or_close("warp item (3C769)");
            {
                const int max_raw = battle_max_hp_raw(u);
                const int cost = max_raw / 20; // idiv 14h (5%)
                const int new_raw = battle_hp_raw(u) - cost;
                const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
                u.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
                u.hp = new_hp;                                   // [48h] -= max/20 (the 25-frame 46359 digit effect is skipped)
            }
            present_cutscene_frame();
            battle_after_move(unit_index); // sub_5401B
            if (should_exit_) return 0;
            battle_consume_tp(unit_index, 0x28); // loc_4CB8B: edx=28h
            return -2;
        }
        case 5: { // sub_3B7CE/3B614: mount when [5Ch] == 0 (3B7CE), dismount otherwise (3B614).
            // Both are sub_4A483 cursor loops. A failed click returns 3 (repick); success (a
            // returned pointer) spends 40 TP and returns -2 (recompute budget) like the caller.
            // 4CB5E writes the returned pointer back into [ecx], but the caster index is
            // unchanged in the port.
            const int uid = u.id;
            const bool mounted = u.code_b != 0;
            const int r = mounted ? battle_item_3b614(unit_index, tx, ty)
                                  : battle_item_3b7ce(unit_index, tx, ty);
            if (r == 3) return 3;
            if (r == 0) {
                if (battle_trace_) fprintf(stderr, "COMBAT: item %s canceled unit=%d\n",
                                            mounted ? "dismount" : "mount", uid);
                return 0;                              // loc_4CDB7 (no cost)
            }
            battle_consume_tp(unit_index, 0x28);       // loc_4CBB4: sub_45B4E(28h)
            if (should_exit_) return 0;
            if (battle_trace_) fprintf(stderr, "COMBAT: item %s unit=%d done (sub_%s)\n",
                                        mounted ? "dismount" : "mount", uid,
                                        mounted ? "3B614" : "3B7CE");
            return -2;                                 // mov eax, 0FFFFFFFEh
        }
        case 6: { // sub_18D36: search. Scans F0382 with the type gate, then camera and marker
            //   (no cost).
            // Original: cl = FF, bx = (class >= 0x0C), si = lap count. dx starts at F040E and
            //   advances one tile at a time, 10 tiles per lap, 2 laps.
            //   ch = F0382; == 0 or == cl is skipped. (F0388+1) == 0 passes, == 3 only when
            //   bx > 0, else skipped.
            //   A hit sets F040E = dx then
            //   1124A/4A321/49E4E/44D6E/454B3 + marker + 49EEE/5E9E8/44D6E/499EF.
            //   A miss shows "Search Failed" + 499EF(0x1F4). The port replaces the
            //   presentation with the camera plus logging.
            const int cls_ge0c = (u.combat_class >= 0x0C) ? 1 : 0; // setnb al
            int si = 0, cur = battle_search_cursor_;
            int found_idx = -1;
            for (int wrap = 0; wrap < 2 && found_idx < 0; ++wrap) {
                for (int k = 0; k < 10; ++k) {
                    cur = (cur + 1) % 10;
                    uint8_t ch = 0, type = 0;
                    if ((size_t)cur < battle_tile_events_.size()) {
                        ch = battle_tile_events_[(size_t)cur].state;
                        type = battle_tile_events_[(size_t)cur].type;
                    } else {
                        ch = (cur >= 0 && cur < 10) ? battle_search_slots_[cur] : 0;
                    }
                    if (ch == 0) continue; // test ch,ch; jz
                    if (ch == 0xFF) continue; // cmp cl(FF),ch; jz (already used)
                    if (type == 0) { found_idx = cur; break; }
                    if (type == 3 && cls_ge0c > 0) { found_idx = cur; break; }
                    // Types 1/2 and so on cannot be searched (the original accepts only 0/3;
                    // 1-7 [0] skips type 1 and [1] hits type 0).
                    continue;
                }
                if (found_idx < 0) si++;
            }
            battle_search_cursor_ = cur; // update word_F040E (kept on failure; the original can also store -1)
            if (found_idx < 0) {
                // Si == 2 shows Search Failed (original 48083 + 499EF 0x1F4); F040E = -1 maps
                // to the cursor only.
                if (si >= 2) battle_search_cursor_ = -1;
                battle_text_band("Search Failed");        // sub_48083("Search Failed")
                wait_info_dismiss(0x1F4);                 // sub_499EF(1F4h)
                if (battle_trace_) fprintf(stderr, "COMBAT: warning: search failed unit=%d cursor=%d (sub_18D36)\n",
                                           u.id, battle_search_cursor_);
                return 0; // no cost (45B4E is not called)
            }
            // Success: move the camera to the event tile (1124A) and draw the marker. No cost.
            {
                const auto& e = battle_tile_events_[(size_t)found_idx];
                camera_center_on((int)e.x, (int)e.y); // sub_1124A(F037D/F037E>>24)
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: search hit unit=%d event[%d] (%u,%u) item=%u type=%u (sub_18D36)\n",
                            u.id, found_idx, (unsigned)e.x, (unsigned)e.y,
                            (unsigned)e.state, (unsigned)e.type);
                // sub_18D36 loc_18DCF: 1124A -> 4A321 -> 49E4E(unit, 5) -> 44D6E(1) -> 454B3 ->
                //   (dim row 0xA rectangle) -> 49EEE (tile marker) -> 5E9E8 -> 44D6E(0) ->
                //   499EF(1F4h). No text.
                //   Dim row 0xA is outside the table (sub_6102C(4) = 4 rows x 256 bytes), so the
                //   original reads uninitialised heap there; the port does not reproduce it and
                //   only draws the tile marker.
                battle_search_marker(unit_index, (int)e.x, (int)e.y);
            }
            return 0;
        }
        case 1: { // sub_3B5B4 (jump table index 1): toggles [14h]&1 - sprite type +/-1 plus
                   //   image slot reallocation. Costs 40, returns -2.
            if (u.combat_flags_14 & 1) {
                u.sprite_type = (int)(uint16_t)(u.sprite_type - 1);   // dec word [+61h]
                u.combat_flags_14 = (uint8_t)(u.combat_flags_14 & 0xFE);
            } else {
                u.sprite_type = (int)(uint16_t)(u.sprite_type + 1);   // inc word [+61h]
                u.combat_flags_14 = (uint8_t)(u.combat_flags_14 | 1);
            }
            reset_unit_pose(u);                                       // equivalent of the 45559 redraw
            present_cutscene_frame();
            if (battle_trace_)
                fprintf(stderr, "COMBAT: item toggle unit=%d type=%d flags14=%02X (sub_3B5B4)\n",
                        u.id, u.sprite_type, (unsigned)u.combat_flags_14);
            battle_consume_tp(unit_index, 0x28);                       // sub_45B4E(0x28)
            return -2;
        }
        case 9: { // sub_3BFAC: teleport - click-move to an E8F46 == -1 tile inside the diamond of
                   //   radius 5 (wc1) or 7. Costs 40, returns -2.
            if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return 3;
            const int radius = (weapon_class(u) == 1) ? 5 : 7;          // unk_731B4 == 1
            const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
            if (std::abs(tx - ux) + std::abs(ty - uy) > radius) return 3;
            std::vector<int> grid;
            battle_ai_169d7_reset(unit_index, grid);                    // sub_169D7 (-1 = walkable, empty)
            if (grid.empty() || battle_ai_wc5_diff_read(grid, tx, ty) != -1) return 3;
            if (!battle_ai_165c2_ok(tx, ty, unit_index, grid, (int)u.facing & 3)) return 3; // sub_165C2 re-check
            if (!battle_warp_unit(unit_index, tx, ty)) return 3;         // sub_3BF00
            if (should_exit_) return 0;
            battle_consume_tp(unit_index, 0x28);                         // 4CB8B: sub_45B4E(0x28)
            return -2;
        }
        case 3: { // sub_3BB05 (4CB5E jump table index 3): Poison - [1Eh] |= 80h and [27h] = C8h
                   //   on the front hostile. A success (1) spends 40 TP.
            int x = (int)(int8_t)u.x, y = (int)(int8_t)u.y;
            switch ((uint8_t)u.facing) { case 0: ++y; break; case 1: --x; break; case 2: --y; break; case 3: ++x; break; default: break; }
            const int t = battle_ai_hostile_unit_at(x, y, unit_index);
            if (t < 0 || (int16_t)units_[(size_t)t].sprite_type >= 0x12C) return 0;
            battle_unit_label(unit_index, "Poison");   // sub_3BB05: sub_48612(caster, "Poison")
            if (should_exit_ || (size_t)t >= units_.size()) return 0;
            units_[(size_t)t].combat_flags_1e = (uint8_t)(units_[(size_t)t].combat_flags_1e | 0x80);
            units_[(size_t)t].ail_27 = 0xC8;
            game_timer_set(0x40);                      // sub_1033B(40h)
            wait_game_tick();                          // sub_499E4
            battle_consume_tp(unit_index, 0x28);
            return 0;
        }
        case 2: { // sub_3B9E1: whirlpool (tiles with terrain attribute 2/3). A success (1) spends
                   //   40 TP.
            if (battle_ai_3b9e1(unit_index) == 1) battle_consume_tp(unit_index, 0x28);
            return 0;
        }
        case 4: { // sub_3BBA7: [1Eh] |= 10h and [23h] = C8h on the front unit with [0Eh] == 4 and
                   //   [0Dh] != 2 (no TP cost).
            int x = (int)(int8_t)u.x, y = (int)(int8_t)u.y;
            switch ((uint8_t)u.facing) { case 0: ++y; break; case 1: --x; break; case 2: --y; break; case 3: ++x; break; default: break; }
            if (x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4) return 0;
            const int t = battle_unit_index_at(x, y);
            if (t < 0 || (size_t)t >= units_.size()) return 0;
            auto& o = units_[(size_t)t];
            if (battle_effective_army(o) != 4 || o.unit_kind_0d == 2) return 0;
            o.combat_flags_1e = (uint8_t)(o.combat_flags_1e | 0x10);
            o.ail_23 = 0xC8;
            return 0;
        }
        case 10: { // sub_3BC67 + 4CC53: clone, then [84h] |= 2, cost 40.
            battle_clone_3bc67(unit_index);
            if (should_exit_ || unit_index >= (int)units_.size()) return 0;
            units_[(size_t)unit_index].flags_84 = (uint8_t)(units_[(size_t)unit_index].flags_84 | 2); // 4CC53
            battle_consume_tp(unit_index, 0x28);
            return 0;
        }
        case 13: { // sub_3C30A: curse. A success (1) spends 40 TP.
            if (battle_ai_3c30a(unit_index) == 1) battle_consume_tp(unit_index, 0x28);
            return 0;
        }
        case 7: { // sub_3BC17: Berserk - puts the unit into auto-battle (berserk) state. 4CB5E
                   //   returns -1 (end of the player turn).
            battle_berserk_3bc17(unit_index);
            return -1;
        }
        case 11: { // sub_3C09F: Morale Down (both sides -2, floor 50). Costs 60 (edx=3Ch ->
                   //   loc_4CBF9).
            battle_item_3c09f(unit_index);             // shared routine (same as VM 09 sub 2)
            if (should_exit_ || unit_index >= (int)units_.size()) return 0;
            battle_consume_tp(unit_index, 0x3C);
            return 0;
        }
        case 14: { // sub_3C3A5: Ice Dragon (13x12 camera-area hostile sweep + 28728 + 2A500 +
                   //   freeze + 2A5BE + 1ED90).
            battle_item_3c3a5(unit_index);             // shared routine (same as VM 09 sub 4)
            if (should_exit_ || unit_index >= (int)units_.size()) return 0;
            units_[(size_t)unit_index].flags_84 = (uint8_t)(units_[(size_t)unit_index].flags_84 | 4);  // 4CC96: [84h] |= 4 (after the call)
            battle_consume_tp(unit_index, battle_tp());   // loc_4CC3D: sub_1A0E9 -> sub_45B4E (all TP)
            return 0;
        }
        case 15: { // sub_3C4C1: Defense (only for [5Ah] col0 == 6; 18 fails with col0 = 0).
            if (!battle_defense_3c4c1(unit_index)) {   // loc_4CCA8: ax != 1 -> loc_4CDB7 without cost
                if (battle_trace_) fprintf(stderr, "COMBAT: defense rejected unit=%d (no guard item/already guarded)\n", u.id);
                return 0;
            }
            battle_consume_tp(unit_index, 0x28);   // loc_4CBF4: 45B4E(28h)
            return 0;
        }
        case 8: { // sub_3BDC7: heal / sun light / MP+20 (chance based). 4CC36 -> loc_4CC3D:
                   //   spends all TP (sub_1A0E9 -> sub_45B4E), returns 0.
            battle_item_3bdc7(unit_index);
            if (should_exit_ || unit_index >= (int)units_.size()) return 0;
            battle_consume_tp(unit_index, battle_tp());
            return 0;
        }
        case 12: { // sub_3C12D: Wish to Win!!! (the caster leaves the field). 4CC73: sub_1A0BF(0)
                   //   sets TP to 0 and returns -4 (finish).
            battle_item_3c12d(unit_index);
            battle_tp_set(0);                          // xor eax,eax; call sub_1A0BF
            return -4;
        }
        case 16: { // loc_4CCB4: transform / revert. With [77h] != 0 it runs sub_3C70C (revert) and
                   //   spends 40 TP (loc_4CB8B); otherwise it summons per id with sub_3C548.
            if (u.stored_77 || u.linked_class >= 0) {
                battle_item_3c70c(unit_index);
                if (should_exit_ || unit_index >= (int)units_.size()) return 0;
                battle_consume_tp(unit_index, 0x28);
                return -2;
            }
            // [0Fh] = unit id: 2 -> form 0x10 (word_FD8BC), 0x16 -> form 0x14 (word_FD8BE),
            //   0x1D -> form 0x0F (word_FD8C0). Only a success (ax == 1) sets the matching
            //   one-shot flag, and either way no TP is spent and the case returns -2 (loc_4CB97).
            if (u.id == 2) { if (battle_item_3c548(unit_index, 0x10) == 1) battle_fd8bc_ = 1; }
            else if (u.id == 0x16) { if (battle_item_3c548(unit_index, 0x14) == 1) battle_fd8be_ = 1; }
            else if (u.id == 0x1D) { if (battle_item_3c548(unit_index, 0x0F) == 1) battle_fd8c0_ = 1; }
            return -2;
        }
        case 17: { // Morale Up: adds 2 to word F0BA0 while it is < 0x96 (2 more when [5Dh] == 4Eh),
                   //   with no upper re-clamp. Always the low word of F0BA0 regardless of legion.
            if (morale_lo_ < 0x96) {                                  // cmp dx,96h; jge
                morale_lo_ = (int16_t)(morale_lo_ + 2);
                if (u.combat_type_5d == 0x4E) morale_lo_ = (int16_t)(morale_lo_ + 2);
            }
            // 4CD67: sub_1033B(10h); [64h]=8; sub_45559; sub_499E4; sub_48612(unit, "Morale Up!");
            //   sub_12124; sub_45559; sub_44DF3; 45B4E (all TP); sub_53E0F.
            game_timer_set(0x10);
            u.anim_frame = 8;
            present_cutscene_frame();
            wait_game_tick();
            battle_unit_label(unit_index, "Morale Up!");
            if (should_exit_) return 0;
            reset_unit_pose(u);
            present_cutscene_frame();
            battle_consume_tp(unit_index, battle_tp());
            run_morale_trigger();                                     // sub_53E0F (last call at 4CD39)
            if (should_exit_) return 0;
            if (battle_trace_) fprintf(stderr, "COMBAT: morale up unit=%d lo=%d hi=%d\n",
                                        u.id, (int)morale_lo_, (int)morale_hi_);
            return 0;
        }
        default: {
            // sub_4CB5E has exactly 18 cases (dx 0..17); anything else has no dispatch entry.
            if (battle_trace_) fprintf(stderr, "COMBAT: item dx=%d outside the 0..17 dispatch table\n", dx);
            return 0;
        }
    }
}

void CombatII::render_player_move_range() {
    if (battle_deploy_active_) { render_attack_range(); return; }   // deploy phase: paint the zone grid (E8F46) directly (sub_1547F)
    if (battle_state_ != 1) return;
    if (battle_free_wait_) return; // free wait (5580F) draws no shading (only 49FE1+4A106)
    // sub_44D67(1) shares one grid: reach in the attack target phase, movement range in the
    // move phase, and reach again in the ring phase - the ring phase shows 1CC09 reach, the
    // opposite of what the name suggests. The original 4D674 does
    // 1CC09 -> 44D67(1) -> 4B767 and the 4D7EA loop re-lays 1CC09; 1547F paints E8 as-is.
    if (battle_atk_phase_ != 0) { render_attack_range(); return; }
    if (battle_move_phase_ != 0) {
        if (battle_player_unit_ < 0 || (size_t)battle_player_unit_ >= units_.size()) return;
        // Move phase: shade the diffusion-reachable tiles (1547F).
    } else {
        // Ring phase (move_phase == 0): shade reach (1CC09); the move diffusion is not drawn.
        if (battle_player_unit_ >= 0 && (size_t)battle_player_unit_ < units_.size())
            render_attack_range();
        return;
    }
    if (!battle_player_diffusion_valid_) return;
    if (map_w_ == 0 || map_h_ == 0) return;
    if (battle_player_diffusion_.size() != (size_t)map_w_ * (size_t)map_h_) return;
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    const uint8_t* lut = battle_move_shade_lut(); // sub_6104A(1)
    // sub_1547F: 13 rows x 13 columns starting at cam_tile (same as sub_10AEB).
    for (int r = 0; r < 13; ++r) {
        for (int c = 0; c < 13; ++c) {
            const int tx = cam_tile_x_ + c, ty = cam_tile_y_ + r;
            if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) continue;
            // `cmp word ptr [ebx],0; jl skip` - a remaining budget of 0 still counts as reached.
            if (battle_player_diffusion_[(size_t)ty * map_w_ + (size_t)tx] < 0) continue;
            const int sx = c * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
            const int sy = r * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
            // sub_10130: 24 bytes x 16 rows (ecx=18h, dx=10h); pixels off screen are skipped.
            for (int y = 0; y < TOWN_TILE_H; ++y) {
                const int py = sy + y;
                if (py < 0 || py >= SCREEN_H) continue;
                uint8_t* line = bb + (size_t)py * SCREEN_W;
                for (int x = 0; x < TOWN_TILE_W; ++x) {
                    const int px = sx + x;
                    if (px < 0 || px >= SCREEN_W) continue;
                    line[px] = lut[line[px]];
                }
            }
        }
    }
}

// sub_19D53 (sub_19D53..loc_19F56). Ported here: the head (loc_19D7C..loc_19DEE) and the
// F042E gate/return code.
//   Returns 1 (paused, waiting or idle), 2 (player turn start - P2), 3 (AI acts - P2/P6).
int CombatII::battle_update() {
    // sub_19D53: `cmp byte_F042E,2; jnz; mov byte_F042E,3` - turn a pause request into paused.
    if (battle_state_ == 2) battle_state_ = 3;
    // loc_19D69: with F042E != 0, `mov eax,1; jmp loc_196C9` (no tick is counted).
    if (battle_state_ != 0) return 1;

    battle_overlay_mode_ = 0;                          // `xor eax,eax; call sub_44DEC`

    // `cmp word_F0422,0; jnz loc_19E02` - when a group walk was interrupted, skip the tick
    // increment and triggers and resume the walk.
    if (battle_scan_active_ == 0) {
        battle_time_ += 1;                             // inc dword_F0418
        if (battle_trace_) fprintf(stderr, "COMBAT: tick %u seq=%d\n", battle_time_, (int)script_seq_);
        // sub_44EB0: the time dot in the bottom HUD - render_hud_frame draws it from
        // battle_time_ % byte_74138.

        // byte_BA84E == 4/5 -> sub_197CA every 30 ticks, otherwise every 0x384 (900) ticks.
        const uint32_t tod_period = (sce_map_kind_ == 4 || sce_map_kind_ == 5) ? 0x1Eu : 0x384u;
        if (battle_time_ % tod_period == 0) battle_time_of_day_step();

        // loc_19DD7: `mov ebx,dword_74134; test ebx,ebx; jl loc_19DEE` -> sequence trigger, then
        // `inc dword_74134`.
        if (script_loaded_ && script_seq_ >= 0) {
            VmResult r;
            const bool f = run_seq_trigger((uint32_t)script_seq_, r);   // sub_5358F
            if (battle_trace_)
                fprintf(stderr, "COMBAT: seq trigger(%d) fired=%d events=%zu\n", (int)script_seq_, f ? 1 : 0, r.events.size());
            if (f && !r.events.empty()) {
                fprintf(stderr, "COMBAT: battle update %u -> seq trigger(%d) %zu event(s)\n",
                        battle_time_, (int)script_seq_, r.events.size());
                play_vm_events(r.events);
                if (should_exit_) return 1;
            }
            // When the trigger ran VM 0x0E (sub_5283D) dword_74134 is already 0 and the
            // original still does `inc` afterwards.
            script_seq_ += 1;
        }

        // sub_51211: consume the reinforcement queue (byte_110694); a unit whose spawn tile is
        // blocked is retried next tick.
        battle_spawn_queue_step();
        if (should_exit_) return 1;

        // sub_53523(dword_F0418): time triggers.
        if (script_loaded_) {
            VmResult r;
            const bool f = run_time_trigger(battle_time_, r);
            if (battle_trace_)
                fprintf(stderr, "COMBAT: time trigger(%u) fired=%d events=%zu\n", battle_time_, f ? 1 : 0, r.events.size());
            if (f && !r.events.empty()) {
                fprintf(stderr, "COMBAT: battle update %u -> time trigger %zu event(s)\n",
                        battle_time_, r.events.size());
                play_vm_events(r.events);
                if (should_exit_) return 1;
            }
        }

        battle_atb_step();               // sub_19AEF: gauge growth and the ready queue
    }

    return battle_turn_scheduler();
}

bool CombatII::load_etc_icons() {
    etc_icons_tried_ = true;
    etc_icons_loaded_ = false;
    std::string cand = game_path("ETC.ICO");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "ETC.ICO");
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: ETC.ICO not found (A.I/status icons skipped)\n");
        return false;
    }
    if (!etc_icons_.load(cand) || etc_icons_.count() <= 10) {
        fprintf(stderr, "COMBAT: warning: ETC.ICO load failed or too few sprites (%d)\n", etc_icons_.count());
        return false;
    }
    etc_icons_loaded_ = true;
    return true;
}

// sub_12161: draw ETC.ICO #0..#7 at each occupied tile whose ATB gauge is ready.
void CombatII::render_ai_ready_markers() {
    if (std::none_of(units_.begin(), units_.end(), [](const CombatUnitPlacement& u) {
            return u.active && u.battle_gauge == 0xFF;
        })) return;
    if (!etc_icons_tried_) load_etc_icons();
    if (!etc_icons_loaded_) return;

    const int first_visible_x = (cam_fine_x_ < 6) ? 1 : 0;
    const int first_visible_y = (cam_fine_y_ + 6 < -4) ? 1 : 0;
    for (const CombatUnitPlacement& u : units_) {
        if (!u.active || u.battle_gauge != 0xFF) continue;

        const int tile_x = u.x - cam_tile_x_;
        const int tile_y = u.y - cam_tile_y_;
        // The original scans a 12x12 tile window, skipping an edge tile when its
        // icon would start outside the visible viewport.
        if (tile_x < first_visible_x || tile_x >= first_visible_x + 12 ||
            tile_y < first_visible_y || tile_y >= first_visible_y + 12) continue;

        // sub_12161 passes (fineX + tileX*24, fineY + tileY*16 + 6)
        // directly to sub_5EB00, placing the icon at the unit tile's lower-left.
        const int sx = tile_x * TOWN_TILE_W + cam_fine_x_;
        const int sy = tile_y * TOWN_TILE_H + cam_fine_y_ + 6;
        // Use the modeled effective army; unknown group-0/VM Faction paths use the helper's raw+1
        // fallback.
        const int effective_army = battle_effective_army(u);
        const bool first_variant = u.auto_battle == 0 && effective_army == 1;
        const int frame = (first_variant ? 0 : 4) + ai_marker_phase_;
        const SpriteEntry* sp = etc_icons_.get(frame);
        if (sp && sp->pixel_data) blit_sprite_rle(sx, sy, sp->pixel_data);
    }
}

void CombatII::render_battle_state_overlay() {
    if (battle_overlay_mode_ != 1) return;
    // sub_454D5 tail (loc_45472) + sub_45BBF head:
    //   F042E==1 -> sub_45BBF: ETC #8 (dword_D6630) at (0x28,0xAA) + gauge/time.
    //   F042E==2 -> ETC #9 (dword_D663C) at the same spot, F042E==3 -> ETC #10 (dword_D6648).
    int idx = -1;
    if (battle_state_ == 1) idx = 8;   // Player Phase (sub_45BBF head)
    else if (battle_state_ == 2) idx = 9;
    else if (battle_state_ == 3) idx = 10;
    else return;
    if (!etc_icons_tried_) load_etc_icons();
    if (etc_icons_loaded_) {
        const SpriteEntry* sp = etc_icons_.get(idx);
        if (sp && sp->pixel_data) blit_sprite_rle(0x28, 0xAA, sp->pixel_data);
    }
    // sub_45BBF tail: gauge/clock only when F042E == 1; states 2/3 show the banner only.
    //   F042F == 1 -> sub_45844 clock (tod_phase_), otherwise sub_45686 TP gauge (battle_tp()).
    if (battle_state_ != 1) return;
    if (battle_flag_420f_ == 1) render_battle_clock(); // sub_197C2 -> 45844
    else render_tp_gauge();                            // sub_1A0E9 -> 45686
}

bool CombatII::load_time_gra() {
    time_gra_tried_ = true;
    time_gra_loaded_ = false;
    std::string cand = game_path("TIME_GRA.SYB");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "TIME_GRA.SYB");
    if (cand.empty() || !file_exists(cand.c_str())) {
        cand = game_path("TIME_GRA.SYF");
        if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "TIME_GRA.SYF");
    }
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: TIME_GRA.SYB/SYF not found (clock dial/hands skipped)\n");
        return false;
    }
    if (!time_gra_.load(cand) || time_gra_.count() < 13) {
        fprintf(stderr, "COMBAT: warning: TIME_GRA load failed or too few sprites (%d, %s)\n",
                time_gra_.count(), cand.c_str());
        return false;
    }
    time_gra_loaded_ = true;
    fprintf(stderr, "COMBAT: TIME_GRA loaded %d sprites (%s, flat 5EE51: #0 gauge tail, #4 dial, #5..12 hands)\n",
            time_gra_.count(), cand.c_str());
    return true;
}

// sub_45686 (dasm 71899-72052): TP gauge disc.
//   Skipped when word_7500E == 0. Centre = (75008>>16, 7500C-6) =
//   (battle_gauge_x_, battle_gauge_y_-6).
//   Over the r=22 shaded background ellipse (6104A(0)+5ECC7(1)+6075A) it paints the TP band
//   (base: >300 -> 0x20, >200 -> 0x50, >100 -> 0x30, else 0x10) and then the core circle
//   (r=n, colour base+shade) driven by the charge amount (n = ceil((TP%100)/5), 20 when 0)
//   plus the outer rings (3-shade of them, r = 26-(shade+1)*8-8k, colours
//   base+shade+1..base+3) with solid fills, and finally blits TIME_GRA #0 flat through 5EE51
//   (72039-72050: X+xo0, Y+yo0).
//   shade: 1..2 -> 3, <=10 -> 2, <=18 -> 1, else 0. TIME_GRA.SYB is flat, not RLE.
void CombatII::render_tp_gauge() {
    if (!battle_gauge_visible_) return; // cmp word_7500E,0; jz
    const int cx = battle_gauge_x_;
    const int cy = battle_gauge_y_ - 6; // sub eax,6
    // Background: shade mode r=22 (dialog_shade_lut row 0, used inside ellipse_shade_fill).
    ellipse_shade_fill(cx, cy, 0x16, 0x16);
    // TP band (71928-71943: a jle chain, so boundary values fall into the lower band).
    const int tp = (int)(int16_t)battle_tp(); // sub_1A0E9 (dword_F0426+2)
    const int16_t si = (int16_t)tp;
    int base;
    if (si > 0x12C) base = 0x20;
    else if (si > 0xC8) base = 0x50;
    else if (si > 0x64) base = 0x30;
    else base = 0x10;
    // Charge amount (71946-71969: rem = TP%100, n = ceil(rem/5), 20 when 0).
    int rem = si % 100; // idiv truncates toward zero; TP is non-negative here
    if (rem < 0) rem = -rem;
    int q = rem / 5, r = rem % 5;
    int n = q + (r != 0 ? 1 : 0);
    if (n == 0) n = 0x14;
    int shade;
    if (n >= 1 && n <= 2) shade = 3;
    else if (n <= 0x0A) shade = 2;
    else if (n <= 0x12) shade = 1;
    else shade = 0;
    // Core circle (71993-72005: rx=ry=n, colour base+shade).
    ellipse_solid_fill(cx, cy, n, n, (uint8_t)(base + shade));
    // Outer rings (72006-72038: start = shade+1..3, r = 0x1A-start*8-8k).
    int start = shade + 1;
    int si_c = base + start;
    int di_r = 0x1A - start * 8;
    const int end = base + 4;
    while (si_c < end) {
        ellipse_solid_fill(cx, cy, di_r, di_r, (uint8_t)si_c);
        si_c++;
        di_r -= 8;
    }
    // Trailing flat (72039-72050: dword_FA6EC/FA6EA/FA6F0 = TIME_GRA #0 xo/yo/ptr).
    // X = 75008>>16+xo0, Y = 75008+2>>16+yo0 - relative to the gauge, not the ellipse centre
    // (Y-6). SPRParser puts the header xo/yo in x_offset/y_offset and the w/h+flat payload at
    // the head of pixel_data (the 5EE51 convention).
    if (!time_gra_tried_) load_time_gra();
    if (time_gra_loaded_) {
        if (const SpriteEntry* tail = time_gra_.get(0))
            blit_flat_transparent(battle_gauge_x_ + (int)tail->x_offset,
                                  battle_gauge_y_ + (int)tail->y_offset, tail->pixel_data);
        else if (battle_trace_)
            fprintf(stderr, "COMBAT: warning: sub_45686 tail entry 0 missing (count=%d)\n", time_gra_.count());
    }
    if (battle_trace_) {
        static int last_tp = -1;
        if (tp != last_tp) {
            last_tp = tp;
            fprintf(stderr, "COMBAT: sub_45686 TP=%d base=0x%02X n=%d shade=%d at (%d,%d)\n",
                    tp, base, n, shade, cx, cy);
        }
    }
}

// sub_45844 (dasm 72053-72233): clock dial, hands and date/time text.
//   Skipped when word_7500E == 0. Input si = tod_phase_ (0..15, byte_FDF84).
//   Divides F0418 (battle_time_) by BA84E (map kind) to build the hour:minute (ecx:edi) and
//   draws the dial (entry 4), hand (entry 5+phase/2), date ("%04d %02d") and time
//   ("%02d:%02d") at offsets from (75008>>16, 75008+2>>16).
//   Dial offsets (FA71A/FA71C/FA720 = TIME_GRA #4 xo/yo/ptr) and hand offsets
//   (FA6EA/FA6EC/FA6F0[eax*4], eax = (phase/2+5)*3 -> #5+phase/2) are blitted flat through
//   5EE51 at X+xo, Y+yo. TIME_GRA is flat, not RLE.
//   Text position and colour follow the original:
//   date at (X-0x15, Y-1) colour 1, time at (X-0x0F, Y+0x0A) colour 0x18 (60944 -> 608E6).
void CombatII::render_battle_clock() {
    if (!battle_gauge_visible_) return; // cmp word_7500E,0; jz
    const int phase = (int)tod_phase_ & 0x0F; // sub_197C2 (byte_FDF84)
    const uint32_t F = battle_time_;          // sub_19D47 (dword_F0418)
    const int kind = (int)sce_map_kind_;      // byte_BA84E
    int edi, ecx;
    if (kind == 4 || kind == 5) {
        edi = (int)(F % 0x0A); // div 0x0A
        int q = edi / 10; (void)q;
        ecx = edi / 10; // (F%10)/10 == 0, as in the original
    } else {
        edi = (int)(F % 0x384); // div 0x384 (900)
        ecx = edi / 0x258;      // /0x258 (600) -> 0..1
    }
    // Si < 8 / >= 8 branch (72089-72112): (phase/2)*3 +/- 0x0C.
    int half = phase / 2; // same as the sar trick (phase >= 0)
    if (phase < 8) ecx += half * 3 + 0x0C;
    else ecx += half * 3 - 0x0C;
    if (kind == 4 || kind == 5) edi *= 6; // imul 6
    else {
        int r1 = edi % 0x258;
        edi = (r1 % 0x258) / 0x0A; // ((F%900)%600)/10 -> 0..59
    }
    edi += (phase % 2) * 30; // (si%2)*15*2
    if (edi > 0x3C) { ecx++; edi -= 0x3C; } // 60 carry
    const int base_x = battle_gauge_x_; // dword_75008>>16
    const int base_y = battle_gauge_y_; // dword_75008+2>>16 (word_7500C)
    // Dial and hand (TIME_GRA flat 5EE51: #4 dial, #5..12 hands for 8 directions, X+xo/Y+yo).
    if (!time_gra_tried_) load_time_gra();
    if (time_gra_loaded_) {
        if (const SpriteEntry* dial = time_gra_.get(4)) {
            blit_flat_transparent(base_x + (int)dial->x_offset, base_y + (int)dial->y_offset,
                                  dial->pixel_data);
        } else if (battle_trace_) {
            fprintf(stderr, "COMBAT: warning: sub_45844 dial entry 4 missing (count=%d)\n", time_gra_.count());
        }
        int e = phase / 2 + 5; // 5..12
        if (const SpriteEntry* hand = time_gra_.get(e)) {
            blit_flat_transparent(base_x + (int)hand->x_offset, base_y + (int)hand->y_offset,
                                  hand->pixel_data);
        } else if (battle_trace_) {
            fprintf(stderr, "COMBAT: warning: sub_45844 hand entry %d missing (count=%d)\n", e, time_gra_.count());
        }
    }
    // Date: FDF82 (tod_day_) -> YYYY = FDF82/0xDC+0x4B7, DD = FDF82%0xDC+1.
    int yyyy = (int)tod_day_ / 0xDC + 0x4B7;
    int dd = (int)tod_day_ % 0xDC + 1;
    char date[32], hm[32];
    snprintf(date, sizeof(date), "%04d %02d", yyyy, dd);
    snprintf(hm, sizeof(hm), "%02d:%02d", ecx, edi);
    if (!small_font_tried_) load_small_font();
    if (small_font_loaded_) {
        draw_small_text(base_x - 0x15, base_y - 1, date, 1, 1);      // 60944 (1,1)
        draw_small_text(base_x - 0x0F, base_y + 0x0A, hm, 0x18, 1);  // 60944 (1,0x18)
    } else {
        uint8_t* dest = video_.get_backbuffer();
        if (dest && font_.is_loaded()) {
            font_.set_text_mode(4, 0x1000, 0xFF);
            font_.draw_string(dest, SCREEN_W, SCREEN_H, base_x - 0x15, base_y - 1, date);
            font_.draw_string(dest, SCREEN_W, SCREEN_H, base_x - 0x0F, base_y + 0x0A, hm);
            font_.set_text_mode(4, 0x1000, 0xFF);
        }
    }
    if (battle_trace_) {
        static int last_phase = -1;
        static uint32_t last_F = 0xFFFFFFFFu;
        if (phase != last_phase || F != last_F) {
            last_phase = phase; last_F = F;
            fprintf(stderr, "COMBAT: sub_45844 phase=%d time=%u kind=%d %s %s at (%d,%d)\n",
                    phase, F, kind, date, hm, base_x, base_y);
        }
    }
}
