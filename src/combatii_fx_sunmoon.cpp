// Magic effect drivers: moon (moon.nsp) and sun light (sunl.nsp, sunl2.nsp)
//   sub_58619 (dasm 97940..98385, called from sub_2A75A+2DE)  Heal Wind - moon.nsp
//   sub_5A267 (dasm 100199..100407, called from sub_2A75A+446) Sun Light - sunl.nsp
//   sub_5A566 (dasm 100408..100639, called from sub_2A75A+44F) Sun Light variant - sunl2.nsp
//
// All three share one original skeleton:
//   1FEA4(<file>.nsp) -> 103A8 (clear effect state) -> 1033B (tick period) -> 11EE4 (release
//   occupancy, sub_58619 only) -> caster screen position via 10971+0Ch / 10988 -> palette fade
//   (5FA2C, or 5F945+5FC21+5FD1C) -> per frame { 10AEB (map) + 12BEE (units) + frame blit +
//   454D5 overlay + 5E9E8 + 499E4 } -> 61115/103A0 restore -> 1FFCD release.
//
// Port mapping, same conventions as combatii_fx_support.cpp:
//   454B3+12BEE+454D5+5E9E8 = battle_fx_present(), 499E4 = wait_game_tick(),
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
//   - 611AA/6106C/6104A (tint LUT setup and selection) are folded into blit_frame_tinted,
//     which caches the same computation, so the port skips LUT selection and passes the
//     original shade (0x3C/0x46) as tint_pct.
//   - 61115/103A0 (palette LUT restore) has no counterpart here; each driver restores the
//     palette with pal_shift_fade(0) / pal_scale_range(100) at the end.
//   - 11EE4/11E04 (occupancy release and rewrite) are dropped because the port models
//     occupancy through units_.
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

