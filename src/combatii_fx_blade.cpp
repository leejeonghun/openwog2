// Spell effect drivers: enchant (blade), barrier, DNA, summon, turn undead
//   The original routines sub_29273 / sub_2C00E / sub_2A1C6 / sub_2B5C4 / sub_2C468 / sub_19168
//   take a target tile (or unit) and call these subroutines. They all share one shape: load
//   MAGICSPR\<name>.nsp with 1FEA4 -> shorten the timer with 1033B -> per frame {redraw units and
//   screen (10AEB, 12BEE) -> 1FFFE (one frame) -> 454D5/454B3 + 5E9E8 -> 499E4} -> release with
//   1FFCD. This file maps that shape onto battle_fx_blit and battle_fx_present.
//
// Common approximations in the port:
//   - MAGICSPR frames are drawn with battle_fx_blit(frame, x, y) (sub_5EE51 inside). The original
//     1FFFE/1FFFC add the frame table entry's x/y offset, so magic_frames_[f].xo/.yo are added
//     here too, the same rule already used by 2016A/203CC.
//   - Not ported: sub_12BEE (redraw the unit in enchant colours), 10AEB (commit the camera),
//     61011/6104A/61147 (per-weapon palette ramps), 5F611/5F570/5F29D (expand, rotate and shrink
//     blitters), 602E3/602BF (palette scroll), 56129/56178/561F4 (particle engine), 20021 (.rpl),
//     103A8 and 12F2A. All of them only redraw a unit or change colours for that frame, so the
//     call sites keep a note and preserve only the draw order and frame placement.
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- Shared position math ----
// sub_10971(tx)+add_x / sub_10988(ty)+add_y: tile -> screen anchor, not the unit centre.
// 135EB (unit_screen_center) adds the sprite offset and centre on top of it.
namespace {

// One point of a trail.
struct FxPoint { int x = 0; int y = 0; };

} // namespace

// sub_13669(eax=unit, edx=colour): solid silhouette of the unit's pose-0 D66A8 frame via
//   5F517, at 10971(x)+0Ch / 10988(y)-2 plus the sprite entry [4]/[5] and the unit's [65h]/[66h].
//   When [unit+77h] != 0 and [unit+5Ch] <= 6 the stored record's sprite is filled at the same
//   live-unit anchor, using the live unit's facing/pose.
void CombatII::battle_fx_silhouette_13669(int unit_index, int color) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    auto draw = [&](int sprite_type) {
        const NC2Frame* fr = nc2_frame_for(sprite_type, u.facing & 3, u.anim_frame);
        if (!fr || fr->pixels.empty()) return;
        DrawEntry e{};
        e.x = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + u.walk_off_x;
        e.y = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y + u.walk_off_y;
        e.type = 3;                                   // sub_5F517 solid fill
        e.idx = color & 0xFF;
        e.nc2 = fr;
        blit_draw_entry(e);                           // adds fr->x_off/y_off
    };
    draw(u.sprite_type);
    if (u.stored_77 && u.code_b <= 6) draw(u.stored_77->sprite_type);
}

