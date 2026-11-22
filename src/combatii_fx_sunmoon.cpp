// Magic effect drivers: moon (moon.nsp) and sun light (sunl.nsp, sunl2.nsp)
//   sub_58619 (dasm 97940..98385, called from sub_2A75A+2DE)  Star Tearer stage 3 - moon.nsp
//   sub_5A267 (dasm 100199..100407, called from sub_2A75A+446) Sun Light - sunl.nsp
//   sub_5A566 (dasm 100408..100639, called from sub_2A75A+44F) Sun Light variant - sunl2.nsp
//
// The three drivers share the file/load/palette setup but not one frame loop:
//   1FEA4(<file>.nsp) -> 103A8 (clear effect state) -> 1033B (tick period) -> 11EE4 (release
//   occupancy, 5A267/5A566) -> caster screen position via 10971+0Ch / 10988 -> palette fade
//   (5FA2C, or 5F945+5FC21+5FD1C) -> driver-specific frames -> 61115/103A0 restore -> 1FFCD.
//   58619's main loop clears the work buffer and composes moon frame 0 -> 12F2A units ->
//   particles -> moon frame 1 -> 5E9E8; it does not call 10AEB/12BEE/454D5. Its intro draws
//   the same layers over the previous buffer without a clear. 5A267/5A566 keep their own
//   frame chains.
//
// Port mapping, same conventions as combatii_fx_support.cpp:
//   454B3+5E9E8 = battle_fx_present_454b3(), 10AEB+12BEE = render_battle_map(),
//   454D5+5E9E8 = battle_fx_present_454d5(), 499E4 = wait_game_tick(),
//   1033B = game_timer_set(), 499BD = combat_rand(), 4F4CD = sfx(),
//   1FFCD = battle_nsp_release(), 5FA2C = pal_scale_range(0, 0x100, ..., pct),
//   5FC21+5FD1C = pal_shift_fade(mul), 5F29D/5F305 = battle_fx_blit_tinted,
//   5F570/5F611 = battle_fx_blit_scaled, 5EE51/1FFFE = battle_fx_blit,
//   10971/10988 = UNIT_ANCHOR.
//
// Deviations shared by all three:
//   - The original draws sprites from the 12-byte unk_F0DAA table ([+2] xo, [+4] yo, [+8] flat).
//     magic_frames_ holds the same table in the same order and stride, so the port uses frame
//     numbers instead: unk_F0DC2/F0DCE/F0DDA = frames 2/3/4, dword_F0DB2/F0DBE = frames 0/1,
//     unk_F0DAA+0x48 = frame 6. xo/yo are added by the caller, as in the original [+2]/[+4].
//   - 611AA page setup for 5A267/5A566 is folded into blit_frame_tinted, which caches the same
//     computation, so those drivers pass the original shade (0x3C) as tint_pct. 58619's
//     6106C(3,11h,46h) is built explicitly with fx_shade_lut_build, and its 5EDDE columns are
//     queued with battle_fx_shade_rect before the particle sprites.
//   - 61115/103A0 (palette LUT restore) has no counterpart here; each driver restores the
//     palette with pal_shift_fade(0) / pal_scale_range(100) at the end.
//   - 11EE4/11E04 (occupancy release and rewrite) are modelled with fx_sprite_hidden, which
//     the port's CC998 map pass checks. Both sun drivers release the caster and then redraw
//     it explicitly with 12F2A, so only the map pass changes.
//   - The 5F611/5F570 target sizes come from original stack locals (var_18/var_1C/var_20)
//     whose values cannot be read statically, so the port approximates them from the frame
//     counter (see each driver).
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <vector>

namespace {

// Unit sprite top-left anchor: sub_10971(x) + 0Ch / sub_10988(y).
//   Same convention as combatii_fx_weapon.cpp (UNIT_ANCHOR_X/Y on top of TILE_ANCHOR_X/Y).
inline void sunmoon_anchor(const CombatUnitPlacement& u, int cam_x, int cam_y, int fine_x, int fine_y,
                           int& sx, int& sy) {
    sx = ((int)(int8_t)u.x - cam_x) * TOWN_TILE_W + fine_x + UNIT_ANCHOR_X;
    sy = ((int)(int8_t)u.y - cam_y) * TOWN_TILE_H + fine_y + TILE_ANCHOR_Y;
}

} // namespace

