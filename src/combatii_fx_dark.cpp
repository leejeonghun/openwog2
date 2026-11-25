// Magic effect drivers - dark/instant-death family (sub_276B9 / sub_2799A / sub_27C5A /
// sub_26C9A / sub_26E1F).
//
// Common structure (the same as battle_fx_projectile_2016a / battle_fx_blast_203cc in
// combatii_battle.cpp):
//   1. sub_1FEA4 loads MAGICSPR\<spell>.nsp into the unk_F0DAA table.
//   2. 1033B sets the timer period, then every frame repeats this sequence in the original order:
//        sub_10AEB (map/FPM) -> sub_12D65 (top tiles + 18605 effects) = battle_fx_world_12d65()
//        -> sub_5F570/5F611 (effect frame)               = battle_fx_blit_scaled()
//        -> sub_12E01 (sprite transfer)                  = battle_fx_flush()
//        -> sub_454D5 (dim, markers, highlights, HP, HUD) = battle_fx_overlay()
//        -> sub_5E9E8 (transfer) -> sub_499E4 (tick)     = battle_fx_commit() / wait_game_tick()
//      sub_12D65 has no 122C4/12567 unit pass, so these frames draw no normal/attacking/water
//      units and no weather. Only step 2 of 276B9 uses 454B3 first and then 12D65 again
//      (battle_fx_world_454b3_12d65); 26E1F uses battle_fx_present_12d65() for the whole frame.
//   3. The screen shake alternates the low word of dword_BA838 (camera fine offset) between 0 and
//      0FFFCh (-4); the port keeps it in cam_fine_y_, the same field the other drivers use.
//
// sub_44DCC (highlight list) inside 454D5 is ported as render_highlight_units(); registration is
// battle_highlight_add (sub_44D89), used by the death effect sub_463FB.
#include "combatii.h"

#include <algorithm>
#include <cstdio>

// An original unk_F0DAA entry is 12 bytes {w, y-offset, x-offset, pixel ptr}; the 5F570/5F611
// family puts the top-left at (centre - size/2 + offset). The port's MagicFxFrame holds
// .w .h .xo .yo .flat.

// ---- sub_26C9A: Death (q1 58) ----
//   eax/edx = target screen coordinates in 16.16, filled in by the caller through 135EB. 11DB2
//   finds the unit on that tile, 135EB computes its centre and death.nsp is loaded. Then:
//     44D6E(1) dims the map and two passes run (di = 0, 1):
//       - 5FD1C brightens the palette from 0 to 0x64 in steps of 0x14 (5 steps), presenting one
//         frame with 45559 after each step
//       - 5FD1C steps it back by 0x0A to darken
//       - di==1 exits (the second pass is fade only)
//       - 1033B(0x10); frames 0..word_F0DA8-1 are drawn at (x, y-0x20) as
//         1FFFE+454B3+5E9E8+499E4, with two extra 499E4 ticks and 4F4CD(0x32) on frame 5
//       - 44D6E(0) restores the map
//     45559; 1FFCD frees the table.
//   The 5FC21/5FD1C palette fade is the same work-palette delta pal_shift_fade builds
//   (unk_12824F = master palette entry 1).
int CombatII::battle_fx_26c9a(int unit_index, int tx, int ty, int k) {
    (void)k;
    // 11DB2: unit standing on the target tile; falls back to the passed unit.
    int victim = -1;
    if (tx >= 0 && ty >= 0 && tx < (int)map_w_ && ty < (int)map_h_)
        victim = battle_unit_index_at(tx, ty);
    if (victim < 0 || (size_t)victim >= units_.size()) victim = unit_index;
    if (victim < 0 || (size_t)victim >= units_.size()) return 0;
    if (!battle_nsp_load("death.nsp") || magic_frames_.empty()) return 0;

    int cx = 0, cy = 0;
    unit_screen_center(units_[(size_t)victim], cx, cy);            // sub_135EB
    const int nframes = (int)magic_frames_.size();

    // 5FC21 turns the 100h copies of palette[1] (unk_12824F) into delta[i] = palette[1] -
    // master[i]; every 5FD1C below writes DAC[i] = master[i] + delta[i]*pct/100, i.e. exactly
    // pal_shift_fade(pct) over the full 100h entries (ASM 31727-31813).
    sfx(0x33);                                                     // sub_4F4CD(33h)
    battle_dim_row0_ = true;                                       // sub_44D6E(1)
    auto fade = [&](int pct) {
        pal_shift_fade(pct, 0x100);                                // sub_5FD1C(0,100h,...)
        video_.present();
        wait_retrace(1);
    };
    for (int pass = 0; pass < 2 && !should_exit_; ++pass) {
        // loc_26D34: pct 0,14h,28h,3Ch,50h, then a single 45559 present.
        for (int t = 0; t <= 0x50 && !should_exit_; t += 0x14) fade(t);
        battle_fx_present_454b3();                                 // sub_45559 = 454B3 + 5E9E8
        // loc_26D58: si is pushed for 5FD1C before `sub esi,0Ah`, so pct is the pre-decrement
        // value: 64h,5Ah,50h,...,0Ah (10 steps). The loop ends at si==0, so pct 0 is never called.
        for (int t = 0x64; t > 0 && !should_exit_; t -= 0x0A) fade(t);
        if (pass == 1) break;                                     // cmp di,1 -> 26E0A
        game_timer_set(0x10);                                     // sub_1033B(10h)
        for (int i = 0; i < nframes && !should_exit_; ++i) {
            battle_fx_blit(i, cx, cy - 0x20);                     // sub_1FFFE + sub_5EE51
            battle_fx_present_454b3();                            // sub_454B3 -> 1FFFE -> 5E9E8
            wait_game_tick();                                     // sub_499E4
            if (i == 5) {                                         // frame 5: two extra ticks plus sound
                wait_game_tick();
                wait_game_tick();
                sfx(0x32);                                        // sub_4F4CD(32h)
            }
        }
        battle_dim_row0_ = false;                                 // sub_44D6E(0)
    }
    battle_dim_row0_ = false;                                     // port cleanup on early exit
    battle_fx_present_454b3();                                    // sub_45559 = 454B3 + 5E9E8
    battle_nsp_release();                                         // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 26C9A death victim=%d center=(%d,%d)\n", units_[(size_t)victim].id, cx, cy);
    return 1;
}

