// sub_2D7D2 -- swstr.nsp (sword strike)
// ASM: dasm/COMBATII_code.ASM 40678..40888. Caller: `sub_2F5EF+6E` (dword_F1474 idx 6).
//   Arguments: eax = unit, dl = k (loca). The ebx value set by the caller is not read.
//
// Original structure (read from the ASM):
//   (1) 135EB takes the unit centre (ebx=esp -> [esp]=y, edx=&[esp+4] -> x), then
//       1FEA4("swstr.nsp", 32h) -> 4F460("f-blast.ssp").
//   (2) sub_2D4EB(unit, camera x, camera y): 1033B(19h), then 2 frames ([unit+64h] = 7, 8)
//       of 454B3 -> 5E9E8 -> 499E4.
//   (3) sub_2D48A(23h): cover the whole palette with (3Fh,3Fh,3Fh) for a grey flash and
//       restore the master palette (unk_12824C) after 23 ticks.
//   (4) var_4 = [unit+63h](facing) * 2, var_8 = k*8 + 0Ch (loop count), [unit+64h] = 9, 1033B(8).
//   (5) each frame:
//       - 454B3 (background + units); while si < var_8-4, var_4++ (swing progress = frame index).
//       - three afterimages: si>2 && si<var_8-3 -> frame (var_4-3)%8 in colour 1Ch,
//                            si>1 && si<var_8-2 -> (var_4-2)%8 in 1Bh,
//                            si>0 && si<var_8-1 -> (var_4-1)%8 in 1Ah, as flat silhouettes (5F093).
//       - main frame: (var_4%8) through 1FFFE(x=centre x, y=centre y), which adds the entry's xo/yo.
//       - redraw the unit with 12F2A(unit, 2), then 5E9E8 -> 499E4.
//       - when var_4 is odd, rotate the facing [63h] = ([63h]+1)%4 and play 4F4CD(32h).
//   (6) after the loop: 12124 (restore pose) -> 1FFCD (release table) -> eax=1.
//
// Port mapping: 1FEA4 -> battle_nsp_load, 4F460 -> battle_ssp_play, 135EB -> unit_screen_center,
//   454B3 -> battle_fx_world, 5E9E8 -> battle_fx_commit, 499E4 -> wait_game_tick,
//   1033B -> game_timer_set, 4F4CD -> sfx, 12F2A -> battle_draw_unit_now, 12124 -> reset_unit_pose,
//   1FFCD -> battle_nsp_release, 5F093/5EE51 -> the file-local silhouette/flat blits,
//   5F9AE -> pal_load_range.
//
// Approximations:
//   - IDA stack aliases: `[esp+6]>>16` = var_8 (loop count), `[esp+0Ah]>>16` = var_4 (swing
//     counter), `[esp+2]>>16` = centre x and `[esp+10h+var_12]>>16` = centre y match the sites
//     where they are computed (var_8/var_4 are stored as words at [esp+8]/[esp+0Ch], the centre
//     as the words 135EB writes to [esp+4]/[esp]).
//   - The original writes 5F093/1FFFE straight into the canvas between 454B3 and 5E9E8. The
//     port keeps that order: battle_fx_world() -> immediate silhouette/flat blits ->
//     battle_draw_unit_now -> battle_fx_commit(). Queueing through battle_fx_blit would draw
//     the background later and invert the order.
//   - 2D4EB's camera arguments (edx/ebx) are unused because 454B3 reads the global camera.
//   - 2D48A fills the palette with grey, waits al ticks and restores it; the port reproduces
//     the order with two 5F9AE calls plus one game_timer_set/wait_game_tick.
//   - The flat-colour argument ecx (1Ch/1Bh/1Ah) of 5F093 is used as the palette index.
//   - 2D4EB's two frames only call 454B3 + 5E9E8, but the port uses battle_fx_present
//     (454B3 + 5E9E8 + 454D5), so the HUD/overlay is drawn once more (the same approximation
//     as the other drivers).
#include "combatii.h"
#include "combatii_internal.h"
#include <cstdint>
#include <vector>

namespace {

// Counterpart of sub_5EB00: canvas coordinates -> backbuffer pixel, dropping off-screen points.
inline void swstr_put(uint8_t* bb, int x, int y, uint8_t px) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    bb[(size_t)y * (size_t)SCREEN_W + (size_t)x] = px;
}

// sub_5F093(ebx=[w][h]+pixels, eax=x, edx=y, cl=flat colour): paint every non-0FFh pixel
//   with colour cl. The original writes dh (=cl) at the canvas address from sub_5EB00 and
//   leaves 0FFh untouched.
void swstr_silhouette(uint8_t* bb, const std::vector<uint8_t>& flat, int w, int h,
                      int x, int y, uint8_t color) {
    if (!bb || flat.empty() || w <= 0 || h <= 0) return;
    if ((size_t)w * (size_t)h > flat.size()) return;
    for (int r = 0; r < h; ++r) {
        const size_t row = (size_t)r * (size_t)w;
        for (int c = 0; c < w; ++c) {
            if (flat[row + (size_t)c] == 0xFF) continue;      // keep transparent pixels
            swstr_put(bb, x + c, y + r, color);
        }
    }
}

