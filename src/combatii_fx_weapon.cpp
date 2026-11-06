// Weapon effect drivers for the V-weapon (normal-tier) family: sub_2CBA1, sub_2CC3A,
// sub_341C7, sub_37FA4 and sub_32589.
//
// All of them register units through sub_44D89, and sub_454D5 (battle_fx_overlay) redraws
// those units through sub_44DCC above the effect and the dim layer.
//
//   sub_2CBA1(caster)   registers every unit in y order ([unit+1]) with the caster inserted
//                       at its own row: rows <= caster row, the caster, then the rest.
//   sub_2CC3A(edx)      registers the entire F0648 list built by sub_1D124 (39762~39772).
//   sub_341C7           registers the whole F0648 list gathered from the 12x12 screen window.
//   sub_37FA4           places the caster before or after the single target by facing:
//                       facing 0 (south) puts the caster first, anything else last.
//
// Frames always follow the original order:
//   battle_fx_world() -> battle_fx_overlay() -> [effect blits] -> battle_fx_commit()
// sub_454B3 falls through into sub_454D5 (ASM 45259), so one call draws the map, the dim
// layer, the registered units and the HUD. Dim darkens map and units first, 44DCC redraws
// the registered units above dim, and the effect goes on top - that is why 44D89 registers.
// 5E9D0 clears the staging buffer, used by effects shown on a black screen; those blocks
// call video_.clear() and render_highlight_units_direct() (44DCC without the map) instead of
// battle_fx_world.
//
// Original routines and their port equivalents:
//   5F3FD/5F517  NC2 strip blitters           -> blit_nc2_frame / blit_nc2_frame_solid
//   5EFB9        scaled and clipped NSP blit  -> blit_flat_transparent at the original size
//                                              (there is no scaler; the blitter clips)
//   5EECF        save/restore a screen rect   -> the map is redrawn every frame instead
//   5F945/5FC21/5FD1C/5F9AE  palette snapshot/mix/apply -> fx_pal_mix, palette_set_entry
//   5ECC7(al)    scanline primitive selector  -> mode 1 (LUT pass) only in 341C7 (9)
//   6104A/6106C  LUT select/build             -> fx_shade_lut_build, same
//                                              build_shade_row_original as the dialogue shade
//   5EDDE        filled/LUT rectangle         -> fx_shade_rect
//   dword_30528 (341C7 (9) rock velocities)   -> kRockVel: word pairs
//                                              {0,-1} {-1,0} {0,0} {1,0} {-1,1} {1,1} {0,1}
//   dword_F0EDE/F0EEA/F0EF6 are the [+8] fields of NSP entries 25/26/27
//   (F0DAA + 12k + 8), so the port uses frames 25/26/27.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- sub_2CBA1: sub_44D89 in row (y) order ----
//   xor ebx,ebx / cmp bx,[F0B9E] / mov eax,F0648[bx] / cl = caster[1] / cmp cl,[eax+1] / jl skip
//   registers units with caster[1] >= unit[1], then the caster itself, then the units with
//   caster[1] < unit[1]. Registration order is draw order, sorted around the caster row.
void CombatII::battle_highlight_add_depth(int caster_index) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    const int caster_y = (int)units_[(size_t)caster_index].y;
    for (int i = 0; i < (int)units_.size(); ++i) {
        if (!units_[(size_t)i].active) continue;
        if ((int)units_[(size_t)i].y <= caster_y) battle_highlight_add(i);
    }
    battle_highlight_add(caster_index);
    for (int i = 0; i < (int)units_.size(); ++i) {
        if (!units_[(size_t)i].active) continue;
        if ((int)units_[(size_t)i].y > caster_y) battle_highlight_add(i);
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2CBA1 caster=%d y=%d -> %zu registered\n",
                units_[(size_t)caster_index].id, caster_y, battle_highlight_units_.size());
}

// ---- sub_44DCC without the map ----
//   The original draws the unit pointers of dword_F9F08 through sub_12F2A, with or without a
//   map behind them. render_highlight_units replays the draw entries built by
//   render_battle_map, which are empty on a black screen, so this path draws directly.
void CombatII::render_highlight_units_direct() {
    for (int ui : battle_highlight_units_) battle_draw_unit_now(ui);
}

// ---- sub_12F2A: redraw one unit ----
//   Uses the coordinate convention of the render_battle_map unit pass (UNIT_ANCHOR_X/Y plus
//   walk offsets) and draws only the sprite and its ground shadow. Sink/decal/clip need the
//   terrain, and 305A2 only overlays a unit on an effect frame.
void CombatII::battle_draw_unit_now(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    if (!u.active || u.drop_wait > 0) return;
    const int sx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + u.walk_off_x;
    const int sy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y + u.walk_off_y;
    if (const NC2Frame* fr = nc2_frame_for(u.sprite_type, u.facing & 3, (int)u.anim_frame)) {
        if (sce_map_kind_ < 4 && !u.flying && fr->shadow_idx >= 0 && fr->shadow_idx < 7 &&
            !shadow_sprites_[fr->shadow_idx].rle.empty()) {
            const bool big = (u.sprite_type >= 0x12C);
            const int sh_x = big ? sx - 12 : sx + fr->sil_x;
            const int sh_y = big ? sy + 4 : sy + fr->sil_y + 12;
            blit_sprite_rle(sh_x, sh_y, shadow_sprites_[fr->shadow_idx].rle.data());
        }
        blit_nc2_frame(sx + fr->x_off, sy + fr->y_off, *fr);
        return;
    }
    // CITIZEN fallback (same facing block and pose wrap as render_battle_map).
    static const int kCitizenDirBlock[4] = {0, 1, 3, 2};
    if (!battle_sprites_loaded_ || battle_sprites_.count() <= 0) return;
    const int dir_block = kCitizenDirBlock[u.facing & 3];
    const int cpose = (u.anim_frame > 0) ? ((int)u.anim_frame - 1) : 0;
    int frame = (int)u.sprite_id * 16 + dir_block * 4 + (cpose & 3);
    if (frame >= battle_sprites_.count()) frame %= battle_sprites_.count();
    const auto* spr = battle_sprites_.get(frame);
    if (!spr || !spr->pixel_data) return;
    blit_sprite_rle(sx + spr->x_offset, sy + spr->y_offset, spr->pixel_data);
}

// sub_30544: reset the trail ring (F1BA0 = F1BA0+2 = 0).
void CombatII::fx_trail_reset() {
    fx_trail_start_ = 0;
    fx_trail_end_ = 0;
}

// sub_305A2(eax=unit, edx=pose): one afterimage trail frame.
//   (1) Redraw the queued slots from start to end (24 entries, modulo).
//   (2) unit == 0: draw the last unit (F1B9C) through sub_12F2A and advance start.
//   (3) Otherwise: [unit+64h] = pose, sub_12F2A(unit), then queue
//         dir  = ([unit+5Dh] >> 24) * 23Ch   (unit slot in unk_D66A8)
//         pose = dx + 11 * [unit+63h]        (13-byte entry in that slot)
//         x = sub_10971(unit.x) + 0Ch + entry[4]
//         y = sub_10988(unit.y) - 2 + entry[5]
//       at end, advance end and set F1B9C = unit.
//   The port uses nc2_frame_for(type, facing, pose) instead of the D66A8 table; the parser
//   keeps the same 13-byte record ([4]/[5] offsets, [9] sprite).
void CombatII::fx_trail_step(int unit_index, int pose) {
    for (int i = fx_trail_start_; i != fx_trail_end_; i = (i + 1) % 24) {
        const FxTrailSlot& s = fx_trail_[(size_t)i];
        if (s.fr) blit_nc2_frame(s.x, s.y, *s.fr);
    }
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) {
        battle_draw_unit_now(fx_trail_last_unit_);
        fx_trail_start_ = (fx_trail_start_ + 1) % 24;
        return;
    }
    auto& u = units_[(size_t)unit_index];
    if (pose >= 0) u.anim_frame = (uint8_t)(pose & 0xFF);
    battle_draw_unit_now(unit_index);
    const NC2Frame* fr = nc2_frame_for(u.sprite_type, u.facing & 3, (int)u.anim_frame);
    if (!fr) return;
    const int sx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;  // 10971+0Ch
    const int sy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;  // 10988-2
    FxTrailSlot& slot = fx_trail_[(size_t)fx_trail_end_];
    slot.x = sx + fr->x_off;
    slot.y = sy + fr->y_off;
    slot.fr = fr;
    fx_trail_last_unit_ = unit_index;
    fx_trail_end_ = (fx_trail_end_ + 1) % 24;
}

// sub_306E6: add an afterimage at an explicit coordinate.
//   The D66A8 entry is 13*(11*facing + ecx) with ecx = [unit+64h]; the ring write matches
//   sub_305A2 (F153A/F156A = x+[4]/y+[5], F14DC = [9], F1B9C = unit). sub_305A2 derives the
//   coordinate from the unit tile, this one takes the mid-dash coordinate.
void CombatII::fx_trail_add_at(int unit_index, int x, int y) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    const NC2Frame* fr = nc2_frame_for(u.sprite_type, u.facing & 3, (int)u.anim_frame);
    if (!fr) return;
    FxTrailSlot& slot = fx_trail_[(size_t)fx_trail_end_];
    slot.x = x + fr->x_off;
    slot.y = y + fr->y_off;
    slot.fr = fr;
    fx_trail_last_unit_ = unit_index;
    fx_trail_end_ = (fx_trail_end_ + 1) % 24;
}

// sub_30784: start = (last slot + 1) % 24, dropping the oldest afterimage.
//   The original recomputes start as `(dword_F1B9C+2 >> 16 + 1) % 24`; an empty ring
//   (start == end) has nothing to drop, so it stays as it is.
void CombatII::fx_trail_drop() {
    if (fx_trail_start_ == fx_trail_end_) return;
    fx_trail_start_ = (fx_trail_start_ + 1) % 24;
}

// sub_30824 / sub_307A6: mix a master-palette range toward colour K by pct%.
//   5F945 snapshots the master palette, 5FC21 builds (K - master) and 5FD1C applies pct.
//   30824 = 0x60..0xFF, 307A6 = 0x00..0x68; K = 0 is a plain darkening.
void CombatII::fx_pal_mix(int start, int end, int color_index, int pct) {
    const int k = color_index & 0xFF;
    const int kr = battle_palette_[(size_t)k][0];
    const int kg = battle_palette_[(size_t)k][1];
    const int kb = battle_palette_[(size_t)k][2];
    const int p = pct > 100 ? 100 : (pct < 0 ? 0 : pct);       // cmp arg,64h; jle
    for (int i = start; i < end && i < 0x100; ++i) {
        const int dr = (int)(int8_t)(uint8_t)(kr - battle_palette_[(size_t)i][0]);
        const int dg = (int)(int8_t)(uint8_t)(kg - battle_palette_[(size_t)i][1]);
        const int db = (int)(int8_t)(uint8_t)(kb - battle_palette_[(size_t)i][2]);
        palette_set_entry((uint8_t)i,
                          (uint8_t)(battle_palette_[(size_t)i][0] + dr * p / 100),
                          (uint8_t)(battle_palette_[(size_t)i][1] + dg * p / 100),
                          (uint8_t)(battle_palette_[(size_t)i][2] + db * p / 100));
    }
}

// sub_5EE51 / sub_5EFB9: blit one unk_F0DAA frame transparent at (x, y). Most NSP sites use
// 5EFB9, whose 13Fh/C7h clip is reproduced here (5EE51 itself is unclipped; the port keeps
// the same safe clip for it).
void CombatII::fx_blit_nsp(int frame, int x, int y) {
    if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
    const MagicFxFrame& fr = magic_frames_[(size_t)frame];
    if (fr.flat.empty()) return;
    static std::vector<uint8_t> buf;
    buf.resize(4 + fr.flat.size());
    buf[0] = (uint8_t)(fr.w & 0xFF); buf[1] = (uint8_t)(fr.w >> 8);
    buf[2] = (uint8_t)(fr.h & 0xFF); buf[3] = (uint8_t)(fr.h >> 8);
    std::memcpy(buf.data() + 4, fr.flat.data(), fr.flat.size());
    blit_flat_clip319(x, y, buf.data());
}

// sub_6106C(page=3, colour, brightness%): the shade LUT used by 341C7 (9).
//   The original switches the fill primitive to the LUT variant (5ECC7(1) -> 5EE2B),
//   selects row 3 (6104A(3)) and paints 5EDDE rectangles, so the rectangle passes existing
//   pixels through the LUT instead of writing a colour. With dword_128558 = 100 - 46h = 1Eh
//   and effect colour = pal[11h] * 46h / 100, entry[i] is the nearest palette match of
//   pal[i] * 1Eh / 100 + effect colour + 9. build_shade_row_original matches 6106C exactly.
void CombatII::fx_shade_lut_build(int color_index, int shade_pct) {
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();   // LUT base = unk_12824C (weather applied)
    build_shade_row_original(dlg_shade_pal_, color_index & 0xFF, shade_pct, fx_shade_lut_);
}

// sub_5EDDE + sub_5EE2B(mode 1): pass every pixel of a rectangle through the LUT.
//   The original selects the scanline primitive through dword_755E2 (5ECC7) and calls it with
//   (x, y, length). Arguments are ax/bx = x0/x1 and dx/cx = y0/y1; both pairs are sorted, then
//   ecx = y1-y0 and ebx = x1-x0. The end coordinates are exclusive.
void CombatII::fx_shade_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= SCREEN_H) continue;
        uint8_t* row = dest + (size_t)y * SCREEN_W;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= SCREEN_W) continue;
            row[x] = fx_shade_lut_[row[x]];
        }
    }
}

