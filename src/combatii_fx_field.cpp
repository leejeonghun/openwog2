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
// Shared skeleton (as in the original): load the NSP table with 1FEA4, set the tick period
// with 1033B, then per frame (454B3 render + 5E9E8 present) + (499E4 wait), and release the
// table with 1FFCD at the end. Particle coordinates are 10971/10988 (tile -> screen) plus
// the table offset, and a particle respawns at the centre when its life expires. The ASM
// particle counts 25/27/61/5/24/29 and frame ranges (f_field 1..13, energy_f 3..6, boom
// 1..13, icestick 1..7) are kept as-is. The per-particle speed/life tables take only the
// life and offset values of the ASM stack copies (dword_1FA07/1FA52/1FAA3,
// dword_1FB5A/1FB6E, dword_1FB82/1FBE2, dword_1FC42/1FCB6); the per-frame motion
// approximates the same shape (radial + gravity).
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct FieldParticle {
    int x = 0, y = 0;
    int vx = 0, vy = 0;
    int life = 0;
    int max_life = 1;
    uint8_t color = 0;
};

// Life seeds copied from the ASM tables (25/27/61 bytes); higher means longer-lived.
constexpr uint8_t kLife20620[25] = {
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x19, 0x1C, 0x20, 0x24, 0x28,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x23, 0x26, 0x2A, 0x2E, 0x32,
};
constexpr uint8_t kLife2095A[27] = {
    0x0F, 0x12, 0x16, 0x1A, 0x19, 0x1E, 0x19, 0x1C, 0x20, 0x24,
    0x28, 0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x0F, 0x12, 0x16, 0x1A,
    0x1E, 0x23, 0x26, 0x2A, 0x2E, 0x32, 0x20,
};
constexpr uint8_t kLife20DDF[61] = {
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x19, 0x1C, 0x20, 0x24, 0x28,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x23, 0x26, 0x2A, 0x2E, 0x32, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x19, 0x1C, 0x20, 0x24, 0x28, 0x0F, 0x12, 0x16, 0x1A, 0x1E,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x23, 0x26, 0x2A, 0x2E, 0x32,
    0x0F, 0x12, 0x16, 0x1A, 0x1E, 0x19, 0x1C, 0x20, 0x1E, 0x0F,
    0x16,
};

} // namespace

// Shared particle effect for k0/k2 of sub_29514. energy=false uses f_field.nsp (1..13),
// true uses energy_f.nsp (3..6). The ASM revives a particle at the caster/target tile
// offset when its life reaches 0 (loc_208CE/loc_210A9); the port respawns the same way.
int CombatII::battle_fx_field_particles(int unit_index, int tx, int ty, int count,
                                        const uint8_t* life_table, bool energy) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "energy_f.nsp" : "f_field.nsp";
    if (!battle_nsp_load(nsp) || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    const int frame_lo = energy ? 3 : 1;
    const int frame_hi = energy ? 6 : 13;
    if (nframes <= 0) { battle_nsp_release(); return 0; }

    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    const int base_x = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int base_y = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;

    std::vector<FieldParticle> ps((size_t)count);
    for (int i = 0; i < count; ++i) {
        FieldParticle& p = ps[(size_t)i];
        // Spread the particles evenly on a circle (the original uses table x/y offsets).
        const double ang = (double)i * 6.283185307179586 / (double)count;
        const int r = 6 + (i % 5) * 3;
        p.x = base_x + (int)(std::cos(ang) * r);
        p.y = base_y + (int)(std::sin(ang) * r * 0.6);
        p.vx = (int)(std::cos(ang) * (2 + (i & 1)));
        p.vy = (int)(std::sin(ang) * (1 + (i % 3))) - 2;
        p.max_life = std::max<int>(1, life_table[i % count]);
        p.life = p.max_life;
        p.color = (uint8_t)(i & 0xFF);
    }

    sfx(0x32);                                                   // sub_4F4CD(32h)
    game_timer_set(5);                                           // sub_1033B(5)
    int spin = 0;
    for (int guard = 0; guard < 0x60 && !should_exit_; ++guard) {
        for (int i = 0; i < count; ++i) {
            FieldParticle& p = ps[(size_t)i];
            if (p.life > 0) {
                p.x += p.vx;
                p.y += p.vy;
                p.vy += 1;                                       // gravity
                --p.life;
                const int range = std::max(1, frame_hi - frame_lo + 1);
                const int frame = frame_lo + ((p.max_life - p.life) / 3) % range;
                battle_fx_blit(std::min(frame, nframes - 1), p.x, p.y);
            } else {
                // loc_208CE/210A9: respawn at the centre offset when the life reaches 0.
                const double ang = (double)i * 6.283185307179586 / (double)count;
                p.x = base_x + (int)(std::cos(ang) * 4);
                p.y = base_y + (int)(std::sin(ang) * 3);
                p.vx = (int)(std::cos(ang) * (1 + (i & 1)));
                p.vy = (int)(std::sin(ang) * 1) - 2;
                p.life = p.max_life;
            }
        }
        if ((spin++ & 3) == 0) sfx(0x32);                        // periodic sfx
        battle_fx_present();
        wait_game_tick();
    }
    sfx(0x33);
    battle_nsp_release();                                        // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: field particles unit=%d count=%d energy=%d tile=(%d,%d)\n",
                caster.id, count, energy ? 1 : 0, tx, ty);
    return 1;
}

