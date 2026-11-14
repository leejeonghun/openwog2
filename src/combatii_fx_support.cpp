// magic effect drivers: heal / wide heal / buff (sub_2476E to sub_26B1F)
// All of them share one skeleton in the original:
//   1FEA4(<spell>.nsp) loads the MAGICSPR frame table, 1124A centres the camera on the caster,
//   then 11DB2/135EB compute the screen position of the target (or the caster). 1033B(period)
//   sets a short game tick, and every frame repeats {454B3 redraws map and units ->
//   1FFFE/5EE51/5F305 draw the magic frame -> 5E9E8 presents -> 499E4 waits}; 1FFCD releases
//   the table at the end.
//   Note: the original's multi-sprite call is sub_1FFFE(eax=x, edx=y, ebx=table entry) =
//   5EE51(x+[+4], y+[+6]); 5F305(eax=x, edx=y, ebx=entry) is the palette-tinted copy. The port
//   maps battle_fx_blit = 5EE51 and battle_fx_blit_tinted = 5F305.
//
// Shared deviations (whole file):
//   - The original adds each table entry's +4/+6 offsets to x/y. The port's MagicFxFrame reads
//     xo/yo from the file header, so battle_fx_blit(frame, x + fr.xo, y + fr.yo) has the same
//     effect. (The code below passes x/y and adds xo/yo where it needs them.)
//   - Colour-table setup (611AA/61147) and the 12BEE/454D5/5E9E8 side calls that never reach the
//     screen directly (tint LUT setup, scroll correction, palette fade preparation) are already
//     covered by port helpers or folded into frame rendering, so they are skipped; each function
//     notes what it leaves out.
//   - Randomness uses combat_rand(n), the sub_499BD equivalent (the sequence differs from DOS rand).
#include "combatii.h"
#include "combatii_internal.h"

#include <cmath>
#include <cstdlib>

namespace {

// Circle split into ten steps: the original uses ten angles of 0x40 (64) and divides by
// flt_77269 to get radians. 0x280 (640) is one full turn, so 640/(2*pi) = 101.859.
constexpr double kAngleDiv = 101.859;
constexpr int    kAngleMax = 0x280;      // 640 = 2*pi
constexpr int    kAngleStep = 0x40;      // 64 = 36 deg

// Unit-circle coordinates for angle i*0x40 (original: fild angle; fdiv flt_77269; fcos/fsin).
inline void ring_point(int angle, int radius, int& dx, int& dy) {
    const double a = (double)angle / kAngleDiv;
    dx = (int)std::lround(std::cos(a) * (double)radius);   // __CHP + fistp (rounded)
    dy = (int)std::lround(std::sin(a) * (double)radius);
}

} // namespace