// sub_58619: moon.nsp (Heal Wind).
//   Port signature: (unit_index, tx, ty, k). The caster is unit_index; k is unused.
//   (1) 585B1 collects hostile units in the 13x13 screen area (word_F13AA/dword_F0BA8) and
//       sets each pose via 1214F: 0x0A (collapsed) for normal units, 0 for large ones.
//   (2) sfx(0x32) -> 1033B(1) -> 5FA2C fades the palette 100% -> 0% (screen darkens).
//   (3) moon frame 0 (dword_F0DB2) -> the units (12BEE) -> moon frame 1 (dword_F0DBE), then
//       5FA2C brings the palette back 0% -> 100% -> 1033B(5) -> 5E9E8 -> 5 ticks.
//   (4) 10 falling particle slots (var_34[]), one every 3rd frame up to frame 100, life 0x12,
//       spawned at x = rand()%0x12C+0x0A, y = rand()%0x8C+0x32 with sfx(0x33).
//   (5) Main loop: shakes camera fine Y by +-1 (var_14) and particle y by +-2 (var_18).
//       life > 0x0F draws frame 2 at x-5+{1,4,7}, y+2 (3 sprites); 0x0A < life <= 0x0F draws
//       frame 3 in the same layout; life <= 0x0A draws frame 4 at x-5+life/2. At life 0 the
//       done counter (var_4) advances. The loop ends after frame 100 when all 10 are done.
//   (6) dword_BA838 = 0 -> repeat the (2) fade (darken) -> 454B3+5E9E8 -> brighten -> 61115 ->
//       12124 restores each pose -> 103A0 -> 1FFCD.
int CombatII::battle_fx_58619(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("moon.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    auto& caster = units_[(size_t)unit_index];
    camera_center_on(tx, ty);                                  // sub_1124A
    int ax = 0, ay = 0;
    sunmoon_anchor(caster, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_, ax, ay);

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
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }
    if (should_exit_) { battle_nsp_release(); return 1; }

    // (3) moon frames 0/1 + units -> palette restore
    if (0 < nframes) battle_fx_blit(0, ax + magic_frames_[0].xo, ay + magic_frames_[0].yo);
    battle_fx_present();
    if (1 < nframes) battle_fx_blit(1, ax + magic_frames_[1].xo, ay + magic_frames_[1].yo);
    battle_fx_present();
    for (int pct = 0; pct <= 0x64 && !should_exit_; pct += 2) {
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }
    game_timer_set(5);                                         // sub_1033B(5)
    battle_fx_present();                                       // sub_5E9E8
    for (int i = 0; i < 5 && !should_exit_; ++i) wait_game_tick();

    // (4) 10 falling particle slots (var_34[0Ah], x=var_5C[], y=var_48[], life 0x12)
    struct MoonP { int x = 0, y = 0, life = 0; };
    constexpr int kSlots = 10;
    MoonP part[kSlots];
    int spawn_slot = 0;                                        // var_C (cycles 0..8)

    game_timer_set(5);                                         // sub_1033B(5), reset before the loop
    const int shake_base_fine_y = cam_fine_y_;                 // original dword_BA838 value
    int shake_amp = 0;                                         // var_18 (+2 / -2)
    int done = 0;                                              // var_4
    for (int frame = 0; frame < 0x400 && !should_exit_; ++frame) {
        if (frame >= 0x64 && done >= kSlots) break;            // `var_10 >= 64h && var_4 >= 0Ah`
        // Per-frame screen shake: flip var_14 -> dword_BA838 = +-1
        const int shake = (frame & 1) ? -1 : 1;
        cam_fine_y_ = shake_base_fine_y + shake;               // camera fine Y read by sub_454B3
        // var_18 alternates +2 / -2 (particle y jitter)
        shake_amp = (shake_amp == 2) ? -2 : 2;

        // Spawn one slot every 3rd frame (first 100 frames only)
        if ((frame % 3) == 0 && frame < 0x64) {
            MoonP& p = part[spawn_slot];
            if (p.life == 0) {
                p.life = 0x12;
                p.x = combat_rand(0x12C) + 0x0A;               // rand()%0x12C + 0x0A
                p.y = combat_rand(0x8C) + 0x32;                // rand()%0x8C + 0x32
                sfx(0x33);                                     // sub_4F4CD(33h)
            }
            spawn_slot = (spawn_slot + 1) % 9;                 // `cmp di,9; jle` -> 0..8
        }

        // Background including the collapsed units + moon frame 1
        if (1 < nframes) battle_fx_blit(1, ax + magic_frames_[1].xo, ay + magic_frames_[1].yo);

        for (int i = 0; i < kSlots; ++i) {
            MoonP& p = part[i];
            const int jitter_y = p.y + shake_amp;
            if (p.life > 0x0F) {                               // frame 2 x 3
                for (int off = 1; off < 0x0A; off += 3) {
                    if (2 < nframes)
                        battle_fx_blit(2, p.x - 5 + off + magic_frames_[2].xo,
                                       jitter_y + 2 + magic_frames_[2].yo);
                }
                --p.life;
            } else if (p.life > 0x0A) {                        // frame 3 x 3
                for (int off = 1; off < 0x0A; off += 3) {
                    if (3 < nframes)
                        battle_fx_blit(3, p.x - 5 + off + magic_frames_[3].xo,
                                       jitter_y + 2 + magic_frames_[3].yo);
                }
                --p.life;
            } else if (p.life > 0) {                           // frame 4 x 1
                if (4 < nframes)
                    battle_fx_blit(4, p.x - 5 + p.life / 2 + magic_frames_[4].xo,
                                   jitter_y + 2 + magic_frames_[4].yo);
                --p.life;
            } else {
                ++done;
            }
        }

        battle_fx_present();
        wait_game_tick();
    }
    cam_fine_y_ = shake_base_fine_y;                           // dword_BA838 = 0

    // (6) Finish: darken -> one frame -> brighten -> LUT and pose restore
    for (int pct = 0x64; pct >= 0 && !should_exit_; pct -= 2) {
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }
    battle_fx_present();
    for (int pct = 0; pct <= 0x64 && !should_exit_; pct += 2) {
        pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);
        wait_game_tick();
    }
    pal_scale_range(0, 0x100, &battle_palette_[0][0], 100);    // 61115 counterpart (original palette)
    for (int oi : list) {
        if ((size_t)oi < units_.size()) reset_unit_pose(units_[(size_t)oi]);   // sub_12124
    }
    battle_nsp_release();                                      // sub_1FFCD
    return 1;
}