// sub_204E8(eax=ty, edx=tx, ebx=N, ecx=flag, [esp+4]=repeat count): "magic<N>.nsp" enchant.
//   Two callers:
//     - sub_29273+DD (X.Blade family): eax=y, edx=x, ebx=k+8, ecx=1, push 1
//     - sub_2C00E+95 (assist-d family): eax/edx = caster's own x/y, ebx=0Bh, ecx=1, push (var+1)
//   Flow: 1124A (camera) -> 11DB2/135EB (centre of the unit on that tile) -> name "magic" + itoa(N)
//   + ".nsp" loaded by 1FEA4 -> 4F4CD(32h) -> 1033B(0Ah) -> (repeat x frame count) times
//     {10AEB; 12BEE when flag==1; 1FFFE(frame = i % frame count); 12BEE when flag==0;
//      454D5; 5E9E8; 499E4} -> 1FFCD.
//   Port: N is k+8 from the handler sub-index (same as the X.Blade caller), repeat 1, flag 1.
//   12BEE/10AEB are not ported, so each frame's present_cutscene_frame stands in for them.
int CombatII::battle_fx_204e8(int unit_index, int tx, int ty, int k) {
    (void)unit_index;
    const int n = k + 8;                                    // magic8..magic10
    char name[32];
    std::snprintf(name, sizeof(name), "magic%d.nsp", n);
    if (!battle_nsp_load(name) || magic_frames_.empty()) return 0;   // sub_1FEA4
    camera_center_on(tx, ty);                               // sub_1124A(eax=tx, edx=ty)
    int cx = 0, cy = 0;
    const int ti = battle_unit_index_at(tx, ty);            // sub_11DB2
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy); // sub_135EB
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    sfx(0x32);                                              // sub_4F4CD(32h)
    game_timer_set(0x0A);                                   // sub_1033B(0Ah)
    const int nframes = (int)magic_frames_.size();
    const int total = std::min(0x400, nframes * 1);          // repeat count 1
    for (int i = 0; i < total && !should_exit_; ++i) {
        const int f = std::min(nframes - 1, i % nframes);
        battle_fx_blit(f, cx + magic_frames_[(size_t)f].xo, cy + magic_frames_[(size_t)f].yo);
        battle_fx_present();                                // sub_454D5 + sub_5E9E8
        wait_game_tick();                                   // sub_499E4
    }
    battle_nsp_release();                                   // sub_1FFCD
    if (battle_trace_) fprintf(stderr, "COMBAT: 204E8 magic%d unit-at=(%d,%d)\n", n, tx, ty);
    return 1;
}

