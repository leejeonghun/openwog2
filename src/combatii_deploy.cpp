// Battle start deployment phase (main_ -> sub_55518).
//
// Original flow (COMBATII_code.ASM 94007-94080, main_ 94788-):
//   main_: sub_55B59 (load, includes sub_4ED18) -> sub_55518 -> sub_137D1 -> 4FD25/4FCD9
//           -> 49411 -> 5580F (battle loop)
//   sub_55518:
//     dword_11209E (low) == 0 -> return at once. sub_4ED18 reads the SCE zone grid (E8F46+2,
//       W*H words) and decrements every word; any result of 0 or 1 (raw 1 or 2) sets it to 1
//       (ASM 85089-85105).
//     sub_540CB: gather roster records with [56h] != 0 from CC990 into dword_111F04 (max
//       0x65); return when empty. (sub_4EA22 puts BLACKBOX records with word_FDAF6[owner]==1
//       into CC990 as [0]=FFh, [56h]=FFh, [0Eh]=1.)
//     sub_103A8 (weather tick off) -> sub_54111 (load CHARACTE icons) ->
//       repeat { sub_54C2C -> sub_551E5 } -> sub_5447C
//     sub_54C2C: list screen over the dimmed battle screen: FRAME.SYB #0 panel at (0x7B,4) +
//       5x4 cells (sub_5466B/sub_544B2) + info panel (sub_54743: portrait, name, class,
//       hp/mp, stats, mount row, ELD). The selected cell cycles [56h] FF->FE->FD every
//       30 ticks (sub_102FF) to blink three icon frames. Enter or a second click on the same
//       cell selects (1); Esc or right click exits (0); a unit with [5Ch]==0 and map kind > 4
//       is refused with 0x7809E. Arrow buttons (0x11E,0x18/0x4D) scroll five cells, the auto
//       button (0x14..0x5C,0xA8..0xC3) runs sub_54A82, mount buttons (0x72+k*0x28,0xB4)
//       buy/refund with unk_754C8 costs and print 0x78088 when short.
//     sub_551E5: place or unplace one unit.
//       [56h]==80h (already placed) -> drop it from the army list (11EE4/1137A), reset
//       [56h]=FFh and let the player pick again. Otherwise copy 0x88 bytes and, when
//       [5Ch]!=0 or map kind==4, build the stored record ([77h]) and mount form (sub_17E6D:
//       kind<4 uses byte_754AE[[5Ch]*2], kind>=4 uses the per-class code or
//       byte_754AE[[5Ch]*2+(kind-3)*6]). sub_44D67(1) turns on the zone shading, then
//       sub_4A483 (cell cursor) -> tile free (11DB2) -> zone (13CBD > -1) -> cancel previous
//       placement -> sub_16357 (facing arrows) -> sub_165C2 (footprint); on success it writes
//       [0],[1],[63h] and adds the unit to the army list ([56h]=80h). Repeats until the
//       cursor returns -1 (cancel), then sub_44D67(0). A copy that placed nothing is discarded.
//     After the loop: clear [5Ch] on every roster record and count [56h]==80h; if that count
//       is 0 after sub_103A0, run sub_54A82 (auto placement).
//   sub_54A82: scan cells from index 0 for E8F46==1 (raw 2) with no occupant (CC998) and place
//     the pending units ([56h]>80h, [5Ch]==0) in order. Facing = header byte
//     (dword_11209E+2). No zone or sub_165C2 check.
//
// Deviations:
//   - sub_44C75 (load bar tick), sub_455BD (HUD panel relocate) and sub_49C05 (cursor shape)
//     are skipped: the port uses the SDL hardware cursor, so the software cursor
//     (sub_5D41E/5D570) and the INT 33h AX=7/8 mouse trap are replaced by g2move::confine_box.
//   - Cell background save/restore (sub_545AA/54603) snapshots the 27x27 backbuffer right
//     after the battle screen is drawn and restores it.
//   - sub_48083's internal wait (sub_499EF(5Ah)) is performed by the caller through
//     wait_info_dismiss(0x5A).
#include "combatii.h"
#include "combatii_internal.h"
#include "combatii_move.h"
#include "combatii_deploy_data.h"
#include "combat_class_names.h"
#include "nc2_parser.h"

#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {
int floor_div_(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// byte_754AE (EXE VA 0x754AE, 0x70 bytes): form table at 2-byte stride, followed by the start of
// the unk_754C8 cost table. Out-of-range indices read the following data as the original does.
constexpr uint8_t k754AE[0x70] = {
    0x07,0x00,0x01,0x00,0x02,0x00,0x05,0x00,0x00,0x00,0x31,0x00,0x20,0x00,0x33,0x00,
    0x35,0x00,0x20,0x00,0x00,0x00,0x3A,0x00,0x3C,0x00,0x64,0x00,0x00,0x00,0x96,0x00,
    0x00,0x00,0xF4,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x64,0x00,0x00,0x00,0xF4,0x01,
    0x00,0x00,0x64,0x00,0x00,0x00,0xC8,0x00,0x00,0x00,0xF4,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0xC8,0x00,0x00,0x00,0xF4,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
inline int t754ae(int idx) { return (idx >= 0 && idx < (int)sizeof(k754AE)) ? (int)k754AE[idx] : 0; }
}  // namespace

// ---- sub_4ED18: SCE zone grid ----
// File offsets 0x196/0x198 = W/H, then W*H words from 0x19A. Each word is decremented into
// E8F46+2; any result of 0 or 1 sets dword_11209E = 1.
void CombatII::battle_deploy_zone_parse(const uint8_t* data, size_t size) {
    battle_deploy_enabled_ = false;
    sce_zone_grid_.clear();
    sce_zone_w_ = sce_zone_h_ = 0;
    if (!data || size < 0x19A) return;
    const size_t w = (size_t)(data[0x196] | (data[0x197] << 8));
    const size_t h = (size_t)(data[0x198] | (data[0x199] << 8));
    const size_t n = w * h;
    if (n == 0 || 0x19A + n * 2 > size) return;
    sce_zone_w_ = (uint16_t)w;
    sce_zone_h_ = (uint16_t)h;
    sce_zone_grid_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t raw = (uint16_t)(data[0x19A + i * 2] | (data[0x19B + i * 2] << 8));
        const uint16_t v = (uint16_t)(raw - 1);                  // dec si
        sce_zone_grid_[i] = (int16_t)v;
        if (v == 0 || v == 1) battle_deploy_enabled_ = true;     // jz / cmp si,1
    }
    if (battle_deploy_enabled_)
        fprintf(stderr, "COMBAT: deploy zone layer present (%zux%zu) - sub_55518 deployment step active\n", w, h);
}

