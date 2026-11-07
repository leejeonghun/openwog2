// combatii_ai: AI decisions (watch/spread/WC1-WC5 target selection/chase/move) and warp.
// Split TU of combatii.cpp; code and comments moved here unchanged.
#include "combatii.h"
#include "combatii_internal.h"
#include "battle_tables.h"
#include "combatii_progression_tables.h"

// sub_3E18B (dasm 61565..61635): distance state for watch target / direct target / home.
// Returns -1 = watch-list target absent or within 1 tile, 0 = [+71h] is FFh and matches
// home-x, 1 = target farther than 1 tile, 2 = signed position differs from both
// zero-extended home coordinates. -2 is a port-only diagnostic for selectors outside the
// seven list heads (7..254), not an original return value.
// sub_1379A(sel,id): walk the dword_CC960[sel*8] linked list and return the first unit
// whose [+0Fh] equals id. sub_4ED18/sub_1134F insert a record at CC968+(effective_army-1)*8,
// so selectors 0..5 address the effective-army lists 0..5 and selector 6 addresses the
// global CC990 list. The original does not test HP here; active filters the port's copy of
// records that have left the army list. List order follows list_seq, the modeled [+7Bh]
// insertion order (initial load = 0, later tail inserts > 0).
int CombatII::battle_ai_cc960_member(int selector, int id) const {
    if (selector < 0 || selector > 6) return -1;
    int best = -1;
    uint64_t best_key = ~0ull;
    for (size_t i = 0; i < units_.size(); ++i) {
        const CombatUnitPlacement& c = units_[i];
        if (!c.active || c.id != id) continue;
        if (selector != 6 && battle_effective_army(c) != selector) continue;
        const uint64_t key = ((uint64_t)c.list_seq << 32) | (uint64_t)i;
        if (best < 0 || key < best_key) {
            best = (int)i;
            best_key = key;
        }
    }
    return best;
}

int CombatII::battle_ai_watch_state(int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const int unit_x = (int)(int8_t)u.x;
    const int unit_y = (int)(int8_t)u.y;

    if (u.ai_watch_sel != 0xFF) {
        const int selector = (int)u.ai_watch_sel;
        if (selector >= 0 && selector <= 6) {
            const int target = battle_ai_cc960_member(selector, (int)u.ai_watch_id);
            if (target < 0) return -1; // sub_1379A found no such ID in that list
            const CombatUnitPlacement& candidate = units_[(size_t)target];
            const int distance = std::abs(unit_x - (int)(int8_t)candidate.x) +
                                 std::abs(unit_y - (int)(int8_t)candidate.y);
            return distance > 1 ? 1 : -1;
        }
        // sel>=7 runs into the CC998 occupancy map, so it is no valid list descriptor.
        return -2; // port diagnostic: outside the modeled selector range
    }

    // [+73h] is a BattleUnit pointer in the original; the port reads the stored unit index.
    if (u.ai_target_unit >= 0 && (size_t)u.ai_target_unit < units_.size()) {
        const CombatUnitPlacement& target = units_[(size_t)u.ai_target_unit];
        const int distance = std::abs(unit_x - (int)(int8_t)target.x) +
                             std::abs(unit_y - (int)(int8_t)target.y);
        return distance > 1 ? 1 : -1;
    }

    // sub_3E18B loads AH from [+71h]; this is the [+71h]==FFh branch.
    // loc_3E1EE: cmp ah,[+6Fh] returns 0 on equality, so home-x==FFh is the 0 case here.
    if (u.ai_home_x == 0xFF) return 0;
    // The remaining home branch compares signed position with zero-extended home coords.
    if (unit_x == (int)u.ai_home_x) return -1;
    if (unit_y == (int)u.ai_home_y) return -1;
    return 2;
}

// sub_3F321 (dasm 63212..63306): AI action state 0/1/2/-1.
// byte_BA84E is compared signed (jge) and the [+48h] HP test is a signed dword (jg).
// [+29h]/[+2Ah] are updated and sub_499BD(12) called only when [+6D] is neither 0 nor 4;
// unk_74144 is indexed by [+2Ah]+[+29h]*20, the critical-pattern table layout.
// Rows outside 0..11 make the original read bytes past the table; the port uses 0
// instead of reading OOB (diagnostic approximation, unused by scan gates).
int CombatII::battle_ai_3f321_state(CombatUnitPlacement& u) {
    if ((int8_t)sce_map_kind_ >= 4) return 0;
    if (u.ai_field_81 != 0) return 1;
    if (u.ai_field_7f != 0) return 2;
    if (u.ai_mode != -1 && battle_hp_raw(u) > 0x3E8) return 1;
    if (u.ai_field_80 != 0xFF) return 2;
    if (u.ai_field_6d == 4) return -1;
    if (u.ai_field_6d == 0) return 0;
    if ((int16_t)u.sprite_type >= 0x12C || u.code_b != 0) return 0;
    if (u.ai_random_2a == 0) u.ai_random_29 = (uint8_t)combat_rand(12);
    u.ai_random_2a = (uint8_t)(((unsigned)u.ai_random_2a + 1u) % 20u);
    int table = 0;
    if (u.ai_random_29 < 12)
        table = (int)kBattleHitPattern[(size_t)u.ai_random_29 * 20u + (size_t)u.ai_random_2a];
    if (u.ai_field_6d == 1) return table > 0x0B ? 0 : 1;
    if (u.ai_field_6d == 2) {
        if (table <= 5) return 1;
        if (table <= 0x0D) return 2;
        return 0;
    }
    return 0;
}

// sub_1665B (dasm 9437..9460): combat_class -> mobility selector.
// Reads unk_70A68[class] and falls back to 0 when selector==5 but [+14h]&1==0.
// dword_EDD6C = unk_70B0D + selector*256; out-of-range selectors return -1 (unmodeled).
int CombatII::battle_ai_mobility_selector(const CombatUnitPlacement& u) const {
    const int cls = (int)u.combat_class;
    if (cls < 0 || cls >= kMobilitySelectorCount) return -1;
    int sel = (int)kMobilitySelectorTable[cls];
    if (sel == 5 && ((u.combat_flags_14 & 1) == 0)) sel = 0;
    if (sel < 0 || sel >= kMobilityGridCount) return -1;
    return sel;
}

// sub_16699 footprint gate (dasm 9461..9578).
// The original walks the unk_D66A8 descriptor footprint against CC998 occupancy
// and unk_88BD4 attr -> dword_EDD6C (unk_70B0D grid) cost. The base tile is the
// bottom-left cell: (x..x+w-1, y-h+1..y), where w/h are entry[0]/[1] of
// the D66A8 record for the actor's appearance and current facing. Every cell
// must pass the padded bounds, signed cost>0 and self-excluding occupancy tests.
bool CombatII::battle_ai_candidate_tile_legal_1x1(int x, int y, int unit_index,
                                                  int mobility_selector) const {
    if (mobility_selector < 0 || mobility_selector >= kMobilityGridCount) return false;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return false;
    int foot_w = 1, foot_h = 1;
    const auto& actor = units_[(size_t)unit_index];
    battle_d66a8_footprint(actor.sprite_type, (int)actor.facing & 3, &foot_w, &foot_h);
    if (foot_w < 1) foot_w = 1;
    if (foot_h < 1) foot_h = 1;
    for (int dy = 0; dy < foot_h; dy++) {
        const int cy = y - (foot_h - 1) + dy; // entry rows extend upward from the base tile
        for (int dx = 0; dx < foot_w; dx++) {
            const int cx = x + dx;
            // Same padded edge as the sub_11DB2 guard, shared with the WC1 threat scan.
            if (cx < 2 || cx >= (int)map_w_ - 2 || cy < 4 || cy >= (int)map_h_ - 4) return false;
            const size_t tile_idx = (size_t)cy * map_w_ + (size_t)cx;
            if (tile_idx >= tile_attr_.size()) return false;
            const int attr = (int)tile_attr_[tile_idx]; // unk_88BD4 equivalent
            if (attr < 0 || attr >= kMobilityGridStride) return false;
            const int8_t cost = (int8_t)kMobilityCostGrids[mobility_selector][attr];
            if (cost <= 0) return false; // `cmp byte,0; jle fail` (0 and 0xFF both rejected)
            const int occupant = battle_unit_index_at_footprint(cx, cy);
            if (occupant >= 0 && occupant != unit_index) return false;
        }
    }
    return true;
}

// Handles only the home-reference path of sub_3E251 (dasm 61669..61804).
// Requires no [+73h] target, [+71h]==FFh, a valid home and a radius-9 diamond.
// sub_1AC08/sub_1AC59 walk the radius-9 Manhattan diamond (181 cells) in y-then-x
// order; sub_1AD2E yields the horizontal edge distance ad2e=min(col,width-1-col).
// score = dist + 18 - 2*ad2e using the sub_49ADF Manhattan distance; lowest wins,
// first on ties. Legality uses only the 1x1 approximation above; no move is composed.
bool CombatII::battle_ai_no_target_home_candidate(int unit_index, int& out_x, int& out_y,
                                                  int& out_score, int& out_ad2e, int& out_dist,
                                                  const char** reject_reason) const {
    if (reject_reason) *reject_reason = nullptr;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) {
        if (reject_reason) *reject_reason = "unit index out of range";
        return false;
    }
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (!u.active) {
        if (reject_reason) *reject_reason = "inactive unit";
        return false;
    }
    if (u.ai_target_unit >= 0 || u.ai_target_marker) {
        if (reject_reason) *reject_reason = "[+73h] target present (target/watch path unported)";
        return false;
    }
    if (u.ai_watch_sel != 0xFF) {
        if (reject_reason) *reject_reason = "[+71h] watch present (watch path unported)";
        return false;
    }
    if (u.ai_home_x == 0xFF && u.ai_home_y == 0xFF) {
        if (reject_reason) *reject_reason = "home (FFh,FFh) has no reference";
        return false;
    }
    // Home (FFh,0), seen for ids 4/3 of the 1-5 samples, also differs from the original
    // home branch and is skipped. loc_3E1EE returns state 0 on home-x==FFh while sub_3E251
    // still writes home coordinates, but (FFh,*) is off-map so no diamond can exist.
    if (u.ai_home_x == 0xFF) {
        if (reject_reason) *reject_reason = "home-x FFh (no-target home reference unmodeled)";
        return false;
    }
    const int mobility = battle_ai_mobility_selector(u);
    if (mobility < 0) {
        if (reject_reason) *reject_reason = "mobility selector unmodeled (class out of range)";
        return false;
    }
    // Requires the same static 1x1 unambiguous condition as the WC1 occupancy gate;
    // without proof that vector occupancy maps 1:1 onto CC998 no candidate is asserted.
    if (!battle_wc1_static_occupancy_unambiguous()) {
        if (reject_reason) {
            *reject_reason = !battle_sce_snapshot_complete_
                ? "complete static SCE snapshot unavailable"
                : !battle_wc1_occupancy_state_known_
                    ? "occupancy-state latch closed after an unmodeled mutation"
                    : "static occupancy or loaded class-0 .NC2 descriptors not proven";
        }
        return false;
    }
    const int home_x = (int)u.ai_home_x;
    const int home_y = (int)u.ai_home_y;
    const int actor_x = (int)(int8_t)u.x;
    const int actor_y = (int)(int8_t)u.y;
    static constexpr int kRadius = 9; // sub_3E251 `mov ebx,9`
    int best_score = 0x7FFF;
    int best_x = -1, best_y = -1, best_ad2e = 0, best_dist = 0;
    bool found = false;
    for (int dy = -kRadius; dy <= kRadius; ++dy) {
        const int y = home_y + dy;
        const int half = kRadius - (dy < 0 ? -dy : dy);
        const int width = half * 2 + 1;
        for (int col = 0; col < width; ++col) {
            const int x = home_x - half + col;
            const int ad2e = col < width - 1 - col ? col : width - 1 - col;
            // sub_3E251 does not clear the actor footprint, so the actor's own tile is
            // blocked as well; legal_1x1 allows self, hence the explicit skip.
            if (x == actor_x && y == actor_y) continue;
            if (!battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility)) continue;
            const int dist = std::abs(actor_x - x) + std::abs(actor_y - y); // sub_49ADF
            const int score = dist + 0x12 - ad2e * 2;
            if (score < best_score) {
                best_score = score;
                best_x = x; best_y = y; best_ad2e = ad2e; best_dist = dist;
                found = true;
            }
        }
    }
    if (!found) {
        if (reject_reason) *reject_reason = "no legal 1x1 candidate in radius-9 diamond";
        return false;
    }
    out_x = best_x; out_y = best_y; out_score = best_score; out_ad2e = best_ad2e; out_dist = best_dist;
    return true;
}

// AI walk animation: reuses the original sub_16D3E (group-0 path walk) playback.
// full_path[0] is the current tile and the last entry the destination. Four subframes per
// tile (timer 9); on completion (or ESC skip) the logical position snaps to the
// destination and the offset is cleared. Returns false when should_exit_. The ATB
// scheduler calls this once per unit, so no other unit interleaves while walking.
// Exact port of sub_1093D + sub_1A2D7 (base bucket) + sub_1A2FE (candidate gate).
// sub_1093D reads the raw unk_88BD4[y*width+x] byte (tile_attr_ here).
// 1A2D7: when raw>=0x64 subtract once (byte math) and store the bucket with its low
// nibble cleared in F0BA4.
// 1A2FE: subtract once from the candidate raw too, then pass when
// (uint8)(cand_base - bucket) < 0x10.
// The 1A2D7 base at sub_3CA3C/sub_3D7B2 entry is the hostile tile, not the actor; only
// the WC>=3 branch of sub_1D124 uses the actor, so this is not generalised. Out of
// range fails closed.
bool CombatII::battle_ai_terrain_bucket_compatible(int base_x, int base_y, int cand_x, int cand_y) const {
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return false;
    if (base_x < 0 || base_y < 0 || base_x >= (int)map_w_ || base_y >= (int)map_h_) return false;
    if (cand_x < 0 || cand_y < 0 || cand_x >= (int)map_w_ || cand_y >= (int)map_h_) return false;
    const size_t bi = (size_t)base_y * map_w_ + (size_t)base_x;
    const size_t ci = (size_t)cand_y * map_w_ + (size_t)cand_x;
    if (bi >= tile_attr_.size() || ci >= tile_attr_.size()) return false;
    uint8_t base_raw = tile_attr_[bi];
    if (base_raw >= 0x64) base_raw = (uint8_t)(base_raw - 0x64);
    const uint8_t bucket = (uint8_t)(base_raw & 0xF0); // `and al,0Fh; sub F0BA4,al`
    uint8_t cand_raw = tile_attr_[ci];
    if (cand_raw >= 0x64) cand_raw = (uint8_t)(cand_raw - 0x64);
    return (uint8_t)(cand_raw - bucket) < 0x10; // `cmp al,10h; setb al`
}

// Port of the decision step: sub_14032 (blocker marks) + sub_143F3 (cost spread).
// Only a 1x1 ordinary footprint is supported, so the single actor cell is seeded with
// the budget (sub_1430E equivalent). Blockers use gate-proven 1x1 vector occupancy
// instead of CC998. Reproduces the E65B0 fringe, the LIFO stack spread, strict max
// updates and the budget+1 actor refresh. sub_1419C (-0x1E fringe), the execution tail
// (0xD2 budget) and path restoration are not included.
std::vector<int> CombatII::battle_ai_build_diffusion(int unit_index, int budget,
                                                     std::vector<char>* out_fringe) const {
    std::vector<int> grid;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return grid;
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return grid;
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    if (!actor.active) return grid;
    int mobility = battle_ai_mobility_selector(actor);
    if (mobility < 0) return grid;
    const int actor_eff = battle_effective_army(actor);
    // sub_143F3 special case for army 9: use the selector 5 grid.
    if (actor_eff == 9) mobility = 5;
    if (mobility < 0 || mobility >= kMobilityGridCount) return grid;
    const int actor_x = (int)(int8_t)actor.x;
    const int actor_y = (int)(int8_t)actor.y;
    if (actor_x < 0 || actor_y < 0 || actor_x >= map_w || actor_y >= map_h) return grid;
    grid.assign((size_t)map_w * (size_t)map_h_, -1); // E8F46+2 = 0xFFFF
    std::vector<char> visited((size_t)map_w * (size_t)map_h_, (char)0xFF); // unk_E65B0 = 0xFF
    // sub_14032 blocker set per actor effective army ([+0Eh] based):
    // 1/2 -> {3,4}, 3 -> {1,2,4}, 4 -> {1,2,3}, 9 -> {8}, no marking otherwise.
    auto is_blocker_army = [&](int cell_eff) {
        switch (actor_eff) {
            case 1:
            case 2: return cell_eff == 3 || cell_eff == 4;
            case 3: return cell_eff == 1 || cell_eff == 2 || cell_eff == 4;
            case 4: return cell_eff == 1 || cell_eff == 2 || cell_eff == 3;
            case 9: return cell_eff == 8;
            default: return false;
        }
    };
    static constexpr int kDirX[4] = { 0, -1, 0, 1 }; // S,W,N,E (order does not affect values)
    static constexpr int kDirY[4] = { 1, 0, -1, 0 };
    for (size_t i = 0; i < units_.size(); ++i) {
        const auto& u = units_[i];
        if (!u.active || u.hp <= 0) continue;
        if (!is_blocker_army(battle_effective_army(u))) continue;
        const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
        if (ux < 0 || uy < 0 || ux >= map_w || uy >= map_h) continue;
        const int idx = uy * map_w + ux;
        grid[(size_t)idx] = -(battle_effective_army(u) + 1); // `inc esi; neg esi`
        for (int d = 0; d < 4; ++d) {
            const int nx = ux + kDirX[d], ny = uy + kDirY[d];
            if (nx < 0 || ny < 0 || nx >= map_w || ny >= map_h) continue;
            const int nidx = ny * map_w + nx;
            if (grid[(size_t)nidx] == -1) visited[(size_t)nidx] = (char)0xF9;
        }
    }
    // sub_13FB5 two-cell border set to -1 at the end of sub_14032. Already -1 from the
    // init, but a blocker on the border overwrote it; E65B0 is left untouched.
    for (int y = 0; y < map_h; ++y) {
        for (int x = 0; x < map_w; ++x) {
            if (x < 2 || y < 2 || x >= map_w - 2 || y >= map_h - 2)
                grid[(size_t)y * (size_t)map_w + (size_t)x] = -1;
        }
    }
    // sub_1430E: seed and push the actor footprint with the budget; 1x1 means one cell.
    const int start_idx = actor_y * map_w + actor_x;
    grid[(size_t)start_idx] = budget;
    std::vector<int> stack;
    stack.push_back(start_idx);
    auto push_bounded = [&](int nidx) {
        // sub_13AB1: past 0xA0 entries drop the whole queue and reset count to 0.
        if (stack.size() >= 0xA0) { stack.clear(); return; }
        stack.push_back(nidx);
    };
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        const int cx = cur % map_w, cy = cur / map_w;
        const int remain = grid[(size_t)cur];
        if (remain <= 0) continue; // `test di,di; jle` (0 is stored but not spread)
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kDirX[d], ny = cy + kDirY[d];
            // sub_143F3 neighbour bounds: x in [2,w-3], y in [2,h-3] (south y+1<=h-3,
            // north y-1>=2, and likewise for the columns).
            if (nx < 2 || ny < 2 || nx >= map_w - 2 || ny >= map_h - 2) continue;
            const int nidx = ny * map_w + nx;
            const size_t ti = (size_t)ny * (size_t)map_w + (size_t)nx;
            if (ti >= tile_attr_.size()) continue;
            const int attr = (int)tile_attr_[ti];
            if (attr < 0 || attr >= kMobilityGridStride) continue;
            const int step_cost = (int)(int8_t)kMobilityCostGrids[mobility][attr];
            if (step_cost <= 0) continue; // `test di,di; jle`
            const int base = grid[(size_t)nidx];
            // A destination holding the blocker sentinel (-(army+1) <= -2) is not
            // updated, matching `cmp di,[var_8/var_18/var_1C]; jz skip` in the original
            // spread loop. -1 (unreached) is updated.
            if (base <= -2) continue;
            const int now = remain - step_cost;
            if (now < 0) continue; // `cmp ebp,-1; jle` (remain word)
            if (base >= now) continue; // strict max: `cmp di,ax; jge`
            grid[(size_t)nidx] = now;
            if (visited[(size_t)nidx] == (char)0xFF) push_bounded(nidx); // only when E65B0==-1
        }
    }
    // unk_E65B0 at sub_143F3 end-state is the visited array as-is (the spread loop only
    // reads E65B0). Copied before the actor-cell refresh (+1).
    if (out_fringe) *out_fringe = visited;
    grid[(size_t)start_idx] = budget + 1; // sub_13D02 refresh on sub_3CA3C entry (sub_14307+1)
    return grid;
}

// Port of the WC1 sub_3CA3C best-score search. The outer loop (sub_3E65E) walks the
// budget-radius diamond in y-then-x order and evaluates the inner loop for each hostile
// tile. The inner loop steps one tile per facing via sub_1A452(range 1)+sub_1A552, which
// equals testing the four hostile neighbours (S,W,N,E). The score is the E8F46 spread
// value (sub_13CBD, -128 when OOB); order is bucket -> score compare -> 165C2. 165C2 tests
// only occupancy (none or self) and spread>=0, ignoring terrain cost and y padding, so it
// is kept separate from 16699 legality. Per hostile, the inner loop picks (f,v) by strict
// max spread (first wins on ties); the outer loop picks the hostile by strict max of
// score = v + (actor facing == (f+2)%4 ? 4 : 0). The (f+2)%4 attack facing and the +4
// facing-match bonus are included.
bool CombatII::battle_ai_3ca3c_wc1_best(int unit_index, const std::vector<int>& diffusion,
                                       int budget_radius, int& out_hx, int& out_hy,
                                       int& out_dx, int& out_dy, int& out_score,
                                       int& out_attack_facing) const {
    out_hx = out_hy = out_dx = out_dy = -1;
    out_score = -1; // edi = 0FFFFFFFFh initial value (strict max)
    out_attack_facing = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    if ((int)diffusion.size() != map_w * map_h) return false;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    if (!actor.active) return false;
    const int actor_x = (int)(int8_t)actor.x;
    const int actor_y = (int)(int8_t)actor.y;
    const int actor_facing = (int)actor.facing;
    static constexpr int kAdjX[4] = { 0, -1, 0, 1 }; // facing 0,1,2,3 = S,W,N,E
    static constexpr int kAdjY[4] = { 1, 0, -1, 0 };
    auto wc1_165c2_ok = [&](int nx, int ny, int v) {
        // The real sub_165C2 tests only occupancy (none or self) and spread>=0, never
        // terrain cost. sub_11DB2's padding guard (x in [2,W-3], y in [4,H-5]) reports
        // "no occupancy" outside the padding, so those cells still pass when spread>=0
        // (a y=3 winner is accepted while the occupancy test is rejected).
        if (v < 0) return false; // sub_13CBD (-128 when OOB)
        if (nx >= 2 && nx < map_w - 2 && ny >= 4 && ny < map_h - 4) {
            const int occupant = battle_unit_index_at(nx, ny);
            if (occupant >= 0 && occupant != unit_index) return false;
        }
        return true;
    };
    bool found = false;
    for (int dy = -budget_radius; dy <= budget_radius; ++dy) {
        const int half = budget_radius - (dy < 0 ? -dy : dy);
        for (int dx = -half; dx <= half; ++dx) {
            // The centre cell is included: hostile_occupied returns false when not
            // inverted and treats itself as hostile when inverted (1201D xor), so no
            // separate exclusion is needed.
            const int hx = actor_x + dx, hy = actor_y + dy;
            if (!battle_ai_hostile_occupied(hx, hy, unit_index)) continue;
            // Inner loop: pick the best (f,v) per hostile by strict max spread, first wins.
            int best_v = -1, best_f = -1, best_nx = -1, best_ny = -1;
            for (int f = 0; f < 4; ++f) {
                const int nx = hx + kAdjX[f], ny = hy + kAdjY[f];
                if (!battle_ai_terrain_bucket_compatible(hx, hy, nx, ny)) continue; // sub_1A2FE
                // sub_13CBD: 0xFFFFFF80 (-128) when out of range.
                const int v = (nx < 0 || ny < 0 || nx >= map_w || ny >= map_h)
                    ? -128
                    : diffusion[(size_t)ny * (size_t)map_w + (size_t)nx];
                if (v <= best_v) continue; // strict max: `cmp di,ax; jge loop` (first wins)
                if (!wc1_165c2_ok(nx, ny, v)) continue; // sub_165C2
                best_v = v; best_f = f; best_nx = nx; best_ny = ny;
            }
            if (best_f < 0) continue; // unreachable hostile
            const int attack_facing = (best_f + 2) % 4; // `[esi]=(f+2)%4`
            const int hostile_score = best_v + (actor_facing == attack_facing ? 4 : 0);
            if (hostile_score <= out_score) continue; // outer strict max (first wins on ties)
            out_score = hostile_score;
            out_hx = hx; out_hy = hy;
            out_dx = best_nx; out_dy = best_ny;
            out_attack_facing = attack_facing;
            found = true;
        }
    }
    if (!found) return false;
    // Even when the destination is the actor's own tile this returns true: the zero-step
    // rotation updates only the facing (sub_15523 returns 2 on that path).
    return true;
}

// ---- WC5 (sub_3CA3C loc_3CC91) chain ----
// Triangle iterators sub_1AF22/sub_1B00D, sub_1AEF3, snapshots sub_1CB53/sub_1CBB1,
// sub_1B7D4 state, sub_1AC08/sub_1AC59 diamonds, sub_13CBD/sub_13D02,
// sub_1BA9C/sub_1C5CE/sub_1C62B, markers sub_1C046/sub_1BB62/sub_1BD1E/sub_1BF2E/
// sub_1C1B2/sub_1C31A, sub_1C685 flood+score, the WC5 inner loop and the sub_3E65E outer.
// 16-bit wrap is reproduced with wc5_s16. This is a direct transcription of a pass that
// was machine-verified against the E8F46 end-state.

// Wc5Iter is defined in combatii_internal.h, shared with combatii_aimage.cpp.

struct CombatII::Wc5Score {
    int b74 = 0;              // F0B74 (actor unit index; a pointer in the original)
    int b82 = 0;              // F0B82 (dest y)
    int b84x = 0, b84s = -1;  // F0B84 low (dest x) / high (score)
    int b8e = 0;              // F0B8E (mode)
    int b8a = 0;              // F0B8A (marker mode)
    std::vector<int> snap;    // F0A46/F0A48 shared scratch (same buffer)
};

// Wc5Dia is defined in combatii_internal.h.

namespace {
inline int wc5_s16(int v) {
    v &= 0xFFFF;
    return (v & 0x8000) ? v - 0x10000 : v;
}
}  // namespace

void CombatII::battle_ai_wc5_iter_init(Wc5Iter& it, int ax, int dx, int cx,
                                       int bx, int stack) const {
    it.x = wc5_s16(ax); it.y = wc5_s16(dx);
    it.c_lo = wc5_s16(cx); it.c_hi = wc5_s16(stack);
    it.n_lo = 0; it.n_hi = 0;
    if (bx >= 0 && bx <= 3) {
        if (bx == 0) {
            it.d96 = 0;
            it.d9c = wc5_s16(-wc5_s16(cx)); it.d9a = 1; it.d98 = 1;
        } else if (bx == 1) {
            it.d9a = wc5_s16(1 ^ 1); it.d96 = 1;
            it.d98 = wc5_s16(-wc5_s16(cx)); it.d9c = wc5_s16(-1);
        } else if (bx == 2) {
            it.d96 = 0;
            it.d9c = wc5_s16(cx); it.d9a = wc5_s16(-1); it.d98 = wc5_s16(-1);
        } else {
            it.d9a = 0; it.d98 = wc5_s16(cx);
            it.d96 = wc5_s16(-1); it.d9c = 1;
        }
    }
    it.done = 0;
}

bool CombatII::battle_ai_wc5_iter_next(Wc5Iter& it, int& out_x,
                                       int& out_y) const {
    if (it.done != 0) return false;
    const int x = it.x, y = it.y;
    const int cx = wc5_s16(it.n_lo + 1), ax = it.c_lo;
    it.n_lo = wc5_s16(cx);
    if (ax != cx) {
        it.x = wc5_s16(it.x + it.d9a);
        it.y = wc5_s16(it.y + it.d96);
        out_x = x; out_y = y;
        return true;
    }
    const int di = wc5_s16(it.n_hi + 1), dx = it.c_hi;
    it.n_hi = wc5_s16(di);
    if (di == dx) {
        it.done = 1;
        it.n_lo = 0;
        out_x = x; out_y = y;
        return true;
    }
    const int eb = wc5_s16(it.c_hi - it.n_hi);
    const int dx2 = wc5_s16(it.d96 + it.d96), ax2 = wc5_s16(it.d9a + it.d9a);
    if (eb == 1) {
        it.c_lo = wc5_s16(it.c_lo - 2);
        const int si = it.d9a;
        if (si == 0) it.d98 = wc5_s16(it.d98 + dx2);
        else it.d9c = wc5_s16(it.d9c + ax2);
        it.x = wc5_s16(it.x + it.d9c);
        it.y = wc5_s16(it.y + it.d98);
    } else {
        it.c_lo = wc5_s16(it.c_lo + 2);
        int cx3 = it.x;
        const int bx3 = it.d9c, bx4 = it.d98, di2 = it.d9a;
        int si = it.y;
        cx3 = wc5_s16(cx3 + bx3);
        si = wc5_s16(si + bx4);
        it.x = wc5_s16(cx3); it.y = wc5_s16(si);
        if (di2 != 0) it.d9c = wc5_s16(it.d9c - ax2);
        else it.d98 = wc5_s16(bx4 - dx2);
    }
    it.n_lo = 0;
    out_x = x; out_y = y;
    return true;
}

void CombatII::battle_ai_wc5_dia_init(Wc5Dia& it, int ax, int dx,
                                       int bx) const {
    it.x = wc5_s16(ax);
    it.c_hi = wc5_s16(bx);
    it.done = 0;
    it.y = wc5_s16(dx - wc5_s16(bx));
    it.c_lo = 1;
    it.n_lo = 0;
    it.n_hi = (bx != 0) ? 2 : wc5_s16(-2);
}

bool CombatII::battle_ai_wc5_dia_next(Wc5Dia& it, int& out_x,
                                       int& out_y) const {
    if (it.done != 0) return false;
    const int x = it.x, y = it.y;
    const int cx = wc5_s16(it.n_lo + 1), axc = it.c_lo;
    it.n_lo = wc5_s16(cx);
    if (axc != cx) {
        it.x = wc5_s16(it.x + 1);
    } else {
        if (it.n_hi > 0) it.x = wc5_s16(it.x - axc);
        int bx = it.y, axh = it.n_hi, cxx = it.c_lo;
        bx = wc5_s16(bx + 1);
        it.n_lo = 0;
        cxx = wc5_s16(cxx + axh);
        it.y = wc5_s16(bx);
        it.c_lo = wc5_s16(cxx);
        if (axh < 0) it.x = wc5_s16(it.x - cxx);
        const int bxh = wc5_s16(it.c_hi - 1);
        it.c_hi = wc5_s16(bxh);
        if (bxh == 0) it.n_hi = wc5_s16(-2);
        if (wc5_s16(it.c_lo) < 0) it.done = 1;
    }
    out_x = x; out_y = y;
    return true;
}

// sub_1AD2E: horizontal edge distance (fold) of the diamond iterator's "next cell".
//   The dword at 0xF0B7A = dir | (width << 16), and sar 17 uses only the 16-bit ax.
//   ax = (int16)((width << 16 | dir) >> 17) and counter is the low word of F0B78.
//   ax < counter -> width-counter-1, otherwise counter.
// 1AC59 advances the state before this call, so the fold reflects the row state of the
// cell after the one just processed (3F0E4 depends on this order).
int CombatII::battle_ai_wc5_1ad2e(const Wc5Dia& it) const {
    const int width = wc5_s16(it.c_lo);
    const int counter = wc5_s16(it.n_lo);
    const uint32_t raw = ((uint32_t)(uint16_t)width << 16) | (uint16_t)it.n_hi;
    const int32_t shifted = (int32_t)raw >> 17;
    const int ax = (int16_t)(uint16_t)(shifted & 0xFFFF);
    if (ax < counter) return wc5_s16(width - counter - 1);
    return counter;
}

