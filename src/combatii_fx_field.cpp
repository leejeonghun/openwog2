// Magic effect drivers: field / wave family
// sub_20620 / sub_2095A / sub_20DDF / sub_21114 / sub_2132B / sub_215FE
//
// sub_29514 (Fire Field / Ice Fog / E.Field) level dispatch:
//   k0 -> sub_20620 (f_field.nsp, 25 particles / energy_f.nsp)
//   k1 -> single target (28C8A) - no effect
//   k2 -> sub_20DDF (f_field.nsp, 61 particles / energy_f.nsp)
// sub_2095A is the bx==1 path of 29514 (no current caller), the 27-particle variant.
//
// sub_29722 (Fire Gazer / Diamond Fist / E.Wave) level dispatch:
//   k0 -> sub_21114 (5x boom.nsp / icestick.nsp)
//   k1 -> sub_2132B (24x boom.nsp / icestick.nsp)
//   k2 -> sub_215FE (29x boom.nsp / icestick.nsp)
//
// 20620/2095A/20DDF share one engine:
//   - the stack copy of the table is 25/27/61 signed (x,y) tile offsets and a life byte each;
//   - x accumulator starts at 0, vx = screen_x/life, vy = 10, y starts at screen_y - life*10;
//   - every frame x += vx, y += 10, frame = life % si while life > 0;
//   - at life <= 0 (and > -40) frame = (-life % di) + si, position frozen at the respawn point;
//   - the effect ends when life[24] <= -40 (the compiler reuses entry 24 as the countdown).
//   20620 uses si=1, di=13 (f_field: frame 0 while alive, 1..13 while dying);
//   energy_f uses si=6, di=3 (frames 0..5 alive, 6..8 dying).
//   2095A starts from a 9x3 grid laid out toward the facing; 20620/20DDF use the raw offsets.
//   20620 respawns when the decremented life becomes 0; 2095A/20DDF one frame later (old life 0).
//
// 21114/2132B/215FE are the boom/icestick family: fixed pixel offsets from the caster tile,
// frame = a per-group counter, and the counters cascade into the next group. No motion.
// 21114: 5 sprites, counters 5, thresholds >2 / <6, tick (frame+6)/3, end c[4].
// 2132B: 24 sprites in groups 3/5/7/9, thresholds >2 / <5, tick (frame+6)/2, end c[3].
// 215FE: 29 sprites in groups 2/3*9, thresholds >1 / <5, tick (frame+9)/3, end c[9].
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// dword_1FA07 (25 bytes) and dword_1FA39 / off_1FA1F+1 (25 bytes each).
constexpr uint8_t kLife20620[25] = {
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x19, 0x1C, 0x20, 0x24, 0x28,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x23, 0x26, 0x2A, 0x2E, 0x32,
};
constexpr int8_t kX20620[25] = {
    -1, 1, 0, 0, -2, 2, 0, 0, -3, 3, 0, 0, -1, -1, -2, 1, 1, 2, -1, -1,
    -2, 1, 1, 2, 0,
};
constexpr int8_t kY20620[25] = {
    0, 0, -1, 1, 0, 0, -2, 2, 0, 0, -3, 3, -2, -1, -1, -1, -2, -1, 2, 1,
    1, 2, 1, 1, 0,
};

// dword_1FA52 (27 bytes), var_5C (9x0, 9x-1, 9x-2), var_24 (-4..4 three times).
constexpr uint8_t kLife2095A[27] = {
    0x0F, 0x12, 0x16, 0x1A, 0x19, 0x1E, 0x19, 0x1C, 0x20, 0x24,
    0x28, 0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x0F, 0x12, 0x16, 0x1A,
    0x1E, 0x23, 0x26, 0x2A, 0x2E, 0x32, 0x20,
};
constexpr int8_t kX2095A[27] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -2, -2, -2, -2, -2, -2, -2, -2, -2,
};
constexpr int8_t kY2095A[27] = {
    -4, -3, -2, -1, 0, 1, 2, 3, 4,
    -4, -3, -2, -1, 0, 1, 2, 3, 4,
    -4, -3, -2, -1, 0, 1, 2, 3, 4,
};

