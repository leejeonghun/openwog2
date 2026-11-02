// Magic effect drivers: ice, lightning and light
//   sub_22172 icemiss.nsp   (Ice Missile / Ice Fog)
//   sub_22486 diamondd.nsp  (Diamond Fist / Diamond Dust)
//   sub_23882 lbolt.nsp     (Lightning Bolt)
//   sub_241F7 p_light.nsp   (P.Lighting, 16-particle pillar)
//   sub_24466 p_light.nsp   (Turn Undead variant, 48-frame band)
//
// Common shape, as in the original:
//   Each driver loads the NSP table with 1FEA4 (unk_F0DAA, 12-byte entries = frame*0Ch),
//   shortens the game tick with 1033B, then per frame does (454B3 render + 5E9E8 present) +
//   (499E4 wait), and clears the table with 1FFCD at the end. 200DE (caster effect pose) and
//   1FFFE (sprite placement) are replaced by battle_fx_blit(frame, x, y), which takes the
//   position directly.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// Frame-count clamp for the drivers that use the 12-byte entry table.
inline int fx_clamp_frame(int f, int n) {
    if (n <= 0) return -1;
    if (f < 0) return 0;
    return (f >= n) ? n - 1 : f;
}

// 16-byte circle/sine approximation that sub_22486 copies from 1FD5A/1FD6A (signed bytes).
constexpr int8_t kDiamondCos[16] = {4, 3, 2, 1, 0, -1, -2, -3, -4, 3, 2, 1, 0, -1, -2, -3};
constexpr int8_t kDiamondSin[16] = {0, 1, 2, 3, 4, 3, 2, 1, 0, -1, -2, -3, -4, -3, -2, -1};

// Initial trail[16] that sub_22486 copies from off_1FD7A to the stack (signed words).
//   dd 120014h, 180010h, 0A0009h, 60008h, 0C0004h, 100004h, 140008h, 0E000Ch
constexpr int16_t kDiamondTrailInit[16] = {
    0x14, 0x12, 0x10, 0x18, 0x09, 0x0A, 0x08, 0x06,
    0x04, 0x0C, 0x04, 0x10, 0x08, 0x14, 0x0C, 0x0E,
};

// 16-particle x offsets that sub_241F7/24466 copy from off_1FDAE / off_1FDFE (words).
//   dd 3 dup(1FFFFh), 0, 3 dup(2FFFEh), 0
constexpr int16_t kPillarX[16] = {
    -1, 1, -1, 1, -1, 1, 0, 0,
    -2, 2, -2, 2, -2, 2, 0, 0,
};

// 16-particle y offsets: dword_1FDCE (sub_241F7) and dword_1FE1E (sub_24466), signed words.
//   dd 0FFFFFFFFh, 0, 1FFFFh, 0FFFEFFFEh, 20002h, 0, 2FFFEh, 2FFFEh
constexpr int16_t kPillarY_241f7[16] = {
    -1, -1, 0, 0, -1, 1, -2, -2,
    2, 2, 0, 0, -2, 2, -2, 2,
};
//   dd 0FFFFFFFFh, 10001h, 0, 1FFFFh, 0FFFEFFFEh, 20002h, 0, 2FFFEh
constexpr int16_t kPillarY_24466[16] = {
    -1, -1, 1, 1, 0, 0, -1, 1,
    -2, -2, 2, 2, 0, 0, -2, 2,
};

} // namespace