int CombatII::battle_ai_wc5_aef3(int combat_class) const {
    return (combat_class == 0x15 || combat_class == 0x16) ? 5 : 7;
}

int CombatII::battle_ai_wc5_diff_read(const std::vector<int>& grid, int x,
                                       int y) const {
    if (map_w_ == 0 || map_h_ == 0) return -128;
    // sub_13CBD: -128 when (uint16)x>=W or (uint16)y>=H. There is no lower-bound test.
    const uint32_t xu = (uint32_t)(x & 0xFFFF), yu = (uint32_t)(y & 0xFFFF);
    if (xu >= map_w_ || yu >= map_h_) return -128;
    const size_t idx = (size_t)yu * map_w_ + xu;
    if (idx >= grid.size()) return -128;
    return grid[idx];
}

void CombatII::battle_ai_wc5_diff_write(std::vector<int>& grid, int x, int y,
                                        int v) const {
    if (map_w_ == 0 || map_h_ == 0) return;
    // sub_13D02 gate: x-2<W-4 and (y-4)&0xFFFF<H-8, i.e. x in [2,W-3], y in [4,H-5].
    const uint32_t xu = (uint32_t)(x & 0xFFFF), yu = (uint32_t)(y & 0xFFFF);
    if (((xu - 2) & 0xFFFFu) >= map_w_ - 4) return;
    if (((yu - 4) & 0xFFFFu) >= map_h_ - 8) return;
    const size_t idx = (size_t)yu * map_w_ + xu;
    if (idx >= grid.size()) return;
    grid[idx] = wc5_s16(v);
}

void CombatII::battle_ai_wc5_snapshot(const std::vector<int>& grid, Wc5Iter& it,
                                       Wc5Score& st, int ax, int dx, int cx,
                                       int bx, int stack) const {
    if (st.snap.size() < 256) st.snap.assign(256, 0);
    battle_ai_wc5_iter_init(it, ax, dx, cx, bx, stack);
    int bi = 0;
    int px = 0, py = 0;
    while (bi < 256 && battle_ai_wc5_iter_next(it, px, py)) {
        st.snap[(size_t)bi] = battle_ai_wc5_diff_read(grid, px, py);
        ++bi;
    }
}

void CombatII::battle_ai_wc5_restore(std::vector<int>& grid, Wc5Iter& it,
                                      Wc5Score& st, int ax, int dx, int cx,
                                      int bx, int stack) const {
    if (st.snap.size() < 256) return;
    battle_ai_wc5_iter_init(it, ax, dx, cx, bx, stack);
    int ci = 0;
    int px = 0, py = 0;
    while (ci < 256 && battle_ai_wc5_iter_next(it, px, py)) {
        battle_ai_wc5_diff_write(grid, px, py, st.snap[(size_t)ci]);
        ++ci;
    }
}

void CombatII::battle_ai_wc5_state_init(Wc5Score& st, int score_init,
                                         int actor) const {
    st.b84s = wc5_s16(score_init);
    st.b8e = 2;
    st.b74 = actor;
}

// sub_1BA9C. Conditional when F0B8A==2, the row-0 quirk when ==3, otherwise -2 always.
// The gate is x in [2,W-3], y in [4,H-5].
void CombatII::battle_ai_wc5_mark_cell(std::vector<int>& grid, Wc5Score& st,
                                        int x, int y) const {
    if (map_w_ == 0 || map_h_ == 0) return;
    const uint32_t xu = (uint32_t)(x & 0xFFFF), yu = (uint32_t)(y & 0xFFFF);
    if (((xu - 2) & 0xFFFFu) >= map_w_ - 4) return;
    if (((yu - 4) & 0xFFFFu) >= map_h_ - 8) return;
    const size_t idx = (size_t)yu * map_w_ + xu;
    if (idx >= grid.size()) return;
    if (st.b8a == 2) {
        const int v = (idx + 1 < grid.size()) ? grid[idx + 1] : -1;
        if (v > -15028) {
            if (v <= -10) {
                battle_ai_wc5_diff_write(grid, x, y, -2);
                return;
            }
        }
        const int v2 = (idx + 1 < grid.size()) ? grid[idx + 1] : -1;
        if (v2 < -1) return;
        battle_ai_wc5_diff_write(grid, x, y, -2);
        return;
    }
    if (st.b8a == 3) {
        const int v = (x + 1 >= 0 && (size_t)(x + 1) < grid.size())
                          ? grid[(size_t)(x + 1)]
                          : -1;
        if (v < -1) return;
        if (x >= 0 && (size_t)x < grid.size()) grid[(size_t)x] = -2;
        return;
    }
    battle_ai_wc5_diff_write(grid, x, y, -2);
}

// sub_1C5CE/sub_1C62B: v = -(v+add), where add is 0x3A98 or 0x0A.
void CombatII::battle_ai_wc5_flood_neg(std::vector<int>& grid, int x, int y,
                                       int add) const {
    if (map_w_ == 0 || map_h_ == 0) return;
    const uint32_t xu = (uint32_t)(x & 0xFFFF), yu = (uint32_t)(y & 0xFFFF);
    if (((xu - 2) & 0xFFFFu) >= map_w_ - 4) return;
    if (((yu - 4) & 0xFFFFu) >= map_h_ - 8) return;
    const size_t idx = (size_t)yu * map_w_ + xu;
    if (idx >= grid.size()) return;
    grid[idx] = wc5_s16(-(grid[idx] + add));
}

// sub_1C046(eax0, edx0, f, ecx0=rows). var_4 keeps edx0 unchanged.
void CombatII::battle_ai_wc5_m_c046(std::vector<int>& grid, Wc5Score& st,
                                    int eax0, int edx0, int f,
                                    int ecx0) const {
    if (f < 0 || f > 3) return;
    int edi = wc5_s16(eax0);
    int var_4 = wc5_s16(edx0);
    const int var_18 = wc5_s16(ecx0);
    int ebx = 1;
    if (f == 0) {
        int var_c = 0;
        while (wc5_s16(var_c) < wc5_s16(var_18)) {
            int ecx = edi, si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(ecx), var_4);
                si = wc5_s16(si + 1); ecx = wc5_s16(ecx + 1);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            var_4 = wc5_s16(var_4 + 1);
            if (var_c & 1) {
                ebx = wc5_s16(ebx + 2);
                edi = wc5_s16(edi - 1);
            }
            var_c = wc5_s16(var_c + 1);
        }
    } else if (f == 1) {
        int var_8 = 0;
        while (wc5_s16(var_8) < wc5_s16(var_18)) {
            int ecx = var_4, si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(edi), wc5_s16(ecx));
                si = wc5_s16(si + 1); ecx = wc5_s16(ecx + 1);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            edi = wc5_s16(edi - 1);
            if (var_8 & 1) {
                var_4 = wc5_s16(var_4 - 1);
                ebx = wc5_s16(ebx + 2);
            }
            var_8 = wc5_s16(var_8 + 1);
        }
    } else if (f == 2) {
        int var_10 = 0;
        while (wc5_s16(var_10) < wc5_s16(var_18)) {
            int si = edi, ecx = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(si), var_4);
                ecx = wc5_s16(ecx + 1); si = wc5_s16(si + 1);
                if (!(wc5_s16(ecx) < wc5_s16(ebx))) break;
            }
            var_4 = wc5_s16(var_4 - 1);
            if (var_10 & 1) {
                ebx = wc5_s16(ebx + 2);
                edi = wc5_s16(edi - 1);
            }
            var_10 = wc5_s16(var_10 + 1);
        }
    } else {
        int var_14 = 0;
        while (wc5_s16(var_14) < wc5_s16(var_18)) {
            int si = var_4, ecx = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(edi), wc5_s16(si));
                ecx = wc5_s16(ecx + 1); si = wc5_s16(si + 1);
                if (!(wc5_s16(ecx) < wc5_s16(ebx))) break;
            }
            edi = wc5_s16(edi + 1);
            if (var_14 & 1) {
                var_4 = wc5_s16(var_4 - 1);
                ebx = wc5_s16(ebx + 2);
            }
            var_14 = wc5_s16(var_14 + 1);
        }
    }
}

// sub_1BB62(eax0, edx0, f, ecx0, arg0). [var_28] is the entry eax.
void CombatII::battle_ai_wc5_m_bb62(std::vector<int>& grid, Wc5Score& st,
                                    int eax0, int edx0, int f, int ecx0,
                                    int arg0) const {
    if (f < 0 || f > 3) return;
    int edi = wc5_s16(edx0);
    int var_18 = wc5_s16(ecx0 - 1);
    int ebx = 1;
    const int dxword = wc5_s16(arg0);
    const int saved_eax = wc5_s16(eax0);
    if (f == 0) {
        const int var_14 = (dxword != 0) ? wc5_s16(-1) : 1;
        edi = wc5_s16(edi + 1);
        int var_1c = 0;
        while (wc5_s16(var_1c) < wc5_s16(var_18)) {
            int ecx = saved_eax, si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(ecx), wc5_s16(edi));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + var_14);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            var_1c = wc5_s16(var_1c + 1);
            edi = wc5_s16(edi + 1);
            ebx = wc5_s16(ebx + 1);
        }
    } else if (f == 1) {
        const int var_10 = (dxword != 0) ? wc5_s16(-1) : 1;
        int var_28 = wc5_s16(saved_eax - 1), var_24 = 0;
        while (wc5_s16(var_24) < wc5_s16(var_18)) {
            int ecx = wc5_s16(edi), si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, saved_eax, wc5_s16(ecx));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + var_10);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            var_28 = wc5_s16(var_28 - 1);
            var_24 = wc5_s16(var_24 + 1);
            ebx = wc5_s16(ebx + 1);
        }
    } else if (f == 2) {
        const int var_8 = (dxword != 0) ? 1 : wc5_s16(-1);
        int edi2 = wc5_s16(edi - 1), var_20 = 0;
        while (wc5_s16(var_20) < wc5_s16(var_18)) {
            int si = saved_eax, ecx = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(si), wc5_s16(edi2));
                ecx = wc5_s16(ecx + 1);
                si = wc5_s16(si + var_8);
                if (!(wc5_s16(ecx) < wc5_s16(ebx))) break;
            }
            edi2 = wc5_s16(edi2 - 1);
            var_20 = wc5_s16(var_20 + 1);
            ebx = wc5_s16(ebx + 1);
        }
    } else {
        const int var_c = (dxword != 0) ? 1 : wc5_s16(-1);
        int var_28 = wc5_s16(saved_eax + 1), var_4 = 0;
        while (wc5_s16(var_4) < wc5_s16(var_18)) {
            int si = wc5_s16(edi), ecx = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, saved_eax, wc5_s16(si));
                ecx = wc5_s16(ecx + 1);
                si = wc5_s16(si + var_c);
                if (!(wc5_s16(ecx) < wc5_s16(ebx))) break;
            }
            var_28 = wc5_s16(var_28 + 1);
            var_4 = wc5_s16(var_4 + 1);
            ebx = wc5_s16(ebx + 1);
        }
    }
}

// sub_1BD1E(eax0, edx0, f, ecx0, arg0). w0 is the entry eax.
void CombatII::battle_ai_wc5_m_bd1e(std::vector<int>& grid, Wc5Score& st,
                                    int eax0, int edx0, int f, int ecx0,
                                    int arg0) const {
    if (f < 0 || f > 3) return;
    int edi = wc5_s16(edx0);
    const int var_24 = wc5_s16(ecx0);
    const int cxword = wc5_s16(arg0);
    int w0 = wc5_s16(eax0);
    int ebx = 2;
    battle_ai_wc5_mark_cell(grid, st, wc5_s16(eax0), wc5_s16(edx0));
    if (f == 0) {
        const int var_c = (cxword != 0) ? wc5_s16(-1) : 1;
        int var_18 = 1;
        edi = wc5_s16(edi + 1);
        while (wc5_s16(var_18) < wc5_s16(var_24)) {
            int ecx = w0, si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(ecx), wc5_s16(edi));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + var_c);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            if (var_18 & 1)
                w0 = wc5_s16(w0 + var_c);
            else
                ebx = wc5_s16(ebx + 1);
            edi = wc5_s16(edi + 1);
            var_18 = wc5_s16(var_18 + 1);
        }
    } else if (f == 1) {
        const int var_10 = (cxword != 0) ? wc5_s16(-1) : 1;
        w0 = wc5_s16(w0 - 1);
        int var_14 = 1;
        while (wc5_s16(var_14) < wc5_s16(var_24)) {
            int si = wc5_s16(edi), ecx = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(w0), wc5_s16(si));
                ecx = wc5_s16(ecx + 1);
                si = wc5_s16(si + var_10);
                if (!(wc5_s16(ecx) < wc5_s16(ebx))) break;
            }
            w0 = wc5_s16(w0 - 1);
            if (var_14 & 1)
                edi = wc5_s16(edi + var_10);
            else
                ebx = wc5_s16(ebx + 1);
            var_14 = wc5_s16(var_14 + 1);
        }
    } else if (f == 2) {
        const int var_8 = (cxword != 0) ? 1 : wc5_s16(-1);
        int var_1c = 1;
        edi = wc5_s16(edi - 1);
        while (wc5_s16(var_1c) < wc5_s16(var_24)) {
            int ecx = w0, si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(ecx), wc5_s16(edi));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + var_8);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            if (var_1c & 1)
                w0 = wc5_s16(w0 + var_8);
            else
                ebx = wc5_s16(ebx + 1);
            edi = wc5_s16(edi - 1);
            var_1c = wc5_s16(var_1c + 1);
        }
    } else {
        const int var_4 = (cxword != 0) ? 1 : wc5_s16(-1);
        w0 = wc5_s16(w0 + 1);
        int var_20 = 1;
        while (wc5_s16(var_20) < wc5_s16(var_24)) {
            int ecx = wc5_s16(edi), si = 0;
            for (;;) {
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(w0), wc5_s16(ecx));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + var_4);
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
            }
            w0 = wc5_s16(w0 + 1);
            if (var_20 & 1)
                edi = wc5_s16(edi + var_4);
            else
                ebx = wc5_s16(ebx + 1);
            var_20 = wc5_s16(var_20 + 1);
        }
    }
}

// sub_1BF2E(eax0, edx0, f, ecx0, arg0). Draws one line at a time.
void CombatII::battle_ai_wc5_m_bf2e(std::vector<int>& grid, Wc5Score& st,
                                    int eax0, int edx0, int f, int ecx0,
                                    int arg0) const {
    if (f < 0 || f > 3) return;
    int edi = wc5_s16(eax0), esi = wc5_s16(edx0);
    const int dxword = wc5_s16(arg0);
    const int cx = wc5_s16(ecx0 - 1);
    int ebx = 0;
    if (f == 0) {
        const int var_4 = (dxword != 0) ? wc5_s16(-1) : 1;
        esi = wc5_s16(esi + 1);
        while (!(wc5_s16(ebx) >= wc5_s16(cx))) {
            battle_ai_wc5_mark_cell(grid, st, wc5_s16(edi), wc5_s16(esi));
            ebx = wc5_s16(ebx + 1);
            esi = wc5_s16(esi + 1);
            edi = wc5_s16(edi + var_4);
        }
    } else if (f == 1) {
        const int var_8 = (dxword != 0) ? wc5_s16(-1) : 1;
        edi = wc5_s16(edi - 1);
        while (!(wc5_s16(ebx) >= wc5_s16(cx))) {
            battle_ai_wc5_mark_cell(grid, st, wc5_s16(edi), wc5_s16(esi));
            ebx = wc5_s16(ebx + 1);
            edi = wc5_s16(edi - 1);
            esi = wc5_s16(esi + var_8);
        }
    } else if (f == 2) {
        const int var_10 = (dxword != 0) ? 1 : wc5_s16(-1);
        esi = wc5_s16(esi - 1);
        while (!(wc5_s16(ebx) >= wc5_s16(cx))) {
            battle_ai_wc5_mark_cell(grid, st, wc5_s16(edi), wc5_s16(esi));
            ebx = wc5_s16(ebx + 1);
            esi = wc5_s16(esi - 1);
            edi = wc5_s16(edi + var_10);
        }
    } else {
        const int var_c = (dxword != 0) ? 1 : wc5_s16(-1);
        edi = wc5_s16(edi + 1);
        while (!(wc5_s16(ebx) >= wc5_s16(cx))) {
            battle_ai_wc5_mark_cell(grid, st, wc5_s16(edi), wc5_s16(esi));
            ebx = wc5_s16(ebx + 1);
            edi = wc5_s16(edi + 1);
            esi = wc5_s16(esi + var_c);
        }
    }
}

// sub_1C1B2(eax0=adj_x, edx0=adj_y, f, ecx0=range). var_4=range-1 and var_18 is the
// pushed eax.
void CombatII::battle_ai_wc5_m_c1b2(std::vector<int>& grid, Wc5Score& st,
                                    int eax0, int edx0, int f,
                                    int ecx0) const {
    if (f < 0 || f > 3) return;
    int edi = wc5_s16(edx0);
    const int var_4 = wc5_s16(ecx0 - 1);
    int var_18 = wc5_s16(eax0);
    int ebx = 1;
    if (f == 0) {
        int si = 0;
        edi = wc5_s16(edi + ebx);
        int var_14 = si;
        for (;;) {
            if (!(wc5_s16(var_14) < wc5_s16(var_4))) return;
            int ecx = var_18;
            si = 0;
            for (;;) {
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(ecx), wc5_s16(edi));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + 1);
            }
            ecx = var_18;
            si = var_14;
            edi = wc5_s16(edi + 1);
            ebx = wc5_s16(ebx + 2);
            ecx = wc5_s16(ecx - 1);
            si = wc5_s16(si + 1);
            var_18 = wc5_s16(ecx);
            var_14 = si;
        }
    } else if (f == 1) {
        int ecx = wc5_s16(var_18 - ebx);
        int var_8 = 0;
        var_18 = wc5_s16(ecx);
        for (;;) {
            if (!(wc5_s16(var_8) < wc5_s16(var_4))) return;
            ecx = wc5_s16(edi);
            int si = 0;
            for (;;) {
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(eax0), wc5_s16(ecx));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + 1);
            }
            ecx = var_18;
            si = var_8;
            ebx = wc5_s16(ebx + 2);
            edi = wc5_s16(edi - 1);
            ecx = wc5_s16(ecx - 1);
            si = wc5_s16(si + 1);
            var_18 = wc5_s16(ecx);
            var_8 = si;
        }
    } else if (f == 2) {
        edi = wc5_s16(edi - ebx);
        int var_c = 0;
        for (;;) {
            if (!(wc5_s16(var_c) < wc5_s16(var_4))) return;
            int si = var_18, ecx = 0;
            for (;;) {
                if (!(wc5_s16(ecx) < wc5_s16(ebx))) break;
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(si), wc5_s16(edi));
                ecx = wc5_s16(ecx + 1);
                si = wc5_s16(si + 1);
            }
            ecx = var_18;
            si = var_c;
            edi = wc5_s16(edi - 1);
            ebx = wc5_s16(ebx + 2);
            ecx = wc5_s16(ecx - 1);
            si = wc5_s16(si + 1);
            var_18 = wc5_s16(ecx);
            var_c = si;
        }
    } else {
        int si = wc5_s16(var_18 + ebx);
        int var_10 = 0;
        var_18 = wc5_s16(si);
        for (;;) {
            if (!(wc5_s16(var_10) < wc5_s16(var_4))) return;
            int ecx = wc5_s16(edi);
            si = 0;
            for (;;) {
                if (!(wc5_s16(si) < wc5_s16(ebx))) break;
                battle_ai_wc5_mark_cell(grid, st, wc5_s16(eax0), wc5_s16(ecx));
                si = wc5_s16(si + 1);
                ecx = wc5_s16(ecx + 1);
            }
            ecx = var_18;
            si = var_10;
            ebx = wc5_s16(ebx + 2);
            edi = wc5_s16(edi - 1);
            ecx = wc5_s16(ecx + 1);
            si = wc5_s16(si + 1);
            var_18 = wc5_s16(ecx);
            var_10 = si;
        }
    }
}

// sub_1C31A(eax0, edx0, f, ecx0): facing-area cleanup that rewrites E8F46==-5 to -4.
void CombatII::battle_ai_wc5_m_c31a(std::vector<int>& grid, int eax0, int edx0,
                                    int f, int ecx0) const {
    if (f < 0 || f > 3) return;
    const int var_54 = wc5_s16(ecx0 - 1);
    const int ebp = -4;
    int di = 3;
    if (f == 0) {
        int var_4 = wc5_s16(eax0 - 1), var_50 = wc5_s16(edx0 + 1), var_38 = 0;
        while (wc5_s16(var_38) < wc5_s16(var_54)) {
            int var_28 = var_4, var_14 = 0;
            while (wc5_s16(di) > wc5_s16(var_14)) {
                if (battle_ai_wc5_diff_read(grid, wc5_s16(var_28),
                                            wc5_s16(var_50)) == -5)
                    battle_ai_wc5_diff_write(grid, wc5_s16(var_28),
                                             wc5_s16(var_50), ebp);
                var_28 = wc5_s16(var_28 + 1);
                var_14 = wc5_s16(var_14 + 1);
            }
            var_50 = wc5_s16(var_50 + 1);
            if (var_38 & 1) {
                var_4 = wc5_s16(var_4 - 1);
                di = wc5_s16(di + 2);
            }
            var_38 = wc5_s16(var_38 + 1);
        }
    } else if (f == 1) {
        int var_8 = wc5_s16(edx0 - 1), var_3c = wc5_s16(eax0 - 1), var_44 = 0;
        int b3c = 0;
        while (wc5_s16(var_44) < wc5_s16(var_54)) {
            b3c = var_8;
            int var_20 = 0;
            while (wc5_s16(di) > wc5_s16(var_20)) {
                if (battle_ai_wc5_diff_read(grid, wc5_s16(var_3c),
                                            wc5_s16(b3c)) == -5)
                    battle_ai_wc5_diff_write(grid, wc5_s16(var_3c),
                                             wc5_s16(b3c), ebp);
                b3c = wc5_s16(b3c + 1);
                var_20 = wc5_s16(var_20 + 1);
            }
            var_3c = wc5_s16(var_3c - 1);
            if (var_44 & 1) {
                var_8 = wc5_s16(var_8 - 1);
                di = wc5_s16(di + 2);
            }
            var_44 = wc5_s16(var_44 + 1);
        }
    } else if (f == 2) {
        int var_48 = wc5_s16(eax0 - 1), var_c = wc5_s16(edx0 - 1), var_40 = 0;
        while (wc5_s16(var_40) < wc5_s16(var_54)) {
            int var_1c = var_48, var_30 = 0;
            while (wc5_s16(di) > wc5_s16(var_30)) {
                if (battle_ai_wc5_diff_read(grid, wc5_s16(var_1c),
                                            wc5_s16(var_30)) == -5)
                    battle_ai_wc5_diff_write(grid, wc5_s16(var_1c),
                                             wc5_s16(var_30), ebp);
                var_1c = wc5_s16(var_1c + 1);
                var_30 = wc5_s16(var_30 + 1);
            }
            var_c = wc5_s16(var_c - 1);
            if (var_40 & 1) {
                var_48 = wc5_s16(var_48 - 1);
                di = wc5_s16(di + 2);
            }
            var_40 = wc5_s16(var_40 + 1);
        }
    } else {
        int var_34 = wc5_s16(edx0 - 1), var_4c = wc5_s16(ecx0 + 1), var_10 = 0;
        while (wc5_s16(var_10) < wc5_s16(var_54)) {
            int var_28 = var_34, var_24 = 0;
            while (wc5_s16(di) > wc5_s16(var_24)) {
                if (battle_ai_wc5_diff_read(grid, wc5_s16(var_4c),
                                            wc5_s16(var_24)) == -5)
                    battle_ai_wc5_diff_write(grid, wc5_s16(var_4c),
                                             wc5_s16(var_24), ebp);
                var_28 = wc5_s16(var_28 + 1);
                var_24 = wc5_s16(var_24 + 1);
            }
            var_4c = wc5_s16(var_4c + 1);
            if (var_10 & 1) {
                var_34 = wc5_s16(var_34 - 1);
                di = wc5_s16(di + 2);
            }
            var_10 = wc5_s16(var_10 + 1);
        }
    }
}

// WC5 1x1 165C2 gate: no occupant or self, and spread>=0.
// Fails outside the sub_11DB2 padded edge (x in [2,W-3], y in [4,H-5]). Large footprints
// and the tactical pattern stride/data (572/143) are used only inside the class-0 1x1
// ordinary gate, so a single cell is examined.
bool CombatII::battle_ai_wc5_165c2_gate(const std::vector<int>& grid, int nx,
                                        int ny, int unit_index) const {
    if (map_w_ == 0 || map_h_ == 0) return false;
    const uint32_t xu = (uint32_t)(nx & 0xFFFF), yu = (uint32_t)(ny & 0xFFFF);
    if (((xu - 2) & 0xFFFFu) >= map_w_ - 4) return false;
    if (((yu - 4) & 0xFFFFu) >= map_h_ - 8) return false;
    if (nx < 0 || ny < 0 || nx >= (int)map_w_ || ny >= (int)map_h_) return false;
    const size_t idx = (size_t)yu * map_w_ + xu;
    if (idx >= grid.size() || grid[idx] < 0) return false;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    int foot_w = 1, foot_h = 1;
    const auto& actor = units_[(size_t)unit_index];
    battle_d66a8_footprint(actor.sprite_type, (int)actor.facing & 3, &foot_w, &foot_h);
    if (foot_w < 1) foot_w = 1;
    if (foot_h < 1) foot_h = 1;
    for (int dy = 0; dy < foot_h; dy++) {
        const int cy = ny - (foot_h - 1) + dy;
        for (int dx = 0; dx < foot_w; dx++) {
            const int cx = nx + dx;
            if (cx < 2 || cx >= (int)map_w_ - 2 || cy < 4 || cy >= (int)map_h_ - 4) return false;
            const size_t cidx = (size_t)cy * map_w_ + (size_t)cx;
            if (cidx >= grid.size() || grid[cidx] < 0) return false;
            const int occupant = battle_unit_index_at_footprint(cx, cy);
            if (occupant >= 0 && occupant != unit_index) return false;
        }
    }
    return true;
}

// sub_1C685 body. Takes the adjacent tile, facing and range, runs the flood, then
// updates F0B84 (destination and score) in st. F0B8E==2 (E8 score) and ==3 (the
// sub_3D7B2 distance path) are supported.
void CombatII::battle_ai_wc5_1c685(std::vector<int>& grid, Wc5Iter& it,
                                    Wc5Score& st, int adj_x, int adj_y, int f,
                                    int rng, int unit_index) const {
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return;
    static constexpr int kStepX[4] = {0, -1, 0, 1};  // S,W,N,E
    static constexpr int kStepY[4] = {1, 0, -1, 0};
    // Head: bucket of the tile one step along (f+2)&3.
    int sx = adj_x, sy = adj_y;
    {
        const int b = (f + 2) & 3;
        sx = wc5_s16(sx + kStepX[b]);
        sy = wc5_s16(sy + kStepY[b]);
    }
    uint8_t raw = 0;
    if (sx >= 0 && sy >= 0 && sx < (int)map_w_ && sy < (int)map_h_)
        raw = tile_attr_[(size_t)sy * map_w_ + (size_t)sx];
    uint8_t ah = raw >= 0x64 ? (uint8_t)(raw - 0x64) : raw;
    const uint8_t bucket = (uint8_t)(ah & (uint8_t)~0x0Fu);
    const uint8_t var_10 = bucket, var_8 = (uint8_t)(bucket + 8);
    const uint8_t var_c = (uint8_t)(bucket + 0x10), var_4 = (uint8_t)(bucket + 0x18);
    // Iterator 1: (adj_x, adj_y, cx=1, f, stack=rng).
    battle_ai_wc5_iter_init(it, adj_x, adj_y, 1, f, rng);
    int vx = adj_x, vy = adj_y;
    {
        int px = 0, py = 0;
        if (battle_ai_wc5_iter_next(it, px, py)) {
            vx = px;
            vy = py;
        }
        while (battle_ai_wc5_iter_next(it, px, py)) {
            vx = px;
            vy = py;
            uint8_t r = 0;
            if (vx >= 0 && vy >= 0 && vx < (int)map_w_ && vy < (int)map_h_)
                r = tile_attr_[(size_t)vy * map_w_ + (size_t)vx];
            uint8_t al = r >= 0x64 ? (uint8_t)(r - 0x64) : r;
            if (al == 0 || al == var_4) {
                battle_ai_wc5_diff_write(grid, vx, vy, -5);
                continue;
            }
            if (al > var_c) {
                battle_ai_wc5_flood_neg(grid, vx, vy, 0x3A98);
                continue;
            }
            if (al > var_10) {
                battle_ai_wc5_flood_neg(grid, vx, vy, 0x0A);
                continue;
            }
            if (st.b8e == 3) battle_ai_wc5_diff_write(grid, vx, vy, -1);
        }
    }
    // Post pass: rect cleanup, cursor reset, adj test, 1C1B2, then adj = -2.
    battle_ai_wc5_m_c31a(grid, adj_x, adj_y, f, rng);
    int cx0 = adj_x, cy0 = adj_y;
    {
        const int b = (f + 1) & 3;
        cx0 = wc5_s16(cx0 + kStepX[b]);
        cy0 = wc5_s16(cy0 + kStepY[b]);
    }
    battle_ai_wc5_iter_init(it, cx0, cy0, 3, f, rng);
    uint8_t araw = 0;
    if (adj_x >= 0 && adj_y >= 0 && adj_x < (int)map_w_ && adj_y < (int)map_h_)
        araw = tile_attr_[(size_t)adj_y * map_w_ + (size_t)adj_x];
    const uint8_t aah = araw >= 0x64 ? (uint8_t)(araw - 0x64) : araw;
    if (aah == 0 || aah > var_c)
        battle_ai_wc5_m_c1b2(grid, st, adj_x, adj_y, f, rng);
    battle_ai_wc5_diff_write(grid, adj_x, adj_y, -2);
    // Main loop.
    for (;;) {
        int px = 0, py = 0;
        if (!battle_ai_wc5_iter_next(it, px, py)) break;
        const int x = px, y = py;
        uint8_t brav = 0;
        if (x >= 0 && y >= 0 && x < (int)map_w_ && y < (int)map_h_)
            brav = tile_attr_[(size_t)y * map_w_ + (size_t)x];
        const uint8_t bl = brav >= 0x64 ? (uint8_t)(brav - 0x64) : brav;
        int si = battle_ai_wc5_diff_read(grid, x, y);
        if (si == -2) continue;
        if (bl == 0 || (bl & 0x0Fu) == 8) {
            if (bl == var_4)
                st.b8a = 2;
            else if (bl == var_8)
                st.b8a = 3;
            else
                st.b8a = 1;
            int rows = wc5_s16(it.c_hi - it.n_hi);
            if (wc5_s16(it.n_lo) == 0) rows = wc5_s16(rows + 1);
            rows = wc5_s16(rows);
            if ((f & 1) != 0) {
                if (y == adj_y) {
                    battle_ai_wc5_m_c046(grid, st, x, y, f, rows);
                    continue;
                }
                int dl = 0;
                if (f == 1)
                    dl = (y < adj_y) ? 1 : 0;
                else
                    dl = (adj_y < y) ? 1 : 0;
                if (wc5_s16(it.n_lo) <= 1)
                    battle_ai_wc5_m_bf2e(grid, st, x, y, f, rows, dl);
                else if (si == -4)
                    battle_ai_wc5_m_bb62(grid, st, x, y, f, rows, dl);
                else
                    battle_ai_wc5_m_bd1e(grid, st, x, y, f, rows, dl);
                continue;
            }
            if (adj_x == x) {
                battle_ai_wc5_m_c046(grid, st, x, y, f, rows);
                continue;
            }
            int dl = 0;
            if (f == 0)
                dl = (adj_x <= x) ? 0 : 1;
            else
                dl = (adj_x < x) ? 1 : 0;
            if (wc5_s16(it.n_lo) <= 1)
                battle_ai_wc5_m_bf2e(grid, st, x, y, f, rows, dl);
            else if (si == -4)
                battle_ai_wc5_m_bb62(grid, st, x, y, f, rows, dl);
            else
                battle_ai_wc5_m_bd1e(grid, st, x, y, f, rows, dl);
            continue;
        }
        if (wc5_s16(it.n_lo) <= 1) continue;
        if (st.b8e != 2) {
            // loc_1C90A (sub_3D7B2 wc5, F0B8E==3): strict minimum sub_49ADF distance
            // from the actor, then the sub_16699 1x1 gate. F0B84_hi carries the best
            // distance across the four facings.
            const auto& actor = units_[(size_t)unit_index];
            const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
            const int dist = std::abs(ax - x) + std::abs(ay - y);
            if (dist >= wc5_s16(st.b84s)) continue;
            const int mobility = battle_ai_mobility_selector(actor);
            if (!battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility)) continue;
            st.b84x = wc5_s16(x);
            st.b84s = wc5_s16(dist);
            st.b82 = wc5_s16(y);
            continue;
        }
        if (si < -15028) {
            si = wc5_s16(-(si + 0x3A98));
            battle_ai_wc5_diff_write(grid, x, y, si);
        } else if (si < -5) {
            si = wc5_s16(-(si + 0x0A));
            battle_ai_wc5_diff_write(grid, x, y, si);
        }
        if (si <= wc5_s16(st.b84s)) continue;
        if (!battle_ai_wc5_165c2_gate(grid, x, y, unit_index)) continue;
        st.b84x = wc5_s16(x);
        st.b84s = wc5_s16(si);
        st.b82 = wc5_s16(y);
    }
}

