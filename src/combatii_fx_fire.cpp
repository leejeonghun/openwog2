// Fire and storm spell effect drivers (sub_5666E / 56896 / 56BD4 / 57766 / 57AC4 / 57F18)
//
// All six share one particle engine: sub_56129 allocates and clears the 100-slot x 26-byte table
// (dword_112160), sub_56178 activates a slot as (x, y, start frame, period, state) and sub_561E5
// deactivates it. Slot layout, with the original offsets:
//   +00 x(i16)  +02 y(i16)  +04 state(1 = active)  +06 age  +08 NSP start frame
//   +0A period (frames)  +0C frame within period  +0E unused  +12 vx  +14 vy
//   The draw code reads dword [+06]>>16 (= base at +08) and dword [+0A]>>16 (= frame at +0C),
//   so the drawn index is base + frame, not age + period.
// Each tick an active slot draws frame = start frame + frame-in-period at (0x1FFFE + x, y) and is
// killed when frame-in-period reaches the period. Empty slots spawn particles with rand%N.
//
// Deviations, all replaced inside this file:
//  - Palette ramps use the shared runtime: 5623E -> pal_ramp_set_width, 562C1 -> pal_ramp_step,
//    56245/5FA2C -> pal_ramp_apply. The four ramped drivers reproduce the ASM's pre-loop
//    562C1(4Bh,0) fade-in and tail 562C1(4Bh,0) fade-out with battle_fx_present() +
//    video_.wait_presented() per step (5FD1C waits for the retrace 5E943; wait_game_tick()
//    would wait a whole 1033B period instead).
//  - sub_10AEB/sub_12BEE (redraw map and units) are replaced by battle_fx_present(), which draws
//    the complete battle frame.
//  - sub_56102/sub_561F4/sub_5EFB9/sub_5F29D (frame offsets and the 480-stride blitter) are
//    replaced by battle_fx_blit with the frame's xo/yo added (dx_draw_off).
//  - 1FEA4 appends to the table instead of replacing it: 56BD4 loads explosio(0..12), then
//    firestor(13..89), then hellfire(90..) and keeps base 0/0x0D/0x5A, so this file uses
//    fx_append_nsp, the appending variant of the same parser, instead of battle_nsp_load.
#include "combatii.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- Particle table (dword_112160: 100 x 26 bytes) ----
struct FxParticle {
    int16_t x = 0, y = 0;       // +00, +02
    int16_t state = 0;          // +04
    int16_t age = 0;            // +06
    int16_t base = 0;           // +08
    int16_t cycle = 0;          // +0A
    int16_t frame = 0;          // +0C
    int16_t pad = 0;            // +0E
    int16_t vx = 0;             // +12
    int16_t vy = 0;             // +14
};
constexpr int FX_SLOTS = 100;   // 0x64
FxParticle fx_slot[FX_SLOTS];

// Rand_ replacement: the original returns 0..0x7FFF and callers take the remainder with idiv.
// Unlike combat_rand elsewhere in the port, this generator is effect-only and file-local.
unsigned fx_seed = 0x2F6E2B1u;
int fx_rand() {
    fx_seed = fx_seed * 1103515245u + 12345u;
    return (int)((fx_seed >> 16) & 0x7FFFu);
}
int fx_rand_mod(int n) { return n > 0 ? fx_rand() % n : 0; }

// sub_56129: allocate and zero the 100 slots.
void fx_particles_init() {
    for (int i = 0; i < FX_SLOTS; ++i) fx_slot[i] = FxParticle{};
}

// sub_5615D: release the table; the port clears the static array instead.
void fx_particles_free() {
    for (int i = 0; i < FX_SLOTS; ++i) fx_slot[i] = FxParticle{};
}

// sub_56178(eax=slot, edx=x, ebx=y, ecx=start frame, [esp+8]=period, [esp+0Ch]=state)
void fx_spawn(int idx, int x, int y, int base, int cycle, int state) {
    if (idx < 0 || idx >= FX_SLOTS) return;
    FxParticle& p = fx_slot[idx];
    p.x = (int16_t)x;
    p.y = (int16_t)y;
    p.state = (int16_t)state;
    p.age = 0;
    p.base = (int16_t)base;
    p.cycle = (int16_t)cycle;
    p.frame = 0;
    p.pad = 0;
    p.vx = 0;
    p.vy = 0;
}

