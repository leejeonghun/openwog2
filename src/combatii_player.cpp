// Player attack command (sub_4D442 id 0 -> sub_1F738): target list, range, multi-target
// resolution and counterattack.
//
// Range computation (sub_1D124/sub_1CC09 and its iterators) lives in combatii_range.cpp (g2range);
// this file consumes its results (F0648 list, E8F46 grid) following the sub_1F738 flow.
//
// The original sub_1F738 runs blocking cursor loops (sub_1F19C/sub_1F466/sub_4A815) inside one
// call.
// The port is frame driven, so the same order becomes battle_atk_phase_ stages:
//   weapon 5/0Fh/13h : sub_1CC09 -> sub_1F19C(edx=1) -> the clicked hostile unit becomes the list
//   weapon 11h/12h   : sub_1CC09 -> sub_1F19C(edx=0) -> E8>=0 tile -> radius-1 diamond -> sub_4A815 confirm
//   weapon 0Bh/0Ch   : sub_1CC09 -> sub_1F466       -> 2x2 block click (an empty list keeps looping)
//   otherwise        : no prompt (reuse the F0648 list built by the menu gate)
// Return (sub_1F738): 0 = cancel, 1 = success or no target (TP 0x28 spent), 2 = attacker gone
// (TP = 0, turn over).
#include "combatii.h"
#include "combatii_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// ---- g2range adapter ----

g2range::Env CombatII::battle_range_env_make() {
    g2range::Env e;
    e.width = (int)map_w_;
    e.height = (int)map_h_;
    e.map_kind = (int)sce_map_kind_;
    e.attr = tile_attr_.empty() ? nullptr : tile_attr_.data();
    // Range::occupant applies the sub_11DB2 guard; occupancy lookup uses the port's 1x1
    // approximation.
    e.occupant_raw = [this](int x, int y) { return battle_unit_index_at(x, y); };
    e.unit_army = [this](int i) { return battle_effective_army(units_[(size_t)i]); };
    e.unit_flags14 = [this](int i) { return (int)units_[(size_t)i].combat_flags_14; };
    e.hostile = [this](int a, int b) { return battle_armies_hostile(a, b, battle_hostility_inverted_); };
    return e;
}

g2range::Unit CombatII::battle_range_unit(int unit_index) const {
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    g2range::Unit r;
    r.x = (int)(int8_t)u.x;
    r.y = (int)(int8_t)u.y;
    r.army = (int)(int8_t)battle_effective_army(u);
    r.cls = u.combat_class;
    r.flags14 = u.combat_flags_14;
    r.facing = u.facing & 3;
    r.weapon_class = weapon_class(u);
    return r;
}

// sub_1D124(unit, x, y, facing): rebuild F0648. Returns the count (dword_F0B9C+2).
int CombatII::battle_range_collect(int unit_index) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const g2range::Env env = battle_range_env_make();
    g2range::Range r(env);
    const int n = r.collect_targets(battle_range_unit(unit_index));
    battle_target_list_ = r.list;
    return n;
}

// 1D124(actor, x, y, dir) as called by sub_3EE2B - only the facing argument differs.
int CombatII::battle_range_collect_dir(int unit_index, int dir) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const g2range::Env env = battle_range_env_make();
    g2range::Range r(env);
    const int n = r.collect_targets_dir(battle_range_unit(unit_index), dir);
    battle_target_list_ = r.list;
    return n;
}

// Variant for the sub_3EE2B facing loop: the original shares the global E8F46 grid. The 13F8A
// clear and the LOS shadow left by wc5 (1D124 loc_1D2DC) survive into 165C2 (the E8 read) of the
// next facing, so the caller passes the grid in and gets it back. Without that, 165C2 stays
// closed after the first facing and later facings never produce candidates.
int CombatII::battle_range_collect_dir_grid(int unit_index, int dir,
                                            std::vector<int16_t>& grid) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const g2range::Env env = battle_range_env_make();
    g2range::Range r(env);
    r.e8 = grid;                       // inherit E8F46
    const int n = r.collect_targets_dir(battle_range_unit(unit_index), dir);
    grid = r.e8;                       // ending grid left by 1D124 (shared side effect)
    battle_target_list_ = r.list;
    return n;
}