// sub_20620 (k0, 25 particles).
int CombatII::battle_fx_20620(int unit_index, int tx, int ty, int energy) {
    return battle_fx_field_particles(unit_index, tx, ty, 25, kLife20620, energy != 0);
}

// sub_2095A (bx==1 variant, 27 particles).
int CombatII::battle_fx_2095a(int unit_index, int tx, int ty, int energy) {
    return battle_fx_field_particles(unit_index, tx, ty, 27, kLife2095A, energy != 0);
}

// sub_20DDF (k2, 61 particles).
int CombatII::battle_fx_20ddf(int unit_index, int tx, int ty, int energy) {
    return battle_fx_field_particles(unit_index, tx, ty, 61, kLife20DDF, energy != 0);
}

// sub_21114 (29722 k0): spread 5 boom/icestick sprites over the caster facing offsets.
//   ASM: index dword_1FB5A (x offsets, 4*5=20 bytes) / dword_1FB6E (y offsets) with
//   [esi+63h] (facing)*5+i, then add the 0x18/0x10 size correction.
//   var_2C[i] starts at 7 (boom) / -1 (icestick) and steps +1/-1 per frame; the sprite is
//   drawn while the condition holds.
int CombatII::battle_fx_21114(int unit_index, int tx, int ty, int energy) {
    (void)tx; (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "icestick.nsp" : "boom.nsp";
    if (!battle_nsp_load(nsp) || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    int cx = 0, cy = 0;
    unit_screen_center(caster, cx, cy);
    const int facing = caster.facing & 3;
    // Five offsets per facing (5*4 bytes at dword_1FB5A / dword_1FB6E in the ASM).
    static constexpr int8_t kX[4][5] = {
        { 0, 0, 0, 0, 0 }, { -1, 0, 1, 0, -1 }, { 0, 1, 0, -1, 0 }, { 1, 0, -1, 0, 1 },
    };
    static constexpr int8_t kY[4][5] = {
        { -1, 0, 1, 0, -1 }, { 0, 1, 0, -1, 0 }, { 1, 0, -1, 0, 1 }, { 0, -1, 0, 1, 0 },
    };
    int life[5];
    for (int i = 0; i < 5; ++i) life[i] = energy ? -1 : 7;
    sfx(0x32);
    game_timer_set(5);
    for (int guard = 0; guard < 0x30 && !should_exit_; ++guard) {
        for (int i = 0; i < 5; ++i) {
            life[i] += energy ? 1 : -1;
            if (life[i] < 0 && energy) continue;
            if (life[i] > 0xD && !energy) continue;
            const int frame = std::min(std::max(0, i * 2), nframes - 1);
            battle_fx_blit(frame, cx + kX[facing][i] * 0x18 + 12,
                           cy + kY[facing][i] * 0x10 + 4);
        }
        battle_fx_present();
        wait_game_tick();
    }
    sfx(0x33);
    battle_nsp_release();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 21114 unit=%d energy=%d\n", caster.id, energy ? 1 : 0);
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

// sub_2132B (29722 k1): 24 boom/icestick sprites spread from the target tile.
int CombatII::battle_fx_2132b(int unit_index, int tx, int ty, int energy) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "icestick.nsp" : "boom.nsp";
    if (!battle_nsp_load(nsp) || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    const int cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    sfx(0x32);
    game_timer_set(5);
    for (int guard = 0; guard < 0x40 && !should_exit_; ++guard) {
        const int r = 2 + guard / 2;
        for (int i = 0; i < 24; ++i) {
            const double ang = (double)i * 6.283185307179586 / 24.0;
            const int frame = std::min(std::max(0, (guard / 4 + i) % 8), nframes - 1);
            battle_fx_blit(frame, cx + (int)(std::cos(ang) * r),
                           cy + (int)(std::sin(ang) * r * 0.7));
        }
        if (guard % 10 == 0) sfx(0x32);
        battle_fx_present();
        wait_game_tick();
    }
    sfx(0x33);
    battle_nsp_release();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2132B unit=%d energy=%d tile=(%d,%d)\n", caster.id, energy ? 1 : 0, tx, ty);
    return 1;
}

// sub_215FE (29722 k2): 29 boom/icestick sprites spread outward from the target tile.
int CombatII::battle_fx_215fe(int unit_index, int tx, int ty, int energy) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const char* nsp = energy ? "icestick.nsp" : "boom.nsp";
    if (!battle_nsp_load(nsp) || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    const int cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    sfx(0x32);
    game_timer_set(5);
    for (int guard = 0; guard < 0x50 && !should_exit_; ++guard) {
        const int r = 1 + guard / 3;
        for (int i = 0; i < 29; ++i) {
            const double ang = (double)i * 6.283185307179586 / 29.0 + guard * 0.05;
            const int frame = std::min(std::max(0, (guard / 3 + i) % 10), nframes - 1);
            battle_fx_blit(frame, cx + (int)(std::cos(ang) * r * 2),
                           cy + (int)(std::sin(ang) * r));
        }
        if (guard % 10 == 0) sfx(0x32);
        battle_fx_present();
        wait_game_tick();
    }
    sfx(0x33);
    battle_nsp_release();
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 215FE unit=%d energy=%d tile=(%d,%d)\n", caster.id, energy ? 1 : 0, tx, ty);
    return 1;
}