// dword_1FAA3 (61 bytes), off_1FADF+1 (61 bytes), dword_1FB1D (61 bytes).
constexpr uint8_t kLife20DDF[61] = {
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x19, 0x1C, 0x20, 0x24, 0x28,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x23, 0x26, 0x2A, 0x2E, 0x32, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x19, 0x1C, 0x20, 0x24, 0x28, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x23, 0x26, 0x2A, 0x2E, 0x32,
    0x0F, 0x16, 0x1E, 0x1C, 0x24, 0x0F, 0x16, 0x1E, 0x16, 0x0F,
    0x16,
};
constexpr int8_t kX20DDF[61] = {
    -1, 1, 0, 0, -2, 2, 0, 0, -3, 3, 0, 0, -1, -1, -2, 1,
    1, 2, -1, -1, -2, 1, 1, 2, 0, -5, -4, -4, -3, -3, -2, -2,
    -1, -1, 0, -4, -3, -3, -2, -2, -1, -1, 0, 5, 4, 4, 3, 3,
    2, 2, 1, 1, 4, 3, 3, 2, 2, 1, 1, 0, 0,
};
constexpr int8_t kY20DDF[61] = {
    0, 0, -1, 1, 0, 0, -2, 2, 0, 0, -3, 3, -2, -1, -1, -1,
    -2, -1, 2, 1, 1, 2, 1, 1, 0, 0, 0, -1, -1, -2, -2, -3,
    -3, -4, -5, 1, 1, 2, 2, 3, 3, 4, 5, 0, 0, -1, -1, -2,
    -2, -3, -3, -4, 1, 1, 2, 2, 3, 3, 4, -4, 4,
};

// sub_21114: dword_1FB5A / dword_1FB6E, 5 offsets per facing.
constexpr int8_t kX21114[20] = {
    0, 0, 0, 0, 0, -1, -2, -3, -4, -5, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5,
};
constexpr int8_t kY21114[20] = {
    1, 2, 3, 4, 5, 0, 0, 0, 0, 0, -1, -2, -3, -4, -5, 0, 0, 0, 0, 0,
};

// sub_2132B: dword_1FB82 / dword_1FBE2, 24 offsets per facing (96 bytes each).
constexpr int8_t kX2132B[96] = {
    -1, 0, 1, -2, -1, 0, 1, 2, -3, -2, -1, 0, 1, 2, 3, -4,
    -3, -2, -1, 0, 1, 2, 3, 4,
    -1, -1, -1, -2, -2, -2, -2, -2, -3, -3, -3, -3, -3, -3, -3, -4,
    -4, -4, -4, -4, -4, -4, -4, -4,
    -1, 0, 1, -2, -1, 0, 1, 2, -3, -2, -1, 0, 1, 2, 3, -4,
    -3, -2, -1, 0, 1, 2, 3, 4,
    1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
};
constexpr int8_t kY2132B[96] = {
    1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    -1, 0, 1, -2, -1, 0, 1, 2, -3, -2, -1, 0, 1, 2, 3, -4,
    -3, -2, -1, 0, 1, 2, 3, 4,
    -1, -1, -1, -2, -2, -2, -2, -2, -3, -3, -3, -3, -3, -3, -3, -4,
    -4, -4, -4, -4, -4, -4, -4, -4,
    -1, 0, 1, -2, -1, 0, 1, 2, -3, -2, -1, 0, 1, 2, 3, -4,
    -3, -2, -1, 0, 1, 2, 3, 4,
};