// sub_1CC09(unit): mark the weapon range into E8F46.
void CombatII::battle_range_mark(int unit_index) {
    battle_range_e8_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const g2range::Env env = battle_range_env_make();
    g2range::Range r(env);
    r.mark_range(battle_range_unit(unit_index));
    battle_range_e8_ = r.e8;
}

// sub_4BD26 id 0 gate: attack is offered when 1D124 finds at least one target.
bool CombatII::battle_player_can_attack(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const auto& u = units_[(size_t)unit_index];
    if (!u.active || u.hp <= 0) return false;
    return battle_range_collect(unit_index) > 0;
}

// ---- input helpers ----

namespace {
int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}

// sub_109A3/sub_109CE: screen coordinates -> tile.
static void mouse_tile(int mx, int my, int cam_tx, int cam_ty, int fine_x, int fine_y,
                       int& tx, int& ty) {
    tx = cam_tx + floor_div(mx - fine_x - TILE_ANCHOR_X, TOWN_TILE_W);
    ty = cam_ty + floor_div(my - fine_y - TILE_ANCHOR_Y, TOWN_TILE_H);
}

static int e8_at(const std::vector<int16_t>& e8, int w, int h, int x, int y) {  // sub_13CBD (-128 outside)
    if (x < 0 || y < 0 || x >= w || y >= h) return -128;
    const size_t i = (size_t)y * (size_t)w + (size_t)x;
    return i < e8.size() ? (int)e8[i] : -128;
}

// ---- start / cancel ----

void CombatII::battle_attack_cancel() {
    battle_atk_phase_ = 0;
    battle_attack_select_ = false;
    battle_range_e8_.clear();
    battle_line_valid_ = false;
}

// sub_4D442 id 0 -> first half of sub_1F738. Weapons without a prompt are executed immediately.
// Original sub_4D674 loc_4D88F (83289-83312): right after the 4BA7F selection, 44D67(0) +
// 4B9D7 (hide) + 44DEC(0) run before 4D442, so entering an attack hides the ring; otherwise the
// 1E61D animation plays with the ring, dim and all-unit HP still on.
void CombatII::battle_attack_begin(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    // sub_4B9D7 equivalent: hide between selections (36->0 over 9 frames, FDAEE=0, FA8BA=0,
    // FA8B6=0).
    // Also close the sub-ring if it is open; battle_attack_finish clears battle_range_e8_.
    ring_close_anim();
    ring_sub_close();
    battle_overlay_mode_ = 0; // sub_44DEC(0) equivalent
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    // Same list the menu gate built (no unit or terrain changes in between).
    battle_range_collect(unit_index);
    const int wc = weapon_class(units_[(size_t)unit_index]);
    battle_atk_wc_ = wc;
    battle_atk_phase_ = 0;
    switch (wc) {
        case 5: case 0x0F: case 0x13:
            battle_range_mark(unit_index);
            battle_atk_phase_ = 1;
            break;
        case 0x11: case 0x12:
            battle_range_mark(unit_index);
            battle_atk_phase_ = 2;
            break;
        case 0x0B: case 0x0C: {
            battle_range_mark(unit_index);
            battle_atk_phase_ = 3;
            // sub_1F466: the cursor starts two tiles ahead (off_1F456: south y+2, west x-2, north
            // y-2, east x+2).
            const auto& u = units_[(size_t)unit_index];
            battle_line_cx_ = u.x;
            battle_line_cy_ = u.y;
            switch (u.facing & 3) {
                case 0: battle_line_cy_ += 2; break;
                case 1: battle_line_cx_ -= 2; break;
                case 2: battle_line_cy_ -= 2; break;
                default: battle_line_cx_ += 2; break;
            }
            battle_line_valid_ = false;
            battle_target_list_.clear();           // mov word ptr dword_F0B9C+2, 0
            break;
        }
        default:
            break;
    }
    if (battle_atk_phase_ != 0) {
        battle_attack_select_ = true;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: attack select unit=%d wc=%d phase=%d\n",
                    units_[(size_t)unit_index].id, wc, battle_atk_phase_);
        return;
    }
    battle_attack_select_ = false;
    battle_attack_finish(unit_index, battle_attack_execute(unit_index));
}

