// thundwav.nsp lightning wave: sub_23ABA / sub_23C36 / sub_23F3A
// ASM: dasm/COMBATII_code.ASM
//   sub_23ABA  27702..27841   (sub_29722 +22F)
//   sub_23C36  27842..28100   (sub_29722 +238)
//   sub_23F3A  28101..28336   (sub_29722 +241)
// All three are called from the sub_29722 thunder row with eax = caster unit. Common shape:
//   1FEA4("thundwav.nsp")  -> load the MAGICSPR table (unk_F0DAA)
//   200DE(unit, n)         -> 1124A(caster tile + n tiles towards the facing)  [camera]
//   1033B(5)               -> 5 game ticks per frame
//   frame loop             -> 454B3 -> 1FFFE (two frames) -> 5E9E8 -> 499E4,
//                             4F4CD(32h) every 10 frames
//   1FFCD                  -> release the table
// Facing offsets (shared off_23A9A / off_23AAA / off_23F2A):
//   0 south y += 10h, 1 west x -= 18h, 2 north y -= 10h, 3 east x += 18h
//
// Port mapping: 1FEA4 -> battle_nsp_load, 1FFFE/5EE51 -> battle_fx_blit (the renderer drops
//   off-screen pixels, standing in for the 5EFB9 clip blit), 454B3 + 5E9E8 -> battle_fx_present,
//   499E4 -> wait_game_tick, 1033B -> game_timer_set, 4F4CD -> sfx, 1FFCD -> battle_nsp_release,
//   1124A -> camera_center_on, 10971/10988 -> tile coordinates from TILE_ANCHOR_X/Y.
//
// The overlapped dword reads in 23C36/23F3A decode to the named word locals:
//   23C36 base tiles: south (tx-1,ty+1), west (tx-6,ty-1), north (tx-1,ty-6), east (tx+1,ty-1).
//   23F3A starts from a T of three bolts around the caster (all three entries start at the
//   caster tile, then the facing branch at 23F9C offsets them by one pixel).
//   The first loop frame does play the sfx: the original's `idiv 10; test edx` is 0 there too.
#include "combatii.h"
#include "combatii_internal.h"

namespace {

// 200DE(eax=unit, edx=n): centre the camera n tiles ahead of the caster along the facing.
inline void thundwav_camera(int facing, int tx, int ty, int n,
                            int& out_tx, int& out_ty) {
    out_tx = tx;
    out_ty = ty;
    switch (facing & 3) {
        case 0: out_ty += n; break;   // south
        case 1: out_tx -= n; break;   // west
        case 2: out_ty -= n; break;   // north
        default: out_tx += n; break;  // east
    }
}

} // namespace

