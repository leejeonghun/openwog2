// combatii_ai_approach: AI movement phase - the approach/chase loop (sub_40295) and its
// route helpers, ported from the COMBATII_code.ASM control flow.
// Listing ranges: sub_40295 at 64669..65182, sub_3DF3B at 61343..61429, sub_3E18B at
// 61565..61635.
//
// Stack-slot aliases (IDA's var_* names do not match the real offsets, so these were
// taken from the listing):
//   dest_x = esp+04, dest_y = esp+14, af = esp+28, ref_x = esp+10, ref_y = esp+0C,
//   var_4 = esp+2C (word, initial 10, reloaded by sub_167C9 after a walk),
//   var_18 = esp+18 (word),
//   var_C = esp+24 (R = first byte of unk_731B4[class*15]: 1 -> 5, 2 -> 7, else 0),
//   var_14 = esp+1C (R+2), var_10 = esp+20 (loop threshold),
//   ebp = target pointer returned by the selection branch (0 = branch failed).
// The selection helpers (3E65E/3E739/3EADD/3EBB4/3ED51/3EF12/3EE2B/3E5AB/3F247/3E251/
// 3DF3B) are the existing ones in combatii_ai.cpp and combatii_aimage.cpp.
// The route helpers (1419C/1499C/15342/15523/14FA4) follow the original search order and
// budget cut; the remaining 1x1 occupancy approximation is shared with sub_165C2.
#include "combatii.h"
#include "combatii_internal.h"
#include "battle_tables.h"

#include <algorithm>
#include <cstdlib>

namespace {
constexpr int kExitTurnEnd = 1;   // loc_40876: call sub_3F415(u, 1), then exit
constexpr int kExitSilent = 2;    // loc_40882: exit without settling

// sub_49ABC/49ABD direction codes: 0=S(0,+1), 1=W(-1,0), 2=N(0,-1), 3=E(+1,0).
constexpr int kTailDirX[4] = {0, -1, 0, 1};
constexpr int kTailDirY[4] = {1, 0, -1, 0};
// Quadrant scan orders used by sub_15523 to pick the next step.
constexpr int kQuadNE[4] = {1, 0, 3, 2};  // NE/E
constexpr int kQuadSE[4] = {1, 2, 3, 0};  // SE
constexpr int kQuadSW[4] = {3, 2, 1, 0};  // SW
constexpr int kQuadNW[4] = {3, 0, 1, 2};  // NW/W
constexpr int kQuadN[4]  = {0, 3, 2, 1};  // N
constexpr int kQuadS[4]  = {2, 3, 0, 1};  // S
constexpr int kTailCap = 0xA0;            // E8CC0/E8F40 queue cap

// Blocker triple shared by 1419C/143F3/1499C: the ASM compares the actor's [+0Eh]
// (effective army) against three slots.
int tail_blocker_triple(int eff, int (&out)[3]) {
    switch (eff) {
        case 1: case 2: out[0] = 3; out[1] = 4; out[2] = 4; return 3;
        case 3: out[0] = 1; out[1] = 2; out[2] = 4; return 3;
        case 4: out[0] = 1; out[1] = 2; out[2] = 3; return 3;
        case 9: out[0] = 8; out[1] = 8; out[2] = 8; return 3;
        default: return 0;
    }
}

// sub_1499C push rank key: 0 = closer to the destination, 1 = farther with the axis
// alignment broken, 2 = farther but still axis aligned.
int tail_rank_key(int px, int py, int nx, int ny, int dx, int dy) {
    const int before = std::abs(px - dx) + std::abs(py - dy);
    const int after = std::abs(nx - dx) + std::abs(ny - dy);
    if (after < before) return 0;
    if (nx != px) return (py == dy) ? 2 : 1;
    return (px == dx) ? 2 : 1;
}

const int* tail_quad_order(int dx, int dy) {
    if (dx > 0 && dy < 0) return kQuadNE;
    if (dx > 0 && dy > 0) return kQuadSE;
    if (dx < 0 && dy > 0) return kQuadSW;
    if (dx < 0 && dy < 0) return kQuadNW;
    if (dx == 0 && dy < 0) return kQuadN;
    if (dx == 0 && dy > 0) return kQuadS;
    if (dx > 0 && dy == 0) return kQuadNE;
    if (dx < 0 && dy == 0) return kQuadNW;
    return kQuadNW; // unreachable: dest == actor is handled by the caller
}

// Reverse of 49ABC: step (fx,fy) -> the opposite direction code.
int tail_reverse_dir(int fx, int fy) {
    if (fx == 0 && fy == -1) return 0; // N -> S
    if (fx == 1 && fy == 0) return 1;  // E -> W
    if (fx == 0 && fy == 1) return 2;  // S -> N
    if (fx == -1 && fy == 0) return 3; // W -> E
    return 0;
}
} // namespace