// sub_561E5: deactivate a slot.
void fx_kill(int idx) {
    if (idx < 0 || idx >= FX_SLOTS) return;
    fx_slot[idx].state = 0;
}

// sub_1FEA4 (appending variant): add the frames of MAGICSPR\<name> after the existing table.
// The file format matches battle_nsp_load: 0x42 = u16 frame count, entries from 0x80, each with a
// 32-byte header {u32 size, u16 w, u16 h, u32 -, i16 x, i16 y, 16B -} followed by size bytes
// (size = 4 + w*h; the payload is [w][h] then w*h flat bytes, 0FFh transparent).
// FrameVec is CombatII::MagicFxFrame, a private nested type, so it is taken as a template argument.
template <class FrameVec>
int fx_append_nsp(const char* path, FrameVec& frames) {
    if (!path || !*path) return 0;
    FILE* fp = fopen(path, "rb");
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
        std::vector<uint8_t> raw(size);
        if (fread(raw.data(), 1, size, fp) != size) break;
        typename FrameVec::value_type fr;
        fr.w = w;
        fr.h = h;
        fr.xo = xo;
        fr.yo = yo;
        if (size >= 4 && size >= 4u + (uint32_t)w * h) {
            fr.flat.assign(raw.begin() + 4, raw.begin() + 4 + (size_t)w * h);
        } else {
            fr.flat.assign(raw.begin(), raw.end());
            fr.flat.resize((size_t)w * h, 0xFF);
        }
        frames.push_back(std::move(fr));
        ++added;
    }
    fclose(fp);
    return added;
}

}  // namespace

// sub_5666E: Explosion (explosio.nsp); scatters sparks over the whole screen.
//   Loads explosio.nsp, sets the timer period to 20 with 1033B(0x14), then walks the 0x64 slots and
//   creates two kinds of particle from rand%4 when a slot is empty:
//     0 -> start frame 0, period 5;  1 -> start frame 5, period 8
//   Position is always (rand%320, rand%200), the whole screen. Active slots draw their frame and
//   are killed at the end of the period; above 0x12C (300) killed no more spawn, and above
//   0x1F4 (500) the loop ends. Every 5 frames 4F4CD(32h) plays (var_8 starts at 5).
int CombatII::battle_fx_5666e(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;                       // handler 2A1C6 passes only the target tile
    battle_nsp_release();                            // the table is already empty in the original
    // 1FEA4 "explosio.nsp"
    if (fx_append_nsp(game_path("MAGICSPR/explosio.nsp").c_str(), magic_frames_) <= 0) return 0;
    camera_center_on(tx, ty);                        // sub_1124A
    fx_particles_init();                             // sub_56129
    game_timer_set(0x14);                            // sub_1033B(14h)
    pal_ramp_set_width(0x14);            // sub_5623E (word_11233C ramp width)
    // ASM 95416..95422: fade the palette 0% -> 75% (4Bh) before the first particle frame.
    // Every 562C1 call ends in 5FD1C -> 5E943 (one 70 Hz retrace). The display palette is
    // captured on present in the port, so each step is presented before waiting one frame.
    for (;;) {
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();                 // sub_5E943 (one display frame)
        if (ramp == 1) break;
    }
    int spawned = 5, ticks = 5;          // ASM var_10 (kill count) and var_8 both start at 5
    for (int guard = 0; guard < 0x800 && !should_exit_; ++guard) {
        pal_ramp_step(0x4B, 0x32);                 // sub_562C1 (3% ramp per frame)
        for (int i = 0; i < FX_SLOTS && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state == 0) {
                const int r = fx_rand_mod(4);
                if (r == 0) {                        // loc_5674D
                    fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0, 5, 1);
                } else if (r == 1) {                 // loc_56786
                    fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 5, 8, 1);
                }
                continue;
            }
            // loc_567B7: dword [+06]>>16 = base(+08); dword [+0A]>>16 = frame(+0C)
            battle_fx_blit(p.base + p.frame, p.x, p.y);
            ++p.frame;
            if (p.frame == p.cycle) {                // period over -> kill the slot
                fx_kill(i);
                if (++spawned > 0x1F4) { i = FX_SLOTS; }
            }
        }
        if (spawned > 0x12C) break;                  // var_10 > 300 -> draw the final frame
        if (ticks % 5 == 0) sfx(0x32);               // 4F4CD(32h) every 5 frames (ASM 95528)
        ++ticks;
        battle_fx_present();                         // sub_454D5 + sub_5E9E8
        wait_game_tick();                            // sub_499E4
    }
    if (!should_exit_) battle_fx_present();          // loc_56858: one last frame
    // loc_56873: tail 562C1(4Bh,0) fade-out (the original stops at 2-3% and does not flush;
    // the port presents each step so the captured palette follows the fade).
    for (;;) {
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();                 // sub_5E943
        if (ramp == 0) break;
    }
    pal_ramp_apply(0);                               // release the ramp (DAC = master)
    battle_nsp_release();                            // sub_1FFCD
    fx_particles_free();                             // sub_5615D
    return 1;
}