// sub_23ABA (eax = caster): draws two thundwav.nsp frames (f%6 and f+6) for 54 frames (0x36),
//   pushing them 10h/18h towards the facing every 9 frames.
//   Base point is (10971(unit.x)+0Ch, 10988(unit.y)) with the facing offset applied once up
//   front and again every 9 frames inside the loop (off_23A9A/off_23AAA).
int CombatII::battle_fx_23aba(int unit_index, int tx, int ty, int k) {
    (void)k; (void)tx; (void)ty;   // the original also uses the caster tile ([unit+0]/[unit+1])
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("thundwav.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    int cam_tx = 0, cam_ty = 0;
    thundwav_camera(facing, (int)(int8_t)c.x, (int)(int8_t)c.y, 3, cam_tx, cam_ty);
    camera_center_on(cam_tx, cam_ty);                                     // sub_200DE(unit, 3)
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    int sx = scr_x((int)(int8_t)c.x) + 0x0C;                               // add esi,0Ch
    int sy = scr_y((int)(int8_t)c.y);
    auto advance = [&]() {                                                 // off_23A9A
        switch (facing) {
            case 0: sy += 0x10; break;
            case 1: sx -= 0x18; break;
            case 2: sy -= 0x10; break;
            default: sx += 0x18; break;
        }
    };
    advance();                                                             // first offset
    game_timer_set(5);                                                     // sub_1033B(5)
    int sub9 = 0;                                                          // var_8
    for (int f = 0; f < 0x36 && !should_exit_; ++f) {                      // cmp cx,36h
        const int fa = f % 6;                                              // idiv 6 remainder
        const int fb = f + 6;                                              // lea ebx,[ecx+6]
        if (fa < nframes) battle_fx_blit(fa, sx, sy);                      // sub_1FFFE
        if (fb < nframes) battle_fx_blit(fb, sx, sy);
        if (++sub9 == 9) { sub9 = 0; advance(); }                           // cmp dx,9
        battle_fx_present();                                               // 454B3 + 5E9E8
        wait_game_tick();                                                  // 499E4
        if (f % 10 == 0) sfx(0x32);                                        // idiv 0Ah
    }
    battle_nsp_release();                                                  // sub_1FFCD
    return 1;
}

// sub_23C36 (eax = caster): draws a six-point lightning wall from thundwav.nsp for 54 frames.
//   The six points are rebuilt per facing every 3 frames. All six points draw the same two
//   frames (phase and phase+6) at the same spot; the 3-frame phase counter cycles 0..2.
//   The stage counter (di) increments every 3 frames; every 6 stages the wall slides 24/16 px
//   perpendicular to the facing.
//   - south(0)/north(2): vertical six points (y = +4 + i*10h), x slide (di/6)*18h
//   - west(1)/east(3): horizontal six points (x = +0Ch + i*18h), y slide (di/6)*10h
int CombatII::battle_fx_23c36(int unit_index, int tx, int ty, int k) {
    (void)k; (void)tx; (void)ty;   // base tile is the caster's ([unit+0]/[unit+1])
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("thundwav.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    int cam_tx = 0, cam_ty = 0;
    thundwav_camera(facing, (int)(int8_t)c.x, (int)(int8_t)c.y, 4, cam_tx, cam_ty);
    camera_center_on(cam_tx, cam_ty);                                     // sub_200DE(unit, 4)
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    game_timer_set(5);                                                     // sub_1033B(5)
    const int bx = (int)(int8_t)c.x, by = (int)(int8_t)c.y;
    int xs[6] = {0}, ys[6] = {0};
    int phase = 0;                                                         // var_10 (0..2)
    int stepi = 0;                                                         // di
    for (int f = 0; f < 0x36 && !should_exit_; ++f) {                      // cmp cx,36h
        if (phase == 0) {                                                  // var_10 == 0
            const int slide = (stepi / 6) * 0x18;                          // (di/6)*18h  (x)
            const int slide_y = (stepi / 6) * 0x10;                        // (di/6)*10h  (y)
            switch (facing) {
                case 0:                                                    // loc_23CCF: var_C (tx-1), var_20 (ty+1)
                    for (int i = 0; i < 6; ++i) {
                        xs[i] = scr_x(bx - 1) + slide + 0x0C;
                        ys[i] = scr_y(by + 1) + i * 0x10 + 4;
                    }
                    break;
                case 1:                                                    // loc_23D26: var_18 (tx-6), var_24 (ty-1)
                    for (int i = 0; i < 6; ++i) {
                        xs[i] = scr_x(bx - 6) + i * 0x18 + 0x0C;
                        ys[i] = scr_y(by - 1) + slide_y + 4;
                    }
                    break;
                case 2:                                                    // loc_23D84: var_C (tx-1), var_14 (ty-6)
                    for (int i = 0; i < 6; ++i) {
                        xs[i] = scr_x(bx - 1) + slide + 0x0C;
                        ys[i] = scr_y(by - 6) + i * 0x10 + 4;
                    }
                    break;
                default:                                                   // loc_23DD7: var_1C (tx+1), var_24 (ty-1)
                    for (int i = 0; i < 6; ++i) {
                        xs[i] = scr_x(bx + 1) + i * 0x18 + 0x0C;
                        ys[i] = scr_y(by - 1) + slide_y + 4;
                    }
                    break;
            }
        }
        for (int j = 0; j < 6; ++j) {                                      // loc_23E4C
            const int fa = phase % 6;                                      // esi = var_10 (0..2)
            const int fb = phase + 6;                                      // add esi,6
            if (fa < nframes) battle_fx_blit(fa, xs[j], ys[j]);            // sub_1FFFE
            if (fb < nframes) battle_fx_blit(fb, xs[j], ys[j]);
        }
        if (++phase == 3) { phase = 0; ++stepi; }                          // loc_23ECC
        battle_fx_present();
        wait_game_tick();
        if (f % 10 == 0) sfx(0x32);                                        // idiv 0Ah
    }
    battle_nsp_release();
    return 1;
}

// sub_23F3A (eax = caster): rolls three thundwav.nsp bolts for 90 frames (0x5A).
//   Each bolt overlays frame (i+move)%6 and move+6 (the second at y+3), where move is the
//   9-frame counter (0..8); every 9 frames all three bolts move 10h/18h towards the facing,
//   stopping at the screen limits: south <= 0A8h, west >= 30h, north >= 20h, east <= 110h.
int CombatII::battle_fx_23f3a(int unit_index, int tx, int ty, int k) {
    (void)k; (void)tx; (void)ty;   // base tile is the caster's ([unit+0]/[unit+1])
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("thundwav.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();
    const CombatUnitPlacement& c = units_[(size_t)unit_index];
    const int facing = (int)(uint8_t)c.facing & 3;
    int cam_tx = 0, cam_ty = 0;
    thundwav_camera(facing, (int)(int8_t)c.x, (int)(int8_t)c.y, 5, cam_tx, cam_ty);
    camera_center_on(cam_tx, cam_ty);                                     // sub_200DE(unit, 5)
    auto scr_x = [&](int t) {
        return (t - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    };
    auto scr_y = [&](int t) {
        return (t - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    };
    game_timer_set(5);                                                     // sub_1033B(5)
    const int bx = (int)(int8_t)c.x, by = (int)(int8_t)c.y;
    // Initial T-shaped three-bolt layout (23F6B copies the caster tile into all three x/y
    // entries; the facing branch at 23F9C then offsets them).
    int tx3[3] = {bx, bx, bx};
    int ty3[3] = {by, by, by};
    switch (facing) {
        case 0: tx3[0] = bx;     ty3[0] = by + 1;
                tx3[1] = bx - 1; ty3[1] = by;
                tx3[2] = bx + 1; ty3[2] = by; break;      // loc_23FBC
        case 1: tx3[0] = bx - 1; ty3[0] = by;
                tx3[1] = bx;     ty3[1] = by - 1;
                tx3[2] = bx;     ty3[2] = by + 1; break;  // loc_23FD2
        case 2: tx3[0] = bx;     ty3[0] = by - 1;
                tx3[1] = bx - 1; ty3[1] = by;
                tx3[2] = bx + 1; ty3[2] = by; break;      // loc_23FE8
        default: tx3[0] = bx + 1; ty3[0] = by;
                tx3[1] = bx;     ty3[1] = by - 1;
                tx3[2] = bx;     ty3[2] = by + 1; break;  // loc_23FFE
    }
    int xs[3], ys[3];
    for (int i = 0; i < 3; ++i) {
        xs[i] = scr_x(tx3[i]) + 0x0C;                 // loc_24014: 10971 + 0Ch
        ys[i] = scr_y(ty3[i]) - 2;                    // 10988 - 2
    }
    int stepi = 0;                                                         // var_C
    int move_t = 0;                                                        // var_4
    for (int f = 0; f < 0x5A && !should_exit_; ++f) {                      // cmp si,5Ah
        for (int j = 0; j < 3; ++j) {                                      // loc_24067
            const int fa = (j + move_t) % 6;                               // lea edx,[ecx+ebp]
            const int fb = move_t + 6;                                     // lea eax,[ebp+6]
            if (fa < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fa];
                battle_fx_blit_clipped(fa, xs[j] + fr.xo, ys[j] + fr.yo);  // sub_5EFB9
            }
            if (fb < nframes) {
                const MagicFxFrame& fr = magic_frames_[(size_t)fb];
                battle_fx_blit_clipped(fb, xs[j] + fr.xo, ys[j] + 3 + fr.yo);
            }
        }
        if (++move_t == 9) {                                               // cmp si,9
            move_t = 0;
            switch (facing) {                                              // off_23F2A
                case 0:                                                    // south: ys[0] <= 0A8h
                    if (ys[0] <= 0xA8) for (int j = 0; j < 3; ++j) ys[j] += 0x10;
                    break;
                case 1:                                                    // west: xs[0] >= 30h
                    if (xs[0] >= 0x30) for (int j = 0; j < 3; ++j) xs[j] -= 0x18;
                    break;
                case 2:                                                    // north: ys[0] >= 20h
                    if (ys[0] >= 0x20) for (int j = 0; j < 3; ++j) ys[j] -= 0x10;
                    break;
                default:                                                   // east: xs[0] <= 110h
                    if (xs[0] <= 0x110) for (int j = 0; j < 3; ++j) xs[j] += 0x18;
                    break;
            }
            ++stepi;                                                       // inc edi
        }
        battle_fx_present();
        wait_game_tick();
        if (f % 10 == 0) sfx(0x32);
    }
    battle_nsp_release();
    return 1;
}