// sub_1419C: mark blockers and their open neighbours. dasm 6143..6240.
// Walks CC998 in tile-index order and accumulates -0x1E into E65B0 for the four E8 == -1
// neighbours of every occupant (N, W, E, S; byte wrap). An occupied tile itself is set to
// -(army+1).
void CombatII::battle_ai_1419c_tail(int unit_index, std::vector<int>& grid,
                                    std::vector<int8_t>& fringe) const {
    const int w = (int)map_w_, h = (int)map_h_;
    grid.assign((size_t)w * (size_t)h, -1);
    fringe.assign((size_t)w * (size_t)h, (int8_t)-1);
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    int tri[3] = {0, 0, 0};
    const int ntri = tail_blocker_triple(battle_effective_army(units_[(size_t)unit_index]), tri);
    if (ntri == 0) return;
    // 1419C scans CC998 in tile-index order, not occupant order: that way the neighbour
    // marks of an earlier blocker and the own-tile write of a later one stay in ASM order.
    std::vector<int> occ((size_t)w * (size_t)h, 0);
    for (const auto& u : units_) {
        if (!u.active || u.hp <= 0) continue;
        const int eff = battle_effective_army(u);
        bool blocker = false;
        for (int k = 0; k < ntri; ++k) if (tri[k] == eff) { blocker = true; break; }
        if (!blocker) continue;
        const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
        if (ux < 0 || uy < 0 || ux >= w || uy >= h) continue;
        occ[(size_t)uy * (size_t)w + (size_t)ux] = eff;
    }
    for (int idx = 0; idx < w * h; ++idx) {
        const int eff = occ[(size_t)idx];
        if (eff == 0) continue;
        const int ux = idx % w, uy = idx / w;
        static constexpr int kMarkOrder[4] = {2, 1, 3, 0}; // N,W,E,S
        for (int d = 0; d < 4; ++d) {
            const int nx = ux + kTailDirX[kMarkOrder[d]];
            const int ny = uy + kTailDirY[kMarkOrder[d]];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const size_t nidx = (size_t)ny * (size_t)w + (size_t)nx;
            if (grid[nidx] != -1) continue;
            fringe[nidx] = (int8_t)(fringe[nidx] - 0x1E);
        }
        grid[(size_t)idx] = -(eff + 1);
    }
}

// sub_1499C: four-bucket wave that expands the 147BB preset (actor tile = marker).
// dasm 6863..7395.
int CombatII::battle_ai_1499c_wave(int unit_index, int dest_x, int dest_y,
                                   std::vector<int>& grid, const std::vector<int8_t>& fringe,
                                   int marker) const {
    const int w = (int)map_w_, h = (int)map_h_;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if ((int)grid.size() != w * h || (int)fringe.size() != w * h) return 0;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    if (ax < 0 || ay < 0 || ax >= w || ay >= h) return 0;
    int mobility = battle_ai_mobility_selector(actor);
    if (battle_effective_army(actor) == 9) mobility = 5;      // 143F3: army 9 branch
    if (mobility < 0 || mobility >= kMobilityGridCount) return 0;
    int tri[3] = {0, 0, 0};
    const int ntri = tail_blocker_triple(battle_effective_army(actor), tri);
    // 13B51 pushes (x, y, remaining) and 13B9A returns that same triple. Re-reading E8
    // after the pop would give a different remaining value if another path raised it.
    struct WaveNode { int x, y, rem; };
    std::vector<WaveNode> stacks[4];                           // E8CC0/E8F3E: 4 buckets
    auto push = [&](int rank, int x, int y, int rem) {
        if ((int)stacks[rank].size() >= kTailCap) return;       // drop above 0xA0
        stacks[rank].push_back({x, y, rem});
    };
    const size_t start = (size_t)ay * (size_t)w + (size_t)ax;
    grid[start] = marker;                                      // 147BB preset
    push(0, ax, ay, marker);
    for (int guard = 0; guard < w * h * 8; ++guard) {
        int rank = -1;
        // 13B9A: the lowest non-empty bucket is popped first.
        for (int k = 0; k < 4; ++k) if (!stacks[k].empty()) { rank = k; break; }
        if (rank < 0) return 0;
        const WaveNode node = stacks[rank].back();
        stacks[rank].pop_back();
        const int px = node.x, py = node.y;
        // loc_14AC3: the destination is reached when E8 there is >= 0 right after the pop.
        if (dest_x >= 0 && dest_y >= 0 && dest_x < w && dest_y < h &&
            battle_ai_wc5_diff_read(grid, dest_x, dest_y) >= 0)
            return 1;
        const int rem = node.rem;
        const int fr = (py + 1 < h) ? (int)fringe[(size_t)(py + 1) * (size_t)w + (size_t)px] : -1;
        for (int d = 0; d < 4; ++d) {                          // fixed order S, W, N, E
            const int nx = px + kTailDirX[d], ny = py + kTailDirY[d];
            if (nx < 2 || ny < 2 || nx >= w - 2 || ny >= h - 2) continue;
            const size_t nidx = (size_t)ny * (size_t)w + (size_t)nx;
            bool sentinel = false;
            for (int k = 0; k < ntri; ++k)
                if (grid[nidx] == -(tri[k] + 1)) { sentinel = true; break; }
            if (sentinel) continue;
            if (nidx >= tile_attr_.size()) continue;
            const int attr = (int)tile_attr_[nidx];
            if (attr < 0 || attr >= kMobilityGridStride) continue;
            const int cost = (int)(int8_t)kMobilityCostGrids[mobility][attr];
            if (cost <= 0) continue;
            int newrem = rem - cost;
            if (fr != -1) newrem += fr;                        // fringe += E65B0[current+W]
            if (newrem <= grid[nidx]) continue;                // strict maximum
            // ASM 0x14C9A..0x14CB6 tests the fringe-adjusted newrem with `cmp edx,-1; jle`,
            // not the remaining value taken from the popped node.
            if (newrem <= -1) continue;
            grid[nidx] = newrem;
            if (newrem <= 0) continue;
            push(tail_rank_key(px, py, nx, ny, dest_x, dest_y), nx, ny, newrem);
        }
    }
    return 0;
}