// sub_58619: moon.nsp (Star Tearer 3rd stage; sub_2A75A kind 1 k==2 loads regen.ssp/i-fist.ssp).
//   Port signature: (unit_index, tx, ty, k). The caster is unit_index; k is unused.
//   (1) 585B1 collects hostile units in the 13x13 screen area (word_F13AA/dword_F0BA8) and
//       sets each pose via 1214F: 0x0A (collapsed) for normal units, 0 for large ones.
//   (2) sfx(0x32) -> 1033B(1) -> 5FA2C fades the palette 100% -> 0% (screen darkens).
//   (3) moon frame 0 at (0,0) -> the hostile units (12F2A) -> moon frame 1 at (1,1), then
//       5FA2C brings the palette back 0% -> 100% -> 1033B(5) -> 5E9E8 -> 5 ticks. The intro
//       has no memset; its frame 0 is drawn over the buffer 454B3 left behind.
//   (4) 10 falling particle slots (var_34[]), one every 3rd frame up to frame 100, life 0x12,
//       spawned at x = rand()%0x12C+0x0A, y = rand()%0x8C+0x32 with sfx(0x33).
//       Before the loop: 6106C(3,11h,46h) builds page 3 (6104A(3) selects it).
//   (5) Main loop: frame 0 is drawn at (0, var_14) before the units, and frame 1 at
//       (0, var_18) after the particles; var_14 starts 0 and flips -1/+1, var_18 flips -2/+2.
//       The original does not redraw the map in this loop. Each particle first dims its
//       vertical trail through that LUT: x = px-5..px+5 while
//       life > 0x0A, x = px-life/2..px+life/2 while 0 < life <= 0x0A, both y = 0..py.
//       The particle then decrements its life and selects the sprite from that decremented
//       value: > 0x0F draws frame 2 at x-5+{1,4,7}, y+2 (3 sprites); 0x0A < life <= 0x0F
//       draws frame 3 in the same layout; <= 0x0A draws no sprite. The 0 < life <= 0x0A path
//       instead tests the pre-decrement life and, while it is > 6, draws frame 4 three times
//       at x-5+{1,4,7}, y+2 before decrementing. At life 0 the done counter (var_4) advances.
//       var_4 is cleared on every pass, so the loop ends after frame 100 once a single pass
//       sees all 10 slots dead. var_18 is only the frame-1 y offset; particle sprites use
//       py+2, never a jittered py.
//   (6) dword_BA838 = 0 -> repeat the (2) fade (darken) -> 454B3+5E9E8 -> brighten -> 61115 ->
//       12124 restores each pose -> 103A0 -> 1FFCD.
int CombatII::battle_fx_58619(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("moon.nsp") || magic_frames_.empty()) return 0;
    weather_tick_enabled_ = false;                             // sub_103A8 (right after 1FEA4)
    const int nframes = (int)magic_frames_.size();
    auto& caster = units_[(size_t)unit_index];
    camera_center_on(tx, ty);                                  // sub_1124A

    // (1) sub_585B1: hostile units in the 13x13 screen area (11DB2 + 1201D hostility + dedup)
    std::vector<int> list;
    const int caster_army = battle_effective_army(caster);
    for (int y = cam_tile_y_; y < cam_tile_y_ + 13; ++y) {
        for (int x = cam_tile_x_; x < cam_tile_x_ + 13; ++x) {
            const int oi = battle_unit_index_at(x, y);
            if (oi < 0 || (size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!o.active) continue;
            if (!battle_armies_hostile(caster_army, battle_effective_army(o),
                                       battle_hostility_inverted_)) continue;
            if (std::find(list.begin(), list.end(), oi) == list.end()) list.push_back(oi);
        }
    }
    // sub_1214F: pose 0x0A normally, 0 for large sprites ([61h] >= 0x12C).
    for (int oi : list) {
        auto& o = units_[(size_t)oi];
        o.anim_frame = (o.sprite_type >= 0x12C) ? 0 : 0x0A;
    }

    game_timer_set(1);                                         // sub_1033B(1)
    sfx(0x32);                                                 // sub_4F4CD(32h)

    // (2) 5FA2C: 100% -> 0% (step -2)
    for (int pct = 0x64; pct >= 0 && !should_exit_; pct -= 2) {
        pal_scale_range(0, 0x100, &battle_master_palette_[0][0], pct);
        wait_game_tick();
    }
    if (should_exit_) { weather_tick_enabled_ = true; battle_nsp_release(); return 1; }

    // 5EFB9 draws the moon frame directly on the work buffer; 12F2A draws only the hostile
    // units. The map/weather pass (10AEB/12BEE/4512E) and 454D5 are not part of this driver.
    // The intro has no memset; only the main loop clears first.
    auto present_58619 = [&](bool clear_buffer, int f0x, int f0y, int f1x, int f1y) {
        if (clear_buffer) video_.clear();                      // memset_ (main loop only)
        if (0 < nframes) fx_blit_nsp(0, f0x, f0y);              // 5EFB9 frame 0
        for (int oi : list) battle_draw_unit_now(oi);           // 12F2A
        battle_fx_flush();                                      // queued particles
        if (1 < nframes) fx_blit_nsp(1, f1x, f1y);              // 5EFB9 frame 1
        battle_fx_commit();                                     // 5E9E8
    };
    // The second 5E9E8 only presents the buffer; it does not redraw the scene or the overlay.
    auto present_existing = [&]() {
        video_.present();
        dump_frame_if_requested();
    };

    // (3) frame 0 at (0,0), units, frame 1 at (1,1) -> palette restore
    present_58619(false, 0, 0, 1, 1);
    wait_game_tick();                                          // 499E4 after the first 5E9E8
    for (int pct = 0; pct <= 0x64 && !should_exit_; pct += 2) {
        pal_scale_range(0, 0x100, &battle_master_palette_[0][0], pct);
        wait_game_tick();
    }
    game_timer_set(5);                                         // sub_1033B(5)
    present_existing();                                        // sub_5E9E8 (same buffer)
    for (int i = 0; i < 5 && !should_exit_; ++i) wait_game_tick();

    fx_shade_lut_build(0x11, 0x46);                            // sub_6106C(3,11h,46h)

    // (4) 10 falling particle slots (var_34[0Ah], x=var_5C[], y=var_48[], life 0x12)
    struct MoonP { int x = 0, y = 0, life = 0; };
    constexpr int kSlots = 10;
    MoonP part[kSlots];
    int spawn_slot = 0;                                        // var_C (cycles 0..9, 10 slots)

    game_timer_set(5);                                         // sub_1033B(5), reset before the loop
    const int shake_base_fine_y = cam_fine_y_;                 // original dword_BA838 value
    int cam_shake = 0;                                         // var_14 (-1/+1, starts 0)
    int shake_amp = 0;                                         // var_18 (-2 / +2, starts 0)
    int done = 0;                                              // var_4
    for (int frame = 0; frame < 0x400 && !should_exit_; ++frame) {
        if (frame >= 0x64 && done >= kSlots) break;            // `var_10 >= 64h && var_4 >= 0Ah`
        // Per-frame screen shake: var_14 starts 0, so the first frame is -1, then +1.
        cam_shake = (cam_shake < 0) ? 1 : -1;
        cam_fine_y_ = shake_base_fine_y + cam_shake;           // dword_BA838
        // var_18 starts 0, so the first loop is -2, then +2 (frame 1's y offset, not a
        // particle jitter: 58990/589E6/58A83 read py[] directly).
        shake_amp = (shake_amp == -2) ? 2 : -2;

        // var_10 is incremented at the top of the original loop; the first loop is 1.
        const int loop_frame = frame + 1;
        if ((loop_frame % 3) == 0 && loop_frame < 0x64) {
            MoonP& p = part[spawn_slot];
            if (p.life == 0) {
                p.life = 0x12;
                p.x = combat_rand(0x12C) + 0x0A;               // rand()%0x12C + 0x0A
                p.y = combat_rand(0x8C) + 0x32;                // rand()%0x8C + 0x32
                sfx(0x33);                                     // sub_4F4CD(33h)
            }
            spawn_slot = (spawn_slot + 1) % kSlots;            // `cmp di,9; jle` -> 0..9, wraps at 10
        }

        // var_4 counts the dead slots of this pass only: the original clears it right before
        // the slot loop (58AC3's `inc [var_4]` also only sees this pass), and the frame-100
        // gate reads the value left by the previous pass.
        done = 0;                                              // var_4 = 0
        for (int i = 0; i < kSlots; ++i) {
            MoonP& p = part[i];
            // 5EDDE: dim the column the particle has already fallen through. The original
            // reads py[] and applies the page-3 LUT to the frame 0/units already drawn,
            // before the particle sprites.
            if (p.life > 0x0A) {
                battle_fx_shade_rect(p.x - 5, 0, p.x + 5, p.y); // 5EDDE (life > 0Ah)
                // `dec al` comes before both compares, so the sprite choice uses life-1.
                const int l2 = p.life - 1;
                if (l2 > 0x0F) {                               // frame 2 x 3
                    for (int off = 1; off < 0x0A; off += 3) {
                        if (2 < nframes)
                            battle_fx_blit_clipped(2, p.x - 5 + off + magic_frames_[2].xo,
                                                   p.y + 2 + magic_frames_[2].yo);
                    }
                } else if (l2 > 0x0A) {                        // frame 3 x 3
                    for (int off = 1; off < 0x0A; off += 3) {
                        if (3 < nframes)
                            battle_fx_blit_clipped(3, p.x - 5 + off + magic_frames_[3].xo,
                                                   p.y + 2 + magic_frames_[3].yo);
                    }
                }
                --p.life;
            } else if (p.life > 0) {                           // 1..0x0A: shade, maybe frame 4 x 3
                const int half = p.life / 2;
                battle_fx_shade_rect(p.x - half, 0, p.x + half, p.y); // 5EDDE (life <= 0Ah)
                if (p.life > 6) {                              // pre-decrement life (`cmp,6; jbe`)
                    for (int off = 1; off < 0x0A; off += 3) {
                        if (4 < nframes)
                            battle_fx_blit_clipped(4, p.x - 5 + off + magic_frames_[4].xo,
                                                   p.y + 2 + magic_frames_[4].yo);
                    }
                }
                --p.life;
            } else {
                ++done;
            }
        }

        present_58619(true, 0, cam_shake, 0, shake_amp);        // frame 0 below units, frame 1 on top
        wait_game_tick();
    }
    cam_fine_y_ = CAM_FINE_Y_REST;                             // dword_BA838 = 0

    // (6) Finish: darken -> one frame -> brighten -> LUT and pose restore
    for (int pct = 0x64; pct >= 0 && !should_exit_; pct -= 2) {
        pal_scale_range(0, 0x100, &battle_master_palette_[0][0], pct);
        wait_game_tick();
    }
    battle_fx_world();                                         // sub_454B3
    battle_fx_commit();                                        // sub_5E9E8
    for (int pct = 0; pct <= 0x64 && !should_exit_; pct += 2) {
        pal_scale_range(0, 0x100, &battle_master_palette_[0][0], pct);
        wait_game_tick();
    }
    pal_scale_range(0, 0x100, &battle_master_palette_[0][0], 100);    // 61115 counterpart (original palette)
    for (int oi : list) {
        if ((size_t)oi < units_.size()) reset_unit_pose(units_[(size_t)oi]);   // sub_12124
    }
    weather_tick_enabled_ = true;                              // sub_103A0
    battle_nsp_release();                                      // sub_1FFCD
    return 1;
}