// sub_3CA3C WC5 arm: facing loop plus epilogue for one hostile.
// Returns the score (-1 when no candidate) and the destination/attack facing.
int CombatII::battle_ai_wc5_hostile_score(std::vector<int>& grid, Wc5Iter& it,
                                           Wc5Score& st, int unit_index,
                                           int hx, int hy, int& out_dx,
                                           int& out_dy,
                                           int& out_attack_facing) const {
    out_dx = out_dy = out_attack_facing = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int cls = (int)actor.combat_class;
    const int rng = battle_ai_wc5_aef3(cls);
    // Head refresh writes EDD72+1 into the actor footprint; EDD72 is the caller's
    // budget and the footprint is 1x1, so only the actor cell changes.
    int di = -1, best_f = -1, best_x = -1, best_y = -1;
    static constexpr int kAdjX[4] = {0, -1, 0, 1};  // S,W,N,E
    static constexpr int kAdjY[4] = {1, 0, -1, 0};
    for (int f = 0; f < 4; ++f) {
        const int ax = hx + kAdjX[f], ay = hy + kAdjY[f];
        battle_ai_wc5_snapshot(grid, it, st, ax, ay, 3, f, rng + 1);
        if (f == 0) battle_ai_wc5_state_init(st, -1, unit_index);
        battle_ai_wc5_1c685(grid, it, st, ax, ay, f, rng, unit_index);
        battle_ai_wc5_restore(grid, it, st, ax, ay, 3, f, rng + 1);
        if (wc5_s16(di) < wc5_s16(st.b84s)) {
            di = wc5_s16(st.b84s);
            best_f = f;
            best_x = wc5_s16(st.b84x);
            best_y = wc5_s16(st.b82);
        }
    }
    if (di == -1) return -1;
    const int attack_facing = (best_f + 2) % 4;
    const int actor_x = (int)(int8_t)actor.x, actor_y = (int)(int8_t)actor.y;
    const int dist = abs(actor_x - hx) + abs(actor_y - hy);
    int score = di + (0x5A - dist);  // wc==5 arm bonus
    const int actor_facing = (int)(int8_t)actor.facing;
    if (actor_facing >= 0 && actor_facing <= 3 && actor_facing == attack_facing)
        score += 4;
    out_dx = best_x;
    out_dy = best_y;
    out_attack_facing = attack_facing;
    return score;
}

// sub_3D7B2 loc_3D9D7: WC5 distance-mode arm. 1CB53 snapshot and 1CBB1 restore bracket
// the 1C685 flood for each facing; 1B7D4 sets F0B8E=3 (distance) and keeps F0B84_hi
// across facings, resetting it to 7FFF only on facing 0. The epilogue adds
// 0x5A - sub_49ADF(actor, hostile) when sub_1AEF3 reports 5.
int CombatII::battle_ai_wc5_3d7b2(int unit_index, int hx, int hy,
                                   const std::vector<int>& e8,
                                   int& out_x, int& out_y) const {
    out_x = out_y = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0x7FFF;
    if (map_w_ == 0 || map_h_ == 0) return 0x7FFF;
    if (e8.size() != (size_t)map_w_ * (size_t)map_h_) return 0x7FFF;
    const auto& actor = units_[(size_t)unit_index];
    const int rng = battle_ai_wc5_aef3((int)actor.combat_class);
    static constexpr int kAdjX[4] = { 0, -1, 0, 1 };
    static constexpr int kAdjY[4] = { 1, 0, -1, 0 };
    std::vector<int> grid = e8;
    Wc5Iter it;
    Wc5Score st;
    st.snap.assign(256, 0);
    battle_ai_wc5_state_init(st, 0x7FFF, unit_index);
    st.b8e = 3;  // sub_1B7D4(actor, 7FFFh, 3) on facing 0
    int best = 0x7FFF, bx = -1, by = -1;
    for (int f = 0; f < 4; ++f) {
        const int ax = hx + kAdjX[f], ay = hy + kAdjY[f];
        battle_ai_wc5_snapshot(grid, it, st, ax, ay, 3, f, rng + 1);
        battle_ai_wc5_1c685(grid, it, st, ax, ay, f, rng, unit_index);
        battle_ai_wc5_restore(grid, it, st, ax, ay, 3, f, rng + 1);
        const int cand = wc5_s16(st.b84s);
        if (cand < best) {
            best = cand;
            bx = wc5_s16(st.b84x);
            by = wc5_s16(st.b82);
        }
    }
    if (best == 0x7FFF) return 0x7FFF;
    if (rng == 5) {
        const int actor_x = (int)(int8_t)actor.x, actor_y = (int)(int8_t)actor.y;
        best += 0x5A - (std::abs(actor_x - hx) + std::abs(actor_y - hy));
    }
    out_x = bx; out_y = by;
    return best;
}

// sub_3E65E: pick the hostile with the strict maximum score (first wins ties).
// Every hostile-occupied cell inside the radius diamond (sub_1AC08/sub_1AC59) is
// scored through sub_3CA3C, so the caller receives dest/facing from the winner.
// Not WC5-specific: sub_3CA3C dispatches the per-wc arms internally
// (loc_3CB73 / loc_3CC3A / loc_3CC91 / loc_3CE1E..loc_3D2E3), so every wc on the
// sub_40DFD k=0/1 path shares this selection. The WC5 loc_3CC91 chain returns the
// same score as battle_ai_wc5_hostile_score.
bool CombatII::battle_ai_3e65e_select(int unit_index, int budget,
                                       int radius, int& out_hostile,
                                       int& out_dx, int& out_dy,
                                       int& out_attack_facing,
                                       int& out_score) const {
    out_hostile = -1;
    out_dx = out_dy = out_attack_facing = -1;
    out_score = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    if (!actor.active) return false;
    const int wc = weapon_class(actor);
    const int actor_x = (int)(int8_t)actor.x, actor_y = (int)(int8_t)actor.y;
    std::vector<int> grid = battle_ai_build_diffusion(unit_index, budget);
    if (grid.empty()) return false;
    Wc5Dia dia;
    battle_ai_wc5_dia_init(dia, actor_x, actor_y, radius);
    int best_score = -1;
    int dx = 0, dy = 0;
    while (battle_ai_wc5_dia_next(dia, dx, dy)) {
        if (dx < 0 || dy < 0 || dx >= (int)map_w_ || dy >= (int)map_h_) continue;
        if (dx == actor_x && dy == actor_y) continue;
        // sub_11DB2 padded edge + occupant.
        const uint32_t xu = (uint32_t)(dx & 0xFFFF), yu = (uint32_t)(dy & 0xFFFF);
        if (((xu - 2) & 0xFFFFu) >= map_w_ - 4) continue;
        if (((yu - 4) & 0xFFFFu) >= map_h_ - 8) continue;
        const int occupant = battle_unit_index_at(dx, dy);
        if (occupant < 0 || occupant == unit_index) continue;
        const auto& other = units_[(size_t)occupant];
        if (!other.active || other.hp <= 0) continue;
        if (!battle_armies_hostile(battle_effective_army(other),
                                   battle_effective_army(actor),
                                   battle_hostility_inverted_))
            continue;
        int hx = dx, hy = dy, adx = -1, ady = -1, af = -1;
        // sub_3CA3C refreshes the actor footprint with budget+1 on entry.
        const int score = battle_ai_3ca3c_cell(unit_index, wc, hx, hy, grid,
                                               budget, adx, ady, af);
        if (score <= best_score) continue;  // strict max, first wins ties
        best_score = score;
        out_hostile = occupant;
        out_dx = adx;
        out_dy = ady;
        out_attack_facing = af;
    }
    out_score = best_score;
    return out_hostile >= 0;
}

// Hostile-occupancy filter shared by sub_3E65E/sub_3DF3B: sub_11DB2 must find an
// occupant and sub_1201D must report hostility. Neither checks the [+14h] flight
// gate, so the port does not either.
// sub_11DB2 rejects the padded edge first: (x-2) < W-4 and (y-4) < H-8, which on a
// positive map means x in [2, W-3] and y in [4, H-5], so hostiles on edge tiles are
// ignored. The self tile is not excluded: sub_1201D is hostility xor byte_E518D, so
// with hostility inverted the actor counts as its own enemy, and the diamond scan
// includes the centre cell while the actor footprint is still present. Without the
// inversion the hostile test is false for self, which is equivalent to skipping it.
bool CombatII::battle_ai_hostile_occupied(int x, int y, int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    if (x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4) return false;
    const int occupant = battle_unit_index_at(x, y);
    if (occupant < 0) return false;
    const auto& self = units_[(size_t)unit_index];
    const auto& other = units_[(size_t)occupant];
    return battle_armies_hostile(battle_effective_army(other), battle_effective_army(self),
                                 battle_hostility_inverted_);
}

// BFS neighbour order: larger remaining gap first, x axis (W/E) on ties, sign
// toward the destination. This is the only simple rule that reproduces both DOS
// 1-5 cases - id3 (61,50)->(56,47) stopping at (58,49) and id4 (61,52)->(56,55)
// stopping at (58,53). The original sub_15523 may use a different si order or
// mask, so treat this as a hypothesis. BFS stays shortest regardless of order,
// so reachability is unaffected.
static void ai_bfs_dir_order(int cx, int cy, int dest_x, int dest_y,
                             int (&ox)[4], int (&oy)[4]) {
    const int rx = dest_x - cx, ry = dest_y - cy;
    const int sx = (rx < 0) ? -1 : 1;
    const int sy = (ry < 0) ? -1 : 1;
    if (std::abs(rx) >= std::abs(ry)) {
        ox[0] = sx; oy[0] = 0;
        ox[1] = 0;  oy[1] = sy;
        ox[2] = -sx; oy[2] = 0;
        ox[3] = 0;  oy[3] = -sy;
    } else {
        ox[0] = 0;  oy[0] = sy;
        ox[1] = sx; oy[1] = 0;
        ox[2] = 0;  oy[2] = -sy;
        ox[3] = -sx; oy[3] = 0;
    }
}

// Shortest legal 4-direction path (BFS) with the same rules as the home search:
// no radius limit, adaptive neighbour order, a 64-step cap, and sub_16699 legality
// (1x1, cost, other units avoided). out_steps excludes the start and ends at the
// destination.
bool CombatII::battle_ai_find_legal_path(int unit_index, int mobility, int dest_x, int dest_y,
                                         std::vector<std::pair<int,int>>& out_steps) const {
    out_steps.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    const int actor_x = (int)(int8_t)units_[(size_t)unit_index].x;
    const int actor_y = (int)(int8_t)units_[(size_t)unit_index].y;
    if (actor_x == dest_x && actor_y == dest_y) return false;
    const int cells = map_w * map_h;
    const int start_idx = actor_y * map_w + actor_x;
    const int dest_idx = dest_y * map_w + dest_x;
    if (start_idx < 0 || start_idx >= cells || dest_idx < 0 || dest_idx >= cells) return false;
    std::vector<int> prev(cells, -1);
    std::vector<int> queue;
    queue.reserve(256);
    prev[start_idx] = start_idx;
    queue.push_back(start_idx);
    size_t head = 0;
    bool reached = false;
    while (!reached && head < queue.size()) {
        const int cur = queue[head++];
        const int cx = cur % map_w, cy = cur / map_w;
        int dir_x[4], dir_y[4];
        ai_bfs_dir_order(cx, cy, dest_x, dest_y, dir_x, dir_y);
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + dir_x[d], ny = cy + dir_y[d];
            if (nx < 0 || ny < 0 || nx >= map_w || ny >= map_h) continue;
            const int nidx = ny * map_w + nx;
            if (prev[nidx] != -1) continue;
            if (!battle_ai_candidate_tile_legal_1x1(nx, ny, unit_index, mobility)) continue;
            prev[nidx] = cur;
            if (nidx == dest_idx) { reached = true; break; }
            queue.push_back(nidx);
        }
    }
    if (!reached) return false;
    for (int cur = dest_idx; cur != start_idx; cur = prev[cur]) {
        if (cur < 0 || cur >= cells || prev[cur] == -1) { out_steps.clear(); return false; }
        out_steps.push_back({ cur % map_w, cur / map_w });
        if ((int)out_steps.size() > 64) { out_steps.clear(); return false; }
    }
    std::reverse(out_steps.begin(), out_steps.end());
    return !out_steps.empty();
}

// Approximates the sub_40295 -> sub_14FA4 movement budget (dasm 65070..65078 passes
// ECX = var_18*4; word_EDD72 = 0xD2 is the separate sub_14300/sub_1419C/sub_1499C
// spread cap). var_18 = trunc((shared_gauge - 10) / 10) and is always >= 4 because of
// the 0x37 lower bound. Tile cost is the signed mobility-grid value. full_path[0] is
// the current tile; out_walk keeps the prefix whose cumulative cost fits the budget.
bool CombatII::battle_ai_truncate_to_move_budget(const std::vector<std::pair<int,int>>& full_path,
                                                 int mobility, int* out_cost,
                                                 std::vector<std::pair<int,int>>& out_walk) const {
    out_walk.clear();
    if (out_cost) *out_cost = 0;
    if (full_path.size() < 2 || mobility < 0 || mobility >= kMobilityGridCount) return false;
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return false;
    int var18 = ((int)battle_ai_gauge_baseline_ - battle_ai_var4_) / 10; // idiv trunc toward zero
    if (var18 < 0) var18 = 0;
    const int budget = (battle_ai_budget_override_ >= 0) ? battle_ai_budget_override_ : var18 * 4;
    out_walk.push_back(full_path[0]);
    int cum = 0;
    for (size_t i = 1; i < full_path.size(); ++i) {
        const int x = full_path[i].first, y = full_path[i].second;
        if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) break;
        const size_t ti = (size_t)y * map_w_ + (size_t)x;
        if (ti >= tile_attr_.size()) break;
        const int attr = (int)tile_attr_[ti];
        if (attr < 0 || attr >= kMobilityGridStride) break;
        const int step_cost = (int)(int8_t)kMobilityCostGrids[mobility][attr];
        if (step_cost <= 0) break; // legal cells always pass; stop defensively anyway
        if (cum + step_cost > budget) break;
        cum += step_cost;
        out_walk.push_back(full_path[i]);
    }
    if (out_cost) *out_cost = cum;
    return out_walk.size() >= 2;
}

// sub_40295 [+6Ch]==0 arm: search hostiles within the budget radius (sub_3E65E) and
// approach the strict maximum with WC1 sub_3CA3C; otherwise scan radius 30
// (sub_3DF3B) and take the WC1 sub_3D7B2 minimum-distance approach.
// The budget-radius scan runs first regardless of the watch_state sign. With no
// hostile in budget the second-sub_3E18B branch decides: state > 0 is handled by the
// home path outside this function, state == 0 alone runs the radius-30 approach, and
// state == -1 ends the turn at loc_40664 without moving.
// Budget-radius hostiles use the decision diffusion (sub_14032/sub_143F3, budget
// var_18*4) and the WC1 sub_3CA3C inner arm (facing order, bucket, E8F46 max,
// sub_165C2). Radius-30 hostiles use the sub_3DF3B outer arm plus WC1 sub_3D7B2.
// Paths come from sub_15523 greedy ascent truncated by sub_14FA4, with BFS only as a
// fallback when the route fails.
void CombatII::battle_ai_wc1_pursuit(int unit_index, int watch_state) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const int actor_id = units_[(size_t)unit_index].id;
    const int actor_x = (int)(int8_t)units_[(size_t)unit_index].x;
    const int actor_y = (int)(int8_t)units_[(size_t)unit_index].y;
    const int mobility = battle_ai_mobility_selector(units_[(size_t)unit_index]);
    // var_18 = trunc((shared_gauge - 10) / 10); the 0x37 lower bound keeps it >= 4.
    int budget = ((int)battle_ai_gauge_baseline_ - battle_ai_var4_) / 10;
    if (budget < 0) budget = 0;
    bool hostile_in_budget = false;
    for (int dy = -budget; dy <= budget && !hostile_in_budget; ++dy) {
        const int half = budget - (dy < 0 ? -dy : dy);
        for (int dx = -half; dx <= half; ++dx) {
            // Centre cell included: hostile_occupied reflects the sub_1201D xor inversion.
            if (battle_ai_hostile_occupied(actor_x + dx, actor_y + dy, unit_index)) {
                hostile_in_budget = true;
                break;
            }
        }
    }
    if (hostile_in_budget) {
        // sub_3E65E arm: decision diffusion, then approach the sub_3CA3C best score.
        // The diffusion budget is var_18*4, as sub_40295 passes it in ECX.
        const std::vector<int> diffusion = battle_ai_build_diffusion(unit_index, budget * 4);
        int hx = -1, hy = -1, dx = -1, dy = -1, score = -1, attack_facing = -1;
        if (!diffusion.empty() &&
            battle_ai_3ca3c_wc1_best(unit_index, diffusion, budget, hx, hy, dx, dy, score,
                                     attack_facing)) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d WC1 budget hostile=(%d,%d) dest=(%d,%d) score=%d facing=%d (sub_3E65E + WC1 sub_3CA3C hostile-bucket/max/165C2/(f+2)%%4/+4; restore/tactics unported)\n",
                        actor_id, hx, hy, dx, dy, score, attack_facing);
            // sub_40295 loc_407BF tail: walk, sub_5401B/sub_17310, settle sub_3C993, sub_167C9.
            if (dx == actor_x && dy == actor_y && attack_facing == (int)units_[(size_t)unit_index].facing) {
                // loc_40589: dest == current cell and facing == [63h] -> di = -2, no move or
                // settle.
                return;
            }
            battle_ai_wc1_branch_walk(unit_index, dx, dy, attack_facing, budget * 4,
                                      battle_ai_var4_, hx, hy);
        } else if (battle_trace_) {
            fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d hostile within budget radius %d but no WC1 sub_3CA3C scored tile, no movement (pos=(%d,%d))\n",
                    actor_id, budget, actor_x, actor_y);
        }
        return;
    }
    // Second-sub_3E18B branch (loc_4062D/loc_40664): only split on state when the budget
    // radius held no hostile. State == -1 ends the turn, so no radius-30 scan happens.
    if (watch_state != 0) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d watch_state=%d (-1: second-sub_3E18B loc_40664 ends turn), no movement (pos=(%d,%d))\n",
                    actor_id, watch_state, actor_x, actor_y);
        return;
    }
    // Second-sub_3E18B state == 0 -> radius-30 approach (sub_3DF3B outer, WC1 sub_3D7B2).
    battle_ai_wc1_radius30_pursuit(unit_index);
}

// Radius-30 approach taken when second-sub_3E18B (loc_4062D/loc_40664) reports state 0.
// sub_3DF3B scans the radius-30 diamond in y-then-x order and keeps the hostile whose
// sub_3D7B2 destination has the strict minimum sub_49ADF distance. The walk then reuses
// the shared sub_1419C/1499C/15342/15523/14FA4 route pipeline.
void CombatII::battle_ai_wc1_radius30_pursuit(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (map_w_ == 0 || map_h_ == 0) return;
    const int actor_id = units_[(size_t)unit_index].id;
    const int actor_x = (int)(int8_t)units_[(size_t)unit_index].x;
    const int actor_y = (int)(int8_t)units_[(size_t)unit_index].y;
    int dest_x = -1, dest_y = -1, hostile_x = -1, hostile_y = -1;
    if (!battle_ai_3df3b_dest(unit_index, 30, dest_x, dest_y, hostile_x, hostile_y)) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d no hostile within radius 30, no movement (pos=(%d,%d))\n",
                    actor_id, actor_x, actor_y);
        return;
    }
    if (dest_x == actor_x && dest_y == actor_y) return; // already on the best approach cell
    const int score = std::abs(actor_x - dest_x) + std::abs(actor_y - dest_y);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d WC1 pursuit hostile=(%d,%d) dest=(%d,%d) score=%d (sub_3DF3B radius-30 + sub_3D7B2 wc1)\n",
                actor_id, hostile_x, hostile_y, dest_x, dest_y, score);
    int q = ((int)battle_ai_gauge_baseline_ - battle_ai_var4_) / 10;
    if (q < 0) q = 0;
    battle_ai_wc1_branch_walk(unit_index, dest_x, dest_y, -1, q * 4, battle_ai_var4_,
                              hostile_x, hostile_y, true);
}

// Returns the units_ index of the occupant at (x,y) when it is hostile to the actor,
// else -1. Approximation of sub_11DB2 + sub_1201D with the same guard and test as
// battle_ai_hostile_occupied. The self tile is tested too: sub_1201D applies to the
// actor itself, so it becomes hostile when byte_E518D inverts hostility.
int CombatII::battle_ai_hostile_unit_at(int x, int y, int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (map_w_ == 0 || map_h_ == 0) return -1;
    if (x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4) return -1;
    const int occupant = battle_unit_index_at(x, y);
    if (occupant < 0) return -1;
    const auto& self = units_[(size_t)unit_index];
    const auto& other = units_[(size_t)occupant];
    if (!battle_armies_hostile(battle_effective_army(other), battle_effective_army(self),
                               battle_hostility_inverted_))
        return -1;
    return occupant;
}

// 1x1 ordinary approximation of sub_165C2.
// The original expands the D66A8 record (per-code/per-mode footprint) and tests each
// cell with sub_11DB2 (unoccupied or self) and sub_13CBD (E8F46 >= 0). The port calls
// this only from the WC1 detail path, where the class-0 .NC2 1x1 gate has been proven,
// so one cell is enough. sub_11DB2's padding guard (x in [2,w-3], y in [4,h-5]) means
// "nothing occupies a cell outside the padding", so those cells still pass when
// E8F46 >= 0.
bool CombatII::battle_ai_165c2_1x1_ok(int x, int y, int unit_index,
                                      const std::vector<int>& e8) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    // sub_13CBD: out of range yields -128, which the E8 >= 0 test rejects.
    if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) return false;
    const size_t cell = (size_t)y * (size_t)map_w_ + (size_t)x;
    if (cell >= e8.size()) return false;
    if (e8[cell] < 0) return false;
    if (x >= 2 && x < (int)map_w_ - 2 && y >= 4 && y < (int)map_h_ - 4) {
        const int occupant = battle_unit_index_at(x, y);
        if (occupant >= 0 && occupant != unit_index) return false;
    }
    return true;
}

// Equivalent of the sub_1AC08/sub_1AC59 diamond enumeration: yields (x, y, ad2e) in
// ascending y (from centre-r) and ascending x within a row. Row width follows the
// standard diamond (1,3,5,..,5,3,1) and ad2e is the sub_1AD2E value: for a row of width
// w = 2h+1, column i (0-based) gives min(i+1, w-i-2), and the last cell of a row is 0
// because the original advances the row and returns 0 with dx = 0 (that value is unused).
template <typename Fn>
static void ai_diamond_each(int cx, int cy, int r, Fn&& fn) {
    for (int j = 0; j <= 2 * r; ++j) {
        const int y = cy - r + j;
        const int half = std::min(j, 2 * r - j);
        const int w = 2 * half + 1;
        const int x0 = cx - half;
        for (int i = 0; i < w; ++i) {
            const int ad2e = (i == w - 1) ? 0 : std::min(i + 1, w - i - 2);
            fn(x0 + i, y, ad2e);
        }
    }
}

// sub_3E739 (sub_40295 [+6Ch]==1, mode 2, sec var_18): allied approach tactic target.
// F0648 (the hostile list) is filled by the sub_4016A threat scan (sub_1D124 class
// 0x0B/0x0C path); the port passes the hostile list of the budget-radius diamond with
// the same approximation as the k=0 path.
//  Phase A: clear E8F46 to -1 over an r=mode diamond around each hostile (inside the
//           sub_13D02 gate).
//  Phase B: walk an r=mode+3 diamond around each hostile and store the alignment value
//           in E65B0 by signed word strict maximum (previous cell sub_1AD2E*16, plus
//           0x10 when on the same row or column); only the low byte is stored.
//  Phase C: on the same diamond, add 0x320 + E65B0 (signed) to each cell whose E8F46
//           (unsigned word) is below 0x320.
//  Phase D: find the strict E8F46 maximum in the actor's r=sec diamond (initial 0, so
//           only positive values qualify) and take the first cell that passes sub_165C2
//           with si 0 then 1 as dest/facing. A maximum of 0 fails with eax = 0, leaving
//           the phase A-C damage in place, as in the original.
//  Phase E: nearest F0648 hostile (sub_49B01 |dx|+|dy| strict minimum), then sub_14F52
//           facing; if the sub_165C2 re-gate fails, fall back to the x/y comparison
//           (2/3) selected by the parity of the phase D si.
// Returns 1 on success (out_hostile = units_ index), 0 on failure.
int CombatII::battle_ai_tact_3e739(int unit_index, int mode, int sec, int diff_budget,
                                   const std::vector<int>& hostiles, int& out_dx,
                                   int& out_dy, int& out_facing, int& out_hostile,
                                   const std::vector<int>* pre_block) const {
    out_dx = out_dy = out_facing = out_hostile = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (map_w_ == 0 || map_h_ == 0) return 0;
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    if (!actor.active) return 0;
    if (hostiles.empty()) return 0; // F0648 count 0 -> phase D maximum stays 0 -> fail
    if (mode < 0 || sec < 0) return 0;
    std::vector<char> fringe;
    std::vector<int> e8 = battle_ai_build_diffusion(unit_index, diff_budget, &fringe);
    if (e8.empty() || fringe.size() != e8.size()) return 0;
    const int actor_x = (int)(int8_t)actor.x, actor_y = (int)(int8_t)actor.y;
    const int start_idx = actor_y * map_w + actor_x;
    if (start_idx < 0 || (size_t)start_idx >= e8.size()) return 0;
    e8[(size_t)start_idx] -= 1; // sub_143F3 end state (undoes the sub_3CA3C head refresh)
    auto e8_read = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= map_w || y >= map_h) return -128; // sub_13CBD OOB
        return e8[(size_t)y * (size_t)map_w + (size_t)x];
    };
    auto e8_write = [&](int x, int y, int v) {
        if (x < 2 || x >= map_w - 2 || y < 4 || y >= map_h - 4) return; // sub_13D02
        e8[(size_t)y * (size_t)map_w + (size_t)x] = (int16_t)v;
    };
    auto hostile_xy = [&](int h) {
        return std::pair<int,int>((int)(int8_t)units_[(size_t)h].x,
                                  (int)(int8_t)units_[(size_t)h].y);
    };
    // sub_3E457 pre-mark (sub_438AE): E8F46 = -1 over an r=2 diamond per hostile (13D02 gate).
    if (pre_block) {
        for (int h : *pre_block) {
            if (h < 0 || (size_t)h >= units_.size()) continue;
            const auto hp = hostile_xy(h);
            ai_diamond_each(hp.first, hp.second, 2,
                            [&](int x, int y, int) { e8_write(x, y, -1); });
        }
    }
    // Phase A
    for (int h : hostiles) {
        if (h < 0 || (size_t)h >= units_.size()) continue;
        const auto hp = hostile_xy(h);
        ai_diamond_each(hp.first, hp.second, mode,
                        [&](int x, int y, int) { e8_write(x, y, -1); });
    }
    // Phase B
    for (int h : hostiles) {
        if (h < 0 || (size_t)h >= units_.size()) continue;
        const auto hp = hostile_xy(h);
        int align = 0;
        ai_diamond_each(hp.first, hp.second, mode + 3, [&](int x, int y, int ad2e) {
            if (x == hp.first || y == hp.second) align = (int16_t)(align + 0x10);
            if (x >= 0 && y >= 0 && x < map_w && y < map_h) {
                const size_t idx = (size_t)y * (size_t)map_w + (size_t)x;
                const int old = (int)(int8_t)fringe[idx];
                if (old < (int16_t)align) fringe[idx] = (char)(align & 0xFF);
            }
            align = (int16_t)(ad2e << 4);
        });
    }
    // Phase C
    for (int h : hostiles) {
        if (h < 0 || (size_t)h >= units_.size()) continue;
        const auto hp = hostile_xy(h);
        ai_diamond_each(hp.first, hp.second, mode + 3, [&](int x, int y, int) {
            if (x < 2 || x >= map_w - 2 || y < 4 || y >= map_h - 4) return;
            const size_t idx = (size_t)y * (size_t)map_w + (size_t)x;
            const int cur = e8[idx] & 0xFFFF;
            if (cur >= 0x320) return;
            const int fr = (int)(int8_t)fringe[idx];
            e8[idx] = (int16_t)((cur + 0x320 + fr) & 0xFFFF);
        });
    }
    // Phase D
    int best = 0;
    int dest_x = -1, dest_y = -1, face = -1;
    ai_diamond_each(actor_x, actor_y, sec, [&](int x, int y, int) {
        const int v = e8_read(x, y);
        if (v <= best) return;
        int si_hit = -1;
        for (int si = 0; si < 2; ++si) {
            if (battle_ai_165c2_1x1_ok(x, y, unit_index, e8)) { si_hit = si; break; }
        }
        if (si_hit < 0) return;
        best = v; dest_x = x; dest_y = y; face = si_hit;
    });
    if (best == 0 || dest_x < 0) return 0;
    // Phase F: nearest F0648 hostile (sub_49B01, strict minimum, first wins ties).
    int best_dist = 0x7FFF;
    int nearest = -1;
    for (int h : hostiles) {
        if (h < 0 || (size_t)h >= units_.size()) continue;
        const auto hp = hostile_xy(h);
        const int dist = std::abs(dest_x - hp.first) + std::abs(dest_y - hp.second);
        if (dist >= best_dist) continue;
        best_dist = dist;
        nearest = h;
    }
    if (nearest < 0) return 0;
    // Phase G: sub_14F52 facing, then the sub_165C2 re-gate (parity fallback on failure).
    const auto nhp = hostile_xy(nearest);
    const int toward = (int)walk_facing_toward(dest_x, dest_y, nhp.first, nhp.second);
    out_dx = dest_x;
    out_dy = dest_y;
    if (battle_ai_165c2_1x1_ok(dest_x, dest_y, unit_index, e8)) {
        out_facing = toward;
    } else if ((face & 1) == 0) {
        out_facing = (nhp.second < dest_y) ? 2 : toward;
    } else {
        out_facing = (nhp.first > dest_x) ? 3 : toward;
    }
    out_hostile = nearest;
    return 1;
}