// ---- sub_26E1F: Black Hole (q1 61) ----
//   eax/edx = caster coordinates in 16.16, ebx (bl) = damage flag (>0 runs the expand phase).
//   dword_1FE74/dword_1FE84/loc_1FE94 hold 16 spiral vectors (+-4). Each frame:
//     1. 5F611 draws table entry 16 (dword_F0E72) at radius var_8 (0x3C, -4 per frame)
//     2. the 16 particles are drawn with 5EE51 at their own frames (entry i) at
//        x = centre - t3[i]*t1[i], y = centre - t2[i]*t3[i]; t3 decays every frame
//     3. 12E01 + 454D5 + 5E9E8 + 499E4.
//   Once the radius drops to 1 or less:
//     flag>0 -> from edi=2 up to 0x28*flag in steps of flag*4 the same particles are expanded with
//               5F570 (entry 16, size edi), then 30 frames of { 5F570 (size kept), particles,
//               1FFFE(0x11+(i&1)), 4F4CD(0x33) and one extra tick when 499BD(3)==0 },
//     flag<=0 -> only 30 frames of { 5EE51(entry 16, 82h,4Bh), particles, 5E9E8, 499E4 }.
//   1FFCD releases the table at the end.
//   5F611/5F570 are ported in combatii_fx_gfx.cpp as battle_fx_blit_scaled.
int CombatII::battle_fx_26e1f(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("b_hole.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();

    // dword_1FE74, dword_1FE84 and loc_1FE94 (ASM 22912-22914) are copied to the stack at
    // entry. loc_1FE94 doubles as the per-particle frame counter: the original decrements the
    // byte after each draw (see draw_particles).
    static const int8_t kT1[16] = {4, 3, 2, 1, 0, -1, -2, -3, -4, 3, 2, 1, 0, -1, -2, -3};
    static const int8_t kT2[16] = {0, 1, 2, 3, 4, 3, 2, 1, 0, -1, -2, -3, -4, -3, -2, -1};
    int8_t t3[16] = {0x14, 0x12, 0x10, 0x18, 0x09, 0x0A, 0x08, 0x06,
                     0x04, 0x0C, 0x04, 0x10, 0x08, 0x14, 0x0C, 0x0E};

    int cx = 0, cy = 0;
    unit_screen_center(units_[(size_t)unit_index], cx, cy);        // sub_10971+0Ch / sub_10988+4
    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                     (int)(int8_t)units_[(size_t)unit_index].y);   // sub_1124A
    // dword_F0E72: every blob phase draws table entry 16 (dword_F0E6C/F0E6E are its +2/+4).
    const int blob_frame = std::min(nframes - 1, 0x10);

    // x = centre - t3[i]*t1[i] (eax), y = centre - t2[i]*t3[i] (edx) before 5EE51. The frame is
    // entry i itself (F0DAA + i*12) and no frame offset is added.
    //   phase 0 (shrink + no damage): t3-- (reset 0Ah when the pre-decrement value is below 4).
    //   phase 1 (expand + 30 hold frames): t3 -= 2 (reset (di>>3)+0Ah when the result is below 4).
    auto draw_particles = [&](int phase, int di) {
        for (int i = 0; i < 16; ++i) {
            battle_fx_blit(i, cx - t3[i] * kT1[i], cy - kT2[i] * t3[i]);  // sub_5EE51
            const int before = t3[i];
            if (phase == 0) {
                t3[i] = (int8_t)(before < 4 ? 0x0A : before - 1);
            } else {
                const int next = before - 2;
                t3[i] = (int8_t)(next < 4 ? (di >> 3) + 0x0A : next);
            }
        }
    };

    game_timer_set(7);                                              // sub_1033B(7)
    sfx(0x32);                                                      // sub_4F4CD(32h)

    // (1) Shrink phase: radius 0x3C decreasing by 4 down to 1 or less (15 frames). sub_5F611
    //     puts the top-left at centre + size/2 + entry offset (ASM 31896-31907).
    int radius = 0x3C;
    for (int guard = 0; guard < 0x40 && radius > 1 && !should_exit_; ++guard) {
        if (radius > 0) {
            const MagicFxFrame& fr = magic_frames_[(size_t)blob_frame];
            battle_fx_blit_scaled(blob_frame, cx + radius / 2 + fr.xo,
                                  cy + radius / 2 + fr.yo, radius, radius, false, true);
        }
        draw_particles(0, 0);
        battle_fx_present_12d65();
        radius -= 4;
        wait_game_tick();
    }

    int di = 2;                                                     // expand size counter
    if (k > 0) {
        // (2) Expand phase: edi from 2 to 0x28*k in steps of k*4, 5F570 centred on the caster.
        for (; di < 0x28 * k && !should_exit_; di += k * 4) {
            const MagicFxFrame& fr = magic_frames_[(size_t)blob_frame];
            battle_fx_blit_scaled(blob_frame, cx - di / 2 + fr.xo, cy - di / 2 + fr.yo,
                                  di, di, true);
            draw_particles(1, di);
            battle_fx_present_12d65();
            wait_game_tick();
        }
        // (3) 30 frames: 5F570 keeps the final size counter and entry 16 while 1FFFE draws
        //     0x11+(counter&1) at the centre; a failed 499BD(3) inserts one extra tick.
        for (int i = 0; i < 0x1E && !should_exit_; ++i) {
            const MagicFxFrame& fr = magic_frames_[(size_t)blob_frame];
            battle_fx_blit_scaled(blob_frame, cx - di / 2 + fr.xo, cy - di / 2 + fr.yo,
                                  di, di, true);
            draw_particles(1, di);
            const int f = std::min(nframes - 1, 0x11 + (i & 1));
            battle_fx_blit(f, cx + magic_frames_[(size_t)f].xo,
                           cy + magic_frames_[(size_t)f].yo);        // sub_1FFFE
            if (combat_rand(3) == 0) {                              // sub_499BD(3) == 0
                sfx(0x33);                                          // sub_4F4CD(33h)
                wait_game_tick();
            }
            battle_fx_present_12d65();
            wait_game_tick();
        }
    } else {
        // (3') No-damage variant: 30 frames of the plain entry-16 blit at (82h,4Bh), no scaling.
        for (int i = 0; i < 0x1E && !should_exit_; ++i) {
            battle_fx_blit(blob_frame, 0x82, 0x4B);                 // sub_5EE51
            draw_particles(0, 0);
            battle_fx_present_12d65();
            wait_game_tick();
        }
    }
    battle_nsp_release();                                           // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 26E1F b_hole unit=%d flag=%d center=(%d,%d)\n",
                units_[(size_t)unit_index].id, k, cx, cy);
    return 1;
}