// sub_15342: fallback destination search. dasm 7698..7870.
// Scans a diamond around the midpoint and keeps the strict minimum of
// E8 + 4*distance, then validates the winner with 165C2.
//   The distance term is measured to the *destination*: 0x153DB..0x153FB calls
//   49B01(current tile, dest_x, dest_y). The midpoint is only the 1AC08 diamond centre
//   (0x15395..0x153A6).
bool CombatII::battle_ai_15342_fallback(int unit_index, int dest_x, int dest_y,
                                        const std::vector<int>& grid,
                                        int& out_x, int& out_y) const {
    out_x = out_y = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const int w = (int)map_w_, h = (int)map_h_;
    if (w == 0 || h == 0) return false;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    const int mx = dest_x + ((ax - dest_x) >> 1);              // sar edx,1
    const int my = dest_y + ((ay - dest_y) >> 1);
    const int r = ((std::abs(ax - dest_x) + std::abs(ay - dest_y)) >> 1) + 7; // 49ADF >> 1 + 7
    int best = 0x7FFF;
    for (int j = 0; j <= 2 * r; ++j) {                         // 1AC08/1AC59 diamond
        const int y = my - r + j;
        const int half = std::min(j, 2 * r - j);
        for (int x = mx - half; x <= mx + half; ++x) {
            const int val = battle_ai_wc5_diff_read(grid, x, y);
            if (val < 0) continue;                             // 13CBD
            // 49B01(current tile, destination): Manhattan distance to the destination, not
            // to the midpoint.
            const int metric = val + 4 * (std::abs(x - dest_x) + std::abs(y - dest_y));
            if (metric >= best) continue;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            if (!battle_ai_165c2_1x1_ok(x, y, unit_index, grid)) continue; // si = 0/1
            best = metric;
            out_x = x; out_y = y;
        }
    }
    return best != 0x7FFF;
}