// sub_308A3: lightning pillar.
//   eax = x, edx = y, ebx = sprite. The same sprite is stacked four times at (x, y + 40h*k)
//   to build a column across the screen. The original falls into 5EFB9 (clip) when y leaves
//   the screen band; the port blitter already clips, so the path is the same.
void CombatII::fx_bolt_strip(int x, int y, int frame) {
    for (int k = 0; k < 4; ++k) fx_blit_nsp(frame, x, y + 0x40 * k);
}

// ==== sub_2CC3A: V-weapon normal-tier animation ====
//   Precondition: sub_1D124 already filled battle_target_list_.
//   Original order:
//     (1) register the whole F0648 list through sub_44D89          (39762~39772)
//     (2) sub_11F74(x,y,0) releases the caster tile, [+64h] = 5    (39776~39785)
//     (3) cache the five swing poses
//         (entry = D66A8 + 23Ch*slot + 13*(11*facing+5+i)):
//         x = 10971+0Ch + entry[4], y = 10988-2 + entry[5]         (39795~39946)
//     (4) sub_44D6E(1) dim + sub_1033B(0Ch)                        (39948~39954)
//     (5) variant (5/7/0Ch) frames:                                (loc_2CEF9)
//         sub_454B3 -> trail silhouettes (5F517, colour (frame+1)%5) -> current pose (5F3FD)
//         -> switch on frame%5 -> shake all units when frame%5==2 -> 5E9E8 -> 499E4;
//         trail length min(6, frame+1)
//     (6) repeat the same drawing until the trail reaches 0            (loc_2D2A1)
//     (7) sub_499DA(40h) -> sub_44D75(clear list) -> sub_44D6E(0) -> [+64h] = frame+5
//   Returns the [+64h] value set in (7), the pose the caller's damage loop starts from.
int CombatII::battle_fx_2cc3a_anim(int unit_index, int variant, int sfx_id) {
    auto& A = units_[(size_t)unit_index];
    // (1) Register every target; the original inserts F0648 in order, duplicates included.
    for (int ti : battle_target_list_) battle_highlight_add(ti);
    // (2) Caster pose 5; tile occupation is modelled by units_ and skipped here.
    A.anim_frame = 5;
    // (3) Coordinates of the five swing poses.
    const int sx = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
    const int sy = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
    const NC2Frame* swing[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 5; ++i) swing[i] = nc2_frame_for(A.sprite_type, A.facing & 3, 5 + i);
    // (4) Dim and tick period.
    battle_dim_row0_ = true;                                     // sub_44D6E(1)
    game_timer_set(0x0C);                                        // sub_1033B(0Ch)
    int frames = 0;                                              // var_2C
    int trail = 0;                                               // di
    for (; frames < variant && !should_exit_; ++frames) {
        battle_fx_world();                                       // sub_454B3 (10AEB,12BEE,4512E)
        battle_fx_overlay();                                     // tail of 454B3 = 454D5 (dim, 44DCC, HUD)
        const int color = (uint8_t)((frames + 1) % 5);           // var_1C
        // Trail silhouettes: the most recent pose first, trail frames deep.
        for (int k = 0; k < trail; ++k) {
            const int pi = ((frames - k) % 5 + 5) % 5;
            const NC2Frame* fr = swing[pi];
            if (!fr) continue;
            blit_nc2_frame_solid(sx + fr->x_off, sy + fr->y_off, *fr, (uint8_t)color); // sub_5F517
        }
        // Current pose, one frame (sub_5F3FD).
        if (const NC2Frame* cf = nc2_frame_for(A.sprite_type, A.facing & 3, 5 + frames % 5))
            blit_nc2_frame(sx + cf->x_off, sy + cf->y_off, *cf);
        // Switch on frame % 5 (loc_2CFCD/loc_2D092/loc_2D0C4/loc_2D183).
        //   case 0/2: 4F4CD(cx) plus one blinking frame when var_24 == 1.
        //      var_24 = (unk_BA8AD + 40*slot + 3 != 0FFh) is a per-slot data byte; the port has
        //      no such table and treats it as always true. The original body is
        //      5EECF(save) -> 5F3FD(pose) -> 5E9E8 -> 499E4 -> 5EE51(restore) -> 5F3FD(next).
        //      The port redraws the world every frame, so showing the pose for one more frame
        //      is one commit + tick.
        //   case 1/3: all units [+64h] = 3 (hit pose); case 3 only when the trail is at least 3.
        switch (frames % 5) {
            case 0:
            case 2:
                sfx(sfx_id);                                          // 4F4CD([esp+66h]>>16 = cx)
                battle_fx_commit();                                   // 5F3FD -> 5E9E8
                wait_game_tick();                                     // 499E4
                if (const NC2Frame* cf = nc2_frame_for(A.sprite_type, A.facing & 3, 5 + frames % 5))
                    blit_nc2_frame(sx + cf->x_off, sy + cf->y_off, *cf);   // 5EE51 + 5F3FD(next pose)
                break;
            case 1:
                for (size_t i = 0; i < units_.size(); ++i)
                    if (units_[i].active && units_[i].sprite_type < 0x12C) units_[i].anim_frame = 3;
                break;
            case 3:
                if (trail > 2)
                    for (size_t i = 0; i < units_.size(); ++i)
                        if (units_[i].active && units_[i].sprite_type < 0x12C) units_[i].anim_frame = 3;
                break;
            default: break;   // case 4 does not exist
        }
        // Shake (loc_2D1B8): when frame%5 == 2 or (frame%5 == 0 and trail > 2), every unit is
        // moved by 2 or 3 pixels according to the caster facing (the whole screen shakes).
        if ((frames % 5) == 2 || ((frames % 5) == 0 && trail > 2)) {
            for (size_t i = 0; i < units_.size(); ++i) {
                auto& u = units_[i];
                if (!u.active) continue;
                u.walk_off_x = 0;
                u.walk_off_y = 0;
                switch (A.facing & 3) {
                    case 0: u.walk_off_y += 2; break;   // south: [+66h] += 2
                    case 1: u.walk_off_x -= 3; break;   // west:  [+65h] -= 3
                    case 2: u.walk_off_y -= 2; break;   // north: [+66h] -= 2
                    default: u.walk_off_x += 3; break;  // east:  [+65h] += 3
                }
            }
            wait_game_tick();                                    // 499E4 at loc_2D22C
        }
        if (trail < 6) ++trail;                                  // cmp di,6; inc
        battle_fx_commit();                                      // sub_5E9E8
        wait_game_tick();                                        // sub_499E4
    }
    // (6) Drain the trail (loc_2D2A1): repeat the same drawing until it reaches 0.
    while (trail > 0 && !should_exit_) {
        battle_fx_world();                                       // sub_454B3
        battle_fx_overlay();                                     // 454D5
        for (int k = 0; k < trail; ++k) {
            const int pi = ((frames - k) % 5 + 5) % 5;
            if (!swing[pi]) continue;
            blit_nc2_frame_solid(sx + swing[pi]->x_off, sy + swing[pi]->y_off, *swing[pi],
                                 (uint8_t)((frames + 1) % 5));
        }
        if (const NC2Frame* cf = nc2_frame_for(A.sprite_type, A.facing & 3, 5 + frames % 5))
            blit_nc2_frame(sx + cf->x_off, sy + cf->y_off, *cf);
        --trail;
        battle_fx_commit();                                      // sub_5E9E8
        wait_game_tick();                                        // sub_499E4
    }
    // (7) Wrap up.
    game_timer_set(0x40);                                        // sub_499DA(40h)
    battle_highlight_clear();                                    // sub_44D75
    battle_dim_row0_ = false;                                    // sub_44D6E(0)
    for (size_t i = 0; i < units_.size(); ++i) {                 // restore the shake offsets
        if (!units_[i].active) continue;
        units_[i].walk_off_x = 0;
        units_[i].walk_off_y = 0;
    }
    const int pose = (frames + 1) % 5 + 5;                       // [64h] = var_10 + 5
    A.anim_frame = (uint8_t)pose;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2CC3A unit=%d variant=%d trail-frames=%d pose=%d\n",
                A.id, variant, frames, pose);
    return pose;
}