// ---- Shared placement tail (sub_551E5 loc_5543C-, sub_54A82 loc_54B04-) ----
static void deploy_finish_unit(CombatUnitPlacement& nu, int x, int y, int facing, uint32_t seq) {
    nu.x = x; nu.y = y;                                          // [0]/[1]
    nu.facing = (int8_t)(facing & 0xFF);                         // [63h]
    nu.ai_home_x = 0xFF;                                         // [6Fh]=FFh
    nu.ai_target_unit = -1;                                      // [73h]=0
    nu.ai_watch_sel = 0xFF;                                      // [71h]=FFh
    nu.battle_gauge = 0;                                         // [56h]=0
    nu.anim_frame = 0;                                           // [64h]=0
    nu.active = true;
    nu.deploy_roster = true;
    nu.battle_list_membership_modeled = true;                    // sub_1134F(CC968[army-1])
    nu.occupancy_hp_known = nu.hp > 0;
    nu.list_seq = seq;
    nu.sprite_id = (uint8_t)(nu.sprite_type % 222);
    nu.team = (nu.army == 0) ? 0 : 1;
}

// Roster copy (0x88 memcpy) and mount form (sub_551E5 loc_5523F-loc_5536B).
void CombatII::battle_deploy_prepare(CombatUnitPlacement& nu, const CombatUnitPlacement& edi) {
    const int map_kind = (int)sce_map_kind_;
    nu = edi;
    nu.stored_77.reset();
    nu.deploy_roster = true;
    nu.active = false;
    const bool formed = (edi.code_b != 0) || map_kind == 4;
    if (!formed) return;                                         // jumps to loc_5536B
    auto stored = std::make_shared<CombatUnitPlacement>(nu);     // ecx = copy of esi
    stored->code_b = 0;                                          // [ecx+5Ch]=0
    stored->list_seq = 0;                                        // [ecx+7Bh]=0
    stored->active = false;
    nu.stored_77 = stored;                                       // [esi+77h]=ecx
    int form;
    if (map_kind < 4) {
        form = t754ae((int)edi.code_b * 2);                      // byte_754AE[[edi+5Ch]*2]
    } else if (nu.code_b == 0) {                                 // loc_552AA: per-class code
        const int cl = nu.combat_class;
        if (cl == 0 || cl == 1) form = 0x29;
        else if (cl == 0x0F || cl == 0x10 || cl == 0x13 || cl == 0x14) form = 0x2B;
        else if (cl == 0x17 || cl == 0x18) form = 0x2D;
        else if (cl == 0x1B || cl == 0x1C || cl == 0x1D || cl == 0x1E || cl == 0x1F || cl == 0x23 || cl == 0x25) form = 0x2F;
        else form = 0x27;
    } else {                                                     // loc_55329: byte_754AE[[5Ch]*2 + (kind-3)*6]
        form = t754ae((int)edi.code_b * 2 + (map_kind - 3) * 6);
    }
    battle_form_17e6d(nu, form);                               // sub_17E6D
    nu.hp = nu.max_hp; nu.hp_deficit = nu.max_hp_deficit;        // [48h]=[4Ch]
}

// ---- sub_13CBD: E8F46 zone value (-128 outside the map) ----
int CombatII::battle_deploy_zone_at(int x, int y) const {
    const int w = (int)map_w_, h = (int)map_h_;
    if (x < 0 || y < 0 || x >= w || y >= h) return -128;
    if (sce_zone_grid_.size() != (size_t)w * (size_t)h) return -128;
    return sce_zone_grid_[(size_t)y * (size_t)w + (size_t)x];
}