// sub_15523: greedy ascent from the destination back to the actor. dasm 7872..8120.
// Neighbours are visited in the quadrant order and the first strict maximum wins.
int CombatII::battle_ai_15523_route(int unit_index, int dest_x, int dest_y,
                                    const std::vector<int>& grid, int marker,
                                    std::vector<std::pair<int,int>>& route,
                                    int& out_edd76) const {
    route.clear();
    out_edd76 = 0;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const int w = (int)map_w_, h = (int)map_h_;
    if (w == 0 || h == 0) return 0;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    if (dest_x == ax && dest_y == ay) {                        // loc_1557D: zero-step case
        route.push_back({ ax, ay });
        out_edd76 = (int)actor.facing & 3;
        return 2;
    }
    // loc_1557D: fail when 11DB2 (the destination occupant) is not the actor.
    const int occupant = battle_unit_index_at(dest_x, dest_y);
    if (occupant >= 0 && occupant != unit_index) return 0;
    const int* order = tail_quad_order(dest_x - ax, dest_y - ay);
    int cx = dest_x, cy = dest_y;
    for (int guard = 0; guard <= kTailCap; ++guard) {
        route.push_back({ cx, cy });
        if ((int)route.size() > kTailCap) { route.clear(); return 0; }
        const int cur_e8 = battle_ai_wc5_diff_read(grid, cx, cy);
        int best_e8 = 0x80000000, bx = -1, by = -1;
        for (int i = 0; i < 4; ++i) {                          // si = 0..3, first strict maximum
            const int d = order[i];
            const int nx = cx + kTailDirX[d], ny = cy + kTailDirY[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const int v = grid[(size_t)ny * (size_t)w + (size_t)nx];
            if (v > best_e8) { best_e8 = v; bx = nx; by = ny; }
        }
        if (bx < 0) { route.clear(); return 0; }
        if (best_e8 == marker) {                               // reached the actor footprint
            const int fx = (route.size() == 1) ? (bx - cx) : (route[1].first - route[0].first);
            const int fy = (route.size() == 1) ? (by - cy) : (route[1].second - route[0].second);
            out_edd76 = tail_reverse_dir(fx, fy);
            return 1;
        }
        if (best_e8 <= cur_e8) { route.clear(); return 0; }    // ascent failed (wild jump)
        cx = bx; cy = by;
    }
    route.clear();
    return 0;
}

// sub_14FA4: trim the 15523 route to the budget diffusion and pick a facing. dasm 7388..7695.
// Pushes the previous route (E8CC0 -> E5E30) into bucket 0 from the actor end, keeping only
// tiles with E8 >= 0, then tests 165C2 with si = 0/1 from the farthest seed. If that fails,
// it looks for a candidate inside a radius-3 1AC08 diamond around the seed (1AD2E strict
// maximum that also passes 165C2) and reruns 15523 from there. Without a candidate it scans
// the remaining seeds; finally bucket 0 is reversed back into route, dropping the tail that
// lies outside the budget.
int CombatII::battle_ai_14fa4_route(int unit_index, int ref_x, int ref_y, int budget,
                                    const std::vector<int>& budget_grid,
                                    std::vector<std::pair<int,int>>& route,
                                    int* out_cost, bool trace) const {
    (void)budget;
    if (out_cost) *out_cost = 0;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (budget_grid.empty()) return -1;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    using Cell = std::pair<int,int>;
    // loc_14FC8: copy E8CC0 -> E5E30, order preserved.
    const std::vector<Cell> prior = route;
    std::vector<Cell> buckets[4];                             // E8F40/E5BB0: 4 buckets
    auto bucket_push = [&](int rank, Cell c) {
        if ((int)buckets[rank].size() >= kTailCap) return -1; // 13B51 cap
        buckets[rank].push_back(c);
        return 1;
    };
    auto bucket_pop = [&]() -> Cell {                         // 13B9A: lowest bucket first
        for (int k = 0; k < 4; ++k)
            if (!buckets[k].empty()) {
                Cell c = buckets[k].back();
                buckets[k].pop_back();
                return c;
            }
        return { -1, -1 };
    };
    auto buckets_empty = [&]() {                             // 13C13
        for (const auto& b : buckets) if (!b.empty()) return false;
        return true;
    };
    auto gate = [&](Cell c, int si) {                        // 165C2
        (void)si;  // ordinary 1x1 unit: si (footprint mode) makes no difference
        return c.first >= 0 && battle_ai_165c2_1x1_ok(c.first, c.second, unit_index, budget_grid);
    };
    // loc_15003: push the previous route into bucket 0 from the actor end; drop E8 < 0.
    for (auto it = prior.rbegin(); it != prior.rend(); ++it)
        if (battle_ai_wc5_diff_read(budget_grid, it->first, it->second) >= 0)
            bucket_push(0, *it);
    // 1524A/152F0: reverse bucket 0 back into E8CC0 and return the 14F52 facing.
    auto seed_tail = [&](int si) -> int {
        if (buckets[0].empty()) return -1;                   // -1 after 11E04
        Cell cell = bucket_pop();
        bucket_push(0, cell);
        int facing = (int)walk_facing_toward(cell.first, cell.second, ref_x, ref_y);
        int ret = facing;
        if (!gate(cell, facing)) {                           // 165C2(cell, 14F52)
            if ((si & 1) == 0) ret = (cell.second <= ref_y) ? si : 2;
            else ret = (cell.first >= ref_x) ? si : 3;
        }
        route.clear();                                       // EDD7E = E8F40[0]
        for (auto it = buckets[0].rbegin(); it != buckets[0].rend(); ++it)
            route.push_back(*it);
        if (battle_trace_ && trace)
            fprintf(stderr, "COMBAT: AI unit=%d 14FA4 seed tail=(%d,%d) si=%d route=%zu facing=%d\n",
                    actor.id, cell.first, cell.second, si, route.size(), ret);
        return ret;
    };
    // loc_1504C: pop one seed and test 165C2 with si = 0 then 1. On success push it back
    // (15240) and jump to 1524A (seed_tail).
    Cell cell = bucket_pop();
    int si = 0;
    while (si < 2 && !gate(cell, si)) ++si;
    if (si != 2) {
        bucket_push(0, cell);
        return seed_tail(si);
    }
    // loc_15091: radius-3 1AC08 diamond. The strict maximum of 1AD2E (edge distance to the
    // next tile) wins, and only tiles that pass 165C2 with si = 0/1 are candidates.
    int best_dist = -1, best_si = 0;
    Cell best{ -1, -1 };
    {
        Wc5Dia it;
        battle_ai_wc5_dia_init(it, cell.first, cell.second, 3);
        int dist = 0, x = 0, y = 0;
        while (battle_ai_wc5_dia_next(it, x, y)) {
            if (dist > best_dist) {
                int s2 = 0;
                while (s2 < 2 && !gate({ x, y }, s2)) ++s2;
                if (s2 != 2) { best_dist = dist; best = { x, y }; best_si = s2; }
            }
            dist = battle_ai_wc5_1ad2e(it);
        }
    }
    if (best.first >= 0) {                                   // loc_15139
        route.clear();
        const int marker = battle_ai_wc5_diff_read(budget_grid, ax, ay);
        std::vector<Cell> rerouted;
        int edd = 0;
        if (battle_ai_15523_route(unit_index, best.first, best.second, budget_grid,
                                  marker, rerouted, edd) != 0)
            route = rerouted;
        int facing = (int)walk_facing_toward(best.first, best.second, ref_x, ref_y);
        if (battle_trace_ && trace)
            fprintf(stderr, "COMBAT: AI unit=%d 14FA4 diamond seed=(%d,%d) -> dest=(%d,%d) dist=%d si=%d route=%zu facing=%d\n",
                    actor.id, cell.first, cell.second, best.first, best.second,
                    best_dist, best_si, route.size(), facing);
        if (!gate(best, facing)) {                           // 15180: 165C2(dest, facing)
            if ((best_si & 1) == 0) facing = (best.second <= ref_y) ? best_si : 2;
            else facing = (best.first >= ref_x) ? best_si : 3;
        }
        return facing;
    }
    // loc_151E0: with no candidate, scan the remaining seeds and push back any that pass
    // 165C2.
    while (!buckets_empty()) {
        cell = bucket_pop();
        si = 0;
        while (si < 2 && !gate(cell, si)) ++si;
        if (si != 2) {
            bucket_push(0, cell);
            return seed_tail(si);
        }
    }
    return seed_tail(si);
}

bool CombatII::battle_ai_route_walk_path(int unit_index, int mobility, int budget,
                                         int& dest_x, int& dest_y, bool use_wave,
                                         int ref_x, int ref_y, int& out_facing,
                                         std::vector<std::pair<int,int>>& out_walk,
                                         int* out_cost, bool trace) const {
    out_walk.clear();
    if (out_cost) *out_cost = 0;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const int w = (int)map_w_, h = (int)map_h_;
    if (w == 0 || h == 0) return false;
    const auto& actor = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)actor.x, ay = (int)(int8_t)actor.y;
    if (dest_x == ax && dest_y == ay) {                        // sub_15523 return 2: already there
        out_walk.push_back({ ax, ay });
        return true;
    }
    std::vector<std::pair<int,int>> route;
    int edd76 = 0;
    std::vector<int> budget_grid = battle_ai_build_diffusion(unit_index, budget);
    if (budget_grid.empty()) return false;
    if (use_wave) {
        // loc_406DB: 14300(0xD2) -> 1419C -> 1499C -> (on failure) 15342 -> 15523
        // (D2 diffusion) -> 14FA4 (trim and retarget with the budget diffusion).
        std::vector<int> grid;
        std::vector<int8_t> fringe;
        battle_ai_1419c_tail(unit_index, grid, fringe);
        if (battle_ai_1499c_wave(unit_index, dest_x, dest_y, grid, fringe, 0xD2) == 0) {
            int fx = -1, fy = -1;
            if (!battle_ai_15342_fallback(unit_index, dest_x, dest_y, grid, fx, fy)) return false;
            dest_x = fx; dest_y = fy;
        }
        const int marker = battle_ai_wc5_diff_read(grid, ax, ay);
        if (battle_ai_15523_route(unit_index, dest_x, dest_y, grid, marker, route, edd76) == 0)
            return false;                                     // 13B37: no route -> di = -2
        const int f14 = battle_ai_14fa4_route(unit_index, ref_x, ref_y, budget, budget_grid,
                                              route, nullptr, trace);
        if (route.empty()) return false;                      // 13B37
        if (f14 >= 0) out_facing = f14;                       // var_8 handed to 1620A
    } else {
        // Di == 1 (loc_405C4): build the 15523 route only; 14FA4 belongs to loc_406DB.
        const int marker = battle_ai_wc5_diff_read(budget_grid, ax, ay);
        if (battle_ai_15523_route(unit_index, dest_x, dest_y, budget_grid, marker, route, edd76) == 0)
            return false;
    }
    if (route.empty()) return false;
    // E8CC0 holds destination-first push order and 13AFA pops from the back, so reverse it
    // into actor-first order.
    out_walk.push_back({ ax, ay });
    int cost = 0;
    for (auto it = route.rbegin(); it != route.rend(); ++it) {
        out_walk.push_back(*it);
        const size_t ti = (size_t)it->second * (size_t)w + (size_t)it->first;
        if (ti >= tile_attr_.size()) continue;
        const int attr = (int)tile_attr_[ti];
        if (attr < 0 || attr >= kMobilityGridStride) continue;
        const int step = (int)(int8_t)kMobilityCostGrids[mobility][attr];
        if (step > 0) cost += step;
    }
    if (out_cost) *out_cost = cost;
    return out_walk.size() >= 2;
}