// ==== sub_341C7: f_quake.nsp earthquake ====
//   Precondition: the units of the 12x12 screen window are in battle_target_list_, gathered
//   like battle_weapon_collect_screen. Original order:
//     (1) register the whole F0648 list through sub_44D89              (49072~49080)
//     (2) sub_44D6E(1) dim, sub_30544 (reset trail), sub_1033B(10h)
//     (3) 8 frames = sub_305A2(caster, pose) for poses 0..3 then sub_305A2(0,0) four times.
//         The port uses i as the pose; the original edx is a word of the collected array
//         [esp+31Ah] that 305A2 stores into [64h]. Each frame:
//         454B3 -> 305A2 -> 5E9E8 -> 30824(0,0Ch) -> 499E4            (loc_34302/loc_3436B)
//     (4) sub_44D6E(0) restore, 4F4CD(32h)
//     (5) 6 frames: 5E9D0(clear) -> 44DCC -> 5EE51(NSP 6+i) -> 5EFB9(NSP i, y-5Ah)
//         -> 12F2A(caster) -> 5E9E8 -> 499E4                          (loc_343ED)
//     (6) 7 frames: 5E9D0 -> 44DCC -> 5EE51(NSP (i&1)+0Ah) -> 12F2A -> 5E9E8 -> 499E4,
//         pose = i+6, caster x moves by -8 and -4                       (loc_344A1)
//     (7) 3 frames: 5E9D0 -> 44DCC -> 5EE51(NSP 11 = F0E30, entry 11+2) -> [64h]=i -> 12F2A
//         -> 5E9E8 -> 499E4                                             (loc_345BC)
//     (8) 2 frames: 5E9D0 -> 44DCC -> 5EE51(NSP 0Ch+i, y+0Ch) -> 5EFB9(NSP 0Eh+i)
//         -> 5E9E8 -> 499E4                                             (loc_346B7)
//     (9) 0x40 (64) frames of falling rocks, see the (9) block below   (loc_34785)
//   Frame total = 8+6+7+3+2+64 = 90. Damage in the tail (28728/287AF) belongs to
//   battle_weapon_2caf7_loop; the palette ramp is replaced by an immediate restore.
void CombatII::battle_fx_341c7_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    for (int ti : battle_target_list_) battle_highlight_add(ti);      // (1)
    // ASM 3429B: save [63h] and set it to 0 (south = frontal pose for the whole effect).
    const int8_t saved_facing = A.facing;
    A.facing = 0;
    battle_dim_row0_ = true;                                          // (2) 44D6E(1)
    fx_trail_reset();                                                 // 30544
    game_timer_set(0x10);                                             // 1033B(10h)
    // The original reads the pose with `mov edx,[esp+edx*2+31Ah]`, which overlaps the 8 bytes
    // of off_30520 (`0x00070005` + `6`) copied in the prologue, so the four words are 5, 7, 6,
    // 0 instead of 0..3 (an original data mistake, but deterministic). After (3) [64h] is
    // therefore 0, and the units in (5) use pose 0.
    static constexpr int kQuakePose[4] = {5, 7, 6, 0};
    for (int i = 0; i < 8 && !should_exit_; ++i) {                    // (3)
        battle_fx_world();                                            // 454B3
        battle_fx_overlay();                                          // 454D5 (tail of 454B3: dim, 44DCC, HUD)
        if (i < 4) fx_trail_step(unit_index, kQuakePose[i]);          // 305A2(caster, pose)
        else fx_trail_step(-1, 0);                                    // 305A2(0,0)
        battle_fx_commit();                                           // 5E9E8
        fx_pal_mix(0x60, 0x100, 0, 0x0C);                             // 30824(0,0Ch)
        wait_game_tick();                                             // 499E4
    }

    battle_dim_row0_ = false;                                         // (4) 44D6E(0)
    sfx(0x32);                                                        // 4F4CD(32h)
    if (!battle_nsp_load("f_quake.nsp") || magic_frames_.empty()) {   // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 341C7 unit=%d f_quake.nsp missing\n", A.id);
        return;
    }
    const int nframes = (int)magic_frames_.size();
    const int sx = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
    const int sy = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
    // The original reloads the file through sub_1FEA4 on every cast, refilling unk_F0DAA.
    // Block (9) shifts entry [+4]/[+6] in place, so the cached frames are restored to their
    // parse-time offsets before reuse.
    for (MagicFxFrame& fr : magic_frames_) { fr.xo = fr.xo0; fr.yo = fr.yo0; }
    // The (5)(6)(7) anchor is si = 10971+4 = sx-8 / di = 10988-8 = sy-6, set at the end of
    // (4) by `mov esi, eax(add 4)` / `sub edi, 8`. (8) uses var_8/var_C = 10971+0Ch /
    // 10988-2 = sx/sy, so the two differ by 8px and 6px and must not be mixed.
    const int si_x = sx - 8;
    const int di_y = sy - 6;
    // (5) Effect and highlighted units on a black screen (loc_343ED, 6 frames):
    //   5E9D0 -> 44DCC -> 5EE51(6+i, si+xo, di+yo) -> 5EFB9(i, same x, y-5Ah) -> 12F2A
    //   -> 5E9E8 -> 499E4. 12F2A takes no argument and draws with the [64h] pose left by (3).
    for (int i = 0; i < 6 && !should_exit_; ++i) {
        video_.clear();                                               // 5E9D0(0, 0, 5DC0h)
        render_highlight_units_direct();                              // 44DCC
        const int f1 = 6 + i;
        if (f1 < nframes) {                                           // 5EE51
            const MagicFxFrame& a = magic_frames_[(size_t)f1];
            fx_blit_nsp(f1, si_x + a.xo, di_y + a.yo);
        }
        if (i < nframes) {                                            // 5EFB9 (y-5Ah)
            const MagicFxFrame& b = magic_frames_[(size_t)i];
            fx_blit_nsp(i, si_x + b.xo, di_y + b.yo - 0x5A);
        }
        battle_draw_unit_now(unit_index);                             // 12F2A
        battle_fx_commit();                                           // 5E9E8
        wait_game_tick();                                             // 499E4
    }

    // (6) 7 frames (loc_344A1): draw 5EE51(0Ah+(i&1)) at the cx/var_14 anchor and only
    //   afterwards apply [64h] = i+6 (when i < 2) and the cx/var_14 movement:
    //     i==0 -> cx -= 4; i==1 -> var_14 += 10h, cx -= 8; i>=2 -> none
    //   So the per-frame coordinates are x = si-{0,4,12,12,...}, y = di-{0,0,10h,10h,...}.
    int cx = si_x;                                                    // mov ecx, esi
    int var14 = di_y;                                                 // mov word ptr [var_14], di
    for (int i = 0; i < 7 && !should_exit_; ++i) {
        game_timer_set(0x20);                                         // 1033B(20h) every frame
        video_.clear();                                               // 5E9D0
        render_highlight_units_direct();                              // 44DCC
        const int f = 0x0A + (i & 1);                                 // (i & 1) + 0Ah
        if (f < nframes) {                                            // 5EE51
            const MagicFxFrame& a = magic_frames_[(size_t)f];
            fx_blit_nsp(f, cx + a.xo, var14 + a.yo);
        }
        battle_draw_unit_now(unit_index);                             // 12F2A ([64h] pose)
        battle_fx_commit();                                           // 5E9E8
        wait_game_tick();                                             // 499E4
        if (i < 2) A.anim_frame = (uint8_t)(i + 6);                   // [64h] = i + 6
        if (i == 1) { var14 += 0x10; cx -= 8; }                       // add [var_14],10h / sub ecx,8
        else if (i == 0) { cx -= 4; }                                 // sub ecx,4
        if (i == 4) sfx(0x33);                                        // 4F4CD(33h)
    }

    // (7) 3 frames (loc_345BC): 5E9D0 -> 44DCC -> 5EE51(NSP 11) -> 12F2A([64h]=8) -> 5E9E8
    //   -> 499E4. The original passes `unk_F0E30`, and F0E30 = F0DAA + 86h = entry 11 + 2
    //   (12*11 = 132 = 0x84, +2 = 0x86), so those fields are entry 11's [+4](x)/[+6](y)/
    //   [+8](sprite): it really draws NSP frame 11.
    //   Blit = (si + 9 + xo, di + blit_dx + yo), starting from [66h] = 0E9h (-23),
    //   [64h] = 8 and blit_dx = -31; i == 0 steps both by -1.
    A.walk_off_y = -23;                                               // mov byte ptr [ebp+66h], 0E9h
    sfx(0x33);                                                        // 4F4CD(33h)
    A.anim_frame = 8;                                                 // mov byte ptr [ebp+64h], 8
    int blit_dx = -0x1F;                                              // mov ecx, 0FFFFFFE1h
    for (int i = 0; i < 3 && !should_exit_; ++i) {
        video_.clear();                                               // 5E9D0
        render_highlight_units_direct();                              // 44DCC
        if (11 < nframes) {                                           // 5EE51 (F0E30 = entry 11+2)
            const MagicFxFrame& a = magic_frames_[11];
            fx_blit_nsp(11, si_x + 9 + a.xo, di_y + blit_dx + a.yo);
        }
        battle_draw_unit_now(unit_index);                             // 12F2A ([64h] = 8)
        battle_fx_commit();                                           // 5E9E8
        wait_game_tick();                                             // 499E4
        if (i == 0) {                                                 // test bx,bx / jnz
            A.walk_off_y -= 1;                                        // dec [ebp+66h]
            blit_dx -= 1;                                             // dec ecx (blit x)
        }
    }

    // The original keeps [66h] = 0E8h (-24) into (8) and (9), but those blocks only redraw
    // the highlighted targets, so the offset is cleared here to keep it out of the damage loop.
    A.walk_off_y = 0;
    A.walk_off_x = 0;

    // Between (7) and (8): sub_6106C(3, 11h, 46h) -> 4F4CD(35h). The LUT is the shade table
    // used by all of (9); 6104A(3) selects it at the start of (9).
    fx_shade_lut_build(0x11, 0x46);                                   // 6106C(3, 11h, 46h)
    sfx(0x35);                                                        // 4F4CD(35h)

    // (8) 2 frames (loc_346B7): 5E9D0 -> 44DCC -> 5EE51(NSP 0Ch+i, y+0Ch) -> 5EFB9(NSP 0Eh+i)
    //   -> 5E9E8 -> 499E4.
    //   The anchors (var_8/var_C) are recomputed as 10971+0Ch / 10988-2 before this block and
    //   the camera has not moved, so they equal the sx/sy used in (5).
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        video_.clear();                                               // 5E9D0
        render_highlight_units_direct();                              // 44DCC
        const int f1 = 0x0C + i;
        if (f1 < nframes) {                                           // 5EE51 (y + 0Ch)
            const MagicFxFrame& a = magic_frames_[(size_t)f1];
            fx_blit_nsp(f1, sx + a.xo, sy + a.yo + 0x0C);
        }
        const int f2 = 0x0E + i;
        if (f2 < nframes) {                                           // 5EFB9
            const MagicFxFrame& b = magic_frames_[(size_t)f2];
            fx_blit_nsp(f2, sx + b.xo, sy + b.yo);
        }
        battle_fx_commit();                                           // 5E9E8
        wait_game_tick();                                             // 499E4
    }

    // (9) Entry: 1033B(9) -> 6104A(3) (the LUT built above) -> 5ECC7(1) (5EDDE in LUT mode).
    //   The tail restores 5ECC7(0), so other 5EDDE users (dialogue dimming) are unaffected.
    game_timer_set(9);                                                // 1033B(9)

    // (9) 0x40 (64) frames of falling rocks (loc_34785). Every frame:
    //     shake (cam_fine_x = rand(3)-1) -> 5E9D0(clear) -> loop over 7 particles -> 5E9E8
    //     -> 499E4. When si > 3Ah the 307A6(10h,24h) palette ramp runs; 4F4CD fires at
    //   si == 0Ah and on every multiple of 12. Particle i draws unk_F0DAA[12h+i]
    //   (frames 18..24) at anchor + [4]/[6] and pushes [4]/[6] by +-rand(2) each frame using
    //   dword_30528 (7 word pairs: x = {0,-1,0,1,-1,1,0}, y = {-1,0,0,0,1,1,1}). The original
    //   edits the F0DAA entries in place, so the movement accumulates; the port does the same
    //   on magic_frames_.
    //   Particles 0/3/5 also draw 5EDDA rectangles and 308A3 lightning pillars. The names
    //   dword_F0EDE/F0EEA/F0EF6 are the sprite pointers of NSP entries 25/26/27
    //   (F0DAA + 12k + 8), the same [+8] field the loader fills.
    // Anchors are computed once (var_8/var_C) before (9) and reused.
    // The shake (dword_BA838) only reaches the 44DCC path through 10971/10988, so the particle
    // coordinates do not shake. loc_34785 stores `var_10 = si & 1` and the particle y adds it,
    // so the rocks descend 1 pixel every other frame.
    const int ax0 = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
    const int ay0 = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
    static constexpr int kRockVel[7][2] = {{0,-1},{-1,0},{0,0},{1,0},{-1,1},{1,1},{0,1}};
    for (int si = 0; si < 0x40 && !should_exit_; ++si) {
        cam_fine_x_ = combat_rand(3) - 1;                             // 499BD(3)-1 (shake)
        const int jit = si & 1;                                       // var_10 = si&1 (y jitter)
        video_.clear();                                               // 5E9D0(0, 0, 5DC0h)
        for (int i = 0; i < 7; ++i) {
            const int fi = 0x12 + i;                                  // frames 18..24
            const int r = combat_rand(2);                             // 499BD(2) shared by ax/bx
            if (fi < nframes) {
                MagicFxFrame& e = magic_frames_[(size_t)fi];
                fx_blit_nsp(fi, ax0 + e.xo, ay0 + e.yo + jit);        // 5EE51
                if (si < 0x24) {                                      // cmp si,24h; no movement at or above
                    if (kRockVel[i][0] > 0) e.xo = (int16_t)(e.xo + r); // add [ecx+4], ax
                    else if (kRockVel[i][0] < 0) e.xo = (int16_t)(e.xo - r); // sub [ecx+4], ax
                    if (kRockVel[i][1] > 0) e.yo = (int16_t)(e.yo + r); // add [ecx+6], bx
                    else if (kRockVel[i][1] < 0) e.yo = (int16_t)(e.yo - r); // sub [ecx+6], bx
                }
            }
            if (i == 3) {                                              // loc_34920
                render_highlight_units_direct();                       // 44DCC
                const int f = 0x0E + combat_rand(4);                   // 0Eh + rand(4)
                if (f < nframes) {
                    const MagicFxFrame& s = magic_frames_[(size_t)f];
                    fx_blit_nsp(f, ax0 + s.xo, ay0 + s.yo);            // 5EFB9
                }
                fx_bolt_strip(0x1E, -si, 26);                          // 308A3 F0EEA
                fx_bolt_strip(0x46, -si, 26);
                fx_bolt_strip(0x10E, -si, 26);
                fx_bolt_strip(0x32, -si, 26);
                fx_bolt_strip(0xBE, -si, 26);
                if (si > 2) {                                          // cmp si,2
                    fx_shade_rect(0x7D, 0, 0x7F, 0x96);                // 5EDDE (mode 1)
                    fx_shade_rect(0xE2, 0, 0xF0, 0x96);
                }
                if (si > 0x0F) fx_shade_rect(0x77, 0, 0x7C, 0x96);     // cmp si,0Fh
            } else if (i == 0) {                                       // loc_34889
                fx_shade_rect(0x4C, 0, 0x4E, 0x78);                    // 5EDDE (first)
                fx_bolt_strip(0x8C, -si, 27);                          // 308A3 F0EF6
                fx_bolt_strip(0xC8, -si, 26);                          // 308A3 F0EEA
                if (si > 0x0C) {
                    fx_bolt_strip(0x1E, -si, 26);
                    fx_shade_rect(0x53, 0, 0x57, 0x78);
                    fx_shade_rect(0xFE, 0, 0x102, 0x78);
                }
            } else if (i == 5) {                                       // loc_34A1B
                fx_bolt_strip(0xBC, -si, 25);                          // 308A3 F0EDE
                fx_bolt_strip(0x5A, -si, 25);
                if (si > 6) {
                    fx_shade_rect(0x9A, 0, 0xAC, 0xBE);
                    fx_shade_rect(0x120, 0, 0x12C, 0xC8);
                }
                if (si > 0x18) {
                    fx_bolt_strip(0x64, -si, 27);
                    fx_bolt_strip(0xE6, -si, 27);
                    fx_shade_rect(0x2B, 0, 0x38, 0xC8);
                    fx_shade_rect(0xD8, 0, 0xDA, 0xC8);
                }
            }
        }
        battle_fx_commit();                                           // 5E9E8
        wait_game_tick();                                             // 499E4
        if (si > 0x3A) fx_pal_mix(0x00, 0x68, 0x10, 0x24);             // 307A6(10h, 24h)
        if (si == 0x0A) sfx(0x34);                                     // 4F4CD(34h)
        if (si % 12 == 0) sfx(0x33);                                   // 4F4CD(33h)
    }
    // ---- tail (loc_34B5A..) ----
    //   (1) Fill a 300h stack buffer with unk_12827C (= unk_12824C + 30h = colour 10h) and
    //       call 5F9AE(0, 100h, buffer): the whole DAC becomes colour 10h for one flash frame.
    //   (2) 5ECC7(0) (5EDDE back to fill mode), dword_BA838 = 0, 44D75 (clear the list), then
    //       45559 = 454B3 + 5E9E8 redraws and presents the map with the solid palette.
    //   (3) 49A3E(0, 100h, buffer, 0Ah) copies the 300h bytes at ecx (= unk_12824C) into
    //       unk_FABE0, builds (copy - solid) with sub_5FC21, then walks si = 0..64h by 0Ah
    //       and writes DAC[i] = copy[i]*pct/100 + solid through 5FD1C: 11 steps from the
    //       solid colour back to the master palette.
    //       5FD1C waits for vertical retrace (5E943) each step, so the port also ticks per
    //       step (same rule as the 276B9/2799A fades). The reference palette is the
    //       weather-applied unk_12824C, hence dlg_shade_pal_.
    cam_fine_x_ = 0;                                                  // dword_BA838 = 0
    battle_highlight_clear();                                         // 44D75
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();         // copy of unk_12824C
    const uint8_t (*master)[3] = dlg_shade_pal_;
    const uint8_t sr = master[0x10][0], sg = master[0x10][1], sb = master[0x10][2];
    for (int i = 0; i < 256; ++i) palette_set_entry((uint8_t)i, sr, sg, sb);   // 5F9AE (solid 10h)
    video_.present();
    wait_game_tick();                                                 // equivalent of 5E943 (retrace) in 5F9AE
    battle_fx_world();                                                // 45559 = 454B3
    battle_fx_commit();                                               //          + 5E9E8
    for (int p = 0; p <= 100; p += 0x0A) {                            // 49A3E: si += [esp+18h](0Ah)
        for (int i = 0; i < 256; ++i) {                               // 5FD1C: A*pct/100 + B (A = master, B = solid)
            palette_set_entry((uint8_t)i,
                              (uint8_t)(master[i][0] * p / 100 + sr),
                              (uint8_t)(master[i][1] * p / 100 + sg),
                              (uint8_t)(master[i][2] * p / 100 + sb));
        }
        video_.present();
        if (should_exit_) break;
        wait_game_tick();                                             // replaces 5E943
    }
    A.facing = saved_facing;                                          // restore [63h] after the fade
    game_timer_set(6);                                                // tail 1033B(6)
    battle_nsp_release();                                             // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 341C7 unit=%d f_quake frames=%d registered=%zu\n",
                A.id, nframes, battle_highlight_units_.size());
}

