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
inline void ring_point(int angle, int radius, int cx, int cy, int& px, int& py) {
    const double a = (double)angle / kAngleDiv;
    // The original loads the centre, faddp, then __CHP + fistp, so the truncation applies
    // to (centre + cos/sin * radius), not to the offset alone.
    px = (int)((double)cx + std::cos(a) * (double)radius);
    py = (int)((double)cy + std::sin(a) * (double)radius);
}

} // namespace

// sub_2476E(eax=caster, dx=target tile x, bx=target tile y): heal1.nsp heal particles.
//   The head advances in 1/16-pixel steps from the caster anchor (20136+0Ch/20146) toward the
//   target centre (135EB). Each outer pass recomputes distance/3.0 and runs var_40 = int(var_30*
//   2.5) sub-steps; every sub-step applies the off_2474E 8-direction displacement (var_4C
//   decays by var_10), shifts the 16-entry trail and draws the seven sub_1FFFE calls at trail
//   15/12/9/6/3/0 plus the new head. While var_4C/var_10 remain set, the direction is remapped
//   through dword_1FE3E and 499E4 waits one tick. On arrival it runs 4F4CD(0x32) -> 1033B(5)
//   -> 15 frames of frame (i%6)+0Ch over the target -> 1FFCD and restores the resting camera
//   fine values. The camera-scroll correction inside the sub-step (dword_7001C/BA838) is still
//   not modelled; the head is converted with the camera that 1124A selected.
int CombatII::battle_fx_2476e(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("heal1.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& caster = units_[(size_t)unit_index];
    const int caster_x = (int)(int8_t)caster.x;
    const int caster_y = (int)(int8_t)caster.y;
    camera_center_on(caster_x, caster_y);                               // sub_1124A

    // sub_20136/sub_20146 are absolute tile -> world; sub_10971/sub_10988 are screen. The heal
    // anchor is 20136+0Ch/20146 and 10971+0Ch/10988, not sub_135EB (which adds the sprite centre).
    const int world_x = caster_x * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0C;
    const int world_y = caster_y * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    int head_x = (caster_x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x0C;
    int head_y = (caster_y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;

    int ex = head_x, ey = head_y;
    const bool tile_ok = (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_);
    const int ti = tile_ok ? battle_unit_index_at(tx, ty) : -1;
    if (ti >= 0 && (size_t)ti < units_.size()) {
        unit_screen_center(units_[(size_t)ti], ex, ey);                  // sub_135EB target centre
    } else if (tile_ok) {
        ex = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        ey = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 2;
    }
    // 135EB returns screen space; the original adds cam*24/16 to get the world target.
    const int target_x = ex + cam_tile_x_ * TOWN_TILE_W;
    const int target_y = ey + cam_tile_y_ * TOWN_TILE_H;
    game_timer_set(2);                                                   // sub_1033B(2)

    constexpr int kTrail = 16;                                           // `cmp bx,10h` (var_8A/var_AA)
    struct P { int x, y; };
    P trail[kTrail];
    for (P& p : trail) { p.x = head_x; p.y = head_y; }                   // loc_2485C init

    int point_x = world_x;                                               // var_14
    int point_y = world_y;                                               // var_3C
    int dir = 0;                                                         // var_4
    int radius = 0;                                                      // var_4C
    int radius_step = 0;                                                 // var_10
    static constexpr int kDirMap[8] = {1, 0, 3, 2, 5, 4, 7, 6};          // dword_1FE3E

    for (int burst = 0; burst < 0x100 && !should_exit_; ++burst) {
        // loc_24881: step toward the target, using the truncated distance/3.0 as the divisor and
        // storing the low byte (mov [var_8], al) exactly like the original.
        const double dx = (double)(target_x - point_x);
        const double dy = (double)(target_y - point_y);
        const double dist3 = std::sqrt(dx * dx + dy * dy) / 3.0;
        const int var30 = (int)dist3;                                    // var_30
        const int step_x = var30 != 0 ? (int)(int8_t)((target_x - point_x) * 10 / var30) : 0;
        const int step_y = var30 != 0 ? (int)(int8_t)((target_y - point_y) * 10 / var30) : 0;

        // The direction choice tests the radius/radius_step left over from the previous pass,
        // before this pass overwrites them with the new distance mode.
        if (radius == 0 && radius_step == 0) {
            const int ax = std::abs(step_x);
            const int ay = std::abs(step_y);
            if (ax <= 5 && step_y > 0) dir = 0;
            else if (ax <= 5 && step_y < 0) dir = 1;
            else if (ay <= 5 && step_x > 0) dir = 2;
            else if (ay <= 5 && step_x < 0) dir = 3;
            else if (step_x < -5 && step_y < -5) dir = 4;
            else if (step_x > 5 && step_y > 5) dir = 5;
            else if (step_x < -5 && step_y > 5) dir = 6;
            else if (step_x > 5 && step_y < -5) dir = 7;
        }

        const int var40 = (int)((double)var30 * 2.5);                    // var_40
        if (dist3 > 30.0) {                                              // loc_24ABC
            radius_step = 1;                                             // var_10
            radius = var40 / 2;                                          // var_4C
        } else if (dist3 > 4.0) {                                        // loc_24ADD
            radius = var40;
            radius_step = 2;
        } else {
            radius = 0;
            radius_step = 0;
        }

        const int base_x = point_x;                                      // var_58
        const int base_y = point_y;                                      // var_54
        int fx = 0;                                                      // edi
        int16_t fy = 0;                                                  // var_48

        for (int n = 0; n < var40 && !should_exit_; ++n) {               // loc_24B1E
            fx += step_x;
            fy = (int16_t)(fy + step_y);

            if (dir <= 7) {                                              // off_2474E
                const int r2 = radius * 2;
                switch (dir) {
                    case 0: fx += r2; break;                             // loc_24B82
                    case 1: fx -= r2; break;                             // loc_24B8B
                    case 2: fy = (int16_t)(fy + r2); break;              // loc_24BA8
                    case 3: fy = (int16_t)(fy - r2); break;              // loc_24B9A
                    case 4: fx -= r2; fy = (int16_t)(fy + r2); break;    // loc_24B8F
                    case 5: fx += r2; fy = (int16_t)(fy - r2); break;    // loc_24B93
                    case 6: fx += r2; fy = (int16_t)(fy + r2); break;    // loc_24BA1
                    case 7: fx -= r2; fy = (int16_t)(fy - r2); break;    // loc_24BAF
                }
                radius -= radius_step;                                   // loc_24BB6
            }

            point_x = base_x + ((int16_t)fx >> 4);                       // var_14
            point_y = base_y + (fy >> 4);                                // var_3C

            // loc_24DB0 converts the updated world point to screen, then loc_24D9D shifts the
            // trail and pushes the previous head.
            const int new_x = point_x - cam_tile_x_ * TOWN_TILE_W + cam_fine_x_ + 8;
            const int new_y = point_y - cam_tile_y_ * TOWN_TILE_H + cam_fine_y_ + 6;
            for (int i = kTrail - 1; i > 0; --i) trail[i] = trail[i - 1];
            trail[0].x = head_x;
            trail[0].y = head_y;
            head_x = new_x;
            head_y = new_y;

            // Seven sub_1FFFE calls at trail 15/12/9/6/3/0 and the current head. Frames use the
            // sub-step counter n; sub_1FFFE adds the table entry's +4/+6 (xo/yo).
            const int f1 = (n % 4) + 9;
            const int f2 = (n % 3) + 9;
            const int f3 = (n % 4) + 4;
            const int f4 = (n % 5) + 4;
            const int f5 = n % 4;
            battle_fx_world_dim();                                       // sub_454B3 + sub_454D5
            auto blit = [&](int f, int x, int y) {
                if (f < 0 || f >= nframes) return;
                const MagicFxFrame& fr = magic_frames_[(size_t)f];
                battle_fx_blit(f, x + fr.xo, y + fr.yo);                 // sub_1FFFE
            };
            blit(f1, trail[15].x, trail[15].y);
            blit(f2, trail[12].x, trail[12].y);
            blit(f1, trail[9].x, trail[9].y);
            blit(f3, trail[6].x, trail[6].y);
            blit(f3, trail[3].x, trail[3].y);
            blit(f4, trail[0].x, trail[0].y);
            blit(f5, head_x, head_y);
            battle_fx_flush();                                           // sub_12E01
            battle_fx_commit();                                          // sub_5E9E8
        }

        if (should_exit_) break;
        if (radius != 0 || radius_step != 0) {                           // loc_24F72
            dir = kDirMap[dir & 7];
            wait_game_tick();                                            // sub_499E4
            continue;
        }
        break;
    }
    if (should_exit_) { battle_nsp_release(); return 1; }
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(5);                                                   // sub_1033B(5)
    for (int i = 0; i < 15 && !should_exit_; ++i) {                      // loc_24FCA (di<0Fh)
        const int f = (i % 6) + 0x0C;
        battle_fx_world_dim();                                           // sub_454B3 + sub_454D5
        if (f >= 0 && f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, ex + fr.xo, ey + fr.yo);                   // sub_1FFFE
        }
        battle_fx_flush();                                               // sub_12E01
        battle_fx_commit();                                              // sub_5E9E8
        wait_game_tick();                                                // sub_499E4
    }
    battle_nsp_release();                                                // sub_1FFCD
    cam_fine_x_ = CAM_FINE_X_REST;                                       // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                                       // dword_BA838 = 0
    return 1;
}

// sub_25047(eax/edx=caster tile, bl=variant): heal1.nsp shrinking ring (party heal).
//   Ten angles (0x40 apart) start at radius var_8=0xA0 and shrink by 1 (variant 1) or 4 (other)
//   per frame. Frame = radius%4 for variant 1, otherwise (radius/4)%4 (loc_250F1/loc_2510C).
//   Variant 1 also rotates each angle by +0x0A per frame (spinning ring). At radius 0:
//   4F4CD(0x32) -> 1033B(5) -> 15 frames of (i%6)+0Ch -> 1FFCD.
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
    int angles[10], radius = 0xA0;
    for (int i = 0; i < 10; ++i) angles[i] = i * kAngleStep;
    while (radius > 0 && !should_exit_) {                                // `cmp var_8,0; jle` (exit)
        // loc_250F1/loc_2510C: variant 1 uses radius%4, the others (radius/4)%4, so the
        // first four frames are 0,3,2,1 as the radius shrinks.
        const int f = variant1 ? (radius % 4) : ((radius / 4) % 4);
        for (int i = 0; i < 10; ++i) {
            int px, py;
            ring_point(angles[i], radius, cx, cy, px, py);
            if (f < nframes) battle_fx_blit(f, px, py);
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
    (void)tx; (void)ty; (void)k;
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
            ring_point(angles[i], radius, cx, cy, px, py);
            if (f < nframes) battle_fx_blit(f, px, py);
            angles[i] += 0x0A;
            if (angles[i] >= kAngleMax) angles[i] = 0;
        }
        radius += 2;                                                     // 454D5(var_8+2)
        battle_fx_present();
        wait_game_tick();
        if ((cy % 8) == 0) sfx(0x32);                                    // `idiv 8; test edx,edx` after 499E4
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
//   Keeps a 50-entry angle table (five groups of ten, 0x40 apart). Ring r starts when the
//   frame counter passes r*10, grows from radius 0 to 0xA0 by 7,5,5,5,5 and rotates +5 (ring 1)
//   or +4 (the others) per frame. Frame = 0x0A + (4 - radius/0x1E) per ring, i.e. 0x0E..0x09
//   as that ring expands. Runs 0x96 frames with 4F4CD(0x32) every 10.
int CombatII::battle_fx_25591(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
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
    static const int kGrow[kRings] = {7, 5, 5, 5, 5};                    // var_30/var_30+2/var_2C/...
    static const int kSpin[kRings] = {5, 4, 4, 4, 4};                    // ring 1 adds 5, the rest 4
    for (int t = 0; t < 0x96 && !should_exit_; ++t) {                    // `cmp bx,96h`
        for (int r = 0; r < kRings; ++r) {
            if (t <= r * 10) continue;                                   // loc_257A2 etc.: jle threshold
            // Frame is recomputed from this ring's current radius (loc_2567F/257A2/258C3/
            // 259E4/25B07): 0x0A + (4 - radius/0x1E).
            const int f = 0x0A + (4 - radius[r] / 0x1E);
            for (int i = 0; i < kPerRing; ++i) {
                int px, py;
                ring_point(angles[r * kPerRing + i], radius[r], cx, cy, px, py);
                if (f >= 0 && f < nframes) battle_fx_blit(f, px, py);
                angles[r * kPerRing + i] += kSpin[r];                    // angle +5 / +4
                if (angles[r * kPerRing + i] >= kAngleMax) angles[r * kPerRing + i] = 0;
            }
            radius[r] += kGrow[r];
            if (radius[r] > 0xA0) {                                      // loc_25787 etc.
                for (int i = 0; i < kPerRing; ++i) angles[r * kPerRing + i] = i * kAngleStep;
                radius[r] = 0;
            }
        }
        battle_fx_present();                                             // 454B3 + 454D5 + 5E9E8
        wait_game_tick();                                                // 499E4
        if ((t % 10) == 0) sfx(0x32);                                    // `idiv 0Ah` on the frame counter
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_261FC(eax/edx=caster tile, ebx=level): powerup.nsp attack-up effect.
//   Draws frame 0..3 once then 4 (unk_F0DDA) ONCE at the target centre + xo/yo with 5F305 tint.
//   Five embers at x = cx + dword_1FE5A[i] ({0,-10,10,5,-5}), y = cy - 0x28 + i*0x0A rise 4 px
//   per frame using 1FFFE(frame 5+k), wrapping below cy - 0x32 back to cy.
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
    static constexpr int kOffsetX[5] = {0, -10, 10, 5, -5};             // dword_1FE5A (ASM 22906)
    int spark_x[5], spark_y[5];
    for (int i = 0; i < 5; ++i) {
        spark_x[i] = cx + kOffsetX[i];                                   // var_24[i]
        spark_y[i] = cy - 0x28 + i * 0x0A;                               // var_18[i]
    }
    const int top = spark_y[0] - 0x0A;                                   // var_4 = cy - 0x32
    const int bottom = spark_y[4];                                       // var_8 = cy
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(4);                                                   // sub_1033B(4)
    const int shade = 0x28;                                              // 61147(eax=1, edx=28h)
    const int ember_f = 5 + (int)(int16_t)(k & 0xFFFF);                  // [esp+30h+var_32] sar 10h + 5
    for (int t = 0; t < 0x1E && !should_exit_; ++t) {                    // `cmp ax,1Eh`
        const int f = (t < 4) ? t : 4;                                   // fixed at +4*0Ch
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit_tinted(f, cx + fr.xo, cy + fr.yo, shade, {0x01}); // sub_5F305 once
        }
        for (int i = 0; i < 5; ++i) {
            if (ember_f >= 0 && ember_f < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)ember_f];
                battle_fx_blit(ember_f, spark_x[i] + fr.xo, spark_y[i] + fr.yo); // sub_1FFFE
            }
            spark_y[i] -= 4;
            if (spark_y[i] < top) spark_y[i] = bottom;
        }
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26391(eax/edx=caster tile, bl=variant): speedup.nsp falling embers (agility up).
//   1124A -> 11DB2/135EB(cx, cy) -> loc_263D1 computes py[0..2] via sub_10988(cy - i) + 4
//   (16px vertical spacing). 40 frames (0x28), 1033B(7), 4F4CD(0x32):
//     - main sprite k*6 + (t%6) at (cx, cy - t/2) alternates under (t%2==0) / over (t%2!=0) 12BEE
//     - 3 embers: py[i]++ (if old_py > cy: py[i] = cy + main.yo - t/2), frame 0x12 + py[i]%3 at
//       (cx + 4 - i*6, py[i]) via 1FFFE (+xo/yo), followed by 454D5 + 5E9E8 + 499E4.
int CombatII::battle_fx_26391(int unit_index, int tx, int ty, int k) {
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
    // ASM 30980-30996: sub_135EB stores cy in [esp+0], then loc_263D1 calls sub_10988(cy - i) + 4.
    int py[3];
    for (int i = 0; i < 3; ++i)
        py[i] = ((int)(int16_t)(cy - i) - (int)(int16_t)cam_tile_y_) * TOWN_TILE_H + (int)(int16_t)cam_fine_y_ + 8;
    game_timer_set(7);                                                   // sub_1033B(7)
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    const int variant = (int)(int8_t)(k & 0xFF);                         // `sar eax, 18h` on var_4
    for (int t = 0; t < 0x28 && !should_exit_; ++t) {                    // `cmp bx,28h`
        const int main_f = variant * 6 + (t % 6);
        int main_xo = 0, main_yo = 0;
        if (main_f >= 0 && main_f < nframes) {
            main_xo = magic_frames_[(size_t)main_f].xo;
            main_yo = magic_frames_[(size_t)main_f].yo;
            if ((t % 2) == 0)
                battle_fx_blit_under_units(main_f, cx + main_xo, cy - (t >> 1) + main_yo);
            else
                battle_fx_blit(main_f, cx + main_xo, cy - (t >> 1) + main_yo);
        }
        for (int i = 0; i < 3; ++i) {
            const int old_py = py[i];
            py[i] = old_py + 1;
            if (old_py > cy) py[i] = cy + main_yo - (t >> 1);
            const int f = 0x12 + (py[i] % 3);                            // `idiv 3; add 12h`
            if (f >= 0 && f < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)f];
                battle_fx_blit(f, cx + 4 - i * 6 + fr.xo, py[i] + fr.yo); // sub_1FFFE
            }
        }
        battle_hud_time_ = battle_time_;                                 // 454D5 frame time
        render_battle_map(false, false, true);                           // sub_10AEB + sub_12BEE
        battle_fx_flush();
        battle_fx_overlay();                                             // sub_454D5
        battle_fx_commit();                                              // sub_5E9E8
        wait_game_tick();                                                // sub_499E4
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26584(eax/edx=caster tile, ebx=direction (esi)): marmor.nsp magic-defence effect.
//   After 1124A it moves the target centre y by +6 and draws one frame per tick for 30 frames,
//   frame = direction*7 + (t%7) (direction*7 selects a 7-frame group per direction).
//   Each frame is sub_10AEB -> [sub_12BEE when t%7 < 5] -> sub_1FFFE -> [sub_12BEE when
//   t%7 >= 5] -> sub_454D5 -> sub_5E9E8, with no weather/message/drop passes.
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
        const int phase = t % 7;
        const int f = dir * 7 + phase;                                   // `imul eax,ebx(7); + dl`
        const bool units_after_effect = (phase >= 5);                    // `cmp dl,4; jbe`
        if (units_after_effect && f < nframes)
            battle_fx_blit_under_units(f, cx, cy);                       // sub_1FFFE before sub_12BEE
        render_battle_map(false, false, true);                           // sub_10AEB + sub_12BEE
        if (!units_after_effect) {
            if (f < nframes) battle_fx_blit(f, cx, cy);                  // sub_1FFFE over the units
            battle_fx_flush();                                           // sub_12E01
        }
        battle_hud_time_ = battle_time_;                                 // 454D5 frame time
        battle_fx_overlay();                                             // sub_454D5
        battle_fx_commit();                                              // sub_5E9E8
        wait_game_tick();                                                // sub_499E4
    }
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}

// sub_26672(eax/edx=caster tile, ebx=level): bless.nsp blessing effect.
//   Draws frame 0..3 once then 4 (unk_F0DDA) ONCE at the target centre + xo/yo with 5F305 tint.
//   Five embers at x = cx + dword_1FE64[i] ({0,-10,7,5,-5}), y = cy - 0x28 + i*0x0A fall +4 px
//   per frame using 5EE51(frame k*3 + 5 + (i%3)), wrapping > cy back to cy - 0x32.
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
    static constexpr int kOffsetX[5] = {0, -10, 7, 5, -5};              // dword_1FE64 (ASM 22908)
    int spark_x[5], spark_y[5];
    for (int i = 0; i < 5; ++i) {
        spark_x[i] = cx + kOffsetX[i];                                   // var_18[i]
        spark_y[i] = cy - 0x28 + i * 0x0A;                               // var_24[i]
    }
    const int top = spark_y[0] - 0x0A;                                   // edi = cy - 0x32
    const int bottom = spark_y[4];                                       // var_4 = cy
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    game_timer_set(4);                                                   // sub_1033B(4)
    const int shade = 0x28;                                              // 61147(eax=1, edx=28h)
    const int level = (int)(int16_t)(k & 0xFFFF);                        // [esp+30h+var_32] sar 10h
    for (int t = 0; t < 0x1E && !should_exit_; ++t) {
        const int f = (t < 4) ? t : 4;                                   // fixed at +4*0Ch
        if (f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit_tinted(f, cx + fr.xo, cy + fr.yo, shade, {0x01}); // sub_5F305 once
        }
        for (int i = 0; i < 5; ++i) {
            const int fr = level * 3 + 5 + (i % 3);                      // ASM 31295-31305
            if (fr >= 0 && fr < nframes) battle_fx_blit(fr, spark_x[i], spark_y[i]); // sub_5EE51
            spark_y[i] += 4;                                             // `add eax,4`
            if (spark_y[i] > bottom) spark_y[i] = top;                   // `cmp ax,dx; jle` -> edi
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
//   611AA(four-colour table, shade 0x28) -> 20021("death.rpl") -> 1033B(5) -> 601E5(0F0h, 0Eh)
//   -> 4F4CD(32h), then 48 frames (0x30):
//     - overlay frame 0 on the target centre with 1FFFE (plain) plus its xo/yo,
//     - a second frame = (t/2) for t<9 then fixed at 5 (unk_F0DE6 = table start+5*0Ch)
//       with 5F305 (tinted) plus its xo/yo,
//     - for t>19, while (9 - (t-20)/2) is at least 6, draw one extra shaking frame using
//       499BD(1)+6 with 1FFFE plus its xo/yo,
//     - 602E3 advances the DAC scroll slot one step before 454D5/5E9E8, so the rotated colours
//       are already on the presented frame.
//   602BF frees the slot after the loop without restoring the DAC (the 14-colour band stays in
//   its final position, as in the original).
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
    battle_rpl_apply("death.rpl");                                       // sub_20021
    game_timer_set(5);                                                   // sub_1033B(5)
    pal_scroll_begin(0xF0, 0x0E);                                        // sub_601E5(0F0h, 0Eh)
    sfx(0x32);                                                           // sub_4F4CD(0x32)
    const int shade = 0x28;                                              // 611AA(ebx=28h)
    for (int t = 0; t < 0x30 && !should_exit_; ++t) {                    // `cmp cx,30h`
        // Per-frame order (315B2..31693): 10AEB (map) -> 1FFFE frame 0 -> 12BEE (units)
        // -> 5F305 frame f2 -> [t>13h] 1FFFE shaking frame -> 454D5 -> 5E9E8. Frame 0 sits
        // below the unit sprites, so it goes through the under-units path instead of the
        // sprite queue; f2/f3 stay in the queue and are flushed between units and 454D5.
        if (0 < nframes) {
            const MagicFxFrame& fr = magic_frames_[0];
            battle_fx_blit_under_units(0, cx + fr.xo, cy + fr.yo);       // sub_1FFFE
        }
        const int f2 = (t < 9) ? (t / 2) : 5;                            // loc_26BF6: unk_F0DE6
        if (f2 < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f2];
            battle_fx_blit_tinted(f2, cx + fr.xo, cy + fr.yo, shade,
                                  {0x00, 0x03, 0x05, 0x07});             // sub_5F305
        }
        if (t > 0x13) {                                                  // `cmp cx,13h; jle`
            int f3 = 9 - (t - 0x14) / 2;                                 // shaking frame
            if (f3 < 6) f3 = 6 + combat_rand(1);                         // 499BD(1)+6
            if (f3 < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)f3];
                battle_fx_blit(f3, cx + fr.xo, cy + fr.yo);              // sub_1FFFE
            }
        }
        pal_scroll_step();                                               // sub_602E3
        battle_hud_time_ = battle_time_;                                 // 454D5 frame time
        // 26B1F's map pass is sub_10AEB + sub_12BEE only; it does not call sub_1547F.
        render_battle_map(false, false, true);                           // sub_10AEB + frame 0 + sub_12BEE
        battle_fx_flush();                                               // sub_5F305 + sub_1FFFE
        battle_fx_overlay();                                             // sub_454D5
        battle_fx_commit();                                              // sub_5E9E8
        wait_game_tick();                                                // sub_499E4
    }
    pal_scroll_end();                                                    // sub_602BF
    battle_nsp_release();                                                // sub_1FFCD
    return 1;
}
