// Mega Thunder (thun7.nsp / thun72.nsp / thun73.nsp)
// Original: dasm/COMBATII_code.ASM
//   sub_58C87  98475..98648  (sub_2A75A +0x3B9, Mega Thunder k=0)
//   sub_58E95  98649..98992  (sub_2A75A +0x3E3, Mega Thunder k=1)
//   sub_592B0  98993..99308  (sub_2A75A +0x40D, Mega Thunder k=2; sub_4426D +0x437)
//
// sub_2A75A calls all three with eax = screen x, edx = screen y, ebx = caster army
// ([0Eh]). Shared skeleton:
//   sub_1FEA4("<n>.nsp") -> sub_103A8 (stop weather tick) -> sub_1124A (centre the
//   camera on the target tile) -> scan the 12x12 visible tiles for hostile units
//   (sub_585B1 -> sub_11DB2 + sub_1201D) -> sub_1033B(5) -> set the collected units
//   to pose 10 (sub_1214F) -> sub_30824 (dim palette) -> sub_44D6E(1) (map dim) ->
//   sub_44D89 (highlight list) -> draw 0x64 frames (sub_58C87/sub_58E95) or 0xC3
//   frames (sub_592B0) -> sub_44D75 (clear highlights) -> restore poses (sub_12124)
//   -> sub_5F9AE (restore master palette) -> sub_44D6E(0) -> sub_103A0 (resume
//   weather tick) -> sub_1FFCD (release the table).
// Per-frame shake toggles `word ptr dword_BA838` (camera fine x) between 1 and 0xFFFF
// in sub_58C87/sub_58E95; the port maps it to cam_fine_x_ = +-1 (+-5 in sub_592B0).
//
// Port mapping: sub_1FEA4 -> battle_nsp_load, sub_1FFCD -> battle_nsp_release,
//   sub_1FFFE/sub_5EE51/sub_5EFB9 -> battle_fx_blit (the frame +4/+6 offsets are added
//   at the call site; the renderer drops off-screen pixels like sub_5EFB9),
//   sub_454B3+sub_5E9E8 -> battle_fx_present, sub_499E4 -> wait_game_tick,
//   sub_1033B -> game_timer_set, sub_499BD/rand_ -> combat_rand, sub_4F4CD -> sfx,
//   sub_103A8/sub_103A0 -> weather_tick_enabled_, sub_44D6E -> battle_dim_row0_,
//   sub_44D89 -> battle_highlight_add, sub_44D75 -> battle_highlight_clear,
//   sub_30824 -> fx_pal_mix(0x60, 0x100, 0, 0x28),
//   sub_5FA2C -> pal_scale_range(0, 0x100, master, pct),
//   sub_5F9AE -> pal_load_range(0, 0x100, master), sub_1214F -> anim_frame,
//   sub_12124 -> reset_unit_pose, sub_11DB2 -> battle_unit_index_at,
//   sub_1201D -> battle_armies_hostile, sub_1124A -> camera_center_on,
//   sub_10971/sub_10988 -> tile origin from TILE_ANCHOR_X/Y.
//
// Deviations (the IDA listing overlaps dword/word locals, so a few original values are
// ambiguous):
//   - sub_58E95 beam x base `[esp+56h] >> 16` aliases var_14/var_1C; read here as the
//     caster screen x plus rand(0xA0), scattered to the right.
//   - sub_58E95 beam C y base `[var_14+2] >> 16` aliases var_10 (caster screen y); read
//     here as the caster y, with beam C falling downwards.
//   - sub_58E95 beam A y uses `edi` (= sub_10988 - 0x64) at draw time and adds 0x18 per
//     frame.
//   - sub_592B0 leaves the sub_44D6E/sub_30824/sub_5FA2C arguments in reused registers
//     (eax/edx/ecx); the port uses the sub_5FA2C convention (0, 0x100, pct).
//   - sub_58E95 draws beams B/C from uninitialised locals until the first phase (1/8/15)
//     arms them (the original has no separate init either). The port starts them at the
//     reference point so nothing jumps off screen; `mov [var_20], ebx` (frame pointer
//     stored over the y local) is not reproduced, and beam B keeps the y set per phase.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