// sub_3EF12 (sub_40295 [+6Ch]==5, push 3): retreat target.
// sub_3E3BF rebuilds F0648 from the hostile-occupied units inside a max(radius,3)
// diamond (y-then-x, centre included); a 1201D inversion also adds the actor itself.
// Phase 1 clears E8F46 to -1 over an r=4 diamond per hostile (13D02 gate). Phase 2
// picks the strict E8F46 maximum in the actor's r=radius diamond (initial word 0xFFFF)
// among cells passing sub_165C2 with si 0/1, and fails when the maximum stays -1.
// Phase 3 derives the facing from F0648[0] through sub_14F52 and, when the sub_165C2
// re-gate fails, uses the phase 2 si parity x/y comparison fallback (2/3).
// Returns 1 on success (di = 1) or -1 on failure (di = -1).
int CombatII::battle_ai_flee_3ef12(int unit_index, int radius, int diff_budget,
                                   int& out_dx, int& out_dy, int& out_facing) const {
    out_dx = out_dy = out_facing = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (map_w_ == 0 || map_h_ == 0) return -1;
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    if (!actor.active) return -1;
    if (radius < 0) return -1;
    const int actor_x = (int)(int8_t)actor.x, actor_y = (int)(int8_t)actor.y;
    // sub_3E3BF: r = max(radius, 3) signed, rebuild F0648.
    const int scan_r = (radius >= 3) ? radius : 3;
    std::vector<int> hostiles;
    ai_diamond_each(actor_x, actor_y, scan_r, [&](int x, int y, int) {
        const int h = battle_ai_hostile_unit_at(x, y, unit_index);
        if (h >= 0) hostiles.push_back(h);
    });
    if (hostiles.empty()) return -1; // sub_3E3BF ax == 0 -> loc_3F0D7 sets eax = -1
    std::vector<char> fringe;
    std::vector<int> e8 = battle_ai_build_diffusion(unit_index, diff_budget, &fringe);
    if (e8.empty() || fringe.size() != e8.size()) return -1;
    const int start_idx = actor_y * map_w + actor_x;
    if (start_idx < 0 || (size_t)start_idx >= e8.size()) return -1;
    e8[(size_t)start_idx] -= 1; // sub_143F3 end state
    auto e8_read = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= map_w || y >= map_h) return -128; // sub_13CBD OOB
        return e8[(size_t)y * (size_t)map_w + (size_t)x];
    };
    auto e8_write = [&](int x, int y, int v) {
        if (x < 2 || x >= map_w - 2 || y < 4 || y >= map_h - 4) return; // sub_13D02
        e8[(size_t)y * (size_t)map_w + (size_t)x] = (int16_t)v;
    };
    // Phase 1: E8F46 = -1 over an r=4 diamond per hostile.
    for (int h : hostiles) {
        if (h < 0 || (size_t)h >= units_.size()) continue;
        const int hx = (int)(int8_t)units_[(size_t)h].x;
        const int hy = (int)(int8_t)units_[(size_t)h].y;
        ai_diamond_each(hx, hy, 4, [&](int x, int y, int) { e8_write(x, y, -1); });
    }
    // Phase 2: actor r=radius, strict maximum starting at word 0xFFFF (-1), with sub_165C2.
    int best = (int)(int16_t)0xFFFF;
    int dest_x = -1, dest_y = -1, face = -1;
    ai_diamond_each(actor_x, actor_y, radius, [&](int x, int y, int) {
        const int v = (int16_t)e8_read(x, y);
        if (v <= best) return;
        int si_hit = -1;
        for (int si = 0; si < 2; ++si) {
            if (battle_ai_165c2_1x1_ok(x, y, unit_index, e8)) { si_hit = si; break; }
        }
        if (si_hit < 0) return;
        best = v; dest_x = x; dest_y = y; face = si_hit;
    });
    if (best == -1 || dest_x < 0) return -1; // loc_3F043: maximum stays -1 (not found)
    // Phase 3: facing from F0648[0].
    const int h0 = hostiles[0];
    const int h0x = (int)(int8_t)units_[(size_t)h0].x;
    const int h0y = (int)(int8_t)units_[(size_t)h0].y;
    const int toward = (int)walk_facing_toward(dest_x, dest_y, h0x, h0y);
    out_dx = dest_x;
    out_dy = dest_y;
    if (battle_ai_165c2_1x1_ok(dest_x, dest_y, unit_index, e8)) {
        out_facing = toward;
    } else if ((face & 1) == 0) {
        out_facing = (h0y < dest_y) ? 2 : toward;
    } else {
        out_facing = (h0x > dest_x) ? 3 : toward;
    }
    return 1;
}

// Branch-only walk tail of sub_40295 (loc_405CA..loc_4084E).
// On di == 1 (sub_3E739/sub_3EF12 success) it uses the sub_15523 route plus the
// sub_1620A walk; with use_wave it runs the full sub_1419C/sub_1499C/sub_15342/
// sub_15523/sub_14FA4 pipeline. After sub_12124/sub_11E04/sub_45559 (after_move and
// occupancy here), loc_407F3 recomputes S = var_18*4 - sub_13CBD(arrival cell), clears
// [57h]/[7F]/[81h] through sub_3C993 and subtracts (S>>2)*10 + var_4 from the shared
// gauge. sub_167C9 (var_4 = 10 + 30n) and sub_13F8A (diffusion clear) only matter when
// the original loop resumes, so the port drops them: one action per turn, with no
// sub_4016A re-check and no extra movement.
bool CombatII::battle_ai_wc1_branch_walk(int unit_index, int dest_x, int dest_y, int facing,
                                         int settle_budget, int var_4, int ref_hx, int ref_hy,
                                         bool use_wave) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const int actor_id = units_[(size_t)unit_index].id;
    const int actor_x = (int)(int8_t)units_[(size_t)unit_index].x;
    const int actor_y = (int)(int8_t)units_[(size_t)unit_index].y;
    const int mobility = battle_ai_mobility_selector(units_[(size_t)unit_index]);
    // Settlement uses the diffusion as it stood before walking (start of the sub_15523
    // route). In the original that is the sub_14032/sub_143F3 rebuild at sub_3E739
    // phase E and at the end of sub_3EF12, so rebuild it here and undo only the
    // start-cell refresh.
    std::vector<int> settle_grid = battle_ai_build_diffusion(unit_index, settle_budget);
    const int start_idx = actor_y * (int)map_w_ + actor_x;
    if (!settle_grid.empty() && start_idx >= 0 && (size_t)start_idx < settle_grid.size())
        settle_grid[(size_t)start_idx] -= 1;
    if (!battle_ai_wc1_walk_to(unit_index, actor_id, actor_x, actor_y, mobility,
                               dest_x, dest_y, ref_hx, ref_hy, facing, use_wave))
        return false;
    if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id)
        return false;
    const int now_x = (int)(int8_t)units_[(size_t)unit_index].x;
    const int now_y = (int)(int8_t)units_[(size_t)unit_index].y;
    int arrive = -128; // sub_13CBD OOB
    if (now_x >= 0 && now_y >= 0 && now_x < (int)map_w_ && now_y < (int)map_h_ &&
        !settle_grid.empty())
        arrive = settle_grid[(size_t)now_y * (size_t)map_w_ + (size_t)now_x];
    const int s = settle_budget - arrive;                 // `sub ebx,eax` (word)
    const int charge = ((s >> 2) * 10) + var_4;           // `sar eax,0x12` equivalent
    auto& u = units_[(size_t)unit_index];
    u.combat_acc57 = 0; // sub_3C993: [57h]=0
    u.ai_field_7f = 0;  // [7F]=0
    u.ai_field_81 = 0;  // [81]=0
    battle_tp_consume(charge > 0 ? charge : 0); // sub_1A0C6 (clamped at 0)
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d branch walk settle S=%d charge=%d shared_gauge=%d (sub_40295 loc_407F3 sub_3C993; sub_167C9 var_4=%d dropped, loop not resumed)\n",
                actor_id, s, charge, battle_tp(), var_4);
    battle_ai_var4_ = battle_ai_167c9(unit_index); // sub_167C9 -> var_4, the next budget denominator
    return true;
}

// ==== sub_40295 jump table k = 2/3/4/6 ====
// The 1x1 occupancy/footprint approximation matches the WC1 gate; the CC998 lifecycle
// is not modelled.

// sub_1A452/sub_1A552 band iterator (file local): cx cells per row over |rng| rows.
// Per-direction steps; dir 1 uses F0B98 = -(cx-1).
struct AiBandIter {
    int x = 0, y = 0;
    int s9a = 0, s96 = 0, s9c = 0, s98 = 0;
    int c_lo = 0, rows = 0;
    int n_lo = 0, n_hi = 0;
    bool done = false;
};

static void ai_band_init(AiBandIter& it, int x, int y, int d, int cx, int rng) {
    it.x = x; it.y = y; it.c_lo = cx; it.n_lo = 0; it.n_hi = 0; it.done = false;
    int sgn = 1;
    if (rng < 0) { sgn = -1; rng = -rng; }
    it.rows = rng;
    switch (d & 3) {
        case 0:  it.s9a = 1;  it.s96 = 0;  it.s9c = -(cx - 1); it.s98 = sgn; break;
        case 1:  it.s9a = 0;  it.s96 = 1;  it.s9c = -sgn;      it.s98 = -(cx - 1); break;
        case 2:  it.s9a = -1; it.s96 = 0;  it.s9c = cx - 1;    it.s98 = -sgn; break;
        default: it.s9a = 0;  it.s96 = -1; it.s9c = sgn;       it.s98 = cx - 1; break;
    }
}

static bool ai_band_next(AiBandIter& it, int& ox, int& oy) {
    if (it.done) return false;
    ox = it.x; oy = it.y;
    if (++it.n_lo != it.c_lo) {
        it.x += it.s9a; it.y += it.s96;
    } else {
        it.n_lo = 0;
        ++it.n_hi;
        it.x += it.s9c; it.y += it.s98;
        if (it.n_hi == it.rows) it.done = true;
    }
    return true;
}

// sub_1A7BA with F0B8E==3 (sub_3D7B2 wc 0xB/0xC). This is the same two-pass sub_1A452
// band geometry as the mode-2 3CA3C arm (jump table at off_1A79A, the slotA/slotB row
// test and the pass-start adjustment), but the candidate score is the strict minimum
// sub_49ADF distance from the actor, gated by sub_16699 instead of sub_13CBD/sub_165C2.
// No E8 read is needed on this path. F0B84_hi (chain) is shared across facings.
int CombatII::battle_ai_1a7ba_distance(int unit_index, int x0, int y0, int d, int K,
                                       int& chain, int& bx, int& by) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return chain;
    if (map_w_ == 0 || map_h_ == 0) return chain;
    static constexpr int kStepX[4] = { 0, -1, 0, 1 };  // S,W,N,E (sub_49ABC)
    static constexpr int kStepY[4] = { 1, 0, -1, 0 };
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    const int mobility = battle_ai_mobility_selector(actor);
    auto attr_at = [&](int x, int y) -> int {  // sub_1093D
        if (x < 0 || y < 0 || x >= map_w || y >= map_h) return 0;
        const size_t i = (size_t)y * (size_t)map_w + (size_t)x;
        return i < tile_attr_.size() ? (int)tile_attr_[i] : 0;
    };
    const int X = x0 + kStepX[(d + 2) & 3];  // back to the hostile cell
    const int Y = y0 + kStepY[(d + 2) & 3];
    const int raw0 = attr_at(X, Y);
    const int coarse0 = raw0 >= 0x64 ? raw0 - 0x64 : raw0;
    const int base = coarse0 & 0xF0;           // var_10
    const int base_hi = (base + 0x10) & 0xFF;  // var_8
    int x1 = X, y1 = Y, slotA = 0, slotB = 0, slotC = 0;
    if (d == 0)      { x1 -= 1; y1 -= 1; slotA = x1;     slotC = 6; }
    else if (d == 1) { slotB = y1 + 1;    x1 += 1;       slotC = 6; }
    else if (d == 2) { slotA = x1 - 1;    y1 += 1;       slotC = 4; }
    else             { slotB = y1 + 1;    x1 -= 1; y1 += 1; slotC = 4; }
    for (int pass = 0; pass < 2; ++pass) {
        const int rng = K + (slotC >> 1);
        int varC = base, var4 = base_hi, edi = 0;
        AiBandIter it;
        ai_band_init(it, x1, y1, d, 2, rng);
        int cx, cy;
        while (ai_band_next(it, cx, cy)) {
            const int raw = attr_at(cx, cy);
            const int al = raw >= 0x64 ? raw - 0x64 : raw;
            if (al == 0) break;
            if (al == varC + 8) break;
            if (al < varC) { if (base_hi != var4) break; }
            if (al > var4) { varC = var4; var4 = (var4 + 0x10) & 0xFF; }
            if (++edi <= slotC) continue;
            if ((d & 1) == 0) { if (cx != slotA) continue; }
            else { if (cy != slotB) continue; }
            const int dist = std::abs(ax - cx) + std::abs(ay - cy);
            if (dist >= chain) continue;  // strict minimum
            if (!battle_ai_candidate_tile_legal_1x1(cx, cy, unit_index, mobility)) continue;
            bx = cx; by = cy; chain = dist;
        }
        if (d == 0)      { x1 += 1; slotA = x1; }
        else if (d == 1) { y1 -= 1; slotB = y1; }
        else if (d == 2) { slotA = x1; x1 += 1; }
        else             { y1 -= 1; slotB = y1; }
    }
    return chain;
}

// Scores one input cell (a hostile position) for the sub_3CA3C arms: wc <= 3
// (loc_3CB73), wc == 4 (loc_3CC3A), wc == 5 (loc_3CC91) and wc 6..0x13
// (loc_3CE1E/CF86/CFFC/D091/D12B/D1D4/D2E3).
// The entry/exit actor refresh (sub_14307+1 -> sub_13D02 with budget+1) is already baked
// into battle_ai_build_diffusion; the release/re-write pair (sub_11EE4/sub_11E04) is a
// no-op for the occupancy test. wc == 5 is the WC5 chain (sub_1AEF3/1CB53/1B7D4/1C685/
// 1CBB1 plus the 0x5A - dist and facing bonuses).
// wc 6..0x13 exclude the actor from the sub_11DB2 occupancy test - the sub_11EE4 head
// release leaves the actor cell empty - and the epilogue adds score*4 + E8[dest] for
// wc in {6,7,8,9,0xD} plus 4 when the facing matches. wc 0xA and 0x14+ are no-ops with
// no candidates.
// Returns the score (-1 when none) and out_facing = (f + 2) % 4.
int CombatII::battle_ai_3ca3c_cell(int unit_index, int wc, int hx, int hy,
                                   std::vector<int>& e8, int budget,
                                   int& out_dx, int& out_dy, int& out_facing) const {
    out_dx = out_dy = out_facing = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (map_w_ == 0 || map_h_ == 0) return -1;
    static constexpr int kStepX[4] = { 0, -1, 0, 1 };   // sub_49ABC 0=S 1=W 2=N 3=E
    static constexpr int kStepY[4] = { 1, 0, -1, 0 };
    const int map_w = (int)map_w_, map_h = (int)map_h_;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int actor_x = (int)(int8_t)actor.x, actor_y = (int)(int8_t)actor.y;
    const int actor_facing = (int)(int8_t)actor.facing;
    if (wc == 5) {
        // loc_3CC91: WC5 chain. As in the per-hostile calls from sub_3E65E/sub_3EADD/
        // sub_3ED51, refresh the actor footprint with budget+1 before scoring.
        battle_ai_wc5_diff_write(e8, actor_x, actor_y, budget + 1);
        Wc5Iter it;
        Wc5Score st;
        st.snap.assign(256, 0);
        return battle_ai_wc5_hostile_score(e8, it, st, unit_index, hx, hy,
                                           out_dx, out_dy, out_facing);
    }
    if (wc == 0xA || wc >= 0x14) return -1;  // loc_3CD6A no-op arm (no candidate)
    int best = -1;
    int fbest = -1;
    if (wc == 4) {
        // loc_3CC3A: single candidate cell, strict E8 maximum. sub_11DB2 rejects other
        // units only, because the sub_11EE4 head release leaves the actor cell free, so
        // the actor tile is accepted. No bucket or sub_165C2 test.
        for (int f = 0; f < 4; ++f) {
            const int cx = hx + kStepX[f], cy = hy + kStepY[f];
            const int v = battle_ai_wc5_diff_read(e8, cx, cy);      // sub_13CBD
            if (v <= best) continue;
            const int occ = battle_unit_index_at(cx, cy);            // sub_11DB2
            if (occ >= 0 && occ != unit_index) continue;
            best = v; out_dx = cx; out_dy = cy; fbest = f;
        }
    } else if (wc <= 3) {
        const int rng = (wc == 3) ? 2 : 1;                          // sub_1A452 range
        for (int f = 0; f < 4; ++f) {
            int cx = hx + kStepX[f], cy = hy + kStepY[f];
            for (int k = 0; k < rng; ++k) {
                // sub_1A2FE with base = input cell; a failure ends this facing.
                if (!battle_ai_terrain_bucket_compatible(hx, hy, cx, cy)) break;
                const int v = battle_ai_wc5_diff_read(e8, cx, cy);  // sub_13CBD
                if (v > best && battle_ai_165c2_1x1_ok(cx, cy, unit_index, e8)) {
                    best = v; out_dx = cx; out_dy = cy; fbest = f;
                }
                cx += kStepX[f]; cy += kStepY[f];
            }
        }
    } else {
        // Wc 6..0x13 arm.
        auto occ_other = [&](int x, int y) {
            const int o = battle_unit_index_at(x, y);
            return o >= 0 && o != unit_index;       // sub_11DB2 (the actor cell is released)
        };
        auto attr_at = [&](int x, int y) -> int {   // sub_1093D (0 outside the map)
            if (x < 0 || y < 0 || x >= map_w || y >= map_h) return 0;
            const size_t i = (size_t)y * (size_t)map_w + (size_t)x;
            return i < tile_attr_.size() ? (int)tile_attr_[i] : 0;
        };
        auto footprint_step = [&](int x, int y, int d, int& sx, int& sy) {
            // sub_1A146: [0]/[1] step of the D66A8 record (slot = actor appearance, dir).
            // The AI detail path only opens behind the class-0 1x1 ordinary gate, so
            // fw = fh = 1 (plain sub_49ABC step); larger footprints stay outside it.
            switch (d & 3) {
                case 0:  sx = x;      sy = y + 1;       break;
                case 1:  sx = x - 1;  sy = y - 0;       break;
                case 2:  sx = x + 0;  sy = y - 1;       break;
                default: sx = x + 1;  sy = y;           break;
            }
        };
        // sub_3C9AC: hostile count (sub_1201D) in the sub_1A452 (cx=2) area past one sub_1A146
        // step.
        auto count_3c9ac = [&](int x, int y, int d, int rng) {
            int sx = x, sy = y;
            footprint_step(x, y, d, sx, sy);
            int n = 0;
            AiBandIter it;
            ai_band_init(it, sx, sy, d, 2, rng);
            int cx, cy;
            while (ai_band_next(it, cx, cy)) {
                if (battle_ai_hostile_unit_at(cx, cy, unit_index) >= 0) ++n;
            }
            return n;
        };
        int di = -1;
        int sxx = -1, syy = -1, sslot = -1;
        // sub_1A7BA (sub_1B7D4 mode 2 plus the two-pass band scan); chain is F0B84_hi.
        auto band_1a7ba = [&](int x0, int y0, int d, int K, int& chain,
                              int& bx, int& by) {
            const int X = x0 + kStepX[(d + 2) & 3];    // back to the hostile cell
            const int Y = y0 + kStepY[(d + 2) & 3];
            const int raw0 = attr_at(X, Y);
            const int coarse0 = raw0 >= 0x64 ? raw0 - 0x64 : raw0;
            const int base = coarse0 & 0xF0;           // var_10
            const int base_hi = (base + 0x10) & 0xFF;  // var_8
            int x1 = X, y1 = Y, slotA = 0, slotB = 0, slotC = 0;
            if (d == 0)      { x1 -= 1; y1 -= 1; slotA = x1;     slotC = 6; }
            else if (d == 1) { slotB = y1 + 1;    x1 += 1;       slotC = 6; }
            else if (d == 2) { slotA = x1 - 1;    y1 += 1;       slotC = 4; }
            else             { slotB = y1 + 1;    x1 -= 1; y1 += 1; slotC = 4; }
            for (int pass = 0; pass < 2; ++pass) {
                const int rng = K + (slotC >> 1);
                int varC = base, var4 = base_hi, edi = 0;
                AiBandIter it;
                ai_band_init(it, x1, y1, d, 2, rng);
                int cx, cy;
                while (ai_band_next(it, cx, cy)) {
                    const int raw = attr_at(cx, cy);
                    const int al = raw >= 0x64 ? raw - 0x64 : raw;
                    if (al == 0) break;
                    if (al == varC + 8) break;
                    if (al < varC) { if (base_hi != var4) break; }
                    if (al > var4) { varC = var4; var4 = (var4 + 0x10) & 0xFF; }
                    if (++edi <= slotC) continue;
                    if ((d & 1) == 0) { if (cx != slotA) continue; }
                    else { if (cy != slotB) continue; }
                    const int v = battle_ai_wc5_diff_read(e8, cx, cy);
                    if (v <= chain) continue;
                    if (battle_ai_165c2_1x1_ok(cx, cy, unit_index, e8)) {
                        bx = cx; by = cy; chain = v;
                    }
                }
                if (d == 0)      { x1 += 1; slotA = x1; }
                else if (d == 1) { y1 -= 1; slotB = y1; }
                else if (d == 2) { slotA = x1; x1 += 1; }
                else             { y1 -= 1; slotB = y1; }
            }
        };
        // Wc 0x11/0x12/0x13: strict sub_13CBD maximum with sub_11DB2 other-unit rejection.
        auto scan_band_max = [&](int x0, int y0, int d, int cxn, int rng) {
            AiBandIter it;
            ai_band_init(it, x0, y0, d, cxn, rng);
            int cx, cy;
            while (ai_band_next(it, cx, cy)) {
                const int v = battle_ai_wc5_diff_read(e8, cx, cy);
                if (v <= di) continue;
                if (occ_other(cx, cy)) continue;
                di = v; sxx = cx; syy = cy; sslot = d;
            }
        };
        int chain = -1, chain_x = -1, chain_y = -1;
        for (int f = 0; f < 4; ++f) {
            int cx0 = hx + kStepX[f], cy0 = hy + kStepY[f];
            if (wc <= 9 || wc == 0xE || wc == 0xD) {
                // loc_3CE03 (wc 0xD entry adjustment) and the shared loc_3CE1E body.
                if (wc == 0xD) {
                    if (f == 0) cy0 += 1;
                    else if (f == 1) cx0 -= 1;
                }
                const int rng = (wc == 8 || wc == 0xD) ? 3 : 2;     // var_18
                if (f == 0) cx0 -= 1;
                else if (f == 3) cy0 += 1;
                int bufx[8], bufy[8], nb = 0;
                AiBandIter it;
                ai_band_init(it, cx0, cy0, f, 2, rng);
                int bx, by;
                while (nb < 8 && ai_band_next(it, bx, by)) {
                    bufx[nb] = bx; bufy[nb] = by; ++nb;
                }
                const int d1 = (f + 2) & 3;
                for (int i = nb - 1; i >= 0; --i) {                 // reverse order
                    const int cnt = count_3c9ac(bufx[i], bufy[i], d1, rng);
                    if (di >= cnt) continue;                        // strict maximum
                    if (battle_ai_165c2_1x1_ok(bufx[i], bufy[i], unit_index, e8)) {
                        di = cnt; sxx = bufx[i]; syy = bufy[i]; sslot = f;
                    }
                }
            } else if (wc == 0xB || wc == 0xC) {
                const int K = (wc == 0xB) ? 9 : 7;
                if (f == 0) chain = -1;                             // sub_1B7D4(-1, 2)
                band_1a7ba(cx0, cy0, f, K, chain, chain_x, chain_y);
                if (di < chain) {
                    di = chain; sxx = chain_x; syy = chain_y; sslot = f;
                }
            } else if (wc == 0xF) {
                int gate = 0;                                       // sub_1AD2E running value
                ai_diamond_each(cx0, cy0, 4, [&](int px, int py, int ad2e) {
                    if (gate < 3) {
                        const int v = battle_ai_wc5_diff_read(e8, px, py);
                        if (v > di && !occ_other(px, py)) {
                            di = v; sxx = px; syy = py; sslot = f;
                        }
                    }
                    gate = ad2e;
                });
            } else if (wc == 0x10) {
                ai_diamond_each(hx, hy, 3, [&](int px, int py, int) {
                    const int v = battle_ai_wc5_diff_read(e8, px, py);
                    if (v <= di) return;
                    if (occ_other(px, py)) return;
                    di = v; sxx = px; syy = py;
                    sslot = (actor_facing + 2) & 3;
                });
                break;                                              // loc_3D11F: f = 4
            } else if (wc == 0x11) {
                int ax2 = cx0 + kStepX[f], ay2 = cy0 + kStepY[f];
                const int s1 = (f + 1) & 3;
                ax2 += kStepX[s1]; ay2 += kStepY[s1];
                scan_band_max(ax2, ay2, f, 3, 7);
            } else if (wc == 0x12) {
                int ax2 = cx0 + kStepX[f], ay2 = cy0 + kStepY[f];
                ax2 += kStepX[f]; ay2 += kStepY[f];
                const int s1 = (f + 1) & 3;
                ax2 += kStepX[s1]; ay2 += kStepY[s1];
                const int prev_di = di;
                scan_band_max(ax2, ay2, f, 3, 3);
                if (di != prev_di) {
                    int slot2 = (f + 1) & 3;
                    if (slot2 & 1) slot2 = (actor_x >= sxx) ? 3 : 1;
                    else slot2 = (actor_y >= syy) ? 0 : 2;
                    sslot = slot2;
                }
            } else if (wc == 0x13) {
                if ((f & 1) == 0) continue;                         // odd f only
                scan_band_max(cx0, cy0, f, 1, 5);
            }
        }
        best = di;
        fbest = sslot;
        out_dx = sxx;
        out_dy = syy;
        if (best == -1) return -1;
        if (wc == 6 || wc == 7 || wc == 8 || wc == 9 || wc == 0xD)
            best = best * 4 + battle_ai_wc5_diff_read(e8, out_dx, out_dy);
        out_facing = (fbest + 2) & 3;
        if (actor_facing == out_facing) best += 4;
        return best;
    }
    if (best == -1) return -1;
    out_facing = (fbest + 2) & 3;
    // Epilogue (loc_3D3A7): add 4 when [63h] == (f+2)%4 (applies to wc <= 4 too).
    if (actor_facing == out_facing) best += 4;
    return best;
}

// sub_3D3E5 arms: wc <= 3 (loc_3D549), wc == 4 (loc_3D613) and wc == 5 (loc_3D6E8).
// The input cell is a hostile position. Returns the score (var_C, 0x7FFF when there is
// no candidate) and out_facing = (f+2)%4.
// var1c = sub_1762C(occupant, sub_17531(actor)); occ_* are occupant fields.
// wc == 5 uses the WC5 chain (the same shape as sub_3CA3C loc_3CC91): sub_1B7D4 resets
// to -1 per direction, the score is score0 - (chain score>>1) by strict minimum, and
// the destination is the chain's (F0B84 lo, F0B82).
int CombatII::battle_ai_3d3e5_cell(int unit_index, int wc, int hx, int hy,
                                   std::vector<int>& e8, int budget, int var1c,
                                   int occ_def, int occ_atk, int occ_facing,
                                   int& out_dx, int& out_dy, int& out_facing) const {
    out_dx = out_dy = out_facing = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0x7FFF;
    if (map_w_ == 0 || map_h_ == 0) return 0x7FFF;
    (void)budget; // entry refresh is baked into battle_ai_build_diffusion (wc5 rewrites it below)
    static constexpr int kStepX[4] = { 0, -1, 0, 1 };
    static constexpr int kStepY[4] = { 1, 0, -1, 0 };
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int wc5_rng = (wc == 5) ? battle_ai_wc5_aef3((int)actor.combat_class) : 0;
    Wc5Iter wc5_it;
    Wc5Score wc5_st;
    wc5_st.snap.assign(256, 0);
    if (wc == 5) {
        // sub_3D3E5 entry refresh (sub_14307+1 -> sub_13D02): actor cell = budget+1.
        battle_ai_wc5_diff_write(e8, (int)(int8_t)actor.x, (int)(int8_t)actor.y,
                                 budget + 1);
    }
    int best = 0x7FFF;
    int fbest = -1;
    const int rng = (wc == 3) ? 2 : 1;
    for (int f = 0; f < 4; ++f) {
        // Damage factor (loc_3D49F): front match -> defence, odd -> 3/4 defence,
        // even -> ((defence<<16)|attack) SAR 17. X = 0x12C - factor.
        const int d = (int16_t)occ_def;
        int term;
        if (occ_facing == f) {
            term = d;
        } else if (((f + occ_facing) & 1) != 0) {
            term = d - (d >> 2);
        } else {
            const uint32_t dw = ((uint32_t)(uint16_t)d << 16) | (uint16_t)occ_atk;
            term = (int32_t)dw >> 17;
        }
        const int xterm = 0x12C - term;
        const int score0 = (int16_t)(0xBB8 - xterm * var1c);        // `movsx esi,si`
        const int cx = hx + kStepX[f], cy = hy + kStepY[f];
        if (wc == 5) {
            // loc_3D6E8: WC5 chain - snapshot, sub_1B7D4(-1, 2, actor) reset, sub_1C685, restore.
            battle_ai_wc5_snapshot(e8, wc5_it, wc5_st, cx, cy, 3, f, wc5_rng + 1);
            battle_ai_wc5_state_init(wc5_st, -1, unit_index);
            battle_ai_wc5_1c685(e8, wc5_it, wc5_st, cx, cy, f, wc5_rng, unit_index);
            battle_ai_wc5_restore(e8, wc5_it, wc5_st, cx, cy, 3, f, wc5_rng + 1);
            const int cand = wc5_s16(wc5_st.b84s);
            if (cand == -1) continue;                               // `cmp eax,-1; jz`
            const int score = wc5_s16(score0 - (cand >> 1));
            if (wc5_s16(score) >= wc5_s16(best)) continue;
            best = score;
            out_dx = wc5_s16(wc5_st.b84x);
            out_dy = wc5_s16(wc5_st.b82);
            fbest = f;
            continue;
        }
        int ncx = cx, ncy = cy;
        const int ncand = (wc == 4) ? 1 : rng;
        for (int k = 0; k < ncand; ++k) {
            if (wc <= 3) {
                if (!battle_ai_terrain_bucket_compatible(hx, hy, ncx, ncy)) break; // 1A2FE
            }
            const int v = battle_ai_wc5_diff_read(e8, ncx, ncy);     // sub_13CBD
            if (v < 0) {
                if (wc == 4) break;                                 // `jl` ends this facing
                ncx += kStepX[f]; ncy += kStepY[f];
                continue;
            }
            const int score = (int16_t)(score0 - (v >> 1));
            if ((int16_t)score >= (int16_t)best) {
                if (wc == 4) break;
                ncx += kStepX[f]; ncy += kStepY[f];
                continue;
            }
            const int occ = battle_unit_index_at(ncx, ncy);
            const bool ok = (wc == 4) ? (occ < 0)                     // sub_11DB2 rejects nonzero
                                      : (occ < 0 || occ == unit_index); // sub_165C2
            if (ok) { best = score; out_dx = ncx; out_dy = ncy; fbest = f; }
            if (wc == 4) break;
            ncx += kStepX[f]; ncy += kStepY[f];
        }
    }
    if (best == 0x7FFF) return 0x7FFF;
    out_facing = (fbest + 2) & 3;
    return best;
}