// sub_5A267: sunl.nsp (Sun Light).
//   Port signature: (unit_index, tx, ty, k); only unit_index is used (the caster).
//   (1) 611AA (9-byte table dword_560F0, shade 0x3C) builds the tint LUT, 1FEA4("sunl.nsp"), 103A8.
//   (2) 1033B(5) -> 11EE4 (release occupancy) -> anchor via 10971+0Ch / 10988, var_C = 0x19.
//   (3) Palette delta: 5FC21 subtracts the master from 100h copies of unk_12824F
//       (= palette[1]); each frame 5FD1C(mul) fades toward that one colour
//       (port: pal_shift_fade).
//   (4) sfx(0x32) -> while var_C > 5, two frames at a time: 12F2A redraws the caster, then
//       frame 0 at the anchor via 5F611 (scaled), then 5E9E8+499E4; var_C--.
//   (5) While var_C < 8, same 12F2A + 5F611 draw; var_C++ (grows again).
//   (6) sfx(0x33) -> 5 rounds x 10 frames: frame [var_8 = 1..5] at the anchor via 5F29D
//       (tinted), then 12F2A redraws the caster; each frame 5FD1C(old mul) is called after
//       the tick and mul += 2, capped at 0x64.
//   (7) 11E04 (rewrite occupancy) -> 5FD1C(0) -> 61115 -> 103A0 -> 1FFCD.
int CombatII::battle_fx_5a267(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("sunl.nsp") || magic_frames_.empty()) return 0;
    weather_tick_enabled_ = false;                             // sub_103A8 (right after 1FEA4)
    const int nframes = (int)magic_frames_.size();
    auto& caster = units_[(size_t)unit_index];
    // 1124A is not inside 5A267; the preceding 28889 announce path centres the camera.
    // It is repeated here for direct driver calls and has no effect after 28889.
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);
    int ax = 0, ay = 0;
    sunmoon_anchor(caster, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_, ax, ay);

    game_timer_set(5);                                         // sub_1033B(5)
    // sub_11EE4(caster): the shrink stages below redraw the caster with 12F2A, so clearing the
    // CC998 footprint only removes the duplicate from the map pass.
    caster.fx_sprite_hidden = true;
    sfx(0x32);                                                 // sub_4F4CD(32h)

    int counter = 0x19;                                        // var_C = 0x19
    const int step = 2;                                        // two frames per step
    // (4)/(5) 5F611 target width and height are both the counter (0x19..6, then 5..7), and the
    //   top-left is anchor + counter/2 + frame offset (the dword reads extract entry+4/+6 as
    //   xo/yo). 5F611 is the shrink blitter, so it is selected even when the target is larger.
    auto draw_scaled = [&](int sz) {
        if (nframes <= 0) return;
        const MagicFxFrame& e = magic_frames_[0];
        battle_fx_redraw_unit(unit_index);                     // sub_12F2A (between 454B3 and 5F611)
        battle_fx_blit_scaled(0, ax + sz / 2 + e.xo, ay + sz / 2 + e.yo,
                              sz, sz, false, true);            // sub_5F611
    };
    while (counter > 5 && !should_exit_) {
        for (int i = 0; i < step && !should_exit_; ++i) {
            draw_scaled(counter);
            battle_fx_present_454b3();                     // sub_454B3 + sub_5E9E8
            wait_game_tick();
        }
        --counter;
    }
    while (counter < 8 && !should_exit_) {
        for (int i = 0; i < step && !should_exit_; ++i) {
            draw_scaled(counter);
            battle_fx_present_454b3();                     // sub_454B3 + sub_5E9E8
            wait_game_tick();
        }
        ++counter;
    }

    // (6) 50 frames (5 rounds x 10): frame = round (1..5), then 12F2A redraws the caster.
    //   5FD1C gets the old mul after the tick, then mul += 2 (capped at 0x64).
    sfx(0x33);                                                 // sub_4F4CD(33h)
    int mul = 0;                                               // si: 0 -> 0x64 in steps of 2
    for (int round = 1; round <= 5 && !should_exit_; ++round) {
        for (int i = 0; i < 10 && !should_exit_; ++i) {
            if (round < nframes)
                battle_fx_blit_tinted_unclipped(round, ax + magic_frames_[round].xo,
                                                ay + magic_frames_[round].yo, 0x3C,
                                                {0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17});
            battle_fx_redraw_unit(unit_index);                 // sub_12F2A after 5F29D
            render_battle_map(false, false, true);              // sub_10AEB + sub_12BEE
            battle_fx_present_454d5();                          // sub_454D5 + sub_5E9E8
            wait_game_tick();
            pal_shift_fade(mul);                               // 5FD1C(old mul)
            mul = std::min(0x64, mul + 2);
        }
    }

    caster.fx_sprite_hidden = false;                           // sub_11E04(caster)
    pal_shift_fade(0);                                         // 5FD1C(0) - original palette
    weather_tick_enabled_ = true;                              // sub_103A0
    battle_nsp_release();                                      // sub_1FFCD
    return 1;
}