// sub_40295 loc_407BF tail (the walk arm).
// Returns 0 = no walkable route (equivalent to di = -2), 1 = walk/turn done, 2 = the
// loc_40882 exit (unit gone, terrain death, ...).
int CombatII::battle_ai_40295_walk(int unit_index, int dest_x, int dest_y, int facing,
                                   int budget, int ref_hx, int ref_hy, bool use_wave) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 2;
    if (map_w_ == 0 || map_h_ == 0) return 0;
    const int actor_id = units_[(size_t)unit_index].id;
    const int ax = (int)(int8_t)units_[(size_t)unit_index].x;
    const int ay = (int)(int8_t)units_[(size_t)unit_index].y;
    const int mobility = battle_ai_mobility_selector(units_[(size_t)unit_index]);
    auto gone = [&]() {
        return should_exit_ || unit_index < 0 || (size_t)unit_index >= units_.size() ||
               !units_[(size_t)unit_index].active || units_[(size_t)unit_index].id != actor_id;
    };
    // Settlement diffusion: E8F46 as 14FA4 rebuilds it before the walk (budget ecx). The
    // start-tile refresh (+1) is undone here.
    std::vector<int> settle_grid = battle_ai_build_diffusion(unit_index, budget);
    const int start_idx = ay * (int)map_w_ + ax;
    if (!settle_grid.empty() && start_idx >= 0 && (size_t)start_idx < settle_grid.size())
        settle_grid[(size_t)start_idx] -= 1;

    if (dest_x == ax && dest_y == ay) {
        // sub_15523 zero-step -> sub_1620A only applies the facing.
        if (facing >= 0 && facing < 4) units_[(size_t)unit_index].facing = (int8_t)facing;
    } else {
        // Port of sub_40295 loc_405C4 (15523 route) and loc_406DB (14300 0xD2, 1419C,
        // 1499C, 15342, 15523, 14FA4). With use_wave the D2 wave decides whether the
        // destination is reachable and may substitute a 15342 fallback target. The route
        // itself is the 15523 greedy ascent, and 14FA4 trims its prefix to the budget
        // (var_18*4).
        std::vector<std::pair<int,int>> walk_path;
        int walk_cost = 0;
        if (!battle_ai_route_walk_path(unit_index, mobility, budget, dest_x, dest_y, use_wave,
                                       ref_hx, ref_hy, facing, walk_path, &walk_cost)) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI unit=%d 40295 walk: no 15523 route to (%d,%d) (wave=%d budget=%d), di=-2\n",
                        actor_id, dest_x, dest_y, use_wave ? 1 : 0, budget);
            return 0;                                  // empty route (13B37) -> di = -2
        }
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d 40295 walk %zu step(s) from=(%d,%d) to=(%d,%d) ref=(%d,%d) facing=%d budget=%d cost=%d wave=%d\n",
                    actor_id, walk_path.size() - 1, ax, ay, walk_path.back().first, walk_path.back().second,
                    ref_hx, ref_hy, facing, budget, walk_cost, use_wave ? 1 : 0);
        if (!battle_ai_play_walk(unit_index, walk_path, facing)) return 2;
    }
    if (gone()) return 2;
    // 12124 (reset pose) and 11E04 are part of play_walk. Settlement:
    // S = budget - E8F46[arrival], then 3C993((S >> 2) * 10 + var_4).
    const int now_x = (int)(int8_t)units_[(size_t)unit_index].x;
    const int now_y = (int)(int8_t)units_[(size_t)unit_index].y;
    int arrive = -128;                                 // sub_13CBD out-of-bounds value
    if (now_x >= 0 && now_y >= 0 && now_x < (int)map_w_ && now_y < (int)map_h_ && !settle_grid.empty())
        arrive = settle_grid[(size_t)now_y * (size_t)map_w_ + (size_t)now_x];
    const int s = (int16_t)(budget - arrive);
    const int charge = ((s >> 2) * 10) + battle_ai_var4_;
    battle_ai_3c993(unit_index, charge > 0 ? charge : 0);
    battle_ai_var4_ = battle_ai_167c9(unit_index);     // refresh var_4 for the next iteration
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d 40295 settle S=%d charge=%d TP=%d var_4=%d\n",
                actor_id, s, charge, battle_tp(), battle_ai_var4_);
    // sub_5401B (area/region triggers) -> 0 means loc_40882. Then sub_17310 -> 0 also
    // means loc_40882.
    battle_after_move(unit_index);
    if (gone()) return 2;
    if (battle_terrain_17310(unit_index) == 0) {
        if (battle_trace_) fprintf(stderr, "COMBAT: AI unit=%d terrain death (17310=0)\n", actor_id);
        return 2;
    }
    if (gone()) return 2;
    battle_wc1_occupancy_preserve_or_close("40295 walk");
    return 1;
}