// K=2 (sub_3EADD, state 2) and k=4 (sub_3ED51, state 4): score every hostile inside
// the budget-radius sec diamond with sub_3CA3C, then pick the strict minimum [+48h] HP
// for state 2 and the strict maximum [+43h] level for state 4. Scores <= -1 (a
// sub_3CA3C failure) are not candidates. The wc 0xA/0x14+ sub_3CA3C arms are no-ops, so
// those units always fail. Returns true on success.
bool CombatII::battle_ai_jump_approach(int unit_index, int state, int sec, int diff_budget,
                                       int& out_dx, int& out_dy, int& out_facing,
                                       int& out_hostile) const {
    out_dx = out_dy = out_facing = out_hostile = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int wc = weapon_class(actor);                             // sub_174CF
    std::vector<int> e8 = battle_ai_build_diffusion(unit_index, diff_budget);
    if (e8.empty()) return false;
    const int ax_ = (int)(int8_t)actor.x, ay_ = (int)(int8_t)actor.y;
    int best_hp = 0x7FFFFFFF;
    int best_level = -1;
    bool found = false;
    ai_diamond_each(ax_, ay_, sec, [&](int x, int y, int) {
        const int occ = battle_ai_hostile_unit_at(x, y, unit_index); // 11DB2+1201D
        if (occ < 0) return;
        int dx = -1, dy = -1, face = -1;
        const int score = battle_ai_3ca3c_cell(unit_index, wc, x, y, e8, diff_budget,
                                               dx, dy, face);
        if (score <= -1) return;
        const auto& other = units_[(size_t)occ];
        if (state == 2) {
            const int hp = battle_hp_raw(other);
            if (hp >= best_hp) return;
            best_hp = hp;
        } else {
            const int lvl = (int)other.combat_value_43;
            if (lvl <= best_level) return;
            best_level = lvl;
        }
        out_dx = dx; out_dy = dy; out_facing = face; out_hostile = occ;
        found = true;
    });
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d +6Ch==%d approach (sub_%s sec=%d) %s dest=(%d,%d) facing=%d target=%s\n",
                actor.id, state, (state == 2) ? "3EADD" : "3ED51", sec,
                found ? "ok" : "fail", out_dx, out_dy, out_facing,
                out_hostile >= 0 ? std::to_string(units_[(size_t)out_hostile].id).c_str() : "-");
    return found;
}

// ==== sub_169D7 / sub_3F0E4 / sub_3F247 / sub_3E21B / sub_3F184 ====
// Early/state selection path of sub_40DFD, checked against the original COMBATII.EXE
// bytes for calling convention and branches.
void CombatII::battle_ai_169d7_reset(int unit_index,
                                     std::vector<int>& grid) const {
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return;
    grid.assign((size_t)map_w_ * (size_t)map_h_, -1);
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int sel = battle_ai_mobility_selector(actor);            // sub_1665B
    if (sel < 0 || sel >= kMobilityGridCount) return;
    const uint8_t* mob = kMobilityCostGrids[sel];
    for (int y = 0; y < (int)map_h_; ++y) {
        for (int x = 0; x < (int)map_w_; ++x) {
            const size_t i = (size_t)y * (size_t)map_w_ + (size_t)x;
            const int attr = (int)tile_attr_[i];                   // unk_88BD4[i]
            const int cost = (int)(int8_t)mob[attr & 0xFF];        // `jle` on the signed value
            if (cost <= 0 || battle_unit_index_at(x, y) >= 0)      // CC998[i*4]!=0
                grid[i] = -2;
        }
    }
}

int CombatII::battle_ai_3e21b_target(int unit_index) const {
    // sub_3E21B resolves [+71h] through sub_1379A first and falls back to [+73h].
    return battle_ai_watch_unit(unit_index);
}

void CombatII::battle_ai_3f0e4(int unit_index, std::vector<int>& grid,
                               int radius, int band, int edd72) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (map_w_ == 0 || map_h_ == 0 || tile_attr_.empty()) return;
    battle_ai_169d7_reset(unit_index, grid);                       // sub_169D7
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    Wc5Dia dia;
    battle_ai_wc5_dia_init(dia, ax, ay, radius);                   // sub_1AC08
    // sub_14307 writes only AX, so the upper half of edi/var_4 comes from EAX before the
    // call (actor.x, 0..127): edi = (EDD72&FFFF)+1 and var_4 = (EDD72&FFFF)+2.
    const int edi = (edd72 & 0xFFFF) + 1;
    const int var4 = (edd72 & 0xFFFF) + 2;
    int ebx = 0;
    int x = 0, y = 0;
    while (battle_ai_wc5_dia_next(dia, x, y)) {                   // sub_1AC59
        if (battle_ai_wc5_diff_read(grid, x, y) == -1) {           // sub_13CBD
            ebx = wc5_s16(edi - ebx);
            battle_ai_wc5_diff_write(grid, x, y, ebx);             // sub_13D02
        }
        const int fold = battle_ai_wc5_1ad2e(dia);                 // sub_1AD2E
        if (fold >= wc5_s16(radius - band))
            ebx = var4;
        else
            ebx = radius - fold;
    }
}

bool CombatII::battle_ai_3f247(int unit_index, int radius, int edd72,
                               int& out_x, int& out_y) const {
    out_x = out_y = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    int tx = -1, ty = -1;
    if (actor.ai_home_x != 0xFF) {                                 // [+6Fh]!=FF
        tx = (int)actor.ai_home_x;
        ty = (int)actor.ai_home_y;
    } else {
        const int target = battle_ai_3e21b_target(unit_index);     // sub_3E21B
        if (target < 0) return false;
        tx = (int)(int8_t)units_[(size_t)target].x;
        ty = (int)(int8_t)units_[(size_t)target].y;
    }
    std::vector<int> grid;
    battle_ai_3f0e4(unit_index, grid, radius, 2, edd72);          // sub_3F0E4
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    Wc5Dia dia;
    battle_ai_wc5_dia_init(dia, ax, ay, radius);                   // sub_1AC08
    int best = 0x7FFF;
    bool found = false;
    int x = 0, y = 0;
    while (battle_ai_wc5_dia_next(dia, x, y)) {                   // sub_1AC59
        const int v = battle_ai_wc5_diff_read(grid, x, y);         // sub_13CBD
        if ((int16_t)v < 0) continue;                              // `test ax,ax; js`
        const int dist = std::abs(x - tx) + std::abs(y - ty);      // sub_49B01
        if (dist >= best) continue;                                // strict minimum
        best = dist;
        out_x = x; out_y = y;
        found = true;
    }
    return found;
}

// sub_3EBB4 with a caller-supplied grid. The sub_40DFD early block reuses the E8 grid
// left by sub_3F0E4, so this does not rebuild it through battle_ai_build_diffusion. The
// entry refresh (sub_14307+1) and exit refresh (sub_14307) are rewritten per candidate
// hostile through sub_13D02, as in sub_3D3E5/sub_3CA3C.
// out_metric mirrors word_F1BA4: best score for wc < 6, best [+46h] for wc >= 6.
bool CombatII::battle_ai_3ebb4_grid(int unit_index, int sec,
                                    const std::vector<int>& grid, int edd72,
                                    int& out_dx, int& out_dy, int& out_facing,
                                    int& out_hostile, int* out_metric) const {
    out_dx = out_dy = out_facing = out_hostile = -1;
    if (out_metric) *out_metric = 0x7FFE;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int wc = weapon_class(actor);                             // sub_174CF
    const int ax_ = (int)(int8_t)actor.x, ay_ = (int)(int8_t)actor.y;
    std::vector<int> e8 = grid;
    if (e8.empty()) return false;
    bool found = false;
    if (wc >= 6) {
        int best_def = 0x7FFE;                                      // var_2C
        ai_diamond_each(ax_, ay_, sec, [&](int x, int y, int) {
            const int occ = battle_ai_hostile_unit_at(x, y, unit_index);
            if (occ < 0) return;
            battle_ai_wc5_diff_write(e8, ax_, ay_, wc5_s16(edd72 + 1));
            int dx = -1, dy = -1, face = -1;
            const int score = battle_ai_3ca3c_cell(unit_index, wc, x, y, e8,
                                                   edd72, dx, dy, face);
            battle_ai_wc5_diff_write(e8, ax_, ay_, wc5_s16(edd72));
            if ((int16_t)score <= 0) return;                        // `test ax,ax; jle`
            const int d = (int16_t)units_[(size_t)occ].combat_value_46;
            if (d > best_def) return;                               // strict minimum
            best_def = d;
            out_dx = dx; out_dy = dy; out_facing = face; out_hostile = occ;
            found = true;
        });
        if (out_metric) *out_metric = best_def;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d +6Ch==3 3EBB4 wc>=6 scan (sec=%d wc=%d) %s def=%d dest=(%d,%d) facing=%d target=%s\n",
                    actor.id, sec, wc, found ? "ok" : "fail", best_def,
                    out_dx, out_dy, out_facing,
                    out_hostile >= 0 ? std::to_string(units_[(size_t)out_hostile].id).c_str() : "-");
        return found;
    }
    const int dc = battle_damage_class(actor);                      // sub_17531
    int best_score = 0x7FFE;                                        // var_10/var_2C
    int best_dist = 0x7FFE;                                         // var_20
    ai_diamond_each(ax_, ay_, sec, [&](int x, int y, int) {
        const int occ = battle_ai_hostile_unit_at(x, y, unit_index);
        if (occ < 0) return;
        const auto& other = units_[(size_t)occ];
        const int var1c = (dc < 0) ? 0x0A : battle_damage_adjustment(other, dc); // 1762C
        battle_ai_wc5_diff_write(e8, ax_, ay_, wc5_s16(edd72 + 1));
        int dx = -1, dy = -1, face = -1;
        const int score = battle_ai_3d3e5_cell(unit_index, wc, x, y, e8, edd72,
                                               var1c, (int16_t)other.combat_value_46,
                                               (int16_t)other.combat_value_44,
                                               (int)other.facing & 3, dx, dy, face);
        battle_ai_wc5_diff_write(e8, ax_, ay_, wc5_s16(edd72));
        if (score > best_score) return;                             // strict minimum
        if (score == best_score) {
            const int dist = std::abs(dx - ax_) + std::abs(dy - ay_); // sub_49ADF
            if (dist > best_dist) return;                           // tie: shorter distance wins
            best_dist = dist;
        } else {
            best_score = score;
        }
        out_dx = dx; out_dy = dy; out_facing = face; out_hostile = occ;
        found = true;
    });
    if (out_metric) *out_metric = best_score;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d +6Ch==3 3EBB4 scan (sec=%d dc=%d wc=%d) %s score=%d dest=(%d,%d) facing=%d target=%s\n",
                actor.id, sec, dc, wc, found ? "ok" : "fail", best_score,
                out_dx, out_dy, out_facing,
                out_hostile >= 0 ? std::to_string(units_[(size_t)out_hostile].id).c_str() : "-");
    return found;
}

// sub_3F184(actor, edx = &dest_x, ebx = &dest_y, ecx = &af, stack = sec):
//   target = sub_3E21B(actor), dmg = sub_17531(actor).
//   dmg >= 0 -> temporarily set unk_72982[target[11h]*8 + dmg] to 0x64.
//   dmg < 0  -> temporarily set target[46h] to 0xF830.
//   Restored after the sub_3EBB4(actor, sec) call. Returns target when the selected
//   unit is target, else 0 (represented as -1). dest/af pass through sub_3EBB4's values.
int CombatII::battle_ai_3f184(int unit_index, int sec,
                              const std::vector<int>& grid, int edd72,
                              int& out_dx, int& out_dy, int& out_af) {
    out_dx = out_dy = out_af = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int target = battle_ai_3e21b_target(unit_index);
    if (target < 0) return -1;
    const int dmg = battle_damage_class(units_[(size_t)unit_index]); // 17531
    int saved_index = -1;
    int saved_value = 0;
    bool table_branch = (dmg >= 0);
    if (table_branch) {
        saved_index = ((int)units_[(size_t)target].combat_class & 0xFF) * 8 + dmg;
        if (saved_index >= 0 && saved_index < kBattleDamageAdjustSize)
            saved_value = (int)(int8_t)kBattleDamageAdjustTable[saved_index];
        battle_ai_damage_adjust_index_ = saved_index;               // temporary 0x64
        battle_ai_damage_adjust_value_ = 0x64;
    } else {
        saved_value = (int16_t)units_[(size_t)target].combat_value_46;
        units_[(size_t)target].combat_value_46 = (int16_t)0xF830u;  // word 0xF830
    }
    int dx = -1, dy = -1, af = -1, occ = -1;
    const bool ok = battle_ai_3ebb4_grid(unit_index, sec, grid, edd72,
                                         dx, dy, af, occ);
    if (table_branch) {
        battle_ai_damage_adjust_index_ = -1;
        battle_ai_damage_adjust_value_ = 0;
    } else {
        units_[(size_t)target].combat_value_46 = (int16_t)saved_value;
    }
    out_dx = dx; out_dy = dy; out_af = af;
    if (!ok || occ != target) return -1;
    return target;
}


// K=3 (sub_3EBB4): for wc < 6 the strict minimum sub_3D3E5 score (ties use the shortest
// sub_49ADF distance); for wc >= 6 the sub_3CA3C score must be positive (16-bit) and the
// occupant's [+46h] defence is a strict minimum (loc_3ECD4). wc5 runs the sub_3D3E5 wc5
// arm (loc_3D6E8, WC5 chain).
// The table path rebuilds E8 through battle_ai_build_diffusion (actor = diff_budget) and
// delegates the selection to battle_ai_3ebb4_grid, which the early path shares.
bool CombatII::battle_ai_jump_3ebb4(int unit_index, int sec, int diff_budget,
                                    int& out_dx, int& out_dy, int& out_facing,
                                    int& out_hostile, int* out_metric) const {
    out_dx = out_dy = out_facing = out_hostile = -1;
    if (out_metric) *out_metric = 0x7FFE;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    std::vector<int> e8 = battle_ai_build_diffusion(unit_index, diff_budget);
    if (e8.empty()) return false;
    return battle_ai_3ebb4_grid(unit_index, sec, e8, diff_budget,
                                out_dx, out_dy, out_facing, out_hostile,
                                out_metric);
}

// K=6 (sub_3EE2B): for each direction 0..3 accept F0648[0] as a candidate when
// sub_165C2 (1x1) passes and sub_1D124(dir) collects exactly one target, then take the
// strict minimum [+48h] HP. dest is the actor position (turn in place), facing is the
// winning direction (the current facing when no candidate exists) and the return value
// says whether a candidate unit was found.
// E8F46 is shared: the original keeps one global grid, so the sub_13F8A clear and LOS
// shadow of sub_1D124 (wc5 loc_1D2DC) persist into the next direction's sub_165C2
// (E8 >= 0). For wc5 the gate closes after the first passing direction, leaving a single
// candidate; for wc0xB/0xC (line_scan) there is no clear, so several directions survive.
bool CombatII::battle_ai_jump_3ee2b(int unit_index, int diff_budget,
                                    int& out_dx, int& out_dy, int& out_facing,
                                    int& out_hostile) {
    out_dx = out_dy = out_facing = out_hostile = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int wc = weapon_class(actor);                             // sub_174CF
    // sub_1D124 arms enabled here: wc 0..4 (including tri3), wc5 (LOS loc_1D2DC),
    // wc0xB/0xC (line_scan loc_1D389), wc6..9/0xD (loc_1D327 band), wc0xE (loc_1D3CF),
    // wc0xF/0x10 (loc_1D420 diamond with sub_49B01 distance > 1), wc0x11 (loc_1D4B4),
    // wc0x12 (loc_1D518 two-pass) and wc0x13 (loc_1D63D); collect_targets_impl covers
    // the same set. wc0xA and 0x14+ do nothing, and wc0xD never fires because the table
    // holds no class for it.
    std::vector<int> e8 = battle_ai_build_diffusion(unit_index, diff_budget);
    if (e8.empty()) return false;
    const int ax_ = (int)(int8_t)actor.x, ay_ = (int)(int8_t)actor.y;
    const int facing = (int)actor.facing & 3;
    int best_hp = 0x7FFFFFFF;
    int best_dir = -1, best_occ = -1;
    for (int d = 0; d < 4; ++d) {
        if (!battle_ai_165c2_1x1_ok(ax_, ay_, unit_index, e8)) continue; // sub_165C2
        std::vector<int16_t> grid(e8.begin(), e8.end());
        const int n = battle_range_collect_dir_grid(unit_index, d, grid); // sub_1D124(dir)
        for (size_t i = 0; i < e8.size() && i < grid.size(); ++i)
            e8[i] = grid[i];                                             // shared E8F46 side effect
        if (n != 1) continue;
        const int occ = battle_target_list_.empty() ? -1 : battle_target_list_[0];
        if (occ < 0 || (size_t)occ >= units_.size()) continue;
        const int hp = battle_hp_raw(units_[(size_t)occ]);
        if (hp >= best_hp) continue;
        best_hp = hp; best_dir = d; best_occ = occ;
    }
    out_dx = ax_;
    out_dy = ay_;
    out_facing = (best_dir >= 0) ? best_dir : facing;
    out_hostile = best_occ;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d +6Ch==6 sub_3EE2B dir-scan %s dir=%d target=%s (165C2+1D124 count==1, strict-min HP, wc=%d)\n",
                actor.id, best_occ >= 0 ? "ok" : "none", best_dir,
                best_occ >= 0 ? std::to_string(units_[(size_t)best_occ].id).c_str() : "-",
                wc);
    return best_occ >= 0;
}

// Shared wiring for the sub_40295 loc_40470 jump table k=1..6.
// Entry conditions: the first sub_3E18B reports <= 0 (watch_state <= 0), the gauge is
// above var_4+0x18 (var_4 = 10 at 40295 40419; the sub_19FAB floor of 0x37 keeps this
// open in practice) and [+6Ch] <= 6. On success (ebp != 0) loc_40589 splits di = -2/-1
// by dest == current cell and facing == [63h], and di == 1 runs the loc_405CA sub_15523
// route, the sub_1620A walk and the loc_407F3 sub_3C993 settlement. On failure (ebp == 0)
// di = -1 and second-sub_3E18B (loc_4062D/loc_40664) takes over: k=1..4 run the
// sub_3DF3B radius-30 approach only when watch_state == 0 (watch_state == -1 or a closed
// gauge ends the turn), while k=5/6 always end the turn through the loc_40690 [+6Ch] test.
bool CombatII::battle_ai_dispatch_plus6c(int unit_index, int watch_state) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    auto& ready = units_[(size_t)unit_index];
    if (!ready.active) return false;
    const int state = (int)ready.ai_decision_state;
    if (state < 1 || state > 6) return false;
    // var_18 = trunc((shared_gauge-10)/10); the spread budget is var_18*4 (40295 40421..40437).
    int var18 = ((int)battle_ai_gauge_baseline_ - battle_ai_var4_) / 10;
    if (var18 < 0) var18 = 0;
    const int diff_budget = var18 * 4;
    int b_dx = -1, b_dy = -1, b_face = -1, b_hostile = -1;
    bool branch_ok = false;
    if ((int)battle_ai_gauge_baseline_ > battle_ai_var4_ + 0x18) {
        if (state == 1) {
            // F0648 equivalent: hostile list inside the budget-radius diamond, using the
            // same rule as the k=0 sub_3E65E approximation. The original F0648 is filled by
            // sub_4016A -> sub_1D124 (classes 0x0B/0x0C collect at radius 7/9), which is
            // not ported here. sub_1D124 never holds the own cell, so self is excluded.
            std::vector<int> hostiles;
            const int actor_x = (int)(int8_t)ready.x;
            const int actor_y = (int)(int8_t)ready.y;
            for (int dy = -var18; dy <= var18; ++dy) {
                const int half = var18 - (dy < 0 ? -dy : dy);
                for (int dx = -half; dx <= half; ++dx) {
                    const int h = battle_ai_hostile_unit_at(actor_x + dx, actor_y + dy,
                                                            unit_index);
                    if (h >= 0 && h != unit_index) hostiles.push_back(h);
                }
            }
            branch_ok = battle_ai_tact_3e739(unit_index, 2, var18, diff_budget, hostiles,
                                             b_dx, b_dy, b_face, b_hostile) == 1;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI unit=%d +6Ch==1 tact (sub_3E739 mode=2 sec=%d hostiles=%zu) %s dest=(%d,%d) facing=%d (watch_state=%d pos=(%d,%d))\n",
                        ready.id, var18, hostiles.size(), branch_ok ? "ok" : "fail",
                        b_dx, b_dy, b_face, watch_state, ready.x, ready.y);
        } else if (state == 5) {
            branch_ok = battle_ai_flee_3ef12(unit_index, 3, diff_budget, b_dx, b_dy,
                                             b_face) == 1;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI unit=%d +6Ch==5 flee (sub_3EF12 push 3) %s dest=(%d,%d) facing=%d (watch_state=%d pos=(%d,%d))\n",
                        ready.id, branch_ok ? "ok" : "fail", b_dx, b_dy, b_face,
                        watch_state, ready.x, ready.y);
        } else if (state == 2 || state == 4) {
            branch_ok = battle_ai_jump_approach(unit_index, state, var18, diff_budget,
                                                b_dx, b_dy, b_face, b_hostile);
        } else if (state == 3) {
            branch_ok = battle_ai_jump_3ebb4(unit_index, var18, diff_budget,
                                             b_dx, b_dy, b_face, b_hostile);
        } else { // state == 6
            branch_ok = battle_ai_jump_3ee2b(unit_index, diff_budget,
                                             b_dx, b_dy, b_face, b_hostile);
        }
    } else if (battle_trace_) {
        fprintf(stderr, "COMBAT: AI unit=%d +6Ch==%d skip jump table (shared_gauge=%d <= var_4+0x18; di=-1 second-sub_3E18B)\n",
                ready.id, state, battle_tp());
    }
    if (branch_ok) {
        if (b_dx == (int)(int8_t)ready.x && b_dy == (int)(int8_t)ready.y &&
            b_face == (int)ready.facing) {
            // loc_40589: dest == current cell and facing == [63h] -> di = -2, no move/walk/settle.
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI unit=%d +6Ch==%d di=-2 dest==pos and facing unchanged, no movement (pos=(%d,%d))\n",
                        ready.id, state, ready.x, ready.y);
            return true;
        }
        // Di == 1: sub_15523 route -> sub_1620A walk -> loc_407BF tail -> loc_407F3 settle.
        const int ref_hx = (b_hostile >= 0) ? (int)(int8_t)units_[(size_t)b_hostile].x : -1;
        const int ref_hy = (b_hostile >= 0) ? (int)(int8_t)units_[(size_t)b_hostile].y : -1;
        battle_ai_wc1_branch_walk(unit_index, b_dx, b_dy, b_face, diff_budget, battle_ai_var4_,
                                  ref_hx, ref_hy);
        return true;
    }
    // Failure (ebp == 0) -> di = -1 -> second-sub_3E18B (loc_4062D/loc_40664). A positive
    // watch_state would be the home path, but this call contract keeps it <= 0 here.
    // k=5/6 end the turn; the rest run the sub_3DF3B radius-30 approach only when
    // watch_state == 0, and end the turn at -1.
    if (watch_state == 0 && state <= 4) {
        battle_ai_wc1_radius30_pursuit(unit_index);
        return true;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: warning: AI unit=%d +6Ch==%d branch failed (watch_state=%d): second-sub_3E18B ends turn, no movement (pos=(%d,%d))\n",
                ready.id, state, watch_state, ready.x, ready.y);
    return true;
}

bool CombatII::battle_ai_wc1_walk_to(int unit_index, int actor_id, int actor_x, int actor_y,
                                     int mobility, int dest_x, int dest_y,
                                     int ref_hx, int ref_hy, int attack_facing, bool use_wave) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    // sub_15523 zero step (dest == current): change facing without moving; no trigger.
    if (dest_x == actor_x && dest_y == actor_y) {
        if (attack_facing >= 0 && attack_facing < 4) {
            units_[(size_t)unit_index].facing = (int8_t)attack_facing;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d zero-step facing to %d (sub_15523 returns 2; sub_1620A writes passed facing)\n",
                        actor_id, attack_facing);
            return true;
        }
        return false;
    }
    // sub_40295 loc_405C4 (sub_15523 route) plus loc_407BF (sub_14FA4 truncation).
    // With use_wave == false the di == 1 selection builds its route on the current turn's
    // budget diffusion (var_18*4).
    int var18 = ((int)battle_ai_gauge_baseline_ - battle_ai_var4_) / 10;
    if (var18 < 0) var18 = 0;
    const int budget = (battle_ai_budget_override_ >= 0) ? battle_ai_budget_override_ : var18 * 4;
    std::vector<std::pair<int,int>> walk_path;
    int walk_cost = 0;
    int walk_facing = attack_facing;
    if (!battle_ai_route_walk_path(unit_index, mobility, budget, dest_x, dest_y, use_wave,
                                   ref_hx, ref_hy, walk_facing, walk_path, &walk_cost)) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d no 15523 route to pursuit dest (%d,%d) budget=%d, no movement\n",
                    actor_id, dest_x, dest_y, budget);
        return false;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d walks %zu step(s) to=(%d,%d) toward hostile (%d,%d) facing=%d budget=%d cost=%d (15523 route + 14FA4 truncation)\n",
                actor_id, walk_path.size() - 1, dest_x, dest_y, ref_hx, ref_hy, attack_facing,
                budget, walk_cost);
    if (!battle_ai_play_walk(unit_index, walk_path, walk_facing)) return false;
    battle_after_move(unit_index);
    if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id)
        return false;
    // sub_40295: sub_5401B then sub_17310; 0 ends the turn (terrain death).
    if (battle_terrain_17310(unit_index) == 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: AI unit=%d terrain death, turn ends (sub_17310=0)\n", actor_id);
        return false;
    }
    if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id)
        return false;
    battle_wc1_occupancy_preserve_or_close("WC1 pursuit move");
    return true;
}

bool CombatII::battle_ai_play_walk(int unit_index, const std::vector<std::pair<int,int>>& full_path,
                                   int attack_facing) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (full_path.size() < 2) return true;
    if (should_exit_) return false;
    // sub_1620A writes the facing it is given into [+63h] after walking the path; it is
    // not the direction of the last step. WC1 sub_1D124 scans a single [+63h] cell, so the
    // next turn's test depends on this. Use attack_facing when it is >= 0 and fall back to
    // the last-step approximation at -1.
    int8_t final_facing = -1;
    if (attack_facing >= 0 && attack_facing < 4) {
        final_facing = (int8_t)attack_facing;
    } else {
        const auto& dest = full_path.back();
        const auto& before_dest = full_path[full_path.size() - 2];
        final_facing = walk_facing_toward(before_dest.first, before_dest.second,
                                         dest.first, dest.second);
    }
    const auto& dest = full_path.back();
    // Every original AI movement call site (sub_40295 loc_407BF, sub_40DFD, sub_41515,
    // sub_43526, sub_438AE) runs `sub_1124A(unit x, y)` then `sub_1620A(unit, facing)`.
    // sub_1620A itself does not move the camera, and the walk-time camera follow
    // (word_EDD70 in sub_15EB5) only engages while the unit sits at screen centre
    // (tile +6), so the camera must be centred first for the screen to track the unit.
    {
        const CombatUnitPlacement& u0 = units_[(size_t)unit_index];
        camera_center_on((int)(int8_t)u0.x, (int)(int8_t)u0.y); // sub_1124A
    }
    if (should_exit_) return false;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    // Convert to the sub_13AB1 stack shape: pushed destination-first and popped from the
    // top, so the stack is full_path[1..] (current tile excluded) reversed.
    std::vector<g2move::PathNode> stack;
    stack.reserve(full_path.size() - 1);
    for (size_t i = full_path.size(); i-- > 1;)
        stack.push_back({ (uint8_t)full_path[i].first, (uint8_t)full_path[i].second });
    battle_walk_nodes(unit_index, std::move(stack), (int)final_facing);                 // sub_1620A
    if (should_exit_) return false;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    u.x = dest.first;
    u.y = dest.second;
    if (final_facing >= 0) u.facing = final_facing;
    reset_unit_pose(u);                                                                 // sub_12124 (offset cleared)
    return true;
}

// sub_43CA9 (wc2 classification): class 19h..20h, 26h..28h and 34h -> 0 (sub_43526);
// 21h..25h -> 1 (sub_438AE); anything else -> 2 (done). No side effects.
int CombatII::battle_ai_43ca9(int combat_class) const {
    const int c = combat_class & 0xFF;
    if ((c >= 0x19 && c <= 0x20) || (c >= 0x26 && c <= 0x28) || c == 0x34) return 0;
    if (c >= 0x21 && c <= 0x25) return 1;
    return 2;
}

// sub_43C5D (AI auto-defence): when cap+14 != 0, TP > 150, the guard item is present
// (5A col 0 == 6) and [1Eh]&6 == 0, defend ([1Eh] |= 4), clear [57h]/[7F]/[81h] and
// subtract 0x28 TP. Maps 1-5 never reach TP 150, so this is a no-op there.
void CombatII::battle_ai_43c5d(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    if (battle_class_capflat((int)u.combat_class, 14) == 0) return;
    if (battle_tp() <= 0x96) return;
    // The sub_184A4 col 0 == 6 and [1Eh]&6 gates live inside sub_3C4C1; the camera move (sub_1124A)
    // comes first.
    camera_center_on((int)(int8_t)u.x, (int)(int8_t)u.y); // sub_1124A
    if (!battle_defense_3c4c1(unit_index)) return;          // sub_3C4C1 (`cmp ax,1; jnz` exits)
    if (battle_trace_) fprintf(stderr, "COMBAT: AI unit=%d auto-defense ([14h]|=4, sub_43C5D)\n", u.id);
    u.combat_acc57 = 0; // sub_3C993: [57h]=0
    u.ai_field_7f = 0;  // [7F]=0
    u.ai_field_81 = 0;  // [81]=0
    battle_tp_consume(0x28); // sub_1A0C6(28h)
}

// sub_3C4C1: "Defense!".
//   Gates: sub_184A4([5Ah], col 0) == 6, [1Eh]&2 == 0 and [1Eh]&4 == 0; any hit returns eax = 0.
//   Body: var_4 = [63h]; [63h] = 0; sub_45559; sub_1E460 (shield icon); sub_5E9E8;
//         sub_48612(unit, "Defense!", 0x11); sub_1033B(0x20) + sub_499E4; [63h] = var_4;
//         [14h] |= 4; sub_45559; eax = 1.
bool CombatII::battle_defense_3c4c1(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (battle_lookup_184a4(units_[(size_t)unit_index].combat_type_5a, 0) != 6) return false;
    if ((units_[(size_t)unit_index].combat_flags_1e & 0x06) != 0) return false;
    const int8_t saved_facing = units_[(size_t)unit_index].facing;   // var_4 = [63h]
    units_[(size_t)unit_index].facing = 0;                            // [63h] = 0
    present_cutscene_frame();                                         // sub_45559
    // The sub_1E460 shield icon is part of the label background snapshot (battle_unit_label
    // draw_shield).
    battle_unit_label(unit_index, "Defense!", true);                  // sub_1E460 + sub_5E9E8 + sub_48612
    if (should_exit_ || (size_t)unit_index >= units_.size()) return true;
    game_timer_set(0x20);                                             // sub_1033B(20h)
    wait_game_tick();                                                 // sub_499E4
    units_[(size_t)unit_index].facing = saved_facing;                 // [63h] = var_4
    units_[(size_t)unit_index].combat_flags_14 =
        (uint8_t)(units_[(size_t)unit_index].combat_flags_14 | 0x04); // [14h] |= 4
    present_cutscene_frame();                                         // sub_45559
    return true;
}