// ---- sub_4A483 (ecx = 1): deployment cell cursor ----
//   Draws a 24x16 cell marker (sub_49E4E dl=5 / sub_49EEE) over the battle screen
//   (sub_454B3). The marker is redrawn only on events (cycling the border colour
//   9->10->11 on each draw); it is on only while the hovered cell (clamped to
//   2..W-3 / 2..H-3) is inside the zone (sub_13CBD >= 0) and free or self
//   (sub_4A321 turns it off, sub_49E4E turns it on). Edge scrolling (sub_4A32C) and the
//   arrow keys (sub_4A3FF) go through handle_scroll_input. Left press returns the raw
//   (unsnapped) cell under the click and true; Esc or right press returns false (original -1).
bool CombatII::battle_deploy_pick_tile(int unit_index, int& rx, int& ry) {
    const int w = (int)map_w_, h = (int)map_h_;
    auto tile_x = [&](int mx) { return cam_tile_x_ + floor_div_(mx - cam_fine_x_ - TILE_ANCHOR_X, TOWN_TILE_W); };   // sub_109A3
    auto tile_y = [&](int my) { return cam_tile_y_ + floor_div_(my - cam_fine_y_ - TILE_ANCHOR_Y, TOWN_TILE_H); };   // sub_109CE
    auto valid = [&](int x, int y) {
        if (battle_deploy_zone_at(x, y) < 0) return false;                  // sub_13CBD ; test ax,ax ; jl
        const int o = battle_unit_index_at_footprint(x, y);                 // sub_11DB2
        return o < 0 || o == unit_index;                                    // free or self
    };
    int last_mx = input_.get_mouse_x(), last_my = input_.get_mouse_y();
    int cx = tile_x(last_mx), cy = tile_y(last_my);                         // start cell = under the mouse (not clamped)
    bool marker_on = true;                                                  // sub_49E4E(unit, 5)
    bool dirty = true;
    for (;;) {
        if (dirty) {                                                        // loc_4A4F3: marker + present
            render_battle_frame();                                          // sub_454B3
            if (marker_on) battle_draw_tile_marker(cx, cy);                 // sub_49EEE (24x16)
            video_.present();
            dump_frame_if_requested();
            dirty = false;
        }
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return false; }
        const MouseState& m = input_.get_mouse();
        if (input_.is_key_pressed(SDLK_ESCAPE) || m.right_clicked) return false;   // loc_4A7FF -> -1
        if (m.left_clicked) {                                               // event 2: loc_4A702
            rx = tile_x(m.x);
            ry = tile_y(m.y);
            return true;
        }
        const bool moved = (m.x != last_mx || m.y != last_my);
        last_mx = m.x; last_my = m.y;
        // sub_45577: compose (sub_454B3) + cursor + present; the deploy screen adds the cell
        // marker.
        const bool scrolled = handle_scroll_input(timer_now_us(), [&] {   // sub_4A32C / sub_4A3FF
            render_battle_frame();
            if (marker_on) battle_draw_tile_marker(cx, cy);
            video_.present();
        });
        if (moved || scrolled) {
            const uint8_t* ks = SDL_GetKeyboardState(nullptr);
            const bool key_dir = ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_DOWN] ||
                                 ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_S];
            int nx = tile_x(m.x), ny = tile_y(m.y);
            if (nx < 2) nx = 2; else if (nx > w - 3) nx = w - 3;            // cmp si,2 / cmp ax,[BA844]-3
            if (ny < 2) ny = 2; else if (ny > h - 3) ny = h - 3;
            cx = nx; cy = ny;
            // Move event (1/6): sub_4A321 clears the marker and sub_49E4E re-enables it only on a
            // valid cell. Arrow-key scrolling (sub_4A3FF) only recomputes the cell and keeps the
            // marker state.
            if (moved || !key_dir) marker_on = valid(cx, cy);
            dirty = true;
        } else {
            SDL_Delay(1);
        }
    }
}

// ---- sub_16357: facing select ----
//   Keeps the cell marker (sized by the facing-specific D66A8 footprint) and draws the
//   MOVE.CUR arrow (facing*2 + blink) at the mouse, confined to the (X-10..X+34, Y-10..Y+26)
//   box. sub_1033B(1Eh) toggles the blink. Left press = facing (0 south, 1 west, 2 north,
//   3 east); right press or right-drag = -1. The keyboard is not read.
int CombatII::battle_deploy_pick_facing(int unit_index, int tx, int ty) {
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    load_move_cursor();
    const int X = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;   // sub_10971
    const int Y = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;   // sub_10988
    const g2move::Box box = g2move::confine_box(X, Y);
    video_.cursor_show(false);                                              // the facing arrow (sub_5D3C5) replaces the cursor

    int blink = 0, facing = 0;
    bool recompute = true;                                                  // var_28 = 1
    int last_cx = -100000, last_cy = -100000;
    int result = -1;
    game_timer_set(0x1E);                                                   // sub_1033B(1Eh)

    auto face_of = [&](int cmx, int cmy) {
        return g2move::facing_from_pointer(cmx, cmy, X, Y, (int)sce_map_kind_, (int)u.combat_class, battle_move_edd76_) & 3;
    };
    auto draw = [&](int cmx, int cmy) {
        render_battle_frame();                                              // marker uses the facing footprint size (sub_49E4E(unit, facing))
        int fw = 1, fh = 1;
        battle_d66a8_footprint(u.sprite_type, facing, &fw, &fh);
        battle_draw_tile_marker(tx, ty, std::max(1, fw) * TOWN_TILE_W, std::max(1, fh) * TOWN_TILE_H);
        uint8_t* bb = video_.get_backbuffer();
        const size_t fi = (size_t)(facing * 2 + (blink & 1));
        if (move_cursor_.size() == 8 && !move_cursor_[fi].rle.empty()) {
            blit_sprite_rle(cmx - 8, cmy - 8, move_cursor_[fi].rle.data());   // sub_5D3C5: 24x24, hotspot (8,8)
        } else if (bb) {
            // Fallback arrow when MOVE.CUR is missing (same triangle as render_move_overlay)
            const uint8_t col = (blink & 1) ? 0x0F : 0x0C;
            static const int dxs[4] = {0, -1, 0, 1}, dys[4] = {1, 0, -1, 0};
            const int dx = dxs[facing], dy = dys[facing];
            const int pxs = (dx == 0) ? 1 : 0, pys = (dy == 0) ? 1 : 0;
            for (int t = 0; t <= 6; ++t)
                for (int b = -(6 - t); b <= (6 - t); ++b) {
                    const int fx = cmx + dx * (t - 3) + pxs * b;
                    const int fy = cmy + dy * (t - 3) + pys * b;
                    if (fx >= 0 && fx < SCREEN_W && fy >= 0 && fy < SCREEN_H) bb[fy * SCREEN_W + fx] = col;
                }
        }
        video_.present();
        dump_frame_if_requested();
    };

    for (;;) {
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; break; }
        const MouseState& m = input_.get_mouse();
        const int cmx = std::min(std::max(m.x, box.x0), box.x1);
        const int cmy = std::min(std::max(m.y, box.y0), box.y1);
        const bool moved = (cmx != last_cx || cmy != last_cy);
        if (m.right_clicked || (m.right_button && moved && last_cx != -100000)) { result = -1; break; }   // event 3 / 7
        if (m.left_clicked) {                                               // event 2: facing as of the last move event
            facing = face_of(cmx, cmy);
            result = facing;
            break;
        }
        if (game_timer_poll()) { blink ^= 1; recompute = true; }            // sub_10364: toggle blink and inject a fake event 1
        if (moved) recompute = true;                                        // event 1/6
        last_cx = cmx; last_cy = cmy;
        if (recompute) {
            facing = face_of(cmx, cmy);
            draw(cmx, cmy);
            recompute = false;
        } else {
            SDL_Delay(1);
        }
    }
    video_.cursor_show(true);                                               // sub_49C05(1)
    cursor_apply(true);
    return result;
}

