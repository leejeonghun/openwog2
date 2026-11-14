// Spell effect drivers: Meteor 3rd stage (sub_570A1), spear fall (sub_2DA5E), summon (sub_21944)
//
// These three NSP/SSP effects follow the conventions of combatii_fx_support.cpp and
// combatii_fx_fire.cpp:
//   sub_1FEA4           -> battle_nsp_load("<file>.nsp"); appending uses the local lambda here
//   sub_1FFFE / sub_5EE51 -> battle_fx_blit(frame, x + frame.xo, y + frame.yo)
//   sub_5F305 / sub_5F29D -> battle_fx_blit_tinted / battle_fx_blit_tinted_unclipped
//   sub_454B3 + sub_5E9E8 -> battle_fx_present()
//   sub_454D5           -> covered by the renderer's overlay pass
//   sub_499E4           -> wait_game_tick()
//   sub_1033B           -> game_timer_set()
//   sub_499BD / rand_    -> combat_rand()
//   sub_4F4CD           -> sfx()
//   sub_4F460           -> battle_ssp_play()
//   sub_1FFCD           -> battle_nsp_release()
//   sub_1124A / sub_135EB -> camera_center_on(tx, ty) / unit_screen_center(units_[i], x, y)
//
// ---- Approximations ----
//  - sub_56217(x, y, entry) blits at (x + entry[4], y + entry[6]); fx_blit_f does the same by
//    adding the frame's xo/yo.
//  - sub_5F093 (solid silhouette blit) is battle_fx_blit_silhouette; the trail colours are the
//    three bytes at loc_2CA21+1 (0x11/0x13/0x15), indexed by trail slot.
//  - sub_2D4EB (480-stride canvas offset) has no equivalent because the port renders straight
//    into the backbuffer; its two pose frames are kept.
//  - sub_44D6E / sub_44D67 / sub_44D75 (effect-unit highlight toggles) are handled by the port's
//    render pass.
//  - sub_21944: IDA overlaps var_20/var_24/var_2E as dwords, so the table-entry offset
//    expression is ambiguous. Only the constants that read cleanly are used: 9 rise frames,
//    the h-row loop, frames 9+(r%5) / 0x0E+(r%5) / 0x13+(r%5), 16px spacing, the 8-fold
//    plume, game_timer_set(7)/(0x1E), sfx(0x32)/(0x33) and D66A8 [0]=tile width / [3]=height.
//    Offsets that appeared only through the overlapped locals are left at 0.
#include "combatii.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---- Effect particle table (original dword_112160: 100 slots x 26 bytes, sub_56129/56178/561E5)
// ----
// Field offsets match the original: +0 x, +2 y, +4 state, +6 age, +8 base frame,
// +0A cycle, +0C frame.
struct ProParticle {
    int16_t x = 0, y = 0;
    int16_t state = 0;
    int16_t age = 0;
    int16_t base = 0;
    int16_t cycle = 0;
    int16_t frame = 0;
    int16_t pad = 0;
};
constexpr int kProSlots = 100;      // 0x64
ProParticle g_pro[kProSlots];

// sub_56129: clear all 100 slots.
void pro_particles_init() {
    for (int i = 0; i < kProSlots; ++i) g_pro[i] = ProParticle{};
}

// sub_56178(slot, x, y, base frame, cycle, state): spawn one particle.
void pro_spawn(int idx, int x, int y, int base, int cycle, int state) {
    if (idx < 0 || idx >= kProSlots) return;
    ProParticle& p = g_pro[idx];
    p.x = (int16_t)x;
    p.y = (int16_t)y;
    p.state = (int16_t)state;
    p.age = 0;
    p.base = (int16_t)base;
    p.cycle = (int16_t)cycle;
    p.frame = 0;
    p.pad = 0;
}

// sub_561E5: switch a slot off.
void pro_kill(int idx) {
    if (idx < 0 || idx >= kProSlots) return;
    g_pro[idx].state = 0;
}

}  // namespace