// ---- (8) loc_381EF: camera midpoint, 12-frame afterimage interpolation, tile move ----
//   Original order:
//     46DF1(caster x, caster y, target x, target y) = 1124A (midpoint), camera centred
//     var_18/var_1C = 10971(caster x)+0Ch / 10988(caster y)-2   ; caster screen anchor
//     var_4 /var_8  = 10971(target x)+0Ch / 10988(target y)-2   ; target screen anchor
//     1033B(9) -> 454B3 -> 30544 (reset trail)
//     loc_3827D x12: 306E6(var_18, var_1C, unit, facing), then var_18 += (var_4-var_18)/5
//                    and var_1C += (var_8-var_1C)/5 (idiv truncates toward zero)
//     11EE4 (release the caster tile), [unit+0]/[unit+1] = var_10/var_14 (landing tile 7
//     ahead), 11E04 (occupy), 305A2(unit, [64h]), 5E9E8, 4F4CD(32h)
//   Nothing is presented during the interpolation, so all 12 afterimages appear together on
//   the last frame.
void CombatII::fx_37fa4_rush(int unit_index, int dash_x, int dash_y) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    // The second point of 46DF1 is not the target unit but the landing tile 7 steps ahead
    // (var_10/var_14); the caller loads `mov ebx,[esp+0Eh]` = var_10 and
    // `mov ecx,[esp+0Ah]` = var_14, the same tile handed to 11E04.
    const int tx = dash_x, ty = dash_y;
    camera_center_on((int)A.x + (tx - (int)A.x) / 2, (int)A.y + (ty - (int)A.y) / 2);   // 46DF1
    auto anchor = [&](int ux, int uy) {
        return std::pair<int, int>{(ux - (int)cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X,
                                   (uy - (int)cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y};
    };
    const auto c = anchor((int)A.x, (int)A.y);        // var_18/var_1C
    const auto t = anchor(dash_x, dash_y);            // var_4/var_8 (landing tile)
    game_timer_set(9);                                // 1033B(9)
    battle_fx_world();                                // 454B3
    battle_fx_overlay();                              // tail of 454B3 = 454D5
    fx_trail_reset();                                 // 30544
    int ax = c.first, ay = c.second;
    for (int i = 0; i < 12; ++i) {                    // loc_3827D
        fx_trail_add_at(unit_index, ax, ay);          // 306E6
        ax += (t.first - ax) / 5;
        ay += (t.second - ay) / 5;
    }
    A.x = (int8_t)dash_x;                             // 11EE4 + [unit+0] = var_10
    A.y = (int8_t)dash_y;                             // 11E04 (occupation is modelled by units_)
    fx_trail_step(unit_index, (int)A.anim_frame);     // 305A2
    battle_fx_commit();                               // 5E9E8
    sfx(0x32);                                        // 4F4CD(32h)
}

// ---- (9) loc_383A2/loc_38426/loc_384E8/loc_38546: lightning strike blocks ----
//   Common body of the four blocks:
//     5E9D0(ebx=5DC0, eax=0, edx=0) -> 5EE51(F0DAA[12*(hit%3)], x+0A0h, y+64h)
//     -> 30784 (drop one afterimage) -> 305A2(unit, pose) -> 44DCC (redraw registered units)
//     -> 5E9E8 -> 499E4
//   They differ only in frame count (1/5/2/5), the 305A2 pose (5/6/7 and 8/9) and 1E2EA (the
//   weapon arc, di<3) in blocks B and D; the arc is left to the port's weapon-arc model.
void CombatII::fx_37fa4_hit_block(int unit_index, int irus_frame, int pose, int nframes) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const int n = (int)magic_frames_.size();
    const int f = ((irus_frame % 3) + 3) % 3;
    for (int i = 0; i < nframes && !should_exit_; ++i) {
        video_.clear();                                          // 5E9D0
        if (f < n) {                                             // 5EE51 (+A0h/+64h)
            const MagicFxFrame& a = magic_frames_[(size_t)f];
            fx_blit_nsp(f, 0xA0 + a.xo, 0x64 + a.yo);
        }
        fx_trail_drop();                                         // 30784
        fx_trail_step(unit_index, pose);                         // 305A2
        render_highlight_units_direct();                         // 44DCC
        battle_fx_commit();                                      // 5E9E8
        wait_game_tick();                                        // 499E4
    }
}

// ==== sub_37FA4: elect + irus.nsp lightning effect ====
//   Precondition: the single target in the 7-tile line ahead is in battle_target_list_.
//   Original order:
//     (1) F0B9C+2 = 0 (clear the list)
//     (2) facing != 0 registers the caster first via 44D89, facing == 0 (south) last
//         (3808F/38102); the target is registered in between (38097 loop).
//     (3) sub_44D6E(1), sub_30544, 1033B(10h), [+64h] = 5
//     (4) 3 frames: 305A2(caster, dir), 5E9E8, 30824(0,0Ch), 499E4, pose +1
//     (5) 4 frames: 454B3, 305A2(0,0), 5E9E8, 30824(0,0Ch), 499E4
//     (6) [+64h]++, 45559 (present), 1033B(9), 454B3, 30544
//     (7) 9 frames: 454B3, 5EE51(NSP (i%3)+0Ch at +A0/+64), 305A2(0,0), 44DCC,
//         5E9E8, 499E4
//   Port scope: (1)..(7). The coordinate interpolation and unit move after (8)
//   (11EE4/11E04) and the black-screen 5E9D0 + 30784 loop after 0Ah overlap the gameplay
//   path already modelled by battle_weapon_37fa4, so only the effect is prepended here.
void CombatII::battle_fx_37fa4_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    // (1) clear the list, (2) register in facing order.
    //   ASM 3808F: `cmp [63h],0; jnz loc_38097` puts the caster first for facing 0 (south)
    //   and after the target at loc_38102 otherwise: facing 0 draws caster then target,
    //   facing 1 draws target then caster.
    battle_highlight_clear();                                         // dword_F0B9C+2 = 0
    const bool front_first = ((A.facing & 3) == 0);
    if (front_first) battle_highlight_add(unit_index);
    for (int ti : battle_target_list_) battle_highlight_add(ti);
    if (!front_first) battle_highlight_add(unit_index);
    // (3)
    battle_dim_row0_ = true;                                          // 44D6E(1)
    fx_trail_reset();                                                 // 30544
    game_timer_set(0x10);                                             // 1033B(10h)
    A.anim_frame = 5;
    // (4) 3 frames
    for (int i = 0; i < 3 && !should_exit_; ++i) {
        fx_trail_step(unit_index, (int)A.anim_frame);                  // 305A2(caster, dir)
        battle_fx_commit();                                            // 5E9E8
        fx_pal_mix(0x60, 0x100, 0, 0x0C);                              // 30824(0,0Ch)
        wait_game_tick();                                              // 499E4
        A.anim_frame = (uint8_t)(A.anim_frame + 1);                     // inc [64h]
    }

    // (5) 4 frames (afterimages only)
    for (int i = 0; i < 4 && !should_exit_; ++i) {
        battle_fx_world();                                             // 454B3
        battle_fx_overlay();                                           // 454D5
        fx_trail_step(-1, 0);                                          // 305A2(0,0)
        battle_fx_commit();                                            // 5E9E8
        fx_pal_mix(0x60, 0x100, 0, 0x0C);                               // 30824(0,0Ch)
        wait_game_tick();                                              // 499E4
    }
    // (6)
    A.anim_frame = (uint8_t)(A.anim_frame + 1);                        // inc [64h]
    present_cutscene_frame();                                          // 45559 = 454B3 + 5E9E8

    // (8) loc_381EF: the original advance (49ABC) already ran at loc_3809B before (3), so only
    // the landing tile is computed here. The advance is at most 7 tiles and stops in front of
    // a hostile unit (1D124 + 165C2 per tile, jmp loc_38102 at loc_3809B).
    int dash_x = A.x, dash_y = A.y;
    const int dash_army = battle_effective_army(A);
    for (int i = 0; i < 7; ++i) {
        int nx = dash_x, ny = dash_y;
        switch (A.facing & 3) {
            case 0: ++ny; break;                                            // south y+1
            case 1: --nx; break;                                            // west x-1
            case 2: --ny; break;                                            // north y-1
            default: ++nx; break;                                           // east x+1
        }
        if (nx < 0 || ny < 0) break;
        if (map_w_ != 0 && nx >= (int)map_w_) break;
        if (map_h_ != 0 && ny >= (int)map_h_) break;
        const int oi = battle_unit_index_at(nx, ny);                    // 165C2
        if (oi >= 0 && (size_t)oi < units_.size()) {
            const auto& o = units_[(size_t)oi];
            if (o.active && o.hp > 0 &&
                battle_armies_hostile(dash_army, battle_effective_army(o), battle_hostility_inverted_))
                break;
        }
        dash_x = nx; dash_y = ny;
    }
    fx_37fa4_rush(unit_index, dash_x, dash_y);                         // (8)
    if (!battle_nsp_load("irus.nsp") || magic_frames_.empty()) {        // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 37FA4 unit=%d irus.nsp missing\n", A.id);
        battle_dim_row0_ = false;
        fx_pal_mix(0x00, 0x100, 0, 0);
        return;
    }
    game_timer_set(9);                                                 // 1033B(9)
    fx_trail_reset();                                                  // 30544

    // (7) 9 frames of lightning.
    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 9 && !should_exit_; ++i) {
        battle_fx_world();                                             // 454B3
        battle_fx_overlay();                                           // 454D5 (dim,44DCC,HUD)
        const int f = (i % 3) + 0x0C;                                   // (cx%3) + 0Ch
        if (f < nframes) {                                              // 5EE51 (+A0h/+64h)
            const MagicFxFrame& a = magic_frames_[(size_t)f];
            fx_blit_nsp(f, 0xA0 + a.xo, 0x64 + a.yo);
        }
        fx_trail_step(-1, 0);                                          // 305A2(0,0)
        render_highlight_units_direct();                               // 44DCC (redraw units above the effect)
        battle_fx_commit();                                            // 5E9E8
        wait_game_tick();                                              // 499E4
    }
    // After (7) (53921~53926): sub_1214F(first target) -> pose 0Ah (0 for sprites >= 12Ch)
    // -> 1033B(0Ch). Dim release and the 5F9AE palette restore belong to the original tail
    // (54205~54212) and are done by the caller battle_weapon_37fa4 after (9).
    if (!battle_target_list_.empty()) {
        const int ti = battle_target_list_[0];
        if (ti >= 0 && (size_t)ti < units_.size()) {
            auto& T = units_[(size_t)ti];
            T.anim_frame = (T.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00;   // sub_1214F
        }
    }
    game_timer_set(0x0C);                                             // 1033B(0Ch)
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 37FA4 unit=%d irus frames=%d registered=%zu\n",
                A.id, nframes, battle_highlight_units_.size());
}