// sub_56896: Fire Storm. Loads explosio (0..12) then firestor (13..) as two appended tables.
//   Group A (slots 0..0x4F) follows the 5666E rule (0 -> base 0 period 5, 1 -> base 5 period 8).
//   Group B (slots 0x50..0x63) spawns only on rand%4==3 with base 0x0D period 7, drifts right with
//     x += 2 every tick and dies at the end of its period.
//   Every 5 frames 4F4CD(0x32) plays, and 4F4CD(0x33) when rand%10==0.
int CombatII::battle_fx_56896(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    battle_nsp_release();
    if (fx_append_nsp(game_path("MAGICSPR/explosio.nsp").c_str(), magic_frames_) <= 0) return 0;
    fx_append_nsp(game_path("MAGICSPR/firestor.nsp").c_str(), magic_frames_);  // appended
    camera_center_on(tx, ty);                        // sub_1124A
    fx_particles_init();
    game_timer_set(0x14);                            // sub_1033B(14h)
    pal_ramp_set_width(0x14);            // sub_5623E (word_11233C ramp width)
    for (;;) {                                   // loc_568F0 fade-in (5E943 per step)
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();
        if (ramp == 1) break;
    }
    int spawned = 0, ticks = 0;
    bool done = false;
    for (int guard = 0; guard < 0x2000 && !should_exit_ && !done; ++guard) {
        pal_ramp_step(0x4B, 0x32);                 // sub_562C1 (3% ramp per frame)
        // Group A: slots 0..0x4F
        for (int i = 0; i < 0x50 && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state == 0) {
                const int r = fx_rand_mod(4);
                if (r == 0) fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0, 5, 1);
                else if (r == 1) fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 5, 8, 1);
                continue;
            }
            battle_fx_blit(p.base + p.frame, p.x, p.y);
            ++p.frame;
            if (p.frame == p.cycle) {
                fx_kill(i);
                if (++spawned > 0x1F4) { done = true; break; }
            }
        }
        // Group B: slots 0x50..0x63
        if (!done) {
            for (int i = 0x50; i < 0x64 && !should_exit_; ++i) {
                FxParticle& p = fx_slot[i];
                if (p.state == 0) {
                    if (fx_rand_mod(4) == 3)
                        fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0x0D, 7, 1);
                    continue;
                }
                battle_fx_blit(p.base + p.frame, p.x, p.y);
                p.x = (int16_t)(p.x + 2);
                ++p.frame;
                if (p.frame == p.cycle) {
                    fx_kill(i);
                    if (++spawned > 0x1F4) { done = true; break; }
                }
            }
        }
        if (done) break;
        if (ticks % 5 == 0) sfx(0x32);               // remainder of `idiv ebp(5)` is 0
        if (fx_rand_mod(10) == 0) sfx(0x33);         // 499BD(0A) == 0
        ++ticks;
        battle_fx_present();
        wait_game_tick();
    }
    if (!should_exit_) battle_fx_present();          // loc_56B96: final frame
    for (;;) {                                   // loc_56BB1 fade-out
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();
        if (ramp == 0) break;
    }
    pal_ramp_apply(0);                               // release the ramp (DAC = master)
    battle_nsp_release();
    fx_particles_free();
    return 1;
}