// ---- sub_21944 (dasm 25067..25288): shared summon/appear effect ----
// eax = screen x, edx = screen y, ebx = unit image slot ([unit+0x60]).
// Reads [0] = tile width, [3] = height h, [4]/[5] = sprite offsets and [9] = the unit's
// span stream from the D66A8 entry:
//   (1) game_timer_set(7), then for 9 frames raise table frames 0..8 at
//       (x + (tile_w > 1 ? 0x0C : 0), y)
//   (2) sfx(0x32), then an h-row loop on the same 7-tick period (no second 1033B): per row
//       present (sub_454B3), blit the main frame
//       (last row = entry 7 = unk_F0DFE), draw the summon body with sub_5F4AF (ecx = r+1 rows),
//       and only when this is not the last row the secondary frame and the 8-fold plume.
//   (3) game_timer_set(0x1E), one wait, sfx(0x33), sub_284A3 (white palette flash: 49A3E
//       step 2 in, 100h back = one hold frame), then 44D6E(0). The caller releases the table.
// Coordinates: sub_10971/10988 already include the +4 tile anchor and the callers add 0Ch to
// both axes, so the port uses those raw values instead of unit_screen_center's sprite centre.
int CombatII::battle_fx_21944(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];
    uint8_t raw[13] = {};
    if (!battle_d66a8_raw(u.sprite_type, (int)u.facing & 3, 0, raw)) return 0;
    const int tile_w = raw[0];                       // [edx]
    const int h = raw[3];                            // [edx+3]
    // sub_21944 never loads an NSP itself: the caller (299DA/3C548/4426D) has already loaded
    // the table (unk_F0DAA), so the port reuses the current magic_frames_.
    if (magic_frames_.empty()) return 0;
    // The raised frames use the D66A8 sprite stream directly (sub_5F4AF).
    const NC2Frame* body = nc2_frame_for(u.sprite_type, (int)u.facing & 3, 0);
    // sub_21944+1B..+2B: sub_44D6E(1, [60h]); the matching sub_44D6E(0) is at the tail.
    struct DimGuard {
        bool& dim;
        explicit DimGuard(bool& d) : dim(d) { dim = true; }
        ~DimGuard() { dim = false; }
    } dim_guard(battle_dim_row0_);

    // 1134F puts the unit in the army list before 21944, but the occupancy registration
    // (11E04) runs after it. The unit is therefore not visible during the effect.
    const bool was_active = u.active;
    u.active = false;
    struct ActiveGuard {
        bool& a; bool v;
        ActiveGuard(bool& a_, bool v_) : a(a_), v(v_) {}
        ~ActiveGuard() { a = v; }
    } active_guard(u.active, was_active);

    camera_center_on(tx, ty);                        // camera already set by 299DA/3C548/4426D
    // sub_10971: (x - camX) * 18h + camFineX + 4; sub_10988: (y - camY) * 10h + camFineY + 4.
    const int raw_x = ((int)(int8_t)u.x - cam_tile_x_) * 24 + cam_fine_x_ + 4 + 0x0C;
    const int raw_y = ((int)(int8_t)u.y - cam_tile_y_) * 16 + cam_fine_y_ + 4 + 0x0C;
    const int dx0 = (tile_w > 1) ? 0x0C : 0;         // `cmp dl,1; jbe` -> var_18 = 0Ch
    const int X = raw_x + dx0;
    const int Y = raw_y;

    game_timer_set(7);                               // sub_1033B(7)
    for (int f = 0; f < 9 && !should_exit_; ++f) {   // `cmp cx,9; jl`
        if (f < (int)magic_frames_.size()) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, X + fr.xo, Y + fr.yo);   // sub_1FFFE
        }
        battle_fx_present();                         // 454B3 + 5E9E8
        wait_game_tick();                            // 499E4
    }
    if (should_exit_) return 1;

    sfx(0x32);                                       // sub_4F4CD(32h)
    const int nframes = (int)magic_frames_.size();
    auto blit_t = [&](int f, int x, int y, int pct) { // sub_5F29D (tinted blit)
        if (f < 0 || f >= nframes) return;
        const MagicFxFrame& fr = magic_frames_[(size_t)f];
        // 611AA(1 colour, shade = 1Eh): only palette entry 1 gets a LUT.
        battle_fx_blit_tinted_unclipped(f, x + fr.xo, y + fr.yo, pct, {0x01});
    };
    // Tint: 611AA(1 colour, shade = 1Eh) -> effect-colour weight 30, destination 70.
    // The shared tinter only allocates a LUT for palette entry 1.
    // 1033B(7) above also sets the period for this loop: the original has no second 1033B
    // here (the next one is 1033B(1Eh) after the rows), so the rows run at period 7.
    for (int r = 0; r < h && !should_exit_; ++r) {   // loc_21BAC: var_14 < [edx+3]
        const int p = r % 5;                         // remainder of `idiv 5`
        const bool last = (r == h - 1);
        // Main frame = 9 + (r%5); the last row uses unk_F0DFE = F0DAA + 0x54 = entry 7.
        blit_t(last ? 7 : 9 + p, X, Y, 0x1E);
        // sub_5F4AF: body rising from the ground, drawing only the first r+1 scanlines.
        if (body)
            blit_nc2_frame_clipped(raw_x + (int)(int8_t)raw[4],
                                   (int)(int8_t)raw[5] + (raw_y - 0x0C) + h - (r + 1),
                                   *body, r + 1);
        if (!last) {
            blit_t(0x0E + p, X, Y, 0x1E);            // secondary frame
            // Plume start = Y + secondary frame's yo - 8, then each sprite adds its own yo
            // and -16 per step (loc_21B3B). blit_t adds the per-frame yo.
            const int sec = 0x0E + p;
            const int plume_y = (sec < nframes) ? (Y + magic_frames_[(size_t)sec].yo - 8)
                                                : (Y - 8);
            for (int c = 0; c < 8; ++c)
                blit_t(0x13 + p, X, plume_y - c * 0x10, 0x1E);
        }
        battle_fx_present();                         // 454B3 + 5E9E8 (flush the queue over the map)
        wait_game_tick();                            // 499E4
    }
    game_timer_set(0x1E);                            // sub_1033B(1Eh)
    wait_game_tick();                                // 499E4
    sfx(0x33);                                       // sub_4F4CD(33h)
    if (!should_exit_) battle_fx_white_flash(2, 0x100);// sub_284A3
    // The caller releases the table (sub_1FFCD): 4426D does `call sub_21944; call sub_1FFCD`.
    return 1;
}