// ---- sub_276B9: Amagedon (q1 62 k2 / Dark Zone family 1) ----
//   eax = caster, edx = target tile x (low word). amagedon.nsp, frame count ecx=0Ah (10).
//   1124A(caster) -> 1033B(5) -> 4F4CD(0x32). Step 1 runs 28 frames (si<0x1Ch):
//     shake = (si>>1)&1 ? 0 : -4, frame = si>>2 (0..6), drawn with 5F570.
//     The target size runs 10..37 (+1 per frame) but the centring uses the +1 value, i.e.
//     top-left = 0xA6/0x64 - (size+1)/2 + frame offset, half a pixel larger than the blit.
//     Both counters (width/height) start at 0Ah: 1FEA4 keeps the passed ecx=0Ah and 1124A
//     preserves ecx itself.
//   The palette is then rebuilt from 100h copies of palette[1] (5F945/5FC21), 4F4CD(0x33)
//   plays and 50
//   frames run (si<0x64, si+=2) { 5E943 + 5FD1C(palette, si, end=0FEh), shake (di>>1)&1, 454B3,
//   frame = di%3+4 cycling 4,5,6, 5F570 (the counter grows by 3 before the blit, so
//   41,44,...,188; centring uses size/2 rounded down) } -> 454D5+5E9E8+499E4.
//   Finally 1033B(0x0C) + 45559, the palette steps back by 3 while si>3 (no sprites), 5F9AE
//   restores the master, dword_7001C=0Ch, shake 0, 1FFCD.
//   5F570 is battle_fx_blit_scaled (expand only) and the palette work is
//   pal_shift_fade (5F945+5FC21+5FD1C).
int CombatII::battle_fx_276b9(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("amagedon.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();

    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                     (int)(int8_t)units_[(size_t)unit_index].y);   // sub_1124A
    game_timer_set(5);                                             // sub_1033B(5)
    sfx(0x32);                                                     // sub_4F4CD(32h)

    // 5F570: draw at centre (0xA6,0x64) with the target size (w,h).
    //   Top-left = centre - target/2 + frame offset (xo/yo); the original divides by the target
    //   size, not the sprite size. Only step 1 increments the counter before the sar 1, so its
    //   centring is (size+1)/2 while step 2 uses size/2.
    auto blit = [&](int frame, int w, int h, bool ceil_half) {
        const int f = std::max(0, std::min(nframes - 1, frame));
        const MagicFxFrame& fr = magic_frames_[(size_t)f];
        const int hx = ceil_half ? (w + 1) / 2 : w / 2;
        const int hy = ceil_half ? (h + 1) / 2 : h / 2;
        battle_fx_blit_scaled(f, 0xA6 - hx + fr.xo, 0x64 - hy + fr.yo, w, h, true);
    };

    int w = 0x0A, h = 0x0A;                    // var_8 (width) / var_4 (height)
    // Step 1: 28 frames, frame = si>>2 (0..6), size +1 per frame (10..37), centring (size+1)/2.
    //   Original frame order: shake -> sub_10AEB -> sub_12D65 -> 5F570 -> sub_12E01 -> sub_454D5
    //   -> sub_5E9E8 -> sub_499E4.
    for (int i = 0; i < 0x1C && !should_exit_; ++i) {
        cam_fine_y_ = ((i >> 1) & 1) ? 0 : -4;                     // low word of dword_BA838
        battle_fx_world_12d65();                                   // sub_10AEB + sub_12D65
        blit(i >> 2, w, h, true);                                  // sub_5F570
        battle_fx_flush();                                         // sub_12E01
        battle_fx_overlay();                                       // sub_454D5
        battle_fx_commit();                                        // sub_5E9E8
        wait_game_tick();                                          // sub_499E4
        ++w; ++h;
    }
    // 5F945 (master copy) -> 5FC21 (copy+3 minus master = delta) -> 4F4CD(33h).
    sfx(0x33);                                                       // sub_4F4CD(33h)

    // Step 2: 50 frames (si 0 -> 0x64, +2), frame = di%3+4 (cycling 4,5,6), size +3 before the
    // blit (41..188).
    for (int i = 0, di = 0; i < 0x64 && !should_exit_; i += 2, ++di) {
        // Explicit sub_5E943 plus sub_5FD1C's own 5E943: two retraces before the palette write.
        // The trailing 499E4 (period 5 = the same 28.5ms) then returns immediately.
        wait_retrace(2);
        cam_fine_y_ = (int16_t)(((di >> 1) & 1) ? 0 : -4);
        pal_shift_fade(i, 0xFE);                                   // sub_5FD1C (si, end 0FEh)
        battle_fx_world_454b3_12d65();                             // sub_454B3 + sub_12D65
        w += 3; h += 3;                                            // the original adds 3 before the blit
        blit(di % 3 + 4, w, h, false);                             // sub_5F570
        battle_fx_flush();                                         // sub_12E01
        battle_fx_overlay();                                       // sub_454D5
        battle_fx_commit();                                        // sub_5E9E8
        wait_game_tick();                                          // sub_499E4
    }
    // Step 3: 1033B(0x0C) + 45559 + 499E4, then palette restore (-3 while si>3: 5E943 + 5FD1C = 2 retraces).
    game_timer_set(0x0C);                                          // sub_1033B(0Ch)
    present_cutscene_frame();                                      // sub_45559
    wait_game_tick();                                              // sub_499E4
    for (int t = 0x64; t > 3 && !should_exit_; t -= 3) {
        wait_retrace(2);                                           // sub_5E943 + sub_5FD1C(5E943)
        pal_shift_fade(t, 0xFE);                                   // 5FD1C restore (end 0FEh)
        present_cutscene_frame();
    }
    cam_fine_x_ = CAM_FINE_X_REST;                                 // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                                 // dword_BA838 = 0
    wait_retrace(1);                                              // 5F9AE: wait for the retrace edge, then write the DAC
    pal_load_range(0, 256, &battle_master_palette_[0][0]);         // 5F9AE restore master
    video_.present();
    battle_nsp_release();                                          // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 276B9 amagedon unit=%d\n", units_[(size_t)unit_index].id);
    return 1;
}