// sub_43CF0 (class special from sub_44913: A1/5C/5D/5B). Returns true when the turn is
// consumed (sub_44913 loc_44A0C). The target list is battle_target_list_ (the F0648
// equivalent) and the attack uses battle_resolve_list (sub_1DDF0 dx = 1 plus
// sub_1E61D/sub_1EF5C/sub_1ED45).
// The sub_1A452/sub_1A552 rect iterator is flattened to a row-major rectangle because
// sub_43CF0 only uses facing 0 with d > 0 (top-left origin, count cells along +x, then
// +y). sub_1A1F8 is the sub_11DB2 guard plus the sub_1201D hostility test (inversion
// included).
// Presentation: 56BD4/57F18/22AF9 and the F13C8/F13D8/F13E4 spell limbs are wired to
// their ported drivers (2A75A/29722); gameplay (targets, damage, state, sub_197AD) is
// equivalent.
bool CombatII::battle_ai_class_special_43cf0(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return true;
    if (map_w_ == 0 || map_h_ == 0) return true;
    const int cls = (int)units_[(size_t)unit_index].combat_class;
    // sub_1A1F8: append the occupant when the sub_11DB2 guard passes and sub_1201D reports
    // hostility (duplicates ignored).
    auto add_by_army = [&](int x, int y) -> int {
        const int oi = battle_ai_hostile_unit_at(x, y, unit_index);
        if (oi < 0) return 0;
        for (int v : battle_target_list_) if (v == oi) return 0;
        battle_target_list_.push_back(oi);
        return 1;
    };
    // sub_1A452(3,0,3) plus sub_11DB2/sub_1201D: count hostiles in a 3x3 block (duplicates count).
    auto count3 = [&](int cx, int cy) -> int {
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (battle_ai_hostile_unit_at(cx + dx, cy + dy, unit_index) >= 0) ++n;
        return n;
    };
    // sub_1AC08/sub_1AC59 radius-7 diamond collection, shared by 5D and 5B.
    auto collect_diamond7 = [&](int ax, int ay) {
        ai_diamond_each(ax, ay, 7, [&](int x, int y, int) { add_by_army(x, y); });
    };
    const int ax = (int)(int8_t)units_[(size_t)unit_index].x;
    const int ay = (int)(int8_t)units_[(size_t)unit_index].y;

    if (cls == 0xA1) {
        // sub_1A1DE(dir 0): scan the 9 cells southward from the front cell (y+1) and take the
        // strict maximum 3x3 hostile count (first wins).
        const int fx = ax, fy = ay + 1;
        int best = 0, bx = 0, by = 0;
        for (int k = 0; k < 9; ++k) {
            const int n = count3(fx, fy + k);
            if (n > best) { best = n; bx = fx; by = fy + k; }
        }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d class=A1 43CF0 best=%d at (%d,%d) (south ray r9)\n",
                    units_[(size_t)unit_index].id, best, bx, by);
        if (best <= 0) return true; // no candidate - sub_44913 loc_44A0C
        battle_target_list_.clear();
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) add_by_army(bx + dx, by + dy);
        battle_atk_tx_ = bx; battle_atk_ty_ = by;                          // sub_46E19
        camera_center_on(fx + ((bx - fx) >> 1), fy + ((by - fy) >> 1));    // sub_46DF1 midpoint -> sub_1124A
        const int r = battle_resolve_list(unit_index, true, nullptr);               // sub_1DDF0 dx=1
        if (r == -2) return true;                                                  // `cmp eax,-2; jz` (skip the presentation)
        for (int t : battle_target_list_) battle_clear_ailment_flags(t, -1); // sub_1E2B1
        return true;
    }
    if (cls == 0x5C) {
        // Clear F0648, then add the single cell (x, y+1) and the 3x6 block at (x-1, y+2) (sub_1A452
        // count 3, r 6).
        battle_target_list_.clear();
        add_by_army(ax, ay + 1);
        for (int row = 0; row < 6; ++row)
            for (int col = 0; col < 3; ++col) add_by_army(ax - 1 + col, ay + 2 + row);
        if (battle_target_list_.empty()) {
            // loc_43F54: F13C8 = sub_2B822 -> sub_2A75A(kind 0, k=1).
            battle_spell_name_ = kSpellNames[19];                // off_74288 (Meteor k=1)
            battle_item_2a75a(unit_index, 0, 1);
            if (should_exit_) return true;
            battle_item_2a75a_fx(unit_index, 0, 1, ax, ay);
            return true;
        }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d class=5C 43CF0 targets=%zu (front+3x6)\n",
                    units_[(size_t)unit_index].id, battle_target_list_.size());
        battle_resolve_list(unit_index, true, nullptr); // 1DDF0 + 1E61D/1EF5C/1ED45
        return true;
    }
    if (cls == 0x5D) {
        battle_target_list_.clear();
        collect_diamond7(ax, ay);
        if (battle_target_list_.empty()) return true;
        const int camx = (int)cam_tile_x_ + 6, camy = (int)cam_tile_y_ + 6;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d class=5D 43CF0 targets=%zu (diamond r7)\n",
                    units_[(size_t)unit_index].id, battle_target_list_.size());
        battle_resolve_list(unit_index, true, nullptr);                 // 1DDF0
        camera_center_on(ax, ay);                              // 1124A(actor)
        battle_unit_label(unit_index, "Magma Fire!");            // 48612
        // sub_43CF0 loc_43FDB: sub_56BD4(eax=x, edx=y) after the damage and label.
        battle_fx_56bd4(unit_index, ax, ay, 0);                 // sub_56BD4 (hellfire)
        if (should_exit_) return true;
        camera_center_on(camx, camy);                          // 1124A(cam+6)
        return true;
    }
    // 5B (no other class reaches here; sub_44913 dispatches only A1/5C/5D/5B).
    const int spr = (int)units_[(size_t)unit_index].sprite_type;
    if (spr == 0x184) {
        if (combat_rand(3) != 0) return true; // sub_499BD(3) != 0
        battle_target_list_.clear();
        collect_diamond7(ax, ay);
        camera_center_on(ax, ay);
        if (battle_target_list_.empty()) {
            // loc_4410F: F13E4 = sub_2B8F3 -> sub_2A75A(kind 1, k=1).
            battle_spell_name_ = kSpellNames[40];                // off_742DC (Star Tearer k=1)
            battle_item_2a75a(unit_index, 1, 1);
            if (should_exit_) return true;
            battle_item_2a75a_fx(unit_index, 1, 1, ax, ay);
            return true;
        }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d spr=184 43CF0 targets=%zu (Sea Blizzard)\n",
                    units_[(size_t)unit_index].id, battle_target_list_.size());
        battle_resolve_list(unit_index, true, nullptr);                 // 1DDF0
        units_[(size_t)unit_index].anim_frame = 6;             // [64h]=6
        battle_unit_label(unit_index, "Sea Blizzard");           // 48612
        // sub_43CF0 loc_440BA: sub_57F18(eax=unit) after the damage and label.
        battle_fx_57f18(unit_index, ax, ay, 0);                // sub_57F18 (blizzard)
        if (should_exit_) return true;
        units_[(size_t)unit_index].anim_frame = 0;             // [64h]=0 (before sub_1ED45)
        return true;
    }
    if (spr >= 0x185 && spr <= 0x187) {
        if (combat_rand(3) != 0) return true;                  // sub_499BD(3) != 0
        units_[(size_t)unit_index].anim_frame = 5;             // [64h]=5
        for (int v = 0; v < 2; ++v) {
            // sub_45559 (sub_454B3 + sub_5E9E8) then sub_499DA(0x10).
            present_cutscene_frame();
            if (should_exit_) return true;
            game_timer_set(0x10);
            units_[(size_t)unit_index].anim_frame = (uint8_t)(v + 6); // [64h]=6,7
        }
        camera_center_on(ax, ay);
        // F13D8 = sub_2B86C: i-fist.ssp + sub_29722(row 1, k=2). The handler owns the
        // damage and the 29437 confuse status; the Sea Mist fallback below runs only when
        // sub_29722 returns -1 (no target).
        battle_spell_name_ = kSpellNames[32];                    // off_742BC (Diamond Fist k=2)
        bool wave_hit = false;
        const int wave_rc = battle_spell_wave(unit_index, 1, 2, ax, ay, true, &wave_hit);
        if (should_exit_) return true;
        if (!wave_hit || wave_rc != 0) {
            battle_unit_label(unit_index, "Sea Mist");           // 48612
            // sub_43CF0 loc_44238: F13D8 returned -1, so play 22AF9(x, y, 2) before the chain.
            battle_fx_22af9(unit_index, ax, ay, 2);             // sub_22AF9 (icetypho)
            if (should_exit_) return true;
            // CC968[0] chain (approximated by effective army, self included) confuse:
            // c = clamp(0x1E + lvl_a - lvl_t, 10, 0x46); rand(100) < c sets [1E] = (&F8)|2,
            // [1F] = 0x64 and calls sub_197AD.
            const int actor_eff = battle_effective_army(units_[(size_t)unit_index]);
            const int actor_lvl = (int)units_[(size_t)unit_index].combat_level_2b;
            for (size_t i = 0; i < units_.size(); ++i) {
                auto& t = units_[i];
                if (!t.active || t.hp <= 0) continue;
                if (battle_effective_army(t) != actor_eff) continue;
                int c = 0x1E + actor_lvl - (int)t.combat_level_2b;
                if (c < 10) c = 10;
                else if (c > 0x46) c = 0x46;
                if (combat_rand(100) >= c) continue;           // sub_28947
                if (t.sprite_type >= 0x12C) continue;
                t.combat_flags_1e = (uint8_t)((t.combat_flags_1e & 0xF8) | 2);
                t.ail_1f = 0x64;
                battle_remove_from_ready_queue((int)i);        // sub_197AD
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: 43CF0 Sea Mist confuse unit=%d c=%d [1E]|2 [1F]=100 197AD\n",
                            t.id, c);
            }
        }
        units_[(size_t)unit_index].anim_frame = 0;             // [64h]=0
        return true;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d class=5B spr=%d outside 184..187 -> no action\n",
                units_[(size_t)unit_index].id, spr);
    return true;
}

// sub_40BFA target collection branches:
//   wc 0Bh/0Ch -> sub_4088C forward fan,
//   wc 11h/12h -> sub_409AD diamond,
//   wc 10h     -> diamond of radius 3 around the attacker (sub_1AC08).
// battle_range_collect fills F0648. When the selected target is not in the list the
// original substitutes F0648[0]; an empty collection returns -1.
int CombatII::battle_ai_40bfa_pick_target(int unit_index, int selected) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return selected;
    const int wc = weapon_class(units_[(size_t)unit_index]);
    if (wc != 0x0B && wc != 0x0C && wc != 0x10 && wc != 0x11 && wc != 0x12)
        return selected;                                    // 5/0Fh/13h: the selected tile only
    const int n = battle_range_collect(unit_index);         // sub_1D124(F0648)
    if (n <= 0) return -1;
    for (int v : battle_target_list_)
        if (v == selected) return selected;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d wc=%d 40BFA target=%d -> F0648[0]=%d (%d targets)\n",
                units_[(size_t)unit_index].id, wc, selected, battle_target_list_.front(), n);
    return battle_target_list_.front();
}

// sub_4426D: class A4 (sprite 0x19E clone boss, 0x195..0x19D spell boss).
//   With sprite 0x19E and fewer than two clones of the same army ([+12h] != 0), copy the
//   actor record into a free cell at x-2/x-1 or x+3/x+4 (class 5F, HP 100000,
//   [12h] = id+10, [61h] = 0x182). Cancelled with probability 1/3.
//   When no clone was created, cast the sprite-specific spell and apply sub_1D770 damage
//   to the whole of armies 1/2 (the original loc_40D66). Returns true when the turn is
//   consumed.
bool CombatII::battle_ai_class_a4_4426d(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return true;
    const int boss_army = battle_effective_army(units_[(size_t)unit_index]);
    const int spr = units_[(size_t)unit_index].sprite_type;
    bool clone_made = false;

    if (spr == 0x19E) {
        int clones = 0;
        for (const auto& o : units_) {
            if (o.active && o.item_use_12 != 0 && battle_effective_army(o) == boss_army) ++clones;
        }
        int nx = -1;
        const int bx = (int)(int8_t)units_[(size_t)unit_index].x;
        const int by = (int)(int8_t)units_[(size_t)unit_index].y;
        if (clones < 2) {
            auto free_at = [&](int x, int y) { return battle_unit_index_at_footprint(x, y) < 0; };
            if (!free_at(bx - 1, by)) {
                if (free_at(bx + 3, by) && free_at(bx + 4, by)) nx = bx + 3;
            } else if (!free_at(bx - 2, by)) {
                if (free_at(bx + 3, by) && free_at(bx + 4, by)) nx = bx + 3;
            } else {
                nx = bx - 2;
            }
        }
        if (nx <= 0 || combat_rand(3) == 0) nx = -1;           // loc_44324/rand(3)
        if (nx > 0) {
            const int boss_id = units_[(size_t)unit_index].id;
            CombatUnitPlacement nu = units_[(size_t)unit_index]; // memcpy 0x88
            nu.x = nx; nu.y = by;
            nu.name = "\xBA\xD0\xBD\xC5";                        // "clone" (replaces aNbOb_0)
            nu.combat_class = 0x5F;                             // [11h]=5Fh
            nu.unit_kind_0d = 1;                                // [0Dh]=1
            nu.combat_str_40 = 0x78;                            // [40h]=78h
            nu.combat_mag41 = 0x64;                             // [41h]=64h
            nu.combat_value_42 = (int16_t)(uint16_t)((uint16_t)(nu.combat_value_42 & 0xFF00) | 0x50);
            nu.combat_value_43 = 0x0A;                          // [43h]=0Ah
            nu.combat_value_44 = 0x00F0;                        // [44h]=2*[40h]
            nu.combat_value_46 = (int16_t)kCloneDefense;        // word_71E60
            nu.max_hp = 1000; nu.max_hp_deficit = 0;            // [4Ch]=186A0h (x100)
            nu.hp = 1000; nu.hp_deficit = 0;                    // [48h]=[4Ch]
            nu.combat_mp_max_52 = 0; nu.combat_mp_50 = 0;       // [52h]/[50h]=0
            nu.battle_gauge = 0;                                // [56h]=0
            nu.combat_acc57 = 0;                                // [57h]=0
            nu.combat_flags_13 = 0;                             // [13h]=0
            nu.combat_flags_1e = 0;                             // [1Eh]=0
            nu.ai_watch_sel = 0xFF;                             // [71h]=FFh
            nu.ai_target_unit = -1;                             // [73h]=0
            nu.attacking = false;                               // [6Bh]=0
            const int v54 = (int)(uint8_t)(nu.combat_value_42 & 0xFF) + (int)kCloneSpeed54;
            nu.combat_value_54 = (int16_t)v54;                  // [54h]
            nu.combat_flags_14 = nu.combat_flags_13;            // [14h]=[13h]
            nu.combat_flags_15 = nu.combat_flags_1e;            // [15h]=[1Eh]
            nu.ai_home_x = nu.ai_watch_sel;                     // [6Fh]=[71h]
            for (int i = 0; i < 6; ++i) nu.spell_charge_2e[i] = 0; // [2Eh..33h]=0
            nu.ai_field_6e = 0;                                 // [6Eh]=0
            nu.sprite_type = 0x182;                             // [61h]=182h
            nu.anim_frame = 0;                                  // [64h]=0
            nu.stored_77.reset();                               // [77h]=0
            nu.ai_field_81 = 0;                                 // [81h]=0
            nu.ai_mode = -1;                                    // [82h]=FFh
            nu.item_use_12 = (uint8_t)(boss_id + 10);           // [12h]=[0Fh]+0Ah
            nu.ai_field_6d = nu.ai_field_6e;                    // [6Dh]=[6Eh]
            nu.ai_decision_state = nu.ai_field_6e;              // [6Ch]=[6Eh]
            nu.facing = (int8_t)nu.anim_frame;                  // [63h]=[64h]
            nu.ai_target_unit = -1;                             // [67h]=[77h]=0
            nu.ai_field_7f = nu.ai_field_81;                    // [7Fh]=[81h]
            nu.ai_field_80 = (uint8_t)nu.ai_mode;               // [80h]=[82h]
            nu.slot_hint = nu.sprite_type & 0xFF;               // approximation of sub_1198D([61h])
            nu.list_seq = battle_next_list_seq();               // sub_1134F
            nu.active = true;
            nu.occupancy_hp_known = true;
            units_.push_back(nu);
            battle_wc1_occupancy_preserve_or_close("A4 clone");
            camera_center_on(nx, by);                           // sub_1124A
            present_cutscene_frame();                           // sub_45559
            clone_made = true;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: A4 clone unit=%d id=%d at=(%d,%d) clones=%d (sub_4426D)\n",
                        boss_id, units_.back().id, nx, by, clones + 1);
        }
    }

    if (clone_made) return true;

    // ---- Spell selection (sprite 0x195 jump table) and damage to armies 1/2 ----
    const char* spell = "none";
    switch (spr) {
        case 0x195: spell = "f-meteo2"; break;
        case 0x196: spell = "b-hole/bolt3"; break;
        case 0x197: spell = "heal/cure"; break;
        case 0x198: spell = "promi"; break;
        case 0x199: spell = "thunder3/bolt1"; break;
        case 0x19A: spell = "2C863"; break;
        case 0x19B: spell = "sun"; break;
        case 0x19C: spell = "atomic/darkzone"; break;
        case 0x19D: spell = "f-blast/m-wind"; break;
        case 0x19E: spell = (combat_rand(3) == 0) ? "thunder3" : "atomic"; break;
        default: break;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: A4 spell unit=%d spr=%d -> %s, sweep armies 1/2 (sub_4426D)\n",
                units_[(size_t)unit_index].id, spr, spell);

    const int boss_id = units_[(size_t)unit_index].id;
    for (int army = 1; army <= 2 && !should_exit_; ++army) {
        std::vector<int> targets;
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& o = units_[i];
            if (!o.active || o.hp <= 0) continue;
            if (battle_effective_army(o) != army) continue;
            targets.push_back((int)i);
        }
        for (int t : targets) {
            if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size()) break;
            if (t < 0 || (size_t)t >= units_.size()) continue;
            auto& tgt = units_[(size_t)t];
            if (!tgt.active || tgt.hp <= 0) continue;
            const int dmg = battle_damage(unit_index, t, army, 0, 0); // sub_1D770(ebx=army, ecx=0)
            const int new_raw = battle_hp_raw(tgt) - dmg;
            const int new_hp = new_raw <= 0 ? 0 : (new_raw + 99) / 100;
            const int delta = new_hp - tgt.hp;
            tgt.hp_deficit = new_hp > 0 ? new_hp * 100 - new_raw : 0;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: A4 damage boss=%d target=%d army=%d dmg=%d hp->%d\n",
                        boss_id, tgt.id, army, dmg, new_hp);
            if (delta != 0) {
                VmEvent hp; hp.type = VmEventType::Hp; hp.hp_unit = t;
                hp.hp_delta = delta; hp.hp_dir = units_[(size_t)unit_index].facing;
                hp.hp_killer = unit_index;
                play_hp_change(hp);
            }
            if (should_exit_) break;
            if (t < (int)units_.size() && units_[(size_t)t].active && units_[(size_t)t].hp > 0)
                run_unit_hp_trigger(t, unit_index, 1);          // sub_53797
        }
    }
    if (unit_index >= 0 && (size_t)unit_index < units_.size())
        units_[(size_t)unit_index].anim_frame = 0;               // [64h]=0
    present_cutscene_frame();                                   // sub_45559
    return true;
}

// sub_41515 (sub_44913 -> sub_43C5D, the low-HP retreat test that follows).
// Returns true for eax = 1 (skip the normal AI and consume the turn), false for eax = 0.
// - Entry guard: sprite_type >= 0x12C or [+6E] == 4 -> 0.
// - hp_pct = [48h] / ([4Ch]/100) by idiv; the original divides by zero when
//   [4Ch]/100 == 0, so the port returns 0. hp_pct > low[+6E] -> 0.
// - Loop (loc_41575): hp_pct <= high -> di = 1 (loc_4168C, no walk). Otherwise
//   (high < hp_pct <= low) attempt a retreat: budget = ((gauge-si)/10)*4 (sub_14300),
//   the sub_14032/sub_143F3 diffusion, then sub_3EF12 (radius 3) dest/facing.
//   Failure (< 0) -> di = 1. Success -> sub_15523 + sub_1620A walk (BFS here) followed
//   by the settlement (S>>2)*10 + si, with S = budget - E8F46[arrival] and sub_3C993
//   clearing [57h]/[7F]/[81h], then si = sub_167C9 (= 10 + 30n) and sub_5401B/sub_17310
//   inside the walk; the loop resumes at loc_416C0 -> loc_41575 while gauge > si+0x18 and
//   otherwise ends.
// - di = 1 exit (loc_41691): when gauge > 0x28, centre the camera (sub_1124A) and heal
//   through sub_4D3BA.
// - On exit [57h] = 0, [81h] = 0 and [7Fh] = 0 (loc_416D6).
// Maps 1-5 start every first turn at full HP, so this never fires there.
bool CombatII::battle_ai_41515_retreat(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    {
        const CombatUnitPlacement& ready = units_[(size_t)unit_index];
        if (!ready.active) return false;
        if (ready.sprite_type >= 0x12C) return false;
    }
    const int ge = (int)units_[(size_t)unit_index].ai_field_6e;
    if (ge == 4 || ge < 0 || ge > 3) return false;
    const int hp_raw = battle_hp_raw(units_[(size_t)unit_index]);
    const int max_raw = battle_max_hp_raw(units_[(size_t)unit_index]);
    if (max_raw <= 0) return false;
    const int denom = max_raw / 100; // `mov ebx,64h; idiv ebx`
    if (denom <= 0) return false;
    const int hp_pct = hp_raw / denom; // `[48h]/([4Ch]/100)`
    static constexpr int kLow[4]  = { 50, 80, 30, 100 }; // dword_3C97B low word
    static constexpr int kHigh[4] = { 30, 50, 30, 80 };  // dword_3C97B high word
    if (hp_pct > kLow[ge]) return false;        // An area trigger (battle_after_move) can reallocate units_, so no reference is
        // Carried across a call; look the unit up again by index at each point.
    auto unit_id = [&]() -> int {
        if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
        return units_[(size_t)unit_index].id;
    };
    auto unit_pos = [&](int& x, int& y) {
        if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
        x = (int)(int8_t)units_[(size_t)unit_index].x;
        y = (int)(int8_t)units_[(size_t)unit_index].y;
        return true;
    };
    int si = 10;      // esi = 0Ah (budget gauge offset, refreshed by sub_167C9 after each walk)
    bool di1 = false; // edi
    for (;;) {
        if (hp_pct <= kHigh[ge]) { di1 = true; break; } // loc_4168C
        const int gauge = battle_tp();
        const int budget = ((gauge - si) / 10) * 4; // sub_14300 (idiv truncation)
        int fx = -1, fy = -1, ff = -1;
        const int fr = battle_ai_flee_3ef12(unit_index, 3, budget, fx, fy, ff);
        if (fr < 0) { di1 = true; break; } // sub_3EF12 eax < 0 -> loc_4168C
        const int actor_id = unit_id();
        const int before_tp = battle_tp();
        int before_x = -1, before_y = -1;
        unit_pos(before_x, before_y);
        // sub_15523 route + sub_1620A walk + sub_12124/sub_11E04/sub_45559,
        // settlement sub_3C993, then sub_167C9/sub_13F8A and sub_5401B/sub_17310
        // (the same walk-tail approximation as the WC1 branch walk).
        if (!battle_ai_wc1_branch_walk(unit_index, fx, fy, ff, budget, si, -1, -1)) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: warning: AI unit=%d sub_41515 retreat walk failed (no path/terrain death), turn consumed (pos=(%d,%d) budget=%d si=%d)\n",
                        actor_id, before_x, before_y, budget, si);
            break;
        }
        if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
            !units_[(size_t)unit_index].active)
            return true;
        si = battle_move_again_threshold(unit_index); // sub_167C9
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d sub_41515 retreat walk done (dest=(%d,%d) facing=%d gauge=%d si=%d)\n",
                    actor_id, fx, fy, ff, battle_tp(), si);
        if (battle_tp() > si + 0x18) {
            int now_x = -1, now_y = -1;
            unit_pos(now_x, now_y);
            if (battle_tp() == before_tp && now_x == before_x && now_y == before_y) {
                // The original would loop again with an unchanged gauge and position; this
                // guard is unreachable because sub_3E3BF clears hostiles up to r3, so the
                // r4 clear guarantees dest != current cell.
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: AI unit=%d sub_41515 loop no-progress guard (gauge=%d si=%d), ends turn\n",
                            actor_id, battle_tp(), si);
                break;
            }
            continue; // loc_416C0 -> loc_41575
        }
        break; // loc_416D6 (gauge <= si+0x18)
    }
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return true;
    if (di1) {
        // loc_41691: when gauge > 0x28, centre the camera and heal through sub_4D3BA.
        if (battle_tp() > 0x28) {
            camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                             (int)(int8_t)units_[(size_t)unit_index].y); // sub_1124A
            battle_wait_heal(unit_index);                                 // sub_4D3BA
        }
    }
    // loc_416D6: [57h] = 0, [81h] = 0, [7Fh] = [81h].
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return true;
    CombatUnitPlacement& ready = units_[(size_t)unit_index];
    ready.combat_acc57 = 0;
    ready.ai_field_81 = 0;
    ready.ai_field_7f = 0;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d sub_41515 retreat done (hp_raw=%d max_raw=%d hp_pct=%d +6E=%d sprite=%d di=%d gauge=%d), clearing [+57]/[+7F]/[+81], skipping normal AI\n",
                ready.id, hp_raw, max_raw, hp_pct, ge, ready.sprite_type, di1 ? 1 : 0,
                battle_tp());
    return true;
}

