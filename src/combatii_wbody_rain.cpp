// sub_39A49: rain.nsp (V-weapon edi 11, the 11th of the 13 bodies at F1474)
// ASM: dasm/COMBATII_code.ASM 55981..56960. Called from sub_302E6 loc_304CB after
//   fire.ssp + effect2.ssp + f-wave.ssp finish (`mov eax,ebx; call sub_39A49`).
//
// Original flow:
//   1. 13F8A + 13D02: mark a band of +-3..6 cells on the caster axis in the E8 grid,
//      keyed by facing (1/3 = x column, 0/2 = y row). The 13CBD gate reads the marks.
//   2. Targets: F14D8==1 (auto) uses sub_398F3 (pick within the 12x12 screen, -1 when
//      none); player path is 4A483 cursor -> 13CBD gate -> 1AC08 (radius 2 diamond) ->
//      1AC59/1A1F8 into F0648.
//   3. 1FEA4 (rain.nsp) -> 103A8 (weather freeze) -> 32 slots x 2 buffers
//      (_nmalloc_ 0x64) -> 1033B(8) -> 44D6E(1) -> 2CBA1(caster, 3) -> 200DE(caster, 0).
//   4. 4-frame intro: 30824(0, 0Ah) + 454B3 + 5E9E8 + 499E4.
//   5. 44D75 (clear list) -> 44D89 per target -> 11EE4 (release caster slot) ->
//      1033B(5) -> anchors from 10971 (x) / 10988 (y).
//   6. Facing branch (table below): seed 32 slots -> 4F4CD(32h) -> 1033B(0x23) ->
//      3-frame intro (fixed F0DAA entries) -> 1033B(5) -> 1214F pose 0Ah per target ->
//      4F4CD(3) -> curtain loop: 454B3 -> one curtain sprite ->
//      39620/38FF7/3930E/38CF8 (update and draw the slots) -> 5E9E8 -> 499E4 until
//      every slot finishes.
//   7. Per target `[1Eh]|=1`, `[1Fh]=0x5A` -> 454B3 -> 12F2A (caster) -> 1033B(0Fh) ->
//      5E9E8 -> 499E4 -> 44D75 -> 11E04 (caster) -> 12124 per target -> 103A0 ->
//      5F9AE(0, 100h, master) -> 44D6E(0) -> free buffers -> 1FFCD ->
//      2CAF7(unit, ebx=2, edx=1B58h) -> 6104A(0,1) -> return edx = 1.
//
// Per-facing constants (as in the listing):
//   facing 1 (west):  intro F0DAA[5..7],   curtain 8 (unk_F0E0A),  anchor y+5,   x-2,
//                     speed rand(4)+1, spread 0x167-rand(5Ah), frame rand(3)+1,
//                     length rand(1Eh)+8Ch
//   facing 3 (east):  intro F0DAA[9..11],  curtain 11,             anchor y+17h, x,
//                     speed rand(4)+1, spread 0B4h+rand(5Ah),  frame rand(2)+1,
//                     length 82h+rand(1Eh); only this branch redraws the caster via
//                     12F2A on every iteration (ASM 3A1E6)
//   facing 2 (north): intro F0DAA[11h..13h], curtain 16 (dword_F0E6E), anchor y+0Eh,
//                     x-0Dh, speed rand(3)+2, spread 15Eh-rand(5Ah), frame rand(2)+1,
//                     length 136h-rand(1Eh)
//   facing 0 (south): intro F0DAA[0Dh..0Fh], curtain 20 (unk_F0E9A), anchor y+0Fh,
//                     x+0Fh, speed rand(3)+1, spread 28h-rand(14h),  frame rand(2)+1,
//                     length rand(1Eh)+82h
//   Curtain entry = F0DAA + 12*k: 0x60=8, 0x84=11, 0xC0=16, 0xF0=20, matching the
//   absolute unk_F0E0A / edi (intro last frame = 11) / dword_F0E6E / unk_F0E9A.
//
// Port mapping: 1FEA4 -> battle_nsp_load, 1FFCD -> battle_nsp_release,
//   103A8 -> weather_tick_enabled_ = false, 1033B -> game_timer_set,
//   44D6E -> battle_dim_row0_, 44D89 -> battle_highlight_add,
//   44D75 -> battle_highlight_clear, 2CBA1 -> battle_highlight_add_depth,
//   200DE(unit,0) -> camera_center_on, 30824 -> fx_pal_mix(0x60,0x100,0,..),
//   454B3 -> battle_fx_world(+flush)+overlay, 5E9E8 -> battle_fx_commit,
//   499E4 -> wait_game_tick, 4F4CD -> sfx, 499BD -> combat_rand,
//   5EFB9 -> fx_blit_nsp (the caller adds the entry xo/yo because the original already
//   does), 10971/10988 -> unit anchors in screen space, 1214F -> anim_frame (0Ah, or 0
//   for sprites >= 12Ch), 12F2A -> battle_draw_unit_now, 12124 -> reset_unit_pose,
//   5F9AE -> pal_load_range, 1D770/2CACA/2CAF7 -> battle_weapon_2caf7_loop(2, 1B58h).
//   6104A/6106C/103A0/11EE4/11E04 are palette-LUT and slot-occupancy details, omitted as
//   in the other bodies.
//
// Approximations:
//   - The 13D02 grid marks and the 398F3 auto pick come from the port collectors
//     (battle_weapon_collect_screen / collect_dia_at), which build the same grid and
//     gate; only the coordinates come from battle_pick_*. Gameplay matches the previous
//     battle_weapon_39a49.
//   - The curtain engine (39620/38FF7/3930E/38CF8, ~300 lines of float particles each)
//     is not ported. A single slot model keeps the shared seed/lifetime/coordinate
//     contract: 32 slots run mode 2 (wait) -> 1 (fall) -> 0 (done); while falling one
//     curtain entry is drawn at (x, y) plus a stem tip at (x, y - length). A slot that
//     reaches the ground line ends in mode 0 and emits 4F4CD(33h); the loop exits when
//     every slot is done. The original's E8 collision (4F4CD(34h) plus slot reset on a
//     blocked cell) is approximated by the same ground condition.
//   - Loop cap: the original stops only when all slots finish; the port also stops after
//     0x200 frames, above the longest seed life (0x167).
//   - The 0x140 boundary clamp in 5EFB9 is replaced by the blitter dropping off-screen
//     pixels.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// One struct replaces the per-slot stack arrays the original keeps for 32 slots
// (var_B0 mode, var_81C/var_8E4 floats, var_114 speed, var_178 spread/life, var_308 frame,
// var_2A4 length, var_36C stem, var_7C age). The two _nmalloc_(0x64) buffers
// (var_68C/var_754) are replaced by the slot state itself.
struct RainSlot {
    float x = 0.0f, y = 0.0f;   // screen position (var_8E4 / var_81C)
    int mode = 2;               // var_B0: 2 = wait, 1 = fall, 0 = done
    int speed = 1;              // var_114: y step per frame
    int spread = 0;             // var_178: fall distance to the ground line (anchor y + spread)
    int frame = 0;              // var_308: entry selector (1..3)
    int length = 0;             // var_2A4: stem length
    int age = 0;                // var_7C / var_B4
    int phase = 0;              // var_36C: spawn phase (+step per frame)
    int angle = 0;              // var_114: spawn target phase (rand(0Dh)+1)
    int dead = 0;               // var_7C/B4: 0Ah = particle playing, 65h = finished marker
    // 38B82/38780: per-slot history (var_68C/var_754, shifted every frame)
    int16_t hx[50] = {};
    int16_t hy[50] = {};
    int hist = 0;
    // 389EA measurements: segment count (var_114) plus two frame counters and the gate
    // phase (var_178); |var_178 - var_2A4| <= 5 switches to mode 1.
    int seg = 0;                // var_114: trail segments per frame (0..4)
    int cd = 0;                 // var_3D0: 0..3, seg++ every 4 frames
    int ce = 0;                 // var_434: 0..5, step (var_308)++ every 6 frames
    int gate_phase = 0;         // var_178: gate phase
};