// sub_2262C(eax=ty, edx=tx, ebx=N, ecx=flag, [esp+4]=repeat count): "magic<N>.nsp" anchored to
//   the tile. sub_29273 calls it with ebx = 5 / 6 / 7, ecx=1, push 2. Unlike 204E8 it does not
//   look up the occupying unit through 11DB2/135EB; the anchor is 10971(tx)+0Ch / 10988(ty)-2.
//   The rest is identical: 1FEA4 -> 4F4CD(32h) -> 1033B(0Ah) -> (repeat x frame count) -> 1FFCD.
int CombatII::battle_fx_2262c(int unit_index, int tx, int ty, int k) {
    (void)unit_index;
    const int n = k + 5;                                    // magic5..magic7
    char name[32];
    std::snprintf(name, sizeof(name), "magic%d.nsp", n);
    if (!battle_nsp_load(name) || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    const int ax = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;  // 10971+0Ch
    const int ay = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 2;   // 10988-2
    sfx(0x32);
    game_timer_set(0x0A);
    const int nframes = (int)magic_frames_.size();
    const int total = std::min(0x400, nframes * 2);          // push 2 (repeat count)
    for (int i = 0; i < total && !should_exit_; ++i) {
        const int f = std::min(nframes - 1, i % nframes);
        battle_fx_blit(f, ax + magic_frames_[(size_t)f].xo, ay + magic_frames_[(size_t)f].yo);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();
    if (battle_trace_) fprintf(stderr, "COMBAT: 2262C magic%d tile=(%d,%d)\n", n, tx, ty);
    return 1;
}

// sub_5637D(eax=tx, edx=ty, ebx=N?): fireblad.nsp particle vortex.
//   Prologue: 61011(0) / 61147(1,0x28) / 61011(1) set the weapon palette ramp (not ported).
//   1FEA4("fireblad.nsp") -> 1124A -> 11DB2/135EB (centre of the occupying unit) -> 1033B(5) ->
//   56129 (initialise particles).
//   (1) 0x14 (20) frames: {56178 (spawn 5 particles, angle = frame); 10AEB;
//       1FFFE(frame = (i % 0Eh) + 6); 12BEE; 454D5; 5E9E8; 499E4}
//   (2) 1033B(0Fh); 6 frames: {10AEB; 12BEE; 561F4(frame = i) in place of 1FFFE; 454D5; 5E9E8;
//       499E4}
//   (3) 1033B(0Ah); until the 20 particles reach frame 0Ah and var_4 becomes 0Fh: {draw every
//       particle with 561F4; update particle age/motion; 454D5; 5E9E8; 499E4}.
//   Port: (1) and (2) are ported as-is; 561F4 is 5F29D with the 61147(1,28h) LUT. (3) is still
//   a 30-frame approximation of the dword_112160 particle engine, now drawn through 561F4.
int CombatII::battle_fx_5637d(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    if (!battle_nsp_load("fireblad.nsp") || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    int cx = 0, cy = 0;
    const int ti = battle_unit_index_at(tx, ty);
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    const int nframes = (int)magic_frames_.size();
    auto blit = [&](int f) {
        f = std::max(0, std::min(nframes - 1, f));
        battle_fx_blit(f, cx + magic_frames_[(size_t)f].xo, cy + magic_frames_[(size_t)f].yo);
    };
    game_timer_set(5);                                      // sub_1033B(5)
    for (int i = 0; i < 0x14 && !should_exit_; ++i) {        // (1) 20 frames
        blit((i % 0x0E) + 6);                               // (i % 14) + 6
        battle_fx_present();
        wait_game_tick();
    }
    auto blit_561f4 = [&](int f) {                          // sub_561F4 -> sub_5F29D
        f = std::max(0, std::min(nframes - 1, f));
        battle_fx_blit_tinted_unclipped(f, cx + magic_frames_[(size_t)f].xo,
                                          cy + magic_frames_[(size_t)f].yo, 60);
    };
    game_timer_set(0x0F);                                   // (2) sub_1033B(0Fh)
    for (int i = 0; i < 6 && !should_exit_; ++i) {
        blit_561f4(i);                                      // 561F4(frame = i)
        battle_fx_present();
        wait_game_tick();
    }
    game_timer_set(0x0A);                                   // (3) particle afterimage (approximation)
    for (int i = 0; i < 30 && !should_exit_; ++i) {
        blit_561f4(i % 6);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();
    if (battle_trace_) fprintf(stderr, "COMBAT: 5637D fireblad tile=(%d,%d)\n", tx, ty);
    return 1;
}

// sub_22767(eax=ty, edx=tx): crystal.nsp, six crystals in orbit.
//   1FEA4("crystal.nsp", 0A0h) -> 1124A -> anchor 10971(tx)+0Ch / 10988(ty) -> 1033B(3) ->
//   edi (frame spacing counter) = 0A0h, -2 per frame, so 80 frames.
//   Each frame: angle += 14h (20), wrapping to 0 at 280h (640). Position = centre +
//   (cos,sin)(angle/100) * bdl_771D0 (= 160.0). The 6-slot trail shifts by one and six frames are
//   drawn (frames 0..5, skipped when x < 0FFFFFFFBh or off screen). When the loop ends (counter < 0)
//   the angle advances by 40h until it reaches 640, then after 1033B(1Eh) frames 0..5 are drawn at
//   the centre for six frames and 1FFCD runs.
int CombatII::battle_fx_22767(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    if (!battle_nsp_load("crystal.nsp") || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    const int cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12; // 10971+0Ch
    const int cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    const int nframes = (int)magic_frames_.size();
    game_timer_set(3);                                      // sub_1033B(3)
    sfx(0x32);                                              // sub_4F4CD(32h)
    const double kRadius = 160.0;                           // dbl_771D0
    const int kStep = 0x14, kWrap = 0x280;                  // angle unit = degrees * 100/360
    std::vector<FxPoint> trail(6, FxPoint{cx, cy});
    int angle = 0;
    for (int frame_no = 0; frame_no < 0x50 && !should_exit_; ++frame_no) {   // 0A0h / 2
        angle = (angle + kStep) % kWrap;
        const double rad = (double)angle / 100.0;           // flt_771CC = 100.0
        FxPoint p;
        p.x = cx + (int)std::lround(std::cos(rad) * kRadius);
        p.y = cy + (int)std::lround(std::sin(rad) * kRadius);
        // Shift the 6-slot trail (copy loop cx=4..0 at 22878)
        for (int s = (int)trail.size() - 1; s > 0; --s) trail[(size_t)s] = trail[(size_t)s - 1];
        trail[0] = p;
        for (int s = 5; s >= 0 && !should_exit_; --s) {
            const FxPoint& q = trail[(size_t)s];
            if (q.x <= -5 || q.y <= -5 || q.x >= SCREEN_W || q.y >= SCREEN_H) continue;
            const int f = std::min(nframes - 1, s);
            battle_fx_blit(f, q.x + magic_frames_[(size_t)f].xo, q.y + magic_frames_[(size_t)f].yo);
        }
        battle_fx_present();                                // sub_454B3 + sub_5E9E8
        wait_game_tick();
    }
    // loc_22951: step 40h until the angle reaches 280h, without drawing.
    while (angle != kWrap) angle = std::min(kWrap, angle + 0x40);
    game_timer_set(0x1E);                                   // sub_1033B(1Eh)
    for (int i = 0; i < 6 && !should_exit_; ++i) {           // loc_22979: six frames at the centre
        const int f = std::min(nframes - 1, i);
        battle_fx_blit(f, cx + magic_frames_[(size_t)f].xo, cy + magic_frames_[(size_t)f].yo);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                   // sub_1FFCD
    if (battle_trace_) fprintf(stderr, "COMBAT: 22767 crystal tile=(%d,%d)\n", tx, ty);
    return 1;
}

// sub_229CC(eax=tx, edx=ty): diamondb.nsp explosion.
//   1124A (camera) -> 1FEA4("diamondb.nsp") -> 1033B(0Ah) -> anchor 10971(tx)+0Ch /
//   10988(ty)+8 -> 4F4CD(32h) -> 26 frames (cx < 19h):
//     - for i > 7, frame (i&1)+7 at the anchor (alternating 7/8)
//     - for i > 0Ah, frame 9 + ((i-8) % 4); otherwise frame i+1
//     - 10AEB -> the frame above -> (12BEE when i <= 0Ah) -> 454D5 + 5E9E8 -> 499E4 -> 1FFCD
int CombatII::battle_fx_229cc(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    if (!battle_nsp_load("diamondb.nsp") || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    const int ax = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;  // 10971+0Ch
    const int ay = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;   // 10988+8
    game_timer_set(0x0A);                                   // sub_1033B(0Ah)
    sfx(0x32);                                              // sub_4F4CD(32h)
    const int nframes = (int)magic_frames_.size();
    auto blit = [&](int f) {
        f = std::max(0, std::min(nframes - 1, f));
        battle_fx_blit(f, ax + magic_frames_[(size_t)f].xo, ay + magic_frames_[(size_t)f].yo);
    };
    for (int i = 0; i < 0x1A && !should_exit_; ++i) {        // cx < 19h
        if (i > 7) blit((i & 1) + 7);                       // alternating 7/8
        if (i > 0x0A) blit(9 + ((i - 8) % 4));
        else blit(i + 1);
        battle_fx_present();                                // sub_454D5 + sub_5E9E8
        wait_game_tick();
    }
    battle_nsp_release();                                   // sub_1FFCD
    if (battle_trace_) fprintf(stderr, "COMBAT: 229CC diamondb tile=(%d,%d)\n", tx, ty);
    return 1;
}

// sub_2690B(bl=flag, eax=ty, edx=tx): barrier.nsp barrier.
//   20021("barrier.rpl") is not ported. 1FEA4("barrier.nsp") -> 1124A -> 11DB2/135EB (centre of
//   the occupying unit) -> 1033B(5) -> 601E5(0F0h) (reset the table scroll, not ported) ->
//   4F4CD(32h).
//   (1) edi = 3Ch decreasing by 4 (15 frames): {10AEB; 5F611 (expand blitter, not ported);
//       12BEE; 454D5; 5E9E8; 602E3; 499E4}
//   (2) when flag > 0: edi = 2 growing by (flag*4) up to 3Ch*flag, drawn with 5F570 (shrink
//       blitter, not ported)
//   (3) when flag == 0: one more frame through 1FFFE (loc_26AB6)
//   (4) 30 frames of 602E3 (palette flash) -> 602BF -> 1FFCD.
//   Port: 5F611 is ported in combatii_fx_gfx.cpp and drawn by battle_fx_blit_scaled with the radius
//   r as the target size. The 602E3 flash is replaced by a plain frame render.
int CombatII::battle_fx_2690b(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    if (!battle_nsp_load("barrier.nsp") || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    int cx = 0, cy = 0;
    const int ti = battle_unit_index_at(tx, ty);
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    game_timer_set(5);                                      // sub_1033B(5)
    sfx(0x32);                                              // sub_4F4CD(32h)
    const int nframes = (int)magic_frames_.size();
    // (1) edi = 0x3C, -4 per frame (15 frames)
    for (int r = 0x3C, i = 0; r > 0 && !should_exit_; r -= 4, ++i) {
        const int f = std::min(nframes - 1, i);
        const int half = r / 2;
        // sub_5F611(eax=centre-r/2+xo, edx=centre-r/2+yo, ecx=r, push r, ebx=frame)
        battle_fx_blit_scaled(f, cx - half + magic_frames_[(size_t)f].xo,
                                 cy - half + magic_frames_[(size_t)f].yo, r, r);
        battle_fx_present();                                // sub_454D5 + sub_5E9E8
        wait_game_tick();
    }
    // (2) the flag > 0 variant shrinks with 5F570 up to flag*3Ch in the original. The port's
    //    callers pass no flag, so it is omitted (use k as the flag if it is ever needed).
    // (4) 30 frames of 602E3 (palette flash, replaced by a frame render)
    for (int i = 0; i < 0x1E && !should_exit_; ++i) {
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                   // sub_1FFCD
    if (battle_trace_) fprintf(stderr, "COMBAT: 2690B barrier tile=(%d,%d)\n", tx, ty);
    return 1;
}

// sub_275A4(bl=flag, eax=ty, edx=tx): dna.nsp transformation.
//   61147(1, 28h) (palette ramp, not ported) -> 1124A -> 11DB2/135EB (centre of the occupying
//   unit) -> 1FEA4("dna.nsp") -> 1033B(4) -> 4F4CD(32h).
//   30 frames (cx < 1Eh): {454B3; frame = (cx < 4) ? cx : 4 (unk_F0DDA = table + 30h = frame 4);
//     draw that frame through 5F305(x = centre+xo, y = centre+yo) in colour-transform mode; when
//     cx > 5 also draw 1FFFE(frame = 5 + ([unit+3]>>24)*6 + (cx % 6)) at the tile position;
//     5E9E8; 499E4} -> 1FFCD (shared loc_268FA).
//   Port: 5F305 is battle_fx_blit_tinted(frame, x, y, 100) (tint 100% keeps the palette), and the
//   extra 1FFFE is one more battle_fx_blit of the same frame.
int CombatII::battle_fx_275a4(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    if (!battle_nsp_load("dna.nsp") || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    int cx = 0, cy = 0;
    const int ti = battle_unit_index_at(tx, ty);
    if (ti >= 0 && (size_t)ti < units_.size()) unit_screen_center(units_[(size_t)ti], cx, cy);
    else {
        cx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
        cy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 8;
    }
    game_timer_set(4);                                      // sub_1033B(4)
    sfx(0x32);                                              // sub_4F4CD(32h)
    const int nframes = (int)magic_frames_.size();
    for (int i = 0; i < 0x1E && !should_exit_; ++i) {
        const int f = std::min(nframes - 1, (i < 4) ? i : 4);        // unk_F0DDA = frame 4
        battle_fx_blit_tinted(f, cx + magic_frames_[(size_t)f].xo,
                                 cy + magic_frames_[(size_t)f].yo, 100);   // sub_5F305
        if (i > 5) {                                                 // extra 1FFFE after loc_2769F
            const int g = std::min(nframes - 1, 5 + (i % 6));
            battle_fx_blit(g, cx + magic_frames_[(size_t)g].xo,
                              cy + magic_frames_[(size_t)g].yo);
        }
        battle_fx_present();                                // sub_5E9E8
        wait_game_tick();
    }
    battle_nsp_release();
    if (battle_trace_) fprintf(stderr, "COMBAT: 275A4 dna tile=(%d,%d)\n", tx, ty);
    return 1;
}

// sub_272AD(eax=ty, edx=tx, ebx=?): soulreca.nsp summon landing.
//   1FEA4("soulreca.nsp", 0A0h) -> 1124A -> anchor 10971(tx)+0Ch / 10988(ty)+2 -> 1033B(3) ->
//   initialise the 21-slot trail -> 4F4CD(32h).
//   Every frame: 454B3; while the progress counter (var_10) is > -2Ah
//     {angle += 14h (20); radius var_C; position = centre + (cos,sin)(angle/100)*radius;
//      shift the 21-slot trail; draw every third slot with 5EFB9 (scale blitter); 5E9E8; 499E4;
//      counter -= 2}
//   Once the counter reaches -2Ah, frames are drawn from top to bottom 30 times (5EFB9), then
//   284A3 (sound-stop family, not ported) -> 1FFCD.
//   Port: 5EFB9 (shrink/rotate blitter) is not ported and becomes battle_fx_blit, and the radius
//   shrinks from 160 to 0 over the frames to imitate the contraction.
int CombatII::battle_fx_272ad(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    if (!battle_nsp_load("soulreca.nsp") || magic_frames_.empty()) return 0;
    camera_center_on(tx, ty);                               // sub_1124A
    const int ax = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;  // 10971+0Ch
    const int ay = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 2;   // 10988+2
    game_timer_set(3);                                      // sub_1033B(3)
    sfx(0x32);                                              // sub_4F4CD(32h)
    const int nframes = (int)magic_frames_.size();
    const int kSlots = 0x15;                                // 21
    std::vector<FxPoint> trail((size_t)kSlots, FxPoint{ax, ay});
    int angle = 0;
    int descend = 0;                                        // high word of var_10 (0 -> -2Ah)
    for (int guard = 0; guard < 0x200 && descend > -0x2A && !should_exit_; ++guard) {
        angle = (angle + 0x14) % 0x280;
        const double rad = (double)angle / 100.0;           // flt_77325 = 100.0
        const double radius = 160.0 * (1.0 - (double)(descend + 0x2A) / (double)0x2A);
        FxPoint p;
        p.x = ax + (int)std::lround(std::cos(rad) * radius);
        p.y = ay + (int)std::lround(std::sin(rad) * radius);
        for (int s = kSlots - 1; s > 0; --s) trail[(size_t)s] = trail[(size_t)s - 1];
        trail[0] = p;
        for (int s = kSlots - 1; s >= 0; s -= 3) {          // every third slot (loc_274EE)
            const FxPoint& q = trail[(size_t)s];
            const int f = std::min(nframes - 1, (int)((kSlots - 1 - s) / 3) % nframes);
            battle_fx_blit(f, q.x + magic_frames_[(size_t)f].xo, q.y + magic_frames_[(size_t)f].yo);
        }
        battle_fx_present();                                // sub_454B3 + sub_5E9E8
        wait_game_tick();
        descend -= 2;                                       // counter -= 2
    }
    // loc_27552: draw frames from top to bottom 30 times (cx = esi-1Eh .. esi)
    const int top = ay - 0x1E;
    for (int yy = top; yy <= ay && !should_exit_; yy += 2) {
        const int f = std::min(nframes - 1, 0);             // the original fixes frame 0 ([var_8+2])
        battle_fx_blit(f, ax + magic_frames_[(size_t)f].xo, yy + magic_frames_[(size_t)f].yo);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();                                   // sub_1FFCD
    if (battle_trace_) fprintf(stderr, "COMBAT: 272AD soulreca tile=(%d,%d)\n", tx, ty);
    return 1;
}

// sub_59675(eax=unit): turn undead; one target unit is struck by a bstar.nsp star and collapses.
//   1FEA4("bstar.nsp", 46h) -> 103A8 (not ported) -> 61147(1,?) (palette ramp, not ported) ->
//   [unit+64h] = 0Ah (hit pose) -> anchor 10971([unit])+0Ch / 10988([unit])-6 -> 1033B(0Fh).
//   (1) 13 frames (cx <= 0Ch): {10AEB; 5EFB9(frame cx, anchor, scale); 12BEE; 12F2A(unit);
//       5E9E8; 499E4}
//   (2) 4F4CD(32h) -> var_28 = 0Dh (frame), descent start var_30 = anchor y+0Ch,
//       var_24 = anchor y-0BCh
//   (3) descent: 5 frames (13..17) + 15 hold frames (18..32) + 4 closing frames (33..36).
//       Each frame draws frame 12 through 5EFB9, one 5F29D frame at y=anchor+0Ch, and 25
//       5F305 rows from anchor+0Ch down to anchor-0BCh (step -8, last row excluded).
//   5EFB9/5F29D/5F305 are all 1:1 blitters; 5EFB9 clips at 13Fh/C7h, 5F29D does not and
//   applies the 61147 LUT, and 5F305 clips like 5EFB9. No scale/rotation is involved.
int CombatII::battle_fx_59675(int target_index) {
    if (target_index < 0 || (size_t)target_index >= units_.size()) return 0;
    auto& u = units_[(size_t)target_index];
    if (!battle_nsp_load("bstar.nsp") || magic_frames_.empty()) return 0;
    weather_tick_enabled_ = false;                         // sub_103A8
    u.anim_frame = 0x0A;                                    // [unit+64h] = 0Ah
    const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
    const int ax = (ux - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;  // 10971+0Ch
    const int ay = (uy - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 6;   // 10988-6
    const int nframes = (int)magic_frames_.size();
    game_timer_set(0x0F);                                   // sub_1033B(0Fh)
    auto frame_ptr = [&](int f) -> const MagicFxFrame* {
        return (f >= 0 && f < nframes) ? &magic_frames_[(size_t)f] : nullptr;
    };
    auto clipped = [&](int f, int x, int y) {               // sub_5EFB9
        if (const MagicFxFrame* e = frame_ptr(f))
            battle_fx_blit_clipped(f, x + e->xo, y + e->yo);
    };
    auto tinted_u = [&](int f, int x, int y) {              // sub_5F29D
        if (const MagicFxFrame* e = frame_ptr(f))
            battle_fx_blit_tinted_unclipped(f, x + e->xo, y + e->yo, 100);
    };
    auto column = [&](int f, int y0, int bottom) {          // sub_5F305, 25 rows
        const MagicFxFrame* e = frame_ptr(f);
        if (!e) return;
        for (int yy = y0; yy > bottom; yy -= 8)
            battle_fx_blit_tinted(f, ax + e->xo, yy + e->yo, 100);
    };
    // (1) 13 frames as the star lands and stays above the unit
    for (int i = 0; i <= 0x0C && !should_exit_; ++i) {
        clipped(i, ax, ay);
        battle_fx_present();                                // sub_5E9E8 (stands in for 12BEE/12F2A)
        wait_game_tick();
    }
    if (should_exit_) { battle_nsp_release(); return 1; }
    sfx(0x32);                                              // sub_4F4CD(32h)
    // (3) 5 frames 13..17, 15 hold frames 18..32, then 4 closing frames 33..36
    const int base_y = ay + 0x0C;                           // var_30
    const int bottom = ay - 0xBC;                           // var_24
    for (int k = 0; k < 5 && !should_exit_; ++k) {
        clipped(12, ax, ay);                                // fixed F0E3E/F0E42 frame
        tinted_u(13 + k, ax, base_y);
        column(18 + k, base_y, bottom);
        battle_fx_present();
        wait_game_tick();
    }
    for (int n = 0; n < 15 && !should_exit_; ++n) {         // loc_59863 hold
        clipped(12, ax, ay);
        tinted_u(17, ax, base_y);
        column(22, base_y, bottom);
        battle_fx_present();
        wait_game_tick();
    }
    for (int k = 0; k < 4 && !should_exit_; ++k) {          // loc_59943 closing sweep
        clipped(12 - k, ax, ay);
        tinted_u(16 - k, ax, base_y);
        column(21 - k, base_y, bottom);
        battle_fx_present();
        wait_game_tick();
    }
    if (!should_exit_) {
        present_cutscene_frame();                           // sub_45559 (454B3 + 5E9E8)
        wait_game_tick();
        wait_game_tick();
    }
    weather_tick_enabled_ = true;                           // sub_103A0
    reset_unit_pose(u);                                     // sub_12124
    battle_nsp_release();                                   // sub_1FFCD
    if (battle_trace_) fprintf(stderr, "COMBAT: 59675 bstar unit=%d\n", u.id);
    return 1;
}