bool CombatII::battle_ai_step(int unit_index) {
    // sub_44913 resets byte_E518D on every exit; keep that lifetime even across early returns.
    battle_hostility_inverted_ = false;
    struct HostilityReset {
        bool& value;
        ~HostilityReset() { value = false; }
    } hostility_reset{battle_hostility_inverted_};

    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    CombatUnitPlacement& ready = units_[(size_t)unit_index];
    if (!ready.active) return false;

    // sub_44913: flags [+1E]&0x10 or [+14]&2 enable a 1-in-3 inversion roll before gauge setup.
    // The port RNG is deterministic and differs from DOS rand(), but the condition and scope match.
    if ((ready.combat_flags_1e & 0x10) != 0 || (ready.combat_flags_14 & 0x02) != 0)
        battle_hostility_inverted_ = (combat_rand(3) == 0);

    ready.battle_gauge = 0; // sub_44913 clears [+56] before dispatch, including unsupported AI branches
    battle_prepare_ai_gauge_baseline(ready); // sub_44913 -> sub_19FAB before AI dispatch
    battle_ai_43c5d(unit_index); // sub_44913 -> sub_43C5D (auto-defence; no-op on maps 1-5)
    // sub_44913 runs the sub_41515 retreat check after sub_43C5D. A true result makes
    // `test ax,ax; jnz loc_44A0C` skip the normal AI (sub_40295/sub_40DFD/sub_43Cx).
    // See battle_ai_41515_retreat for details.
    if (battle_ai_41515_retreat(unit_index)) return true;
    // This shared word feeds sub_4016A's sub_1A0E9() gate and sub_40295's route budget.
    const int wc = weapon_class(ready);
    // sub_44913 dispatch, class first and then wc:
    //   [+11h] A1/5C/5D/5B -> sub_43CF0, A4 -> sub_4426D.
    //   wc == 2 -> sub_43CA9/sub_43526/sub_438AE (the mage AI in combatii_aimage.cpp).
    //   wc in the sub_40DFD set {5,B,C,F,10,11,12,13} -> sub_40DFD. The shared body
    //   covers the var_8 map (wc0xB 9, wc0xC 3, wc0x12/0x13 5, else 7) and the same
    //   table arms as wc5: k=0/1 (sub_3E65E -> per-wc sub_3CA3C), k=2/5 (sub_3EADD),
    //   k=4 (sub_3ED51), k=3 (sub_3EBB4 wc >= 6) and k=6 (sub_3EE2B, sub_1D124 all wc).
    //   Everything else (0,1,3,4,6,7,8,9,10,14,...) -> sub_40295: wc1 (0B/0C) in detail
    //   and the rest as an approximate pursuit.
    {
        const int cls = (int)ready.combat_class;
        if (cls == 0xA1 || cls == 0x5C || cls == 0x5D || cls == 0x5B) {
            // sub_43CF0: target collection, sub_1DDF0(dx=1) list attack and state changes.
            battle_ai_class_special_43cf0(unit_index);
            return true; // sub_44913 loc_44A0C (turn consumed)
        }
        if (cls == 0xA4) {
            battle_ai_class_a4_4426d(unit_index);                // sub_4426D
            return true;
        }
        if (wc == 2) {
            // sub_44913: sub_43CA9 -> 0 sub_43526, 1 sub_438AE, 2 done (combatii_aimage.cpp).
            const int sub = battle_ai_43ca9(cls);
            if (battle_trace_) fprintf(stderr, "COMBAT: AI unit=%d class=%d wc=2 43CA9=%d (%s)\n",
                                        ready.id, cls, sub, sub == 0 ? "43526" : sub == 1 ? "438AE" : "none");
            if (sub == 0 || sub == 1) battle_ai_mage_turn(unit_index, sub);
            return true;
        }
        // The sub_40DFD wc set falls through to the block below; non-wc5 units share the
        // same body. The sub_40295 wcs (0,1,3,4,...) also fall through: wc1 in detail and
        // the rest as an approximation.
    }

    if (ai_uses_class40dfd_path(wc)) {
        // sub_40DFD path, shared by wc 0xB/C/F/10..13 as well as wc5.
        // The [+6Ch]==0 hostile selection is sub_3E65E plus the per-wc sub_3CA3C arm:
        // wc5 runs the 3CC91 chain (sub_1AEF3/1CB53/1B7D4/1C685/1CBB1, strict maximum
        // per facing, first wins ties, outer score + (0x5A - dist) + 4 on a facing match),
        // wc0xB/C use the loc_3CF86 band, wc0xF loc_3CFFC, and so on. sub_1A1F8 has no
        // line-of-sight test, so units never block sight here.
        // The radius is the sub_3E65E diamond radius Q + var_8 (Manhattan), with
        // Q = (gauge-10)/10 and the loc_40E48..40EA7 var_8 map (wc5 class 0x15/0x16 = 5,
        // else 7; wc0xB = 9; wc0xC = 3; wc0x12/0x13 = 5; else 7). The budget is
        // recomputed as var_18*4 on every iteration of the loc_40EC4 outer loop.
        // The [+6Ch] table (sub_40DFD off_40DE1) dispatches k=0/1 -> sub_3E65E(Q+var_8),
        // k=2/5 -> sub_3EADD, k=4 -> sub_3ED51, k=3 -> sub_3EBB4 (wc5 through sub_3D3E5
        // loc_3D6E8, wc >= 6 through sub_3CA3C + [+46h]) and k=6 -> sub_3EE2B (sub_1D124
        // wc5 LOS, wcB,C line_scan; on failure loc_41504 sub_3F415(edx=0) burns 40 and
        // ends the turn). Every sub_1D124 arm is ported except wc0xA/0x14+, which do
        // nothing in the original either.
        // The pre-table early selection for k=3 (sub_3F184 / sub_3EBB4 with
        // r = var_4+var_8 and edi = 2) and the sub_3E18B state path are not ported, so
        // only the table call is wired: sub_3E18B reports <= 0 in the R1-D measurement, so
        // the r14 table call dominates the final choice. With var_4 == 0 (guard kind 0)
        // the original skips that early block, so its absence matches the original.
        // wc5/0xF still take the sub_3EF12 (push 3/1) retreat selection first (4106B..410AC).
        // The loc_411A0 di decision after selection is ported: di = -2 only when
        // dest == current cell and af == [+63h], otherwise di = 1. di == 1 runs the
        // loc_413B4 distance gate for a warp (3 <= dist <= var_4, where var_4 is the first
        // byte of unk_731B4[class*15]: 1 -> 5, 2 -> 7, and 5 for wc5 classes such as 53;
        // non-wc5 stays 0, so it always walks) or the walk. The walk uses the whole
        // sub_1620A path without budget truncation, ending facing af, then settles
        // (S>>2)*10 + var_24 with S = var_18*4 - E8[arrival], calls sub_167C9 (var_24 =
        // 10 + 30n) and sub_5401B.
        // The attack loop (40FB9 + 414B7..414E1 + sub_40BFA) requires gauge >= 0x28 and
        // [+6D] != 4, spends 40 through sub_3C993 per shot and repeats while
        // gauge >= var_C (starting at 10, +4 each round). di == 1 moves first
        // (loc_413B4: warp or walk plus settlement). On selection failure loc_411D8 ends
        // through sub_3F415 when [+6Ch] >= 5, and otherwise charges [+57h] += 0x14 when
        // wc == 5, di == -1, gauge == [+54h] and [+57h] < 0x1F4, then ends the turn.
        const char* const ai_tag = (wc == 5) ? "wc5" : "40dfd";
        int attack_gauge = (int)battle_ai_gauge_baseline_;
        int attack_var_c = 10; // sub_40DFD entry: var_C=10, var_24=10
        int attack_var_24 = 10; // refreshed by sub_167C9 after a walk (10 + 30n)
        // sub_40DFD var_4 (guard kind): first byte of unk_731B4[class*15], 1 -> 5, 2 -> 7,
        // else 0. wc5 classes take 5 or 7 from their guard kind (class 53 is 1 -> 5).
        int warp_max_dist = 0;
        {
            const int guard_kind = (int)class_stat_guard_kind(ready.combat_class);
            if (guard_kind == 1) warp_max_dist = 5;
            else if (guard_kind == 2) warp_max_dist = 7;
        }
        int sel_dx = -1, sel_dy = -1, sel_af = -1; // dest/af chosen this iteration
        bool sel_is_chain = false; // true for a sub_3E65E/sub_3EADD/sub_3ED51 pick (loc_411A0)
        bool sel_di2 = false;      // sub_3F184/sub_3EBB4 early or sub_3F247 state path (di = 2)
        const unsigned k6c = ready.ai_decision_state; // [+6Ch]
        if (k6c == 6 && battle_trace_) {
            fprintf(stderr, "COMBAT: AI %s unit=%d +6Ch==6 sub_3EE2B table arm (1D124 wc arms)\n",
                    ai_tag, ready.id);
        }
        bool k6_fail_end = false; // k=6 selection failure ends at loc_41504 (sub_3F415 edx=0)
        bool table_selected = false;  // original ebp != 0 (excludes the nearest fallback)
        int table_metric = 0x7FFE;    // word_F1BA4
        auto find_ranged_target = [&]() {
            sel_dx = sel_dy = sel_af = -1;
            sel_is_chain = false;
            sel_di2 = false;
            table_selected = false;
            table_metric = 0x7FFE;
            if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0) return -1;
            int var_18 = (attack_gauge - 10) / 10;
            if (var_18 < 0) var_18 = 0;
            int hostile = -1, dx = -1, dy = -1, af = -1, score = -1;
            const int scan_radius = battle_40dfd_scan_radius(attack_gauge, wc, ready.combat_class);
            // sub_40DFD off_40DE1: k 0/1 -> sub_3E65E, k 2/5 -> sub_3EADD, k 4 -> sub_3ED51.
            // k=2/5/4 take the same arguments as sub_3E65E: radius Q+var_8, budget Q*4.
            bool selected = false;
            if (k6c == 6) {
                // Table k=6 -> sub_3EE2B(actor, &dest, &af). On success dest is the current
                // cell and af the winning direction, which feeds the loc_411A0 di decision
                // (di = -2 when dest == current cell and af == [63h], else 1). On failure
                // (ebp == 0) the path runs loc_411D8 (no charge because k >= 5) ->
                // loc_4121E -> loc_41265 -> loc_4129C (k == 6) -> loc_41504
                // (sub_3F415 edx = 0), burning 40 when gauge > 0x28 before ending.
                // The sub_3E18B > 0 state path (sub_3F247) is still assumed unreachable, as
                // on the wc5 path. Every sub_1D124 arm is ported, so this works for any wc.
                if (battle_ai_jump_3ee2b(unit_index, var_18 * 4, dx, dy, af, hostile)) {
                    selected = true;
                } else {
                    // sub_3F415(u, edx = 0): applies the [6Dh] != 4 defence flag, TP cost and wait
                    // recovery.
                    battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                    battle_ai_turn_end_3f415(unit_index, 0);
                    attack_gauge = (int)battle_ai_gauge_baseline_;
                    k6_fail_end = true;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: warning: AI %s unit=%d +6Ch==6 sub_3EE2B failed: sub_3F415 charge (gauge=%d), turn ends\n",
                                ai_tag, ready.id, attack_gauge);
                }
            } else if (k6c == 2 || k6c == 5) {
                selected = battle_ai_jump_approach(unit_index, 2, scan_radius, var_18 * 4,
                                                   dx, dy, af, hostile);
            } else if (k6c == 4) {
                selected = battle_ai_jump_approach(unit_index, 4, scan_radius, var_18 * 4,
                                                   dx, dy, af, hostile);
            } else if (k6c == 3) {
                // Table k=3 -> sub_3EBB4 (off_40DE1[3]). The original's pre-table early
                // selection (sub_3F184 / sub_3EBB4 with r = var_4+var_8, edi = 2) is
                // computed earlier in the loc_40EC4 block.
                selected = battle_ai_jump_3ebb4(unit_index, scan_radius, var_18 * 4,
                                                dx, dy, af, hostile, &table_metric);
            } else {
                selected = battle_ai_3e65e_select(unit_index, var_18 * 4,
                                                  scan_radius, hostile,
                                                  dx, dy, af, score);
            }
            table_selected = selected;
            if (selected) {
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: AI %s class=%u unit=%d +6Ch=%u hostile=%d dest=(%d,%d) facing=%d score=%d budget=%d radius=%d (sub_%s)\n",
                            ai_tag, (unsigned)ready.combat_class, units_[(size_t)unit_index].id, k6c,
                            hostile >= 0 && (size_t)hostile < units_.size()
                                ? units_[(size_t)hostile].id
                                : -1,
                            dx, dy, af, score, var_18 * 4, scan_radius,
                            (k6c == 2 || k6c == 5) ? "3EADD" : (k6c == 4 ? "3ED51" : (k6c == 3 ? "3EBB4" : (k6c == 6 ? "3EE2B" : "3E65E"))));
                sel_dx = dx;
                sel_dy = dy;
                sel_af = af;
                sel_is_chain = true;
                return hostile;
            }
            // Fallback: instead of the original moved/40BFA failure path after ebp == 0,
            // take the nearest unit by Manhattan distance (unreachable on maps 1-5).
            const auto& actor = units_[(size_t)unit_index];
            int candidate = -1, best_distance = std::numeric_limits<int>::max();
            for (size_t i = 0; i < units_.size(); ++i) {
                const auto& other = units_[i];
                if (!other.active || other.hp <= 0 ||
                    !battle_armies_hostile(battle_effective_army(actor), battle_effective_army(other),
                                           battle_hostility_inverted_)) continue;
                const int distance = std::abs(other.x - actor.x) + std::abs(other.y - actor.y);
                const int ray_range = std::max(std::abs(other.x - actor.x), std::abs(other.y - actor.y));
                if (distance <= 0 || ray_range > scan_radius || distance >= best_distance)
                    continue;
                candidate = (int)i;
                best_distance = distance;
            }
            return candidate;
        };

        // sub_40DFD attack loop (40FB9 + 414B7..414E1 + sub_40BFA): one dispatch may attack
        // several times while the gauge allows it. Each iteration runs the sub_3E65E
        // selection, the loc_411A0 di decision, a loc_413B4 move when di == 1 (warp or walk
        // plus settlement) and then a sub_40BFA shot. sub_40BFA fires once when
        // gauge >= 0x28 and [+6D] != 4, and subtracts 40 from the shared gauge through
        // sub_3C993 -> sub_1A0C6. The loop repeats while gauge >= var_C (wc5 starts at 10
        // and adds 4 per round), which gives exactly two shots for map 1-5 Dark Assassin
        // (baseline 85: 85 -> 45 -> 5).
        // loc_40FB9 (gauge <= var_24+0x18 -> di = -3 -> sub_3F415, then end) is equivalent
        // to break here: sub_3F415 has no observable effect on maps 1-5 (healing at full HP
        // is 0), and the trailing 40BFA(-1) it would reach has no effect either because
        // [+6D] == 0.
        int ranged_attacks = 0;
        for (int attack_iter = 0; attack_iter < 10; ++attack_iter) {
            // Units_ can be reallocated during an attack, so look it up again each iteration.
            if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                return true;
            if (attack_gauge <= attack_var_24 + 0x18) break; // loc_40FB9
            // sub_40BFA gate: sub_1A0E9() >= 0x28 and [+6D] != 4.
            if (attack_gauge < 0x28 || units_[(size_t)unit_index].ai_field_6d == 4) break;
            // sub_40DFD loc_4106B..410AC: with var_4 == 0 (guard kind 0) and wc in {5,0xF},
            // pick the sub_3EF12 retreat target (push 3 for wc5, else 1) before the table.
            // Success (1) sets di = 1 with that dest/facing and skips the table. After the
            // move, sub_40BFA(actor, var_3C = 0) returns -1, so a gauge below var_C ends
            // through sub_3F415(edx = 0); otherwise var_C += 4 and loc_40EC4 selects again.
            //
            // ---- sub_40DFD early block 40EC4..40FB4 ----
            // With var_4 != 0 and gauge > 0x28: sub_14300(Q*4) -> sub_3F0E4(r = var_4,
            // band 2) -> sub_14300(-1) -> sub_3E18B. When watch_state == 1, sub_3F184 runs
            // with r = var_1C = var_4+var_8; otherwise, with [+6Ch] == 3, sub_3EBB4 runs on
            // the same E8 grid with the same r. If a target exists and the actor-to-dest
            // distance exceeds 2, di = 2 skips sub_3E5AB and the table, and loc_413B4 uses
            // the sub_3BF00 warp plus 40 regardless of distance. EDD72 stays Q*4 during
            // sub_3F0E4 and is restored to -1 before selection (original 40F2A); the
            // sub_3F247 state path uses the same Q*4.
            bool early_di2 = false;
            int early_target = -1, early_dx = -1, early_dy = -1, early_af = -1;
            int early_metric = 0x7FFE;
            const int q_early = (attack_gauge > 10) ? (attack_gauge - 10) / 10 : 0;
            const int map_radius_early = (wc == 5)
                ? ((ready.combat_class == 0x15 || ready.combat_class == 0x16) ? 5 : 7)
                : (wc == 0x0B ? 9 : (wc == 0x0C ? 3 : ((wc == 0x12 || wc == 0x13) ? 5 : 7)));
            const int watch_state = battle_ai_watch_state(unit_index); // sub_3E18B
            if (warp_max_dist != 0 && attack_gauge > 0x28) {
                std::vector<int> early_grid;
                battle_ai_3f0e4(unit_index, early_grid, warp_max_dist, 2,
                                q_early * 4);                       // sub_3F0E4
                int t = -1, ex = -1, ey = -1, eaf = -1;
                if (watch_state == 1) {
                    t = battle_ai_3f184(unit_index,
                                        warp_max_dist + map_radius_early,
                                        early_grid, -1, ex, ey, eaf); // sub_3F184
                } else if (k6c == 3) {
                    int occ = -1;
                    if (battle_ai_3ebb4_grid(unit_index,
                                             warp_max_dist + map_radius_early,
                                             early_grid, -1,
                                             ex, ey, eaf, occ,
                                             &early_metric)) {      // sub_3EBB4
                        t = occ;
                    }
                }
                if (t >= 0) {
                    const int ex0 = (int)(int8_t)units_[(size_t)unit_index].x;
                    const int ey0 = (int)(int8_t)units_[(size_t)unit_index].y;
                    if (std::abs(ex - ex0) + std::abs(ey - ey0) > 2) { // sub_49ADF
                        early_di2 = true;
                        early_target = t;
                        early_dx = ex; early_dy = ey; early_af = eaf;
                    }
                }
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: AI %s unit=%d 40EC4 early 3F0E4/3EBB4 watch=%d Q=%d r=%d band=2 -> %s target=%d dest=(%d,%d) af=%d metric=%d\n",
                            ai_tag, ready.id, watch_state, q_early, warp_max_dist,
                            early_di2 ? "di=2" : "none", early_target,
                            early_dx, early_dy, early_af, early_metric);
            }
            bool flee_move = false;
            if (!early_di2 && warp_max_dist == 0 && (wc == 5 || wc == 0x0F)) {
                int fx = -1, fy = -1, ff = -1;
                int flee_q = (attack_gauge - 10) / 10;
                if (flee_q < 0) flee_q = 0;
                if (battle_ai_flee_3ef12(unit_index, (wc == 5) ? 3 : 1, flee_q * 4,
                                         fx, fy, ff) == 1) {
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI %s unit=%d +6Ch=%u sub_3EF12 pre-table retreat select (push %d) ok dest=(%d,%d) facing=%d\n",
                                ai_tag, units_[(size_t)unit_index].id, k6c, (wc == 5) ? 3 : 1,
                                fx, fy, ff);
                    sel_dx = fx;
                    sel_dy = fy;
                    sel_af = ff;
                    sel_is_chain = true;
                    flee_move = true;
                }
            }
            int target = -1;
            bool state_warp = false;
            if (!flee_move) {
                if (early_di2 && watch_state > 0) {
                    // loc_40FC4: with sub_3E18B > 0 and di == 2, var_3C = var_40 and the
                    // sub_3E5AB/table path is skipped.
                    target = early_target;
                    sel_dx = early_dx; sel_dy = early_dy; sel_af = early_af;
                    sel_is_chain = true;
                    sel_di2 = true;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI %s unit=%d early di=2 bypasses 3E5AB/table (watch=%d)\n",
                                ai_tag, ready.id, watch_state);
                } else {
                    target = find_ranged_target();
                    // 41139..41157: compare the k=3 table result (sub_3EBB4) against the
                    // early pick using word_F1BA4 (score or defence). A strictly smaller
                    // value keeps the early pick: at 4113F `jge` takes the table and `jl`
                    // keeps early.
                    if (early_di2 && k6c == 3 &&
                        (!table_selected || early_metric < table_metric)) {
                        target = early_target;
                        sel_dx = early_dx; sel_dy = early_dy; sel_af = early_af;
                        sel_is_chain = true;
                        sel_di2 = true;
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI %s unit=%d early di=2 wins over table 3EBB4 (early=%d table=%d)\n",
                                    ai_tag, ready.id, early_metric, table_metric);
                    }
                    // 4121E: with di == -1, var_4 != 0 and sub_3E18B > 0, run sub_3F247
                    // (r = var_4, EDD72 = Q*4). Success sets di = 2 and af = -1 and leaves
                    // var_3C at 0, so sub_40BFA returns -1: the warp spends 40 and goes to
                    // the 414E1 timer without attacking.
                    if (target < 0 && !sel_di2 && warp_max_dist != 0 && watch_state > 0) {
                        int sx = -1, sy = -1;
                        if (battle_ai_3f247(unit_index, warp_max_dist, q_early * 4, sx, sy)) {
                            target = -1;
                            sel_dx = sx; sel_dy = sy; sel_af = -1;
                            sel_is_chain = true;
                            sel_di2 = true;
                            state_warp = true;
                            if (battle_trace_)
                                fprintf(stderr, "COMBAT: AI %s unit=%d 4121E state 3F247 ok dest=(%d,%d) af=-1 (no 40BFA target)\n",
                                        ai_tag, ready.id, sx, sy);
                        }
                    }
                    if (target < 0 && !state_warp) {
                        // loc_411D8: when the table selection fails (ebp == 0, di == -1) and
                        // [+6Ch] >= 5, end through 4121E..41504 (sub_3F415). Otherwise, with
                        // k < 5, wc == 5, gauge == [+54h] and [+57h] < 0x1F4, charge
                        // [+57h] += 0x14 and end the turn at 4150D.
                        auto& u8 = units_[(size_t)unit_index];
                        if (k6c < 5 && wc == 5 && u8.combat_acc57 < 0x1F4 &&
                            (int)u8.combat_value_54 == attack_gauge) {
                            u8.combat_acc57 = (uint16_t)(u8.combat_acc57 + 0x14);
                            if (battle_trace_)
                                fprintf(stderr, "COMBAT: warning: AI %s unit=%d selection failed: loc_411D8 [+57h]+=20 (gauge=%d [+54h]=%d acc=%u), turn ends (no sub_3F415)\n",
                                        ai_tag, u8.id, attack_gauge, (int)u8.combat_value_54,
                                        (unsigned)u8.combat_acc57);
                            return true;
                        }
                        break;
                    }
                }
            }
            const int attacker_id = units_[(size_t)unit_index].id;
            const int actor_x = units_[(size_t)unit_index].x;
            const int actor_y = units_[(size_t)unit_index].y;
            const int actor_facing = units_[(size_t)unit_index].facing & 3;
            // loc_411A0: di = -2 (fire in place) only when dest == current cell and
            // af == [+63h], otherwise di = 1. A non-chain selection (ebp == 0 here) also
            // fires in place. The sub_3EF12 retreat is the exception: the original forces
            // di = 1 there (410B6). The sub_3F184/sub_3EBB4 early and sub_3F247 state paths
            // force di = 2.
            const int di = sel_di2
                               ? 2
                               : flee_move
                                     ? 1
                                     : (!sel_is_chain || (sel_dx == actor_x && sel_dy == actor_y &&
                                                          sel_af >= 0 && sel_af < 4 &&
                                                          sel_af == actor_facing))
                                           ? -2
                                           : 1;
            if (battle_trace_ && di == 2)
                fprintf(stderr, "COMBAT: AI %s unit=%d di=2 dest=(%d,%d) af=%d state_warp=%d (sub_411A0/early)\n",
                        ai_tag, attacker_id, sel_dx, sel_dy, sel_af, state_warp ? 1 : 0);
            if (battle_trace_ && di == 1)
                fprintf(stderr, "COMBAT: AI %s unit=%d di=1 dest=(%d,%d) af=%d (sub_411A0; dest!=pos or facing mismatch)\n",
                        ai_tag, attacker_id, sel_dx, sel_dy, sel_af);
            if (di == 1 || di == 2) {
                // sub_40DFD loc_413B4: when di > 0, sub_1124A centres the camera on the
                // actor before the sub_3BF00/sub_1620A move, so the walk stays on screen.
                camera_center_on(actor_x, actor_y);
                const int move_dist = std::abs(sel_dx - actor_x) + std::abs(sel_dy - actor_y); // sub_49ADF
                if (di == 2 || (move_dist >= 3 && move_dist <= warp_max_dist)) {
                    // loc_413F6 warp arm: di == 2 (early/state path) skips the distance
                    // gate. Even with dest == current cell the original calls sub_3BF00 and
                    // spends 40 (the port only skips the warp call). Facing is written from
                    // sel_af when it is 0..3 (413F6..4142B).
                    if (sel_dx != actor_x || sel_dy != actor_y) {
                        if (!battle_warp_unit(unit_index, sel_dx, sel_dy)) break;
                        if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                            !units_[(size_t)unit_index].active ||
                            units_[(size_t)unit_index].id != attacker_id)
                            return true;
                    }
                    attack_gauge -= 0x28;
                    if (attack_gauge < 0) attack_gauge = 0;
                    battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                    if (sel_af >= 0 && sel_af < 4)
                        units_[(size_t)unit_index].facing = (int8_t)sel_af;
                    if (state_warp) {
                        // 414B7: sub_40BFA(actor, edx = var_3C = 0) == -1 goes to 414E1.
                        // Below var_C it ends at 41504 (sub_3F415 edx = 0); otherwise
                        // var_C += 4 and the loop returns to loc_40EC4 to select again
                        // without attacking.
                        if (attack_gauge < attack_var_c) {
                            auto& u9 = units_[(size_t)unit_index];
                            if (u9.sprite_type < 0x12C) {
                                if (attack_gauge > 0x28) {
                                    attack_gauge -= 0x28;
                                    u9.combat_flags_13 = (uint8_t)(u9.combat_flags_13 | 1);
                                }
                                battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                                if (attack_gauge > 0x14) battle_wait_heal(unit_index);
                            }
                            if (battle_trace_)
                                fprintf(stderr, "COMBAT: AI %s unit=%d state 3F247 warp then 40BFA=-1, gauge=%d < var_C=%d -> sub_3F415, turn ends\n",
                                        ai_tag, attacker_id, attack_gauge, attack_var_c);
                            return true;
                        }
                        attack_var_c += 4;
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI %s unit=%d state 3F247 warp re-loop (gauge=%d var_C=%d)\n",
                                    ai_tag, attacker_id, attack_gauge, attack_var_c);
                        continue;
                    }
                } else {
                    // loc_4142F walk arm: sub_1620A walks the whole restored path without
                    // budget truncation and writes the ending facing af into [+63h]; with
                    // dest == pos it is a zero-step turn.
                    int var_18_now = (attack_gauge - 10) / 10; // 40EC4 Q (idiv trunc)
                    if (var_18_now < 0) var_18_now = 0;
                    // Decision diffusion for the S settlement (sub_14FA4 re-spread, actor = 4Q),
                    // built before moving.
                    const std::vector<int> walk_grid =
                        battle_ai_build_diffusion(unit_index, var_18_now * 4);
                    if (walk_grid.empty()) break;
                    if (sel_dx == actor_x && sel_dy == actor_y) {
                        // Zero-step turn: only the facing changes; the 4142F settlement and
                        // triggers still run. S = 4Q - E8[self] = 0 leaves the charge at
                        // var_24 (10), matching the emulated zero step.
                        if (sel_af >= 0 && sel_af < 4)
                            units_[(size_t)unit_index].facing = (int8_t)sel_af;
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI %s unit=%d zero-step facing to %d (sub_15523 returns 2; sub_1620A writes passed facing)\n",
                                    ai_tag, attacker_id, sel_af);
                    } else {
                        std::vector<std::pair<int,int>> steps;
                        const int mobility =
                            battle_ai_mobility_selector(units_[(size_t)unit_index]);
                        if (!battle_ai_find_legal_path(unit_index, mobility, sel_dx, sel_dy,
                                                       steps))
                            break; // fail closed (the DOS sub_15523 fallthrough never fires on maps 1-5)
                        std::vector<std::pair<int,int>> full_path;
                        full_path.reserve(steps.size() + 1);
                        full_path.emplace_back(actor_x, actor_y);
                        full_path.insert(full_path.end(), steps.begin(), steps.end());
                        if (!battle_ai_play_walk(unit_index, full_path, sel_af)) break;
                        if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                            !units_[(size_t)unit_index].active ||
                            units_[(size_t)unit_index].id != attacker_id)
                            return true;
                    }
                    battle_after_move(unit_index); // sub_5401B + region
                    if (should_exit_ || unit_index < 0 ||
                        (size_t)unit_index >= units_.size() ||
                        !units_[(size_t)unit_index].active ||
                        units_[(size_t)unit_index].id != attacker_id)
                        return true;
                    // sub_40DFD: sub_5401B then sub_17310; 0 ends the turn (terrain death).
                    if (battle_terrain_17310(unit_index) == 0) {
                        if (battle_trace_) fprintf(stderr, "COMBAT: AI unit=%d terrain death, turn ends (sub_17310=0)\n", attacker_id);
                        return true;
                    }
                    if (should_exit_ || unit_index < 0 ||
                        (size_t)unit_index >= units_.size() ||
                        !units_[(size_t)unit_index].active ||
                        units_[(size_t)unit_index].id != attacker_id)
                        return true;
                    battle_wc1_occupancy_preserve_or_close("AI class-53 walk");
                    // loc_4142F settlement: S = var_18*4 - E8[arrival] and the charge is
                    // (S>>2)*10 + the low word of var_24. The [var_28+2]>>16 overlap read is
                    // that same low word.
                    // Only E8 >= 0 destinations reach here, so S >= 0 on the pre-move
                    // actor-rooted spread. battle_ai_build_diffusion bakes the sub_3CA3C head
                    // refresh (budget+1) into the actor cell, so subtract 1 there to match
                    // the sub_14FA4 re-spread (actor = var_18*4). A zero-step turn then gives
                    // S = 0 and a charge of 10.
                    int walk_charge = attack_var_24;
                    {
                        const int nx = units_[(size_t)unit_index].x;
                        const int ny = units_[(size_t)unit_index].y;
                        const size_t cell = (size_t)ny * map_w_ + (size_t)nx;
                        if (nx < 0 || ny < 0 || nx >= (int)map_w_ || ny >= (int)map_h_ ||
                            cell >= walk_grid.size() || walk_grid[cell] < 0)
                            break; // fail closed (never fires on maps 1-5)
                        int e8_at_dest = walk_grid[cell];
                        if (nx == actor_x && ny == actor_y) e8_at_dest -= 1;
                        const int s_value = var_18_now * 4 - e8_at_dest;
                        walk_charge = ((s_value >> 2) * 10) + (attack_var_24 & 0xFFFF);
                    }
                    attack_gauge -= walk_charge;
                    if (attack_gauge < 0) attack_gauge = 0;
                    battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI %s unit=%d walk settle charge=%d gauge=%d (sub_4142F (S>>2)*10+var_24)\n",
                                ai_tag, attacker_id, walk_charge, attack_gauge);
                    // sub_167C9: n hostiles in the radius-1 diamond (self plus 4 neighbours)
                    // gives var_24 = 10 + 30n, using the internal sub_11DB2 gate and the
                    // sub_1201D hostility test.
                    {
                        const auto& actor = units_[(size_t)unit_index];
                        const int actor_eff = battle_effective_army(actor);
                        static constexpr int kAdjX[5] = { 0, 0, -1, 0, 1 }; // self+S,W,N,E
                        static constexpr int kAdjY[5] = { 0, 1, 0, -1, 0 };
                        int hostile_count = 0;
                        for (int dir = 0; dir < 5; ++dir) {
                            const int cx = (int)(int8_t)actor.x + kAdjX[dir];
                            const int cy = (int)(int8_t)actor.y + kAdjY[dir];
                            if (cx < 2 || cx >= (int)map_w_ - 2 || cy < 4 ||
                                cy >= (int)map_h_ - 4)
                                continue;
                            const int occupant = battle_unit_index_at(cx, cy);
                            if (occupant < 0) continue;
                            const auto& other = units_[(size_t)occupant];
                            if (battle_armies_hostile(battle_effective_army(other),
                                                      actor_eff, battle_hostility_inverted_))
                                ++hostile_count;
                        }
                        attack_var_24 = 10 + 30 * hostile_count;
                    }
                }
                // loc_414B7: re-gate sub_40BFA after the move; the walk settlement may have dropped
                // the gauge.
                if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                    !units_[(size_t)unit_index].active ||
                    units_[(size_t)unit_index].hp <= 0)
                    return true;
                if (attack_gauge < 0x28 || units_[(size_t)unit_index].ai_field_6d == 4) break;
            }
            if (flee_move) {
                // Original 414B7..414FB: sub_40BFA(actor, var_3C = 0) returns -1 (loc_40DD1).
                // Below var_C it ends at loc_41504 (sub_3F415 edx = 0); otherwise
                // var_C += 4 and the loop returns to loc_40EC4, where sub_3EF12 has
                // priority every round.
                if (attack_gauge < attack_var_c) {
                    battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                    battle_ai_turn_end_3f415(unit_index, 0); // loc_41504 (defence flag, TP cost, wait recovery)
                    attack_gauge = (int)battle_ai_gauge_baseline_;
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI %s unit=%d flee-move then sub_3F415 (gauge=%d < var_C=%d), turn ends\n",
                                ai_tag, attacker_id, attack_gauge, attack_var_c);
                    return true;
                }
                attack_var_c += 4;
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: AI %s unit=%d flee-move re-loop (gauge=%d var_C=%d)\n",
                            ai_tag, attacker_id, attack_gauge, attack_var_c);
                continue;
            }
            const int attack_target = battle_ai_40bfa_pick_target(unit_index, target);
            if (attack_target < 0 || (size_t)attack_target >= units_.size()) break;
            const int target_id = units_[(size_t)attack_target].id;
            const int distance = std::abs(units_[(size_t)attack_target].x - units_[(size_t)unit_index].x) +
                                 std::abs(units_[(size_t)attack_target].y - units_[(size_t)unit_index].y);
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI %s unit=%d ranged attack target=%d distance=%d (iter=%d gauge=%d var_C=%d)\n",
                        ai_tag, attacker_id, target_id, distance, attack_iter, attack_gauge, attack_var_c);
            // battle_resolve_attack can run the event VM (deaths) and change units_. A
            // validation failure (false) stops without spending, like the original; a real
            // execution subtracts 40.
            const bool executed = battle_resolve_attack(unit_index, attack_target, true);
            if (should_exit_) return true;
            if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                return true;
            if (!executed) break;
            ++ranged_attacks;
            attack_gauge -= 0x28; // sub_3C993(edx = 0x28) -> sub_1A0C6 (floor 0)
            if (attack_gauge < 0) attack_gauge = 0;
            battle_ai_gauge_baseline_ = (int16_t)attack_gauge; // update the shared word
            if (attack_gauge < attack_var_c) break; // `cmp ax,cx; jl 41504`
            attack_var_c += 4; // `mov edi,ecx; add edi,4`
        }
        if (k6_fail_end) return true; // loc_41504 -> loc_4150D already applied sub_3F415
        if (ranged_attacks > 0) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI %s unit=%d ranged loop done attacks=%d gauge=%d var_C=%d\n",
                        ai_tag, units_[(size_t)unit_index].id, ranged_attacks, attack_gauge, attack_var_c);
            battle_ai_turn_end_3f415(unit_index, 0); // loc_41504: sub_3F415(edx = 0) after the attack loop
            return true;
        }
        // With zero shots because there was no target, fall through to the approach. Zero
        // shots from the gauge gate end here instead, since the original cannot prepare a
        // move either (it needs gauge > 0x28).
        if (unit_index < 0 || (size_t)unit_index >= units_.size()) return true;
        if (attack_gauge < 0x28 || units_[(size_t)unit_index].ai_field_6d == 4) return true;
        // The approach below (warp or walk) continues with the same attack_gauge/attack_var_c.

        // No legal shot: close to the nearest hostile. The original uses sub_3E65E's path result;
        // here choose an unoccupied in-bounds tile beside that hostile, then use its verified
        // <=5-tile Warp branch (sub_40DFD -> sub_3BF00) when it is close enough.
        int nearest = -1, nearest_distance = std::numeric_limits<int>::max();
        {
            const auto& actor = units_[(size_t)unit_index];
            for (size_t i = 0; i < units_.size(); ++i) {
                const auto& other = units_[i];
                if (!other.active || other.hp <= 0 ||
                    !battle_armies_hostile(battle_effective_army(actor), battle_effective_army(other),
                                           battle_hostility_inverted_)) continue;
                const int distance = std::abs(other.x - actor.x) + std::abs(other.y - actor.y);
                if (distance < nearest_distance) { nearest = (int)i; nearest_distance = distance; }
            }
        }
        if (nearest < 0) {
            if (battle_trace_) fprintf(stderr, "COMBAT: AI %s unit=%d has no living hostile target\n", ai_tag, ready.id);
            // No walk or Warp occurred, so the original has no sub_5401B movement-trigger call
            // here.
            return true;
        }
        if (nearest_distance == 1) {
            const int8_t facing = walk_facing_toward(units_[(size_t)unit_index].x,
                                                     units_[(size_t)unit_index].y,
                                                     units_[(size_t)nearest].x,
                                                     units_[(size_t)nearest].y);
            if (facing >= 0) units_[(size_t)unit_index].facing = facing;
            // Attack through the same gauge loop as the sub_40DFD di == 2 assault. Being
            // adjacent costs nothing extra; the sub_3BF00 cost only applies to the warp arm.
            for (int attack_iter = 0; attack_iter < 10; ++attack_iter) {
                if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                    !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                    return true;
                if (attack_gauge < 0x28 || units_[(size_t)unit_index].ai_field_6d == 4) break;
                // Re-target: when the current one dies, move to the next adjacent hostile.
                int adj = -1;
                {
                    const auto& actor = units_[(size_t)unit_index];
                    for (size_t i = 0; i < units_.size(); ++i) {
                        const auto& other = units_[i];
                        if (!other.active || other.hp <= 0 ||
                            !battle_armies_hostile(battle_effective_army(actor),
                                                   battle_effective_army(other),
                                                   battle_hostility_inverted_)) continue;
                        if (std::abs(other.x - actor.x) + std::abs(other.y - actor.y) != 1) continue;
                        adj = (int)i;
                        break;
                    }
                }
                if (adj < 0) break;
                if (!battle_resolve_attack(unit_index, adj)) break;
                if (should_exit_) return true;
                if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                    !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                    return true;
                attack_gauge -= 0x28;
                if (attack_gauge < 0) attack_gauge = 0;
                battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                if (attack_gauge < attack_var_c) break;
                attack_var_c += 4;
            }
            battle_ai_turn_end_3f415(unit_index, 0);
            return true;
        }
        const int actor_x = units_[(size_t)unit_index].x;
        const int actor_y = units_[(size_t)unit_index].y;
        const int enemy_x = units_[(size_t)nearest].x;
        const int enemy_y = units_[(size_t)nearest].y;
        const int actor_id = units_[(size_t)unit_index].id;
        const int enemy_id = units_[(size_t)nearest].id;
        static constexpr int kNeighborX[4] = { 0, -1, 1, 0 };
        static constexpr int kNeighborY[4] = { -1, 0, 0, 1 };
        int destination_x = -1, destination_y = -1;
        int destination_distance = std::numeric_limits<int>::max();
        for (int dir = 0; dir < 4; ++dir) {
            const int nx = enemy_x + kNeighborX[dir], ny = enemy_y + kNeighborY[dir];
            if (nx < 0 || ny < 0 || nx >= (int)map_w_ || ny >= (int)map_h_ ||
                (nx == actor_x && ny == actor_y)) continue;
            const int occupant = battle_unit_index_at(nx, ny);
            if (occupant >= 0 && occupant != unit_index) continue;
            const int distance = std::abs(nx - actor_x) + std::abs(ny - actor_y);
            if (distance < destination_distance) {
                destination_x = nx;
                destination_y = ny;
                destination_distance = distance;
            }
        }
        if (destination_x < 0) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: warning: AI %s unit=%d cannot reach a free tile beside target=%d\n",
                        ai_tag, actor_id, enemy_id);
            return true;
        }

        if (destination_distance <= 5) {
            // sub_40DFD: <= var_4 (5) selects sub_3BF00. Its Warp arm skips sub_5401B.
            // The warp arm also spends 40 through sub_3C993(edx = 0x28), so subtract it before the
            // loop.
            if (!battle_warp_unit(unit_index, destination_x, destination_y)) return true;
            attack_gauge -= 0x28;
            if (attack_gauge < 0) attack_gauge = 0;
            battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
            for (int attack_iter = 0; attack_iter < 10; ++attack_iter) {
                if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                    !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                    return true;
                if (attack_gauge < 0x28 || units_[(size_t)unit_index].ai_field_6d == 4) break;
                int target = find_ranged_target();
                if (target < 0) break;
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: AI %s unit=%d attacks after Warp! target=%d (iter=%d gauge=%d var_C=%d)\n",
                            ai_tag, units_[(size_t)unit_index].id, units_[(size_t)target].id,
                            attack_iter, attack_gauge, attack_var_c);
                if (!battle_resolve_attack(unit_index, target, true)) break;
                if (should_exit_) return true;
                if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                    !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                    return true;
                attack_gauge -= 0x28;
                if (attack_gauge < 0) attack_gauge = 0;
                battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
                if (attack_gauge < attack_var_c) break;
                attack_var_c += 4;
            }
            battle_ai_turn_end_3f415(unit_index, 0);
            return true;
        }

        // Beyond the verified Warp cutoff, approximate sub_1620A with one clear cardinal step.
        // This is the original walk branch, so run its area/region movement triggers.
        static constexpr int kStepX[4] = { 1, -1, 0, 0 };
        static constexpr int kStepY[4] = { 0, 0, 1, -1 };
        int step_x = actor_x, step_y = actor_y;
        int step_distance = destination_distance;
        for (int dir = 0; dir < 4; ++dir) {
            const int nx = actor_x + kStepX[dir], ny = actor_y + kStepY[dir];
            if (nx < 0 || ny < 0 || nx >= (int)map_w_ || ny >= (int)map_h_ ||
                battle_unit_index_at(nx, ny) >= 0) continue;
            const int distance = std::abs(destination_x - nx) + std::abs(destination_y - ny);
            if (distance < step_distance) { step_x = nx; step_y = ny; step_distance = distance; }
        }
        if (step_x == actor_x && step_y == actor_y) return true;
        units_[(size_t)unit_index].facing = walk_facing_toward(actor_x, actor_y, step_x, step_y);
        units_[(size_t)unit_index].x = step_x;
        units_[(size_t)unit_index].y = step_y;
        battle_wc1_occupancy_preserve_or_close("AI unit movement");
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI %s unit=%d walks to=(%d,%d) toward target=%d\n",
                    ai_tag, actor_id, step_x, step_y, enemy_id);
        battle_after_move(unit_index);
        if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
            !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id) return true;
        // The walk cost (the sub_3C993 settlement) is not ported here, so nothing is
        // subtracted; only the attack loop spends gauge. Map 1-5 Dark Assassin always has a
        // target in range, so it never takes this walk branch.
        for (int attack_iter = 0; attack_iter < 10; ++attack_iter) {
            if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                return true;
            if (attack_gauge < 0x28 || units_[(size_t)unit_index].ai_field_6d == 4) break;
            int target = find_ranged_target();
            if (target < 0) break;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI %s unit=%d attacks after walk target=%d (iter=%d gauge=%d var_C=%d)\n",
                        ai_tag, units_[(size_t)unit_index].id, units_[(size_t)target].id,
                        attack_iter, attack_gauge, attack_var_c);
            if (!battle_resolve_attack(unit_index, target, true)) break;
            if (should_exit_) return true;
            if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
                !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0)
                return true;
            attack_gauge -= 0x28;
            if (attack_gauge < 0) attack_gauge = 0;
            battle_ai_gauge_baseline_ = (int16_t)attack_gauge;
            if (attack_gauge < attack_var_c) break;
            attack_var_c += 4;
        }
        battle_ai_turn_end_3f415(unit_index, 0);
        return true;
    }

    // ==== sub_40295 body loop (402FC): sub_4016A (scan, attack, forced weapon, magic), ====
    // move, repeat while TP >= var_10, then sub_3F415 =====
    // di = -2 from sub_4016A, or a sub_5401B/sub_17310 failure, escapes without sub_3F415
    // (loc_40882); other exits go through loc_40876 -> sub_3F415(u, 1).
    // WC1 (0B/0C) only diagnoses without scanning or moving until occupancy is proven.
    const bool wc1_class_step = wc == 1 &&
        (ready.combat_class == 0x0B || ready.combat_class == 0x0C);
    bool allow_scan = true;
    if (wc1_class_step) {
        allow_scan = ready.sprite_type >= 0 && ready.sprite_type < 0x12C &&
                     ready.facing >= 0 && ready.facing <= 3 &&
                     battle_wc1_static_occupancy_unambiguous();
    }
    if (!allow_scan) {
        battle_ai_40295_move(unit_index, wc); // diagnostics only, no movement
        return true;
    }
    // sub_40295 body: the literal port in combatii_ai_approach.cpp (the move stage runs even
    // when sub_4016A >= 0, preserving the di semantics).
    return battle_ai_40295_body(unit_index, wc);
}