// sub_215FE: dword_1FC42 / dword_1FCB6, 29 bytes per facing (116 bytes each). The
// disassembly lists them as dwords; byte (facing*29+i) is taken directly.
constexpr uint32_t kX215dw[29] = {
    0x00FF01FFu, 0x0100FF01u, 0xFF0100FFu, 0x00FF0100u, 0x0100FF01u,
    0xFF0100FFu, 0x00FF0100u, 0xFF000001u, 0xFEFEFFFFu, 0xFDFDFDFEu,
    0xFBFCFCFCu, 0xFAFAFBFBu, 0xF9F9F9FAu, 0xF7F8F8F8u, 0x01FFF7F7u,
    0xFF0100FFu, 0x00FF0100u, 0x0100FF01u, 0xFF0100FFu, 0x00FF0100u,
    0x0100FF01u, 0x000100FFu, 0x01010100u, 0x03020202u, 0x04040303u,
    0x05050504u, 0x07060606u, 0x08080707u, 0x09090908u,
};
constexpr uint32_t kY215dw[29] = {
    0x01010000u, 0x02020201u, 0x04030303u, 0x05050404u, 0x06060605u,
    0x08070707u, 0x09090808u, 0xFF01FF09u, 0x00FF0100u, 0x0100FF01u,
    0xFF0100FFu, 0x00FF0100u, 0x0100FF01u, 0xFF0100FFu, 0x00000100u,
    0xFEFFFFFFu, 0xFDFDFEFEu, 0xFCFCFCFDu, 0xFAFBFBFBu, 0xF9F9FAFAu,
    0xF8F8F8F9u, 0xFFF7F7F7u,    0x0100FF01u, 0xFF0100FFu, 0x00FF0100u, 0x0100FF01u, 0xFF0100FFu,
    0x00FF0100u, 0x0100FF01u,
};
inline int fx_i8(uint32_t dw, int idx) {
    return (int8_t)((dw >> (8 * (idx & 3))) & 0xFF);
}

static_assert(sizeof(kLife20620) == 25, "20620 life table");
static_assert(sizeof(kX20620) == 25 && sizeof(kY20620) == 25, "20620 offsets");
static_assert(sizeof(kLife2095A) == 27, "2095A life table");
static_assert(sizeof(kX2095A) == 27 && sizeof(kY2095A) == 27, "2095A offsets");
static_assert(sizeof(kLife20DDF) == 61, "20DDF life table");
static_assert(sizeof(kX20DDF) == 61 && sizeof(kY20DDF) == 61, "20DDF offsets");
static_assert(sizeof(kX21114) == 20 && sizeof(kY21114) == 20, "21114 offsets");
static_assert(sizeof(kX2132B) == 96 && sizeof(kY2132B) == 96, "2132B offsets");
static_assert(sizeof(kX215dw) == 29 * 4 && sizeof(kY215dw) == 29 * 4, "215FE offsets");

// sub_200DE(eax=unit, edx=n): centre the camera n tiles ahead of the caster.
void field_camera_ahead(int facing, int tx, int ty, int n, int& ox, int& oy) {
    ox = tx;
    oy = ty;
    switch (facing & 3) {
        case 0: oy += n; break;
        case 1: ox -= n; break;
        case 2: oy -= n; break;
        default: ox += n; break;
    }
}

} // namespace

