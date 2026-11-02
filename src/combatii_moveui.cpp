// Player move command (sub_4D674 loc_4D98E -> sub_16834): pick destination -> validate path
// -> pick facing (sub_16357) -> walk.
//
// Original flow (sub_16834):
//   sub_4A483   destination click loop. Esc or right click = -1 (back to the ring). Ignored
//               when the tile has an occupant other than self, when E8F46 < 0, or when
//               sub_15523 returns 0.
//   sub_16357   facing select loop. The mouse is confined to a box around the destination
//               tile (X-10..X+34, Y-10..Y+26); the facing follows whichever axis dominates
//               around the box centre (X+12, Y+8). Left press confirms, right press or
//               right-drag cancels (-1, back to destination select). The keyboard is not read
//               (sub_5D22E is the mouse event ring buffer), so Esc does nothing here.
//   sub_165C2   destination footprint check (0 returns to destination select).
//   sub_1620A   walk the path, then [+63h] = the chosen facing.
// The math (sub_15523/sub_165C2/facing formula) lives in combatii_move.cpp (g2move).
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {
int floor_div_(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}

// ==== helpers ====

void CombatII::battle_ring_return(int unit_index) {
    battle_attack_cancel();
    // Ring E8 holds the 1CC09 attack range; a leftover 16834 diffusion would be stale, so drop it.
    // (The original also overwrites the shared E8 grid with 1CC09; enter_ring,
    // cancel_to_free and end_turn already clear it.)
    battle_player_diffusion_.clear();
    battle_player_diffusion_valid_ = false;
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    // loc_4DA7B: end the turn when TP < var18 or var24 != 0, otherwise reopen the ring (4D7EA).
    if (battle_tp() < battle_move_var18_) {
        if (battle_trace_) fprintf(stderr, "COMBAT: ring TP=%d <var18=%d, turn ends (4DA7B)\n", battle_tp(), battle_move_var18_);
        battle_player_end_turn(battle_var24_action_end_);
        return;
    }
    if (battle_var24_action_end_) {
        battle_player_end_turn(true);
        return;
    }
    // Re-enter the 4D7EA loop: recentre with 4D582+1124A, then 1CC09 + ring (4D582 uses the
    // post-move destination).
    battle_4d582_center(unit_index);
    battle_player_show_menu(unit_index); // 1CC09 + ring (not the move diffusion)
}

// sub_13909: MOVE.CUR = eight 24x24 RLE frames (4 facings x 2 blink). Falls back to a drawn arrow
// when the file is missing.
void CombatII::load_move_cursor() {
    if (move_cursor_tried_) return;
    move_cursor_tried_ = true;
    move_cursor_.clear();
    std::string path = file_find_case_insensitive(GAME_DATA_DIR, "MOVE.CUR");
    if (path.empty() || !file_exists(path.c_str())) {
        fprintf(stderr, "COMBAT: warning: MOVE.CUR not found - drawing the facing arrow directly\n");
        return;
    }
    SPRParser spr;
    if (!spr.load(path, false) || spr.count() < 8) {
        fprintf(stderr, "COMBAT: warning: MOVE.CUR parse failed (%s) - drawing the facing arrow directly\n", spr.error().c_str());
        return;
    }
    move_cursor_.resize(8);
    for (int i = 0; i < 8; ++i) {
        const SpriteEntry* se = spr.get(i);
        if (!se || !se->pixel_data || se->data_size <= 0) { move_cursor_.clear(); return; }
        TerrainOverlayFrame& fr = move_cursor_[(size_t)i];
        fr.w = se->word0; fr.h = se->word2; fr.x = se->x_offset; fr.y = se->y_offset;
        fr.rle.assign(se->pixel_data, se->pixel_data + se->data_size);
    }
    fprintf(stderr, "COMBAT: MOVE.CUR loaded 8 frames\n");
}

void CombatII::battle_move_cancel() {
    if (battle_move_phase_ == 2) {
        video_.cursor_show(true);   // facing arrow -> default cursor (sub_49C05(1))
        cursor_apply(true);
    }
    battle_move_phase_ = 0;
    battle_face_init_ = false;
}

// ==== entry (sub_4BA7F -2 -> loc_4D98E) ====

void CombatII::battle_move_begin(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    // loc_4D98E entry gate: no move when TP < var_18 (83474-83477; var_18 starts at 0Ah and
    // sub_167C9 updates it).
    if (battle_tp() < battle_move_var18_) {
        if (battle_trace_) fprintf(stderr, "COMBAT: move rejected unit=%d TP=%d <var18=%d\n", units_[(size_t)unit_index].id, battle_tp(), battle_move_var18_);
        return;
    }
    // sub_4D674 loc_4D96F -> loc_4D98E: the 4BA7F -2 (move) path hides the ring via
    // sub_4B9D7 (hide 36->0) before sub_16834 (14032+143F3+44D67(1)) shows the move range.
    // Leaving the ring open would keep it drawn during the walk (battle_move_walk, phase 0),
    // so it is hidden on entry as in the original (no-op when already closed).
    ring_sub_close();
    ring_close_anim(); // equivalent to sub_4B9D7 (sfx 0x1F, 36->0 over 9 frames, FDAEE=0)
    battle_player_begin_overlay(unit_index);          // re-spread the budget from the current TP/var18 (sub_14300)
    battle_move_budget_ = ((battle_tp() - battle_move_var18_) / 10) * 4; // 83478-83489
    battle_move_phase_ = 1;
    if (battle_trace_) fprintf(stderr, "COMBAT: move select unit=%d budget=%d (TP=%d var18=%d)\n", units_[(size_t)unit_index].id, battle_move_budget_, battle_tp(), battle_move_var18_);
}

// ==== per-phase input ====

bool CombatII::battle_move_input(int unit_index) {
    if (battle_move_phase_ == 0 || unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    const MouseState& m = input_.get_mouse();
    const bool esc = input_.is_key_pressed(SDLK_ESCAPE);
    const bool left = input_.was_left_clicked();
    const int w = (int)map_w_, h = (int)map_h_;

    // Screen -> tile (sub_109A3 / sub_109CE)
    const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
    const int tx = cam_tile_x_ + floor_div_(mx - cam_fine_x_ - TILE_ANCHOR_X, TOWN_TILE_W);
    const int ty = cam_tile_y_ + floor_div_(my - cam_fine_y_ - TILE_ANCHOR_Y, TOWN_TILE_H);

    // E8F46 diffusion grid (-128 outside the map, as in sub_13CBD)
    const bool have_grid = battle_player_diffusion_valid_ &&
                           battle_player_diffusion_.size() == (size_t)w * (size_t)h;
    auto e8_at = [&](int x, int y) -> int {
        if (!have_grid || x < 0 || y < 0 || x >= w || y >= h) return -128;
        return battle_player_diffusion_[(size_t)y * (size_t)w + (size_t)x];
    };

    if (battle_move_phase_ == 1) {
        // sub_455BD: the sub_4A483 loop checks HUD panel overlap on every mouse event and
        //   moves the panel to another slot (78827: loop tail loc_4A6E7).
        battle_hud_panel_relocate(mx, my);
        // sub_4A483: Esc or right click -> -1, so sub_16834 returns -1 and control goes back
        // to the ring.
        if (esc || m.right_clicked) {
            if (battle_trace_) fprintf(stderr, "COMBAT: move cancelled unit=%d\n", u.id);
            battle_move_cancel();
            battle_ring_return(unit_index);
            return esc;
        }
        if (!left) return false;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) return false;

        // sub_16834: ignore a tile occupied by another unit.
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& o = units_[i];
            if (o.active && o.hp > 0 && o.x == tx && o.y == ty && (int)i != unit_index) return false;
        }
        // The destination must be a reachable E8F46 tile (`cmp word,0; jl` -> sub_13CBD).
        // Without a diffusion (mover gate unmet) fall back to the old radius approximation.
        const int distance = std::abs(tx - u.x) + std::abs(ty - u.y);
        int e8dest = -1;
        int edd76 = 0;
        if (have_grid) {
            e8dest = e8_at(tx, ty);
            if (e8dest < 0) {
                if (battle_trace_) fprintf(stderr, "COMBAT: move rejected unit=%d target=(%d,%d) E8<0\n", u.id, tx, ty);
                return false;
            }
            // sub_15523: descend the gradient from the destination back to the origin; ignore on 0.
            std::vector<int16_t> grid(battle_player_diffusion_.size());
            for (size_t i = 0; i < grid.size(); ++i) grid[i] = (int16_t)battle_player_diffusion_[i];
            g2move::PathEnv env;
            env.width = w; env.height = h;
            env.e8 = grid.data(); env.e8_size = grid.size();
            env.occupant_raw = [this](int x, int y) { return battle_unit_index_at(x, y); };
            const g2move::PathResult pr = g2move::trace_path(env, u.x, u.y, u.facing & 3, unit_index,
                                                             battle_move_budget_, tx, ty);
            if (pr.ret == 0) {
                if (battle_trace_) fprintf(stderr, "COMBAT: warning: move rejected unit=%d target=(%d,%d) path trace failed\n", u.id, tx, ty);
                return false;
            }
            edd76 = pr.edd76 >= 0 ? pr.edd76 : battle_move_edd76_;
            battle_move_path_ = pr.nodes;
        } else {
            if (distance > battle_player_move_range_) {
                if (battle_trace_) fprintf(stderr, "COMBAT: move rejected unit=%d target=(%d,%d) range=%d (fallback)\n",
                                           u.id, tx, ty, battle_player_move_range_);
                return false;
            }
            edd76 = (std::abs(tx - u.x) > std::abs(ty - u.y)) ? (tx > u.x ? 3 : 1) : (ty > u.y ? 0 : 2);
            // No diffusion means no path queue: join with an x-then-y line like the sub_15523
            // straight tail.
            battle_move_path_.clear();
            if (distance == 0) battle_move_path_.push_back({(uint8_t)u.x, (uint8_t)u.y});
            for (int cx = tx, cy = ty; cx != u.x || cy != u.y;) {
                battle_move_path_.push_back({(uint8_t)cx, (uint8_t)cy});
                if (cx > u.x) --cx; else if (cx < u.x) ++cx; else if (cy > u.y) --cy; else ++cy;
            }
        }
        battle_move_dest_x_ = tx;
        battle_move_dest_y_ = ty;
        battle_move_e8dest_ = e8dest;
        battle_move_edd76_ = edd76;
        battle_move_phase_ = 2;                      // enter sub_16357
        battle_face_init_ = false;
        load_move_cursor();
        video_.cursor_show(false);                   // the facing arrow (sub_5D3C5) replaces the cursor
        return false;
    }

    // ---- phase 2: sub_16357 ----
    const int X = (battle_move_dest_x_ - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;   // sub_10971
    const int Y = (battle_move_dest_y_ - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;   // sub_10988
    const g2move::Box box = g2move::confine_box(X, Y);          // INT 33h AX=7/8
    const int cmx = std::min(std::max(mx, box.x0), box.x1);
    const int cmy = std::min(std::max(my, box.y0), box.y1);
    const bool moved = !battle_face_init_ || cmx != battle_face_mx_ || cmy != battle_face_my_;

    // Right press (3) or right-drag (7) -> -1, so sub_16834 runs sub_13B45 and returns to
    // destination select. The sub_16357 loop reads only sub_5D22E (mouse event ring) and never
    // calls getch_, so Esc does nothing in this phase (it is not a cancel); it is consumed only
    // so it cannot leak into the port's quit handling.
    if (m.right_clicked || (m.right_button && moved && battle_face_init_)) {
        if (battle_trace_) fprintf(stderr, "COMBAT: facing cancelled unit=%d\n", u.id);
        battle_move_cancel();
        battle_move_phase_ = 1;
        return esc;      // Esc is only consumed (no action)
    }

    // Event 1/6 (move or left-drag) and the fake event 1 injected by the timer recompute the
    // facing.
    bool recompute = moved;
    if (!battle_face_init_) {
        battle_face_init_ = true;                    // on start var_28 = 1 (compute once)
        battle_face_choice_ = 0;                     // var_4 = 0
        battle_face_blink_ = 0;                      // var_8 = 0
        game_timer_set(0x1E);                        // sub_1033B(1Eh)
        recompute = true;
    }
    if (game_timer_poll()) {                         // sub_10364: toggle blink and inject event 1
        battle_face_blink_ = (battle_face_blink_ + 1) & 1;
        recompute = true;
    }
    if (recompute) {
        battle_face_mx_ = cmx;
        battle_face_my_ = cmy;
        battle_face_choice_ = g2move::facing_from_pointer(cmx, cmy, X, Y, (int)sce_map_kind_, (int)u.combat_class,
                                                           battle_move_edd76_);
    }
    if (!left) return esc;                           // Esc is only consumed (the original reads no keyboard)

    // Left press (2) returns var_4. sub_165C2: the destination footprint (raw D66A8 w x h) must
    // be free with E8 >= 0. Classes 1-7 are all 1x1; large units are checked with their real rect.
    const int facing = battle_face_choice_ & 3;
    bool fp_ok = true;
    if (have_grid) {
        std::vector<int16_t> grid(battle_player_diffusion_.size());
        for (size_t i = 0; i < grid.size(); ++i) grid[i] = (int16_t)battle_player_diffusion_[i];
        g2move::PathEnv env;
        env.width = w; env.height = h;
        env.e8 = grid.data(); env.e8_size = grid.size();
        env.occupant_raw = [this](int x, int y) { return battle_unit_index_at(x, y); };
        int foot_w = 1, foot_h = 1;
        battle_d66a8_footprint(units_[(size_t)unit_index].sprite_type, facing, &foot_w, &foot_h);
        fp_ok = g2move::footprint_ok(env, unit_index, battle_move_dest_x_, battle_move_dest_y_, foot_w, foot_h);
    }
    if (!fp_ok) {                                    // jz loc_169CD: sub_13B45 then back to destination select
        battle_move_cancel();
        battle_move_phase_ = 1;
        return esc;
    }
    battle_move_cancel();
    battle_move_commit(unit_index, facing);
    return esc;
}

// ==== move commit (second half of sub_16834 + loc_4D98E bookkeeping) ====

void CombatII::battle_move_commit(int unit_index, int facing) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const int tx = battle_move_dest_x_, ty = battle_move_dest_y_;
    const int distance = std::abs(tx - units_[(size_t)unit_index].x) + std::abs(ty - units_[(size_t)unit_index].y);
    // sub_16834 (ASM 9709-9711): `call sub_103A0; xor eax,eax; call sub_44D67` clears word_FA8BC
    // right after the facing is confirmed, before the camera alignment (sub_1124A) and the walk
    // (sub_1620A). Nothing drawn during the walk or afterwards through sub_45559 (tile events,
    // sub_5401B, sub_17310) shows shading until the next 44D67(1) (ring re-entry 1CC09, re-move
    // 14032+143F3). The port decides shading by data presence (render_player_move_range), so it
    // clears both sources:
    //  - battle_range_e8_: the 1CC09 range painted around the origin when the ring opened. In the
    //    phase 0 walk the "ring phase" branch painted it, leaving range shading at the origin.
    //  - battle_player_diffusion_valid_: the move diffusion for phases 1/2; unused after the commit
    //    (the budget and E8 values are already stored).
    battle_range_e8_.clear();
    battle_player_diffusion_valid_ = false;
    {
        CombatUnitPlacement& u = units_[(size_t)unit_index];
        u.combat_acc57 = 0;                          // mov word ptr [esi+57h], 0
        camera_center_on(u.x, u.y);                  // sub_1124A (unit's current tile): centre the unit before walking
        if (should_exit_) return;
    }
    // sub_1620A: walk the path, then [+63h] = chosen facing; then sub_12124 (standing pose,
    // zero offsets).
    battle_move_walk(unit_index, facing);
    if (should_exit_) return;
    CombatUnitPlacement& u2 = units_[(size_t)unit_index];
    u2.x = tx;
    u2.y = ty;
    u2.facing = (int8_t)(facing & 3);
    reset_unit_pose(u2);                             // sub_12124
    // sub_11E04 (CC998 footprint restore): the port models occupancy from the units_ positions,
    // so no separate CC998 write is needed. The footprint is the raw D66A8 w x h (1-7 all 1x1).
    {
        int foot_w = 1, foot_h = 1;
        battle_d66a8_footprint(u2.sprite_type, u2.facing & 3, &foot_w, &foot_h);
        if ((foot_w != 1 || foot_h != 1) && battle_trace_)
            fprintf(stderr, "COMBAT: move unit=%d footprint %dx%d (sub_11E04 D66A8, base-tile applied)\n",
                    u2.id, foot_w, foot_h);
    }
    battle_wc1_occupancy_preserve_or_close("player unit movement");
    // sub_187B1/sub_18B2E (tile events, F037E 12 bytes), kept after 11E04 and before
    // 45559/5401B in the 16834+144/153 order. They do not fire in the 1-7 start area
    // (around 12,53), whose events are (5,6)/(59,51).
    {
        const int tile_ev = battle_tile_187b1(unit_index);
        if (tile_ev >= 0) battle_tile_18b2e(unit_index, tile_ev);
        if (should_exit_) return;
        if ((size_t)unit_index >= units_.size() || !units_[(size_t)unit_index].active) return;
    }
    const int moved_unit_id = u2.id;
    battle_after_move(unit_index);                   // sub_5401B: area -> zone unit-count trigger (u may be invalid after this)
    if (should_exit_) return;
    if ((size_t)unit_index >= units_.size() || !units_[(size_t)unit_index].active) return;
    // sub_17310 (terrain damage/heal): after 5401B and before the TP charge. 0 = death
    // (-2 equivalent: no TP spent, turn ends).
    if (battle_terrain_17310(unit_index) == 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: move unit=%d terrain death, turn ends (sub_17310=0)\n",
                                   moved_unit_id);
        if (should_exit_) return;
        // play_unit_death already removed the unit from the queue, zeroed the gauge and cleared
        // player_unit, so no further bookkeeping is needed. The original sets var_24=1 and goes to
        // 4DAAF (end turn); the port lets battle_player_end_turn(true) tidy the state.
        if (battle_player_unit_ == unit_index) battle_player_end_turn(true);
        return;
    }
    if (should_exit_) return;
    if ((size_t)unit_index >= units_.size() || !units_[(size_t)unit_index].active) return;
    // sub_13F8A (clear E8F46): skipped because the port keeps the turn diffusion in a vector and
    // overwrites it in the next begin_overlay.
    // loc_4D98E bookkeeping: consume = 10 + ((budget - E8[dest])/4)*10, then 45B4E.
    if (battle_move_e8dest_ >= 0) battle_consume_tp(unit_index, battle_move_tp_cost(battle_move_budget_, battle_move_e8dest_));
    else battle_consume_tp(unit_index, 10 + distance * 10); // fallback approximation
    if (battle_trace_) fprintf(stderr, "COMBAT: move unit=%d -> (%d,%d) facing=%d TP=%d\n", moved_unit_id, tx, ty, facing, battle_tp());
    if ((size_t)unit_index < units_.size() && battle_tp_changed()) units_[(size_t)unit_index].combat_acc57 = 0; // 83317
    // Rest of loc_4D98E + loc_4DA69/loc_4DA7B: with an attack available (1D124 && TP >= 40)
    // return to the ring; otherwise use the sub_167C9 threshold to re-move (16834) or end the turn.
    if ((size_t)unit_index >= units_.size() || !units_[(size_t)unit_index].active) return;
    if (battle_player_can_attack(unit_index) && battle_tp() >= 0x28) {
        // Back to the ring without a 167C9 refresh (4DA7B); var18 stays for the next move entry.
        battle_ring_return(unit_index);              // follow-up attack available -> ring
        return;
    }
    {
        const int thr = battle_move_again_threshold(unit_index); // sub_167C9
        battle_move_var18_ = thr; // loc_4DA69: var_18 = ax, then 4D98E (for the next budget and gate)
        if (battle_trace_) fprintf(stderr, "COMBAT: move var18=%d (167C9)\n", thr);
        if (battle_tp() >= thr && battle_tp() >= 10) {
            // var_24==0 path: when TP still meets the threshold, go straight to the next move
            // (4D98E->16834) without the ring.
            battle_move_begin(unit_index);
            return;
        }
    }
    // loc_4DA7B: ring when TP >= var18 and var_24 == 0, otherwise end the turn.
    if (battle_tp() >= battle_move_var18_ && !battle_var24_action_end_) battle_ring_return(unit_index);
    else battle_player_end_turn(battle_var24_action_end_);
}