// ==== sub_34C48: ju + shak2.nsp tremor ====
//   Precondition: the targets gathered by 1A452(3) + 1A552 sweep + 1A1F8 are in
//   battle_target_list_. Original order (ASM 49681~50380), 106 frames:
//     (0) 1FEA4("shak2.nsp"), 103A8, 1033B(5), anchors 10971+0Ch / 10988-4,
//         44D89(all targets), 11EE4(caster), 44D6E(1, 28h), 30824(0, 28h), 5F9AE,
//         1214F(target pose 0Ah, 0 for sprites >= 12Ch)
//     (1) one wave = 24 frames, three waves:
//         [64h]=5, 4F4CD(1), 454B3, 12F2A(caster), 5E9E8, 499E4 x3
//         [64h]=6, 8/9 frames: frame = di + c (c = 24h..24h+varA), group A offsets,
//         targets [65h]/[66h] +-1, 4F4CD(14h), [64h]=7 for 3 frames (4F4CD(1) after the first)
//         [64h]=8, 9/8 frames: frame = di + c (c = 2Dh..2Dh+varB), group B offsets,
//         targets +-1, [64h]=7 for 3 frames
//       di = facing block: south 22h, west 0, north 3Ch, east 11h.
//       varA/varB = south/north (7,8), west/east (8,7) - the two groups are opposite.
//     (2) tail 33 frames: re-anchor to 10971+0Ch / 10988-2, 4F4CD(32h), then F0DAA
//         12h..1Ah three frames each and 13h six frames (no unit redraw).
//     (3) 5F9AE, 44D6E(0), 44D75, 11E04(caster), 12124(caster), damage
//         (1D770/2CACA/45BFB/45ED2/287AF), 1FFCD, 103A0
//   Damage happens after the effect, so the caller battle_weapon_34c48 applies it.
void CombatII::battle_fx_34c48_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    if (!battle_nsp_load("shak2.nsp") || magic_frames_.empty()) {         // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 34C48 unit=%d shak2.nsp missing\n", A.id);
        return;
    }
    weather_tick_enabled_ = false;                                       // 103A8
    game_timer_set(5);                                                   // 1033B(5)
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0C;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y - 4;
    for (int ti : battle_target_list_) battle_highlight_add(ti);          // 44D89 all targets
    battle_dim_row0_ = true;                                             // 44D6E(1, 28h)
    fx_pal_mix(0x60, 0x100, 0, 0x28);                                    // 30824(0, 28h)
    for (int ti : battle_target_list_) {                                 // 1214F
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        T.anim_frame = (T.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00;
    }
    const int f = A.facing & 3;
    static const int kBase[4] = {0x22, 0x00, 0x3C, 0x11};                 // south, west, north, east
    static const int kVarA[4] = {7, 8, 7, 8};
    static const int kVarB[4] = {8, 7, 8, 7};
    static const int kXA[4] = {0, 0, 0, 0x1E};
    static const int kYA[4] = {0x10, -3, -0x10, -3};
    static const int kXB[4] = {0, 0, 0, 0x14};
    static const int kYB[4] = {0x1C, -0x12, -0x1E, 0x06};
    static const int kShiftX[4] = {-1, 1, 0, 0};
    static const int kShiftY[4] = {0, 0, 1, -1};
    const int dir = kBase[f];
    auto blit = [&](int frame, int xo, int yo) {                          // 5EE51
        if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
        const MagicFxFrame& e = magic_frames_[(size_t)frame];
        fx_blit_nsp(frame, ax + xo + e.xo, ay + yo + e.yo);
    };
    // Original frame order: 454B3 -> 12F2A(caster) -> 5EE51(effect) -> 5E9E8 -> 499E4.
    auto frame = [&](int pose, int bframe, int xo, int yo) {
        A.anim_frame = (uint8_t)pose;
        battle_fx_world();                                                // 454B3
        battle_fx_overlay();
        battle_draw_unit_now(unit_index);                                 // 12F2A
        if (bframe >= 0) blit(bframe, xo, yo);                             // 5EE51
        battle_fx_commit();                                               // 5E9E8
        wait_game_tick();                                                 // 499E4
    };
    auto shift_targets = [&]() {                                          // [65h]/[66h] +-1
        for (int ti : battle_target_list_) {
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            units_[(size_t)ti].walk_off_x += kShiftX[f];
            units_[(size_t)ti].walk_off_y += kShiftY[f];
        }
    };
    for (int wave = 0; wave < 3 && !should_exit_; ++wave) {
        sfx(1);                                                           // 4F4CD(1)
        frame(5, -1, 0, 0);                                              // [64h] = 5
        for (int k = 0; k < 2 && !should_exit_; ++k) wait_game_tick();     // 2 of the 3 499E4 calls
        for (int c = 0x24; c <= 0x24 + kVarA[f] && !should_exit_; ++c)
            frame(6, dir + c, kXA[f], kYA[f]);
        shift_targets();
        sfx(0x14);                                                        // 4F4CD(14h)
        for (int k = 0; k < 3 && !should_exit_; ++k) {
            frame(7, -1, 0, 0);
            if (k == 0) sfx(1);
        }
        for (int c = 0x2D; c <= 0x2D + kVarB[f] && !should_exit_; ++c)
            frame(8, dir + c, kXB[f], kYB[f]);
        shift_targets();
        for (int k = 0; k < 3 && !should_exit_; ++k) frame(7, -1, 0, 0);
    }
    // (2) Tail: re-anchor to 10971+0Ch / 10988-2, then 12h..1Ah three frames each, 13h six.
    const int tx = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0C;
    const int ty = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y - 2;
    auto tail = [&](int frame) {
        battle_fx_world();                                                // 454B3
        battle_fx_overlay();
        if (frame >= 0 && (size_t)frame < magic_frames_.size()) {
            const MagicFxFrame& e = magic_frames_[(size_t)frame];
            fx_blit_nsp(frame, tx + e.xo, ty + e.yo);
        }
        battle_fx_commit();                                               // 5E9E8
        wait_game_tick();                                                 // 499E4
    };
    sfx(0x32);                                                            // 4F4CD(32h)
    for (int fr = 0x12; fr <= 0x1A && !should_exit_; ++fr)
        for (int k = 0; k < 3 && !should_exit_; ++k) tail(fr);
    for (int k = 0; k < 6 && !should_exit_; ++k) tail(0x13);
    // (3)
    pal_load_range(0, 256, &battle_palette_[0][0]);                       // 5F9AE
    battle_dim_row0_ = false;                                            // 44D6E(0)
    battle_highlight_clear();                                            // 44D75
    battle_nsp_release();                                                // 1FFCD
    weather_tick_enabled_ = true;                                        // 103A0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 34C48 unit=%d dir=%d frames=%d(%d)\n",
                A.id, dir, (int)magic_frames_.size(), 3 * 24 + 33);
}