// ---- sub_551E5 ----
//   Returns true when at least one unit was (re)placed.
bool CombatII::battle_deploy_551e5(size_t ri, std::vector<int>& slot) {
    if (ri >= battle_roster_cc990_.size()) return false;
    if (slot.size() < battle_roster_cc990_.size()) slot.resize(battle_roster_cc990_.size(), -1);
    CombatUnitPlacement& edi = battle_roster_cc990_[ri];
    const int w = (int)map_w_, h = (int)map_h_;
    int cur = slot[ri];                                          // map copy for this roster entry (units_ index)
    bool placed = false;                                         // var_4

    battle_deploy_active_ = true;                                // sub_44D67(1): zone shading (word_FA8BC)
    if (edi.battle_gauge == 0x80) {                              // already placed -> drop from the army list
        if (cur >= 0 && (size_t)cur < units_.size()) units_[(size_t)cur].active = false;   // 11EE4 + 1137A
        edi.battle_gauge = 0xFF;                                 // mov byte [edi+56h],0FFh
        if (battle_trace_) fprintf(stderr, "COMBAT: deploy unplace id=%d\n", edi.id);
    } else {
        CombatUnitPlacement nu;
        battle_deploy_prepare(nu, edi);
        if (cur >= 0 && (size_t)cur < units_.size()) units_[(size_t)cur] = nu;
        else { units_.push_back(nu); cur = (int)units_.size() - 1; slot[ri] = cur; }
    }
    if (cur < 0 || (size_t)cur >= units_.size()) { battle_deploy_active_ = false; return false; }

    while (!should_exit_) {
        // ---- sub_4A483: cell pick; Esc or right click = -1 ----
        int tx = 0, ty = 0;
        if (!battle_deploy_pick_tile(cur, tx, ty)) break;        // cmp eax,-1 ; jz loc_554CE

        // sub_11DB2: the tile must be free (the self copy is inactive, so it does not count)
        if (battle_unit_index_at_footprint(tx, ty) >= 0) continue;
        if (battle_deploy_zone_at(tx, ty) <= -1) continue;       // sub_13CBD ; cmp -1 ; jle

        if (placed) {                                            // cancel the previous placement
            units_[(size_t)cur].active = false;                  // 11EE4 + 1137A
            edi.battle_gauge = 0xFF;
            placed = false;
        }
        if (sce_map_kind_ == 5) battle_move_edd76_ = (int)sce_deploy_facing_;   // sub_158AC(dword_11209E>>16)

        // ---- sub_16357: facing ----
        const int facing = battle_deploy_pick_facing(cur, tx, ty);
        if (should_exit_) break;
        if (facing < 0) continue;

        // ---- sub_165C2: footprint ----
        CombatUnitPlacement& u = units_[(size_t)cur];
        {
            g2move::PathEnv env;
            env.width = w; env.height = h;
            env.e8 = sce_zone_grid_.data(); env.e8_size = sce_zone_grid_.size();
            env.occupant_raw = [this](int x, int y) { return battle_unit_index_at(x, y); };
            int fw = 1, fh = 1;
            battle_d66a8_footprint(u.sprite_type, facing, &fw, &fh);
            if (!g2move::footprint_ok(env, cur, tx, ty, fw, fh)) continue;
        }

        deploy_finish_unit(u, tx, ty, facing, battle_next_list_seq());
        edi.battle_gauge = 0x80;                                 // mov byte [edi+56h],80h
        placed = true;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: deploy place id=%d at (%d,%d) facing=%d form=%d\n", u.id, tx, ty, facing, (int)u.code_b);
    }
    battle_deploy_active_ = false;                               // loc_554CE: sub_44D67(0)
    if (!placed && edi.battle_gauge != 0x80) {
        // Nothing placed -> discard the copy (sub_119B6 / sub_1134A)
        units_[(size_t)cur].active = false;
    }
    return placed;
}

