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
//   3. 1FEA4 (rain.nsp) -> 103A8 (weather freeze) -> 50 slots (0x32) x 2 buffers
//      (_nmalloc_ 0x64) -> 1033B(8) -> 44D6E(1) -> 2CBA1(caster, 3) -> 200DE(caster, 0).
//   4. 4-frame intro: 30824(0, 0Ah) + 454B3 + 5E9E8 + 499E4.
//   5. 44D75 (clear list) -> 44D89 per target -> 11EE4 (release caster slot) ->
//      1033B(5) -> anchors from 10971 (x) / 10988 (y).
//   6. Facing branch (table below): seed 50 slots -> 4F4CD(32h) -> 1033B(0x23) ->
//      3-frame intro (fixed F0DAA entries) -> 1033B(5) -> 1214F pose 0Ah per target ->
//      4F4CD(3) -> curtain loop: 454B3 -> one curtain sprite ->
//      39620/38FF7/3930E/38CF8 (update and draw the slots) -> 5E9E8 -> 499E4 until
//      every slot finishes.
//   7. Per target `[1Eh]|=1`, `[1Fh]=0x5A` -> 454B3 -> 12F2A (caster) -> 1033B(0Fh) ->
//      5E9E8 -> 499E4 -> 44D75 -> 11E04 (caster) -> 12124 per target -> 103A0 ->
//      5F9AE(0, 100h, master) -> 44D6E(0) -> free buffers -> 1FFCD ->
//      2CAF7(unit, ebx=2, edx=1B58h) -> 6104A(0,1) -> return edx = 1.
//
// Per-facing constants (listing, raw effective addresses):
//   facing 1 (west):  intro F0DAA[5..7],   curtain 8 (F0E0A),   seed x+5,    y-2,
//                     seg rand(4)+1, angle 167h-rand(5Ah), step rand(3)+1, gate 8Ch+rand(1Eh)
//   facing 3 (east):  intro F0DAA[9..11],  no curtain sprite,  seed x+17h,  y,
//                     seg rand(4)+1, angle 0B4h+rand(5Ah), step rand(3)+1, gate 28h-rand(14h);
//                     only this branch redraws the caster via 12F2A every iteration (3A1E6)
//   facing 2 (north): intro F0DAA[11h..13h], curtain 16 (F0E6A), seed x+0Eh,  y-0Dh,
//                     seg rand(3)+2, angle 5Ah+rand(5Ah),  step rand(2)+1, gate 136h-rand(1Eh)
//   facing 0 (south): intro F0DAA[0Dh..0Fh], curtain 20 (F0E9A), seed x+0Fh,  y,
//                     seg rand(3)+2, angle 15Eh-rand(5Ah), step rand(2)+1, gate 82h+rand(1Eh)
//   Every slot picks one of the 13 cells of the 1AC59 radius-2 diamond; its target is
//   10971(cell)+0Ch (x) and 10988(cell)-2 (y).
//   Curtain entry = F0DAA + 12*k: 0x60=8, 0xC0=16, 0xF0=20 (no entry is drawn for 3).
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
// Curtain engine: 39620 (facing 1), 38FF7 (3), 38CF8 (2), 3930E (0) are ported with the
// original float maths. Mode 2 runs 389EA (x += cos[angle], y += sin[angle], 38780 trail,
// 38B82 shift) then the per-engine gate; mode 1 runs the per-engine init + segment loop
// (38780/38B82) and target test; mode 0 runs 38BC6. The 50-slot state mirrors var_B0,
// var_7C, var_81C, var_8E4, var_114, var_178, var_308, var_2A4, var_36C, var_68C, var_754,
// var_1DC, var_240, var_5C4 and var_4FC. The 0..167h angle tables are generated as float
// cos/sin of degrees (flt_74A68 / flt_744C8).
//
// Port notes: F0DAA's data is unexplored, so its [4]/[6]/[8] triplets are read through the
// parsed magic_frames_ table (entry index == frame index, the intro convention). The auto
// 398F3 branch's picked cell is not exposed by the port collector, so its seed diamond is
// centred on the first collected target; the player branch uses battle_pick_*. The original
// has no frame cap, the port keeps a high safety cap (0x800). 38780/38B82 write directly to
// dword_755CC with a 0x1E0 stride; the port writes the same (x, y) pixels to the 320x200
// backbuffer and clips.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kRainSlots = 0x32;       // cmp cx,32h
constexpr int kRainHistory = 0x32;     // _nmalloc_(64h) = 50 words per buffer