// sub_58BBD frame pick: remaining life (before decrement) -> table frame 0/1/2.
//   0Ah, 9, 2, 1 -> 0 ; 8, 7, 4, 3 -> 1 ; 6, 5 -> 2  (comparison chain 98386..98465)
inline int thun7_particle_frame(int life) {
    switch (life) {
        case 8: case 7: case 4: case 3: return 1;
        case 6: case 5:                 return 2;
        default:                        return 0;
    }
}

} // namespace

// sub_58C87: thun7.nsp storm. Ten slots over 0x64 frames: each frame revives one empty
// slot at (base_x-0xA0 + rand%0x140, base_y-0x32 + rand%0x96) with life 9 and draws
// frame 0/1/2 by remaining life. Plays sub_4F4CD(0x32/0x33) once per frame.
int CombatII::battle_fx_58c87(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("thun7.nsp") || magic_frames_.empty()) return 0;   // sub_1FEA4
    weather_tick_enabled_ = false;                                          // sub_103A8
    camera_center_on(tx, ty);                                               // sub_1124A
    const int caster_army = units_[(size_t)unit_index].army;                // caller's ebx

    // 12x12 tile scan: collect hostile units (sub_585B1 -> sub_11DB2 + sub_1201D).
    //   The original accumulates into word_F13AA (count) and dword_F0BA8[] (list),
    //   without duplicates.
    std::vector<int> hit;
    for (int cy = cam_tile_y_; cy < cam_tile_y_ + 0x0C; ++cy) {
        for (int cx = cam_tile_x_; cx < cam_tile_x_ + 0x0C; ++cx) {
            const int oi = battle_unit_index_at(cx, cy);                    // sub_11DB2
            if (oi < 0 || (size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!battle_armies_hostile(caster_army, battle_effective_army(o),
                                       battle_hostility_inverted_)) continue; // sub_1201D
            if (std::find(hit.begin(), hit.end(), oi) == hit.end()) hit.push_back(oi);
        }
    }
    game_timer_set(5);                                                      // sub_1033B(5)
    for (int oi : hit) {                                                    // sub_1214F
        auto& u = units_[(size_t)oi];
        u.anim_frame = (u.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00;
    }
    fx_pal_mix(0x60, 0x100, 0, 0x28);                                       // sub_30824(0, 28h)
    battle_dim_row0_ = true;                                                // sub_44D6E(1)
    for (int oi : hit) battle_highlight_add(oi);                            // sub_44D89

    // Origin = sub_10971(x), sub_10988(y); the original computes it once before 454B3.
    const int base_x = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int base_y = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    const int nframes = (int)magic_frames_.size();
    uint8_t life[10] = {};
    int16_t px[10] = {}, py[10] = {};

    // sub_58BBD: play sub_499BD(2) == 0 ? 0x32 : 0x33 and draw the live slots by life.
    auto draw_particles = [&]() {
        sfx(combat_rand(2) == 0 ? 0x32 : 0x33);                             // sub_4F4CD
        for (int i = 0; i < 10; ++i) {
            const int l = life[i];
            if (l <= 0) continue;
            life[i] = (uint8_t)(l - 1);                                     // dec bh, after drawing
            const int f = thun7_particle_frame(l);
            if (f >= nframes) continue;
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, px[i] + fr.xo, py[i] + fr.yo);                // sub_5EFB9
        }
    };

    for (int frame = 0; frame < 0x64 && !should_exit_; ++frame) {           // cmp cx,64h
        for (int i = 0; i < 10; ++i) {                                      // revive one empty slot
            if (life[i] != 0) continue;
            life[i] = 9;                                                    // mov [..],9
            px[i] = (int16_t)(base_x - 0xA0 + combat_rand(0x140));
            py[i] = (int16_t)(base_y - 0x32 + combat_rand(0x96));
            break;                                                          // jmp loc_58DFE
        }
        draw_particles();
        cam_fine_x_ = (cam_fine_x_ == 1) ? -1 : 1;                          // toggle dword_BA838
        battle_fx_present();                                                // 454B3 + 5E9E8
        wait_game_tick();                                                   // sub_499E4
    }

    battle_highlight_clear();                                               // sub_44D75
    cam_fine_x_ = 0;                                                        // mov word [BA838],0
    for (int oi : hit) {
        if ((size_t)oi < units_.size()) reset_unit_pose(units_[(size_t)oi]); // sub_12124
    }
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                       // sub_5F9AE
    battle_dim_row0_ = false;                                               // sub_44D6E(0)
    weather_tick_enabled_ = true;                                           // sub_103A0
    battle_nsp_release();                                                   // sub_1FFCD
    return 1;
}