// ---- sub_54A82: auto placement ----
void CombatII::battle_deploy_54a82(const std::vector<size_t>& party, std::vector<int>& slot) {
    if (slot.size() < battle_roster_cc990_.size()) slot.resize(battle_roster_cc990_.size(), -1);
    const size_t n = party.size();
    auto next = [&](size_t from) -> size_t {
        for (size_t i = from; i < n; ++i) {
            const auto& r = battle_roster_cc990_[party[i]];
            if (r.battle_gauge > 0x80 && r.code_b == 0) return i;        // cmp [56h],80h ; jbe / cmp [5Ch],0
        }
        return n;
    };
    size_t cur = next(0);
    if (cur == n) return;
    size_t done = 0;                                                     // edi: count of [56h] <= 80h
    for (size_t i = 0; i < n; ++i)
        if (battle_roster_cc990_[party[i]].battle_gauge <= 0x80) ++done;
    const int w = (int)map_w_, h = (int)map_h_;
    if (w <= 0 || h <= 0 || sce_zone_grid_.size() != (size_t)w * (size_t)h) return;
    for (int idx = 0; idx < w * h; ++idx) {
        if (sce_zone_grid_[(size_t)idx] != 1) continue;                  // cmp E8F46+2[idx*2],1
        const int x = idx % w, y = idx / w;
        if (battle_unit_index_at_footprint(x, y) >= 0) continue;         // cmp dword_CC998[idx*4],0
        const size_t ri = party[cur];
        CombatUnitPlacement& edi = battle_roster_cc990_[ri];
        CombatUnitPlacement nu = edi;                                    // sub_112FD + memcpy 0x88
        nu.stored_77.reset();
        edi.battle_gauge = 0x80;
        deploy_finish_unit(nu, x, y, (int)sce_deploy_facing_, battle_next_list_seq());
        int ci = slot[ri];
        if (ci >= 0 && (size_t)ci < units_.size()) units_[(size_t)ci] = nu;
        else { units_.push_back(nu); slot[ri] = (int)units_.size() - 1; }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: deploy auto id=%d at (%d,%d) facing=%d\n", nu.id, x, y, (int)sce_deploy_facing_);
        cur = next(cur + 1);
        if (cur == n) break;
        ++done;
        if (done == n) break;
    }
}