// sub_22172(caster in eax, target tile x in edx, target tile y in bx, strength in cl) -
//   icemiss.nsp projectile.
//   1FEA4 "icemiss.nsp" -> 200DE(caster, 3) -> start point (10971(caster x)+0Ch, 10988(caster y))
//   -> the target unit is looked up with 11DB2 (tile) and centred with 135EB.
//   The speed component is cl+3; depending on facing ([unit+63h]: 0 south, 1 west, 2 north,
//   3 east) the projectile moves:
//     south: y += 3, x approaches by (target-x)/5      (frame c*4+3)
//     west:  x -= 3, y approaches by (target-y)/5      (frame c*4+0)
//     north: y -= 3, x approaches by (target-x)/5      (frame c*4+1)
//     east:  x += 3, y approaches by (target-y)/5      (frame c*4+2)
//   Ends with 1FFCD + sfx(0x33) once the travel axis is within 6px of the target (or reaches it).
//   Each frame draws a 5-cell tail with frames 8/9/10 in the order (i%3)+8.
//   Deviation: the tail counter (trail[i] = (distance+2)/3 in the original) is replaced by a
//   fixed 3px spacing.
int CombatII::battle_fx_22172(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("icemiss.nsp") || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    CombatUnitPlacement& caster = units_[(size_t)unit_index];

    int x = 0, y = 0;
    unit_screen_center(caster, x, y);          // sub_10971 / sub_10988
    x += 12;                                   // add eax, 0Ch

    int ex = x, ey = y;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);                 // sub_135EB
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }

    const int facing = caster.facing & 3;
    const bool vertical = (facing == 0 || facing == 2);                 // south/north travel vertically
    const int mag = (k & 0xFF) + 3;                                    // movsx ax, cl / add eax, 3

    sfx(0x32);                                                         // sub_4F4CD(32h)
    game_timer_set(5);                                                 // sub_1033B(5)
    int counter = 0;
    for (int guard = 0; guard < 0x100 && !should_exit_; ++guard) {
        const int sx = (ex - x) / 5;                                   // idiv 5
        const int sy = (ey - y) / 5;
        int frame = 0;
        switch (facing) {
            case 0:  y = y + 3; x = x + sx; frame = counter * 4 + 3; break;   // south
            case 1:  x = x - 3; y = y + sy; frame = counter * 4 + 0; break;   // west
            case 2:  y = y - 3; x = x + sx; frame = counter * 4 + 1; break;   // north
            default: x = x + 3; y = y + sy; frame = counter * 4 + 2; break;   // east
        }
        battle_fx_blit(fx_clamp_frame(frame, nframes), x, y);

        // 5-cell tail (frames 8,9,10 cycling). It starts offset by x-=(vy*5), y-=(vx*4) and
        // steps by vx/vy per cell, matching the edi/esi progression of original loc_22333.
        const int vx = vertical ? 0 : (facing == 1 ? -mag : mag);
        const int vy = vertical ? (facing == 0 ? mag : -mag) : 0;
        int tpos_y = y + vx * 4;
        int tpos_x = x + vy * 5;
        for (int i = 0; i < 5; ++i) {
            battle_fx_blit(fx_clamp_frame(8 + (i % 3), nframes), tpos_x, tpos_y);
            tpos_x += vy;
            tpos_y += vx;
        }
        battle_fx_present();                                           // sub_5E9E8 (includes 454B3)
        ++counter;

        const bool reached = (x == ex && y == ey);
        const bool near = vertical ? (std::abs(y - ey) <= 6) : (std::abs(x - ex) <= 6);
        if (reached || near) break;
        wait_game_tick();                                              // sub_499E4
    }
    sfx(0x33);                                                         // sub_4F4CD(33h)
    battle_nsp_release();                                              // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 22172 icemiss unit=%d facing=%d target=(%d,%d)\n",
                caster.id, facing, ex, ey);
    return 1;
}

// sub_22486(caster in eax, target tile x in dx, target tile y in bx) - diamondd.nsp 16-particle
// burst.
//   1FEA4 "diamondd.nsp" -> 200DE(caster) -> target centre via 11DB2/135EB.
//   After 1033B(5), for 50 frames (x = 0..0x31):
//     - Past frame 0x0A (si > 0Ah) also draw frame si/3+3 at the centre.
//     - 16 particles: frame = i%3; while alive (trail[i] > 0) the position is
//       (target x - 4 - cos[i]*trail, target y - sin[i]*trail). trail[i] -= 2; below 2 it is
//       reset to 0x10 until frame 0x28 and then set to 0xD8F0 (-10000, permanently dead).
//     - sfx(0x32) once every 5 frames. Ends with 1FFCD + sfx(0x33).
//   Deviation: the original 1FFFE also adds the frame entry's [edx+8] as a y/position
//   correction; the port uses the frame offsets (.xo/.yo) unchanged.
int CombatII::battle_fx_22486(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("diamondd.nsp") || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    CombatUnitPlacement& caster = units_[(size_t)unit_index];

    int ex = 0, ey = 0;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);                 // sub_135EB
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    } else {
        unit_screen_center(caster, ex, ey);
    }

    int16_t trail[16];
    for (int i = 0; i < 16; ++i) trail[i] = kDiamondTrailInit[i];

    game_timer_set(5);                                                 // sub_1033B(5)
    for (int si = 0; si < 0x32 && !should_exit_; ++si) {
        if (si > 0x0A)                                                 // centre ripple
            battle_fx_blit(fx_clamp_frame(si / 3 + 3, nframes), ex, ey);
        for (int i = 0; i < 16; ++i) {
            if (trail[i] <= 0) continue;
            const int r = trail[i];
            const int px = ex - 4 - (int)kDiamondCos[i] * r;            // `sub edi, 4`
            const int py = ey - (int)kDiamondSin[i] * r;
            battle_fx_blit(fx_clamp_frame(i % 3, nframes), px, py);
            trail[i] = (int16_t)(trail[i] - 2);
            if (trail[i] < 2) trail[i] = (si < 0x28) ? (int16_t)0x10 : (int16_t)0xD8F0;
        }
        battle_fx_present();
        if (si == 5) sfx(0x32);                                        // sub_4F4CD(32h)
        wait_game_tick();
    }
    battle_nsp_release();                                              // sub_1FFCD
    sfx(0x33);                                                         // sub_4F4CD(33h)
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 22486 diamondd unit=%d target=(%d,%d)\n", caster.id, ex, ey);
    return 1;
}