// sub_56BD4: Hell Fire. Appends explosio (0..12), firestor (13..89) and hellfire (90..).
//   (1) Every 15 frames, while the frame counter is < 0x28, lights slots 0x5A..0x5C as
//       (x=0, y=0xAA, start frame 0x14, period 3, state 1); slot index = 0x5A + frame/15.
//   (2) Those three slots draw the same frame at (x, y), (x+0x78, y) and (x+0xF0, y) every tick,
//       are killed once age > 0x28, and otherwise advance the period frame and rise with y -= 6.
//   (3) Slots 0..0x4F spawn by the 56896 group-A rule; slots 0x50..0x59 spawn by the group-B
//       rule (rand%4==1, base 0x0D, period 7, drift right). 500 killed ends the effect.
//   Every 5 frames 4F4CD(0x32/0x34 by rand%2) plays, and 0x33 when rand%10==0.
int CombatII::battle_fx_56bd4(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    battle_nsp_release();
    if (fx_append_nsp(game_path("MAGICSPR/explosio.nsp").c_str(), magic_frames_) <= 0) return 0;
    fx_append_nsp(game_path("MAGICSPR/firestor.nsp").c_str(), magic_frames_);
    fx_append_nsp(game_path("MAGICSPR/hellfire.nsp").c_str(), magic_frames_);
    camera_center_on(tx, ty);                        // sub_1124A
    fx_particles_init();
    game_timer_set(0x14);                            // sub_1033B(14h)
    pal_ramp_set_width(0x14);            // sub_5623E (word_11233C ramp width)
    for (;;) {                                   // loc_56C3A fade-in (5E943 per step)
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();
        if (ramp == 1) break;
    }
    int spawned = 0, ticks = 0, frames = -1;
    bool done = false;
    for (int guard = 0; guard < 0x4000 && !should_exit_ && !done; ++guard) {
        pal_ramp_step(0x4B, 0x32);                 // sub_562C1 (3% ramp per frame)
        ++frames;                                  // ASM var_14 starts at -1
        // (1) light slots 0x5A..0x5C every 15 frames while the frame counter is < 0x28
        if (frames % 0x0F == 0 && frames < 0x28) {
            const int slot = 0x5A + frames / 0x0F;
            fx_spawn(slot, 0, 0xAA, 0x14, 3, 1);
        }
        // (2) draw slots 0x5A..0x5C as a horizontal chain of three
        for (int i = 0x5A; i < 0x5D && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) continue;
            const int f = p.base + p.frame;
            battle_fx_blit(f, p.x, p.y);
            battle_fx_blit(f, p.x + 0x78, p.y);
            battle_fx_blit(f, p.x + 0xF0, p.y);
            ++p.age;
            if (p.age > 0x28) { fx_kill(i); continue; }
            ++p.frame;
            if (p.frame == p.cycle) p.frame = 0;
            p.y = (int16_t)(p.y - 6);
        }
        // (3a) slots 0..0x4F: 56896 group-A rule
        for (int i = 0; i < 0x50 && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state == 0) {
                const int r = fx_rand_mod(4);
                if (r == 0) fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0, 5, 1);
                else if (r == 1) fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 5, 8, 1);
                continue;
            }
            battle_fx_blit(p.base + p.frame, p.x, p.y);
            ++p.frame;
            if (p.frame == p.cycle) {
                fx_kill(i);
                if (++spawned > 0x1F4) { done = true; break; }
            }
        }
        if (done) break;
        // (3b) slots 0x50..0x59: rand%4==1, base 0x0D, period 7, drift right
        for (int i = 0x50; i < 0x5A && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state == 0) {
                if (fx_rand_mod(4) == 1)
                    fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0x0D, 7, 1);
                continue;
            }
            battle_fx_blit(p.base + p.frame, p.x, p.y);
            p.x = (int16_t)(p.x + 2);
            ++p.frame;
            if (p.frame == p.cycle) {
                fx_kill(i);
                if (++spawned > 0x1F4) { done = true; break; }
            }
        }
        if (done) break;
        if (ticks % 5 == 0) sfx(0x32 + 2 * fx_rand_mod(2));  // 499BD(2) -> 32h/34h
        if (fx_rand_mod(10) == 0) sfx(0x33);
        ++ticks;
        battle_fx_present();
        wait_game_tick();
    }
    if (!should_exit_) battle_fx_present();          // loc_57041: final frame
    for (;;) {                                   // loc_5705C fade-out
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();
        if (ramp == 0) break;
    }
    pal_ramp_apply(0);                               // release the ramp (DAC = master)
    battle_nsp_release();
    fx_particles_free();
    return 1;
}