// sub_5A267: sunl.nsp (Sun Light).
//   Port signature: (unit_index, tx, ty, k); only unit_index is used (the caster).
//   (1) 611AA (9-byte table dword_560F0, shade 0x3C) builds the tint LUT, 1FEA4("sunl.nsp"), 103A8.
//   (2) 1033B(5) -> 11EE4 (release occupancy) -> anchor via 10971+0Ch / 10988, var_C = 0x19.
//   (3) Palette delta: subtract the master from a copy of unk_12824F (= master+3) with 5FC21;
//       each frame 5FD1C(mul) brightens toward the next colour (port: pal_shift_fade).
//   (4) sfx(0x32) -> while var_C > 5, two frames at a time: frame 0 at the anchor via 5F611
//       (scaled), then 5E9E8+499E4; var_C--.
//   (5) While var_C < 8, same 5F611 draw; var_C++ (grows again).
//   (6) sfx(0x33) -> frame [var_8 = 1] at the anchor via 5F29D (tinted), 10 frames x 6 rounds;
//       each frame 5FD1C(mul += 2, capped 0x64) brightens the palette.
//   (7) 11E04 (rewrite occupancy) -> 5FD1C(0) -> 61115 -> 103A0 -> 1FFCD.
int CombatII::battle_fx_5a267(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("sunl.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    auto& caster = units_[(size_t)unit_index];
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);       // 1124A, called by 2A75A
    int ax = 0, ay = 0;
    sunmoon_anchor(caster, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_, ax, ay);

    game_timer_set(5);                                         // sub_1033B(5)
    sfx(0x32);                                                 // sub_4F4CD(32h)

    int counter = 0x19;                                        // var_C = 0x19
    const int step = 2;                                        // two frames per step
    // (4)/(5) 5F611 target width and height are both the counter (0x19..6, then 5..7), and the
    //   top-left is anchor + counter/2 + frame offset (the dword reads extract entry+4/+6 as
    //   xo/yo). 5F611 is the shrink blitter, so it is selected even when the target is larger.
    auto draw_scaled = [&](int sz) {
        if (nframes <= 0) return;
        const MagicFxFrame& e = magic_frames_[0];
        battle_fx_blit_scaled(0, ax + sz / 2 + e.xo, ay + sz / 2 + e.yo,
                              sz, sz, false, true);            // sub_5F611
    };
    while (counter > 5 && !should_exit_) {
        for (int i = 0; i < step && !should_exit_; ++i) {
            draw_scaled(counter);
            battle_fx_present();
            wait_game_tick();
        }
        --counter;
    }
    while (counter < 8 && !should_exit_) {
        for (int i = 0; i < step && !should_exit_; ++i) {
            draw_scaled(counter);
            battle_fx_present();
            wait_game_tick();
        }
        ++counter;
    }

    // (6) 60 frames (10 x 6): draw frame 1 tinted and brighten the palette
    sfx(0x33);                                                 // sub_4F4CD(33h)
    int mul = 0;                                               // si: 0 -> 0x64 in steps of 2
    const int frame_no = 1;                                    // var_8 = 1
    for (int outer = 0; outer < 6 && !should_exit_; ++outer) {
        for (int i = 0; i < 10 && !should_exit_; ++i) {
            if (frame_no < nframes)
                battle_fx_blit_tinted_unclipped(frame_no, ax + magic_frames_[frame_no].xo,
                                                ay + magic_frames_[frame_no].yo, 0x3C);
            battle_fx_present();
            wait_game_tick();
            mul = std::min(0x64, mul + 2);
            pal_shift_fade(mul);                               // 5FD1C
        }
    }

    pal_shift_fade(0);                                         // 5FD1C(0) - original palette
    battle_nsp_release();                                      // sub_1FFCD
    return 1;
}