// sub_23882(caster in eax, target tile x in edx, target tile y in bx, (bolt count-1) in cl) -
//   lbolt.nsp lightning.
//   1FEA4 "lbolt.nsp" -> 200DE(caster) -> start (10971(x)+0Ch, 10988(y)) -> target via 11DB2/135EB.
//   sfx(0x32) -> 1033B(2) -> bolt count = cl+1.
//   Per frame: each axis approaches by (target-current)/5, except the facing axis ([63h]),
//   which moves a fixed +-3 (south y+3, west x-3, north y-3, east x+3 - same table as
//   sub_22172).
//   Each bolt draws frame = 499BD(3) + facing*3 at a position jittered by 499BD(5)-2 on both
//   axes. Ends with 1FFCD once the travel axis is within 6px (or exactly on target).
int CombatII::battle_fx_23882(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("lbolt.nsp") || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    CombatUnitPlacement& caster = units_[(size_t)unit_index];

    int x = 0, y = 0;
    unit_screen_center(caster, x, y);                                  // sub_10971 / sub_10988
    x += 12;

    int ex = x, ey = y;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);                 // sub_135EB
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }

    const int facing = caster.facing & 3;
    const int bolts = (k & 0xFF) + 1;                                  // mov ah, var_4 / inc ah
    sfx(0x32);                                                         // sub_4F4CD(32h)
    game_timer_set(2);                                                 // sub_1033B(2)
    for (int guard = 0; guard < 0x100 && !should_exit_; ++guard) {
        const int sx = (ex - x) / 5;                                   // idiv edi(5)
        const int sy = (ey - y) / 5;
        switch (facing) {
            case 0:  x = x + sx; y = y + 3; break;                     // south
            case 1:  x = x - 3;  y = y + sy; break;                    // west
            case 2:  x = x + sx; y = y - 3; break;                     // north
            default: x = x + 3;  y = y + sy; break;                    // east
        }
        for (int b = 0; b < bolts; ++b) {
            const int f = combat_rand(3) + facing * 3;                 // 499BD(3) + [63h]*3
            const int px = x + combat_rand(5) - 2;
            const int py = y + combat_rand(5) - 2;
            battle_fx_blit(fx_clamp_frame(f, nframes), px, py);
        }
        battle_fx_present();
        if (x == ex && y == ey) break;
        const bool vertical = (facing == 0 || facing == 2);
        if (vertical ? (std::abs(y - ey) <= 6) : (std::abs(x - ex) <= 6)) break;
        wait_game_tick();
    }
    battle_nsp_release();                                              // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 23882 lbolt unit=%d facing=%d bolts=%d\n", caster.id, facing, bolts);
    return 1;
}