// sub_5EE51(eax=x, edx=y, ebx=[w][h]+pixels): blit in the original colours, skipping 0FFh.
void swstr_blit_flat(uint8_t* bb, const std::vector<uint8_t>& flat, int w, int h, int x, int y) {
    if (!bb || flat.empty() || w <= 0 || h <= 0) return;
    if ((size_t)w * (size_t)h > flat.size()) return;
    for (int r = 0; r < h; ++r) {
        const size_t row = (size_t)r * (size_t)w;
        for (int c = 0; c < w; ++c) {
            const uint8_t px = flat[row + (size_t)c];
            if (px == 0xFF) continue;
            swstr_put(bb, x + c, y + r, px);
        }
    }
}

}  // namespace

int CombatII::battle_fx_2d7d2(int unit_index, int k) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const int kk = (k < 0) ? 0 : k;

    int cx = 0, cy = 0;
    unit_screen_center(units_[(size_t)unit_index], cx, cy);          // sub_135EB
    if (!battle_nsp_load("swstr.nsp") || magic_frames_.empty()) return 0;  // sub_1FEA4
    const int nframes = (int)magic_frames_.size();
    battle_ssp_play("f-blast.ssp");                                  // sub_4F460

    // (2) sub_2D4EB: two attack-pose frames (1033B(19h) -> 454B3 + 5E9E8 + 499E4 x2)
    game_timer_set(0x19);
    for (int i = 0; i < 2 && !should_exit_; ++i) {
        if ((size_t)unit_index >= units_.size()) break;
        units_[(size_t)unit_index].anim_frame = (uint8_t)(7 + i);    // [64h] = 7, 8
        battle_fx_present();                                        // 454B3 + 5E9E8
        wait_game_tick();                                           // sub_499E4
    }
    if (should_exit_) { battle_nsp_release(); return 1; }

    // (3) sub_2D48A(23h): cover the whole palette with grey 3Fh and restore it 23 ticks later.
    {
        uint8_t grey[0x100 * 3];
        for (int i = 0; i < 0x100; ++i) {
            grey[i * 3 + 0] = 0x3F;
            grey[i * 3 + 1] = 0x3F;
            grey[i * 3 + 2] = 0x3F;
        }
        pal_load_range(0, 0x100, grey);                              // sub_5F9AE(grey buffer)
        game_timer_set(0x23);                                        // sub_1033B(al)
        wait_game_tick();                                            // sub_499E4
        pal_load_range(0, 0x100, &battle_palette_[0][0]);            // sub_5F9AE(unk_12824C)
    }

    // (4) var_4 = facing*2, var_8 = k*8+0Ch, [64h]=9, 1033B(8)
    int16_t swing = (int16_t)((int)(units_[(size_t)unit_index].facing & 3) * 2);
    const int count = kk * 8 + 0x0C;
    if ((size_t)unit_index < units_.size()) units_[(size_t)unit_index].anim_frame = 9;
    game_timer_set(8);

    // (5) swing loop
    for (int si = 0; si < count && !should_exit_; ++si) {
        if ((size_t)unit_index >= units_.size()) break;
        battle_fx_world();                                          // sub_454B3
        if (si < count - 4) ++swing;                                 // `cmp si, var_8-4; jge`

        uint8_t* bb = video_.get_backbuffer();
        auto silhouette = [&](int rel, int color) {                   // sub_5F093
            int f = (int)((swing + rel) % 8);                        // `idiv 8` remainder
            if (f < 0 || f >= nframes) return;
            const MagicFxFrame& fr = magic_frames_[(size_t)f];
            swstr_silhouette(bb, fr.flat, fr.w, fr.h,
                             cx + fr.xo, cy + fr.yo, (uint8_t)color);
        };
        if (si > 2 && si < count - 3) silhouette(-3, 0x1C);
        if (si > 1 && si < count - 2) silhouette(-2, 0x1B);
        if (si > 0 && si < count - 1) silhouette(-1, 0x1A);

        const int mainf = (int)(swing % 8);                          // var_4 % 8
        if (mainf >= 0 && mainf < nframes) {                        // sub_1FFFE
            const MagicFxFrame& fr = magic_frames_[(size_t)mainf];
            swstr_blit_flat(bb, fr.flat, fr.w, fr.h, cx + fr.xo, cy + fr.yo);
        }

        battle_draw_unit_now(unit_index);                            // sub_12F2A(ebx=2)
        battle_fx_commit();                                         // sub_5E9E8
        wait_game_tick();                                           // sub_499E4

        if ((swing & 1) != 0) {                                     // var_4 % 2
            if ((size_t)unit_index < units_.size()) {
                auto& uu = units_[(size_t)unit_index];
                uu.facing = (int8_t)(((int)uu.facing + 1) & 3);      // [63h] = (+1)%4
            }
            sfx(0x32);                                              // sub_4F4CD(32h)
        }
    }

    // (6) 12124 -> 1FFCD
    if ((size_t)unit_index < units_.size()) reset_unit_pose(units_[(size_t)unit_index]);
    battle_nsp_release();                                           // sub_1FFCD
    return 1;
}