// sub_57766: Leopard (leopard.nsp, 12 frames = 4 trail particles x 3 frames).
//   Slots 0x0A..0x0D stay lit with (start frame 0/3/6/9, period 3, state 1) and cycle their frames.
//   Slots 0..9 are born somewhere on screen and move with vx/vy (5..24, each negated with 50%
//   probability) for at most 11 frames, dying when they leave (0..320, 20..0x104). Once more than
//   0x1E have died no more spawn (var_4 = 1), and the effect ends when no particle is alive.
//   Each slot leaves a trail as long as its age: the 11 words of off_56099
//   (0A,0B,0C,0D,0D,0C,0D,0D,0C,0D,0D) are drawn at (x, y - 10*n) with 56102, the blitter that
//   adds the frame's xo/yo.
//   Period 3; 4F4CD(0x32) every 6 frames.
int CombatII::battle_fx_57766(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    battle_nsp_release();
    if (fx_append_nsp(game_path("MAGICSPR/leopard.nsp").c_str(), magic_frames_) <= 0) return 0;
    // 11 words copied from off_56099: 0A,0B,0C,0D,0D,0C,0D,0D,0C,0D,0D.
    static const int kTrail[11] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D};
    camera_center_on(tx, ty);                        // sub_1124A
    fx_particles_init();                             // sub_56129
    fx_spawn(0x0A, 0, 0, 0, 3, 1);                   // 57766: slots 0Ah/0Bh/0Ch/0Dh, base 0/3/6/9
    fx_spawn(0x0B, 0, 0, 3, 3, 1);
    fx_spawn(0x0C, 0, 0, 6, 3, 1);
    fx_spawn(0x0D, 0, 0, 9, 3, 1);
    game_timer_set(3);                               // sub_1033B(3)
    int dead = 0, ticks = 3;                         // ASM var_8 starts at 3 -> 32h at frame 2
    bool no_more = false;
    auto draw_off = [&](int frame, int x, int y) {    // sub_56102 (adds the frame xo/yo)
        if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
        battle_fx_blit(frame, x + magic_frames_[(size_t)frame].xo,
                       y + magic_frames_[(size_t)frame].yo);
    };
    for (int guard = 0; guard < 0x4000 && !should_exit_; ++guard) {
        // Advance the period frame of slots 0x0A..0x0D
        for (int i = 0x0A; i < 0x0E; ++i) {
            FxParticle& p = fx_slot[i];
            ++p.frame;
            if (p.frame == p.cycle) p.frame = 0;
        }
        for (int i = 0; i < 0x0A && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) {
                if (no_more) continue;
                const int cycle = fx_rand_mod(10);
                fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0, cycle, 1);
                p.vx = (int16_t)(5 + fx_rand_mod(0x14));
                p.vy = (int16_t)(5 + fx_rand_mod(0x14));
                if (fx_rand_mod(2)) p.vx = (int16_t)(-p.vx);
                if (fx_rand_mod(2)) p.vy = (int16_t)(-p.vy);
                continue;
            }
            ++p.age;
            if (p.age >= 0x0B) {                     // loc_57974: moves for at most 11 frames
                p.age = 0x0B;
                p.x = (int16_t)(p.x + p.vx);
                p.y = (int16_t)(p.y + p.vy);
                if (p.x < 0 || p.x > 0x140 || p.y < 0x14 || p.y > 0x104) {
                    fx_kill(i);
                    ++dead;
                }
            }
            if (dead > 0x1E) no_more = true;
            // Trail: one slot per age step, 10 px apart
            for (int c = 0; c < p.age && c < 11; ++c) {
                const FxParticle& t = fx_slot[kTrail[c]];
                draw_off(t.base + t.frame, p.x, p.y - c * 0x0A);
            }
        }
        // Ends when var_4 != 0 and slots 0..9 are all empty
        bool any_alive = false;
        for (int i = 0; i < 0x0A; ++i) if (fx_slot[i].state == 1) any_alive = true;
        if (no_more && !any_alive) break;
        if (++ticks % 6 == 0) sfx(0x32);             // 32h when the var_8 counter reaches 5
        battle_fx_present();                         // sub_5E9E8
        wait_game_tick();                            // sub_499E4
    }
    battle_nsp_release();                            // sub_1FFCD
    fx_particles_free();                             // sub_5615D
    return 1;
}

