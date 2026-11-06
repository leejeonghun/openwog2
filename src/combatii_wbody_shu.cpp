// sub_377A6: shu.nsp (V-weapon edi 9)
// ASM: dasm/COMBATII_code.ASM 52968..53597. Called from sub_302E6 case 9 after
//   pazizik.ssp (32h) and f-blast.ssp (33h).
//
// Original flow:
//   0. `[F0B9C+2]=0` clears the target list; ebx=7 is stored in var_4.
//   1. Targets: 1A1DE (front tile) -> 1A452 (width 1, length 7) -> 1A552/1A1F8.
//      Facing 1/3 (west/east) offsets the start y, 0/2 (south/north) the start x, by
//      -1/0/+1, sweeping three rows of a 3-wide by 7-deep corridor. An empty list
//      returns -1 (loc_378C0).
//   2. 1FEA4 (shu.nsp) - 103A8 (weather tick off) - 200DE (caster) - 1033B(5) -
//      44D89 (all targets) -> 454B3 -> 44D6E(1) -> 30824(0,28h) -> 5E9E8 -> 499E4.
//   3. cx = (facing 1|3) ? 0 : 10h - the NSP 16-frame block selector (horizontal /
//      vertical). Anchor = 10971(x)+0Ch / 10988(y)-2. 11EE4 (caster), 1214F (target
//      pose), 1033B(0Fh).
//   4. 10 frames (edi 0..9): 454B3 -> frame cx+edi -> per-facing blit -> 5E9E8 ->
//      499E4; every edi%3==0 emits 4F4CD(33h).
//   5. 3 waves x 4 frames (loc_37AAE): frames cx+8, cx+7 (dec loop) -> 4F4CD(33h) ->
//      frames cx+8, cx+9 (inc loop).
//   6. 4 slides x 3 frames (loc_37C98): fixed frame cx+0Ah, anchor moved along the
//      facing (west -10h / east +10h / south +0Ch / north -0Ch). 1033B(5), 4F4CD(32h).
//   7. One final frame: cx+0Bh.
//   8. Tail: 44D75 -> 5F9AE (master palette) -> 44D6E(0) -> 5E9E8 -> 499E4 ->
//      11E04 (caster) -> target `[1Eh]|=4`/`[1Fh]=5Ah` -> 2CAF7(2, 2710h) -> 1FFCD ->
//      103A0.
//
// Per-facing blit - entry is 12 bytes { [+0]=w, [+2]=h, [+4]=xo, [+6]=yo, [+8]=flat }:
//   facing 3: 5F0CF - x = anchor x - w - xo, y = anchor y + yo    (mirror-X blitter)
//   facing 2: 5F1AB - x = anchor x + xo,     y = anchor y - h - yo (mirror-Y blitter)
//   otherwise: 5EFB9 - x = anchor x + xo,    y = anchor y + yo    (clipped blitter)
//   5F0CF walks each row backwards with `dec edx`; 5F1AB reverses the row order.
//
// Gameplay: keeps the caller (battle_weapon_377a6) behavior - battle_weapon_collect_screen(0)
//   collection, -1 on an empty list, per-target `[1Eh]|=4`/`[1Fh]=5Ah`, and the
//   2CAF7(2, 2710h) result.
//
// Approximations:
//   - 11EE4/11E04 (release and re-register occupancy) are omitted because the port models
//     occupancy with units_; only 1214F (target pose 0Ah, 0 for sprites >= 12Ch) is kept.
//   - 454B3 does not include 454D5, but drawing the targets registered by 44D89 needs the
//     highlight pass, so battle_fx_overlay() is called too (same as 341C7/34C48).
//   - The second argument of 44D6E(1) (28h) is modelled by the battle_dim_row0_ scalar.
//
// The two coordinate systems in 6/7 (loc_37C98..loc_37EF4): before sliding, snapshots
//   var_10 = var_1C and var_20 = var_8 are taken, and each slide frame draws twice - once
//   at the snapshot position (var_10/var_20) and once at the moving var_1C/var_8, using the
//   same frame (cx+0Ah). Only var_1C/var_8 move at the end of the frame. The final frame
//   (cx+0Bh) is drawn once at the snapshot position.
#include "combatii.h"
#include "combatii_internal.h"

#include <cstdio>
#include <cstring>
#include <vector>