// ---- sub_2DA5E (dasm 40898..41164): spear fall (f-fall.ssp + slight/spearfly/fistlv3.nsp) ----
// eax = unit, edx = frame count (the original uses 2*edx+1), ebx = variant
// (0 slight / 1 spearfly / otherwise fistlv3).
//   (1) sub_4F460("f-fall.ssp") starts the fall sound, sub_1FEA4 loads the table, sub_135EB
//       takes the unit centre.
//   (2) sub_2D4EB: 1033B(19h), then poses 7 and 8, each presented once.
//   (3) game_timer_set(5), [unit+0x64] = 9 (pose locked), frame count = 2*edx+1.
//   (4) For variant 1 the start position shifts by +-(8, 0x0C) according to facing [0x63]
//       (off_2DA3E).
//   (5) sfx(0x32), then the frame loop: sub_454B3 -> draw the existing trail through sub_5F093
//       (solid silhouette, colours 0x11/0x13/0x15 at loc_2CA21+1; frame = facing*4 +
//       (counter+3-slot)/4) -> shift the four dword entries -> main frame through sub_1FFFE
//       (frame = facing*4 + counter/4) -> sub_5E9E8 -> sub_499E4 -> count up to 3, store the
//       current point at slot 0, counter++, advance by facing (off_2DA4E).
//   (6) While a trail remains (var_C > 0) keep presenting sub_454B3 plus the trail, then restore
//       the pose with sub_12124 and release the table with sub_1FFCD.
// The port takes the variant as k (0 slight / 1 spearfly / 2 fistlv3) and derives the frame count
// from the start-to-target distance: the edx passed by 2EB61 is produced by sub_2CBA1's overlay
// bookkeeping and cannot be recovered statically, so the duration remains an approximation.
int CombatII::battle_fx_2da5e(int unit_index, int tx, int ty, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    auto& u = units_[(size_t)unit_index];
    const char* nsp = (k == 0) ? "slight.nsp" : (k == 1 ? "spearfly.nsp" : "fistlv3.nsp");
    battle_ssp_play("f-fall.ssp", 0x32);             // sub_4F460("f-fall.ssp",32h)
    if (!battle_nsp_load(nsp) || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();

    camera_center_on(tx, ty);
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);                   // sub_135EB
    const bool variant1 = (k == 1);
    const int facing = (int)u.facing & 3;

    // (2) sub_2D4EB: two pose frames (7, 8) on the 19h timer.
    game_timer_set(0x19);                            // sub_1033B(19h)
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        u.anim_frame = 7 + i;                        // `add al,7`
        battle_fx_present();                         // sub_454B3 + sub_5E9E8
        wait_game_tick();                            // sub_499E4
    }
    if (should_exit_) { reset_unit_pose(u); battle_nsp_release(); return 1; }

    // Frame count = 2*edx+1 in the original; the port substitutes the Manhattan distance to the
    // target tile (see the note above).
    const int dist = (tx >= 0 && ty >= 0) ? (std::abs(tx - (int)u.x) + std::abs(ty - (int)u.y)) : 0;
    int frames = (dist > 0) ? (2 * dist + 1) : 7;
    if (frames > 0x40) frames = 0x40;                // clamp so the effect cannot run too long
    u.anim_frame = 9;                                // `mov byte [ebp+64h],9`
    game_timer_set(5);                               // sub_1033B(5)

    // Variant 1 start offset per facing (off_2DA3E: +8 / -0x0C / -8 / +0x0C).
    int wx = cx, wy = cy;
    if (variant1) {
        switch (facing) {
            case 0: wx += 8; break;                  // loc_2DB0A
            case 1: wy -= 0x0C; break;               // loc_2DB12
            case 2: wx -= 8; break;                  // loc_2DB1A
            default: wy += 0x0C; break;              // loc_2DB22
        }
    }

    sfx(0x32);                                       // sub_4F4CD(32h)
    // Four dword trail entries (x + (y << 16) in the original stack array).
    struct TrailPoint { int x = 0; int y = 0; };
    TrailPoint trail[4] = {{wx, wy}, {wx, wy}, {wx, wy}, {wx, wy}};
    int trail_n = 0;                                 // var_C (0..3)
    int counter = 0;                                 // var_4 (frame counter)
    static const uint8_t kTrailColor[3] = {0x11, 0x13, 0x15};   // loc_2CA21+1
    auto draw_trail = [&](int n) {                   // loc_2DB3E / loc_2DCB3
        for (int s = n - 1; s >= 0; --s) {
            const int f = ((counter + 3 - s) / 4) + facing * 4;
            if (f < 0 || f >= nframes) continue;
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit_silhouette(f, trail[s].x + fr.xo, trail[s].y + fr.yo,
                                      kTrailColor[s < 3 ? s : 2]); // sub_5F093
        }
    };
    auto step_offsets = [&]() {                      // off_2DA4E
        switch (facing) {
            case 0: wx += 8; break;
            case 1: wy -= 0x0C; break;
            case 2: wx -= 8; break;
            default: wy += 0x0C; break;
        }
    };
    for (int t = 0; t < frames && !should_exit_; ++t) {      // loc_2DC69
        draw_trail(trail_n);                                  // twice: 454B3 then the trail
        for (int s = trail_n; s > 0; --s) trail[s] = trail[s - 1];  // loc_2DBAF
        // Main frame: facing*4 + counter/4 through 1FFFE.
        const int f = (counter / 4) + facing * 4;
        if (f >= 0 && f < nframes) {
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            battle_fx_blit(f, wx + fr.xo, wy + fr.yo);        // sub_1FFFE
        }
        battle_fx_present();                                  // sub_5E9E8
        wait_game_tick();                                     // sub_499E4
        if (trail_n < 3) ++trail_n;                           // `cmp bx,3; jge`
        trail[0] = TrailPoint{wx, wy};                        // loc_2DC21
        ++counter;                                            // var_4
        step_offsets();                                       // off_2DA4E
    }
    // Tail: only the trail remains (loc_2DC98).
    while (trail_n > 0 && !should_exit_) {                    // `test cx,cx; jg`
        draw_trail(trail_n);
        battle_fx_present();                                  // 454B3 + 5E9E8
        wait_game_tick();                                     // 499E4
        --trail_n;
    }
    reset_unit_pose(u);                                       // sub_12124
    battle_nsp_release();                                     // sub_1FFCD
    return 1;
}