// sub_5A566: sunl2.nsp (Sun Light variant).
//   Port signature: (unit_index, tx, ty, k); only unit_index is used (the caster).
//   (1) 611AA (9-byte table dword_560F9, shade 0x3C) -> 1FEA4("sunl2.nsp") -> 103A8 -> 1033B(5)
//       -> 11EE4 -> anchor (10971+0Ch / 10988) -> delta palette (5FC21) -> sfx(0x34).
//   (2) 5 rounds (var_10 = 1..5), each 8 frames (var_1C = 6..13): draws var_10 copies of
//       frame var_1C at size 8*(count-1)+step-5, centered, with 454D5 + 5E9E8 per copy.
//   (3) sfx(0x32) -> 5 rounds (var_8 = 1..5) x 10 frames: 5F29D frame var_8 at the anchor,
//       then frame 6 via 5F570 at size 41..90, with 5FD1C(mul += 2), mul capped at 0x64.
//   (4) 11E04 -> sfx(0x33) -> 5FD1C(0) -> 61115 -> 103A0 -> 1FFCD.
int CombatII::battle_fx_5a566(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("sunl2.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    auto& caster = units_[(size_t)unit_index];
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);
    int ax = 0, ay = 0;
    sunmoon_anchor(caster, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_, ax, ay);

    game_timer_set(5);                                         // sub_1033B(5)

    // (2) count 1..5, step 6..13. The original draws `count` copies of frame `step` at
    // size 8*(count-1)+step-5, centered on the frame offset (probe: 1..40 px).
    for (int count = 1; count <= 5 && !should_exit_; ++count) {
        for (int step = 6; step <= 0x0D && !should_exit_; ++step) {
            const int fi = std::min(nframes - 1, step);
            const int sz = 8 * (count - 1) + (step - 5);
            const MagicFxFrame& e = magic_frames_[(size_t)fi];
            for (int n = 0; n < count && !should_exit_; ++n) {
                battle_fx_blit_scaled(fi, ax + e.xo - sz / 2, ay + e.yo - sz / 2,
                                      sz, sz, true);
                battle_fx_overlay();                           // sub_454D5
                battle_fx_present();
                wait_game_tick();
            }
        }
    }

    // (3) 5 rounds: 5F29D frame 1..5 at the anchor, then frame 6 via 5F570 with the size
    // 41..90 growing by one each frame (probe-confirmed).
    sfx(0x32);                                                 // sub_4F4CD(32h)
    int fade = 0;
    for (int round = 1; round <= 5 && !should_exit_; ++round) {
        for (int i = 0; i < 10 && !should_exit_; ++i) {
            if (round < nframes) {
                const MagicFxFrame& e = magic_frames_[(size_t)round];
                battle_fx_blit_tinted_unclipped(round, ax + e.xo, ay + e.yo, 60); // 5F29D (611AA 3Ch)
            }
            const int sz = 41 + (round - 1) * 10 + i;
            if (6 < nframes) {
                const MagicFxFrame& e = magic_frames_[6];
                battle_fx_blit_scaled(6, ax + e.xo - sz / 2, ay + e.yo - sz / 2,
                                      sz, sz, true);          // 5F570
            }
            battle_fx_present();
            wait_game_tick();
            pal_shift_fade(fade);                              // 5FD1C
            fade = std::min(100, fade + 2);
        }
    }

    sfx(0x33);                                                 // sub_4F4CD(33h)
    pal_shift_fade(0);                                         // 5FD1C(0)
    battle_nsp_release();                                      // sub_1FFCD
    return 1;
}