// ---- sub_54C2C: deployment list screen ----
//   Returns 1 when a unit was selected (the caller runs sub_551E5(scroll+sel)), 0 to exit.
int CombatII::battle_deploy_list(const std::vector<size_t>& party, std::vector<int>& slot, int& scroll, int& sel) {
    if (!hud_frames_tried_) load_hud_frames();
    const int count = (int)party.size();                              // dword_1120A2+2
    const int map_kind = (int)sce_map_kind_;                          // byte_BA84E
    auto rec = [&](int i) -> CombatUnitPlacement* {
        return (i >= 0 && i < count) ? &battle_roster_cc990_[party[(size_t)i]] : nullptr;
    };

    // ---- FRAME.SYB helpers (sub_5F3FD: blit with no offset) ----
    auto fill_rect = [&](int x, int y, int w, int h, uint8_t c) {      // sub_5EDDE mode 0 (width = x2-x1)
        uint8_t* bb = video_.get_backbuffer();
        if (!bb) return;
        for (int yy = std::max(0, y); yy < std::min(SCREEN_H, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(SCREEN_W, x + w); ++xx)
                bb[(size_t)yy * SCREEN_W + (size_t)xx] = c;
    };
    auto fs = [&](int idx) -> const SpriteEntry* { return hud_frames_loaded_ ? hud_frames_.get(idx) : nullptr; };
    auto fs_draw = [&](int idx, int x, int y) {
        const SpriteEntry* s = fs(idx);
        if (s && s->pixel_data) { blit_sprite_rle(x, y, s->pixel_data); return; }
        // Fallback panels when FRAME.SYB is missing
        switch (idx) {
            case 0: fill_rect(0x7B, 4, 0xA8, 0x7C, 0x08); fill_rect(0x7D, 6, 0xA4, 0x78, 0x00); break;
            case 3: fill_rect(0x14, 0x8C, 0x128, 0x1A, 0x00); break;
            case 4: fill_rect(0x14, 0xA8, 0x128, 0x1A, 0x00); break;
            case 5: fill_rect(0xDC, 0xAB, 0x60, 0x16, 0x00); break;
            default: break;
        }
    };
    // sub_5F517: repaint all opaque pixels in one colour (same RLE stream as blit_sprite_rle)
    auto silhouette = [&](int x, int y, const uint8_t* rle, uint8_t color) {
        uint8_t* bb = video_.get_backbuffer();
        if (!bb || !rle) return;
        const uint16_t chunks = (uint16_t)(rle[0] | (rle[1] << 8));
        if (chunks == 0 || chunks > 800) return;
        const uint8_t* p = rle + 2;
        int rel = 0;
        for (uint16_t i = 0; i < chunks; ++i) {
            const int skip = p[0] | (p[1] << 8);
            const int total = p[2] * 4 + p[3];
            p += 4;
            rel += skip;
            for (int b = 0; b < total; ++b) {
                const int py = y + (rel + b) / SCREEN_W, px = x + (rel + b) % SCREEN_W;
                if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) bb[(size_t)py * SCREEN_W + (size_t)px] = color;
            }
            p += total;
            rel += total;
        }
    };
    auto present = [&]() {                                            // sub_5D41E + sub_5E9E8
        video_.present();
        dump_frame_if_requested();
    };

    // ---- sub_544B2: one cell centred at (x,y) ----
    auto draw_cell = [&](int x, int y, const CombatUnitPlacement& r) {
        fill_rect(x - 0xB, y - 0xB, 0x17, 0x17, 0);
        const int fi = (r.battle_gauge == 0x80) ? 3 : (0xFF - (int)r.battle_gauge);
        const NC2Frame* fr = nullptr;
        if (fi == 3) fr = nc2_frame_for(r.sprite_type, 1, 0);          // record 11 = facing 1, pose 0 ("placed" icon)
        else if (fi >= 0 && fi <= 2) fr = nc2_frame_for(r.sprite_type, 0, 2 + fi);   // records 2,3,4
        if (fr) blit_nc2_frame(x + fr->x_off, y + fr->y_off, *fr);
        if (r.battle_gauge == 0x80) render_dialog_dimming(x - 0xB, y - 0xB, 0x17, 0x17);   // again with mode 1
    };

    // ---- sub_545AA / sub_54603: save and restore the selected cell background (27x27) ----
    std::vector<uint8_t> snap(27 * 27, 0);
    int snap_x = 0, snap_y = 0;
    auto snap_save = [&](int s) {
        snap_x = (s % 5) * 0x1C + 0x8B;
        snap_y = (s / 5) * 0x1B + 0x15;
        const uint8_t* bb = video_.get_backbuffer();
        if (!bb) return;
        for (int r = 0; r < 27; ++r)
            for (int c = 0; c < 27; ++c) {
                const int px = snap_x + c, py = snap_y + r;
                snap[(size_t)r * 27 + (size_t)c] = (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                                                       ? bb[(size_t)py * SCREEN_W + (size_t)px] : 0;
            }
    };
    auto snap_restore_and_draw = [&](int s, const CombatUnitPlacement& r) {
        uint8_t* bb = video_.get_backbuffer();
        const int x0 = (s % 5) * 0x1C + 0x8B, y0 = (s / 5) * 0x1B + 0x15;
        if (bb && x0 == snap_x && y0 == snap_y) {
            for (int rr = 0; rr < 27; ++rr)
                for (int c = 0; c < 27; ++c) {
                    const int px = x0 + c, py = y0 + rr;
                    if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                        bb[(size_t)py * SCREEN_W + (size_t)px] = snap[(size_t)rr * 27 + (size_t)c];
                }
        }
        draw_cell(x0 + 0xD, y0 + 0xD, r);
    };

    // ---- sub_5466B: panel + 5x4 cells ----
    auto draw_panel = [&]() {
        fs_draw(0, 0x7B, 4);
        int x = 0x98, y = 0x22;
        for (int i = scroll; i < count; ++i) {
            draw_cell(x, y, *rec(i));
            x += 0x1C;
            if (x > 0x108) {
                y += 0x1B;
                x = 0x98;
                if (y > 0x73) break;
            }
        }
    };

    // ---- sub_546D3: cell hit test (raw coordinates) ----
    auto hit_cell = [&](int x, int y) -> int {
        if (x < 0x8D || x > 0x119 || y < 0x17 || y > 0x83) return -1;
        const int idx = (x - 0x8D) / 0x1C + ((y - 0x17) / 0x1B) * 5;
        if (scroll + idx >= count) return -1;
        return idx;
    };

    int mount_flag[3] = {0, 0, 0};                                    // word_112098[3]
    // ---- sub_54743: info panel ----
    auto draw_info = [&](const CombatUnitPlacement& r) {
        char b[64];
        show_portrait(r.face, 0x48, 0x88);                            // sub_48132(0x48,0x88,[+10h],0)
        fs_draw(3, 0x14, 0x8C);
        draw_text_wrapped(0x14, 0x92, r.name);                        // sub_5E048(0x14,0x92,[+2])
        draw_small_text(0x6C, 0x93, combat_class_name(r.combat_class), 0x10, 1);
        snprintf(b, sizeof(b), "hp %u", (unsigned)(int)(int16_t)r.hp);                // sub_13853 -> cwde -> %lu
        draw_small_text(0xD0, 0x93, b, 0x28, 1);
        snprintf(b, sizeof(b), "mp %d", (int)(int16_t)r.combat_mp_50);                // dword[+4Eh] sar 10h
        draw_small_text(0x102, 0x93, b, 0x28, 1);
        snprintf(b, sizeof(b), "str %d", (int)r.combat_str_40);
        draw_small_text(0x6C, 0x9C, b, 8, 1);
        snprintf(b, sizeof(b), "int %d", (int)r.combat_mag41);
        draw_small_text(0x9E, 0x9C, b, 8, 1);
        snprintf(b, sizeof(b), "dex %d", (int)(uint8_t)(r.combat_value_42 & 0xFF));
        draw_small_text(0xD0, 0x9C, b, 8, 1);
        snprintf(b, sizeof(b), "cha %d", (int)r.combat_value_43);
        draw_small_text(0x102, 0x9C, b, 8, 1);
        fs_draw(4, 0x14, 0xA8);

        const int var8 = (map_kind < 4) ? 0 : map_kind - 3;
        const int ride = deploydata::kClassRideFlag[std::min<int>(r.combat_class, 164)];
        if (r.battle_gauge != 0x80 && ((ride == 1 && map_kind <= 1) || map_kind >= 4)) {
            for (int k = 0; k < 3; ++k) {
                const int bx = 0x72 + k * 0x28;
                const int cl = r.combat_class;
                if (k == 2 && (map_kind == 0 || map_kind == 1) && !(cl >= 1 && cl <= 5) && cl != 9) mount_flag[k] = 0;
                else mount_flag[k] = 1;
                const int idx = deploydata::kMountIcon[var8 * 3 + k];
                if (idx != -1 && mount_flag[k] == 1) {
                    const SpriteEntry* s = fs(idx + 6);
                    if (s && s->pixel_data) {
                        silhouette(bx + s->x_offset + 1, s->y_offset + 0xB5, s->pixel_data, 0x0A);
                        silhouette(bx + s->x_offset, s->y_offset + 0xB4, s->pixel_data, 0x0F);
                    }
                }
            }
        }
        if (r.code_b != 0) {                                          // selected mount (always drawn, even when placed)
            const int k = (int)r.code_b - 1;
            const int idx = (var8 * 3 + k >= 0 && var8 * 3 + k < 12) ? deploydata::kMountIcon[var8 * 3 + k] : -1;
            const SpriteEntry* s = fs(idx + 6);
            if (idx >= 0 && s && s->pixel_data) blit_sprite_rle(0x72 + k * 0x28 + s->x_offset, s->y_offset + 0xB4, s->pixel_data);
        }
        fs_draw(5, 0xDC, 0xAB);
        char mb[32];
        snprintf(mb, sizeof(mb), "%-6d", (int)battle_stash_money_[std::min<size_t>(fdf85_word_, 99)]);
        draw_small_text(0x104, 0xB3, mb, 1, 1);
    };

    auto redraw_all = [&]() {
        battle_dim_row0_ = true;                                      // sub_44D6E(1)
        render_battle_frame();                                        // sub_454B3
        snap_save(sel);                                               // sub_545AA(sel)
        draw_panel();                                                 // sub_5466B(scroll)
        if (const CombatUnitPlacement* r = rec(scroll + sel)) draw_info(*r);   // sub_54743
        battle_dim_row0_ = false;                                     // sub_44D6E(0)
    };
    // Arrow press feedback: sub_1033B(20h) + pressed frame + present + sub_499E4
    auto arrow_feedback = [&](int entry, int x, int y) {
        game_timer_set(0x20);
        fs_draw(entry, x, y);
        present();
        wait_game_tick();
    };
    auto page_up = [&]() { if (scroll > 0) { scroll -= 5; if (sel < 15) sel += 5; } };
    auto page_down = [&]() { if (count - scroll > 0x14) { scroll += 5; if (sel > 5) sel -= 5; } };
    auto message = [&](const std::string& s) {                        // sub_48083 (includes the internal wait)
        battle_text_band(s, false);
        wait_info_dismiss(0x5A);
    };

    // ---- events (sub_5D22E / getch_) ----
    int last_mx = input_.get_mouse_x(), last_my = input_.get_mouse_y();
    auto poll = [&](int& type, int& ex, int& ey) -> bool {
        const MouseState& m = input_.get_mouse();
        type = 0; ex = m.x; ey = m.y;
        if (input_.any_getch_key_pressed()) {
            if (input_.is_key_pressed(SDLK_RETURN) || input_.is_key_pressed(SDLK_KP_ENTER)) type = 0x0D;
            else if (input_.is_key_pressed(SDLK_UP)) type = 0x4800;
            else if (input_.is_key_pressed(SDLK_DOWN)) type = 0x5000;
            else if (input_.is_key_pressed(SDLK_ESCAPE)) type = 0x1B;
            else type = 0x7FFF;                                       // any other key: si=0, ignored
        } else if (m.right_clicked) type = 3;
        else if (m.left_clicked) type = 2;
        else if (m.x != last_mx || m.y != last_my) type = 1;
        last_mx = m.x; last_my = m.y;
        return type != 0;
    };

    bool redraw = true;                                               // si
    for (;;) {
        if (redraw) redraw_all();
        present();
        int type = 0, ex = 0, ey = 0;
        for (;;) {
            input_.update();
            if (input_.quit_requested()) { should_exit_ = true; return 0; }
            const bool have = poll(type, ex, ey);
            if (half_tick_poll()) {                                   // sub_102FF: selected cell icon blink
                if (CombatUnitPlacement* r = rec(scroll + sel)) {
                    if (r->battle_gauge != 0x80) {
                        const int t = 0x100 - (int)r->battle_gauge;
                        r->battle_gauge = (uint8_t)(0xFF - (t % 3));  // FF->FE->FD->FF
                        snap_restore_and_draw(sel, *r);               // sub_54603
                        present();
                    }
                }
            }
            if (have) break;
            SDL_Delay(1);
        }

        redraw = true;                                                // mov esi,1
        if (type == 0x0D) {                                           // Enter (loc_550DE)
            const CombatUnitPlacement* r = rec(scroll + sel);
            if (r && r->code_b == 0 && map_kind > 4) message(deploydata::kStrNotMounted);   // "not mounted" message (0x7809E)
            else return 1;
        } else if (type == 0x4800) {                                  // up
            arrow_feedback(1, 0x11E, 0x18);
            page_up();
        } else if (type == 0x5000) {                                  // down
            arrow_feedback(2, 0x11E, 0x4D);
            page_down();
        } else if (type == 2) {                                       // left press (loc_54DAE)
            const int idx = hit_cell(ex, ey);
            if (idx != -1) {
                if (idx != sel) {
                    if (CombatUnitPlacement* prev = rec(scroll + sel))
                        if (prev->battle_gauge != 0x80) prev->battle_gauge = 0xFF;   // stop the blink
                    sel = idx;
                } else {
                    const CombatUnitPlacement* r = rec(scroll + idx);
                    if (r && (r->code_b != 0 || map_kind <= 4)) return 1;
                    // [5Ch]==0 and map kind > 4: no selection, just redraw (loc_551C7)
                }
            } else {
                game_timer_set(0x20);
                if (ex >= 0x11E && ex <= 0x130 && ey >= 0x18 && ey < 0x4A) {
                    arrow_feedback(1, 0x11E, 0x18);
                    page_up();
                } else if (ex >= 0x11E && ex <= 0x130 && ey >= 0x4D && ey < 0x7F) {
                    arrow_feedback(2, 0x11E, 0x4D);
                    page_down();
                } else if (ex >= 0x14 && ex <= 0x5C && ey >= 0xA8 && ey <= 0xC3) {
                    battle_deploy_54a82(party, slot);                 // auto-place button
                } else if (ey >= 0xA8 && ey <= 0xC0 && (map_kind <= 1 || map_kind >= 4)) {
                    CombatUnitPlacement* r = rec(scroll + sel);
                    const bool ridable = r && (deploydata::kClassRideFlag[std::min<int>(r->combat_class, 164)] != 0 || map_kind >= 4);
                    const int k = (ex - 0x66) / 0x28;                 // idiv: truncates toward zero
                    if (ridable && k >= 0 && k < 3 && mount_flag[k] == 1 && r->battle_gauge != 0x80) {
                        uint32_t& stash = battle_stash_money_[std::min<size_t>(fdf85_word_, 99)];
                        const int row = (map_kind >= 4) ? (map_kind - 4) : 0;
                        const uint32_t* cost = &deploydata::kMountCost[std::min(row, 3) * 3];
                        int money = (int)stash;
                        const int cur = r->code_b;
                        if (cur != 0) money += (int)cost[cur - 1];    // refund the current mount
                        if (cur == k + 1) {                           // pressing the same mount again clears it
                            r->code_b = 0;
                            stash = (uint32_t)money;
                        } else if (money >= (int)cost[k]) {
                            r->code_b = (uint8_t)(k + 1);
                            stash = (uint32_t)(money - (int)cost[k]);
                        } else {                                      // "%3d ELD short" message (0x78088)
                            char num[16];
                            snprintf(num, sizeof(num), "%3d", (int)cost[k] - money);
                            message(std::string(num) + (deploydata::kStrEldShortFmt + 3));
                        }
                    }
                }
            }
        } else {
            redraw = false;                                           // move (1), other keys, Esc, right press: si=0
        }
        if (type == 0x1B || type == 3) return 0;                      // loc_551C7: 1Bh / 3 -> exit
    }
}

// ---- sub_55518 ----
int CombatII::battle_deploy_55518() {
    if (!battle_deploy_enabled_) return 0;                               // cmp word [11209E],0 ; jz
    if (map_w_ == 0 || sce_zone_grid_.size() != (size_t)map_w_ * (size_t)map_h_) {
        fprintf(stderr, "COMBAT: deploy zone %ux%u != map %zux%zu - deployment step skipped\n",
                (unsigned)sce_zone_w_, (unsigned)sce_zone_h_, (size_t)map_w_, (size_t)map_h_);
        return 0;
    }
    // sub_540CB
    std::vector<size_t> party;
    for (size_t i = 0; i < battle_roster_cc990_.size() && party.size() < 0x65; ++i)
        if (battle_roster_cc990_[i].battle_gauge != 0) party.push_back(i);
    if (party.empty()) return 0;                                         // cmp word [1120A2+2],0
    std::vector<int> slot(battle_roster_cc990_.size(), -1);

    weather_tick_enabled_ = false;                                       // sub_103A8
    battle_deploy_active_ = false;                                       // word_FA8BC: off during the list, on only inside sub_551E5
    battle_range_e8_ = sce_zone_grid_;                                   // E8F46 = the zone grid (painted by sub_1547F)
    fprintf(stderr, "COMBAT: deploy phase (sub_55518): party=%zu kind=%d\n", party.size(), (int)sce_map_kind_);

    const bool auto_mode = auto_advance_ || std::getenv("G2_DEPLOY_AUTO") != nullptr;
    if (!auto_mode) {
        int scroll = 0, sel = 0;                                         // var_4 / var_8
        while (!should_exit_) {
            if (battle_deploy_list(party, slot, scroll, sel) == 0) break;     // sub_54C2C
            const int idx = scroll + sel;
            if (idx < 0 || idx >= (int)party.size()) continue;
            battle_deploy_551e5(party[(size_t)idx], slot);               // sub_551E5(scroll+sel)
        }
    }

    // 55518 loc_5557C-: clear [5Ch] on all roster records and count [56h]==80h; when that
    // count is 0 after sub_103A0, run sub_54A82.
    int placed = 0;
    for (size_t ri : party) {
        battle_roster_cc990_[ri].code_b = 0;
        if (battle_roster_cc990_[ri].battle_gauge == 0x80) ++placed;
    }
    weather_tick_enabled_ = true;                                        // sub_103A0
    if (placed == 0) {
        battle_deploy_54a82(party, slot);
        for (size_t ri : party)
            if (battle_roster_cc990_[ri].battle_gauge == 0x80) ++placed;
    }
    battle_deploy_active_ = false;
    battle_range_e8_.clear();
    fprintf(stderr, "COMBAT: deploy phase done: placed=%d/%zu\n", placed, party.size());
    return placed;
}