// ==== walk animation (sub_1620A) ====
// The logic (g2move::walk_path) only emits drawing/timer/sfx events; here each snapshot is applied
// to the live unit and camera and drawn.
void CombatII::battle_move_walk(int unit_index, int facing) {
    // Player move: walk the nodes sub_15523 pushed onto battle_move_path_ (the sub_13AB1 queue).
    battle_walk_nodes(unit_index, battle_move_path_, facing);
    battle_move_path_.clear();
}

// Replay the sub_1620A body. stack = the sub_13AB1 queue in push order, popped from the back (the
// origin node is last). Shared by the player move (sub_16834) and the AI move (sub_40295 loc_407BF
// and four other sites). Camera alignment (sub_1124A) belongs to the caller, not to sub_1620A,
// because following the unit while walking (word_EDD70 in sub_15EB5) only turns on when the unit
// sits at the screen centre (tile + 6).
void CombatII::battle_walk_nodes(int unit_index, std::vector<g2move::PathNode> stack, int facing) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const CombatUnitPlacement& u0 = units_[(size_t)unit_index];
    g2move::WalkEnv env;
    env.map_w = (int)map_w_; env.map_h = (int)map_h_;
    env.map_kind = (int)sce_map_kind_;
    env.tile_attr = [this](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) return 0;
        const size_t i = (size_t)y * map_w_ + (size_t)x;
        return i < tile_attr_.size() ? (int)tile_attr_[i] : 0;
    };
    g2move::WalkUnitInfo info;
    info.code_b = u0.code_b; info.cls = u0.combat_class; info.flags14 = u0.combat_flags_14;
    g2move::WalkState st;
    st.ux = u0.x; st.uy = u0.y; st.facing = u0.facing; st.pose = u0.anim_frame;
    st.off_x = st.off_y = 0;
    st.cam_tx = cam_tile_x_; st.cam_ty = cam_tile_y_; st.cam_fx = cam_fine_x_; st.cam_fy = cam_fine_y_;

    bool aborted = false, last_draw = false;
    auto apply = [&](const g2move::WalkState& s) {
        CombatUnitPlacement& u = units_[(size_t)unit_index];
        u.x = s.ux; u.y = s.uy; u.facing = (int8_t)s.facing; u.anim_frame = (uint8_t)s.pose;
        u.walk_off_x = s.off_x; u.walk_off_y = s.off_y;
        cam_tile_x_ = s.cam_tx; cam_tile_y_ = s.cam_ty; cam_fine_x_ = s.cam_fx; cam_fine_y_ = s.cam_fy;
    };
    auto present = [&](const g2move::WalkState& s) {
        apply(s);
        render_battle_frame();
        video_.present();
        input_.update();                              // keep the window responsive (the original reads no input)
        if (input_.quit_requested()) { should_exit_ = true; aborted = true; }
    };
    g2move::walk_path(env, info, st, std::move(stack), facing,
        [&](g2move::WalkEv ev, int arg, const g2move::WalkState& s) {
            if (aborted) return;
            const bool is_draw = (ev == g2move::WalkEv::Draw);
            switch (ev) {
            case g2move::WalkEv::Draw:
                // sub_15BFB presents the same frame twice (page flip); once is enough in the port.
                if (!last_draw) present(s);
                break;
            case g2move::WalkEv::ScrollFrame: present(s); break;
            case g2move::WalkEv::DrawNoFlip: apply(s); break;          // sub_454B3: no present
            case g2move::WalkEv::Tick: apply(s); wait_game_tick(); break;
            case g2move::WalkEv::Timer: game_timer_set(arg); break;
            case g2move::WalkEv::Sfx: sfx(arg); break;
            }
            last_draw = is_draw;
        });
    apply(st);
}