// sub_241F7(caster in eax, (strength-1) in dl) - p_light.nsp 16-particle pillar.
//   1FDAE (16 word x offsets) / 1FDCE (16 word y offsets) / 1FDEE (16 particle progress bytes,
//   all zero). Particle position = 10971(caster x + tblx)+0Ch, 10988(caster y + tbly). 1033B(5).
//   Total frames = cl*32 + 11. Every 5 frames: palette restore (5F9E9) -> wait -> dim (5F9AE)
//   + sfx(0x32).
//   Particle i is alive only while i*4 < current frame; for progress p (0..11), p < 8 draws
//   frame p at (x, y) and frame p+8 at the four corners (x+-4p, y+-4p) plus the centre. p then
//   advances by one. Ends with 1FFCD.
//   Port: 5F9E9 maps to pal_load_range(0, 0x100, master palette). 5F9AE (dimming) is not ported,
//   so only the restore and the sound (4F4CD 32h) remain.
int CombatII::battle_fx_241f7(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty;                         // the original does not use the target tile either
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("p_light.nsp") || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    CombatUnitPlacement& caster = units_[(size_t)unit_index];

    int px[16], py[16];
    uint8_t phase[16] = {};
    {
        int base_x = 0, base_y = 0;
        unit_screen_center(caster, base_x, base_y);                     // sub_10971 / sub_10988
        for (int i = 0; i < 16; ++i) {
            px[i] = base_x + 12 + kPillarX[i];
            py[i] = base_y + kPillarY_241f7[i];
        }
    }
    game_timer_set(5);                                                 // sub_1033B(5)
    const int total = (k & 0xFF) * 32 + 11;                            // shl eax,5 / add eax,0Bh
    int tick = 0;
    for (int frame = 0; frame < total && !should_exit_; ++frame, ++tick) {
        if (tick % 5 == 0) {                                           // var_10 % 5 == 0
            battle_fx_present();                                       // 454B3
            // sub_5F9E9(0, 100h, unk_12824C, 1): restore the master palette (battle_palette_)
            pal_load_range(0, 256, &battle_palette_[0][0]);
            sfx(0x32);                                                 // 4F4CD(32h)
            wait_game_tick();
        }
        for (int i = 0; i < 16; ++i) {
            if (frame < i * 4) continue;                               // `cmp edx, ebx` (i*4) / jl
            const int p = phase[i];
            if (p >= 12) continue;
            if (p < 8) battle_fx_blit(fx_clamp_frame(p, nframes), px[i], py[i]);
            const int q = p * 4;                                       // `shl eax, 2`
            const int f = fx_clamp_frame(p + 8, nframes);
            battle_fx_blit(f, px[i] - q, py[i] - q);
            battle_fx_blit(f, px[i] - q, py[i] + q);
            battle_fx_blit(f, px[i] + q, py[i] - q);
            battle_fx_blit(f, px[i] + q, py[i] + q);
            battle_fx_blit(f, px[i], py[i]);
            phase[i] = (uint8_t)(p + 1);
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                              // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 241F7 p_light unit=%d frames=%d\n", caster.id, total);
    return 1;
}

// sub_24466(caster in eax) - p_light.nsp variant (Turn Undead effect).
//   1FDFE (16 word x offsets) / 1FE1E (16 word y offsets); particle placement follows sub_241F7.
//   After 1033B(5), 48 frames (0..0x2F):
//     - At frames 0 and 8: palette restore (5F9E9) -> wait -> dim (5F9AE) + sfx(0x32).
//     - When frame % 16 == 0, push all 16 particles away from the caster (x 0x18, y 0x10).
//     - frame < 8  -> particles 0..7 with frames (f, f+8)
//       frame < 16 -> particles 8..15 with the same two frames
//       otherwise  -> particles 0..15 with frames (f%8, f%8+8)
//       Off-screen positions (x <= -50 or y >= 200) are skipped. Ends with 1FFCD.
//   Port: palette restore uses pal_load_range as in sub_241F7 (5F9AE dimming is not ported).
int CombatII::battle_fx_24466(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("p_light.nsp") || !battle_fx_loaded()) return 0;
    const int nframes = battle_fx_frame_count();
    CombatUnitPlacement& caster = units_[(size_t)unit_index];

    int base_x = 0, base_y = 0;
    unit_screen_center(caster, base_x, base_y);                        // sub_10971 / sub_10988
    base_x += 12;
    int px[16], py[16];
    for (int i = 0; i < 16; ++i) {
        px[i] = base_x + kPillarX[i];
        py[i] = base_y + kPillarY_24466[i];
    }
    game_timer_set(5);                                                 // sub_1033B(5)
    for (int frame = 0; frame < 0x30 && !should_exit_; ++frame) {
        if (frame == 0 || frame == 8) {                                // `test cx,cx` / `cmp cx,8`
            battle_fx_present();
            pal_load_range(0, 256, &battle_palette_[0][0]);             // sub_5F9E9 palette restore
            sfx(0x32);                                                 // sub_4F4CD(32h)
            wait_game_tick();
        }
        if (frame % 16 == 0) {                                         // `idiv 10h` / `test edx,edx`
            for (int i = 0; i < 16; ++i) {                            // push 0x18/0x10 away from the caster
                if (px[i] < base_x) px[i] -= 0x18;                     // `sub edi, 18h`
                else if (px[i] > base_x) px[i] += 0x18;                // `add esi, 18h`
                if (py[i] < base_y) py[i] -= 0x10;                     // `sub edx, 10h`
                else if (py[i] > base_y) py[i] += 0x10;                // `add edi, 10h`
            }
        }
        int first = 0, last = 16;
        if (frame < 8)       last = 8;                                 // particles 0..7
        else if (frame < 16) { first = 8; last = 16; }                 // particles 8..15
        int f1 = frame, f2 = frame + 8;
        if (frame >= 16) { f1 = frame % 8; f2 = f1 + 8; }               // `idiv 8` -> dx
        f1 = fx_clamp_frame(f1, nframes);
        f2 = fx_clamp_frame(f2, nframes);
        for (int i = first; i < last; ++i) {
            if (px[i] <= -50 || py[i] >= 200) continue;                // 0FFFFFFCEh / 0C8h
            battle_fx_blit(f1, px[i], py[i]);
            battle_fx_blit(f2, px[i], py[i]);
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                              // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 24466 p_light-v unit=%d\n", caster.id);
    return 1;
}