// ---- sub_570A1 (dasm 96212..96730): Meteor k=2 (pro.nsp + explosio.nsp) ----
// eax = screen x, edx = screen y.
//   (0) sub_1FEA4("pro.nsp") + sub_1FEA4("explosio.nsp"), sub_1124A, sub_56129, sub_135EB.
//   (1) sub_56178(0x4E, x=0,     y=0x64, base=0x53, cycle=7, state=1)
//       sub_56178(0x4F, x=0x140, y=0x64, base=0x4C, cycle=7, state=1), game_timer_set(0x0A),
//       sfx(0x32). Horizontal sweep: step var_C by -4 until cx == 4, blitting two frames
//       (0x53+ and 0x4C+ (i/2)%7) at (x, 0x64) through sub_56217.
//   (2) game_timer_set(0x14), sub_5623E(0x14), sfx(0x33), then until the sub_562C1(0x4B,0x32)
//       ramp ends: shake camera fine y by rand%3-1; slots 0..0x4D spawn with rand%4 as
//       0x5A (cycle 5) / 0x5F (cycle 8) particles; slots 0x50..0x59 use rand%12&3 for base
//       0/9/12/15; slots 0x5A..0x5E use rand%12 for base 0x24/0x2E/0x38/0x42; slots
//       0x4E..0x4F always draw (frame resets to 0 at cycle end). sfx(0x33) every 6 frames.
//   (3) Finally draw sub_10AEB + sub_12BEE plus slots 0x4E..0x4F until sub_562C1 returns 0,
//       then run the horizontal sweep once more in the opposite direction (cx 0 -> -0x4C by 4).
// The port keeps the (2) slot table in ProParticle, waits on the ramp with pal_ramp_step and
// shakes the camera through cam_fine_y_. sub_1FEA4 does not clear the table, so the explosio
// frames are appended after the pro frames (see the append lambda below).
int CombatII::battle_fx_570a1(int unit_index, int tx, int ty, int k) {
    (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];

    // (0) Append both tables: the original sub_1FEA4 does not clear the table.
    battle_nsp_release();
    auto append_nsp = [&](const char* name) -> int {
        const std::string path = game_path((std::string("MAGICSPR/") + name).c_str());
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) return 0;
        uint16_t count = 0;
        if (fseek(fp, 0x42, SEEK_SET) != 0 || fread(&count, 2, 1, fp) != 1 ||
            count == 0 || count > 512 || fseek(fp, 0x80, SEEK_SET) != 0) {
            fclose(fp);
            return 0;
        }
        int added = 0;
        for (uint16_t i = 0; i < count; ++i) {
            uint8_t hdr[32];
            if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) break;
            const uint32_t size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                                  ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            const uint16_t w = (uint16_t)(hdr[4] | (hdr[5] << 8));
            const uint16_t h = (uint16_t)(hdr[6] | (hdr[7] << 8));
            const int16_t xo = (int16_t)(hdr[12] | (hdr[13] << 8));
            const int16_t yo = (int16_t)(hdr[14] | (hdr[15] << 8));
            if (size == 0 || size > 0x100000 || w == 0 || h == 0 || (int)w * (int)h > 250000) break;
            std::vector<uint8_t> buf(size);
            if (fread(buf.data(), 1, size, fp) != size) break;
            MagicFxFrame fr;
            fr.w = w; fr.h = h; fr.xo = xo; fr.yo = yo; fr.xo0 = xo; fr.yo0 = yo;
            if (size >= 4 && size >= 4u + (uint32_t)w * h)
                fr.flat.assign(buf.begin() + 4, buf.begin() + 4 + (size_t)w * h);
            else {
                fr.flat.assign(buf.begin(), buf.end());
                fr.flat.resize((size_t)w * h, 0xFF);
            }
            magic_frames_.push_back(std::move(fr));
            ++added;
        }
        fclose(fp);
        return added;
    };
    if (append_nsp("pro.nsp") <= 0) return 0;
    append_nsp("explosio.nsp");                      // sub_1FEA4("explosio.nsp") appends to the tail
    const int nframes = (int)magic_frames_.size();

    camera_center_on(tx, ty);                        // sub_1124A
    pro_particles_init();                            // sub_56129
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);                   // sub_135EB

    // fx_blit_f: sub_56217(x, y, entry) blits at (x + entry[4], y + entry[6]).
    auto fx_blit_f = [&](int f, int x, int y) {
        if (f < 0 || f >= nframes) return;
        const MagicFxFrame& fr = magic_frames_[(size_t)f];
        battle_fx_blit(f, x + fr.xo, y + fr.yo);
    };

    // (1) Place the two frames at (0..0x140, 0x64) and sweep right.
    pro_spawn(0x4E, 0x000, 0x64, 0x53, 7, 1);
    pro_spawn(0x4F, 0x140, 0x64, 0x4C, 7, 1);
    game_timer_set(0x0A);                            // sub_1033B(0Ah)
    sfx(0x32);                                       // sub_4F4CD(32h)
    // Cx starts at -0x48, steps by +4 and ends at 4 (19 frames).
    int sweep_i = 0;                                 // di (frame counter)
    for (int x = -0x48; x != 4; x += 4) {             // loc_57143 (`cmp cx,4; jnz`)
        if (should_exit_) break;
        const int p = ((sweep_i++ / 2) % 7);         // `sub eax,edx; sar 1; idiv 7`
        fx_blit_f(0x53 + p, x, 0x64);                // sub_56217
        fx_blit_f(0x4C + p, x, 0x64);                // sub_56217
        battle_fx_present();                         // 454B3 + 454D5 + 5E9E8
        wait_game_tick();                            // 499E4
    }
    if (should_exit_) { battle_nsp_release(); return 1; }

    // (2) Palette ramp plus the particle fountain.
    game_timer_set(0x14);                            // sub_1033B(14h)
    pal_ramp_set_width(0x14);                        // sub_5623E(14h)
    sfx(0x33);                                       // sub_4F4CD(33h)
    int kills = 0;                                   // var_4
    for (int guard = 0; guard < 0x400 && !should_exit_; ++guard) {
        // sub_10AEB + sub_12BEE, then shake camera fine y by rand%3-1.
        cam_fine_y_ = (int16_t)(combat_rand(3) - 1);         // word dword_BA838
        pal_ramp_step(0x4B, 0x32);                           // sub_562C1(4Bh,32h); the result is unused
        for (int i = 0; i < kProSlots && !should_exit_; ++i) {
            ProParticle& p = g_pro[i];
            if (p.state == 0) {
                if (i < 0x4E) {                          // slots 0..0x4D: explosio sparks
                    const int r = combat_rand(4);
                    if (r == 0) pro_spawn(i, combat_rand(320), combat_rand(200), 0x5A, 5, 1);
                    else if (r == 1) pro_spawn(i, combat_rand(320), combat_rand(200), 0x5F, 8, 1);
                } else if (i >= 0x50 && i < 0x5A) {      // loc_57361: four-direction columns
                    const int r = combat_rand(12) & 3;
                    static const int base[4] = {0x00, 0x09, 0x12, 0x1B};
                    static const int px[4] = {0x1E, 0x1E, 0x122, 0x122};
                    pro_spawn(i, px[r], combat_rand(200), base[r], 9, 1);
                } else if (i >= 0x5A && i < 0x5F) {      // loc_57478: two rising side rows
                    int r = combat_rand(12) - 1;
                    if (r < 0) r = 0;
                    r &= 3;
                    static const int base[4] = {0x24, 0x2E, 0x38, 0x42};
                    static const int px[4] = {0xDC, 0xDC, 0x6E, 0x6E};
                    int y = combat_rand(10);
                    if (r == 1 || r == 2) y += 0x64;     // `add edx,64h`
                    pro_spawn(i, px[r], y, base[r], 10, 1);
                }
                continue;
            }
            if (i >= 0x4E && i < 0x50) {                 // 0x4E/0x4F always draw
                fx_blit_f(p.base + p.frame, p.x, p.y);
                ++p.frame;
                if (p.frame >= p.cycle) p.frame = 0;
                continue;
            }
            fx_blit_f(p.base + p.frame, p.x, p.y);       // sub_1FFFE(base + frame)
            ++p.frame;
            if (p.frame == p.cycle) {                    // cycle end: switch off and count
                pro_kill(i);
                if (++kills > 0x1F4) break;              // `cmp si,1F4h; jg`
            }
        }
        if (kills > 0x1F4) break;
        battle_fx_present();                             // 454D5 + 5E9E8
        wait_game_tick();                                // 499E4
        if (guard % 6 == 5) sfx(0x33);                   // every 6 frames (var_10 == 5)
    }
    if (should_exit_) { battle_nsp_release(); return 1; }

    // (3) Keep only 0x4E/0x4F until the ramp reaches 0 (loc_57630).
    for (int guard = 0; guard < 0x100 && !should_exit_; ++guard) {
        for (int i = 0x4E; i < 0x50; ++i) {
            ProParticle& p = g_pro[i];
            fx_blit_f(p.base + p.frame, p.x, p.y);
            ++p.frame;
            if (p.frame >= p.cycle) p.frame = 0;
        }
        battle_fx_present();
        wait_game_tick();
        if (pal_ramp_step(0x4B, 0) == 0) break;          // `test ax,ax; jnz`: exit when it returns 0
    }

    // (4) Final horizontal sweep: cx 0 -> -0x4C by 4, frames 0x53+ / 0x4C+ (cx/2)%7.
    // cx runs from 0 down to -0x4C (19 frames); the frame counter di starts at 0x140.
    int back_i = 0;                                      // var_14 (counts up from 0x140)
    for (int x = 0; x != -0x4C; x -= 4) {                // loc_576D1 (`cmp cx,0FFB4h`)
        if (should_exit_) break;
        const int p = ((back_i++ / 2) % 7);
        fx_blit_f(0x53 + p, x, 0x64);
        fx_blit_f(0x4C + p, x, 0x64);
        battle_fx_present();                             // 454B3 + 454D5 + 5E9E8
        wait_game_tick();                                // 499E4
    }
    battle_nsp_release();                                // sub_1FFCD
    return 1;
}