// ---- sub_2799A: Atomic Blast (q1 62 k1) ----
//   eax = caster, edx = target tile x. atomicbl.nsp, ecx=0Ch -> dword_7001C=0Ch, and the size
//   counters di/si also start at 0Ah (esi=0Ah -> edi=esi). No shake.
//   1124A -> 5F945 (master copy 0..100h) -> 1033B(5) -> 4F4CD(0x32):
//     - 24 frames (bx<0x18): shake (i>>1)&1, 454B3 -> 5F570 (frame = i>>2 -> 0..5, centre
//       0xA6/0x64, width/height = counter) -> 454D5+5E9E8+499E4, counter +1
//     - 24 frames (var_4<0x18): frame = var_4%2+4 (alternating 4,5), same 5F570 (counter +3);
//       afterwards 27B88 rotates the copy's R channel by one (%) -> 5F9AE (copy -> DAC)
//     - copy+3 -> 5FC21 (delta) -> 45559 -> 5FD1C stepping down by 4 from si=64h ->
//       5F9AE (master) + dword_7001C=0Ch + shake 0 -> 1FFCD
//   5F570 is battle_fx_blit_scaled (expand only); the rotation is pal_cycle_entries.
int CombatII::battle_fx_2799a(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("atomicbl.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();

    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                     (int)(int8_t)units_[(size_t)unit_index].y);   // sub_1124A
    pal_snapshot_range(0, 256, fx_pal_cycle_);                     // sub_5F945 (copy the step 2 rotation works on)
    game_timer_set(5);                                             // sub_1033B(5)
    cam_fine_x_ = CAM_FINE_X_REST;                                 // dword_7001C = 0Ch
    sfx(0x32);                                                     // sub_4F4CD(32h)

    // 5F570: centre (0xA6,0x64), target size = the square counter value.
    //   Step 1 puts the top-left at centre - (size+1)/2 and step 2 at centre - size/2 (the
    //   original applies `inc` then `sar 1` only in step 1).
    auto blit = [&](int frame, int s, bool ceil_half) {
        const int f = std::max(0, std::min(nframes - 1, frame));
        const MagicFxFrame& fr = magic_frames_[(size_t)f];
        const int half = ceil_half ? (s + 1) / 2 : s / 2;
        battle_fx_blit_scaled(f, 0xA6 - half + fr.xo, 0x64 - half + fr.yo, s, s, true);
    };

    int s = 0x0A;                              // di=si=0Ah: both target size counters
    // Step 1: 24 frames, frame = i>>2 (0..5), size +1 per frame (10 -> 33).
    //   Original order: shake -> sub_10AEB -> sub_12D65 -> 5F570 -> sub_12E01 -> sub_454D5 ->
    //   sub_5E9E8 -> sub_499E4.
    for (int i = 0; i < 0x18 && !should_exit_; ++i) {
        cam_fine_y_ = (int16_t)(((i >> 1) & 1) ? 0 : -4);
        battle_fx_world_12d65();                                   // sub_10AEB + sub_12D65
        blit(i >> 2, s, true);                                     // sub_5F570
        battle_fx_flush();                                         // sub_12E01
        battle_fx_overlay();                                       // sub_454D5
        battle_fx_commit();                                        // sub_5E9E8
        wait_game_tick();                                          // sub_499E4
        ++s;
    }
    // Step 2: 24 frames, frame = i%2+4 (alternating 4,5), size +3 before the blit
    //   (37,40,...,106), centring size/2 rounded down. The palette R channel rotates afterwards.
    for (int i = 0; i < 0x18 && !should_exit_; ++i) {
        cam_fine_y_ = (int16_t)(((i >> 1) & 1) ? 0 : -4);
        battle_fx_world_12d65();                                   // sub_10AEB + sub_12D65
        s += 3;
        blit(i % 2 + 4, s, false);                                 // sub_5F570
        battle_fx_flush();                                         // sub_12E01
        battle_fx_overlay();                                       // sub_454D5
        battle_fx_commit();                                        // sub_5E9E8
        wait_game_tick();                                          // sub_499E4
        pal_cycle_entries(fx_pal_cycle_, 256);                     // loc_27B88
        wait_retrace(1);                                           // sub_5F9AE waits before the DAC write
        pal_load_range(0, 256, fx_pal_cycle_);                     // sub_5F9AE (rotated copy)
    }
    // Step 3: 45559 + palette restore (si=0x64 down by 4, no sprites, 1 retrace/step) + 5F9AE + shake 0.
    present_cutscene_frame();                                      // sub_45559
    for (int t = 0x64; t > 4 && !should_exit_; t -= 4) {
        wait_retrace(1);                                           // sub_5E943 inside sub_5FD1C
        pal_shift_fade(t);                                         // 5FD1C
        present_cutscene_frame();
    }
    cam_fine_x_ = CAM_FINE_X_REST;                                 // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;
    wait_retrace(1);                                              // 5F9AE: wait for the retrace edge, then write the DAC
    pal_load_range(0, 256, &battle_master_palette_[0][0]);         // sub_5F9AE restore master
    video_.present();
    battle_nsp_release();                                          // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 2799A atomicbl unit=%d\n", units_[(size_t)unit_index].id);
    return 1;
}