int CombatII::battle_fx_377a6_anim(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    auto& A = units_[(size_t)unit_index];

    // ---- 1. target collection: 1A1DE front tile, then 1A452 (width 1, length 7) on ----
    //   three rows offset by edi = -1..1 -> 1A552 loop + 1A1F8. Facing 1/3 (west/east)
    //   offsets y and 0/2 (south/north) offsets x, sweeping a 3-wide by 7-deep corridor.
    const int facing = (int)A.facing & 3;
    battle_target_list_.clear();
    {
        int fx = 0, fy = 0;
        battle_weapon_front_tile(unit_index, fx, fy);                    // sub_1A1DE
        const g2range::Env env = battle_range_env_make();
        g2range::Range rg(env);
        const g2range::Unit au = battle_range_unit(unit_index);
        for (int off = -1; off <= 1; ++off) {
            int ox = fx, oy = fy;
            if (facing == 1 || facing == 3) oy += off;                   // west/east: column offset
            else                          ox += off;                   // south/north: row offset
            rg.rect_init(ox, oy, 1, au.facing, 7);                      // sub_1A452
            int px = ox, py = oy;
            for (int guard = 0; guard < 0x10000 && rg.rect_next(px, py); ++guard)
                rg.add_by_army(px, py, au.army);                        // sub_1A552 + 1A1F8
        }
        battle_target_list_ = rg.list;
    }
    if (battle_target_list_.empty()) {
        if (battle_trace_) fprintf(stderr, "COMBAT: 377a6 unit=%d empty -> -1\n", A.id);
        return -1;
    }
    const std::vector<int> targets = battle_target_list_;

    // ---- 2. assets; without them only the presentation is skipped, the damage stays ----
    if (!battle_nsp_load("shu.nsp") || magic_frames_.empty()) {          // sub_1FEA4
        if (battle_trace_)
            fprintf(stderr, "COMBAT: warning: 377A6 unit=%d shu.nsp missing (presentation skipped)\n", A.id);
        for (int ti : targets) battle_weapon_apply_status(ti, 0x04, 0x5A);
        return battle_weapon_2caf7_loop(unit_index, 2, 0x2710);
    }
    const int nframes = (int)magic_frames_.size();
    weather_tick_enabled_ = false;                                       // sub_103A8
    camera_center_on((int)(int8_t)A.x, (int)(int8_t)A.y);                // sub_200DE (edx=0)
    game_timer_set(5);                                                   // sub_1033B(5)
    for (int ti : targets) battle_highlight_add(ti);                     // sub_44D89 for all
    battle_fx_world();                                                   // sub_454B3
    battle_dim_row0_ = true;                                             // sub_44D6E(1)
    fx_pal_mix(0x60, 0x100, 0, 0x28);                                    // sub_30824(0, 28h)
    battle_fx_commit();                                                  // sub_5E9E8
    wait_game_tick();                                                    // sub_499E4

    // ---- 3. anchor and facing block ----
    const int block = (facing == 1 || facing == 3) ? 0 : 0x10;           // mov ecx,0/10h
    int ax = (A.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + 0x0C; // 10971+0Ch
    int ay = (A.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y - 2;    // 10988-2
    for (int ti : targets) {                                             // sub_1214F
        if (ti < 0 || (size_t)ti >= units_.size()) continue;
        auto& T = units_[(size_t)ti];
        T.anim_frame = (T.sprite_type < 0x12C) ? (uint8_t)0x0A : (uint8_t)0x00;
    }
    game_timer_set(0x0F);                                                // sub_1033B(0Fh)

    // sub_5F0CF (mirror X) / sub_5F1AB (mirror Y) / sub_5EFB9 (plain): flat [w][h]+px,
    //   0FFh transparent. The original computes the screen offset with 5EB00 and walks the
    //   rows/columns backwards to fill the same box (clipped at 0x140/0xC8, as the port
    //   blitter does).
    auto blit_flat_flip = [&](const std::vector<uint8_t>& flat, int w, int h, int x, int y,
                              bool flip_h, bool flip_v) {
        if (flat.empty() || w <= 0 || h <= 0) return;
        std::vector<uint8_t> buf((size_t)4 + flat.size());
        buf[0] = (uint8_t)(w & 0xFF); buf[1] = (uint8_t)(w >> 8);
        buf[2] = (uint8_t)(h & 0xFF); buf[3] = (uint8_t)(h >> 8);
        if (!flip_h && !flip_v) {
            std::memcpy(buf.data() + 4, flat.data(), flat.size());
        } else {
            for (int row = 0; row < h; ++row) {
                const int srow = flip_v ? (h - 1 - row) : row;
                for (int col = 0; col < w; ++col) {
                    const int scol = flip_h ? (w - 1 - col) : col;
                    buf[(size_t)4 + (size_t)row * (size_t)w + (size_t)col] =
                        flat[(size_t)srow * (size_t)w + (size_t)scol];
                }
            }
        }
        blit_flat_clip319(x, y, buf.data());        // 5EFB9/5F0CF/5F1AB clip at 13Fh/C7h
    };
    // Facing 3 = 5F0CF (mirror X), facing 2 = 5F1AB (mirror Y), otherwise 5EFB9.
    auto blit = [&](int frame_no, int bx, int by) {
        if (frame_no < 0 || (size_t)frame_no >= magic_frames_.size()) return;
        const MagicFxFrame& fr = magic_frames_[(size_t)frame_no];
        int x = bx + fr.xo;
        int y = by + fr.yo;
        if (facing == 3) x = bx - fr.w - fr.xo;
        else if (facing == 2) y = by - fr.h - fr.yo;
        blit_flat_flip(fr.flat, fr.w, fr.h, x, y, facing == 3, facing == 2);
    };
    // 454B3 -> 454D5 -> blit -> 5E9E8 -> 499E4 (original frame order).
    auto frame = [&](int frame_no, int bx, int by) {
        battle_fx_world();                                               // sub_454B3
        battle_fx_overlay();                                             // sub_454D5
        blit(frame_no, bx, by);
        battle_fx_commit();                                              // sub_5E9E8
        wait_game_tick();                                                // sub_499E4
    };

    // ---- 4. ten frames (loc_379BC) ----
    for (int i = 0; i < 0x0A && !should_exit_; ++i) {
        frame(block + i, ax, ay);
        if ((i % 3) == 0) sfx(0x33);                                     // idiv 3, remainder 0
    }

    // ---- 5. 3 waves x 4 frames (loc_37AAE / loc_37B87) ----
    sfx(0x32);                                                           // 4F4CD(32h) before the waves
    for (int wave = 0; wave < 3 && !should_exit_; ++wave) {
        for (int i = 8; i >= 7 && !should_exit_; --i) frame(block + i, ax, ay);
        sfx(0x33);                                                       // 4F4CD(33h)
        for (int i = 8; i <= 9 && !should_exit_; ++i) frame(block + i, ax, ay);
    }

    // ---- 6. 4 slides x 3 frames (loc_37C98) ----
    //   var_10/var_20 = pre-slide snapshot (ax/ay), var_1C/var_8 = moving position (sx/sy).
    game_timer_set(5);                                                   // 1033B(5)
    sfx(0x32);                                                           // 4F4CD(32h)
    int sx = ax, sy = ay;
    for (int rep = 0; rep < 4 && !should_exit_; ++rep) {
        for (int i = 0; i < 3 && !should_exit_; ++i) {
            battle_fx_world();                                           // sub_454B3
            battle_fx_overlay();
            blit(block + 0x0A, ax, ay);                                  // snapshot (var_10/var_20)
            blit(block + 0x0A, sx, sy);                                  // moving (var_1C/var_8)
            battle_fx_commit();                                          // sub_5E9E8
            wait_game_tick();                                            // sub_499E4
        }
        switch (facing) {
            case 1: sx -= 0x10; break;                                   // sub [var_1C],10h
            case 3: sx += 0x10; break;                                   // add [var_1C],10h
            case 0: sy += 0x0C; break;                                   // add [var_8],0Ch
            default: sy -= 0x0C; break;                                  // sub [var_8],0Ch
        }
    }

    // ---- 7. one final frame (cx+0Bh) at the snapshot position (var_10/var_20) ----
    if (!should_exit_) frame(block + 0x0B, ax, ay);

    // ---- 8. tail ----
    battle_highlight_clear();                                            // sub_44D75
    pal_load_range(0, 0x100, &battle_palette_[0][0]);                    // sub_5F9AE (master)
    battle_dim_row0_ = false;                                            // sub_44D6E(0)
    battle_fx_commit();                                                  // sub_5E9E8
    wait_game_tick();                                                    // sub_499E4
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 377A6 unit=%d facing=%d block=%02X targets=%zu frames=%d\n",
                A.id, facing, block, targets.size(), nframes);
    if (should_exit_) return 1;
    // Tail order as in the original: 11E04 -> status -> 2CAF7 -> 1FFCD -> 103A0.
    for (int ti : targets) battle_weapon_apply_status(ti, 0x04, 0x5A);   // [1Eh]|=4, [1Fh]=5Ah
    const int r = battle_weapon_2caf7_loop(unit_index, 2, 0x2710);       // sub_2CAF7(2, 2710h)
    battle_nsp_release();                                                // sub_1FFCD
    weather_tick_enabled_ = true;                                        // sub_103A0
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 377a6 unit=%d -> 2CAF7(2,2710h) -> %d\n", A.id, r);
    return 1;  // loc_37F7C: eax = 1 regardless of the 2CAF7 result
}