// Shared engine for 20620/2095A/20DDF.
//   facing_layout: 2095A's 9x3 grid (x = var_24, y = var_5C) oriented by the caster facing.
//   respawn_on_new_zero: 20620 respawns when the new life is 0, the other two when the old life
//   was 0. death_rand is the 499BD bound at life==15 (3 in 20620/2095A, 5 in 20DDF); respawn_mode
//   0 is 20620's plain 499BD(5)->32h, 1 is 2095A/20DDF's 499BD(5)->32h+499BD(2).
int CombatII::battle_fx_field_engine(int unit_index, int tx, int ty, int count,
                                     const int8_t* xo, const int8_t* yo, const uint8_t* life,
                                     bool energy, bool facing_layout, int camera_dy,
                                     bool respawn_on_new_zero, int death_rand, int respawn_mode) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "energy_f.nsp" : "f_field.nsp";
    if (!battle_nsp_load(nsp) || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const int alive_mod = energy ? 6 : 1;     // si
    const int dead_mod = energy ? 3 : 13;     // di
    const int dead_base = energy ? 6 : 1;     // si (added for the dying frames)
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    camera_center_on(tx, ty + camera_dy);                      // sub_1124A
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };

    struct P {
        int bx = 0, by = 0;   // tile origin
        int ax = 0;           // x accumulator (var_F8/var_94)
        int ay = 0;           // y position (var_12C/var_CC)
        int vx = 0, vy = 0;   // var_90/var_1E4, var_C4/var_21C
        int life = 0;
    };
    std::vector<P> p((size_t)count);
    for (int i = 0; i < count; ++i) {
        P& q = p[(size_t)i];
        if (facing_layout) {
            const int a = kY2095A[i];          // var_24 (sawtooth)
            const int b = kX2095A[i];          // var_5C (0/-1/-2)
            switch (facing) {
                case 0: q.bx = tx + a; q.by = ty - b; break;   // loc_20A04
                case 1: q.bx = tx + b; q.by = ty - a; break;   // loc_20A39
                case 2: q.bx = tx + a; q.by = ty + b; break;   // loc_20A69
                default: q.bx = tx - b; q.by = ty - a; break;  // loc_20A9B
            }
        } else {
            q.bx = tx + xo[i];                 // loc_206A6 / loc_20E65
            q.by = ty + yo[i];
        }
        q.life = life[i];
        q.ax = 0;                              // var_F8/var_94 = 0
        q.ay = scr_y(q.by) - q.life * 10;      // var_12C/var_CC
        q.vx = scr_x(q.bx) / q.life;           // var_90/var_1E4
        q.vy = 10;                             // var_C4/var_21C
    }
    // The original keeps the frame-entry pointer in ebp across particles and frames; a particle
    // with life <= -40 jumps straight to the draw and reuses whatever ebp currently holds.
    int carry_frame = 0;
    game_timer_set(5);                         // sub_1033B(5)
    while (!should_exit_ && p[24].life > -40) { // cmp life[24], -40
        for (int i = 0; i < count && !should_exit_; ++i) {
            P& q = p[(size_t)i];
            int frame = -1;
            if (q.life > 0) {
                q.ax += q.vx;                  // add var_F8, var_90
                q.ay += q.vy;                  // add var_12C, var_C4
                frame = q.life % alive_mod;    // idiv si
            } else if (q.life > -40) {
                frame = (-q.life) % dead_mod + dead_base;  // idiv di; add si
            } else {
                frame = carry_frame;           // ebp is not reloaded for life <= -40
            }
            if (q.life > -40) carry_frame = frame;
            if (q.life == 15 && combat_rand(death_rand) == 0) sfx(0x34);
            if (frame >= 0 && frame < nframes)
                battle_fx_blit(frame, q.ax + 0x0C, q.ay);  // 1FFFE, y > -30 is dropped by the blit
            const int old = q.life;
            --q.life;
            const bool respawn = respawn_on_new_zero ? (q.life == 0) : (old == 0);
            if (respawn) {
                if (combat_rand(5) == 0) {
                    if (respawn_mode == 0) {
                        sfx(0x32);             // 20620: 499BD(5) -> 32h
                    } else {
                        sfx(0x32 + combat_rand(2));  // 2095A/20DDF: 32h + 499BD(2)
                    }
                }
                q.ax = scr_x(q.bx);            // loc_208CE / loc_210A9
                q.ay = scr_y(q.by);
            }
        }
        battle_fx_present();                   // 454D5 + 5E9E8
        wait_game_tick();                      // 499E4
    }
    battle_nsp_release();                      // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: field unit=%d count=%d energy=%d tile=(%d,%d)\n",
                c.id, count, energy ? 1 : 0, tx, ty);
    return 1;
}