// sub_57AC4: Ice Storm (leopard.nsp + icestorm.nsp).
//   Same trail part as 57766 (slots 0x0A..0x0D, trail slots 0..9), plus one pass over slots
//   0x50..0x63 that spawns a snow particle with state = rand%4 (base 0x0C = first icestorm frame,
//   period 7) and drifts it right with x += 2. off_560AF is the same 11-word table as off_56099:
//   {0Ah,0Bh,0Ch,0Dh,0Dh,0Ch,0Dh,0Dh,0Ch,0Dh,0Dh}. 4F4CD(0x32) every 6 frames, and 0x33 when
//   rand%10==0.
int CombatII::battle_fx_57ac4(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    battle_nsp_release();
    if (fx_append_nsp(game_path("MAGICSPR/leopard.nsp").c_str(), magic_frames_) <= 0) return 0;
    fx_append_nsp(game_path("MAGICSPR/icestorm.nsp").c_str(), magic_frames_);
    // off_560AF (22-byte copy = 11 words, same sequence as off_56099); age cap 0x0B.
    static const int kTrail[11] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D};
    camera_center_on(tx, ty);
    fx_particles_init();
    fx_spawn(0x0A, 0, 0, 0, 3, 1);
    fx_spawn(0x0B, 0, 0, 3, 3, 1);
    fx_spawn(0x0C, 0, 0, 6, 3, 1);
    fx_spawn(0x0D, 0, 0, 9, 3, 1);
    game_timer_set(3);                               // sub_1033B(3)
    int dead = 0, ticks = 3;                         // ASM var_8 starts at 3 -> 32h at frame 2
    bool no_more = false;
    auto draw_off = [&](int frame, int x, int y) {    // sub_56102
        if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
        battle_fx_blit(frame, x + magic_frames_[(size_t)frame].xo,
                       y + magic_frames_[(size_t)frame].yo);
    };
    for (int guard = 0; guard < 0x4000 && !should_exit_; ++guard) {
        for (int i = 0x0A; i < 0x0E; ++i) {
            FxParticle& p = fx_slot[i];
            ++p.frame;
            if (p.frame == p.cycle) p.frame = 0;
        }
        for (int i = 0; i < 0x0A && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) {
                if (no_more) continue;
                const int cycle = fx_rand_mod(10);
                fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0, cycle, 1);
                p.vx = (int16_t)(5 + fx_rand_mod(0x14));
                p.vy = (int16_t)(5 + fx_rand_mod(0x14));
                if (fx_rand_mod(2)) p.vx = (int16_t)(-p.vx);
                if (fx_rand_mod(2)) p.vy = (int16_t)(-p.vy);
                continue;
            }
            ++p.age;
            if (p.age >= 0x0B) {
                p.age = 0x0B;
                p.x = (int16_t)(p.x + p.vx);
                p.y = (int16_t)(p.y + p.vy);
                if (p.x < 0 || p.x > 0x140 || p.y < 0x14 || p.y > 0x104) {
                    fx_kill(i);
                    ++dead;
                }
            }
            if (dead > 0x1E) no_more = true;
            for (int c = 0; c < p.age && c < 11; ++c) {
                const FxParticle& t = fx_slot[kTrail[c]];
                draw_off(t.base + t.frame, p.x, p.y - c * 0x0A);
            }
        }
        // Snow: one pass over slots 0x50..0x63 (ASM var_10 = 0x50..0x63).
        for (int i = 0x50; i < 0x64 && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) {
                const int state = fx_rand_mod(4);          // 56178 state = rand%4
                fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0x0C, 7, state);
                continue;
            }
            draw_off(p.base + p.frame, p.x, p.y);
            p.x = (int16_t)(p.x + 2);
            ++p.frame;
            if (p.frame == p.cycle) fx_kill(i);
        }
        bool any_alive = false;
        for (int i = 0; i < 0x0A; ++i) if (fx_slot[i].state == 1) any_alive = true;
        if (no_more && !any_alive) break;
        if (++ticks % 6 == 0) sfx(0x32);
        if (fx_rand_mod(10) == 0) sfx(0x33);
        battle_fx_present();
        wait_game_tick();
    }
    battle_nsp_release();
    fx_particles_free();
    return 1;
}