// sub_58E95: thun72.nsp lightning plus a triple beam, over 0x64 frames.
//   - One of seven slots (life 9) is revived per frame at
//     (base_x-0xA0 + rand%0x140, base_y-0x32 + rand%0x96) and drawn via sub_58BBD.
//   - When ecx % 0x14 is 1, 8 or 0xF the beams are armed (var_4 = 1) and three sheets
//     are drawn every frame:
//       A) frame (t%3)+3 : x = var_30 (caster x + rand%0xA0, -0x0C per frame),
//          y = sub_10988-0x64 (+0x18 per frame)
//       B) frame (t%3)+6 : x = sub_10971-0x32 + rand%0xC8, y = sub_10988+0x64
//       C) frame (t%3)+9 : x = sub_10971-0xA0 (+0x20 per frame),
//          y = sub_10988-0x64 + rand%0x96 (+8 per frame)
//     A frame that draws beams also plays sub_4F4CD(0x34) when sub_499BD(3) == 0.
int CombatII::battle_fx_58e95(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("thun72.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    weather_tick_enabled_ = false;                                          // sub_103A8
    camera_center_on(tx, ty);                                               // sub_1124A
    const int caster_army = units_[(size_t)unit_index].army;

    std::vector<int> hit;
    for (int cy = cam_tile_y_; cy < cam_tile_y_ + 0x0C; ++cy) {
        for (int cx = cam_tile_x_; cx < cam_tile_x_ + 0x0C; ++cx) {
            const int oi = battle_unit_index_at(cx, cy);                    // sub_11DB2
            if (oi < 0 || (size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!battle_armies_hostile(caster_army, battle_effective_army(o),
                                       battle_hostility_inverted_)) continue; // sub_1201D
            if (std::find(hit.begin(), hit.end(), oi) == hit.end()) hit.push_back(oi);
        }
    }
    game_timer_set(5);                                                      // sub_1033B(5)
    for (int oi : hit) {
        auto& u = units_[(size_t)oi];
        u.anim_frame = (u.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00;
    }
    fx_pal_mix(0x60, 0x100, 0, 0x28);                                       // sub_30824
    battle_dim_row0_ = true;                                                // sub_44D6E(1)
    for (int oi : hit) battle_highlight_add(oi);                            // sub_44D89

    const int base_x = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X; // 10971
    const int base_y = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y; // 10988
    const int nframes = (int)magic_frames_.size();
    uint8_t life[10] = {};
    int16_t px[10] = {}, py[10] = {};
    // Beam state (var_14 / var_C / var_8 / var_4 / var_30 / var_28 / var_20 / var_2C / var_24).
    int beam_left_x = base_x - 0xA0;            // var_14 (beam C x base)
    int beam_bot_y = base_y + 0x64;             // var_C  (beam B y)
    int beam_top_y = base_y - 0x64;             // var_8  (beam A y base)
    bool beams_on = false;                      // var_4
    int beam_a_x = base_x;                      // var_30
    int beam_b_x = base_x, beam_b_y = beam_bot_y;
    int beam_c_x = base_x, beam_c_y = base_y;
    int beam_a_y_drift = beam_top_y;            // edi (+0x18 per frame)

    auto draw_particles = [&]() {
        sfx(combat_rand(2) == 0 ? 0x32 : 0x33);
        for (int i = 0; i < 10; ++i) {
            const int l = life[i];
            if (l <= 0) continue;
            life[i] = (uint8_t)(l - 1);
            const int f = thun7_particle_frame(l);
            if (f >= nframes) continue;
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, px[i] + fr.xo, py[i] + fr.yo);                // sub_5EFB9
        }
    };

    for (int frame = 0; frame < 0x64 && !should_exit_; ++frame) {
        const int t = frame % 0x14;                                         // idiv 0x14
        if (t == 1 || t == 8 || t == 0x0F) {                                // loc_59004
            if (t == 1) {
                beam_a_x = base_x + combat_rand(0xA0);                      // var_30
                beam_a_y_drift = beam_top_y;                                // edi = var_8
            }
            if (t == 8) {
                beam_b_y = beam_bot_y;                                      // var_20 = var_C
                beam_b_x = base_x - 0x32 + combat_rand(0xC8);               // var_28
            }
            if (t == 0x0F) {
                beam_c_y = base_y - 0x64 + combat_rand(0x96);               // var_2C
                beam_c_x = beam_left_x;                                     // var_24 = var_14
            }
            beams_on = true;                                                // var_4 = 1
        }
        for (int i = 0; i < 7; ++i) {                                       // cmp ax,7
            if (life[i] != 0) continue;
            life[i] = 9;
            px[i] = (int16_t)(base_x - 0xA0 + combat_rand(0x140));
            py[i] = (int16_t)(base_y - 0x32 + combat_rand(0x96));
            break;
        }
        draw_particles();
        if (beams_on) {
            const int t3 = frame % 3;                                       // idiv 3 remainder
            const int fa = t3 + 3, fb = t3 + 6, fc = t3 + 9;                // lea (t%3)+3 / +6 / +9
            if (fa < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fa];
                battle_fx_blit(fa, beam_a_x + fr.xo, beam_a_y_drift + fr.yo);
            }
            beam_a_x -= 0x0C;                                               // sub var_30,0Ch
            if (fb < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fb];
                battle_fx_blit(fb, beam_b_x + fr.xo, beam_b_y + fr.yo);
            }
            if (fc < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fc];
                battle_fx_blit(fc, beam_c_x + fr.xo, beam_c_y + fr.yo);
            }
            beam_a_y_drift += 0x18;                                         // add edi,18h
            beam_c_x += 0x20;                                               // var_24 += 20h
            beam_c_y += 8;                                                  // var_2C += 8
            if (combat_rand(3) == 0) sfx(0x34);                             // 499BD(3)
        }
        cam_fine_x_ = (cam_fine_x_ == 1) ? -1 : 1;                          // loc_59228
        battle_fx_present();
        wait_game_tick();
    }

    battle_highlight_clear();                                               // sub_44D75
    cam_fine_x_ = 0;
    for (int oi : hit) {
        if ((size_t)oi < units_.size()) reset_unit_pose(units_[(size_t)oi]);
    }
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                       // sub_5F9AE
    battle_dim_row0_ = false;
    weather_tick_enabled_ = true;                                           // sub_103A0
    battle_nsp_release();
    return 1;
}