// sub_5A566: sunl2.nsp (Sun Light variant).
//   Port signature: (unit_index, tx, ty, k); only unit_index is used (the caster).
//   (1) 611AA (9-byte table dword_560F9, shade 0x3C) -> 1FEA4("sunl2.nsp") -> 103A8 -> 1033B(5)
//       -> 11EE4 -> anchor (10971+0Ch / 10988) -> delta palette (5FC21) -> sfx(0x34).
//   (2) 5 rounds (var_10 = 1..5), each 8 frames (var_1C = 6..13): 12F2A redraws the caster,
//       then draws var_10 copies of frame var_1C at size 8*(count-1)+step-5, centered, with
//       454D5 + 5E9E8 per copy.
//   (3) sfx(0x32) -> 5 rounds (var_8 = 1..5) x 10 frames: 5F29D frame var_8 at the anchor,
//       then 12F2A redraws the caster, then frame 6 via 5F570 at size 41..90, with
//       5FD1C(old mul) after the tick and mul += 2, capped at 0x64.
//   (4) 11E04 -> sfx(0x33) -> 5FD1C(0) -> 61115 -> 103A0 -> 1FFCD.
int CombatII::battle_fx_5a566(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("sunl2.nsp") || magic_frames_.empty()) return 0;
    weather_tick_enabled_ = false;                             // sub_103A8 (right after 1FEA4)
    const int nframes = (int)magic_frames_.size();
    auto& caster = units_[(size_t)unit_index];
    // 1124A comes from the caller's 28889 announce path, not from 5A566. Kept for direct
    // driver calls; it is a no-op when the caller has already centred the camera.
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);
    int ax = 0, ay = 0;
    sunmoon_anchor(caster, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_, ax, ay);

    game_timer_set(5);                                         // sub_1033B(5)
    // sub_11EE4(caster): both stages redraw the caster with 12F2A, so the map pass must skip it.
    caster.fx_sprite_hidden = true;

    // (2) count 1..5, step 6..13. The original draws `count` copies of frame `step` at
    // size 8*(count-1)+step-5, centered on the frame offset (probe: 1..40 px).
    for (int count = 1; count <= 5 && !should_exit_; ++count) {
        for (int step = 6; step <= 0x0D && !should_exit_; ++step) {
            const int fi = std::min(nframes - 1, step);
            const int sz = 8 * (count - 1) + (step - 5);
            const MagicFxFrame& e = magic_frames_[(size_t)fi];
            for (int n = 0; n < count && !should_exit_; ++n) {
                battle_fx_redraw_unit(unit_index);                 // sub_12F2A (after 12BEE, before 5F570)
                battle_fx_blit_scaled(fi, ax + e.xo - sz / 2, ay + e.yo - sz / 2,
                                      sz, sz, true);
                render_battle_map(false, false, true);         // sub_10AEB + sub_12BEE
                battle_fx_present_454d5();                     // sub_454D5 + sub_5E9E8
                wait_game_tick();
            }
        }
    }

    // (3) 5 rounds: 5F29D frame 1..5 at the anchor, then 12F2A redraws the caster, then
    // frame 6 via 5F570 with the size 41..90 growing by one each frame (probe-confirmed).
    sfx(0x32);                                                 // sub_4F4CD(32h)
    int fade = 0;
    for (int round = 1; round <= 5 && !should_exit_; ++round) {
        for (int i = 0; i < 10 && !should_exit_; ++i) {
            if (round < nframes) {
                const MagicFxFrame& e = magic_frames_[(size_t)round];
                battle_fx_blit_tinted_unclipped(round, ax + e.xo, ay + e.yo, 60,
                                                {0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17}); // 611AA 3Ch
            }
            battle_fx_redraw_unit(unit_index);                 // sub_12F2A after 5F29D
            const int sz = 41 + (round - 1) * 10 + i;
            if (6 < nframes) {
                const MagicFxFrame& e = magic_frames_[6];
                battle_fx_blit_scaled(6, ax + e.xo - sz / 2, ay + e.yo - sz / 2,
                                      sz, sz, true);          // 5F570
            }
            render_battle_map(false, false, true);             // sub_10AEB + sub_12BEE
            battle_fx_flush();                                 // sub_12E01 (5F29D + 12F2A + 5F570)
            battle_fx_commit();                                // sub_5E9E8 (no 454D5 here)
            wait_game_tick();
            pal_shift_fade(fade);                              // 5FD1C
            fade = std::min(100, fade + 2);
        }
    }

    caster.fx_sprite_hidden = false;                           // sub_11E04(caster)
    sfx(0x33);                                                 // sub_4F4CD(33h)
    pal_shift_fade(0);                                         // 5FD1C(0)
    weather_tick_enabled_ = true;                              // sub_103A0
    battle_nsp_release();                                      // sub_1FFCD
    return 1;
}