// Per-slot ground line: the point where 38CF8 compares the current y against
// `arg_C (slot target y) +- 5`, expressed through the slot spread (anchor y + spread).
// Clamped to 0x30..0xC8 so slots never fall far off screen.
inline int rain_slot_ground(int anchor_y, int spread) {
    const int d = spread < 0x30 ? 0x30 : (spread > 0xC8 ? 0xC8 : spread);
    return anchor_y + d;
}

}  // namespace

int CombatII::battle_fx_39a49_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& A = units_[(size_t)unit_index];
    const int caster_id = A.id;

    // Step 2: target collection, keeping battle_weapon_39a49 gameplay.
    int n = 0;
    if (battle_f14d8_ == 1) {
        n = battle_weapon_collect_screen(unit_index, 0);                       // sub_398F3
    } else if (battle_pick_tx_ >= 0 && battle_pick_ty_ >= 0) {
        n = battle_weapon_collect_dia_at(unit_index, battle_pick_tx_, battle_pick_ty_, 2); // 1AC08 r2
    }
    if (n <= 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 39a49 unit=%d empty -> -1\n", caster_id);
        return -1;
    }

    // Step 3: load rain.nsp, freeze the weather, build the 32 slots.
    if (!battle_nsp_load("rain.nsp") || magic_frames_.empty()) {               // sub_1FEA4
        // Without the asset only the gameplay tail runs (status + 2CAF7).
        for (int ti : battle_target_list_) battle_weapon_apply_status(ti, 0x01, 0x5A);
        const int r = battle_weapon_2caf7_loop(unit_index, 2, 0x1B58);         // sub_2CAF7
        if (battle_trace_)
            fprintf(stderr, "COMBAT: warning: 39a49 unit=%d rain.nsp missing (damage only) -> %d\n", caster_id, r);
        return r < 0 ? -1 : 1;
    }
    const int nframes = (int)magic_frames_.size();
    weather_tick_enabled_ = false;                                             // sub_103A8

    constexpr int kSlots = 0x32;                                               // cmp cx,32h
    std::vector<RainSlot> slot((size_t)kSlots);

    game_timer_set(8);                                                         // sub_1033B(8)
    battle_dim_row0_ = true;                                                   // sub_44D6E(1)
    battle_highlight_add_depth(unit_index);                                    // sub_2CBA1(unit,3)
    camera_center_on((int)(int8_t)A.x, (int)(int8_t)A.y);                      // sub_200DE(unit,0)

    auto frame_flush = [&]() {                                                 // 454B3 + 5E9E8 + 499E4
        battle_fx_world();                                                     // sub_454B3(10AEB/12BEE/4512E)
        battle_fx_flush();                                                     // sub_12E01
        battle_fx_overlay();                                                   // 454B3 fall-through 454D5
        battle_fx_commit();                                                    // sub_5E9E8
        wait_game_tick();                                                      // sub_499E4
    };

    // Step 4: 4-frame intro - 30824(0, 0Ah) darkens, then the frame is presented.
    for (int i = 0; i < 4 && !should_exit_; ++i) {
        fx_pal_mix(0x60, 0x100, 0, 0x0A);                                      // sub_30824(0, 0Ah)
        frame_flush();
    }
    if (should_exit_) {                                                        // ESC: restore state only
        weather_tick_enabled_ = true;
        battle_dim_row0_ = false;
        battle_highlight_clear();
        battle_nsp_release();
        return 1;
    }

    // Step 5: clear the list, re-add only the targets (44D75 -> 44D89); the caster slot
    // release (11EE4) is omitted.
    battle_highlight_clear();                                                  // sub_44D75
    for (int ti : battle_target_list_) battle_highlight_add(ti);               // 44D89
    game_timer_set(5);                                                         // sub_1033B(5)
    const int anchor_x = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X; // sub_10971
    const int anchor_y = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y; // sub_10988

    // Step 6: facing branch. Intro frames, curtain entry and seed constants per the
    // table at the top of the file.
    const int facing = (int)A.facing & 3;
    int intro_first = 0;                                                       // F0DAA start index
    int curtain_frame = 0;                                                     // fixed entry
    int seed_y = 0, seed_x = 0;
    int spread_base = 0, spread_rand = 0;
    int speed_rand = 0, speed_add = 0;
    // Measured 39A49 initialisation formulas for seg (var_114), gate (var_178),
    // step (var_308) and target (var_2A4).
    int seg_rand = 0, seg_add = 0, step_rand = 0;
    int gate_base = 0, gate_rand = 0;  bool gate_sub = false;
    int tgt_base = 0,  tgt_rand = 0;   bool tgt_sub = false;
    bool redraw_caster = false;                                                // facing 3 only
    switch (facing) {
        case 1:                                                                // west (loc_39D50)
            intro_first = 5; curtain_frame = 8;
            seed_y = anchor_y + 5;      seed_x = anchor_x - 2;
            speed_rand = 4; speed_add = 1;
            spread_base = 0x167; spread_rand = 0x5A;
            seg_rand = 4; seg_add = 1; step_rand = 3;
            gate_base = 0x167; gate_rand = 0x5A; gate_sub = true;
            tgt_base = 0x8C;   tgt_rand = 0x1E;  tgt_sub = false;
            break;
        case 3:                                                                // east (loc_3A026)
            intro_first = 9; curtain_frame = 11;
            seed_y = anchor_y + 0x17;   seed_x = anchor_x;
            speed_rand = 4; speed_add = 1;
            spread_base = 0xB4; spread_rand = 0x5A;
            seg_rand = 4; seg_add = 1; step_rand = 3;
            gate_base = 0xB4;  gate_rand = 0x5A; gate_sub = false;
            tgt_base = 0x28;   tgt_rand = 0x14;  tgt_sub = true;
            redraw_caster = true;
            break;
        case 2:                                                                // north (loc_3A2C6)
            intro_first = 0x11; curtain_frame = 16;
            seed_y = anchor_y + 0x0E;   seed_x = anchor_x - 0x0D;
            speed_rand = 3; speed_add = 2;
            spread_base = 0x15E; spread_rand = 0x5A;
            seg_rand = 3; seg_add = 2; step_rand = 2;
            gate_base = 0x5A;  gate_rand = 0x5A; gate_sub = false;
            tgt_base = 0x136;  tgt_rand = 0x1E;  tgt_sub = true;
            break;
        default:                                                               // south (loc_3A59E)
            intro_first = 0x0D; curtain_frame = 20;
            seed_y = anchor_y + 0x0F;   seed_x = anchor_x + 0x0F;
            speed_rand = 3; speed_add = 2;
            spread_base = 0x15E; spread_rand = 0x5A;
            seg_rand = 3; seg_add = 2; step_rand = 2;
            gate_base = 0x15E; gate_rand = 0x5A; gate_sub = true;
            tgt_base = 0x82;   tgt_rand = 0x1E;  tgt_sub = false;
            break;
    }

    for (int i = 0; i < kSlots; ++i) {                                         // loc_39D50 / 3A026 / 3A2C6 / 3A59E
        RainSlot& s = slot[(size_t)i];
        s.mode = 2;
        s.x = (float)seed_x;
        s.y = (float)seed_y;
        s.speed = combat_rand(speed_rand) + speed_add;                         // approximation: mode 1 fall speed
        s.spread = spread_base - combat_rand(spread_rand);                     // approximation: ground line
        if (s.spread < 0) s.spread = 0;
        s.seg = combat_rand(seg_rand) + seg_add;                               // var_114 (1..4 / 2..4)
        s.gate_phase = gate_sub ? gate_base - combat_rand(gate_rand)
                                : gate_base + combat_rand(gate_rand);          // var_178
        s.frame = combat_rand(step_rand) + 1;                                  // var_308 (1..3 / 1..2)
        s.length = tgt_sub ? (tgt_base - combat_rand(tgt_rand))
                           : (tgt_base + combat_rand(tgt_rand));               // var_2A4
        s.age = 0;
        s.phase = 0;
        s.angle = combat_rand(0x0D) + 1;                                       // var_114
        s.dead = 0;
        s.hist = 0;
        s.cd = 0;
        s.ce = 0;
    }
    sfx(0x32);                                                                 // sub_4F4CD(32h)
    game_timer_set(0x23);                                                      // sub_1033B(23h)

    // 3-frame intro: entry (intro_first + i) at (anchor + 0Fh/0Ah).
    for (int i = 0; i < 3 && !should_exit_; ++i) {                             // loc_39E80 / 3A142 / 3A3F9 / 3A6C0
        const int f = intro_first + i;
        battle_fx_world();
        battle_fx_flush();
        battle_fx_overlay();
        if (f >= 0 && f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            fx_blit_nsp(f, anchor_x + 0x0F + fr.xo, anchor_y + 0x0A + fr.yo);   // sub_5EFB9
        }
        battle_fx_commit();
        wait_game_tick();
    }
    if (should_exit_) {
        weather_tick_enabled_ = true;
        battle_dim_row0_ = false;
        battle_highlight_clear();
        battle_nsp_release();
        return 1;
    }

    game_timer_set(5);                                                         // sub_1033B(5)
    for (int ti : battle_target_list_) {                                       // sub_1214F for all
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        units_[(size_t)ti].anim_frame =
            (units_[(size_t)ti].sprite_type >= 0x12C) ? 0 : 0x0A;
    }
    sfx(0x03);                                                                 // sub_4F4CD(3)

    // Curtain loop: update the 32 slots and draw one curtain frame.
    //   The original calls 39620/38FF7/3930E/38CF8; that particle framework is not ported,
    //   so a single slot model carries only the shared seed/life/coordinate contract
    //   (see the approximations at the top of the file).
    int frames_run = 0;
    for (int guard = 0; guard < 0x200 && !should_exit_; ++guard) {
        battle_fx_world();
        battle_fx_flush();
        battle_fx_overlay();
        if (curtain_frame >= 0 && curtain_frame < nframes) {
            const MagicFxFrame& cf = magic_frames_[(size_t)curtain_frame];
            fx_blit_nsp(curtain_frame, anchor_x + 0x0F + cf.xo, anchor_y + 0x0A + cf.yo); // 5EFB9 fixed entry
        }
        if (redraw_caster) battle_draw_unit_now(unit_index);                   // 3A1E6 / sub_12F2A

        // 38780 trail point: shift the history and plot one pixel per colour step.
        // 389EA runs this per segment; mode 1 runs it per frame.
        auto draw_trail = [&](RainSlot& s) {
            for (int k = 49; k > 0; --k) {
                s.hx[k] = s.hx[k - 1];
                s.hy[k] = s.hy[k - 1];
            }
            s.hx[0] = (int16_t)s.x;
            s.hy[0] = (int16_t)s.y;
            if (s.hist < 50) ++s.hist;
            uint8_t* bb = video_.get_backbuffer();
            if (!bb) return;
            auto put = [bb](int x, int y, uint8_t c) {
                if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
                    bb[(size_t)y * SCREEN_W + (size_t)x] = c;
            };
            const int hx0 = s.hx[0], hy0 = s.hy[0];
            if (s.hist == 1) {                                             // 38780 leading 3x3
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) put(hx0 + dx, hy0 + dy, 0x10);
            }
            int step = 0;
            for (int si = 0; si < s.hist; ++si) {
                if (si % 10 == 4 && step < 7) ++step;
                const uint8_t col = (uint8_t)(0x10 + step);
                const int px = s.hx[si], py = s.hy[si];
                if (si <= 0x10) {
                    put(px, py, col); put(px + 1, py, col); put(px - 1, py, col);
                    put(px, py - 1, col); put(px, py + 1, col);
                } else {
                    put(px, py, col);
                }
            }
        };

        bool any_alive = false;
        for (int i = 0; i < kSlots && !should_exit_; ++i) {
            RainSlot& s = slot[(size_t)i];
            // ---- mode 2: 389EA spawn wait (ASM 54413..54571) ----
            //   Every frame 389EA: move var_114 (segment count) times and draw the 38780
            //   trail; var_3D0++ (0..3, var_114++ up to 4 every 4 frames); var_434++
            //   (0..5, var_308++ up to 0Ah every 6 frames).
            //   Then the per-engine gate: facing 1 starts when var_178 < var_2A4 and
            //   otherwise -step; facings 3/0 start when |diff| <= 5 and otherwise +step
            //   with a 168h wrap; facing 2 starts when |diff| <= 5 and otherwise +step
            //   with no wrap. Starting emits 4F4CD(33h).
            if (s.mode == 2) {
                any_alive = true;
                for (int k = 0; k < s.seg && !should_exit_; ++k) {
                    const float a = (float)s.gate_phase * 0.01745329252f;      // flt_74A68/744C8 approximation
                    s.x += std::cos(a);
                    s.y += std::sin(a);
                    draw_trail(s);                                             // sub_38780
                }
                if (++s.cd > 3) { s.cd = 0; if (s.seg < 4) ++s.seg; }
                if (++s.ce > 5) { s.ce = 0; if (s.frame < 0x0A) ++s.frame; }
                // Gate per engine: 39620 (facing 1) starts when var_178 < var_2A4 and
                // otherwise subtracts var_308; 38FF7 (3) and 3930E (0) start at
                // |diff| <= 5 and otherwise add with a 168h wrap; 38CF8 (2) adds without
                // the wrap.
                bool start = false;
                if (facing == 1) {
                    start = (s.gate_phase < s.length);
                } else {
                    const int d = s.gate_phase - s.length;
                    start = (d >= -5 && d <= 5);
                }
                if (start) {
                    s.mode = 1;
                    s.age = 0;
                    s.hist = 0;
                    sfx(0x33);                                                 // 4F4CD(33h)
                } else if (facing == 1) {
                    s.gate_phase -= s.frame;                                   // 39620: subtract
                } else {
                    s.gate_phase += s.frame;                                   // var_178 += var_308
                    if ((facing == 0 || facing == 3) && s.gate_phase >= 0x168)
                        s.gate_phase -= 0x168;                                 // 38FF7/3930E wrap
                }
                continue;
            }
            // ---- mode 0: 38BC6 NSP 0Ah..12h (3-frame spacing), above 12h -> 65h marker ----
            if (s.mode == 0) {
                if (s.dead >= 0x0A && s.dead != 0x65) {
                    const int f = 0x0A + std::min((s.dead - 0x0A) / 3, 8);
                    if (f < nframes) {
                        const MagicFxFrame& pf = magic_frames_[(size_t)f];
                        fx_blit_nsp(f, (int)s.x + pf.xo, (int)s.y + pf.yo);   // sub_5EFB9
                    }
                    ++s.dead;
                    if (s.dead > 0x12) s.dead = 0x65;
                    any_alive = true;
                }
                continue;
            }
            if (s.mode != 1) continue;
            any_alive = true;
            s.age += 1;                                                        // var_7C++
            if (s.age > 0x1F4) { s.mode = 0; s.dead = 0x0A; continue; }         // defensive life cap
            s.y += (float)s.speed;                                             // var_81C += speed
            s.x += (float)((s.frame & 1) ? 1 : -1);                            // spread direction from frame parity
            // 38B82 + 38780: colour = 10h + clamp(+1 every si%10==4, 0..7);
            //   si <= 10h is a 5-pixel cross, si > 10h a single pixel (draw_trail).
            draw_trail(s);
            const float slot_ground = (float)rain_slot_ground(anchor_y, s.spread);
            if (s.y >= slot_ground) {                                          // target - current <= 1
                s.y = slot_ground;
                s.mode = 0;
                s.dead = 0x0A;                                                 // death flag
                sfx(0x34);                                                     // 4F4CD(34h)
                continue;
            }
            // Curtain stem: the fixed entry at (x, y) and (x, y - length).
            if (curtain_frame >= 0 && curtain_frame < nframes) {
                const MagicFxFrame& cf = magic_frames_[(size_t)curtain_frame];
                const int px = (int)s.x, py = (int)s.y;
                fx_blit_nsp(curtain_frame, px + cf.xo, py + cf.yo);            // sub_5EFB9
                if (s.length > 0) fx_blit_nsp(curtain_frame, px + cf.xo, py - s.length + cf.yo);
            }
        }
        battle_fx_commit();                                                    // sub_5E9E8
        wait_game_tick();                                                      // sub_499E4
        ++frames_run;
        if (!any_alive) break;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 39a49 unit=%d facing=%d targets=%zu curtain entry=%d frames=%d\n",
                caster_id, facing, battle_target_list_.size(), curtain_frame, frames_run);
    if (should_exit_) {
        weather_tick_enabled_ = true;
        battle_dim_row0_ = false;
        battle_highlight_clear();
        battle_nsp_release();
        return 1;
    }

    // Step 7: apply status, redraw the caster, clean up, then damage.
    for (int ti : battle_target_list_) {                                       // loc_3A85A
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        units_[(size_t)ti].combat_flags_1e = (uint8_t)(units_[(size_t)ti].combat_flags_1e | 0x01);
        units_[(size_t)ti].ail_1f = 0x5A;
    }
    battle_fx_world();                                                         // sub_454B3
    battle_fx_flush();
    battle_fx_overlay();
    battle_draw_unit_now(unit_index);                                          // sub_12F2A
    game_timer_set(0x0F);                                                      // sub_1033B(0Fh)
    battle_fx_commit();                                                        // sub_5E9E8
    wait_game_tick();                                                          // sub_499E4
    battle_highlight_clear();                                                  // sub_44D75
    for (int ti : battle_target_list_) {                                       // sub_12124 for all
        if (ti >= 0 && (size_t)ti < units_.size()) reset_unit_pose(units_[(size_t)ti]);
    }
    weather_tick_enabled_ = true;                                              // sub_103A0
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                          // sub_5F9AE (master)
    battle_dim_row0_ = false;                                                  // sub_44D6E(0)
    battle_nsp_release();                                                      // sub_1FFCD
    const int r = battle_weapon_2caf7_loop(unit_index, 2, 0x1B58);             // sub_2CAF7(2, 1B58h)
    if (battle_trace_) fprintf(stderr, "COMBAT: 39a49 unit=%d 2caf7(2,1B58h) -> %d\n", caster_id, r);
    return 1;                                                                  // loc_3A943: eax = edx = 1 (2CAF7 result ignored)
}