// ---- sub_27C5A: Amagedon full-screen variant (q1 62 k2 variant) ----
//   eax = caster, edx = target tile x, ecx=1 so the size counter starts at 1. amagedon.nsp,
//   1033B(5), 4F4CD(0x32).
//   - 3 frames (di<3): frame = di>>2 (=0), shake (di>>1)&1, 5F570 is drawn five times at
//     different centres: (64h,46h) (E8h,46h) (A6h,64h) (64h,82h) (E8h,82h).
//     Top-left = centre - (target+1)/2 + frame offset (xo/yo) and the size grows by 1 per blit
//     (1..180).
//   - copy+3 -> 5FC21 (delta) -> 4F4CD(0x33) -> from di=1 in steps of 3: 5FD1C(di) + shake
//     (di>>1)&1 + 454B3/12D65 draw frame = (var_8%3)+4 (cycling 5,6,4) at the same five centres.
//   - while di>3 only the palette steps back by 3 (no sprites) -> 5F9AE (master) +
//     dword_7001C=0Ch -> 1FFCD.
//   The five centres, the growth, the frame cycle and the palette steps are ported as-is.
int CombatII::battle_fx_27c5a(int unit_index, int tx, int ty, int k) {
    (void)tx; (void)ty; (void)k;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (!battle_nsp_load("amagedon.nsp") || magic_frames_.empty()) return 0;
    const int nframes = (int)magic_frames_.size();

    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                     (int)(int8_t)units_[(size_t)unit_index].y);   // sub_1124A
    game_timer_set(5);                                             // sub_1033B(5)
    sfx(0x32);                                                     // sub_4F4CD(32h)

    // The five centres 5F570 uses (x, y), the 64h/E8h/A6h/46h/82h constants above.
    static const int kCentersX[5] = {0x64, 0xE8, 0xA6, 0x64, 0xE8};
    static const int kCentersY[5] = {0x46, 0x46, 0x64, 0x82, 0x82};

    // The size grows by 1 per blit: frame 0 uses 1,2,3,4,5, frame 3 uses 16..20 and the last
    //   uses 176..180. Top-left = centre - (size+1)/2 + offset.
    auto place_all = [&](int frame, int& s) {
        const int f = std::max(0, std::min(nframes - 1, frame));
        const MagicFxFrame& fr = magic_frames_[(size_t)f];
        for (int p = 0; p < 5; ++p) {
            const int half = (s + 1) / 2;
            battle_fx_blit_scaled(f, kCentersX[p] - half + fr.xo,
                                  kCentersY[p] - half + fr.yo, s, s, true);
            ++s;
        }
    };

    int size = 1;                                                  // var_C/var_4: ecx=1 persists
    int fc = 1;                                                    // frame counter: (fc%3)+4 = 5,6,4
    // Step 1: 3 frames, frame = di>>2 (=0), the five blits consume sizes 1..15.
    //   Original order: shake -> sub_10AEB -> sub_12D65 -> 5F570 x5 -> sub_12E01 -> sub_454D5 ->
    //   sub_5E9E8 -> sub_499E4.
    for (int i = 0; i < 3 && !should_exit_; ++i) {
        cam_fine_y_ = (int16_t)(((i >> 1) & 1) ? 0 : -4);
        battle_fx_world_12d65();                                   // sub_10AEB + sub_12D65
        place_all(0, size);                                        // sub_5F570 x5
        battle_fx_flush();                                         // sub_12E01
        battle_fx_overlay();                                       // sub_454D5
        battle_fx_commit();                                        // sub_5E9E8
        wait_game_tick();                                          // sub_499E4
        ++fc;
    }
    sfx(0x33);                                                     // sub_4F4CD(33h)
    // Step 2: di 1 -> 61h (+3), palette 5FD1C(di) = 1,4,...,97, frame = (fc%3)+4 cycling 5,6,4.
    //   This loop has no sub_499E4 in the original (it ends with 12E01 -> 454D5 -> 5E9E8);
    //   5FD1C calls sub_5E943 (vertical retrace) once on entry, pacing the frame at 70.086Hz.
    for (int di = 1; di < 0x64 && !should_exit_; di += 3) {
        cam_fine_y_ = (int16_t)(((fc >> 1) & 1) ? 0 : -4);
        pal_shift_fade(di);                                        // sub_5FD1C
        battle_fx_world_12d65();                                   // sub_10AEB + sub_12D65
        place_all((fc % 3) + 4, size);                             // sub_5F570 x5
        battle_fx_flush();                                         // sub_12E01
        battle_fx_overlay();                                       // sub_454D5
        battle_fx_commit();                                        // sub_5E9E8
        wait_retrace(1);                                           // sub_5E943 inside sub_5FD1C
        ++fc;
    }
    // Step 3: restore the palette only (sub_5E943 + sub_5FD1C = 2 retraces/step); pct is the
    // pre-decrement value, 100,97,...,4.
    for (int t = 0x64; t > 3 && !should_exit_; t -= 3) {
        wait_retrace(2);                                           // sub_5E943 + sub_5FD1C(5E943)
        pal_shift_fade(t, 0xFE);                                   // 5FD1C restore (end 0FEh)
        present_cutscene_frame();
    }
    cam_fine_x_ = CAM_FINE_X_REST;                                 // dword_7001C = 0Ch
    cam_fine_y_ = CAM_FINE_Y_REST;                                 // dword_BA838 = 0
    wait_retrace(1);                                              // 5F9AE: wait for the retrace edge, then write the DAC
    pal_load_range(0, 256, &battle_master_palette_[0][0]);         // sub_5F9AE restore master
    video_.present();
    battle_nsp_release();                                          // sub_1FFCD
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 27C5A amagedon-multi unit=%d\n", units_[(size_t)unit_index].id);
    return 1;
}