bool CombatII::battle_ai_40295_body(int unit_index, int wc) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    auto alive = [&]() {
        return !should_exit_ && unit_index >= 0 && (size_t)unit_index < units_.size() &&
               units_[(size_t)unit_index].active && units_[(size_t)unit_index].hp > 0;
    };
    struct Var4Reset {
        int& v;
        explicit Var4Reset(int& r) : v(r) { v = 10; }
        ~Var4Reset() { v = 10; }
    } var4_reset{battle_ai_var4_};

    // Entry block (40295..402FC).
    // ASM 40295+1B..+3E:
    //   xor edx,edx / mov dl,[eax+11h] / mov eax,edx / shl eax,4 / sub eax,edx -> class*15
    //   mov al,byte ptr unk_731B4[edx] -> var_C = 5 for 1, 7 for 2, 0 otherwise
    // R (var_C) therefore comes from the first byte of the class stat table (the guard
    // radius kind), not from the weapon class. Units whose kind maps to 0 skip the early
    // 3F184/3EBB4 block and the 3F247 state path and go straight to walking
    // (3E5AB/3E251/3DF3B -> 15523 -> 1620A).
    (void)wc; // the 40295 body never reads the weapon class; 3CA3C/3D3E5 derive it from [+11h]
    const uint8_t guard_kind = class_stat_guard_kind(units_[(size_t)unit_index].combat_class);
    const int R = (guard_kind == 1) ? 5 : (guard_kind == 2 ? 7 : 0);     // var_C
    const int sec = R + 2;                                // var_14
    int di = 0;                                           // edi: entry value, never -2
    int var_10 = 10;
    int dest_x = -1, dest_y = -1, af = -1, ref_x = -1, ref_y = -1;
    bool wave_route = false;                              // take the loc_406DB path (0xD2 wave)
    int tgt = -1;                                         // early 3F184/3EBB4 result (esp+0)
    bool ebp_set = false;                                 // ebp from the previous iteration

    auto finish = [&](int kind) -> bool {
        if (kind == kExitTurnEnd && alive()) battle_ai_turn_end_3f415(unit_index, 1);
        return true;
    };

    for (int guard = 0; guard < 256; ++guard) {           // loc_402FC
        if (!alive()) return true;
        const int v = battle_ai_4016a(unit_index, true);
        if (!alive()) return true;
        // Every exit of sub_4016A reaches loc_40271 `mov eax,edi`, so its return value is
        // edi. 40295+80 re-reads that edi (di) with `movsx eax,di; cmp eax,-2`, so di must
        // be restored from the return value here as well.
        di = v;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d 40295 loop: 4016A=%d di=%d TP=%d var_10=%d var_4=%d\n",
                    units_[(size_t)unit_index].id, v, di, battle_tp(), var_10, battle_ai_var4_);
        if (v == -2) return finish(kExitSilent);          // loc_40882
        if (v == -1 && di == -2) return finish(kExitTurnEnd); // loc_40876

        // loc_40323: var_18 = (TP - var_4) / 10 (idiv, truncates toward 0, stored as word)
        const int var18 = (int16_t)((battle_tp() - battle_ai_var4_) / 10);
        di = -1;
        tgt = -1;
        wave_route = false;                                    // reset every iteration
        const int ws = battle_ai_watch_state(unit_index);       // sub_3E18B (pure)
        const int k6c = (int)units_[(size_t)unit_index].ai_decision_state;
        const int budget4 = var18 * 4;

        // ---- Early block (4033E..403FF): R != 0 and TP > 0x28 ----
        if (R != 0 && battle_tp() > 0x28) {
            std::vector<int> grid;
            battle_ai_3f0e4(unit_index, grid, R, 1, budget4);      // 14300 then 3F0E4(R, band=1)
            int t = -1, ex = -1, ey = -1, eaf = -1;                // values after 14300(-1)
            if (ws == 1) {
                t = battle_ai_3f184(unit_index, sec, grid, -1, ex, ey, eaf);
            } else if (k6c == 3) {
                int occ = -1;
                if (battle_ai_3ebb4_grid(unit_index, sec, grid, -1, ex, ey, eaf, occ)) t = occ;
            }
            if (t >= 0) {
                const int px = (int)(int8_t)units_[(size_t)unit_index].x;
                const int py = (int)(int8_t)units_[(size_t)unit_index].y;
                if (std::abs(ex - px) + std::abs(ey - py) >= 2) {   // sub_49ADF, `cmp ax,2; jl`
                    di = 2;
                    tgt = t;
                    dest_x = ex; dest_y = ey; af = eaf;
                }
            }
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI unit=%d 40295 early block ws=%d k=%d -> %s t=%d dest=(%d,%d) af=%d\n",
                        units_[(size_t)unit_index].id, ws, k6c, di == 2 ? "di=2" : "none", t, ex, ey, eaf);
        }

        // ---- loc_40409: gauge gate ----
        bool ebp = false;
        if (battle_tp() <= battle_ai_var4_ + 0x18) {
            di = -3;                                           // loc_405E1
        } else {
            // 14300(var18*4), 14032, 143F3: each helper rebuilds the diffusion with budget4.
            if (ws > 0) {
                if (di == 2) {
                    ebp = (tgt >= 0);                          // ebp = var_30
                } else {
                    ebp = battle_ai_3e5ab(unit_index, budget4, dest_x, dest_y, af);   // 3E5AB
                }
            } else {
                di = -3;                                       // loc_40470
                const int k = k6c;
                int dx = -1, dy = -1, face = -1, hostile = -1, score = -1;
                if (k > 6) {
                    ebp = ebp_set;                             // ja loc_4051E: stale ebp
                } else if (k == 0) {
                    ebp = battle_ai_3e65e_select(unit_index, budget4, var18, hostile, dx, dy, face, score);
                } else if (k == 1) {
                    std::vector<int> hostiles;
                    for (int h : battle_target_list_)
                        if (h >= 0 && (size_t)h < units_.size() && units_[(size_t)h].active)
                            hostiles.push_back(h);          // F0648 list
                    ebp = battle_ai_tact_3e739(unit_index, 2, var18, budget4, hostiles,
                                               dx, dy, face, hostile) == 1;
                } else if (k == 2 || k == 4) {
                    ebp = battle_ai_jump_approach(unit_index, k, var18, budget4, dx, dy, face, hostile);
                } else if (k == 3) {
                    ebp = battle_ai_jump_3ebb4(unit_index, var18, budget4, dx, dy, face, hostile);
                } else if (k == 5) {
                    const int r = battle_ai_flee_3ef12(unit_index, 3, budget4, dx, dy, face);
                    di = r;                                    // mov edi, eax
                    ebp = (r == 1);
                } else {                                       // k == 6
                    ebp = battle_ai_jump_3ee2b(unit_index, budget4, dx, dy, face, hostile);
                }
                if (k <= 6 && ebp) {
                    dest_x = dx; dest_y = dy; af = face;
                    if (hostile >= 0 && (size_t)hostile < units_.size()) {
                        ref_x = (int)(int8_t)units_[(size_t)hostile].x;
                        ref_y = (int)(int8_t)units_[(size_t)hostile].y;
                    }
                }
            }
            // loc_4051E
            ebp_set = ebp;
            if (!ebp) {
                di = -1;
            } else {
                const auto& uu = units_[(size_t)unit_index];
                if ((int)(int8_t)uu.x == dest_x && (int)(int8_t)uu.y == dest_y &&
                    (int)(int8_t)uu.facing == (int16_t)af) {
                    di = -2;                                   // loc_40589: same tile and facing
                } else if (di != 2) {
                    di = 1;
                }
            }
            // loc_405C4: di == 1 builds the 15523 route (the walk arm runs the search).
        }

        // loc_405E6: branch failed, R != 0 and 3E18B > 0 -> 3F247 state path
        if (di == -1 && R != 0 && ws > 0) {
            int sx = -1, sy = -1;
            if (battle_ai_3f247(unit_index, R, budget4, sx, sy)) {
                dest_x = sx; dest_y = sy;
                di = 2;
                af = -1;
            }
        }

        // loc_4062D: still undecided -> 3E251 (ws > 0) or 3DF3B (radius 0x1E)
        if (di == -1) {
            bool have_dest = true;
            std::vector<int> dest_e8 = battle_ai_build_diffusion(unit_index, budget4);
            const std::vector<int>* dest_grid = dest_e8.empty() ? nullptr : &dest_e8;
            if (ws > 0) {
                int rx = -1, ry = -1;
                if (!battle_ai_3e251_dest(unit_index, dest_x, dest_y, rx, ry, dest_grid)) have_dest = false;
                ref_x = rx; ref_y = ry;
            } else {
                if (ws == -1 || ws == -2) return finish(kExitTurnEnd);       // loc_40664
                if (k6c == 5 || k6c == 6) return finish(kExitTurnEnd);
                int hx = -1, hy = -1;
                if (!battle_ai_3df3b_dest(unit_index, 0x1E, dest_x, dest_y, hx, hy, dest_grid)) have_dest = false;
                // sub_3DF3B returns 0 when the best destination is the current tile.
                if (have_dest && dest_x == (int)(int8_t)units_[(size_t)unit_index].x &&
                    dest_y == (int)(int8_t)units_[(size_t)unit_index].y)
                    have_dest = false;
                ref_x = hx; ref_y = hy;
            }
            if (!have_dest) return finish(kExitTurnEnd);                     // loc_40876
            const int ux = (int)(int8_t)units_[(size_t)unit_index].x;
            const int uy = (int)(int8_t)units_[(size_t)unit_index].y;
            if (dest_x == ux && dest_y == uy) {
                di = -2;                                                     // loc_406D1
            } else {
                // loc_406DB tail: the walk arm runs 14300(0xD2) -> 1419C -> 1499C ->
                // 15342 on failure -> 15523 -> 14FA4. Leave di = 1 and enable the wave
                // path; if 15523/14FA4 cannot build a route (13B37), the walk arm returns
                // 0 and di becomes -2.
                wave_route = true;
                af = -1;                                                     // EDD76 from the walk arm
                di = 1;
            }
        }

        // loc_4076A
        if (di >= 1) {
            if (di == 2) {
                // Warp arm: 1124A -> 3BF00 -> 3C993(0x28) -> [63h] = af when af < 4 -> 13F8A
                const int ux = (int)(int8_t)units_[(size_t)unit_index].x;
                const int uy = (int)(int8_t)units_[(size_t)unit_index].y;
                camera_center_on(ux, uy);
                if (dest_x != ux || dest_y != uy) {
                    if (!battle_warp_unit(unit_index, dest_x, dest_y)) return finish(kExitTurnEnd);
                    if (!alive()) return true;
                }
                battle_ai_3c993(unit_index, 0x28);
                if ((uint16_t)af < 4) units_[(size_t)unit_index].facing = (int8_t)af;
            } else {
                // Walk arm (loc_407BF): 1124A -> 1620A -> 12124 -> 11E04 -> 45559 -> 3C993
                // -> 167C9 -> 5401B -> 17310
                const int ref_hx = ref_x, ref_hy = ref_y;
                const int w = battle_ai_40295_walk(unit_index, dest_x, dest_y, af, budget4, ref_hx, ref_hy,
                                                   wave_route);
                if (w == 2) return finish(kExitSilent);                      // loc_40882
                if (w == 0) di = -2;
            }
        }

        // loc_40853: exit when TP < var_10; otherwise var_10 += 4 and exit when di == -3
        if (battle_tp() < var_10) return finish(kExitTurnEnd);
        var_10 += 4;
        if (di == -3) return finish(kExitTurnEnd);
    }
    return finish(kExitTurnEnd);
}