// sub_57F18: Blizzard (leopard + icestorm + blizzard tables).
//   Sets the ramp width with 5623E(0x29), starts with 1033B(0x0A) and plays 4F4CD(0x33).
//   Each tick shakes the screen slightly: low word of dword_BA838 = rand%3 - 1.
//   Group A, 0..9: trail particles as in 57766, but only counted, never drawn.
//   Group B, 0x14..0x27: base 0x0C period 7 snow, drifting right with x += 2 and killed at the end
//     of the period.
//   Group C, 0x28..0x3B: three kinds of snowflake from rand%3, (state = rand%5, period 4) with
//     base 0x13 / 0x17 / 0x1B, starting at y = -(rand%100), x = rand%320 + 0x50. While age < 8
//     they fall down-left with x -= 10, y += 0x14; inside (0 < x < 0x12C, x > -0x50, y < 0x12C)
//     they draw base + 0/1/2/3 by age band (<=7 / <=9 / <=10 / otherwise). Above age 0x0A
//     4F4CD(0x34) plays and the slot dies.
//   When group C is done the shake returns to 0, a few more frames are drawn and the table freed.
int CombatII::battle_fx_57f18(int unit_index, int tx, int ty, int k) {
    (void)unit_index; (void)k;
    battle_nsp_release();
    if (fx_append_nsp(game_path("MAGICSPR/leopard.nsp").c_str(), magic_frames_) <= 0) return 0;
    fx_append_nsp(game_path("MAGICSPR/icestorm.nsp").c_str(), magic_frames_);
    fx_append_nsp(game_path("MAGICSPR/blizzard.nsp").c_str(), magic_frames_);
    camera_center_on(tx, ty);
    fx_particles_init();
    fx_spawn(0x0A, 0, 0, 0, 3, 1);
    fx_spawn(0x0B, 0, 0, 3, 3, 1);
    fx_spawn(0x0C, 0, 0, 6, 3, 1);
    fx_spawn(0x0D, 0, 0, 9, 3, 1);
    game_timer_set(0x0A);                            // sub_1033B(0A)
    pal_ramp_set_width(0x29);            // sub_5623E (word_11233C ramp width)
    for (;;) {                                   // loc_57FDE fade-in (5E943 per step)
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();
        video_.wait_presented();
        if (ramp == 1) break;
    }
    sfx(0x33);                                       // 4F4CD(33)
    int dead = 0, ticks = 0;
    bool no_more = false;
    auto draw_off = [&](int frame, int x, int y) {    // sub_56102/sub_561F4
        if (frame < 0 || (size_t)frame >= magic_frames_.size()) return;
        battle_fx_blit(frame, x + magic_frames_[(size_t)frame].xo,
                       y + magic_frames_[(size_t)frame].yo);
    };
    for (int guard = 0; guard < 0x6000 && !should_exit_; ++guard) {
        pal_ramp_step(0x4B, 0x32);                 // sub_562C1 (3% ramp per frame)
        cam_fine_y_ = (int16_t)(fx_rand_mod(3) - 1);  // low word of dword_BA838 (screen shake)
        for (int i = 0x0A; i < 0x0E; ++i) {
            FxParticle& p = fx_slot[i];
            ++p.frame;
            if (p.frame == p.cycle) p.frame = 0;
        }
        // Group A: trail particles (age only, never drawn)
        for (int i = 0; i < 0x0A && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) {
                if (no_more) continue;
                const int cycle = fx_rand_mod(10);
                fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0, cycle, 1);
                p.vx = (int16_t)(5 + fx_rand_mod(0x14));
                p.vy = (int16_t)(5 + fx_rand_mod(0x14));
                if (fx_rand_mod(2)) p.vx = (int16_t)(-p.vx);
                if (fx_rand_mod(2)) p.vy = (int16_t)(-p.vy);
                continue;
            }
            ++p.age;
            if (p.age >= 0x0B) {
                p.age = 0x0B;
                p.x = (int16_t)(p.x + p.vx);
                p.y = (int16_t)(p.y + p.vy);
                if (p.x < 0 || p.x > 0x140 || p.y < 0x14 || p.y > 0x104) {
                    fx_kill(i);
                    ++dead;
                }
            }
            if (dead > 0x1E) no_more = true;
        }
        // Group B: slots 0x14..0x27
        for (int i = 0x14; i < 0x28 && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) {
                if (no_more) continue;
                const int state = fx_rand_mod(4);          // 56178 state = rand%4
                fx_spawn(i, fx_rand_mod(320), fx_rand_mod(200), 0x0C, 7, state);
                continue;
            }
            draw_off(p.base + p.frame, p.x, p.y);
            p.x = (int16_t)(p.x + 2);
            ++p.frame;
            if (p.frame == p.cycle) fx_kill(i);
        }
        // Group C: slots 0x28..0x3B (snowflakes)
        for (int i = 0x28; i < 0x3C && !should_exit_; ++i) {
            FxParticle& p = fx_slot[i];
            if (p.state != 1) {
                if (no_more) continue;
                const int r = fx_rand_mod(3);
                const int state = fx_rand_mod(5);
                const int y = -fx_rand_mod(100);
                const int x = fx_rand_mod(320) + 0x50;
                const int base = (r == 0) ? 0x13 : (r == 1 ? 0x17 : 0x1B);
                fx_spawn(i, x, y, base, 4, state);   // state = rand%5 (drawn only when 1)
                continue;
            }
            ++p.age;
            if (p.age < 8) {                         // loc_5843B: fall down-left for 8 frames
                p.x = (int16_t)(p.x - 0x0A);
                p.y = (int16_t)(p.y + 0x14);
            }
            if (p.x > 0 && p.x < 0x12C && p.x > -0x50 && p.y < 0x12C) {
                const int off = (p.age < 8) ? 0 : (p.age < 9 ? 1 : (p.age < 0x0A ? 2 : 3));
                draw_off(p.base + off, p.x, p.y);
            }
            if (p.age > 0x0A) {                      // age above 10 -> 4F4CD(34) then kill
                sfx(0x34);
                fx_kill(i);
            }
        }
        bool any_alive = false;
        for (int i = 0x14; i < 0x3C; ++i) if (fx_slot[i].state == 1) any_alive = true;
        if (no_more && !any_alive) break;
        if (fx_rand_mod(10) == 0) sfx(0x33);
        ++ticks;
        battle_fx_present();
        wait_game_tick();
    }
    cam_fine_y_ = 0;                                 // dword_BA838 = 0 (restore the shake)
    // loc_58571: 454B3 once, then 562C1(4Bh,0) with a 5E9E8 flush per step.
    if (!should_exit_) battle_fx_present();
    for (;;) {
        if (should_exit_) break;
        const int ramp = pal_ramp_step(0x4B, 0);
        battle_fx_present();                         // 454B3 + 5E9E8 for the fade step
        video_.wait_presented();                     // sub_5E943
        if (ramp == 0) break;
    }
    pal_ramp_apply(0);                               // release the ramp (DAC = master)
    battle_nsp_release();
    fx_particles_free();
    (void)ticks;
    return 1;
}