// sub_2476E(eax=caster, dx=target tile x, bx=target tile y): heal1.nsp heal particles.
//   Starts 16 particles at the caster screen position (+0Ch,+2) and moves them toward the
//   target centre (135EB) each frame by "remaining distance*10/dist". On arrival it runs
//   4F4CD(0x32) -> 1033B(5) -> 15 frames of frame (i%6)+0Ch over the target -> 1FFCD.
//   The original path also performs 8-direction selection (var_4) and camera scroll correction
//   (dword_7001C/BA838); the port substitutes straight caster-to-target interpolation (the
//   render step applies the camera correction).
int CombatII::battle_fx_2476e(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("heal1.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);      // sub_1124A
    int sx = 0, sy = 0;
    unit_screen_center(caster, sx, sy);                                  // 10971/10988
    sx += 12; sy += 2;                                                   // +0Ch / +2
    int ex = sx, ey = sy;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);                  // sub_135EB target centre
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 2;
    }
    game_timer_set(2);                                                   // sub_1033B(2)

    constexpr int kParticles = 16;                                       // `cmp bx,10h`
    struct P { int x, y; };
    P parts[kParticles];
    const int dx = ex - sx, dy = ey - sy;
    const int dist = (int)std::lround(std::sqrt((double)dx * dx + (double)dy * dy));
    const int step_x = dist > 0 ? dx * 10 / dist : 0;                    // (delta*10)/dist
    const int step_y = dist > 0 ? dy * 10 / dist : 0;
    for (int i = 0; i < kParticles; ++i) {
        // The original starts all 16 coordinates at the caster centre and shifts them one
        // step per frame to form a tail.
        parts[i].x = sx; parts[i].y = sy;
    }
    bool moving = (step_x != 0 || step_y != 0);
    for (int guard = 0; guard < 0x200 && moving && !should_exit_; ++guard) {
        moving = false;
        for (int i = kParticles - 1; i > 0; --i) parts[i] = parts[i - 1];  // loc_24D9D tail shift
        parts[0].x += step_x;
        parts[0].y += step_y;
        if (std::abs(ex - parts[0].x) > std::abs(step_x) ||
            std::abs(ey - parts[0].y) > std::abs(step_y)) moving = true;
        for (int i = 0; i < kParticles; ++i) {
            // Four overlaid sprites: frames (i%4)+9, (i%3)+9, (i%4)+4, (i%5)+4.
            const int f1 = (i % 4) + 9;
            const int f2 = (i % 3) + 9;
            const int f3 = (i % 4) + 4;
            const int f4 = (i % 5) + 4;
            if (f1 < nframes) battle_fx_blit(f1, parts[i].x, parts[i].y);
            if (f2 < nframes) battle_fx_blit(f2, parts[i].x + 2, parts[i].y);
            if (f3 < nframes) battle_fx_blit(f3, parts[i].x, parts[i].y - 2);
            if (f4 < nframes) battle_fx_blit(f4, parts[i].x + 2, parts[i].y - 2);
        }
        battle_fx_present();                                             // sub_454B3 + sub_5E9E8
        wait_game_tick();                                                // sub_499E4
    }
    if (should_exit_) { battle_nsp_release(); return 1; }
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(5);                                                   // sub_1033B(5)
    for (int i = 0; i < 15 && !should_exit_; ++i) {                      // loc_24FCA (di<0Fh)
        const int f = (i % 6) + 0x0C;
        if (f < nframes) battle_fx_blit(f, ex, ey);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_25047(eax/edx=caster tile, bl=variant): heal1.nsp shrinking ring (party heal).
//   Ten angles (0x40 apart) start at radius var_8=0xA0 and shrink by 1 (variant 1) or 4 (other)
//   per frame while drawing frame (counter%4). Variant 1 also rotates each angle by +0x0A per
//   frame (spinning ring). At radius 0: 4F4CD(0x32) -> 1033B(5) -> 15 frames of (i%6)+0Ch ->
//   1FFCD.
int CombatII::battle_fx_25047(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("heal1.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const bool variant1 = (k == 1);                                      // `cmp var_4,1`
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], cx, cy);                  // sub_135EB
    } else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    game_timer_set(2);                                                   // sub_1033B(2)
    int angles[10], radius = 0xA0, frame_no = 0;
    for (int i = 0; i < 10; ++i) angles[i] = i * kAngleStep;
    while (radius > 0 && !should_exit_) {                                // `cmp var_8,0; jle` (exit)
        const int f = frame_no % 4;                                      // sar/idiv 4 -> remainder
        ++frame_no;
        for (int i = 0; i < 10; ++i) {
            int px, py;
            ring_point(angles[i], radius, px, py);
            if (f < nframes) battle_fx_blit(f, cx + px, cy + py);
            if (variant1) {
                angles[i] += 0x0A;                                       // loc_25192 rotate
                if (angles[i] >= kAngleMax) angles[i] = 0;
            }
        }
        radius -= variant1 ? 1 : 4;                                      // `dec` / `sub 4`
        battle_fx_present();
        wait_game_tick();
    }
    if (should_exit_) { battle_nsp_release(); return 1; }
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(5);                                                   // sub_1033B(5)
    for (int i = 0; i < 15 && !should_exit_; ++i) {
        const int f = (i % 6) + 0x0C;
        if (f < nframes) battle_fx_blit(f, cx, cy);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_25254(eax/edx=unit tile): wideheal.nsp expanding ring (wide heal 1).
//   Ten angles; radius var_8 grows from 0 to 0xA0 by +2 while each angle rotates by +0x0A.
//   Frame = (screen y/2) % 5, with 4F4CD(0x32) every 8 frames; 1FFCD once the radius reaches
//   0xA0.
int CombatII::battle_fx_25254(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("wideheal.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);      // sub_1124A
    int cx = 0, cy = 0;
    unit_screen_center(caster, cx, cy);                                  // 10971+0Ch / 10988
    cx += 12;
    game_timer_set(5);                                                   // sub_1033B(5)
    int angles[10];
    for (int i = 0; i < 10; ++i) angles[i] = i * kAngleStep;
    int radius = 0;
    while (radius < 0xA0 && !should_exit_) {                             // `cmp var_8,0A0h; jge`
        const int f = ((cy / 2) % 5);                                     // sar 11h -> /2, idiv 5
        for (int i = 0; i < 10; ++i) {
            int px, py;
            ring_point(angles[i], radius, px, py);
            if (f < nframes) battle_fx_blit(f, cx + px, cy + py);
            angles[i] += 0x0A;
            if (angles[i] >= kAngleMax) angles[i] = 0;
        }
        radius += 2;                                                     // 454D5(var_8+2)
        if ((cy % 8) == 0) sfx(0x32);                                    // `idiv 8; test edx,edx`
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_253DC(eax/bl=caster tile): wideheal.nsp random embers (wide heal 2).
//   Runs 18 (var_2C[0x12]) slots for 100 frames (0x64). An empty slot respawns with
//   life = 499BD(0Ch)+4 and speed = 499BD(0Ah)-5 at the caster centre; each frame moves y by the
//   speed (upward when negative) and decrements life. A slot is released when y passes 199
//   (0C7h) or life runs out. Frame = (life%5)+5, with 4F4CD(0x32) every 10 frames.
int CombatII::battle_fx_253dc(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("wideheal.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    game_timer_set(5);                                                   // sub_1033B(5)
    constexpr int kSlots = 0x12;                                         // 18
    struct Spark { int alive = 0, x = 0, y = 0, life = 0, speed = 0; };
    Spark sp[kSlots];
    for (int frame = 0; frame < 0x64 && !should_exit_; ++frame) {        // `cmp di,64h`
        for (int i = 0; i < kSlots; ++i) {
            if (!sp[i].alive && frame < 0x48) {                          // new ember (loc_2547D)
                sp[i].alive = 1;
                sp[i].x = cx;
                sp[i].y = cy;
                sp[i].life = combat_rand(0x0C) + 4;                      // 499BD(12)+4
                sp[i].speed = combat_rand(10) - 5;                       // 499BD(10)-5
            }
            if (!sp[i].alive) continue;
            sp[i].y += sp[i].speed;
            --sp[i].life;
            if (sp[i].y > 0xC7 || sp[i].life <= 0) { sp[i].alive = 0; continue; }
            const int f = (sp[i].life % 5) + 5;
            if (f < nframes) battle_fx_blit(f, sp[i].x, sp[i].y);
        }
        battle_fx_present();
        wait_game_tick();
        if ((frame % 10) == 0) sfx(0x32);                                // `idiv ebp(10)`
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_25591(eax=cx, dx=cy): wideheal.nsp five-layer expanding ring (wide heal 3).
//   Keeps a 50-entry angle table (five groups of ten, 0x40 apart) and adds one ring every 10
//   frames. Ring r (0..4) grows from radius 0 to 0xA0 by 7,5,5,5,5 while rotating +4 per frame.
//   Frame = 0x0A + (4 - (t%0x1E)), i.e. it cycles 0x0E..0x0A over a 30-frame period.
int CombatII::battle_fx_25591(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("wideheal.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    camera_center_on((int)(int8_t)caster.x, (int)(int8_t)caster.y);      // sub_1124A
    int cx = 0, cy = 0;
    unit_screen_center(caster, cx, cy);
    cx += 12;                                                            // 10971+0Ch
    game_timer_set(5);                                                   // sub_1033B(5)
    constexpr int kRings = 5, kPerRing = 10;
    int angles[kRings * kPerRing];
    int radius[kRings] = {0, 0, 0, 0, 0};
    for (int r = 0; r < kRings; ++r)
        for (int i = 0; i < kPerRing; ++i) angles[r * kPerRing + i] = i * kAngleStep;
    static const int kGrow[kRings] = {7, 5, 5, 5, 5};                    // radius growth per ring
    int t = 0;
    for (int guard = 0; guard < 0x400 && !should_exit_; ++guard) {
        const int f = 0x0A + (4 - (t % 0x1E));                           // idiv 1Eh -> remainder
        int drawn = 0;
        for (int r = 0; r < kRings; ++r) {
            if (t < r * 10) continue;                                    // `cmp var_10, r*10; jle`
            for (int i = 0; i < kPerRing; ++i) {
                int px, py;
                ring_point(angles[r * kPerRing + i], radius[r], px, py);
                if (f < nframes) battle_fx_blit(f, cx + px, cy + py);
                angles[r * kPerRing + i] += 4;                           // angle +4
                if (angles[r * kPerRing + i] >= kAngleMax) angles[r * kPerRing + i] = 0;
            }
            radius[r] += kGrow[r];
            if (radius[r] > 0xA0) radius[r] = 0;                         // ring reset
            ++drawn;
        }
        if (!drawn) { battle_fx_present(); wait_game_tick(); ++t; continue; }
        battle_fx_present();
        wait_game_tick();
        ++t;
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_261FC(eax/edx=caster tile, ebx=angle): powerup.nsp attack-up effect.
//   Places five sprites along the target centre (x-0x28 + i*0x0A, a horizontal row of five).
//   The frame runs 0..3 once and then stays at 4 (unk_F0DDA = table start+4*0Ch), drawn with the
//   5F305 tint. In the same places five embers rise 4 px per frame using frame 5.
//   30 frames, 1033B(4), 4F4CD(0x32).
int CombatII::battle_fx_261fc(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("powerup.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(4);                                                   // sub_1033B(4)
    (void)k;
    const int shade = 0x28;                                              // 61147(eax=1, edx=28h)
    int spark_y[5];
    const int top = cy - 0x1E, bottom = cy;
    for (int i = 0; i < 5; ++i) spark_y[i] = cy + (i - 2) * 4;            // dword_1FE58 layout (approx.)
    for (int t = 0; t < 0x1E && !should_exit_; ++t) {                    // `cmp ax,1Eh`
        const int f = (t < 4) ? t : 4;                                   // fixed at +4*0Ch
        for (int i = 0; i < 5; ++i) {
            const int x = cx - 0x28 + i * 0x0A;                          // `sub 28h; add 0Ah`
            if (f < nframes) battle_fx_blit_tinted(f, x, cy, shade, {0x01}); // sub_5F305
            // Rising ember: frame 5, y -= 4, wrapped to the bottom when it leaves the top
            spark_y[i] -= 4;
            if (spark_y[i] < top) spark_y[i] = bottom;
            if (5 < nframes) battle_fx_blit(5, x + 2, spark_y[i]);       // sub_1FFFE
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26391(eax/edx=caster tile, bl=variant): speedup.nsp falling embers (agility up).
//   Drops three (var edi<3) particles downward from above the target (y-0..2, each offset by +4)
//   while drawing frame = 0x12 + (y%3). 40 frames (0x28), 1033B(7), 4F4CD(0x32) at start.
int CombatII::battle_fx_26391(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("speedup.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    game_timer_set(7);                                                   // sub_1033B(7)
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    int py[3];
    for (int i = 0; i < 3; ++i) py[i] = cy - (i * 4) + 4;                 // 10988(i)+4 layout (approx.)
    for (int t = 0; t < 0x28 && !should_exit_; ++t) {                    // `cmp bx,28h`
        for (int i = 0; i < 3; ++i) {
            const int f = 0x12 + (py[i] % 3);                            // `idiv 3; add 12h`
            if (f < nframes) battle_fx_blit(f, cx - (py[i] / 2), py[i]);
            py[i] += 6 + i * 2;                                          // fall downward
            if (py[i] <= cy) py[i] = cy - 0x20;                          // restart above the target
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26584(eax/edx=caster tile, ebx=direction (esi)): marmor.nsp magic-defence effect.
//   After 1124A it moves the target centre y by +6 and draws one frame per tick for 30 frames,
//   frame = direction*7 + (t%7) (direction*7 selects a 7-frame group per direction).
//   1033B(0x0A), 4F4CD(0x32) at start.
int CombatII::battle_fx_26584(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("marmor.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    cy += 6;                                                             // `add edx,6`
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(0x0A);                                                // sub_1033B(0Ah)
    const int dir = (k / 3) & 0x7;                                       // original esi = direction (0..6)
    for (int t = 0; t < 0x1E && !should_exit_; ++t) {                    // `cmp cx,1Eh`
        const int f = dir * 7 + (t % 7);                                 // `imul eax,ebx(7); + dl`
        if (f < nframes) battle_fx_blit(f, cx, cy);                      // sub_1FFFE
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26672(eax/edx=caster tile, ebx=direction): bless.nsp blessing effect.
//   Uses the same row of five as 261FC (centre x-0x28 + i*0x0A) drawn with the 5F305 tint, and
//   above it five embers rise 4 px per frame using frame (i%3)*3+5.
//   30 frames, 1033B(4), 4F4CD(0x32).
int CombatII::battle_fx_26672(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("bless.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(4);                                                   // sub_1033B(4)
    (void)k;
    const int shade = 0x28;                                              // 61147(eax=1, edx=28h)
    int spark_y[5];
    const int top = cy - 0x28, bottom = cy;
    for (int i = 0; i < 5; ++i) spark_y[i] = cy + (i - 2) * 4;
    for (int t = 0; t < 0x1E && !should_exit_; ++t) {
        const int f = (t < 4) ? t : 4;                                   // fixed at +4*0Ch
        for (int i = 0; i < 5; ++i) {
            const int x = cx - 0x28 + i * 0x0A;
            if (f < nframes) battle_fx_blit_tinted(f, x, cy, shade, {0x01}); // sub_5F305
            const int fr = (i % 3) * 3 + 5;                              // `lea [eax+eax*2]; +5`
            spark_y[i] -= 4;                                             // `sub eax,4`
            if (spark_y[i] < top) spark_y[i] = bottom;                   // wrap back to var_8
            if (fr < nframes) battle_fx_blit(fr, x + 2, spark_y[i]);     // sub_5EE51
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26810(eax/edx=caster tile, ebx=variant): silence.nsp silence effect.
//   Builds the tint LUT with 611AA (two-colour table, shade 0x28) and draws frame
//   variant*4 + (t%4) over the target centre with 5F29D (tinted blitter) for 30 frames.
//   1033B(4), 4F4CD(0x32).
int CombatII::battle_fx_26810(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("silence.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(4);                                                   // sub_1033B(4)
    const int shade = 0x28;                                              // 611AA(ebx=28h)
    const int dir = (k / 3) & 0x3;                                       // variant (edi), groups of 4
    for (int t = 0; t < 0x1E && !should_exit_; ++t) {                    // `cmp cx,1Eh`
        const int f = dir * 4 + (t % 4);                                 // `idiv 4` -> remainder
        if (f < nframes) battle_fx_blit_tinted_unclipped(f, cx, cy, shade,
                                                         {0x00, 0x57});  // word_1FE6E
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26B1F(eax/edx=caster tile): confuse.nsp confuse (curse) effect.
//   Tint LUT from 611AA (four-colour table, shade 0x28), 20021("death.rpl"), fade setup with
//   601E5(0xF0), then 1033B(5, 0x0E) and 48 frames (0x30):
//     - overlay frame 0 on the target centre with 5F305,
//     - a second frame = (t/2) for t<10 then fixed at 9 (unk_F0DE6 = table start+9*0Ch),
//     - for t>19, while (9 - (t-20)/2) is at least 6, draw one extra shaking frame using
//       499BD(1)+6.
//   602E3 advances the fade (the port does this in the render step); 602BF clears it.
int CombatII::battle_fx_26b1f(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("confuse.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    camera_center_on(tx, ty);                                            // sub_1124A
    int cx = 0, cy = 0;
    const int ti = (tx >= 0 && ty >= 0) ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    game_timer_set(5);                                                   // sub_1033B(5, 0Eh)
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    const int shade = 0x28;                                              // 611AA(ebx=28h)
    for (int t = 0; t < 0x30 && !should_exit_; ++t) {                    // `cmp cx,30h`
        if (0 < nframes) battle_fx_blit_tinted(0, cx, cy, shade,
                                               {0x00, 0x03, 0x05, 0x07}); // dword_1FE70
        const int f2 = (t < 9) ? (t / 2) : 9;                            // `sar eax,1` -> /2
        if (f2 < nframes) battle_fx_blit_tinted(f2, cx, cy, shade,
                                                {0x00, 0x03, 0x05, 0x07});
        if (t > 0x13) {                                                  // `cmp cx,13h; jle`
            int f3 = 9 - (t - 0x14) / 2;                                 // shaking frame
            if (f3 < 6) f3 = 6 + combat_rand(1);                         // 499BD(1)+6
            if (f3 < nframes) battle_fx_blit(f3, cx, cy - 4);            // sub_1FFFE
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}