// sub_592B0: thun73.nsp pillar lightning over 0xC3 frames.
//   - When frame % 0x33 == 0x14 and frame < 0xB4 a new pillar is placed at
//     y = sub_10988 - rand%0x1E, x = sub_10971 - 0x1E + rand%0x46, life (var_4) = 0x0C.
//   - When frame % 0x0A == 0 and frame < 0xAA up to sub_499BD(5)+1 empty slots (0..4)
//     are filled with life 9 at (base_x-0xA0 + rand%0x140, base_y-0x32 + rand%0x96).
//   - Pillar frames 3/4/5 are drawn by life at (x+xo, y+yo); camera fine x toggles
//     +-5 by life%2; life 9 -> sub_4F4CD(0x34) plus 100% brightness, life > 2 ->
//     (life*5+0x3C)%, life == 2 -> 100% plus a dim-table rebuild.
int CombatII::battle_fx_592b0(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("thun73.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    weather_tick_enabled_ = false;                                          // sub_103A8
    camera_center_on(tx, ty);                                               // sub_1124A
    const int caster_army = units_[(size_t)unit_index].army;

    std::vector<int> hit;
    for (int cy = cam_tile_y_; cy < cam_tile_y_ + 0x0C; ++cy) {
        for (int cx = cam_tile_x_; cx < cam_tile_x_ + 0x0C; ++cx) {
            const int oi = battle_unit_index_at(cx, cy);                    // sub_11DB2
            if (oi < 0 || (size_t)oi >= units_.size()) continue;
            const auto& o = units_[(size_t)oi];
            if (!battle_armies_hostile(caster_army, battle_effective_army(o),
                                       battle_hostility_inverted_)) continue; // sub_1201D
            if (std::find(hit.begin(), hit.end(), oi) == hit.end()) hit.push_back(oi);
        }
    }
    game_timer_set(5);                                                      // sub_1033B(5)
    for (int oi : hit) {
        auto& u = units_[(size_t)oi];
        u.anim_frame = (u.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00;
    }
    fx_pal_mix(0x60, 0x100, 0, 0x28);                                       // sub_30824
    battle_dim_row0_ = true;                                                // sub_44D6E(1)
    for (int oi : hit) battle_highlight_add(oi);                            // sub_44D89

    const int base_x = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X; // 10971
    const int base_y = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y; // 10988
    const int nframes = (int)magic_frames_.size();
    uint8_t life[10] = {};
    int16_t px[10] = {}, py[10] = {};
    int beam_life = 0;                                  // var_4
    int beam_x = 0, beam_y = 0;                         // var_20 / var_14

    auto draw_particles = [&]() {
        sfx(combat_rand(2) == 0 ? 0x32 : 0x33);
        for (int i = 0; i < 10; ++i) {
            const int l = life[i];
            if (l <= 0) continue;
            life[i] = (uint8_t)(l - 1);
            const int f = thun7_particle_frame(l);
            if (f >= nframes) continue;
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, px[i] + fr.xo, py[i] + fr.yo);                // sub_5EFB9
        }
    };

    for (int frame = 0; frame < 0xC3 && !should_exit_; ++frame) {           // cmp dx,0C3h
        if (frame % 0x33 == 0x14 && frame < 0xB4) {                         // loc_593C0: new pillar
            beam_y = base_y - combat_rand(0x1E);
            beam_x = base_x - 0x1E + combat_rand(0x46);
            beam_life = 0x0C;
        }
        if (frame % 0x0A == 0 && frame < 0xAA) {                            // loc_59434: debris
            const int want = combat_rand(5) + 1;                            // 499BD(5)+1
            int made = 0;
            for (int i = 0; i < 5 && made < want; ++i) {
                if (life[i] != 0) continue;
                life[i] = 9;
                px[i] = (int16_t)(base_x - 0xA0 + combat_rand(0x140));
                py[i] = (int16_t)(base_y - 0x32 + combat_rand(0x96));
                ++made;
            }
        }
        draw_particles();
        if (beam_life > 0) {
            // Frame by life: > 8 -> 3 (unk_F0DCE = base + 0x24), > 4 -> 4, else -> 5.
            // Records 3/4/5 are 0x0C bytes apart from unk_F0DAA (loc_59510/loc_5951C).
            const int f = (beam_life > 8) ? 3 : (beam_life > 4 ? 4 : 5);
            if (f < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)f];
                battle_fx_blit(f, beam_x + fr.xo, beam_y + fr.yo);         // sub_5EFB9
            }
            cam_fine_x_ = (beam_life % 2 == 0) ? 5 : -5;                    // dword_BA838 = 5/-5
            if (beam_life == 9) {                                           // loc_59569
                sfx(0x34);                                                  // sub_4F4CD(34h)
                battle_dim_row0_ = true;                                    // sub_44D6E(1)
                pal_scale_range(0, 0x100, &battle_palette_[0][0], 0x64);    // sub_5FA2C(100)
            } else if (beam_life > 2) {                                     // loc_5959B
                battle_dim_row0_ = true;                                    // sub_44D6E(1)
                const int pct = beam_life * 5 + 0x3C;                       // imul 5; add 3Ch
                pal_scale_range(0, 0x100, &battle_palette_[0][0], pct);     // sub_5FA2C
            } else if (beam_life == 2) {                                    // loc_595C5
                pal_scale_range(0, 0x100, &battle_palette_[0][0], 0x64);    // full palette
                battle_dim_row0_ = true;                                    // sub_44D6E(1)
                fx_pal_mix(0x60, 0x100, 0, 0x28);                           // sub_30824(0, 28h)
            }
            --beam_life;                                                    // dec var_4
        } else {
            cam_fine_x_ = 0;                                                // xor ecx,ecx
        }
        battle_fx_present();                                                // 5E9E8
        wait_game_tick();                                                   // 499E4
    }

    battle_highlight_clear();                                               // sub_44D75
    cam_fine_x_ = 0;
    for (int oi : hit) {
        if ((size_t)oi < units_.size()) reset_unit_pose(units_[(size_t)oi]);
    }
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                       // sub_5F9AE
    battle_dim_row0_ = false;
    weather_tick_enabled_ = true;                                           // sub_103A0
    battle_nsp_release();
    return 1;
}