// ==== sub_32589: chang + slash + arcion.nsp single-target melee ====
//   Precondition: the single target gathered by 1A1F8 is in battle_target_list_[0].
//   Original order (ASM 46914~47381):
//     (1) 1FEA4("arcion.nsp"), then 103A8 (weather tick off)
//     (2) 11EE4(caster) and 11EE4(target) register tile occupation; units_ models this here
//     (3) facing block base var_C: south 0, north 0Ah, east 14h, west 1Eh (10 frames each)
//     (4) 44D6E(1, 0Ah) dim, 30824(0, 10h), 1033B(5), anchors 10971+0Bh / 10988-2, 4F4CD(32h)
//     (5) stage A, 27 frames: k = 0..8, three frames each, 30824(0,6) before every k.
//         frame = var_C + k; each frame 454B3 -> (target 12F2A when facing != 0) -> 5EE51
//         -> (target 12F2A when facing == 0) -> 5E9E8 -> 499E4
//     (6) stage B, 15 frames: k = 28h,2Ah,2Ch,2Eh,30h, three frames each. frame = k plus
//         (7+var_C) or (8+var_C), toggled per k, plus k+1 when k > 2Ch.
//     (7) stage C, 45 frames: 5 rounds of k = 32h,34h,36h, three frames each; frame = k,
//         var_C+7/8 and k+1.
//     (8) 6 frames: memset(755CC+0BB80h, 0, 17700h) clears the visible 200 rows of the 480
//         stride buffer -> 5E9E8 -> 499E4 (no 454B3).
//     (9) tail 36 frames: anchor 10971(target.x)+0Ch / 10988(target.y)-2, 4F4CD(33h),
//         entry = F0DAA[var_C+9]. Each frame 454B3 -> (5EE51 when facing == 0)
//         -> 5F42D/5F4AF (target D66A8 pose 0 at x-4 and x+4) -> (5EE51 when facing != 0)
//         -> 5E9E8 -> 499E4
//     (10) 44D6E(0, 100h), 5F9AE(0, 100h), 11E04(caster), 1DB1D(caster, target),
//          1FFCD, 103A0 (weather tick on)
//   Returns 1 (4C98A proceeds with 301BB and consumption).
//
//   Port approximation: 11EE4/11E04 tile occupation is modelled by units_ and skipped.
//   5F42D/5F4AF are transparent RLE blits of the D66A8 sprite, equivalent to two
//   blit_nc2_frame calls at x-4 / x+4.
void CombatII::battle_fx_32589_anim(int unit_index, int target_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (target_index < 0 || (size_t)target_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    const int caster_id = A.id;                                         // kept for the trace after 1DB1D
    const int target_id = units_[(size_t)target_index].id;
    if (!battle_nsp_load("arcion.nsp") || magic_frames_.empty()) {       // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 32589 unit=%d arcion.nsp missing\n", caster_id);
        return;
    }
    weather_tick_enabled_ = false;                                      // 103A8
    const int nframes = (int)magic_frames_.size();
    // (3) Facing block: south(0)=0, north(2)=0Ah, east(3)=14h, west(1)=1Eh.
    static const int kDirBase[4] = {0x00, 0x1E, 0x0A, 0x14};
    const int dir_base = kDirBase[A.facing & 3];
    // (4)
    battle_dim_row0_ = true;                                            // 44D6E(1, 0Ah)
    fx_pal_mix(0x60, 0x100, 0, 0x0A);                                   // 30824(0, 10h)
    game_timer_set(5);                                                  // 1033B(5)
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0B;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y - 2;
    sfx(0x32);                                                          // 4F4CD(32h)
    const bool target_before = ((A.facing & 3) != 0);                    // facing != 0 draws the target before the effect
    auto blit = [&](int frame) {
        if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;  // 5EE51
        const MagicFxFrame& e = magic_frames_[(size_t)frame];
        fx_blit_nsp(frame, ax + e.xo, ay + e.yo);
    };
    auto frame = [&](std::initializer_list<int> frames) {
        battle_fx_world();                                              // 454B3
        battle_fx_overlay();                                            // 454D5
        if (target_before) battle_draw_unit_now(target_index);           // 12F2A
        for (int f : frames) blit(f);
        if (!target_before) battle_draw_unit_now(target_index);
        battle_fx_commit();                                             // 5E9E8
        wait_game_tick();                                               // 499E4
    };
    // (5) stage A (27 frames)
    for (int k = 0; k <= 8 && !should_exit_; ++k) {
        fx_pal_mix(0x60, 0x100, 0, 6);                                   // 30824(0, 6)
        for (int r = 0; r < 3 && !should_exit_; ++r) frame({dir_base + k});
    }
    // (6) stage B (15 frames): var_8 toggles 7/8 and the frame is var_8 + var_C.
    int var8 = 7;
    for (int k = 0x28; k <= 0x30 && !should_exit_; k += 2) {
        for (int r = 0; r < 3 && !should_exit_; ++r) {
            if (k > 0x2C) frame({k, var8 + dir_base, k + 1});
            else frame({k, var8 + dir_base});
        }
        var8 = (var8 == 7) ? 8 : 7;                                      // cmp 7 / setz / add 7
    }
    // (7) stage C (45 frames, 5 rounds)
    for (int round = 0; round < 5 && !should_exit_; ++round) {
        for (int k = 0x32; k <= 0x36 && !should_exit_; k += 2) {
            for (int r = 0; r < 3 && !should_exit_; ++r) frame({k, var8 + dir_base, k + 1});
            var8 = (var8 == 7) ? 8 : 7;
        }
    }
    // (8) 6 black-screen frames (the original clears without 454B3, then 5E9E8, 499E4).
    for (int i = 0; i < 6 && !should_exit_; ++i) {
        video_.clear();                                                  // memset(755CC+0BB80h)
        battle_fx_commit();                                              // 5E9E8
        wait_game_tick();                                                // 499E4
    }
    // (9) 36-frame tail: target anchor plus a pose-0 D66A8 sprite smear.
    const auto& T = units_[(size_t)target_index];
    const int tx = (T.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0C;
    const int ty = (T.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y - 2;
    const NC2Frame* tfr = nc2_frame_for(T.sprite_type, T.facing & 3, 0);  // D66A8 pose 0
    const int impact = dir_base + 9;                                     // var_C + 9
    sfx(0x33);                                                           // 4F4CD(33h)
    for (int i = 0; i < 0x24 && !should_exit_; ++i) {
        battle_fx_world();                                               // 454B3
        battle_fx_overlay();                                             // 454D5
        if (!target_before) blit(impact);
        if (tfr) {                                                       // 5F42D / 5F4AF
            const int sx = tx + tfr->x_off, sy = ty + tfr->y_off;
            blit_nc2_frame(sx - 4, sy, *tfr);
            blit_nc2_frame(sx + 4, sy, *tfr);
        }
        if (target_before) blit(impact);
        battle_fx_commit();                                              // 5E9E8
        wait_game_tick();                                                // 499E4
    }
    // (10) original 47275~47310: 44D6E(0,100h), 5F9AE, 11E04(caster), 1DB1D(caster, target),
    //      1FFCD, 103A0
    battle_dim_row0_ = false;                                            // 44D6E(0, 100h)
    pal_load_range(0, 256, &battle_palette_[0][0]);                      // 5F9AE
    if (target_index >= 0 && (size_t)target_index < units_.size() && units_[(size_t)target_index].active)
        play_unit_death(target_index, unit_index);                       // 1DB1D ([48h] = 0)
    battle_nsp_release();                                                // 1FFCD
    weather_tick_enabled_ = true;                                        // 103A0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 32589 unit=%d target=%d dir=%d frames=%d(%d)\n",
                caster_id, target_id, dir_base, nframes, 27 + 15 + 45 + 6 + 0x24);
}

// ==== sub_32BCB: i-star2 + slash + ice + icer.nsp (edi 3) ====
// The original (ASM 47602~48220) uses three ice-particle routines:
//   33C31  draws each particle frame byte (var_A0) through 5F29D (scaled and tinted).
//          The byte advances every 4 frames and wraps to 1Ch past 0x25 / 0x29.
//   33E06  steps 60 particles (cos/sin tables flt_74A68/flt_744C8 at 1 degree steps,
//          colour 18h 3x3 plus).
//   33A2D  draws 5 bolts with 5EE51 and turns units within +-20 into 26h.
//
// Frame breakdown for one target, 429 frames:
//   0..6     (1) intro 5EE51(F0DAA[0..6]) + 12F2A                              (7)
//   7..12    (2) intro 5EE51(F0DAA[5..0]); screen cleared before cx == 0       (6)
//   13..44   (3) 33C31 + 12F2A + 33E06, spawning with var_A0[si++]=13h          (32*)
//   45..166  (4) silhouette sweep: 5F517(pose 8) x (var_18+1) + 5F3FD, y -3    (122)
//   167..235 (5) var_28 0..4 x (var_C 7..9 x 3): 33C31 + 5F3FD(pose var_C)      (45)
//   176..265 (6) var_28 0..8 x 6: 33C31 + 5F3FD(pose 9) + 5EE51(7) on first    (54, overlaps 5)
//   266..274 (7) var_8 7..9 x 3 frames                                          (9)
//   275..295 (8) var_14 0Ch..12h x 3 + 5EE51(F0DAA[var_14]) at (598,138)       (21)
//   296..304 (9) 3 x 3 frames (5F3FD only)                                      (9)
//   305..426 (10) y +3 sweep: 5F517(pose 9) x (var_24+1) + 5F3FD               (122)
//   427      12F2A for one frame
//   428      tail: 44D75, 5F9AE(0), 44D6E(0), [64h]=4, 11E04, restore [63h],
//            12124 x CNT, 2CAF7(4), 1FFCD, 103A0
//
// Approximations (marked in the code below):
//   - 33E06's 3x3 plus lands at row y-64h of the port's 320x200 backbuffer (the visible
//     window of the 480 canvas).
//   - The static tables F0F06/F0F1E/F0F2A/F0F36 are BSS filled by 1FEA4 and are not in the
//     file, so nothing past frame 0x64 (after (8)) is drawn.
//   - 5F517 draws 0 frames when var_18/var_24 == 0, not var+1.
//   - (*) (3) is data dependent: it ends when cnt particles pass 1Ah and all 60 are dead.
//     One target gives 32 frames (19h -> 1Bh); more targets only lengthen the spawn phase
//     (capped at 600).
bool CombatII::battle_fx_32bcb_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    auto& A = units_[(size_t)unit_index];
    const int caster_id = A.id;
    if (!battle_nsp_load("icer.nsp") || magic_frames_.empty()) {           // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 32BCB unit=%d icer.nsp missing\n", caster_id);
        return false;                                                      // 2CAF7 belongs to the caller
    }
    const int nframes = (int)magic_frames_.size();
    weather_tick_enabled_ = false;                                         // 103A8
    const int8_t saved_facing = A.facing;                                  // save [63h]
    A.facing = 0;                                                          // [63h] = 0
    const int cnt = (int)battle_target_list_.size();                       // 0xF0B9E
    for (int ti : battle_target_list_) battle_highlight_add(ti);           // 44D89 all targets
    battle_dim_row0_ = true;                                               // 44D6E(1, 28h)
    fx_pal_mix(0x60, 0x100, 0, 0x00);                                      // 30824(0)
    A.anim_frame = 8;                                                      // [64h] = 8
    const int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
    const int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
    const int var54 = ax + 0x0C;                                           // 10971+0Ch
    const int si_y = ay - 2;                                               // 10988-2
    game_timer_set(0x0C);                                                  // 1033B(0Ch)
    const NC2Frame* fr8 = nc2_frame_for(A.sprite_type, 0, 8);              // D66A8 pose 8
    const NC2Frame* fr9 = nc2_frame_for(A.sprite_type, 0, 9);              // D66A8 pose 9

    // ---- 33C31/33E06: 60 ice particles ----
    // 32E35 creates all 60 at once (sub-step = 499BD(6)+2, angle = 499BD(168h)); the (3)
    // loop wakes one every 4 frames with var_A0[si++] = 13h, and until then it is not drawn.
    constexpr int kSlots = 60;                                             // cmp bx, 3Ch
    std::vector<uint8_t> p_frame(kSlots, 0);                               // var_A0 frame byte
    std::vector<uint8_t> p_alive(kSlots, 1);                               // var_DC alive
    std::vector<uint8_t> p_steps(kSlots, 0);                               // var_154 sub-step
    std::vector<uint16_t> p_speed(kSlots, 0);                              // var_208 angle (degrees)
    std::vector<float> p_x(kSlots), p_y(kSlots);                           // var_4D8 / var_5C8
    int tx0 = ax, ty0 = ay;
    if (!battle_target_list_.empty()) {
        const int u0 = battle_target_list_[0];
        if (u0 >= 0 && (size_t)u0 < units_.size()) {
            tx0 = (units_[(size_t)u0].x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
            ty0 = (units_[(size_t)u0].y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
        }
    }
    for (int i = 0; i < kSlots; ++i) {
        p_steps[(size_t)i] = (uint8_t)(combat_rand(6) + 2);                // 499BD(6)+2
        p_speed[(size_t)i] = (uint16_t)combat_rand(0x168);                 // 499BD(168h)
        p_x[(size_t)i] = (float)(tx0 + 2);
        p_y[(size_t)i] = (float)(ty0 - 0x1F);
    }
    int next_particle = 0;                                                 // si
    int frame_counter = 0;                                                 // var_5C
    bool ice_active = true;                                                // (3) end / 64h switch
    auto ice_draw = [&]() {                                                // 33C31
        if (!ice_active) return;
        for (int i = 0; i < kSlots; ++i) {
            const int v = p_frame[(size_t)i];
            if (v <= 0 || v >= 0x64) continue;
            if (i >= cnt) continue;                                        // one particle per target
            const int idx = (v >= 0x1F && v < 0x26) ? (0x3C - v) : v;       // 1Fh..25h mirror the frames
            if (idx <= 0 || idx >= nframes) continue;
            const int ui = battle_target_list_[(size_t)i];
            if (ui < 0 || (size_t)ui >= units_.size()) continue;
            const auto& U = units_[(size_t)ui];
            const int ux = (U.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0C;
            const int uy = (U.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y + 2;
            const MagicFxFrame& e = magic_frames_[(size_t)idx];
            battle_fx_blit(idx, ux + e.xo, uy + e.yo);                     // 5F29D (scaler not ported)
            if ((frame_counter % 4) == 3) {                                // +1 every 4 frames, after drawing
                int f = v + 1;
                if (f < 0x26) { if (f > 0x21) f = 0x1C; }
                else if (f > 0x29) f = 0x1C;
                p_frame[(size_t)i] = (uint8_t)f;
            }
        }
    };
    // 33E06: a live particle advances var_154 steps along var_208 degrees. Up to step 11 it
    // paints a 5-pixel plus, afterwards a single pixel (colour 18h, +1 every 10 steps, max 1Fh).
    auto ice_trail = [&]() {
        if (!ice_active) return;
        uint8_t* buf = video_.get_backbuffer();
        if (!buf) return;
        for (int i = 0; i < kSlots; ++i) {
            if (p_alive[(size_t)i] != 1) continue;
            const int steps = p_steps[(size_t)i];
            const double ang = (double)p_speed[(size_t)i] * 0.017453292519943295;
            const float ca = (float)std::cos(ang);
            const float sa = (float)std::sin(ang);
            for (int k = 0; k < steps; ++k) {
                p_x[(size_t)i] += ca;
                p_y[(size_t)i] += sa;
                const int px = (int)std::lround(p_x[(size_t)i]);
                const int py = (int)std::lround(p_y[(size_t)i]);
                const uint8_t col = (uint8_t)(0x18 + (k / 10 > 7 ? 7 : k / 10));
                auto put = [&](int x, int y) {
                    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
                        buf[(size_t)y * SCREEN_W + (size_t)x] = col;
                };
                if (k <= 0x10) {
                    put(px, py); put(px + 1, py); put(px - 1, py);
                    put(px, py - 1); put(px, py + 1);
                } else {
                    put(px, py);
                }
            }
            // Off-screen particles die (original bounds -32h/172h/0FAh, +50 margin here).
            if (p_x[(size_t)i] < -50.0f || p_x[(size_t)i] > SCREEN_W + 50 ||
                p_y[(size_t)i] < -50.0f || p_y[(size_t)i] > SCREEN_H + 50)
                p_alive[(size_t)i] = 0;
        }
    };
    auto present = [&]() {                                                 // 5E9E8 + 499E4
        battle_fx_commit();
        wait_game_tick();
    };
    auto blit_intro = [&](int k) {                                         // 5EE51
        if (k < 0 || k >= nframes) return;
        const MagicFxFrame& e = magic_frames_[(size_t)k];
        fx_blit_nsp(k, var54 + e.xo, si_y - 0x20 + e.yo);
    };
    auto solid = [&](const NC2Frame* fr, int y, int color) {               // 5F517
        if (fr) blit_nc2_frame_solid(var54 + fr->x_off, y, *fr, (uint8_t)color);
    };
    auto span = [&](const NC2Frame* fr, int y) {                           // 5F3FD
        if (fr) blit_nc2_frame(var54 + fr->x_off, y, *fr);
    };

    // (1) 7 frames + (2) 6 frames
    for (int k = 0; k <= 6 && !should_exit_; ++k) {
        battle_fx_world();                                                 // 454B3
        battle_fx_overlay();                                               // 454D5
        battle_draw_unit_now(unit_index);                                  // 12F2A
        blit_intro(k);
        present();
    }
    for (int k = 5; k >= 0 && !should_exit_; --k) {
        battle_fx_world();
        battle_fx_overlay();
        if (k == 0) video_.clear();                                        // memset(755CC+0BB80h,1,17700h)
        battle_draw_unit_now(unit_index);
        blit_intro(k);
        present();
    }
    game_timer_set(5);                                                     // 1033B(5)
    sfx(0x32);                                                             // 4F4CD(32h)
    // (3) Spawn and step: one particle wakes every 4 frames (var_A0[si++]=13h) and the
    //    sub-step grows every 10 frames. It ends when cnt particles pass 1Ah and all 60
    //    particles are dead (32 frames for one target).
    while (ice_active && !should_exit_) {
        if ((frame_counter % 10) == 9)
            for (int i = 0; i < kSlots; ++i)
                if (p_steps[(size_t)i] < 0x31) ++p_steps[(size_t)i];
        if ((frame_counter % 4) == 1 && next_particle < cnt && next_particle < kSlots)
            p_frame[(size_t)next_particle++] = 0x13;
        battle_fx_world();
        battle_fx_overlay();
        ice_draw();                                                        // 33C31
        battle_draw_unit_now(unit_index);                                  // 12F2A
        ice_trail();                                                       // 33E06
        present();
        ++frame_counter;
        int aged = 0, alive = 0;
        for (int i = 0; i < kSlots; ++i) {
            if (p_frame[(size_t)i] > 0x1A) ++aged;
            if (p_alive[(size_t)i] == 1) ++alive;
        }
        if (aged >= cnt && alive == 0) ice_active = false;                 // loc_33027
        if (frame_counter >= 600) ice_active = false;                      // safety cap
    }
    // (4) 122 frames: silhouette sweep (var_40 = ay + var_48, stepping down by 3).
    int var48 = fr8 ? (int)fr8->y_off : 0;
    int var40 = ay + var48;
    int var18 = 0;
    while (var40 >= 0x14 && !should_exit_) {
        if (var40 >= 0x29) var18 = (var18 + 1 > 7) ? 7 : var18 + 1;
        else var18 = (var18 - 1 < -1) ? -1 : var18 - 1;
        var48 -= 3;
        var40 = ay + var48;
        battle_fx_world();
        battle_fx_overlay();
        ice_draw();
        const int n5 = var18 > 0 ? var18 + 1 : 0;                          // number of 5F517 frames
        for (int k = n5 - 1; k >= 0; --k)                                  // top to bottom
            solid(fr8, var40 + 2 + 2 * k, 0x28 + k);
        span(fr8, var40);
        ++frame_counter;
        present();
    }
    // (5)(6) var_28 0..8: 0..4 run (5) (9 frames) then (6) (6 frames); 5..8 run (6) only.
    for (int var28 = 0; var28 <= 8 && !should_exit_; ++var28) {
        if (var28 < 5) {
            for (int varC = 7; varC <= 9 && !should_exit_; ++varC) {
                const NC2Frame* fv = nc2_frame_for(A.sprite_type, 0, varC);
                for (int r = 0; r < 3 && !should_exit_; ++r) {
                    battle_fx_world();
                    battle_fx_overlay();
                    ice_draw();
                    span(fv, 18);                                          // 5F3FD(pose var_C, y=18)
                    sfx(0x33);                                             // 4F4CD(33h)
                    ++frame_counter;
                    present();
                }
            }
        }
        for (int r = 0; r < 6 && !should_exit_; ++r) {
            battle_fx_world();
            battle_fx_overlay();
            ice_draw();
            span(fr9, 18);
            if (r == 0) blit_intro(7);                                     // 5EE51(7)
            ++frame_counter;
            present();
        }
    }
    // (7) var_8 7..9, three frames each
    for (int var8 = 7; var8 <= 9 && !should_exit_; ++var8) {
        const NC2Frame* fv = nc2_frame_for(A.sprite_type, 0, var8);
        for (int r = 0; r < 3 && !should_exit_; ++r) {
            battle_fx_world();
            battle_fx_overlay();
            ice_draw();
            span(fv, 18);
            ++frame_counter;
            present();
        }
    }
    // (8) var_14 0Ch..12h, three frames each + 5EE51(F0DAA[var_14]) at (598,138)
    sfx(0x34);                                                             // 4F4CD(34h, 0Ch)
    for (int var14 = 0x0C; var14 <= 0x12 && !should_exit_; ++var14) {
        // Particle bytes switch to 64h before the last var_14 (12h), so F2 is gone from frame 293.
        if (var14 == 0x12) ice_active = false;
        for (int r = 0; r < 3 && !should_exit_; ++r) {
            battle_fx_world();
            battle_fx_overlay();
            ice_draw();
            span(fr9, 18);
            if (var14 < nframes) {
                const MagicFxFrame& e = magic_frames_[(size_t)var14];
                fx_blit_nsp(var14, 0x0A + 0x24C + e.xo, 0x0A + 0x80 + e.yo);  // (598,138)
            }
            ++frame_counter;
            present();
        }
    }
    // (9) 3 x 3 frames of 5F3FD only (the particles are already 64h after (8) and are skipped).
    for (int round = 0; round < 3 && !should_exit_; ++round) {
        for (int r = 0; r < 3 && !should_exit_; ++r) {
            battle_fx_world();
            battle_fx_overlay();
            span(fr9, 18);
            ++frame_counter;
            present();
        }
    }
    // (10) 122 frames of a y +3 sweep while var_48 is below [esi+5].
    const int var48_end = fr9 ? (int)fr9->y_off : 0;
    int var24 = 0;
    while (var48 < var48_end && !should_exit_) {
        if (var48 + 0x15 < 0) var24 = (var24 + 1 > 7) ? 7 : var24 + 1;
        else var24 = (var24 - 1 < -1) ? -1 : var24 - 1;
        const int var50 = ay + var48 + 3;
        battle_fx_world();
        battle_fx_overlay();
        const int n5 = var24 > 0 ? var24 + 1 : 0;
        for (int k = 0; k < n5; ++k)                                       // bottom to top
            solid(fr9, var50 - 2 * n5 + 2 * k, 0x28 + (n5 - 1 - k));
        span(fr9, var50);
        var48 += 3;
        ++frame_counter;
        present();
    }
    // Frame 427: 12F2A for one frame.
    if (!should_exit_) {
        battle_fx_world();
        battle_fx_overlay();
        battle_draw_unit_now(unit_index);                                  // 12F2A
        present();
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 32BCB unit=%d cnt=%d frames=%d icer=%d\n",
                caster_id, cnt, frame_counter + 2, nframes);
    // Frame 428 tail in original op order: hlclr, pal, dim, occ-, pose0, 2CAF7, nsprl, wx+
    battle_highlight_clear();                                              // 44D75
    pal_load_range(0, 256, &battle_palette_[0][0]);                        // 5F9AE(0)
    battle_dim_row0_ = false;                                              // 44D6E(0)
    A.anim_frame = 4;                                                      // [64h] = 4 (pose 0)
    A.facing = saved_facing;                                               // restore [63h]
    battle_weapon_2caf7_loop(unit_index, 4, 0);                            // 2CAF7(4, 0)
    battle_nsp_release();                                                  // 1FFCD
    weather_tick_enabled_ = true;                                          // 103A0
    return true;
}

// ==== sub_367EE: nang.nsp (edi 8, shoot+f-arrow+bomb1) ====
// Original is 995 lines. The first 46 frames are pinned by the timeline probe; the rest
// follows the ASM structure:
//   (1) 4 frames F0DAA[0..3] at (si, di) plus the global sprite
//   (2) 4 frames global sprite only
//   (3) 6 frames F0DAA[4..9] at (si, 14h), 30824(0,0Ah) every frame
//   1 frame  1033B(4,0Ah), 30824(0), 5E9E8, 499E4 (no 454B3)
//   (4) 3 frames F0DAA[33h..35h] at (si, 14h)
//   (5) three phases (var_10 0..2): spawn 2 waves of 5 slots, 1033B(5), 4F4CD(33h)
//      -> 9 frames (global + F0E22/F0E16 + 40 particles), 3 frames (F0DAA[var_10] + 40)
//   (6) spawn 0x23..0x27, 4F4CD(33h), spawn 0x1E..0x22, then until no particle is alive
//      (max 1Eh frames) global + F0E24 + 40 particles
//   (7) 1033B(8) + 4 frames (global + F0E24), 44D6E(0,100h), 5F9AE(0)
//   (8) 8 frames cx=7..0 (F0DAA[cx] when cx>=4), then 4 frames cx=3..0 (F0DAA[cx])
//   (9) 1033B(14h) + 1 frame (global + F0DAC), 44D75, 11E04, restore [63h], 12124 all,
//      2CAF7(4,1770h), 1FFCD, 103A0
//
// sub_36531 (40-particle render) is ported: while life > 4 move by (dx,dy), then decrement
//   life, then pick one of four NSP frames by slot/5 group (off_34C38 = 1,2,3,8,9,4,5,6):
//   life >7 -> base*4+0Bh, 7 -> +0Ch, 6 -> +0Dh, 5 -> +0Eh, drawn with 5EE51 plus offsets.
//   At life 4 the group acceleration (flt_77ACD.., kAx/kAy below) and 4F4CD(34h) apply;
//   life 1..4 use the static tables F1032/F103E/F104A/F1056 (BSS = 0), i.e. frame 0.
//
// Three equivalences:
//   - The word dword_BA838 +-1 toggle only changes the 16.16 camera fraction (1/65536 px),
//     which the integer-pixel port ignores: equivalent, not approximate.
//   - F1064/F1066/F106A and F0E16/F0E22/F0DAC are aliases of F0DAA entries, not separate
//     tables (0xF0E16 = F0DAA + 9*12, 0xF0E22 = +10*12, 0xF106A = +58*12+8), so the port
//     uses F0DAA[58] (last frame), [10], [9] and [0].
void CombatII::battle_fx_367ee_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    if (!battle_nsp_load("nang.nsp") || magic_frames_.empty()) {          // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 367EE unit=%d nang.nsp missing\n", A.id);
        return;
    }
    const int nframes = (int)magic_frames_.size();
    weather_tick_enabled_ = false;                                        // 103A8
    const int8_t saved_facing = A.facing;                                 // save [63h]
    A.facing = 0;
    for (int ti : battle_target_list_) battle_highlight_add(ti);          // 44D89
    game_timer_set(8);                                                    // 1033B(8)
    const int ax0 = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
    const int ay0 = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
    const int si = ax0 + 0x0C;                                            // 10971+0Ch
    const int di = ay0 - 2;                                               // 10988-2
    // F106A/F1064/F1066 alias [+8]/[+2]/[+4] of the F0DAA[58] record, not separate globals
    // (0xF106A - 0xF0DAA = 12*58 + 8). nang.nsp has 59 frames, so the last frame is exactly 58.
    const int gframe = (nframes > 0x3A) ? 0x3A : -1;                       // F0DAA[3Ah]
    sfx(0x32);                                                            // 4F4CD(32h)
    // 40 slots, filled by 32E35/36BB1: alive(var_5C), x(var_14C), y(var_32C), velocity
    // (var_1EC/var_28C), life(var_AC). The frame comes from the slot/5 group and the life.
    struct PS { bool alive = false; float x = 0, y = 0, dx = 0, dy = 0; int life = 0; };
    std::vector<PS> p(40);
    static const int kGroupBase[8] = {1, 2, 3, 8, 9, 4, 5, 6};            // off_34C38
    static const float kAx[8] = {-40.0f, -30.0f, -25.0f, 0.0f, 0.0f, 25.0f, 30.0f, 40.0f};  // flt_77ACD..77AB1
    static const float kAy[8] = {15.0f, 30.0f, 40.0f, 45.0f, 45.0f, 40.0f, 30.0f, 15.0f};   // flt_77AB5..77AB1
    auto ring = [&](int idx, int x, int y) {                               // 5EE51(F0DAA[idx])
        if (idx < 0 || idx >= nframes) return;
        const MagicFxFrame& e = magic_frames_[(size_t)idx];
        battle_fx_blit(idx, x + e.xo, y + e.yo);
    };
    auto gblit = [&]() {                                                  // global sprite
        if (gframe < 0 || gframe >= nframes) return;
        const MagicFxFrame& e = magic_frames_[(size_t)gframe];
        battle_fx_blit(gframe, si - 0x0C + e.xo, di + 8 + e.yo);           // (576, 390)
    };
    auto frame = [&]() { battle_fx_world(); battle_fx_overlay(); };        // 454B3
    auto present = [&]() { battle_fx_commit(); wait_game_tick(); };        // 5E9E8,499E4
    // sub_36531: move only while life > 4, then decrement life and pick the frame by
    // life and group, drawing 5EE51(frame, x + [rec+4], y + [rec+6]). Life 1..4 uses the
    // static tables F1032/F103E/F104A/F1056 (BSS = 0), i.e. frame 0 with no offset.
    auto draw_p = [&]() {
        for (int i = 0; i < 40; ++i) {
            PS& s = p[(size_t)i];
            if (!s.alive) continue;
            const int g = i / 5;
            if (s.life > 4) { s.x += s.dx; s.y += s.dy; }
            if (--s.life <= 0) { s.alive = false; continue; }
            if (s.life <= 4) {                                             // F1032/F103E/F104A/F1056
                if (s.life == 4) {                                         // the fragment bursts
                    s.x += kAx[g]; s.y += kAy[g];
                    sfx(0x34);                                             // 4F4CD(34h)
                }
                if (nframes > 0) battle_fx_blit(0, (int)std::lround(s.x), (int)std::lround(s.y));
                continue;
            }
            const int base = kGroupBase[g] * 4;
            const int idx = (s.life > 7) ? (base + 0x0B) : (base + 0x13 - s.life);
            if (idx < 0 || idx >= nframes) continue;
            const MagicFxFrame& e = magic_frames_[(size_t)idx];
            battle_fx_blit(idx, (int)std::lround(s.x + e.xo), (int)std::lround(s.y + e.yo));
        }
    };
    // (1) 4 frames + (2) 4 frames
    for (int cx = 0; cx <= 3 && !should_exit_; ++cx) {
        frame(); gblit(); ring(cx, si, di); present();
    }
    for (int k = 0; k < 4 && !should_exit_; ++k) {
        frame(); gblit(); present();
    }
    battle_dim_row0_ = true;                                              // 44D6E(1, 14h)
    // (3) 6 frames: F0DAA[4..9] at y = 14h (the original starts at ecx = 4 and stops below 0Ah)
    for (int cx = 4; cx <= 9 && !should_exit_; ++cx) {
        fx_pal_mix(0x60, 0x100, 0, 0x0A);                                 // 30824(0, 0Ah)
        frame(); gblit(); ring(cx, si, 0x14); present();
    }
    game_timer_set(4);                                                    // 1033B(4, 0Ah)
    fx_pal_mix(0x60, 0x100, 0, 0x00);                                     // 30824(0)
    present();
    // (4) 3 frames: F0DAA[33h..35h]
    for (int cx = 0; cx <= 2 && !should_exit_; ++cx) {
        frame(); gblit(); ring(0x33 + cx, si, 0x14); present();
    }
    for (int ti : battle_target_list_) {                                  // 1214F
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        units_[(size_t)ti].anim_frame = 0;
    }
    // Spawn: x/y come from the caller, velocity = -(3+rand(3)) / +(3+rand(3)) x flt_77ADA
    // (0.25) or / dbl_77AEA (1.2), life = 0Fh + rand(0Fh).
    const float kSlope = 0.25f;                                           // flt_77ADA
    const float kSlopeLate = 1.0f / 1.2f;                                 // dbl_77AEA
    auto rnd = [&](int n) { return (int)combat_rand(n); };                // 499BD(n)
    auto spawn = [&](int i, int x, int y, bool late) {
        if (i < 0 || i >= 40) return;
        PS& s = p[(size_t)i];
        const float v = (float)(-3 - rnd(3));                             // fchs + (3 - rand(3))
        s.alive = true;
        s.x = (float)x;
        s.y = (float)y;
        s.dx = v;
        s.dy = -v * (late ? kSlopeLate : kSlope);
        s.life = 0x0F + rnd(0x0F);                                        // var_AC
    };
    // (5) three phases
    for (int phase = 0; phase <= 2 && !should_exit_; ++phase) {
        if (phase == 0) {
            for (int i = 0; i <= 4; ++i)                                   // 0..4
                spawn(i, si - 0x2D + rnd(0x28), di + rnd(0x14), false);
            for (int i = 5; i <= 9; ++i)                                   // 5..9
                spawn(i, si - 0x23 + rnd(0x23), di + 5 + rnd(0x14), false);
        } else if (phase == 1) {
            for (int i = 0x0A; i <= 0x0E; ++i)                             // 10..14
                spawn(i, si - 0x19 + rnd(0x1E), di + 0x0A + rnd(0x1E), true);
            for (int i = 0x0F; i <= 0x13; ++i)                             // 15..19
                spawn(i, si - 0x14 + rnd(0x1E), di + 0x0F + rnd(0x23), true);
        } else {
            for (int i = 0x19; i <= 0x1D; ++i)                             // 25..29
                spawn(i, si - 5 + rnd(0x1E), di + 0x0A + rnd(0x1E), true);
            for (int i = 0x14; i <= 0x18; ++i)                             // 20..24
                spawn(i, si + rnd(0x1E), di + 0x0F + rnd(0x23), true);
        }
        game_timer_set(5);                                                // 1033B(5)
        sfx(0x33);                                                        // 4F4CD(33h)
        for (int cx = 0; cx <= 8 && !should_exit_; ++cx) {                // 9 frames
            frame(); gblit();
            ring(cx < 5 ? 10 : 9, si, di);                                 // F0E22=F0DAA[10] / F0E16=F0DAA[9]
            draw_p(); present();
        }
        for (int cx = 0; cx <= 2 && !should_exit_; ++cx) {                // 3 frames
            frame(); gblit(); ring(phase, si, di);                         // F0DAA[var_10]
            draw_p(); present();
        }
    }
    // (6) last wave and wait for the particles to die
    for (int i = 0x23; i <= 0x27; ++i)                                     // 35..39
        spawn(i, si + 0x19 + rnd(0x19), di + 0x0A + rnd(0x19), true);
    sfx(0x33);                                                            // 4F4CD(33h)
    for (int i = 0x1E; i <= 0x22; ++i)                                     // 30..34
        spawn(i, si + 5 + rnd(0x1E), di + 0x0F + rnd(0x1E), true);
    for (int k = 0; k < 0x1E && !should_exit_; ++k) {
        bool any = false;
        for (const auto& s : p) if (s.alive) any = true;
        if (!any) break;
        frame(); gblit();
        ring(10, si, di);                                                  // F0E24..F0E2A = F0DAA[10]
        draw_p(); present();
    }
    // (7)
    game_timer_set(8);                                                    // 1033B(8)
    for (int k = 0; k < 4 && !should_exit_; ++k) {
        frame(); gblit(); ring(10, si, di);                                // F0E24..F0E2A = F0DAA[10]
        present();
    }
    battle_dim_row0_ = false;                                              // 44D6E(0, 100h)
    pal_load_range(0, 256, &battle_palette_[0][0]);                        // 5F9AE(0)
    // (8) 8 frames (cx=7..0, F0DAA[cx] only when cx>=4), then 4 frames (cx=3..0)
    for (int cx = 7; cx >= 0 && !should_exit_; --cx) {
        frame(); gblit();
        if (cx >= 4) ring(cx, si, 0x14);
        present();
    }
    for (int cx = 3; cx >= 0 && !should_exit_; --cx) {
        frame(); gblit(); ring(cx, si, 0x14); present();
    }
    // (9)
    game_timer_set(0x14);                                                  // 1033B(14h)
    frame(); gblit(); ring(0, si, di); present();                          // F0DAC = F0DAA[0]
    battle_highlight_clear();                                             // 44D75
    A.facing = saved_facing;                                              // restore [63h]
    for (int ti : battle_target_list_) {                                  // 12124
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        units_[(size_t)ti].anim_frame = 0;
    }
    battle_weapon_2caf7_loop(unit_index, 4, 0x1770);                       // 2CAF7(4, 1770h)
    battle_nsp_release();                                                 // 1FFCD
    weather_tick_enabled_ = true;                                         // 103A0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 367EE unit=%d cnt=%d nang=%d\n", A.id, (int)battle_target_list_.size(), nframes);
}

// ==== sub_3AEE8: io.nsp (edi 12, f-blast family) ====
// Original is 478 lines plus sub_3ADBE (12 particle frames) and sub_3A950 (80-particle render).
// The first frames are pinned by the timeline probe: (1) 8 frames F0DAA[0..7] at (si, di),
// 1033B(5), then 12 frames of 3ADBE (only 9..15 pinned: 3 frames ring 1 + 4 frames ring 0).
//   (1) 8 frames F0DAA[0..7] at (10971+0Ch, 10988+2)
//   (2) 1033B(5) + 80 particles (life 499BD(3)+1, angle 499BD(168h), offset rand&1 -> 2/4)
//   (3) 15 x 3ADBE (amplitude 1): 12 frames (var_4 = 2..7 x 2 shakes), every frame
//      5EE51(F0DAA[var_4]) + 80 particles + target [65h] shake (-1 even sub-frame, +1 odd)
//   (4) 9 x 3ADBE (amplitude 2), 4F4CD(32h) per block
//   (5) 4 x 3ADBE (amplitude 3)
//   (6) 2 rounds x 4 frames: 5EE51(F0DAA[round]) + target [65h] +-5 alternating
//   (7) tail: [1E]|=2, [1F]=5Ah, 197AD(type < 12Ch), 1FFCD, 5F9AE(0), 44D6E(0),
//      44D75, 11E04, restore [63h], 12124 all, 2CAF7(3, 1B58h), 103A0
//
// sub_3A950 (80-particle render): not sprites but pixel trails.
//   There are TWO counters, which is easy to confuse:
//     - var_1C0(ecx) = steps per call. Starts at 1..3 and only the ch>1 branch increments
//       it (cap 1Eh). step == life-1 also marks the last step.
//     - arg_8(var_D0) = number of trail entries drawn. Starts at 0 and increments every
//       step (cap 22h).
//   Each slot accumulates steps along its angle (arg_C) and every step stores the current
//   coordinate at [0] of the coordinate arrays (arg_0/arg_4, words), shifting the rest up:
//   array[si] holds the newest step first.
//   Drawing happens once, on the step where step == life-1: first the 28h 3x3 head marker,
//   then the trail (si <= 10h a 5-pixel plus, afterwards one pixel, colour 28h +1 for every
//   si mod 0Ah == 4, max 2Fh).
//   Leaving the bounds (-3Ch/17Ch, -3Ch/104h) only clears alive; that call still draws.
//   Sub-counter(arg_10), phase(arg_14), offset(arg_18): when ch > 1 and the sub-counter
//   passes life, life = min(life+1, 1Eh); every three phases the offset moves +1 (cap 0Ah,
//   ch=2) or -1 (floor 6, ch=3); each call speed = (speed - off) mod 168h.
//   The first call has life = 2, i.e. 14 writes per slot (9 marker + 5 plus).
// Ring entry: 3ADBE builds its index with `mov eax,[esp+10h+var_8+2]; sar eax,16`; that dword
//   load is frame +0Ah, and shifting by 16 gives the word at frame +0Ch, which is 3ADBE's own
//   counter var_4 (2..7), not the high word of ebx (var_8) on entry. Verified on all 336 blits
//   (entries 2..7, 12 frames per call). ebx is the 46h byte buffer (var_2C) passed by 3AEE8
//   and goes straight to 3A950's register argument.
void CombatII::battle_fx_3aee8_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& A = units_[(size_t)unit_index];
    if (!battle_nsp_load("io.nsp") || magic_frames_.empty()) {            // 1FEA4
        if (battle_trace_) fprintf(stderr, "COMBAT: warning: 3AEE8 unit=%d io.nsp missing\n", A.id);
        return;
    }
    const int nframes = (int)magic_frames_.size();
    weather_tick_enabled_ = false;                                        // 103A8
    const int8_t saved_facing = A.facing;                                 // save [63h]
    A.facing = 0;
    for (int ti : battle_target_list_) battle_highlight_add(ti);          // 44D89
    const int ax0 = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X;
    const int ay0 = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y;
    const int si = ax0 + 0x0C;                                            // 10971+0Ch
    const int di = ay0 + 2;                                               // 10988+2
    battle_dim_row0_ = true;                                              // 44D6E(1, 28h)
    fx_pal_mix(0x60, 0x100, 0, 0x00);                                     // 30824(0)
    game_timer_set(0x14);                                                 // 1033B(14h)
    auto ring = [&](int idx, int x, int y) {                              // 5EE51(F0DAA[idx])
        if (idx < 0 || idx >= nframes) return;
        const MagicFxFrame& e = magic_frames_[(size_t)idx];
        battle_fx_blit(idx, x + e.xo, y + e.yo);
    };
    auto frame = [&]() { battle_fx_world(); battle_fx_overlay(); };        // 454B3
    auto present = [&]() { battle_fx_commit(); wait_game_tick(); };        // 5E9E8,499E4
    for (int cx = 0; cx <= 7 && !should_exit_; ++cx) {
        // (1) 8 frames
        frame(); ring(cx, si, di); present();
    }
    game_timer_set(5);                                                    // 1033B(5)
    // Slot layout in 3A950's argument order: alive(eax), x float(edx), y float(ebx), steps(ecx,
    // var_1C0), angle(arg_C), trail count(arg_8, var_D0), sub-counter(arg_10), phase(arg_14),
    // offset(arg_18). ch(arg_1C) is 1/2/3 per block from 3AEE8; see the sub_3A950 comment
    // above (the two counters are the key part).
    struct PS {
        bool alive = true;
        float x = 0, y = 0;
        int speed = 0, off = 0, up = 0, ph = 0;
        int life = 1;                                                      // var_1C0: steps per call
        int trail = 0;                                                     // var_D0: trail entries
        int16_t tx[0x24] = {}, ty[0x24] = {};                              // arg_0/arg_4[slot][si]
    };
    std::vector<PS> p(80);
    for (int i = 0; i < 80; ++i) {                                        // init the 80 particles
        p[(size_t)i].x = (float)(si - 4);                                 // [esp+42Ah]>>16 - 4
        p[(size_t)i].y = (float)(di - 0x17);                              // [esp+426h]>>16 - 17h
        p[(size_t)i].speed = (int)combat_rand(0x168);                     // 499BD(168h) - angle in degrees
        p[(size_t)i].off = combat_rand(2) ? 4 : 2;                        // var_24: rand&1 -> 2/4
        p[(size_t)i].life = 1 + (int)combat_rand(3);                      // 499BD(3)+1
    }
    auto draw_p = [&](int ch) {                                           // sub_3A950
        uint8_t* buf = video_.get_backbuffer();
        if (!buf) return;
        auto put = [&](int x, int y, uint8_t col) {
            if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
                buf[(size_t)y * SCREEN_W + (size_t)x] = col;
        };
        auto head = [](PS& s, int xi, int yi) {                            // loc_3AC54
            s.tx[0] = (int16_t)xi;
            s.ty[0] = (int16_t)yi;
            for (int i = s.life - 1; i >= 0; --i) {                        // [i+1] = [i]
                s.tx[i + 1] = s.tx[i];
                s.ty[i + 1] = s.ty[i];
            }
        };
        for (auto& s : p) {
            if (!s.alive) continue;
            const double ang = (double)s.speed * 0.017453292519943295;    // flt_74A68=cos / flt_744C8=sin
            const float ca = (float)std::cos(ang);
            const float sa = (float)std::sin(ang);
            for (int step = 0; s.life > step; ++step) {                   // loc_3ACDE - life is fixed for the whole call
                s.x += ca;
                s.y += sa;
                // __CHP + fistp truncates toward zero; this is not lround.
                const int xi = (int)s.x;
                const int yi = (int)s.y;
                // 3A950 death bounds: x < -3Ch or > 17Ch, y < -3Ch or > 104h
                if (xi < -0x3C || xi > 0x17C || yi < -0x3C || yi > 0x104) s.alive = false;
                if (step != s.life - 1) {                                 // still stepping: store and shift
                    head(s, xi, yi);
                } else {                                                  // last step: marker and trail
                    for (int dy = -1; dy <= 1; ++dy)                       // 28h 3x3 head marker
                        for (int dx = -1; dx <= 1; ++dx) put(xi + dx, yi + dy, 0x28);
                    int ramp = 0;                                         // si = 0 is the newest step
                    for (int si = 0; si < s.trail; ++si) {
                        if (si % 10 == 4) ++ramp;
                        if (ramp > 7) ramp = 7;
                        const uint8_t col = (uint8_t)(0x28 + ramp);
                        const int px = s.tx[si], py = s.ty[si];
                        if (si <= 0x10) {                                 // si <= 10h -> 5-pixel plus
                            put(px, py, col); put(px + 1, py, col); put(px - 1, py, col);
                            put(px, py - 1, col); put(px, py + 1, col);
                        } else {
                            put(px, py, col);
                        }
                    }
                    head(s, xi, yi);
                }
                if (++s.trail > 0x22) s.trail = 0x22;                     // loc_3ACBE (every step, cap 22h)
            }
            if (ch > 1) {                                                 // cmp ch,1 / jle
                if (++s.up > s.life) {                                    // arg_10 sub-counter
                    s.up = 0;
                    if (++s.life > 0x1E) s.life = 0x1E;                   // clamp to 1Eh (may also decrease)
                }
                if (++s.ph > 2) {                                         // arg_14 phase
                    s.ph = 0;
                    if (ch == 2) { if (s.off < 0x0A) ++s.off; }            // cap 0Ah
                    else         { if (s.off > 6) --s.off; }               // floor 6
                }
            }
            s.speed = (s.speed - s.off + 720) % 360;                      // loc_3AD72
        }
    };
    auto shake = [&](int amount, bool subtract) {                          // [65h] -+
        for (int ti : battle_target_list_) {
            if (ti < 0 || (size_t)ti >= units_.size()) continue;
            units_[(size_t)ti].walk_off_x += subtract ? -(int16_t)amount : (int16_t)amount;
        }
    };
    // 3ADBE: var_4 = 2..7 (6 values) x 2 sub-frames = 12 frames; the ring entry is var_4.
    // The caller passes (3ADBE shake amplitude, 3A950 ch):
    //   (3) 15 blocks push 1, push 1 -> (1, 1); (4) 9 blocks push 3, push 2 -> (3, 2);
    //   (5) 4 blocks push 5, push 3 -> (5, 3)
    auto adbe = [&](int amount, int ch) {
        for (int k = 2; k <= 7 && !should_exit_; ++k) {                     // loc_3ADD6 (var_4 = 2..7)
            for (int sub = 0; sub < 2 && !should_exit_; ++sub) {            // test si,si
                shake(amount, sub == 0);
                frame();                                                  // 454B3
                ring(k, si, di);                                          // 5EE51(F0DAA[var_4])
                draw_p(ch);                                                // 3A950
                present();                                                 // 5E9E8,499E4
            }
        }
    };
    for (int k = 0; k < 15 && !should_exit_; ++k) {
        // (3) 15 x 12 frames
        sfx(0x32);                                                        // 4F4CD(32h)
        adbe(1, 1);
    }
    for (int k = 0; k < 9 && !should_exit_; ++k) {                          // (4)
        sfx(0x32);
        adbe(3, 2);
    }
    for (int k = 0; k < 4 && !should_exit_; ++k) adbe(5, 3);                // (5)
    for (int round = 0; round < 2 && !should_exit_; ++round) {
        // (6) 2 x 4 frames
        for (int cx = 0; cx < 4 && !should_exit_; ++cx) {
            shake(5, (cx & 1) == 0);
            frame();
            ring(round, si, di);
            present();
        }
    }

    // (7) tail
    for (int ti : battle_target_list_) {
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        if (T.sprite_type >= 0x12C) continue;                              // cmp [61h], 12Ch
        battle_weapon_apply_status(ti, 0x02, 0x5A);                        // [1E]|=2 , [1F]=5Ah
        battle_remove_from_ready_queue(ti);                                 // sub_197AD
    }
    battle_nsp_release();                                                  // 1FFCD
    pal_load_range(0, 256, &battle_palette_[0][0]);                        // 5F9AE(0)
    battle_dim_row0_ = false;                                              // 44D6E(0)
    battle_highlight_clear();                                              // 44D75
    A.facing = saved_facing;                                               // restore [63h]
    for (int ti : battle_target_list_) {                                   // 12124
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        units_[(size_t)ti].anim_frame = 0;
    }
    battle_weapon_2caf7_loop(unit_index, 3, 0x1B58);                        // 2CAF7(3, 1B58h)
    weather_tick_enabled_ = true;                                          // 103A0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 3AEE8 unit=%d cnt=%d io=%d\n", A.id, (int)battle_target_list_.size(), nframes);
}