// sub_40295 move stage: the branches after sub_4016A returns -1 (sub_3E18B state,
// [+6Ch] jump table, radius-30 approach). WC1 (0B/0C) detail path plus an approximate
// pursuit for other classes. Returns true when handled.
bool CombatII::battle_ai_40295_move(int unit_index, int wc) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    CombatUnitPlacement& ready = units_[(size_t)unit_index];
    const bool wc1_class = wc == 1 &&
        (ready.combat_class == 0x0B || ready.combat_class == 0x0C);
    const bool wc1_scan_inputs_supported = wc1_class && ready.sprite_type >= 0 &&
        ready.sprite_type < 0x12C && ready.facing >= 0 && ready.facing <= 3;
    const bool wc1_static_occupancy_unambiguous = wc1_scan_inputs_supported &&
        battle_wc1_static_occupancy_unambiguous();
    const bool wc1_directional_scan = wc1_scan_inputs_supported && wc1_static_occupancy_unambiguous;
    const bool wc1_scan_unmodeled = wc1_class && !wc1_directional_scan;
    const bool wc1_threat_found = false; // sub_4016A already handled scanning and attacks

    if (wc1_class) {
        // The full sub_40295 state machine is in battle_ai_40295_body; this fallback only
        // runs when the conservative WC1 occupancy gate is withheld.
        const int watch_state = battle_ai_watch_state(unit_index);
        if (wc1_directional_scan) {
            // Original second-sub_3E18B branch (loc_4062D/loc_40664): state > 0 runs the
            // sub_3E251 home path, state == 0 with [+6Ch] == 0 runs the sub_3E65E budget
            // radius search and then the sub_3DF3B radius-30 approach, and state == -1 ends
            // the turn without moving.
            if (battle_trace_) {
                const char* reason = wc1_threat_found
                    ? "WC1 threat found but sub_4016A gauge/[+6D] gate closed"
                    : "no modeled hostile on WC1 directional tile";
                fprintf(stderr, "COMBAT: AI weapon_class=%d unit=%d %s (sub_1D124 one-tile scan; gate-proven unique 1x1 base-tile lookup, CC968/CC998 lifecycle unmodeled; watch_state=%d pos=(%d,%d) shared_gauge=%d hostility_inverted=%d)\n",
                        wc, ready.id, reason, watch_state, ready.x, ready.y, (int)battle_ai_gauge_baseline_,
                        battle_hostility_inverted_ ? 1 : 0);
            }
            const int mobility = battle_ai_mobility_selector(ready);
            if (watch_state > 0) {
            // sub_3E251 home path: the candidate is always computed and only logged under
            // trace. The shortest legal 4-direction path to it is walked in one turn. The
            // diffusion tie-break and tactic evaluation are not ported, so the destination is
            // verified but the intermediate path is approximate.
            int cand_x = -1, cand_y = -1, cand_score = 0, cand_ad2e = 0, cand_dist = 0;
            const char* cand_reject = nullptr;
            const bool has_cand = battle_ai_no_target_home_candidate(unit_index, cand_x, cand_y,
                                                                     cand_score, cand_ad2e, cand_dist,
                                                                     &cand_reject);
            if (battle_trace_) {
                if (has_cand) {
                    const bool is_home = (cand_x == (int)ready.ai_home_x &&
                                           cand_y == (int)ready.ai_home_y);
                    const bool is_stay = (cand_x == (int)(int8_t)ready.x &&
                                          cand_y == (int)(int8_t)ready.y);
                    fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d WC1 no-target home candidate (%d,%d) score=%d ad2e=%d dist=%d home=(%u,%u) mobility=%d%s%s (sub_3E251 radius-9 diamond, sub_16699 1x1/cost approx; path tie-break unported)\n",
                            ready.id, cand_x, cand_y, cand_score, cand_ad2e, cand_dist,
                            (unsigned)ready.ai_home_x, (unsigned)ready.ai_home_y, mobility,
                            is_home ? " IS-HOME" : "", is_stay ? " IS-STAY" : "");
                } else {
                    fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d WC1 no-target home candidate none: %s (home=(%u,%u) mobility=%d watch_state=%d pos=(%d,%d))\n",
                            ready.id, cand_reject ? cand_reject : "unknown",
                            (unsigned)ready.ai_home_x, (unsigned)ready.ai_home_y, mobility,
                            watch_state, ready.x, ready.y);
                }
            }
            if (has_cand) {
                const int actor_x = (int)(int8_t)ready.x;
                const int actor_y = (int)(int8_t)ready.y;
                const int manhattan = std::abs(actor_x - cand_x) + std::abs(actor_y - cand_y);
                if (manhattan == 0) return true; // already on the best cell (sub_40295 returns 0xFFFFFFFE, no move)
                if (manhattan == 1) {
                    // Only the single adjacent step of sub_1620A is performed. The candidate
                    // already passed sub_16699 1x1/cost legality, but occupancy is rechecked
                    // right before moving: self-excluding vector occupancy approximates CC998
                    // and is allowed only while the gate is open.
                    if (!battle_ai_candidate_tile_legal_1x1(cand_x, cand_y, unit_index, mobility)) {
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: warning: AI weapon_class=1 unit=%d adjacent home step (%d,%d) recheck failed, no movement\n",
                                    ready.id, cand_x, cand_y);
                        return true;
                    }
                    const int actor_id = ready.id;
                    const std::vector<std::pair<int,int>> full_path = {
                        { actor_x, actor_y }, { cand_x, cand_y }
                    };
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d walks adjacent to=(%d,%d) home=(%u,%u) score=%d (sub_3E251 candidate, sub_16D3E walk)\n",
                                actor_id, cand_x, cand_y,
                                (unsigned)ready.ai_home_x, (unsigned)ready.ai_home_y, cand_score);
                    std::vector<std::pair<int,int>> walk_path;
                    int walk_cost = 0;
                    if (!battle_ai_truncate_to_move_budget(full_path, mobility, &walk_cost, walk_path)) return true;
                    if (!battle_ai_play_walk(unit_index, walk_path)) return true;
                    // A verified 1x1 adjacent move keeps vector uniqueness, so the latch stays
                    // open. The movement trigger runs after the walk, as at the original call site.
                    battle_after_move(unit_index);
                    if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
                        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id)
                        return true;
                    battle_wc1_occupancy_preserve_or_close("WC1 adjacent move");
                    return true;
                }
                // Dist > 1: walk the shortest legal path to the candidate in one turn. Every
                // cell must satisfy sub_16699 1x1/cost legality and avoid other units. The
                // neighbour order is adaptive (ai_bfs_dir_order, a hypothesis from the DOS
                // id3/id4 stop comparison). The original diffusion (sub_143F3/1499C) restore
                // (sub_15523) and tactics (sub_14FA4/sub_165C2) are not ported, so the order
                // is not confirmed against the original. The trigger runs once after the walk.
                {
                    const int map_w = (int)map_w_, map_h = (int)map_h_;
                    const int cells = map_w * map_h;
                    const int start_idx = actor_y * map_w + actor_x;
                    const int dest_idx = cand_y * map_w + cand_x;
                    std::vector<int> prev(cells, -1);
                    std::vector<int> queue;
                    queue.reserve(256);
                    prev[start_idx] = start_idx;
                    queue.push_back(start_idx);
                    size_t head = 0;
                    bool reached = (start_idx == dest_idx);
                    while (!reached && head < queue.size()) {
                        const int cur = queue[head++];
                        const int cx = cur % map_w, cy = cur / map_w;
                        int dir_x[4], dir_y[4];
                        ai_bfs_dir_order(cx, cy, cand_x, cand_y, dir_x, dir_y);
                        for (int d = 0; d < 4; ++d) {
                            const int nx = cx + dir_x[d], ny = cy + dir_y[d];
                            if (nx < 0 || ny < 0 || nx >= map_w || ny >= map_h) continue;
                            const int nidx = ny * map_w + nx;
                            if (prev[nidx] != -1) continue;
                            if (!battle_ai_candidate_tile_legal_1x1(nx, ny, unit_index, mobility)) continue;
                            prev[nidx] = cur;
                            if (nidx == dest_idx) { reached = true; break; }
                            queue.push_back(nidx);
                        }
                    }
                    if (!reached) {
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d no legal path to home candidate (%d,%d), no movement\n",
                                    ready.id, cand_x, cand_y);
                        return true;
                    }
                    std::vector<std::pair<int,int>> path;
                    for (int cur = dest_idx; cur != start_idx; cur = prev[cur]) {
                        if (cur < 0 || cur >= cells || prev[cur] == -1) { path.clear(); break; }
                        path.push_back({ cur % map_w, cur / map_w });
                        if ((int)path.size() > 64) { path.clear(); break; }
                    }
                    if (path.empty()) {
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: warning: AI weapon_class=1 unit=%d path restore failed to (%d,%d), no movement\n",
                                    ready.id, cand_x, cand_y);
                        return true;
                    }
                    std::reverse(path.begin(), path.end());
                    const int actor_id = ready.id;
                    const unsigned home_x = (unsigned)ready.ai_home_x;
                    const unsigned home_y = (unsigned)ready.ai_home_y;
                    if (battle_trace_) {
                        fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d walks %zu step(s) to=(%d,%d) home=(%u,%u) score=%d (sub_3E251 candidate, sub_16D3E walk)\n",
                                actor_id, path.size(), cand_x, cand_y, home_x, home_y, cand_score);
                    }
                    std::vector<std::pair<int,int>> full_path;
                    full_path.reserve(path.size() + 1);
                    full_path.push_back({ actor_x, actor_y });
                    full_path.insert(full_path.end(), path.begin(), path.end());
                    std::vector<std::pair<int,int>> walk_path;
                    int walk_cost = 0;
                    if (!battle_ai_truncate_to_move_budget(full_path, mobility, &walk_cost, walk_path)) {
                        if (battle_trace_)
                            fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d move budget exhausted before first step, no movement\n",
                                    actor_id);
                        return true;
                    }
                    if (battle_trace_ && walk_path.size() != full_path.size())
                        fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d move budget truncates %zu-step path to %zu step(s) (cost=%d)\n",
                                actor_id, full_path.size() - 1, walk_path.size() - 1, walk_cost);
                    if (!battle_ai_play_walk(unit_index, walk_path)) return true;
                    // Run the movement trigger once after the walk, as at the original call site.
                    battle_after_move(unit_index);
                    if (should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
                        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id)
                        return true;
                    battle_wc1_occupancy_preserve_or_close("WC1 home path move");
                    return true;
                }
            }
            // No candidate: end the turn without moving; this only covers part of the original -1
            // path.
            return true;
            } else if (ready.ai_decision_state == 0) {
                battle_ai_wc1_pursuit(unit_index, watch_state);
            } else if (battle_ai_dispatch_plus6c(unit_index, watch_state)) {
                // K=1..6 jump-table branch (including the internal second-sub_3E18B fallback)
                // handled.
            } else if (battle_trace_) {
                fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d watch_state=%d +6Ch=%u dispatch declined (state outside 1..6), no movement (pos=(%d,%d))\n",
                        ready.id, watch_state, (unsigned)ready.ai_decision_state, ready.x, ready.y);
            }
            return true;
        } else if (wc1_scan_inputs_supported && !wc1_directional_scan) {
            if (battle_trace_) {
                const char* occupancy_reason = !battle_sce_snapshot_complete_
                    ? "complete static SCE snapshot unavailable"
                    : !battle_wc1_occupancy_state_known_
                        ? "occupancy-state latch closed after an unmodeled mutation"
                        : "static occupancy or loaded class-0 .NC2 descriptors not proven";
                fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d WC1 scan withheld: %s; no target scan/movement (pos=(%d,%d) watch_state=%d)\n",
                        ready.id, occupancy_reason, ready.x, ready.y, watch_state);
            }
        } else if (wc1_scan_unmodeled) {
            // The original still calls sub_3F321 first here; the state is computed for diagnostics.
            // The [+6D] == 0/4 paths have no side effects and maps 1-5 hold no other
            // [+6D] value, so this call touches no RNG state or fields there.
            const int st3f321 = battle_ai_3f321_state(ready);
            if (battle_trace_) {
                fprintf(stderr, "COMBAT: AI weapon_class=1 unit=%d WC1 outside conservative state-0/.NC2/facing gate; no target scan/movement (kind=%u +7F=%u +80=%u +81=%u +82=%d +6D=%u type=%d facing=%d hp_raw=%d watch_state=%d st3f321=%d pos=(%d,%d))\n",
                        ready.id, (unsigned)sce_map_kind_, (unsigned)ready.ai_field_7f,
                        (unsigned)ready.ai_field_80, (unsigned)ready.ai_field_81, (int)ready.ai_mode,
                        (unsigned)ready.ai_field_6d, ready.sprite_type, (int)ready.facing,
                        battle_hp_raw(ready), watch_state, st3f321, ready.x, ready.y);
            }
        } else {
            if (battle_trace_) {
                fprintf(stderr, "COMBAT: AI weapon_class=%d unit=%d no port target candidate (effective hostility checked; sub_1D124 tile-pattern scan unported; watch_state=%d pos=(%d,%d) shared_gauge=%d hostility_inverted=%d)\n",
                        wc, ready.id, watch_state, ready.x, ready.y, (int)battle_ai_gauge_baseline_,
                        battle_hostility_inverted_ ? 1 : 0);
            }
        }

        // No-target movement is not ported. Do not synthesize sub_5401B while stationary:
        // the original reaches area/region triggers from actual movement call sites.
        return true;
    }
    // sub_40295 [+6Ch]==0 branch equivalent: approach the hostile inside the budget radius
    // through the decision diffusion and sub_3CA3C (the same range-1 adjacency as WC1). The
    // wc0 sub_3CA3C arm is loc_3CB73 with range 1, identical to WC1, so
    // battle_ai_wc1_pursuit is reused as is: diffusion budget var_18*4, E8 maximum, bucket,
    // sub_165C2, BFS with budget truncation, sub_1620A walk, sub_5401B/sub_17310 and facing.
    // Map 1-7 bats (class 76, wc0) therefore walk the whole budget (for example 20) instead
    // of a single greedy step. watch_state feeds the second-sub_3E18B branch.
    {
        const int watch_state = battle_ai_watch_state(unit_index);
        const auto& uu = units_[(size_t)unit_index];
        // +6Ch != 0: the sub_40295 jump table (k=1..6) is ported. This generic path cannot
        // prove the class-0 .NC2 1x1 occupancy gate, so sub_165C2 stays a 1x1 approximation
        // (logged). Unported arms (wc5, wc6+ sub_3CA3C, sub_3D3E5 wc5) leave dispatch false.
        if (watch_state <= 0 && uu.ai_decision_state >= 1 && uu.ai_decision_state <= 6 &&
            battle_ai_dispatch_plus6c(unit_index, watch_state)) {
            if (battle_trace_ && uu.ai_decision_state != 0)
                fprintf(stderr, "COMBAT: AI weapon_class=%d unit=%d +6Ch=%u dispatched on generic path (165C2 1x1 approximation)\n",
                        wc, uu.id, (unsigned)uu.ai_decision_state);
            return true;
        }
        if (uu.ai_decision_state != 0) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI weapon_class=%d unit=%d +6Ch=%u dispatch declined (state outside 1..6), no movement (pos=(%d,%d) watch=%d)\n",
                        wc, uu.id, (unsigned)uu.ai_decision_state, uu.x, uu.y, watch_state);
            return true;
        }
        battle_ai_wc1_pursuit(unit_index, watch_state);
        return true;
    }
}

// sub_484F9(eax = x, edx = y, ebx = text, ecx = frame, stack = color_arg): one label frame.
//   x -= strlen*3                                (glyph width 5 plus 1 px spacing, so centring)
//   k = frame < 8 ? 8-frame : min(frame-7, 6)    (outline colour; var_4 also updates to the
//                                               8-frame value only while frame < 8)
//   Draw (x-1,y) (x+1,y) (x,y-1) (x,y+1) in colour k with spacing 1 (sub_608E6).
//   Fill: di = var_4 (the 8-frame value just written while frame < 8, otherwise the residue
//         of the previous frame)
//     di > 9 : di += 6; var_4 = di; when di > 0x11, var_4 = color_arg
//     di > 7 : var_4 = di + 0x39
//     else   : var_4 unchanged
//   Draw (x,y) in colour (uint8)var_4.
// sub_48612 calls only sub_484F9 / sub_499E4 / sub_5E9E8, and only sub_484F9 touches the
// var_4 slot ([esp+14h+var_4] inside sub_484F9), so var_4 survives between frames
// (sub_5E9E8 pushes four registers and never reaches that slot). Frames 1..7 therefore use
// the 8-frame value for both outline and fill (7 -> 1), and from frame 8 on the fill is 1
// with the outline min(frame-7, 6) (1 -> 6).
void CombatII::battle_label_draw_frame(int x, int y, const char* text, int frame, int color_arg,
                                       int16_t& label_var4) {
    const int len = (int)strlen(text);
    const int xt = x - len * 3;
    int k;
    if (frame < 8) {
        k = 8 - frame;
        label_var4 = (int16_t)k;
    } else {
        k = frame - 7;
        if (k > 6) k = 6;
    }
    const uint8_t outline = (uint8_t)(k & 0xFF);
    draw_small_text(xt - 1, y, text, outline, 1);
    draw_small_text(xt + 1, y, text, outline, 1);
    draw_small_text(xt, y - 1, text, outline, 1);
    draw_small_text(xt, y + 1, text, outline, 1);
    int di = label_var4;
    if (di > 9) {
        di += 6;
        label_var4 = (int16_t)di;
        if ((int16_t)di > 0x11) label_var4 = (int16_t)color_arg;
    } else if (di > 7) {
        label_var4 = (int16_t)(di + 0x39);
    }
    draw_small_text(xt, y, text, (uint8_t)(label_var4 & 0xFF), 1);
}

// Load ITEM.SPR (sub_18115). A missing file only drops the shield icon.
bool CombatII::load_item_spr() {
    item_spr_tried_ = true;
    item_spr_loaded_ = false;
    std::string cand = game_path("ITEM.SPR");
    if (!file_exists(cand.c_str())) cand = game_path("item.spr");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "ITEM.SPR");
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: ITEM.SPR not found (shield icon skipped)\n");
        return false;
    }
    if (!item_spr_.load(cand) || item_spr_.count() < 1) {
        fprintf(stderr, "COMBAT: warning: ITEM.SPR load failed: %s\n", item_spr_.error().c_str());
        return false;
    }
    // sub_18115 loc_1815A: apply sub_10031 (the 320 -> 480 stride patch) to all 0x7C loaded
    // frames. The equipment screen, drag icons and shield icon all draw through sub_5F3FD
    // (blit_sprite_480); without the patch every row is 160 px off and the icons distort.
    item_spr_.convert_stride_480();
    item_spr_loaded_ = true;
    fprintf(stderr, "COMBAT: ITEM.SPR loaded %d frames (sub_10031 320->480 stride patch)\n",
            item_spr_.count());
    return true;
}

// sub_1E460: shield icon for a successful defence (word_F0430 == 1) or the "Defense!" presentation.
void CombatII::battle_draw_shield_icon(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (u.combat_type_5a == 0) return;
    if (!battle_item_table_tried_) battle_load_item_table();
    if (battle_lookup_184a4(u.combat_type_5a, 0) != 6) return;     // sub_184A4(id, 0) != 6 -> skip
    if (!item_spr_tried_) load_item_spr();
    if (!item_spr_loaded_) return;
    const SpriteEntry* sp = item_spr_.get((int)u.combat_type_5a - 1);   // unk_EFDB0 + (id-1)*12
    if (!sp || !sp->pixel_data) return;
    // Esi = sub_10971(x)+10h+(int8)[65h], ecx = sub_10988(y)+4+(int8)[66h]
    const int esi = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 0x10 + (int)(int8_t)u.walk_off_x;
    const int ecx = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + 4 + (int)(int8_t)u.walk_off_y;
    const int x = esi + sp->x_offset;
    const int y = (int16_t)(sp->y_offset + ecx + 0x18);              // movsx ecx, ax
    blit_sprite_480(x, y, sp->pixel_data);
}

void CombatII::battle_unit_label(int unit_index, const char* text, bool draw_shield) {
    if (!text || unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (!small_font_tried_) load_small_font();
    const CombatUnitPlacement& unit = units_[(size_t)unit_index];
    // sub_10971(x) + 0Ch and sub_10988(y) - 10h, computed once on entry.
    const int label_x = (unit.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int label_y = (unit.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 16;
    // The original paints over the current backbuffer (the previous frame). The port keeps
    // a snapshot, restores it each frame and then draws the label; the glyph positions are
    // identical, so the result matches overpainting.
    render_battle_frame();
    if (draw_shield) battle_draw_shield_icon(unit_index);   // sub_3C4C1 -> sub_1E460 (then sub_5E9E8)
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    const std::vector<uint8_t> background(bb, bb + (size_t)SCREEN_W * SCREEN_H);
    int16_t label_var4 = 0;
    game_timer_set(3);                                           // sub_1033B(3)
    for (int frame = 1; frame <= 36; ++frame) {                  // loc_48652: si = 1..0x24
        if (should_exit_) return;
        bb = video_.get_backbuffer();
        if (!bb) return;
        std::copy(background.begin(), background.end(), bb);
        battle_label_draw_frame(label_x, label_y, text, frame, 0x11, label_var4); // sub_484F9
        wait_game_tick();                                        // sub_499E4
        video_.present();                                        // sub_5E9E8
        dump_frame_if_requested();
    }
}

void CombatII::battle_spell_announce(int unit_index, const char* name) {
    if (!name || !*name || unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (!small_font_tried_) load_small_font();
    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x, (int)(int8_t)units_[(size_t)unit_index].y); // sub_1124A
    if (units_[(size_t)unit_index].sprite_type < 0x12C) {
        units_[(size_t)unit_index].anim_frame = 7;                     // [64h] = 7
        game_timer_set(0x20);                                          // sub_1033B(20h)
        for (int i = 0; i < 2 && !should_exit_; ++i) {
            present_cutscene_frame();                                  // sub_45559
            units_[(size_t)unit_index].anim_frame = (uint8_t)(units_[(size_t)unit_index].anim_frame + 1);
            wait_game_tick();                                          // sub_499E4
        }
    }
    const CombatUnitPlacement& unit = units_[(size_t)unit_index];
    const int label_x = (unit.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + 12;
    const int label_y = (unit.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y - 0x14;
    // The sub_484F9 fill slot (var_4, at esp-24 before the call) keeps its value across
    // frames for frame >= 8. Unlike sub_48612, this loop calls sub_454B3 first every frame,
    // and that chain (sub_454B3 -> sub_454D5 pushes ebx/ecx/edx -> the first push of the
    // final sub_45431 call is ebx) writes the low word of ebx into exactly the same slot.
    // For frame >= 8, di is therefore that ebx value, not last frame's:
    //   - The previous sub_484F9 returns from sub_608E6, which leaves
    //     `mov ebx,[esp-2]; sar ebx,10h` (the 1 px glyph spacing) behind, so ebx = 1.
    //     sub_5E9E8/sub_454B3 preserve ebx, hence di = 1 (<= 7, slot unchanged, fill 1).
    //   - Exception: when dword_FA8B6 (map dimming, sub_44D6E(1)) is set, the first block
    //     of sub_454D5 runs `mov ebx,130h`, so di = 0x130, di+6 > 0x11 and the argument
    //     colour (0x11) becomes the fill.
    // frame < 8 writes the 8-frame value directly, so the residue does not matter there.
    const int16_t slot_residue = battle_dim_row0_ ? (int16_t)0x130 : (int16_t)1;
    int16_t label_var4 = 0;
    game_timer_set(4);                                                 // sub_1033B(4)
    for (int frame = 1; frame <= 36 && !should_exit_; ++frame) {
        render_battle_frame();                                         // sub_454B3
        if (frame >= 8) label_var4 = slot_residue;                     // the sub_45431 push of ebx overwrites the slot
        battle_label_draw_frame(label_x, label_y, name, frame, 0x11, label_var4); // sub_484F9
        video_.present();                                              // sub_5E9E8
        dump_frame_if_requested();
        wait_game_tick();                                              // sub_499E4
    }
}

bool CombatII::battle_warp_unit(int unit_index, int destination_x, int destination_y) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const CombatUnitPlacement& source = units_[(size_t)unit_index];
    if (!source.active || source.hp <= 0 || destination_x < 0 || destination_y < 0 ||
        destination_x >= (int)map_w_ || destination_y >= (int)map_h_ ||
        (destination_x == source.x && destination_y == source.y)) return false;
    const int occupant = battle_unit_index_at(destination_x, destination_y);
    if (occupant >= 0 && occupant != unit_index) return false;

    const int source_x = source.x, source_y = source.y, unit_id = source.id;
    camera_center_on((source_x + destination_x) / 2, (source_y + destination_y) / 2); // sub_46DF1 midpoint

    battle_unit_label(unit_index, "Warp!");                    // sub_48612(unit, "Warp!", 0x11)
    if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0 ||
        units_[(size_t)unit_index].id != unit_id) return false;

    game_timer_set(3);
    units_[(size_t)unit_index].anim_frame = 0;
    for (int frame = 0; frame < 12; ++frame) { // sub_3BF00: advance facing once per tick, 12 ticks total
        units_[(size_t)unit_index].facing = (int8_t)((units_[(size_t)unit_index].facing + 1) & 3);
        present_cutscene_frame();
        wait_game_tick();
    }
    if (unit_index < 0 || (size_t)unit_index >= units_.size() ||
        !units_[(size_t)unit_index].active || units_[(size_t)unit_index].hp <= 0) return false;
    const int new_occupant = battle_unit_index_at(destination_x, destination_y);
    if (new_occupant >= 0 && new_occupant != unit_index) return false;
    units_[(size_t)unit_index].x = destination_x;
    units_[(size_t)unit_index].y = destination_y;
    battle_wc1_occupancy_preserve_or_close("Warp movement");
    // The 12-step rotation is three full turns, so sub_3BF00 leaves the starting facing intact.
    present_cutscene_frame(); // sub_11EE4 -> update coordinates -> sub_11E04 -> redraw
    if (battle_trace_)
        fprintf(stderr, "COMBAT: warp unit=%d (%d,%d)->(%d,%d)\n",
                unit_id, source_x, source_y, destination_x, destination_y);
    return true;
}