// sub_20620 (k0, 25 particles), camera (tx, ty-1).
int CombatII::battle_fx_20620(int unit_index, int tx, int ty, int energy) {
    return battle_fx_field_engine(unit_index, tx, ty, 25, kX20620, kY20620, kLife20620,
                                  energy != 0, false, -1, true, 3, 0);
}

// sub_2095A (bx==1 variant, 27 particles), 9x3 grid oriented toward the facing.
int CombatII::battle_fx_2095a(int unit_index, int tx, int ty, int energy) {
    return battle_fx_field_engine(unit_index, tx, ty, 27, kX2095A, kY2095A, kLife2095A,
                                  energy != 0, true, 0, false, 3, 1);
}

// sub_20DDF (k2, 61 particles).
int CombatII::battle_fx_20ddf(int unit_index, int tx, int ty, int energy) {
    return battle_fx_field_engine(unit_index, tx, ty, 61, kX20DDF, kY20DDF, kLife20DDF,
                                  energy != 0, false, 0, false, 5, 1);
}

// sub_21114 (29722 k0): 5 boom/icestick sprites at fixed facing offsets.
//   x = 10971(unit.x) + xo*18h + 0Ch, y = 10988(unit.y) + yo*10h + 4; no motion.
//   Counter c[0..4] = 7 (boom) / -1 (icestick); c[0] moves every frame, and c[i] cascades into
//   c[i+1] when c[i] > 2 (energy) / c[i] < 6 (boom). Frame = c[i]. 200DE(unit, 3), 1033B(2).
int CombatII::battle_fx_21114(int unit_index, int tx, int ty, int energy) {
    (void)tx; (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "icestick.nsp" : "boom.nsp";
    if (!battle_nsp_load(nsp) || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const int end_frame = energy ? 7 : 13;            // di
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    int cam_x = 0, cam_y = 0;
    field_camera_ahead(facing, (int)(int8_t)c.x, (int)(int8_t)c.y, 3, cam_x, cam_y);
    camera_center_on(cam_x, cam_y);                    // sub_200DE(unit, 3)
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    int px[5], py[5], ctr[5];
    for (int i = 0; i < 5; ++i) {
        const int idx = facing * 5 + i;
        px[i] = scr_x((int)(int8_t)c.x) + kX21114[idx] * 0x18 + 0x0C;
        py[i] = scr_y((int)(int8_t)c.y) + kY21114[idx] * 0x10 + 4;
        ctr[i] = energy ? -1 : 7;
    }
    game_timer_set(2);                                 // sub_1033B(2)
    int frame_no = 0;                                  // var_8
    while (!should_exit_) {
        if (energy) ++ctr[0]; else --ctr[0];           // loc_21220
        for (int i = 4; i >= 0; --i) {                 // loc_21236 (reverse order)
            if (ctr[i] >= 0 && ctr[i] < end_frame && ctr[i] < nframes)
                battle_fx_blit(ctr[i], px[i], py[i]);  // 1FFFE
            if (i < 4) {                               // cascade into the next group
                if (energy) { if (ctr[i] > 2) ++ctr[i + 1]; }
                else        { if (ctr[i] < 6) --ctr[i + 1]; }
            }
        }
        ++frame_no;
        battle_fx_present();                           // sub_5E9E8
        wait_game_tick();                              // sub_499E4
        if (frame_no % end_frame == 0) game_timer_set((frame_no + 6) / 3);
        if (frame_no % 10 == 0) sfx(0x32);
        if (energy ? (ctr[4] > end_frame) : (ctr[4] == 0)) break;
    }
    battle_nsp_release();                              // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 21114 unit=%d energy=%d\n", c.id, energy ? 1 : 0);
    return 1;
}

// sub_2132B (29722 k1): 24 boom/icestick sprites in groups of 3/5/7/9.
//   x = 10971(unit.x) + xo*18h + 0Ch, y = 10988(unit.y) + yo*10h (no +4). 200DE(unit, 2),
//   1033B(4 boom / 9 icestick). Counters cascade while c[i] > 2 (energy) / c[i] < 5 (boom).
int CombatII::battle_fx_2132b(int unit_index, int tx, int ty, int energy) {
    (void)tx; (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "icestick.nsp" : "boom.nsp";
    if (!battle_nsp_load(nsp) || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const int end_frame = energy ? 7 : 13;             // var_C
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    int cam_x = 0, cam_y = 0;
    field_camera_ahead(facing, (int)(int8_t)c.x, (int)(int8_t)c.y, 2, cam_x, cam_y);
    camera_center_on(cam_x, cam_y);                    // sub_200DE(unit, 2)
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    int px[24], py[24];
    for (int i = 0; i < 24; ++i) {
        const int idx = facing * 24 + i;
        px[i] = scr_x((int)(int8_t)c.x) + kX2132B[idx] * 0x18 + 0x0C;
        py[i] = scr_y((int)(int8_t)c.y) + kY2132B[idx] * 0x10;
    }
    int ctr[4] = { energy ? -1 : 7, energy ? -1 : 7, energy ? -1 : 7, energy ? -1 : 7 };
    game_timer_set(energy ? 9 : 4);                    // sub_1033B
    int frame_no = 0;                                  // var_10
    while (!should_exit_) {
        if (energy) ++ctr[0]; else --ctr[0];
        int sprite = 0;
        int draw_count = 3;                            // var_8 = 3, +2 per group
        for (int g = 0; g < 4; ++g) {                  // loc_214A8
            for (int k = 0; k < draw_count; ++k, ++sprite) {
                if (ctr[g] >= 0 && ctr[g] < end_frame && ctr[g] < nframes)
                    battle_fx_blit(ctr[g], px[sprite], py[sprite]);
            }
            draw_count += 2;
            if (g < 3) {
                if (energy) { if (ctr[g] > 2) ++ctr[g + 1]; }
                else        { if (ctr[g] < 5) --ctr[g + 1]; }
            }
        }
        battle_fx_present();                           // 454D5 + 5E9E8
        wait_game_tick();                              // 499E4
        ++frame_no;
        if (frame_no % 6 == 0) game_timer_set((frame_no + 6) / 2);
        if (frame_no % 10 == 0) sfx(0x32);
        if (energy ? (ctr[3] > end_frame) : (ctr[3] == 0)) break;
    }
    battle_nsp_release();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2132B unit=%d energy=%d\n", c.id, energy ? 1 : 0);
    return 1;
}

// sub_215FE (29722 k2): 29 boom/icestick sprites in groups of 2 then 9x3.
//   x = 10971(unit.x) + xo*18h + 0Ch, y = 10988(unit.y) + yo*10h + 4. 200DE(unit, 5),
//   1033B(2). Counters cascade while c[i] > 1 (energy) / c[i] < 5 (boom); end at c[9].
int CombatII::battle_fx_215fe(int unit_index, int tx, int ty, int energy) {
    (void)tx; (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "icestick.nsp" : "boom.nsp";
    if (!battle_nsp_load(nsp) || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const int end_frame = energy ? 7 : 13;             // var_4
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    int cam_x = 0, cam_y = 0;
    field_camera_ahead(facing, (int)(int8_t)c.x, (int)(int8_t)c.y, 5, cam_x, cam_y);
    camera_center_on(cam_x, cam_y);                    // sub_200DE(unit, 5)
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    int px[29], py[29];
    for (int i = 0; i < 29; ++i) {
        const int idx = facing * 29 + i;
        const int xo = fx_i8(kX215dw[idx / 4], idx % 4);
        const int yo = fx_i8(kY215dw[idx / 4], idx % 4);
        px[i] = scr_x((int)(int8_t)c.x) + xo * 0x18 + 0x0C;
        py[i] = scr_y((int)(int8_t)c.y) + yo * 0x10 + 4;
    }
    int ctr[10];
    for (int i = 0; i < 10; ++i) ctr[i] = energy ? -1 : 7;
    game_timer_set(2);                                 // sub_1033B(2)
    int frame_no = 0;                                  // var_C
    while (!should_exit_) {
        if (energy) ++ctr[0]; else --ctr[0];
        int sprite = 0;
        // group 0: two sprites (loc_2176E)
        for (int k = 0; k < 2; ++k, ++sprite) {
            if (ctr[0] >= 0 && ctr[0] < end_frame && ctr[0] < nframes)
                battle_fx_blit(ctr[0], px[sprite], py[sprite]);
        }
        if (energy) { if (ctr[0] > 1) ++ctr[1]; } else { if (ctr[0] < 5) --ctr[1]; }
        // groups 1..9: three sprites each (loc_217EE)
        for (int g = 1; g < 10; ++g) {
            for (int k = 0; k < 3; ++k, ++sprite) {
                if (ctr[g] >= 0 && ctr[g] < end_frame && ctr[g] < nframes)
                    battle_fx_blit(ctr[g], px[sprite], py[sprite]);
            }
            if (g < 9) {
                if (energy) { if (ctr[g] > 1) ++ctr[g + 1]; }
                else        { if (ctr[g] < 5) --ctr[g + 1]; }
            }
        }
        ++frame_no;                                    // inc var_C
        battle_fx_present();                           // 454D5 + 5E9E8
        wait_game_tick();                              // 499E4
        if (frame_no % end_frame == 0) game_timer_set((frame_no + 9) / 3);
        if (frame_no % 10 == 0) sfx(0x32);
        if (energy ? (ctr[9] > end_frame) : (ctr[9] == 0)) break;
    }
    battle_nsp_release();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 215FE unit=%d energy=%d\n", c.id, energy ? 1 : 0);
    return 1;
}

// sub_22AF9 (29514 bx==1, Ice Fog): pick icewind/icestor/icetypho.nsp by k and radiate
//   50 particles (0x32). Start position is (i%10)*0x20 + (i%4)*4, y=(i/10)*0x28.
int CombatII::battle_fx_22af9(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = (k == 0) ? "icewind.nsp" : (k == 1 ? "icestor.nsp" : "icetypho.nsp");
    if (!battle_nsp_load(nsp) || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    const int cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    struct P { int x, y, vx, vy, life; };
    std::vector<P> ps(50);
    for (int i = 0; i < 50; ++i) {
        P& p = ps[(size_t)i];
        p.x = (i % 10) * 0x20 + (i % 4) * 4;                 // loc_22B50 offset math
        p.y = (i / 10) * 0x28;
        p.vx = ((i % 7) - 3);
        p.vy = ((i % 5) - 2) * 2;
        p.life = 0x14 + (i % 0x10);
    }
    sfx(0x32);
    game_timer_set(5);
    for (int guard = 0; guard < 0x48 && !should_exit_; ++guard) {
        for (int i = 0; i < 50; ++i) {
            P& p = ps[(size_t)i];
            if (p.life <= 0) { p.life = 0x14 + (i % 0x10); continue; }
            p.x += p.vx;
            p.y += p.vy;
            p.vy += 1;
            --p.life;
            const int frame = std::min(std::max(0, (0x14 - p.life) / 2), nframes - 1);
            battle_fx_blit(frame, cx + p.x - 0xA0, cy + p.y - 0x78);
        }
        if (guard % 10 == 0) sfx(0x32);
        battle_fx_present();
        wait_game_tick();
    }
    sfx(0x33);
    battle_nsp_release();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 22AF9 unit=%d k=%d nsp=%s\n", caster.id, k, nsp);
    return 1;
}