// ==== rendering ====

// sub_49EEE/sub_49CB1: tile marker (cycling border colour + MAIN.CUR corners). tw/th come from
// sub_49E4E (dl==5 -> 24x16, otherwise the D66A8 footprint).
void CombatII::battle_draw_tile_marker(int tx, int ty, int tw, int th) {
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    if (!sel_corner_tried_) load_main_cur();
    const int sx = (tx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X; // sub_10971
    const int sy = (ty - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y; // sub_10988
    int x2 = sx + tw, y2 = sy + th;
    if (x2 > 0x130) x2 = 0x130;   // sub_49EEE: cmp FB1CE+2,130h
    if (y2 > 0xC3) y2 = 0xC3;     // cmp FB1C4,0C3h
    int x1 = sx - 1, y1 = sy - 1; // dec edx/dec eax
    if (x1 < 0x10) x1 = 0x10;     // cmp dx,10h
    if (y1 < 4) y1 = 4;           // cmp FB1C8+2,4
    // sub_49CB1: the colour is the top byte of dword_75021 and cycles ((colour-8)%3)+9 after each
    // draw. It shares the byte with the speaker frame, so sel_frame_color_ is used directly.
    const int8_t col = (int8_t)sel_frame_color_;
    sel_frame_color_ = (uint8_t)((((int)col - 8) % 3) + 9);
    int rx1 = std::min(x1, x2), rx2 = std::max(x1, x2);
    int ry1 = std::min(y1, y2), ry2 = std::max(y1, y2);
    auto put = [&](int x, int y) {
        if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) bb[(size_t)y * SCREEN_W + (size_t)x] = (uint8_t)col;
    };
    for (int x = rx1; x <= rx2; ++x) { put(x, ry1); put(x, ry2); }
    for (int y = ry1; y <= ry2; ++y) { put(rx1, y); put(rx2, y); }
    if (sel_corner_loaded_) {
        blit_sprite_rle(x1 - 1, y1 - 1, sel_corner_[0].data());
        blit_sprite_rle(x2 - 6, y1 - 1, sel_corner_[1].data());
        blit_sprite_rle(x1 - 1, y2 - 5, sel_corner_[2].data());
        blit_sprite_rle(x2 - 6, y2 - 5, sel_corner_[3].data());
    }
}

void CombatII::render_move_overlay() {
    if (battle_move_phase_ == 0) return;
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    // sub_49E4E/49EEE/49CB1: the destination marker is not a plain 0x0F line but the same style
    // as the speaker frame: border colour 9->10->11 (dword_75021+3) plus the MAIN.CUR corners
    // (sub_5F3FD). Its size is 1x1 = 24x16 (the 49E4E dl==4/5 path) and clipping follows 4A106
    // (clamp x2>0x130, y2>0xC3, x1<0x10, y1<4). The port redraws the world every frame, so the
    // background backup/restore (5EECF/4A210) is skipped.
    auto box = [&](int tx, int ty) {
        // Footprint: raw D66A8 entry[0]*24 / entry[1]*16 (as 49E4E). 1x1 = 24x16.
        int tw = TOWN_TILE_W, th = TOWN_TILE_H;
        if (battle_player_unit_ >= 0 && (size_t)battle_player_unit_ < units_.size()) {
            const auto& mu = units_[(size_t)battle_player_unit_];
            int foot_w = 1, foot_h = 1;
            battle_d66a8_footprint(mu.sprite_type, mu.facing & 3, &foot_w, &foot_h);
            if (foot_w > 1) tw = foot_w * TOWN_TILE_W;
            if (foot_h > 1) th = foot_h * TOWN_TILE_H;
        }
        battle_draw_tile_marker(tx, ty, tw, th);
    };
    const int w = (int)map_w_, h = (int)map_h_;
    if (battle_move_phase_ == 1) {
        // sub_4A483: draw the marker (49E4E/49EEE) only when the hovered tile is reachable and
        // free or self.
        const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
        const int tx = cam_tile_x_ + floor_div_(mx - cam_fine_x_ - TILE_ANCHOR_X, TOWN_TILE_W);
        const int ty = cam_tile_y_ + floor_div_(my - cam_fine_y_ - TILE_ANCHOR_Y, TOWN_TILE_H);
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) return;
        if (battle_player_diffusion_valid_ && battle_player_diffusion_.size() == (size_t)w * (size_t)h &&
            battle_player_diffusion_[(size_t)ty * (size_t)w + (size_t)tx] < 0) return;
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& o = units_[i];
            if (o.active && o.hp > 0 && o.x == tx && o.y == ty && (int)i != battle_player_unit_) return;
        }
        box(tx, ty);
        return;
    }

    // Phase 2: destination tile marker + facing arrow cursor (sub_5D3C5: 24x24, hotspot (8,8)).
    box(battle_move_dest_x_, battle_move_dest_y_);
    const int X = (battle_move_dest_x_ - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
    const int Y = (battle_move_dest_y_ - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
    const g2move::Box cb = g2move::confine_box(X, Y);
    const int mx = std::min(std::max(input_.get_mouse_x(), cb.x0), cb.x1);
    const int my = std::min(std::max(input_.get_mouse_y(), cb.y0), cb.y1);
    const int f = battle_face_choice_ & 3;
    if (move_cursor_.size() == 8 && !move_cursor_[(size_t)(f * 2 + (battle_face_blink_ & 1))].rle.empty()) {
        blit_sprite_rle(mx - 8, my - 8, move_cursor_[(size_t)(f * 2 + (battle_face_blink_ & 1))].rle.data());
        return;
    }
    // Fallback arrow: a small triangle pointing along the facing (the blink swaps the colour).
    const uint8_t col = (battle_face_blink_ & 1) ? 0x0F : 0x0C;
    static const int dxs[4] = {0, -1, 0, 1}, dys[4] = {1, 0, -1, 0};    // 0 south, 1 west, 2 north, 3 east
    const int dx = dxs[f], dy = dys[f];
    const int pxs = (dx == 0) ? 1 : 0, pys = (dy == 0) ? 1 : 0;       // axis perpendicular to the direction
    for (int t = 0; t <= 6; ++t) {                                     // t=0 base, t=6 tip
        for (int b = -(6 - t); b <= (6 - t); ++b) {
            const int fx = mx + dx * (t - 3) + pxs * b;
            const int fy = my + dy * (t - 3) + pys * b;
            if (fx >= 0 && fx < SCREEN_W && fy >= 0 && fy < SCREEN_H) bb[fy * SCREEN_W + fx] = col;
        }
    }
}