// ---- target-phase input ----

// Called every frame; true = this frame's input was consumed.
bool CombatII::battle_attack_input(int unit_index) {
    if (battle_atk_phase_ == 0 || unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const MouseState& m = input_.get_mouse();
    const bool left = input_.was_left_clicked();
    const bool right = m.right_clicked;
    const int w = (int)map_w_, h = (int)map_h_;
    int tx, ty;
    mouse_tile(input_.get_mouse_x(), input_.get_mouse_y(), cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_, tx, ty);
    // sub_455BD (83407): the target cursor loop (loc_4D98E) also checks HUD panel overlap each
    // iteration.
    battle_hud_panel_relocate(input_.get_mouse_x(), input_.get_mouse_y());

    // Inputs the original treats as cancel: Esc (1Bh) and a right click (event 3).
    const bool esc = input_.is_key_pressed(SDLK_ESCAPE);
    const bool cancel = esc || right;

    switch (battle_atk_phase_) {
        case 4: {  // sub_4A815: left click/Enter = 1, Esc/right click = -1 (clear the list, go to 1F789)
            const bool ok = left || input_.is_key_pressed(SDLK_RETURN) || input_.is_key_pressed(SDLK_KP_ENTER);
            if (!ok && !cancel) return false;
            if (cancel) battle_target_list_.clear();
            if (battle_target_list_.empty()) {      // cmp word ptr dword_F0B9C+2,0 ; jz loc_1F789
                battle_range_mark(unit_index);
                battle_atk_phase_ = 2;
                return true;
            }
            battle_atk_phase_ = 0;
            battle_attack_select_ = false;
            battle_attack_finish(unit_index, battle_attack_execute(unit_index));
            return true;
        }
        case 1:    // sub_1F19C(edx=1)
        case 2: {  // sub_1F19C(edx=0)
            if (cancel) {                            // return 0 -> loc_1F9FA
                if (battle_trace_) fprintf(stderr, "COMBAT: attack cancelled unit=%d\n", units_[(size_t)unit_index].id);
                battle_attack_cancel();
                battle_attack_finish(unit_index, 0);
                return true;
            }
            if (!left) return false;
            // Hover: target only when the occupant of an E8>=0 tile is hostile (edi); otherwise edi
            // = 0.
            int hover = -1;
            if (e8_at(battle_range_e8_, w, h, tx, ty) >= 0) {
                const int occ = battle_unit_index_at(tx, ty);
                // sub_11DB2 returns 0 outside the guard.
                const bool in_guard = (unsigned)(tx - 2) < (unsigned)(w - 4) && (unsigned)(ty - 4) < (unsigned)(h - 8);
                if (in_guard && occ >= 0 &&
                    battle_armies_hostile(battle_effective_army(units_[(size_t)unit_index]),
                                          battle_effective_army(units_[(size_t)occ]), battle_hostility_inverted_))
                    hover = occ;
            }
            if (battle_atk_phase_ == 1) {
                if (hover < 0) return true;           // test edi,edi ; jnz / cmp var_C,0 ; jnz -> keep looping
                battle_target_list_.assign(1, hover); // dword_F0B9C+2 = 1 ; F0648[0] = edi
                battle_atk_tx_ = tx;                  // sub_46E19
                battle_atk_ty_ = ty;
                battle_atk_phase_ = 0;
                battle_attack_select_ = false;
                battle_attack_finish(unit_index, battle_attack_execute(unit_index));
                return true;
            }
            // 11h/12h: accept any tile, but a negative grid value starts the pick over.
            battle_atk_tx_ = tx;
            battle_atk_ty_ = ty;
            if (e8_at(battle_range_e8_, w, h, tx, ty) < 0) {   // sub_13CBD ; jl loc_1F790
                return true;
            }
            const g2range::Env env = battle_range_env_make();
            g2range::Range r(env);
            r.area_diamond(battle_range_unit(unit_index), tx, ty, 1);
            battle_target_list_ = r.list;
            battle_range_e8_ = r.e8;                  // the diamond stays in E8F46 and is visible during sub_4A815
            battle_atk_phase_ = 4;
            return true;
        }
        case 3: {  // sub_1F466
            if (cancel) {
                if (battle_trace_) fprintf(stderr, "COMBAT: attack cancelled unit=%d\n", units_[(size_t)unit_index].id);
                battle_attack_cancel();
                battle_attack_finish(unit_index, 0);
                return true;
            }
            // Move event (code 1): the hovered tile's E8 must be non-negative; x and y advance only
            // where the neighbouring tile's E8 allows it.
            if (e8_at(battle_range_e8_, w, h, tx, ty) >= 0) {
                battle_line_valid_ = true;
                if (e8_at(battle_range_e8_, w, h, tx + 1, ty) >= 0) battle_line_cx_ = tx;
                if (e8_at(battle_range_e8_, w, h, tx, ty - 1) >= 0) battle_line_cy_ = ty;
            } else {
                battle_line_valid_ = false;           // mov [var_4], ecx(=0)
            }
            if (!left || !battle_line_valid_) return left;
            battle_atk_tx_ = battle_line_cx_;         // sub_46E19(var_10, var_C)
            battle_atk_ty_ = battle_line_cy_;
            const g2range::Env env = battle_range_env_make();
            g2range::Range r(env);
            r.collect_block2x2(battle_range_unit(unit_index), battle_line_cx_, battle_line_cy_);
            battle_target_list_ = r.list;
            if (battle_target_list_.empty()) return true;   // empty list keeps the loop going
            battle_atk_phase_ = 0;
            battle_attack_select_ = false;
            battle_attack_finish(unit_index, battle_attack_execute(unit_index));
            return true;
        }
        default:
            return false;
    }
}

// Where sub_4D442 id 0 handles the sub_1F738 return value (same settlement as around loc_4D95A).
void CombatII::battle_attack_finish(int unit_index, int ret) {
    battle_range_e8_.clear();
    battle_line_valid_ = false;
    if (should_exit_) return;
    if (battle_trace_) fprintf(stderr, "COMBAT: attack unit=%d ret=%d\n",
                               (size_t)unit_index < units_.size() ? units_[(size_t)unit_index].id : -1, ret);
    if (ret == 2) {                                   // attacker gone: TP = 0, then end (sub_1A0BF(0), ret -4)
        battle_tp_set(0);
        battle_var24_action_end_ = true;
        battle_player_end_turn(true);
        return;
    }
    if (ret == 1) battle_consume_tp(unit_index, 0x28); // 45B4E(28h)
    if (battle_tp_changed() && (size_t)unit_index < units_.size()) units_[(size_t)unit_index].combat_acc57 = 0; // 83317
    // Back to the ring: end the turn when TP < var18 (4DA7B), otherwise 1CC09 + ring (no move
    // spread).
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    if (battle_tp() < battle_move_var18_) {
        if (battle_trace_) fprintf(stderr, "COMBAT: ring TP=%d <var18=%d, turn ends\n", battle_tp(), battle_move_var18_);
        battle_player_end_turn(false);
        return;
    }
    battle_player_show_menu(unit_index);
}

// ---- execution (second half of sub_1F738) ----

int CombatII::battle_attack_execute(int unit_index) {
    const int ui = unit_index;
    if (ui < 0 || (size_t)ui >= units_.size()) return 0;
    const int wc = battle_atk_wc_;
    // sub_1F738 entry: `mov ebp, dword_F0648; mov ebp, [ebp+48h]` saves the first target's HP
    // (x100).
    // At the end (loc_1F9B7) subtracting the same target's HP gives the damage dealt.
    const int drain_target = battle_target_list_.empty() ? -1 : battle_target_list_.front();
    const int drain_hp_before = (drain_target >= 0 && (size_t)drain_target < units_.size())
                                    ? battle_hp_raw(units_[(size_t)drain_target]) : 0;
    // var_4: 1 for weapons that pick their own target (5/0B/0C/0F/10/11/12/13), which skip the
    // counterattack.
    const bool var4 = wc == 5 || wc == 0x0B || wc == 0x0C || wc == 0x0F || wc == 0x10 || wc == 0x11 ||
                      wc == 0x12 || wc == 0x13;

    // Camera: when var_4 == 1 and weapon != 10h, centre between the front tile and the chosen tile
    // (sub_1A1DE + sub_46DF1 -> sub_1124A).
    if (var4 && wc != 0x10) {
        const g2range::Env env = battle_range_env_make();
        g2range::Range r(env);
        const g2range::Range::Pt f = r.front_tile(battle_range_unit(ui), units_[(size_t)ui].facing & 3);
        const int mx = (int16_t)f.x + (((int16_t)battle_atk_tx_ - (int16_t)f.x) >> 1);
        const int my = (int16_t)f.y + (((int16_t)battle_atk_ty_ - (int16_t)f.y) >> 1);
        camera_center_on(mx, my);
        if (should_exit_) return 0;
    }

    const int cls = (int)units_[(size_t)ui].combat_class;
    std::vector<std::pair<int, BattleAttackRoll>> hits;
    if (cls >= 0x86 && cls <= 0x8B) {
        // Large units: up to 3 passes, each rebuilding the list with sub_1D124 (no 1EBFE).
        for (int di = 0; di < 3;) {
            const int r = battle_resolve_list(ui, true, &hits);
            if (should_exit_) return 0;
            if (r == -1) return 2;
            if (r == -2) return 1;
            if (di < 2) battle_counterattack_list(ui);
            if (should_exit_) return 0;
            if ((size_t)ui >= units_.size() || !units_[(size_t)ui].active) break;
            if (battle_range_collect(ui) == 0) break;
            ++di;
        }
    } else {
        const int r = battle_resolve_list(ui, true, &hits);
        if (should_exit_) return 0;
        if (r == -1) return 2;
        if (r == -2) return 1;
        // sub_1EBFE: physical-hit status effect on each target.
        for (const auto& h : hits) {
            battle_apply_1ebfe(ui, h.first, h.second.hit_kind, h.second.result);
            if (should_exit_) return 0;
        }
        if (!var4) battle_counterattack_list(ui);       // sub_1EF9A
        if (should_exit_) return 0;
        for (int t : battle_target_list_) battle_clear_ailment_flags(t, -1);  // sub_1E2B1
    }
    // sub_1F738 loc_1F9B7: drain class - `cmp byte ptr unk_73B5A[class+class*8],1` - when set,
    // heal the difference between ebp and the first target's current HP with sub_462A7
    // (no subtraction when the target is gone).
    if ((size_t)ui < units_.size() && kBattleClassData[units_[(size_t)ui].combat_class][2] == 1) {
        int amount = drain_hp_before;
        if (drain_target >= 0 && (size_t)drain_target < units_.size())
            amount -= battle_hp_raw(units_[(size_t)drain_target]);
        battle_heal_462a7(ui, amount);
        if (battle_trace_)
            fprintf(stderr, "COMBAT: drain unit=%d heal_raw=%d (sub_1F738 loc_1F9B7)\n",
                    units_[(size_t)ui].id, amount);
    }
    // sub_11FF7(eff-1, attacker): 1 while still in the army list, 0 otherwise (ASM: mov eax,1 / xor
    // eax,eax).
    if ((size_t)ui >= units_.size() || !units_[(size_t)ui].active || units_[(size_t)ui].hp <= 0) return 2;
    return 1;
}

// ---- sub_1DDF0 + sub_1E61D/1EF5C/1ED45 ----
// Returns 1 normally, -1 when the attacker died after the declaration, -2 when no target is left.
// declare == false is a counterattack (edx = 0).
int CombatII::battle_resolve_list(int attacker_index, bool declare,
                                  std::vector<std::pair<int, BattleAttackRoll>>* out) {
    if (out) out->clear();
    if (attacker_index < 0 || (size_t)attacker_index >= units_.size()) return -1;
    auto alive = [&](int i) {
        return i >= 0 && (size_t)i < units_.size() && units_[(size_t)i].active && units_[(size_t)i].hp > 0;
    };
    if (declare) {
        // sub_53797(attacker, F0648[0], 0) declaration event -> per-target sub_5394D (mode 0, table
        // 5) contact trigger.
        if (!battle_target_list_.empty() && alive(battle_target_list_[0]))
            run_unit_hp_trigger(attacker_index, battle_target_list_[0], 0);
        if (should_exit_) return -1;
        for (int t : battle_target_list_) {
            run_contact_trigger(attacker_index, t, 0);              // sub_5394D(mode 0)
            if (should_exit_) return -1;
        }
        if (!alive(attacker_index)) return -1;           // sub_11FF7 == 0
        // Drop vanished targets (null pointers); an empty list returns -2.
        battle_target_list_.erase(
            std::remove_if(battle_target_list_.begin(), battle_target_list_.end(),
                           [&](int i) { return !alive(i); }),
            battle_target_list_.end());
        if (battle_target_list_.empty()) return -2;
    }

    // The hit kind (sub_1D6B1) is decided once before the target loop.
    const int hit_kind = battle_hit_kind(units_[(size_t)attacker_index]);
    const std::vector<int> targets = battle_target_list_;
    std::vector<std::pair<int, BattleAttackRoll>> rolls;
    std::vector<int> damage;
    for (int t : targets) {
        // Keep the per-target order of roll (one random draw) -> damage (sub_1D770) identical to
        // the original.
        BattleAttackRoll roll = battle_attack_roll(attacker_index, t, hit_kind);
        int dmg = 0;                                      // x100 (negative = healing)
        if (roll.result != 2) dmg = battle_damage(attacker_index, t, roll.hit_kind, roll.area_mode, roll.def_bonus);
        rolls.push_back({t, roll});
        damage.push_back(dmg);
    }

    // sub_1E61D: attack presentation for all targets; the result lands in word_F0430 per target.
    // dword_F0B8A+2 = sub_1D6B1 hit_kind (0/1/2, 1DEA7) is passed to 1E2EA and the wind-up motion.
    VmEvent attack;
    attack.type = VmEventType::Attack;
    attack.attacker_unit = attacker_index;
    attack.attacker_mode = hit_kind;
    for (const auto& r : rolls) attack.attack_targets.push_back({r.first, 0, r.second.result});
    play_attack_sequence(attack);
    if (should_exit_) return 1;

    // Apply damage and deaths (sub_1ED45 -> sub_1DB1D). The original [+48h] is x100, kept exactly
    // as hp_deficit.
    for (size_t k = 0; k < rolls.size(); ++k) {
        const int t = rolls[k].first;
        const BattleAttackRoll& roll = rolls[k].second;
        if (roll.result != 2 && units_[(size_t)t].active) {
            auto& tgt = units_[(size_t)t];
            int new_raw = battle_hp_raw(tgt) - damage[k];
            if (new_raw > battle_max_hp_raw(tgt)) new_raw = battle_max_hp_raw(tgt);
            const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
            const int delta = new_hp - tgt.hp;
            tgt.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
            battle_recalc_summon_attack(t);                   // sub_17E17(target, damage)
            if (battle_trace_)
                fprintf(stderr, "COMBAT: attack unit=%d target=%d result=%d hit_kind=%d area=%d guard_bonus=%d damage_x100=%d hp %d -> %d\n",
                        units_[(size_t)attacker_index].id, tgt.id, roll.result, roll.hit_kind, roll.area_mode,
                        roll.def_bonus, damage[k], tgt.hp, new_hp);
            if (delta != 0) {                             // a delta of 0 is read as death by play_hp_change
                VmEvent hp;
                hp.type = VmEventType::Hp;
                hp.hp_unit = t;
                hp.hp_delta = delta;
                hp.hp_dir = units_[(size_t)attacker_index].facing;
                hp.hp_killer = attacker_index;
                play_hp_change(hp);
            }
        } else if (battle_trace_) {
            fprintf(stderr, "COMBAT: attack unit=%d target=%d result=dodge\n",
                    units_[(size_t)attacker_index].id, units_[(size_t)t].id);
        }
        if (should_exit_) return 1;
    }
    // sub_1ED45: targets that survived get the hit event (mode 1).
    for (const auto& r : rolls) {
        const int t = r.first;
        if (alive(t) && alive(attacker_index)) run_unit_hp_trigger(t, attacker_index, 1);
        if (should_exit_) return 1;
    }
    if (out) *out = rolls;
    return 1;
}

// ---- sub_1EF9A (counterattack) ----
// Each candidate in F0648 (the attacker's target list) that passes the gate builds its own
// sub_1D124 list and counterattacks once through sub_1DDF0 (edx = 0, no declaration).
void CombatII::battle_counterattack_list(int attacker_index) {
    if (attacker_index < 0 || (size_t)attacker_index >= units_.size()) return;
    const std::vector<int> candidates = battle_target_list_;
    std::vector<int> counters;
    {
        const auto& attacker = units_[(size_t)attacker_index];
        for (int idx : candidates) {
            if (idx < 0 || (size_t)idx >= units_.size()) continue;
            const auto& cand = units_[(size_t)idx];
            const int cls = (int)cand.combat_class;
            if (cls >= 0x86 && cls <= 0x8B) {
                if ((((int)cand.facing + 2) & 3) == (int)attacker.facing) counters.push_back(idx);
                continue;
            }
            const int wc = weapon_class(cand);
            const int expected = (wc == 8) ? 3 : ((wc == 3 || wc >= 7) ? 2 : 1);   // sub_1EF7C
            const int actual = std::abs(attacker.x - cand.x) + std::abs(attacker.y - cand.y);  // sub_49ADF
            if (actual > expected) continue;
            if (cand.combat_flags_1e & 0x22) continue;
            int fx = (int)(int8_t)cand.x, fy = (int)(int8_t)cand.y;                // sub_49ABC: one tile ahead
            switch (cand.facing & 3) {
                case 0: fy += 1; break;
                case 1: fx -= 1; break;
                case 2: fy -= 1; break;
                default: fx += 1; break;
            }
            bool ok = false;
            if ((cand.combat_flags_13 & 2) && battle_unit_index_at(fx, fy) == attacker_index) ok = true;
            else if (cand.combat_flags_13 & 4) ok = true;
            if (ok) counters.push_back(idx);
        }
    }

    // sub_1EE23: each candidate turns to face the attacker (sub_137B0).
    auto turn = [&](int idx, int dir) {
        auto& u = units_[(size_t)idx];
        const bool plain = u.sprite_type < 0x12C && !(u.combat_flags_1e & 2);
        if (plain || (u.code_b >= 0x27 && u.code_b <= 0x2C)) u.facing = (int8_t)(dir & 3);
    };
    {
        const auto& A = units_[(size_t)attacker_index];
        const int af = A.facing & 3;
        if (weapon_class(A) == 4) {
            const g2range::Env env = battle_range_env_make();
            g2range::Range r(env);
            const g2range::Unit au = battle_range_unit(attacker_index);
            int16_t sx = (int16_t)au.x, sy = (int16_t)au.y;   // 1EE23: one tile ahead via 49ABC
            switch (af) {
                case 0: sy += 1; break;
                case 1: sx -= 1; break;
                case 2: sy -= 1; break;
                default: sx += 1; break;
            }
            const int dirs[3] = {(af + 2) & 3, (af + 3) & 3, (af + 1) & 3};
            const auto tiles = r.tri3_tiles(sx, sy, af);
            for (size_t i = 0; i < tiles.size() && i < 3; ++i) {
                const int occ = battle_unit_index_at(tiles[i].x, tiles[i].y);
                const bool in_guard = (unsigned)((int16_t)tiles[i].x - 2) < (unsigned)((int)map_w_ - 4) &&
                                      (unsigned)((int16_t)tiles[i].y - 4) < (unsigned)((int)map_h_ - 8);
                if (in_guard && occ >= 0 &&
                    battle_armies_hostile(battle_effective_army(A), battle_effective_army(units_[(size_t)occ]),
                                          battle_hostility_inverted_))
                    turn(occ, dirs[i]);
            }
        } else {
            for (int idx : candidates)
                if (idx >= 0 && (size_t)idx < units_.size()) turn(idx, (af + 2) & 3);
        }
    }

    for (int c : counters) {
        if (c < 0 || (size_t)c >= units_.size()) continue;
        if (battle_range_collect(c) <= 0) continue;        // sub_1D124 for the candidate
        if (battle_trace_)
            fprintf(stderr, "COMBAT: counterattack unit=%d targets=%zu (sub_1EF9A)\n", units_[(size_t)c].id,
                    battle_target_list_.size());
        battle_resolve_list(c, false, nullptr);            // sub_1DDF0(edx=0) + 1E61D + 1EF5C + 1ED45
        if (should_exit_) return;
        for (int t : battle_target_list_) battle_clear_ailment_flags(t, -1);  // sub_1E2B1
    }
}

// ---- rendering ----

void CombatII::render_attack_range() {
    // sub_1547F paints the shared E8F46+2 grid directly (44D67(1) gate).
    // The caller (render_player_move_range) separates attack targets from the ring; looking at
    // atk_phase here would make the ring phase (atk 0) dead code (4D674 runs
    // 1CC09->44D67(1)->4B767).
    if (battle_range_e8_.empty()) return;
    if (map_w_ == 0 || map_h_ == 0) return;
    if (battle_range_e8_.size() != (size_t)map_w_ * (size_t)map_h_) return;
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    const uint8_t* lut = battle_move_shade_lut();         // sub_6104A(1)
    for (int r = 0; r < 13; ++r) {
        for (int c = 0; c < 13; ++c) {
            const int tx = cam_tile_x_ + c, ty = cam_tile_y_ + r;
            if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) continue;
            if (battle_range_e8_[(size_t)ty * map_w_ + (size_t)tx] < 0) continue;   // cmp word,0 ; jl
            const int sx = c * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
            const int sy = r * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
            for (int y = 0; y < TOWN_TILE_H; ++y) {
                const int py = sy + y;
                if (py < 0 || py >= SCREEN_H) continue;
                uint8_t* line = bb + (size_t)py * SCREEN_W;
                for (int x = 0; x < TOWN_TILE_W; ++x) {
                    const int px = sx + x;
                    if (px < 0 || px >= SCREEN_W) continue;
                    line[px] = lut[line[px]];
                }
            }
        }
    }
}