// Per-slot state, matching the stack arrays the four engines share.
struct RainSlot {
    uint8_t mode = 2;       // var_B0: 2 = wait, 1 = fall, 0 = death anim
    uint8_t death = 0;      // var_7C: 0, 0Ah..12h, 65h finished
    float x = 0.0f;         // var_81C
    float y = 0.0f;         // var_8E4
    int seg = 1;            // var_114: segments per frame
    int hist = 0;           // var_36C: history count (max 31h)
    int angle = 0;          // var_178: degree angle 0..167h
    int step = 1;           // var_308: angle step
    int gate_target = 0;    // var_2A4: angle gate target
    int c1 = 0;             // var_3D0: seg growth / death frame counter
    int c2 = 0;             // var_434: step growth counter
    int tx = 0;             // var_1DC: seed target x (10971 + 0Ch)
    int ty = 0;             // var_240: seed target y (10988 - 2)
    float velA = 0.0f;      // var_5C4
    float velB = 0.0f;      // var_4FC
    int16_t hx[kRainHistory] = {};  // var_68C
    int16_t hy[kRainHistory] = {};  // var_754
};

// flt_74A68 / flt_744C8: 360 float entries, degrees.
struct RainTrigTable {
    float c[360];
    float s[360];
    RainTrigTable() {
        for (int i = 0; i < 360; ++i) {
            const double r = (double)i * 3.14159265358979323846264338327950288 / 180.0;
            c[i] = (float)std::cos(r);
            s[i] = (float)std::sin(r);
        }
    }
};
const RainTrigTable& rain_trig() {
    static const RainTrigTable table;
    return table;
}

// __CHP + fistp: round to nearest, then keep the low word (fistp indefinite -> 0).
int rain_round(float v) {
    if (!std::isfinite(v) || std::fabs(v) > 1.0e9f) return 0;
    return (int)std::nearbyint(v);
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
    int curtain_frame = -1;                                                    // fixed entry, -1 = none
    int seed_x = 0, seed_y = 0;
    // 39A49 initialisation for seg (var_114), gate angle (var_178), step (var_308)
    // and gate target (var_2A4).
    int seg_rand = 0, seg_add = 0, step_rand = 0;
    int gate_base = 0, gate_rand = 0;  bool gate_sub = false;
    int tgt_base = 0,  tgt_rand = 0;   bool tgt_sub = false;
    bool redraw_caster = false;                                                // facing 3 only
    switch (facing) {
        case 1:                                                                // west (loc_39D50)
            intro_first = 5; curtain_frame = 8;
            seed_x = anchor_x + 5;      seed_y = anchor_y - 2;
            seg_rand = 4; seg_add = 1; step_rand = 3;
            gate_base = 0x167; gate_rand = 0x5A; gate_sub = true;
            tgt_base = 0x8C;   tgt_rand = 0x1E;  tgt_sub = false;
            break;
        case 3:                                                                // east (loc_3A026)
            intro_first = 9; curtain_frame = -1;                                // no curtain sprite
            seed_x = anchor_x + 0x17;   seed_y = anchor_y;
            seg_rand = 4; seg_add = 1; step_rand = 3;
            gate_base = 0xB4;  gate_rand = 0x5A; gate_sub = false;
            tgt_base = 0x28;   tgt_rand = 0x14;  tgt_sub = true;
            redraw_caster = true;
            break;
        case 2:                                                                // north (loc_3A2C6)
            intro_first = 0x11; curtain_frame = 16;
            seed_x = anchor_x + 0x0E;   seed_y = anchor_y - 0x0D;
            seg_rand = 3; seg_add = 2; step_rand = 2;
            gate_base = 0x5A;  gate_rand = 0x5A; gate_sub = false;
            tgt_base = 0x136;  tgt_rand = 0x1E;  tgt_sub = true;
            break;
        default:                                                               // south (loc_3A59E)
            intro_first = 0x0D; curtain_frame = 20;
            seed_x = anchor_x + 0x0F;   seed_y = anchor_y;
            seg_rand = 3; seg_add = 2; step_rand = 2;
            gate_base = 0x15E; gate_rand = 0x5A; gate_sub = true;
            tgt_base = 0x82;   tgt_rand = 0x1E;  tgt_sub = false;
            break;
    }

    // 1AC08/1AC59 radius-2 diamond (13 cells, row-major from the top): every cell is
    // stored by 1AC59, whether occupied or not. 10971 + 0Ch / 10988 - 2 is the seed point.
    static constexpr int kDiamond[13][2] = {
        { 0, -2}, {-1, -1}, { 0, -1}, { 1, -1},
        {-2,  0}, {-1,  0}, { 0,  0}, { 1,  0}, { 2, 0},
        {-1,  1}, { 0,  1}, { 1,  1},
        { 0,  2},
    };
    int dia_x[13] = {}, dia_y[13] = {};
    int centre_x = battle_pick_tx_, centre_y = battle_pick_ty_;
    if (centre_x < 0 || centre_y < 0) {                                        // auto 398F3 branch
        if (!battle_target_list_.empty()) {
            const auto& T = units_[(size_t)battle_target_list_.front()];
            centre_x = (int)(int8_t)T.x;
            centre_y = (int)(int8_t)T.y;
        } else {
            centre_x = (int)(int8_t)A.x;
            centre_y = (int)(int8_t)A.y;
        }
    }
    for (int r = 0; r < 13; ++r) {
        const int cx = centre_x + kDiamond[r][0];
        const int cy = centre_y + kDiamond[r][1];
        dia_x[r] = (cx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0C;
        dia_y[r] = (cy - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y - 2;
    }

    for (int i = 0; i < kSlots; ++i) {                                         // loc_39D50 / 3A026 / 3A2C6 / 3A59E
        RainSlot& s = slot[(size_t)i];
        s.mode = 2;
        s.death = 0;
        s.x = (float)seed_x;
        s.y = (float)seed_y;
        s.velA = 0.0f;
        s.velB = 0.0f;
        const int pick = combat_rand(0x0D);                                    // sub_499BD(0Dh)
        s.tx = dia_x[pick];                                                    // var_1DC
        s.ty = dia_y[pick];                                                    // var_240
        s.hist = 0;
        s.c1 = 0;
        s.c2 = 0;
        s.seg = combat_rand(seg_rand) + seg_add;                               // var_114
        s.angle = gate_sub ? gate_base - combat_rand(gate_rand)
                           : gate_base + combat_rand(gate_rand);               // var_178
        s.step = combat_rand(step_rand) + 1;                                   // var_308
        s.gate_target = tgt_sub ? (tgt_base - combat_rand(tgt_rand))
                                : (tgt_base + combat_rand(tgt_rand));          // var_2A4
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

    const RainTrigTable& trig = rain_trig();

    // 38780: draw the 3x3 head (colour 10h) and the history ramp only on the last segment;
    // the new rounded point is stored at history[0] for every segment (loc_389BD).
    auto plot_point = [&](RainSlot& s, int k) {
        const int ix = rain_round(s.x);
        const int iy = rain_round(s.y);
        if (k == s.seg - 1) {
            uint8_t* bb = video_.get_backbuffer();
            if (bb) {
                auto put = [bb](int px, int py, uint8_t col) {
                    if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                        bb[(size_t)py * SCREEN_W + (size_t)px] = col;
                };
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) put(ix + dx, iy + dy, 0x10);
                int ramp = 0;
                for (int si = 0; si < s.hist; ++si) {
                    if (si % 10 == 4 && ramp < 7) ++ramp;
                    const uint8_t col = (uint8_t)(0x10 + ramp);
                    const int px = s.hx[si], py = s.hy[si];
                    if (si <= 0x10) {
                        put(px, py, col); put(px + 1, py, col); put(px - 1, py, col);
                        put(px, py - 1, col); put(px, py + 1, col);
                    } else {
                        put(px, py, col);
                    }
                }
            }
        }
        s.hx[0] = (int16_t)ix;
        s.hy[0] = (int16_t)iy;
    };

    // 38B82: shift [0..hist-1] to [1..hist]; the count itself is not modified.
    auto shift_history = [](RainSlot& s) {
        for (int j = s.hist - 1; j >= 0; --j) {
            s.hx[j + 1] = s.hx[j];
            s.hy[j + 1] = s.hy[j];
        }
    };

    // 389EA: mode 2. seg sub-steps by the degree angle table, then seg grows every 4
    // frames (max 4) and step every 6 frames (max 0Ah).
    auto wait_step = [&](RainSlot& s) {
        for (int k = 0; k < s.seg; ++k) {
            const int a = s.angle;
            s.x += trig.c[a];          // flt_74A68[var_178]
            s.y += trig.s[a];          // flt_744C8[var_178]
            plot_point(s, k);
            shift_history(s);
            if (s.hist < 0x31) ++s.hist;
        }
        if (++s.c1 > 3) { s.c1 = 0; if (s.seg < 4) ++s.seg; }
        if (++s.c2 > 5) { s.c2 = 0; if (s.step < 0x0A) ++s.step; }
    };

    // Per-engine mode 2 gate (after 389EA). 39620 (facing 1): angle < target, else
    // angle -= step (no wrap). 38FF7 (facing 3): strict target+-5, else angle += step
    // with 168h wrap (loc_390A2). 3930E (facing 0): inclusive, angle += step with
    // 168h wrap (loc_393B9). 38CF8 (facing 2): inclusive, angle += step without wrap
    // (loc_38DA1 has no 168h check).
    auto wait_gate = [&](RainSlot& s) {
        bool start;
        if (facing == 1) {
            start = s.angle < s.gate_target;
            if (!start) s.angle -= s.step;
        } else {
            if (facing == 3)
                start = s.angle > s.gate_target - 5 && s.angle < s.gate_target + 5;
            else
                start = s.angle >= s.gate_target - 5 && s.angle <= s.gate_target + 5;
            if (!start) {
                s.angle += s.step;
                if ((facing == 0 || facing == 3) && s.angle >= 0x168) s.angle -= 0x168;
            }
        }
        if (start) { s.mode = 1; sfx(0x33); }
    };

    // Mode 1 init + segment loop + target test. d24 = ty - y, d20 = x - tx; the four
    // engines differ in the branch test, the step axis/sign and the target axis.
    // 39620 (facing 1) and 3930E (facing 0) share the y+=1/x-=velA step; 38CF8
    // (facing 2, loc_3A2C6) uses y-=1/x+=velA with (tx-x)<1 end; 3930E (facing 0,
    // loc_3A59E) uses y+=1/x-=velA with (x-tx)<1 end; 38FF7 (facing 3) negates d20.
    auto fall_step = [&](RainSlot& s) {
        if (s.velA == 0.0f && s.velB == 0.0f) {
            const float d24 = (float)s.ty - s.y;
            const float d20 = s.x - (float)s.tx;
            const float n = (facing == 3) ? -d20 : d20;
            const bool useB = (facing == 2) ? (d20 < d24) : (n > d24);
            if (useB) {
                s.seg = combat_rand(3) + 9;
                s.velB = d24 / n;
            } else {
                s.seg = combat_rand(2) + 8;
                s.velA = n / d24;
            }
        }
        for (int k = 0; k < s.seg; ++k) {
            if (s.velA != 0.0f) {
                if (facing == 3)      { s.y += 1.0f; s.x += s.velA; }
                else if (facing == 2) { s.y -= 1.0f; s.x += s.velA; }
                else                  { s.y += 1.0f; s.x -= s.velA; }
            } else {
                if (facing == 3)      { s.x += 1.0f; s.y += s.velB; }
                else if (facing == 2) { s.x += 1.0f; s.y -= s.velB; }
                else                  { s.x -= 1.0f; s.y += s.velB; }
            }
            plot_point(s, k);
            shift_history(s);
        }
        const float tx = (float)s.tx, ty = (float)s.ty;
        bool dead;
        if (facing == 1 || facing == 3) dead = (ty - s.y) < 1.0f;
        else if (facing == 0)           dead = (s.x - tx) < 1.0f;
        else                            dead = (tx - s.x) < 1.0f;
        if (dead) { s.mode = 0; s.death = 0x0A; s.c1 = 0; sfx(0x34); }
    };

    // 38BC6: entries 0Ah..0Eh use frames 0..4, 0Fh..12h frames 3..0 (3 frames each),
    // then the slot is marked 65h.
    auto death_step = [&](RainSlot& s) {
        const int d = s.death;
        if (d >= 0x13) return;
        const int f = (d < 0x0F) ? (d - 0x0A) : (0x12 - d);
        if (f >= 0 && f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            fx_blit_nsp(f, rain_round(s.x) + fr.xo, rain_round(s.y) + fr.yo);
        }
        if (++s.c1 > 2) {
            s.c1 = 0;
            ++s.death;
            if (d >= 0x0F && s.death > 0x12) s.death = 0x65;
        }
    };

    int frames_run = 0;
    for (int guard = 0; guard < 0x800 && !should_exit_; ++guard) {
        battle_fx_world();
        battle_fx_flush();
        battle_fx_overlay();
        if (curtain_frame >= 0 && curtain_frame < nframes) {
            const MagicFxFrame& cf = magic_frames_[(size_t)curtain_frame];
            fx_blit_nsp(curtain_frame, anchor_x + 0x0F + cf.xo, anchor_y + 0x0A + cf.yo); // 5EFB9
        }
        if (redraw_caster) battle_draw_unit_now(unit_index);                   // 3A1E6 / sub_12F2A

        for (int i = 0; i < kSlots && !should_exit_; ++i) {
            RainSlot& s = slot[(size_t)i];
            if (s.mode == 2) {
                wait_step(s);
                wait_gate(s);
            } else if (s.mode == 1) {
                fall_step(s);
            } else if (s.death != 0) {
                death_step(s);
            }
        }
        battle_fx_commit();                                                    // sub_5E9E8
        wait_game_tick();                                                      // sub_499E4
        ++frames_run;

        bool all_done = true;
        for (int i = 0; i < kSlots; ++i) {
            if (slot[(size_t)i].death <= 0x64) { all_done = false; break; }
        }
        if (all_done) break;
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
