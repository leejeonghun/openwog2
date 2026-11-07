// combatii_aimage: mage (wc==2) AI - sub_44913 -> sub_43CA9 -> sub_43526 / sub_438AE.
// dasm line numbers: 432A0(68893) 42DC1(68466) 42B77(68230) 42C97(68333) 42D51(68415)
//   43526(69113) 438AE(69404) 3E3BF/3E457/3E5AB/3E251/3E01B/3DF3B(61343..62060) 3F415(63307).
// The original ASM is the reference. Lower paths the port does not implement (14FA4/15523) are
// bridged with the existing approximations (BFS route, battle_cast_magic); each one is marked in a
// comment at its use. LOS collection uses g2range::Range (combatii_range.cpp), a direct port of
// sub_1B7BF/sub_1B7FA.
#include "combatii.h"
#include "combatii_internal.h"
#include "battle_tables.h"

namespace {

// sub_1AC08/sub_1AC59 diamond (y ascending, x ascending inside a row). Radius 0 is the centre tile.
template <typename Fn>
void mage_diamond(int cx, int cy, int r, Fn&& fn) {
    if (r < 0) return;
    for (int j = 0; j <= 2 * r; ++j) {
        const int y = cy - r + j;
        const int half = std::min(j, 2 * r - j);
        for (int x = cx - half; x <= cx + half; ++x) fn(x, y);
    }
}

inline int s16(int v) { return (int16_t)v; }

// sub_1A452/sub_1A552 band iterator (file local copy, mirroring combatii_ai.cpp).
struct AiBandIter {
    int x = 0, y = 0;
    int s9a = 0, s96 = 0, s9c = 0, s98 = 0;
    int c_lo = 0, rows = 0;
    int n_lo = 0, n_hi = 0;
    bool done = false;
};

void ai_band_init(AiBandIter& it, int x, int y, int d, int cx, int rng) {
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

bool ai_band_next(AiBandIter& it, int& ox, int& oy) {
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

}  // namespace

// ---- sub_3E21B ----
int CombatII::battle_ai_watch_unit(int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (u.ai_watch_sel != 0xFF) {
        return battle_ai_cc960_member((int)u.ai_watch_sel, (int)u.ai_watch_id);
    }
    if (u.ai_target_unit >= 0 && (size_t)u.ai_target_unit < units_.size()) return u.ai_target_unit;
    return -1;
}

// ---- sub_42B77 ----
// sub_1AF22 triangle iterator (x, y, facing, cx=1, radius); the first 1B00D call drops the start
// tile.
int CombatII::battle_ai_mage_42b77(int unit_index, int mode, int radius) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    Wc5Iter it;
    battle_ai_wc5_iter_init(it, (int)(int8_t)u.x, (int)(int8_t)u.y, 1, (int)u.facing & 3,
                            s16(radius));
    int x = 0, y = 0;
    battle_ai_wc5_iter_next(it, x, y);  // drop the start tile
    while (battle_ai_wc5_iter_next(it, x, y)) {
        if (x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4) continue;  // 11DB2 guard
        const int t = battle_unit_index_at(x, y);
        if (t < 0 || (size_t)t >= units_.size()) continue;
        const CombatUnitPlacement& o = units_[(size_t)t];
        const bool hostile = battle_armies_hostile(battle_effective_army(u), battle_effective_army(o),
                                                   battle_hostility_inverted_);
        if (mode == 0) {
            if (!hostile && o.combat_flags_1e != 0) return 1;
        } else if (mode == 1) {
            if (!hostile) {
                if (o.unit_kind_0d == 2) continue;
                if (s16(o.sprite_type) >= 0x12C) continue;
                if ((battle_max_hp_raw(o) / 100) * 0x46 > battle_hp_raw(o)) return 1;
            } else if (o.unit_kind_0d == 2) {
                return 1;
            }
        } else if (mode == 2) {
            if (!hostile && (o.combat_flags_1e & 7)) return 1;
        }
    }
    return -1;
}

// ---- sub_42D51 ----
int CombatII::battle_ai_mage_42d51(int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const int army = battle_effective_army(u);
    int count = 0, last = -1;
    for (size_t i = 0; i < units_.size(); ++i) {  // approximation of walking the CC968[army-1] list
        const auto& o = units_[i];
        if (!o.active || battle_effective_army(o) != army) continue;
        if ((int)o.item_use_12 == u.id + 10) { ++count; last = (int)i; }
    }
    if (count == 1) {
        const auto& o = units_[(size_t)last];
        const int m = battle_max_hp_raw(o);
        if ((m >> 1) > battle_hp_raw(o)) return 1;
    }
    return count == 0 ? 1 : 0;
}

// ---- sub_42C97 ----
// Original: ecx = wounded (<70% HP), esi = total; returns 1 when (esi*100)/ecx < 30. Because
// esi >= ecx this is always false, so the routine effectively returns -1.
int CombatII::battle_ai_mage_42c97(int army) const {
    int lo, hi;
    if (army == 1 || army == 2) { lo = 1; hi = 2; }
    else if (army == 3) { lo = 3; hi = 3; }
    else if (army == 4) { lo = 4; hi = 4; }
    else return -1;
    int wounded = 0, total = 0;
    for (const auto& o : units_) {
        if (!o.active) continue;
        const int a = battle_effective_army(o);
        if (a < lo || a > hi) continue;
        if ((battle_max_hp_raw(o) / 10) * 7 > battle_hp_raw(o)) ++wounded;
        ++total;
    }
    if (s16(wounded) == 0) return -1;
    if ((s16(total) * 100) / s16(wounded) < 0x1E) return 1;
    return -1;
}

// Approximation of the corpse list (dword_D6620): retreated/dead units of the same army.
static bool mage_corpse_exists(const std::vector<CombatUnitPlacement>& units, int army,
                               int (*eff)(const CombatUnitPlacement&)) {
    for (const auto& o : units)
        if (!o.active && o.hp <= 0 && eff(o) == army) return true;
    return false;
}
static int mage_eff_army(const CombatUnitPlacement& u) { return battle_effective_army(u); }

// ---- sub_42DC1 ----
// Returns R = group*21 + d*3 + kind, -1 when no spell is possible, -2 when MP/TP are short.
int CombatII::battle_ai_mage_42dc1(int unit_index, int mode) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (mode == 1 && combat_rand(3) == 0) return -1;
    if (s16(u.combat_mp_50) < 0) return -1;
    const int var10 = (int)(int8_t)battle_lookup_184a4(u.combat_type_5b, 0x36);  // cbw
    int esi = 6;
    if ((mode == 1 || mode == 2) && combat_rand(3) == 0) esi = 3;
    const int tp = battle_tp();
    const int mp = s16(u.combat_mp_50);
    if (tp == s16(u.combat_value_54) && weapon_class(u) == 2) {
        while (esi >= 0) {                       // loc_42E49: MP only
            if (mp >= (int)kSpellCost[esi]) break;
            --esi;
        }
    } else {
        while (esi >= 0) {                       // loc_42E62: both MP and the TP threshold
            if (mp >= (int)kSpellCost[esi] && tp >= battle_spell_thr(u, esi)) break;
            --esi;
        }
    }
    if (esi < 0) return -2;
    int g = 0, kind = 0, d = -1;                 // var_18, var_8, edi
    const int army = battle_effective_army(u);
    auto morale = [&]() { return army == 3 ? (int)morale_hi_ : (int)morale_lo_; };
    switch (mode) {
    case 0: {
        if (esi > 2 && (u.combat_flags_1e & 0x10)) esi = 2;
        g = 3;
        kind = battle_spell_kind(u, 3);
        if (kind < 0) return -1;
        if (army != 4 && esi == 6 && morale() < 0x50 && mp > 100) { d = 6; break; }
        if (esi >= 3) {
            const int radius = s16(2 * kind + 3 + var10);
            mage_diamond((int)(int8_t)u.x, (int)(int8_t)u.y, radius, [&](int x, int y) {
                if (d != -1) return;
                if (x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4) return;
                const int t = battle_unit_index_at(x, y);
                if (t < 0 || (size_t)t >= units_.size()) return;
                const auto& o = units_[(size_t)t];
                if (battle_armies_hostile(battle_effective_army(u), battle_effective_army(o),
                                          battle_hostility_inverted_) &&
                    o.unit_kind_0d == 2)
                    d = 3;
            });
            if (d != -1) break;
        }
        if (esi >= 2 && battle_ai_mage_42b77(unit_index, 0, var10 + 5) == 1) { d = 2; break; }
        if (battle_ai_mage_42b77(unit_index, 1, var10 + 5) == 1) { d = combat_rand(2); break; }
        if (esi >= 3 && battle_ai_mage_42b77(unit_index, 2, var10 + 5) == 1) {
            g = 4;
            kind = battle_spell_kind(u, 4);
            if (kind >= 0) {
                d = (esi >= 5 && combat_rand(3) == 0) ? 5 : 3;
                break;
            }
            g = 3;  // loc_43062: the negative kind is kept, so the d=4 path below fails the R check (-1)
        }
        if (mp <= 0x50) break;
        if (esi < 4) break;
        if (battle_ai_mage_42c97(army) == 1) d = 4;
        break;
    }
    case 1: {
        if (esi > 3 && (u.combat_flags_1e & 0x10)) esi = 3;
        int tries = 0;
        while (tries < 3) {
            g = combat_rand(3);
            kind = battle_spell_kind(u, g);
            if (kind >= 0) break;
            ++tries;
        }
        if (tries == 3) return -1;
        const int q = battle_max_hp_raw(u) / 100;
        if (q * 70 > battle_hp_raw(u) && mp > 0x32 && esi >= 4 &&
            battle_ai_mage_42d51(unit_index) == 1) { d = 4; break; }
        if (army != 4 && esi == 6 && morale() < 0x50 && mp > 100) { d = 6; break; }
        int n;
        if (esi >= 5) n = 5; else { ++esi; n = esi; }
        d = combat_rand(n);
        if (d == 4) d = 5;
        break;
    }
    case 2: {
        if (u.combat_flags_1e & 0x10) return -1;
        g = 6;
        kind = battle_spell_kind(u, 6);
        if (kind < 0) return -1;
        if (army != 4 && esi >= 3 && battle_ai_mage_42d51(unit_index) == 1 && morale() < 0x50 &&
            mp > 0x32 && mage_corpse_exists(units_, army, mage_eff_army)) { d = 3; break; }
        ++esi;
        d = combat_rand(esi);
        if (d == 3) d = 0;
        break;
    }
    case 3: {
        if (esi > 4 && (u.combat_flags_1e & 0x10)) esi = 4;
        g = 4;
        kind = battle_spell_kind(u, 4);
        if (kind < 0) return -1;
        ++esi;
        d = combat_rand(esi);
        break;
    }
    default:
        return -1;
    }
    if (d == -1) return -1;
    if ((unsigned)(g & 0xFFFF) > 6u || d > 6 || (unsigned)(kind & 0xFFFF) > 2u) return -1;
    return g * 21 + d * 3 + kind;
}

// ---- sub_1B7BF / sub_1B7FA: AI spell LOS collection and marking ----
// sub_1B7BF(army, expect_1201D, mode) sets the global filters F0B88/F0B86/F0B8E; sub_1B7FA(x, y,
// facing, count, rows) walks the front cone with the 1AF22 triangle iterator, clips it by terrain
// brightness and fills the tiles behind an occluder with the shadow marker (13D02 = -2)
// (ASM 16793..17012). The port uses the real body through g2range::Range::los_from_actor
// (= 1B7BF + 13F8A + 1B7FA) and only consumes its output here:
//   mode 0: marks visible tiles with 1 in E8F46; the callers (419F1/41BF5/42151/42375/4258C/42743)
//           filter centre candidates with `grid_read(cx,cy) >= 0` (ASM `13CBD < 0 -> skip`).
//   mode 1: collects units whose 11DB2 occupant passes 1201D(occupant army, caster army) == expect,
//           in F0648 order. Only 41715 / 4189C / 42A1D consume the list; no other collector reads
//           F0B9C.
//   rows (stack argument): callers push `[esp-2]>>16`, the low word of the frame, which holds the
//   sub_28465(caster, cx) result stored just before. cx differs per handler (see
//   battle_ai_spell_pick_target); count (ecx) is always 1.
void CombatII::battle_ai_los_1b7fa(g2range::Range& rng, int caster_index, int expect, int mode,
                                   int los_range) {
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    const CombatUnitPlacement& A = units_[(size_t)caster_index];
    if (!A.active || A.hp <= 0) return;
    rng.los_from_actor(battle_range_unit(caster_index), expect, mode, los_range);
}

void CombatII::battle_ai_collect_1b7fa(int caster_index, bool want_hostile, int los_range,
                                       std::vector<int>& out) {
    out.clear();
    if (caster_index < 0 || (size_t)caster_index >= units_.size()) return;
    const g2range::Env env = battle_range_env_make();
    g2range::Range rng(env);
    // 41715/42A1D expect 1 (hostile) and 4189C expects 0 (ally), matching 1B7BF's edx argument.
    battle_ai_los_1b7fa(rng, caster_index, want_hostile ? 1 : 0, 1, los_range);
    for (int t : rng.list) {
        if (t < 0 || (size_t)t >= units_.size()) continue;
        out.push_back(t);
    }
}

// ---- Target selection (sub_41715 / sub_4189C / sub_419F1 ...) ----
// The eleven AI spell target collectors ported from the ASM:
//   41715(28982) 4189C(29273) 419F1(29514/28B24) 41BF5(28C8A) 41E41(29722) 42025(2A1C6)
//   42151(2AE44) 42375(2B031) 4258C(2B1D0) 42743(2B1D0/2B391/2C468) 42A1D(2C00E/2C28F)
// The calling convention differs per handler; this function derives the stack/register arguments
// from q1/k. The rows value in 1B7FA/1AF22 is always `sub_28465(caster, cx)` =
// (int8)184A4[5Bh][0x36] + cx, where cx comes from the handler (e.g. 28ED6 uses 7 when k>0 and 5
// otherwise; the 419F1 path of 29514 uses k+5).
//   ASM 66448..68260 (collectors), 34375..34830 (28ED6/29273), 34904..35078 (29514), 35078..
//   (29722), 35985.. (2A1C6), 37119..37216 (2AE44), 37296.. (2B031), 37431..37582 (2B1D0),
//   37582.. (2B391), 38126 (2B9C8), 38207 (2BA8E), 38547..38715 (2B391 wrapper radius),
//   38728.. (2C00E), 39109 (2C468), 39191 (2C566).
// Only 41715 / 4189C / 42A1D consume the 1B7FA list (F0648); the others filter centre candidates
// with the mode-0 visibility marks in E8F46 (`grid_read >= 0`).
bool CombatII::battle_ai_spell_pick_target(int unit_index, int hq1, int kind, int& tx, int& ty) {
    tx = ty = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (!u.active || u.hp <= 0) return false;
    const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
    const int state = (int)u.ai_decision_state;                          // [+6Ch]
    const int caster_army = battle_effective_army(u);
    const int f = (int)u.facing & 3;                                     // [+63h]

    // sub_28465(caster, add) = cbw(184A4([5Bh], 0x36)) + add
    auto r28465 = [&](int add) -> int {
        return s16((int)(int8_t)battle_lookup_184a4(u.combat_type_5b, 0x36) + add);
    };
    auto gate = [&](const CombatUnitPlacement& o) {                      // [15h]&80 / [5Ch]<7 (sub_28596 gate)
        return o.active && o.hp > 0 && !(o.combat_flags_15 & 0x80) && o.code_b < 7;
    };
    auto hostile = [&](const CombatUnitPlacement& o) {                   // sub_1201D(caster, target) != 0
        return battle_armies_hostile(caster_army, battle_effective_army(o), battle_hostility_inverted_);
    };
    auto dist_to = [&](int x, int y) { return std::abs(ux - x) + std::abs(uy - y); }; // sub_49ADF
    auto lost_hp = [&](const CombatUnitPlacement& o) {                   // ([4Ch]-[48h]) / 64h (idiv)
        return (battle_max_hp_raw(o) - battle_hp_raw(o)) / 100;
    };
    auto wounded = [&](const CombatUnitPlacement& o) {                   // [4Ch]/64h*46h > [48h]
        return (battle_max_hp_raw(o) / 100) * 0x46 > battle_hp_raw(o);
    };

    g2range::Env pick_env = battle_range_env_make();
    g2range::Range pick_range(pick_env);
    auto occ = [&](int x, int y) -> const CombatUnitPlacement* {         // sub_11DB2
        const int t = pick_range.occupant_at(x, y);
        if (t < 0 || (size_t)t >= units_.size()) return nullptr;
        return &units_[(size_t)t];
    };
    // sub_1B7BF + sub_13F8A + sub_1B7FA (mode 0): mark the visible tiles in E8F46.
    auto los_mark = [&](int rows) { battle_ai_los_1b7fa(pick_range, unit_index, 1, 0, rows); };
    auto visible = [&](int x, int y) { return pick_range.grid_read(x, y) >= 0; };   // sub_13CBD
    // sub_1AC08/1AC59 diamond.
    auto diamond = [&](int cx, int cy, int r, auto&& fn) {
        pick_range.dia_init(cx, cy, r);
        int x = 0, y = 0;
        for (int n = 0; n < 0x10000 && pick_range.dia_next(x, y); ++n) fn(x, y);
    };
    // sub_1AF22 (front tile, cx=1, facing, rows); the first 1B00D call drops the start tile.
    auto cone_begin = [&](int rows, Wc5Iter& it) {
        const g2range::Range::Pt ft = pick_range.front_tile(battle_range_unit(unit_index), f);
        battle_ai_wc5_iter_init(it, ft.x, ft.y, 1, f, rows);
        int cx = 0, cy = 0;
        battle_ai_wc5_iter_next(it, cx, cy);
    };
    // Centre-candidate walk. With caster_first the caster's tile is the first candidate:
    // 42151/42375/
    // 4258C overwrite the 1B00D start coordinates with the caster's and then enter the loop
    // (ASM 67437..67447). 42743 does the same only when mask < 0FFh. The caster tile is
    // marked directly with 13D02(caster, 1); after it only visible cone tiles are visited.
    auto center_scan = [&](int rows, bool caster_first, auto&& visit) {
        Wc5Iter it;
        cone_begin(rows, it);
        if (caster_first) {
            pick_range.grid_write(ux, uy, 1);                            // sub_13D02(caster, 1)
            if (visible(ux, uy)) visit(ux, uy);
        }
        int cx = 0, cy = 0;
        for (int g = 0; g < 0x4000 && battle_ai_wc5_iter_next(it, cx, cy); ++g) {
            if (!visible(cx, cy)) continue;                              // sub_13CBD < 0 -> skip
            visit(cx, cy);
        }
    };
    // Common tail of the centre selectors: best != 0 is required, and a distance > 10h fails when
    // best > 0.
    auto center_finish = [&](int best, int bx, int by) -> bool {
        if (bx < 0 || s16(best) == 0) return false;
        if (dist_to(bx, by) > 0x10 && s16(best) > 0) return false;
        tx = bx; ty = by;
        return true;
    };

    // Score tables (selected by the caster's [6Ch], each adds a target field):
    //   off_419D5 / off_41BD9 (419F1, 41BF5): 0,1,5,6 = +1 / 2 = lost HP / 3 = 78h-[59h] / 4 = [43h]
    auto add_tbl_a = [&](int acc, const CombatUnitPlacement& o) -> int {
        switch (state) {
            case 2: return s16(acc) + lost_hp(o);                        // movsx ecx,cx; add
            case 3: return acc + (0x78 - (int)o.combat_def59);
            case 4: return acc + (int)o.combat_value_43;
            default: return acc + 1;
        }
    };
    //   off_42359 (42375): 0,1,5,6 = +1 / 2,3 = lost HP / 4 = [43h]
    auto add_tbl_b = [&](int acc, const CombatUnitPlacement& o) -> int {
        switch (state) {
            case 2: case 3: return s16(acc) + lost_hp(o);
            case 4: return acc + (int)o.combat_value_43;
            default: return acc + 1;
        }
    };
    //   off_41E25 (41E41, caster fields): 0,1,6 = +1 / 2,5 = lost HP / 3 = 78h-[59h] / 4 = [43h]
    auto caster_score_41e41 = [&](int acc) -> int {
        switch (state) {
            case 2: case 5: return s16(acc) + lost_hp(u);
            case 3: return acc + (0x78 - (int)u.combat_def59);
            case 4: return acc + (int)u.combat_value_43;
            default: return acc + 1;
        }
    };
    //   off_42009 (42025, caster fields): 0,1,5,6 = +1 / 2 = lost HP / 3 = 78h-[59h] / 4 = [43h]
    auto caster_score_42025 = [&](int acc) -> int {
        switch (state) {
            case 2: return s16(acc) + lost_hp(u);
            case 3: return acc + (0x78 - (int)u.combat_def59);
            case 4: return acc + (int)u.combat_value_43;
            default: return acc + 1;
        }
    };
    // Local buffer reproducing sub_28596's duplicate rejection without keeping a list.
    std::vector<int> collected;

    // sub_41715(cx): single target. Candidates are walked in the order of the front-visibility list
    // (F0648) built by sub_1B7FA. Value per state: 0/1/5/6 distance (min), 2 HP word (min),
    // 3 defence (min) then attack (max), 4 attack (max).
    auto pick_41715 = [&](int cx) -> bool {
        if (state > 6) return false;
        int best = (state == 4) ? -1 : 0x7FFF;                            // loc_41796
        int bx = -1, by = -1;
        std::vector<int> cand;
        battle_ai_collect_1b7fa(unit_index, true, r28465(cx), cand);      // 1B7BF (expect 1 = hostile)
        for (int t : cand) {
            const auto& o = units_[(size_t)t];
            if (!gate(o)) continue;                                       // [15h]&80 / [5Ch]<7
            const int x = (int)(int8_t)o.x, y = (int)(int8_t)o.y;
            if (state == 3) {                                             // loc_41827 -> loc_41841
                const int v59 = (int)o.combat_def59;
                if (v59 < best) { best = v59; bx = x; by = y; }
                const int v43 = (int)o.combat_value_43;
                if (v43 > best) { best = v43; bx = x; by = y; }
                continue;
            }
            const int v = (state == 2) ? s16(battle_hp_raw(o))            // loc_41821
                         : (state == 4) ? (int)o.combat_value_43         // loc_41841
                         : s16(dist_to(x, y));                           // loc_417F5
            if (state == 4) { if (v > best) { best = v; bx = x; by = y; } }
            else            { if (v < best) { best = v; bx = x; by = y; } }
        }
        if (bx < 0 || best == -1 || best == 0x7FFF) return false;
        if (dist_to(bx, by) > 0x10) return false;                         // sub_49ADF; cmp ax,10h
        tx = bx; ty = by;
        return true;
    };

    // sub_4189C(cx): ally buff target. Weapon class (sub_174CF) 2 or >= 6 is excluded, as is any
    // unit with [+15h]&7 set. The collected list is scanned first and the caster is appended last.
    auto pick_4189c = [&](int cx) -> bool {
        auto fighter = [&](const CombatUnitPlacement& o) {
            if (!gate(o)) return false;
            const int wc = weapon_class(o);
            if (wc == 2 || wc >= 6) return false;
            return (o.combat_flags_15 & 7) == 0;
        };
        int bx = -1, by = -1;
        std::vector<int> cand;
        battle_ai_collect_1b7fa(unit_index, false, r28465(cx), cand);      // 1B7BF (expect 0 = ally)
        for (int t : cand) {
            if (bx >= 0) break;
            if (t == unit_index) continue;
            const auto& o = units_[(size_t)t];
            if (!fighter(o)) continue;
            bx = (int)(int8_t)o.x; by = (int)(int8_t)o.y;
        }
        if (bx < 0 && fighter(u)) { bx = ux; by = uy; }                   // end of F0648 = the caster
        if (bx < 0) return false;
        if (dist_to(bx, by) > 0x10) return false;
        tx = bx; ty = by;
        return true;
    };

    // sub_419F1(cx, radius): centre whose radius-summed hostile value is the largest (strict
    // compare).
    //   Candidates come from the 1B7FA mode-0 marks, so only unoccluded cone tiles (from the second
    //   tile on) are considered.
    auto pick_419f1 = [&](int cx, int radius) -> bool {
        if (state > 6) return false;
        const int rows = r28465(cx);
        int best = 0, bx = -1, by = -1;
        los_mark(rows);                                         // 1B7BF(1,1,0) + 13F8A + 1B7FA
        center_scan(rows, false, [&](int ccx, int ccy) {
            int acc = 0;
            diamond(ccx, ccy, radius, [&](int x, int y) {
                const CombatUnitPlacement* o = occ(x, y);
                if (!o || !gate(*o) || !hostile(*o)) return;   // [15h]&80 / [5Ch]<7 / 1201D==1
                acc = add_tbl_a(acc, *o);
            });
            if (s16(acc) > s16(best)) { best = acc; bx = ccx; by = ccy; }  // cmp cx,var; jle
        });
        return center_finish(best, bx, by);
    };

    // sub_41BF5(cx): table area (28C8A). For every cone tile it scans a 9-wide, 3-long rectangle
    // (sub_1A452 count 9, range 3) at the facing-adjusted position and sums the hostile value.
    //   Facing offset (loc_41C9E..41CCB): 0 -> (-4, 0) / 1 -> (0, -4) / 2 -> (4, 0) / 3 -> (0, 4)  (x, y)
    auto pick_41bf5 = [&](int cx) -> bool {
        if (state > 6) return false;
        static constexpr int kOffX[4] = { -4, 0, 4, 0 };
        static constexpr int kOffY[4] = { 0, -4, 0, 4 };
        const int rows = r28465(cx);
        int best = 0, bx = -1, by = -1;
        los_mark(rows);                                         // 1B7BF(1,1,0) + 13F8A + 1B7FA
        center_scan(rows, false, [&](int ccx, int ccy) {
            pick_range.rect_init(ccx + kOffX[f], ccy + kOffY[f], 9, f, 3);   // sub_1A452
            int acc = 0, x = 0, y = 0;
            for (int n = 0; n < 0x10000 && pick_range.rect_next(x, y); ++n) { // sub_1A552
                const CombatUnitPlacement* o = occ(x, y);
                if (!o || !gate(*o) || !hostile(*o)) continue;
                acc = add_tbl_a(acc, *o);
            }
            if (s16(acc) > s16(best)) { best = acc; bx = ccx; by = ccy; }
        });
        return center_finish(best, bx, by);
    };

    // sub_42151(cx): Heal (2B9C8 -> 2AE44). This selects a single tile: for each cone tile (caster
    //   tile first) the unit standing there qualifies when it is an ally that is wounded or undead, or
    //   a hostile undead; every candidate must also pass [15h]&80==0, [5Ch]<7 and [61h]<12Ch.
    //   Value by caster [6Ch]: 0/1/5/6 min distance, 2/3 min HP word, 4 max [43h] (strict compare).
    //   The initial value is 0 for state 4 and 7FFFh otherwise; 0/7FFFh at the end means failure, as
    //   does a distance > 10h.
    auto pick_42151 = [&](int cx) -> bool {
        const int rows = r28465(cx);
        int best = (state == 4) ? 0 : 0x7FFF;
        int bx = -1, by = -1;
        battle_ai_los_1b7fa(pick_range, unit_index, 0, 0, rows);   // 1B7BF(army,0,0)+13F8A+1B7FA
        center_scan(rows, true, [&](int ccx, int ccy) {
            const CombatUnitPlacement* o = occ(ccx, ccy);
            if (!o) return;
            if (o->combat_flags_15 & 0x80) return;
            if (o->code_b >= 7) return;
            if (s16(o->sprite_type) >= 0x12C) return;
            if (hostile(*o)) {
                if (o->unit_kind_0d != 2) return;                       // loc_422B0
            } else if (!wounded(*o) && o->unit_kind_0d != 2) {
                return;                                                 // otherwise only the undead
            }
            if (state > 6) return;
            const int ox = (int)(int8_t)o->x, oy = (int)(int8_t)o->y;
            switch (state) {
                case 2: case 3: {                                       // loc_422F6
                    const int v = s16(battle_hp_raw(*o));
                    if (v >= s16(best)) return;
                    best = v;
                    break;
                }
                case 4: {                                               // loc_42303
                    const int v = (int)o->combat_value_43;
                    if (!(v > s16(best))) return;
                    best = v;
                    break;
                }
                default: {                                              // loc_422CA
                    const int d = s16(dist_to(ox, oy));
                    if (d >= s16(best)) return;
                    best = d;
                    break;
                }
            }
            bx = ccx; by = ccy;
        });
        if (s16(best) == 0 || s16(best) == 0x7FFF || bx < 0) return false;
        if (dist_to(bx, by) > 0x10) return false;
        tx = bx; ty = by;
        return true;
    };

    // sub_42375(cx, radius): Wide Heal (2BA8E -> 2B031). Centre (caster tile first) whose radius
    // sum
    //   of ally (wounded or undead) and hostile (undead) value is the largest. Score table off_42359.
    auto pick_42375 = [&](int cx, int radius) -> bool {
        if (state > 6) return false;
        const int rows = r28465(cx);
        int best = 0, bx = -1, by = -1;
        battle_ai_los_1b7fa(pick_range, unit_index, 0, 0, rows);   // 1B7BF(army,0,0)
        center_scan(rows, true, [&](int ccx, int ccy) {
            int acc = 0;
            diamond(ccx, ccy, radius, [&](int x, int y) {
                const CombatUnitPlacement* o = occ(x, y);
                if (!o) return;
                if (o->combat_flags_15 & 0x80) return;
                if (o->code_b >= 7) return;
                if (s16(o->sprite_type) >= 0x12C) return;
                if (hostile(*o)) {
                    if (o->unit_kind_0d != 2) return;
                } else if (!wounded(*o) && o->unit_kind_0d != 2) {
                    return;
                }
                acc = add_tbl_b(acc, *o);
            });
            if (s16(acc) > s16(best)) { best = acc; bx = ccx; by = ccy; }
        });
        return center_finish(best, bx, by);
    };

    // sub_4258C(cx, radius): Cure (2BBB7 -> 2B1D0 kind 0). The radius is k (the stack word of
    // 2B1D0).
    //   Centre (caster tile first) with the most allies whose [+1Eh] != 0.
    auto pick_4258c = [&](int cx, int radius) -> bool {
        const int rows = r28465(cx);
        int best = 0, bx = -1, by = -1;
        los_mark(rows);                                         // 1B7BF(1,1,0) + 13F8A + 1B7FA
        center_scan(rows, true, [&](int ccx, int ccy) {
            int acc = 0;
            diamond(ccx, ccy, radius, [&](int x, int y) {
                const CombatUnitPlacement* o = occ(x, y);
                if (!o || !gate(*o)) return;
                if (hostile(*o)) return;                                // 1201D != 0 -> skip
                if (o->combat_flags_1e == 0) return;                    // cmp byte [1Eh],0
                ++acc;
            });
            if (s16(acc) > s16(best)) { best = acc; bx = ccx; by = ccy; }
        });
        return center_finish(best, bx, by);
    };

    // sub_42743(cx, radius, mask): centre selector shared by 2B1D0 (Confuse), 2B391 (buff/debuff)
    //   and 2C468 (DNA Change). rows = (mask == 4000h) ? cx : 28465(cx). When mask < 0FFh the
    //   expected side is 0 (ally) and the caster tile is the first candidate; otherwise it is 1
    //   (hostile) and the walk starts at the second cone tile. Either way the cone walk runs to the
    //   end. A unit in range qualifies when [15h]&80==0, [5Ch]<7, 1201D==expected side, [61h]<12Ch
    //   and it does not already carry the mask's state. Value per mask: 40h = +2 when [1Eh]&7 /
    //   800h = +1 for a caster with MP and a known spell / 8h = skip weapon class 2 / 4000h = skip
    //   [1Eh]&20 / otherwise +1. The strictly largest sum wins.
    auto pick_42743 = [&](int cx, int radius, int mask) -> bool {
        const int rows = (mask == 0x4000) ? s16(cx) : r28465(cx);
        const bool want_hostile = (mask >= 0xFF);                       // var_8
        int best = 0, bx = -1, by = -1;
        los_mark(rows);                                         // 1B7BF(1,1,0) + 13F8A + 1B7FA
        center_scan(rows, !want_hostile, [&](int ccx, int ccy) {
            int score = 0;
            diamond(ccx, ccy, radius, [&](int x, int y) {
                const CombatUnitPlacement* o = occ(x, y);
                if (!o) return;
                if (o->combat_flags_15 & 0x80) return;                  // test [15h],80h
                if (o->code_b >= 7) return;                             // cmp [5Ch],7
                if (hostile(*o) != want_hostile) return;                // cmp 1201D, var_8
                if (s16(o->sprite_type) >= 0x12C) return;               // cmp [61h],12Ch
                if (mask < 0xFF) {
                    if (o->combat_flags_15 & (uint8_t)(mask & 0xFF)) return;      // [15h]&mask
                } else {
                    if (o->combat_flags_1e & (uint8_t)((mask >> 8) & 0xFF)) return; // [1Eh]&mask>>8
                }
                if (mask == 0x40) {                                     // loc_42952
                    if (!(o->combat_flags_1e & 0x07)) return;
                    score += 2;
                    return;
                }
                if (mask == 0x800) {                                    // loc_4295F
                    if ((uint16_t)o->combat_mp_50 == 0) return;
                    bool can_cast = false;
                    for (int gI = 0; gI < 7 && !can_cast; ++gI)
                        if (battle_spell_kind(*o, gI) >= 0) can_cast = true;       // sub_28429
                    if (!can_cast) return;
                    ++score;
                    return;
                }
                if (mask == 0x8) {                                      // loc_4293B
                    if (weapon_class(*o) == 2) return;
                    ++score;
                    return;
                }
                if (mask == 0x4000) {                                   // loc_42982
                    if (o->combat_flags_1e & 0x20) return;
                    ++score;
                    return;
                }
                ++score;                                                // loc_42988
            });
            if (s16(score) > s16(best)) { best = score; bx = ccx; by = ccy; }
        });
        return center_finish(best, bx, by);
    };

    // sub_41E41(k): AI collector of 29722 (wave spells). The origin is the 1A146 front tile (k==2
    //   uses the caster tile instead) and, when k!=0, the facing offset from loc_3C98B is added
    //   (ASM 67103..67106, 67124/67144). k==0 scans a 1x5 rectangle (sub_1A452 count 1, range 5, no
    //   offset), k==1 a cone (sub_1AF22 cx=3, rows 4, start tile included) and anything else a 3x10
    //   rectangle (count 3, range 10). 11EE4 removes the caster from the occupancy test.
    //   Qualifying: 1201D!=0, [15h]&80==0, [5Ch]<7 and no duplicate per sub_28596. The score uses the
    //   caster's [6Ch] table (off_41E25) and a sum of 0 fails (29722 loc_2976F `test ax,ax`).
    auto pick_41e41 = [&](int kk) -> bool {
        if (state > 6) return false;
        // loc_3C98B bytes (EXE VA 0x3C98B = FF 00 00 FF 01 00 00 01): facing 0..3 -> (dx, dy)
        static const int kDirOff[4][2] = { {-1, 0}, {0, -1}, {1, 0}, {0, 1} };
        int ox = ux, oy = uy;
        if (kk != 2) {
            const g2range::Range::Pt ft = pick_range.front_tile(battle_range_unit(unit_index), f);
            ox = ft.x; oy = ft.y;                                       // sub_1A146
        }
        if (kk != 0) { ox += kDirOff[f][0]; oy += kDirOff[f][1]; }
        Wc5Iter cone;
        if (kk == 0)      pick_range.rect_init(ox, oy, 1, f, 5);
        else if (kk == 1) battle_ai_wc5_iter_init(cone, ox, oy, 3, f, 4);
        else              pick_range.rect_init(ox, oy, 3, f, 10);
        collected.clear();
        int score = 0;
        for (int n = 0; n < 0x10000; ++n) {
            int x = 0, y = 0;
            const bool more = (kk == 1) ? battle_ai_wc5_iter_next(cone, x, y)
                                        : pick_range.rect_next(x, y);
            if (!more) break;
            const CombatUnitPlacement* o = occ(x, y);
            if (!o) continue;
            const int ti = (int)(o - units_.data());
            if (ti == unit_index) continue;                             // 11EE4: the caster's tile counts as empty
            if (!hostile(*o)) continue;                                 // sub_1201D
            if (o->combat_flags_15 & 0x80) continue;
            if (o->code_b >= 7) continue;
            bool dup = false;
            for (int q : collected) if (q == ti) { dup = true; break; }
            if (dup) continue;                                          // sub_28596
            collected.push_back(ti);
            score = caster_score_41e41(score);
        }
        if (s16(score) == 0) return false;
        tx = ux; ty = uy;                                              // 29722 targets the caster's tile
        return true;
    };

    // sub_42025(k, kind): AI collector of the 2A1C6 family. Radius = 28465(2k+3); kind==4 (Barrier)
    //   subtracts 2 and accepts only allies with [61h]<12Ch.
    //   Qualifying: 1201D == var_C (1 = hostile, 0 = ally), [15h]&80==0, [5Ch]<7 and no duplicate
    //   per sub_28596. The score uses the caster's [6Ch] table (off_42009) and a sum of 0 fails
    //   (2A1C6 `test ax,ax`).
    auto pick_42025 = [&](int kind20) -> bool {
        if (state > 6) return false;
        int radius = r28465(2 * kind + 3);
        if (kind20 == 4) radius = s16(radius - 2);
        const bool want_hostile = (kind20 != 4);
        collected.clear();
        int score = 0;
        diamond(ux, uy, radius, [&](int x, int y) {
            const CombatUnitPlacement* o = occ(x, y);                   // the caster is included
            if (!o) return;
            const int ti = (int)(o - units_.data());
            if (hostile(*o) != want_hostile) return;                    // cmp ax, var_C
            if (o->combat_flags_15 & 0x80) return;
            if (o->code_b >= 7) return;
            if (kind20 == 4 && s16(o->sprite_type) >= 0x12C) return;    // cmp word [61h],12Ch
            bool dup = false;
            for (int q : collected) if (q == ti) { dup = true; break; }
            if (dup) return;
            collected.push_back(ti);
            score = caster_score_42025(score);
        });
        if (s16(score) == 0) return false;
        tx = ux; ty = uy;                                              // 2A1C6 targets the caster's tile
        return true;
    };

    // sub_42A1D(cx, arg5): walks the whole collected list and keeps the last qualifying tile (no
    //   break, ASM 68180..68196). arg5==0 (2C00E Melt Item, `push 0`) skips units whose [5Ah]/[5Bh]/
    //   [5Dh]/[5Eh]/[5Fh] are all 0; arg5!=0 (2C28F Death, `push 1`) skips [61h]>=12Ch or [43h]>32h.
    //   The chosen tile must be within 10h of the caster.
    auto pick_42a1d = [&](int cx, bool melt_mode) -> bool {
        int bx = -1, by = -1;
        std::vector<int> cand;
        battle_ai_collect_1b7fa(unit_index, true, r28465(cx), cand);      // 1B7BF (expect 1 = hostile)
        for (int t : cand) {
            const auto& o = units_[(size_t)t];
            if (!gate(o)) continue;                                       // [15h]&80 / [5Ch]<7
            if (melt_mode) {
                if (o.combat_type_5a == 0 && o.combat_type_5b == 0 && o.combat_type_5d == 0 &&
                    o.inv_5e == 0 && o.inv_5f == 0) continue;             // every equipment slot is empty
            } else {
                if (s16(o.sprite_type) >= 0x12C) continue;                // loc_42AF7
                if ((int)o.combat_value_43 > 0x32) continue;
            }
            bx = (int)(int8_t)o.x; by = (int)(int8_t)o.y;                 // the last qualifying tile wins
        }
        if (bx < 0) return false;
        if (dist_to(bx, by) > 0x10) return false;
        tx = bx; ty = by;
        return true;
    };

    // ---- q1 (= R/3) -> collector and the cx/radius each handler passes (ASM call sites) ----
    // d = q1 % 7 selects the table column: 0=28ED6 (single) 1=29273 (ally buff) 2=29514 (table
    // area)
    //   3=29722 (wave) 4=summon 5=2A1C6 6=2A75A. The value each handler passes in ecx (the 28465
    //   addend): 28ED6: 7 when k>0 else 5 / 29273: 7 when k>1 else 5 / 29514: 28C8A (cx 5) when k==1,
    //   otherwise 419F1 (cx k+5, radius k+3).
    switch (hq1) {
        case 5:  return pick_42025(0);
        case 12: return pick_42025(1);
        case 19: return pick_42025(2);
        case 24: return pick_42025(3);        // 2BBDB Turn Undead
        case 33: return pick_42025(4);        // 2BF08 Barrier
        case 48: return pick_42025(5);        // 2C62C Dark Zone
        default: break;
    }
    if (hq1 >= 0 && hq1 < 21) {
        const int d = hq1 % 7;
        if (d == 0) return pick_41715(kind > 0 ? 7 : 5);          // 28ED6 -> 28982 -> 41715
        if (d == 1) return pick_4189c(kind > 1 ? 7 : 5);          // 29273 -> 4189C
        if (d == 2) return (kind == 1) ? pick_41bf5(5)            // 29514 -> 28C8A -> 41BF5
                                       : pick_419f1(kind + 5, kind + 3);   // 29514 -> 419F1
        if (d == 3) return pick_41e41(kind);                      // 29722 -> 41E41
        return false;
    }
    switch (hq1) {
        case 21: return pick_42151(kind > 1 ? 7 : 5);             // 2B9C8 Heal -> 2AE44 -> 42151
        case 22: return pick_42375(5, kind + 2);                  // 2BA8E Wide Heal -> 2B031 -> 42375
        case 23: return pick_4258c(5, kind);                      // 2BBB7 Cure -> 2B1D0(kind 0) -> 4258C
        // 2B391 wrapper (2BE3B..2BF2F): mask and radius (the value loaded into ecx before the SSP
        // call)
        case 28: return pick_42743(5, 1, 0x0008);                 // Power UP
        case 29: return pick_42743(5, 2, 0x0010);                 // Speed UP
        case 30: return pick_42743(5, 3, 0x0020);                 // Magic Armor
        case 31: return pick_42743(5, 3, 0x0040);                 // Bless
        case 32: return pick_42743(5, 1, 0x0800);                 // Silence
        case 34: return pick_42743(5, 3, 0x2000);                 // Curse
        case 42: return pick_42743(5, kind, 0x1000);              // 2BFEA Confuse -> 2B1D0 (kind 1): radius k
        case 43: return pick_42a1d(5, true);                      // 2C00E Melt Item(arg5=0)
        case 44: return pick_42a1d(5, false);                     // 2C28F Death(arg5=1)
        case 46: return pick_42743(5, 0, 0x4000);                 // 2C468 DNA Change: radius 0, rows = cx
        case 47: return pick_419f1(5, kind + 3);                  // 2C566 -> 28B24 -> 419F1
        default: break;
    }
    return false;
}

// ---- F13B0 AI path ----
int CombatII::battle_ai_cast_spell(int unit_index, int r_code) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (r_code < 0 || r_code >= 147) return -1;
    const int q1 = r_code / 3, k = r_code % 3;
    // Every F13B0[49] handler is ported. The original also does not cast when the target collector
    // fails (28982/2A1C6/2B1D0/2B391/2C468 return -1), so all handlers that use a collector are
    // listed here.
    auto needs_tile = [](int q) {
        switch (q) {
            case 0: case 1: case 2: case 3: case 7: case 8: case 9: case 10:
            case 14: case 15: case 16: case 17:  // 28ED6 / 29273 / 29514 / 29722
            case 5: case 12: case 19:            // 2A1C6 Explosion/Leopard/P.Lighting
            case 24: case 33: case 48:           // 2A1C6 Turn Undead / Barrier / Dark Zone
            case 21: case 22:                    // 42375 Heal / Wide Heal
            case 23:                             // 4258C Cure
            case 28: case 29: case 30: case 31: case 32: case 34:   // 2B391
            case 42:                             // 2B1D0 kind 1 (Confuse)
            case 43: case 44: case 46: case 47:  // 42A1D / 42743
                return true;
            default:
                return false;
        }
    };
    int tx = -1, ty = -1;
    if (!battle_ai_spell_pick_target(unit_index, q1, k, tx, ty)) {
        if (needs_tile(q1)) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: AI unit=%d spell %s no target (41715 family), cast fails\n",
                        units_[(size_t)unit_index].id, kSpellNames[r_code]);
            return -1;
        }
        tx = ty = -1;
    }
    const int rc = battle_cast_magic(unit_index, r_code, tx, ty);
    return rc == 0 ? 0 : -1;
}

// ---- sub_432A0 ----
int CombatII::battle_ai_mage_432a0(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    if (s16(units_[(size_t)unit_index].combat_mp_50) < 0x0A || battle_tp() < 0x32 ||
        (units_[(size_t)unit_index].combat_flags_1e & 8))
        return -1;
    int result = 0;                                  // var_C
    const int loops = units_[(size_t)unit_index].ai_field_7f ? 1 : 4;
    int handler_res = -1;                            // var_10
    int var14 = 0;
    for (int di = 0; di < loops; ++di) {
        CombatUnitPlacement& u = units_[(size_t)unit_index];
        int R;
        if (u.ai_field_7f) {
            R = (int)u.ai_field_7f - 1;
        } else if (di == 0 && u.ai_field_80 != 0xFF) {
            const int q1 = (int)u.ai_field_80;
            const int kd = battle_spell_kind(u, q1 / 7);
            R = kd;
            if (kd >= 0) {
                R = kd + q1 * 3;
                if (s16(u.combat_mp_50) < (int)kSpellCost[q1 % 7]) R = -1;
            }
        } else {
            R = battle_ai_mage_42dc1(unit_index, di);
            if (R == -2) return result;
        }
        // dword_F13B0 has 49 entries and the name table off_7423C has 147 (= 49*3). When the SCE
        // [7Fh]/[80h] value is out of range the original calls through a garbage pointer; the port
        // treats that input as "no spell" (not reproducible).
        if (R < -1 || R >= 147) R = -1;
        if (R != -1) {
            const int q1 = R / 3, d = q1 % 7;
            const int tp_total = s16(battle_tp()) + s16(u.combat_acc57);
            const int thr = battle_spell_thr(u, d);
            if (thr > tp_total) {
                if (battle_tp() == s16(u.combat_value_54)) {
                    if (s16(u.combat_acc57) < 0x1F4) u.combat_acc57 = (uint16_t)(u.combat_acc57 + 0x14);
                    u.ai_field_7f = (uint8_t)((R & 0xFF) + 1);
                    battle_tp_consume(s16(battle_tp()));  // sub_1A0C6 (all TP)
                    if (battle_trace_)
                        fprintf(stderr, "COMBAT: AI unit=%d spell %s waits for TP (thr=%d total=%d, [7Fh]=%d)\n",
                                u.id, kSpellNames[R], thr, tp_total, (int)u.ai_field_7f);
                    return 2;
                }
            } else {
                battle_spell_auto_ = true;           // sub_28422(1)
                handler_res = battle_ai_cast_spell(unit_index, R);
                battle_spell_auto_ = false;          // sub_28422(0)
                if (should_exit_ || (size_t)unit_index >= units_.size()) return result;
                CombatUnitPlacement& uc = units_[(size_t)unit_index];
                var14 = battle_spell_thr(uc, d) - s16(uc.combat_acc57);
                uc.ai_field_7f = 0;
                uc.combat_acc57 = 0;
            }
        }
        if (handler_res == -1) continue;             // loc_4350C: inc edi
        CombatUnitPlacement& uc = units_[(size_t)unit_index];
        const int d = (R / 3) % 7;
        uc.combat_mp_50 = (uint16_t)(s16(uc.combat_mp_50) - (int)kSpellCost[d]);
        if (s16(var14) > 0) {                        // sub_3C993(u, var_14)
            uc.combat_acc57 = 0; uc.ai_field_7f = 0; uc.ai_field_81 = 0;
            battle_tp_consume(s16(var14));
        }
        if (s16(uc.combat_mp_50) < 0) uc.combat_mp_50 = 0;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d cast %s (R=%d) MP=%d TP=%d\n", uc.id,
                    kSpellNames[R], R, (int)uc.combat_mp_50, battle_tp());
        return 1;
    }
    return result;
}

// ---- sub_3F415 ----
void CombatII::battle_ai_turn_end_3f415(int unit_index, int edx) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto sub3c993 = [&](int amount) {
        auto& u = units_[(size_t)unit_index];
        u.combat_acc57 = 0; u.ai_field_7f = 0; u.ai_field_81 = 0;
        battle_tp_consume(s16(amount));
    };
    if (s16(units_[(size_t)unit_index].sprite_type) >= 0x12C) return;
    {
        auto& u = units_[(size_t)unit_index];
        if (u.ai_field_6d != 4) {
            if (battle_class_73b59((int)u.combat_class, 0) == 1) {
                u.combat_flags_13 |= 2;
            } else if (battle_tp() > 0x3C && s16(edx) == 1) {
                sub3c993(0x3C);
                u.combat_flags_13 |= 2;
            }
        }
    }
    if (battle_tp() > 0x28) {
        sub3c993(0x28);
        units_[(size_t)unit_index].combat_flags_13 |= 1;
    }
    if (battle_tp() > 0x14) {
        camera_center_on((int)(int8_t)units_[(size_t)unit_index].x,
                         (int)(int8_t)units_[(size_t)unit_index].y);  // sub_1124A
        battle_wait_heal(unit_index);                                  // sub_4D3BA
    }
}

// ---- sub_3E3BF ----
std::vector<int> CombatII::battle_ai_3e3bf(int unit_index, int radius) const {
    std::vector<int> out;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return out;
    const auto& u = units_[(size_t)unit_index];
    if (s16(radius) < 3) radius = 3;
    mage_diamond((int)(int8_t)u.x, (int)(int8_t)u.y, radius, [&](int x, int y) {
        const int h = battle_ai_hostile_unit_at(x, y, unit_index);
        if (h >= 0) out.push_back(h);
    });
    return out;
}

// ---- sub_3E457 ----
int CombatII::battle_ai_3e457(int unit_index, int radius, std::vector<int>& out_wounded,
                              std::vector<int>& out_pre_hostiles) const {
    out_wounded.clear();
    out_pre_hostiles.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const auto& u = units_[(size_t)unit_index];
    if ((battle_max_hp_raw(u) / 10) * 7 > battle_hp_raw(u)) return -1;
    if (u.combat_flags_1e != 0) return -1;
    if (s16(radius) < 3) radius = 3;
    out_pre_hostiles = battle_ai_3e3bf(unit_index, radius);
    mage_diamond((int)(int8_t)u.x, (int)(int8_t)u.y, radius, [&](int x, int y) {
        if (x < 2 || x >= (int)map_w_ - 2 || y < 4 || y >= (int)map_h_ - 4) return;
        const int t = battle_unit_index_at(x, y);
        if (t < 0 || (size_t)t >= units_.size()) return;
        const auto& o = units_[(size_t)t];
        if (battle_armies_hostile(battle_effective_army(u), battle_effective_army(o),
                                  battle_hostility_inverted_)) return;
        if (o.unit_kind_0d == 2) return;
        if (s16(o.sprite_type) >= 0x12C) return;
        if ((battle_max_hp_raw(o) / 10) * 7 > battle_hp_raw(o) || o.combat_flags_1e != 0)
            out_wounded.push_back(t);
    });
    return (int)out_wounded.size();
}

// ---- sub_3E5AB ----
bool CombatII::battle_ai_3e5ab(int unit_index, int budget, int& out_dx, int& out_dy,
                               int& out_facing) const {
    out_dx = out_dy = out_facing = -1;
    const int t = battle_ai_watch_unit(unit_index);
    if (t < 0) return false;
    const auto& u = units_[(size_t)unit_index];
    const auto& tu = units_[(size_t)t];
    const int wc = weapon_class(u);
    std::vector<int> e8 = battle_ai_build_diffusion(unit_index, budget);
    if (e8.empty()) return false;
    const int hx = (int)(int8_t)tu.x, hy = (int)(int8_t)tu.y;
    if (wc < 6 && u.ai_decision_state == 3) {
        const int dc = battle_damage_class(u);
        const int var1c = (dc < 0) ? 0x0A : battle_damage_adjustment(tu, dc);
        const int score = battle_ai_3d3e5_cell(unit_index, wc, hx, hy, e8, budget, var1c,
                                               (int16_t)tu.combat_value_46, (int16_t)tu.combat_value_44,
                                               (int)tu.facing & 3, out_dx, out_dy, out_facing);
        return s16(score) != 0x7FFF;
    }
    const int score = battle_ai_3ca3c_cell(unit_index, wc, hx, hy, e8, budget, out_dx, out_dy, out_facing);
    return s16(score) >= 0;
}

// ---- sub_3E251 ----
// Reference (ref) = target at [+73h], watched unit at [+71h] or home ([+6F],[+70]). With a target
// the code first tries 3D7B2 (per-wc distance arm; wc 5 needs the caller's E8 diffusion) and, if
// that returns 0x7FFF, picks the minimum of score = dist + 0x12 - 2*ad2e over a radius-9 diamond
// (the first wins ties). With no candidate the reference tile is kept.
bool CombatII::battle_ai_3e251_dest(int unit_index, int& out_x, int& out_y, int& out_rx,
                                    int& out_ry, const std::vector<int>* e8) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const auto& u = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)u.x, ay = (int)(int8_t)u.y;
    // sub_3E251 reads [+73h] first; sub_3E21B (watch_unit) reads [+71h] first, so the
    // direct target is resolved here before falling back to the watch-list lookup.
    int t = -1;
    if (u.ai_target_unit >= 0 && (size_t)u.ai_target_unit < units_.size())
        t = u.ai_target_unit;
    else if (u.ai_watch_sel != 0xFF)
        t = battle_ai_watch_unit(unit_index);
    bool has_unit = false;
    int rx, ry;
    if (t >= 0) {
        rx = (int)(int8_t)units_[(size_t)t].x; ry = (int)(int8_t)units_[(size_t)t].y;
        has_unit = true;
    } else {
        rx = (int)u.ai_home_x; ry = (int)u.ai_home_y;
    }
    out_rx = rx; out_ry = ry;
    if (rx < 0 || ry < 0 || rx >= (int)map_w_ || ry >= (int)map_h_) return false;
    const int mobility = battle_ai_mobility_selector(u);
    if (has_unit) {
        // loc_3E2B1: 3D7B2 picks the nearest legal destination around the target/watch
        // unit; only when it returns 0x7FFF does the radius-9 home/observation scan run.
        int bx = -1, by = -1;
        const int score = battle_ai_3d7b2_dest(unit_index, rx, ry, bx, by, e8);
        if (score != 0x7FFF) { out_x = bx; out_y = by; return true; }
    }
    int best_score = 0x7FFF, bx = -1, by = -1;
    const int radius = 9;
    for (int dy = -radius; dy <= radius; ++dy) {
        const int half = radius - (dy < 0 ? -dy : dy);
        const int width = half * 2 + 1;
        for (int col = 0; col < width; ++col) {
            const int x = rx - half + col, y = ry + dy;
            const int ad2e = std::min(col, width - 1 - col);
            if (x == ax && y == ay) continue;
            if (!battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility)) continue;
            const int score = std::abs(ax - x) + std::abs(ay - y) + 0x12 - 2 * ad2e;
            if (score < best_score) { best_score = score; bx = x; by = y; }
        }
    }
    if (bx < 0) { bx = rx; by = ry; }
    out_x = bx; out_y = by;
    return true;
}

// ---- sub_3D7B2 ----
// Candidate destinations around a hostile, scored by sub_49ADF (manhattan distance from
// the actor). Dispatch (dasm 60799..60828): wc<=3 walks the sub_1A452 facing strip
// (loc_3D8B4) and applies sub_16699 with the candidate facing; wc 4 uses loc_3D976's
// single sub_16699 candidate per facing; wc 5 uses the WC5 1CB53/1C685 distance chain
// when the caller supplies its E8 diffusion (loc_3D9D7), and the adjacent fallback
// otherwise; wc 6..9 and 0xD/0xE scan the sub_1A452 band (loc_3DB17) through
// sub_16699 (loc_3DB6B); wc 0xA (cmp dl,0Bh; jb loc_3DAA2) and wc>0x13 have no
// candidate; wc 0xB/0xC use the sub_1A7BA mode-3 distance band (loc_3DBDA);
// wc 0xF uses the sub_1AC08 diamond through sub_11DB2 (loc_3DC49); wc 0x10 uses a
// radius-3 diamond through sub_11DB2 (loc_3DCD8); wc 0x11..0x13 scan their bands
// through sub_11DB2 (loc_3DD58/3DDFC/3DEAF).
int CombatII::battle_ai_3d7b2_dest(int unit_index, int hx, int hy, int& out_x,
                                   int& out_y, const std::vector<int>* e8) const {
    out_x = out_y = -1;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0x7FFF;
    const auto& u = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)u.x, ay = (int)(int8_t)u.y;
    const int wc = weapon_class(u);
    const int mobility = battle_ai_mobility_selector(u);
    if (wc == 5 && e8 && e8->size() == (size_t)map_w_ * (size_t)map_h_) {
        int wx = -1, wy = -1;
        const int score = battle_ai_wc5_3d7b2(unit_index, hx, hy, *e8, wx, wy);
        if (score != 0x7FFF) { out_x = wx; out_y = wy; }
        return score;
    }
    static constexpr int kStepX[4] = { 0, -1, 0, 1 };  // S,W,N,E (sub_49ABC)
    static constexpr int kStepY[4] = { 1, 0, -1, 0 };
    if (wc == 0xB || wc == 0xC) {
        // loc_3DBDA: 1B7D4(7FFFh/F0B84_hi, 3) + 1A7BA mode 3; the outer keeps the strict
        // minimum, so the shared chain ends at the best distance and its last destination.
        const int K = (wc == 0xB) ? 9 : 7;
        int chain = 0x7FFF, cx = -1, cy = -1;
        for (int f = 0; f < 4; ++f) {
            const int x0 = hx + kStepX[f], y0 = hy + kStepY[f];
            battle_ai_1a7ba_distance(unit_index, x0, y0, f, K, chain, cx, cy);
        }
        if (chain == 0x7FFF) return 0x7FFF;
        out_x = cx; out_y = cy;
        return chain;
    }
    auto empty_for_11db2 = [&](int x, int y) {
        const int o = battle_unit_index_at(x, y);
        return o < 0 || o == unit_index;  // sub_11EE4 released the actor footprint
    };
    int best = 0x7FFF, bx = -1, by = -1;
    auto consider = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) return;
        const int dist = std::abs(ax - x) + std::abs(ay - y);
        if (dist >= best) return;
        if (!empty_for_11db2(x, y)) return;
        best = dist; bx = x; by = y;
    };
    auto diamond_scan = [&](int cx, int cy, int radius, int fold_limit, bool limit_fold) {
        int gate = 0;
        for (int j = 0; j <= 2 * radius; ++j) {
            const int y = cy - radius + j;
            const int half = std::min(j, 2 * radius - j);
            const int w = 2 * half + 1;
            for (int i = 0; i < w; ++i) {
                const int x = cx - half + i;
                // sub_1AD2E: min(i+1, w-i-2), with the row-end call returning 0.
                const int fold = (i == w - 1) ? 0 : std::min(i + 1, w - i - 2);
                if (!limit_fold || gate < fold_limit) consider(x, y);
                gate = fold;  // sub_1AC59 advances before sub_1AD2E is read
            }
        }
    };
    if (wc <= 3) {
        const int rng = (wc == 3) ? 2 : 1;  // loc_3D8B4
        for (int f = 0; f < 4; ++f) {
            int x = hx + kStepX[f], y = hy + kStepY[f];
            for (int k = 0; k < rng; ++k) {
                if (!battle_ai_terrain_bucket_compatible(hx, hy, x, y)) break;  // 1A2FE
                if (battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility, f))
                    consider(x, y);  // 16699 (candidate facing f)
                x += kStepX[f]; y += kStepY[f];
            }
        }
    } else if (wc == 4) {
        // loc_3D976: one candidate per facing, gated only by sub_16699 (no 1A2FE bucket).
        for (int f = 0; f < 4; ++f) {
            const int x = hx + kStepX[f], y = hy + kStepY[f];
            if (battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility, f))
                consider(x, y);
        }
    } else if (wc == 5) {
        // Adjacent fallback only when the caller has no E8 diffusion for the 1C685 mode-3
        // chain (the mage and sub_40295 callers pass one).
        for (int f = 0; f < 4; ++f) {
            const int x = hx + kStepX[f], y = hy + kStepY[f];
            if (battle_ai_terrain_bucket_compatible(hx, hy, x, y) &&
                battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility, f))
                consider(x, y);
        }
    } else if (wc == 0x10) {
        diamond_scan(hx, hy, 3, 0, false);  // loc_3DCD8: single radius-3 diamond
    } else if (wc >= 6 && wc <= 0x13 && wc != 0xA) {  // wc 0xA: jb loc_3DAA2, no candidate
        const int rng = (wc == 8 || wc == 0xD) ? 3 : 2;  // loc_3DB17
        for (int f = 0; f < 4; ++f) {
            int cx0 = hx + kStepX[f], cy0 = hy + kStepY[f];
            if (wc == 0xF) {
                diamond_scan(cx0, cy0, 4, 3, true);  // loc_3DC49: first three folds
                continue;
            }
            if (wc == 0xD) {  // loc_3DAFC entry adjustment
                if (f == 0) cy0 += 1;
                else if (f == 1) cx0 -= 1;
            }
            int cxn = 2, scan_rng = rng;
            if (wc == 0x11) {  // loc_3DD58
                cx0 += kStepX[f] + kStepX[(f + 1) & 3];
                cy0 += kStepY[f] + kStepY[(f + 1) & 3];
                cxn = 3; scan_rng = 7;
            } else if (wc == 0x12) {  // loc_3DDFC
                cx0 += 2 * kStepX[f] + kStepX[(f + 1) & 3];
                cy0 += 2 * kStepY[f] + kStepY[(f + 1) & 3];
                cxn = 3; scan_rng = 3;
            } else if (wc == 0x13) {  // loc_3DEAF: odd facings only
                if ((f & 1) == 0) continue;
                cxn = 1; scan_rng = 5;
            } else {
                if (f == 0) cx0 -= 1;
                else if (f == 3) cy0 += 1;
            }
            // loc_3DB6B (wc 6..9, 0xD, 0xE): sub_16699 with the candidate facing.
            // loc_3DD58/3DDFC/3DEAF (wc 0x11..0x13): sub_11DB2 only (consider).
            const bool use_16699 =
                (wc >= 6 && wc <= 9) || wc == 0xD || wc == 0xE;
            AiBandIter it;
            ai_band_init(it, cx0, cy0, f, cxn, scan_rng);
            int x, y;
            while (ai_band_next(it, x, y)) {
                if (use_16699 &&
                    !battle_ai_candidate_tile_legal_1x1(x, y, unit_index, mobility, f))
                    continue;
                consider(x, y);
            }
        }
    }
    if (bx < 0) return 0x7FFF;
    out_x = bx; out_y = by;
    return best;
}

// ---- sub_3DF3B ----
// For every hostile inside the radius diamond, evaluate 3D7B2 and keep the strictly
// smallest distance (first hostile wins ties).
bool CombatII::battle_ai_3df3b_dest(int unit_index, int radius, int& out_x, int& out_y,
                                    int& out_hx, int& out_hy, const std::vector<int>* e8) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const auto& u = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)u.x, ay = (int)(int8_t)u.y;
    int best = 0x7FFF;
    bool found = false;
    mage_diamond(ax, ay, radius, [&](int x, int y) {
        if (battle_ai_hostile_unit_at(x, y, unit_index) < 0) return;
        int dx = -1, dy = -1;
        const int score = battle_ai_3d7b2_dest(unit_index, x, y, dx, dy, e8);
        if (score >= best) return;
        best = score; out_x = dx; out_y = dy; out_hx = x; out_hy = y;
        found = true;
    });
    return found;
}

// ---- sub_3E01B ----
bool CombatII::battle_ai_3e01b_dest(int unit_index, int radius, int& out_x, int& out_y,
                                    int& out_facing) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    const auto& u = units_[(size_t)unit_index];
    const int ax = (int)(int8_t)u.x, ay = (int)(int8_t)u.y;
    int best = 0;
    bool found = false;
    out_facing = (int)u.facing & 3;
    mage_diamond(ax, ay, radius, [&](int x, int y) {
        if (x >= 2 && x < (int)map_w_ - 2 && y >= 4 && y < (int)map_h_ - 4) {
            const int occ = battle_unit_index_at(x, y);
            if (occ >= 0 && occ != unit_index) return;        // the caster's footprint is already released
        }
        int hist[4] = { 0, 0, 0, 0 };
        int score = 0;
        mage_diamond(x, y, 3, [&](int sx, int sy) {
            if (sx < 2 || sx >= (int)map_w_ - 2 || sy < 4 || sy >= (int)map_h_ - 4) return;
            const int t = battle_unit_index_at(sx, sy);
            if (t < 0 || t == unit_index || (size_t)t >= units_.size()) return;
            const auto& o = units_[(size_t)t];
            if (battle_armies_hostile(battle_effective_army(u), battle_effective_army(o),
                                      battle_hostility_inverted_)) return;
            hist[(int)o.facing & 3]++;
            score += 10;
        });
        score += radius - (std::abs(ax - x) + std::abs(ay - y));
        if (s16(score) <= best) return;
        best = s16(score);
        out_x = x; out_y = y;
        int bh = 0;
        for (int i = 0; i < 4; ++i)
            if (bh < hist[i]) { bh = hist[i]; out_facing = i; }
        found = true;
    });
    return found;
}

// ---- sub_43526 (type 0) / sub_438AE (type 1) ----
bool CombatII::battle_ai_mage_turn(int ui, int type) {
    if (ui < 0 || (size_t)ui >= units_.size()) return false;
    const int actor_id = units_[(size_t)ui].id;
    auto alive = [&]() {
        return !should_exit_ && (size_t)ui < units_.size() && units_[(size_t)ui].active &&
               units_[(size_t)ui].id == actor_id;
    };
    auto U = [&]() -> CombatUnitPlacement& { return units_[(size_t)ui]; };
    auto trace = [&](const char* what, int di, int S) {
        if (battle_trace_)
            fprintf(stderr, "COMBAT: AI unit=%d mage%d %s di=%d S=%d TP=%d pos=(%d,%d)\n", actor_id,
                    type, what, di, S, battle_tp(), (int)(int8_t)U().x, (int)(int8_t)U().y);
    };
    if (s16(U().combat_mp_50) < 0x0A) return true;                // loc_438A6 (no end-of-turn settlement)
    int T = 0x0A, P = 0x14;                                       // var_8, var_C
    auto end_turn = [&]() { if (alive()) battle_ai_turn_end_3f415(ui, 1); };
    if (type == 1 && U().ai_field_7f != 0) {
        if (battle_ai_mage_432a0(ui) == 2) return true;
        if (!alive()) return true;
    }
    enum class Mv { Exit, Stay, Walked };
    // sub_15523 route + sub_1620A walk + 3C993 settlement + 167C9/5401B/17310.
    auto do_walk = [&](int dx, int dy, int facing, int S, int ref_x, int ref_y, bool facing_to_ref) -> Mv {
        (void)facing_to_ref;  // 14FA4 returns the seed/dest -> ref facing directly.
        if (ref_x < 0 || ref_y < 0) { ref_x = dx; ref_y = dy; }  // 43526 tail always has a valid ref
        battle_ai_budget_override_ = S * 4;
        // The 43526/438AE movement tail runs 14300(D2)/1419C/1499C/15342/15523/14FA4 in that order.
        // The route comes from the route pipeline inside branch_walk, and the facing is overwritten
        // by 14FA4's return value (var_8 -> 1620A).
        const bool r = battle_ai_wc1_branch_walk(ui, dx, dy, facing, S * 4, T, ref_x, ref_y, true);
        battle_ai_budget_override_ = -1;
        if (!r || !alive()) return Mv::Exit;
        T = battle_move_again_threshold(ui);                      // sub_167C9
        return Mv::Walked;
    };
    for (int guard = 0; guard < 256; ++guard) {
        if (!alive()) return true;
        if (type == 0) {
            int r, n = 0;
            do {
                r = battle_ai_mage_432a0(ui);
                if (!alive()) return true;
            } while (r == 1 && ++n < 32);
            if (r == 2) { end_turn(); return true; }
        }
        const int tp = battle_tp();
        int di = -1, S = 0;
        int dx = -1, dy = -1, face = -1, hostile = -1;
        if (tp <= T + 0x18) {
            di = -3;
        } else {
            S = (tp - T) / 10;
            const int budget = S * 4;
            int w = battle_ai_watch_state(ui);
            if (w == -2) w = 0;                                   // unmodelled selector: treated as no watch target
            const bool use_5ab = (type == 0) ? (w > 0) : (w != 0);
            if (use_5ab) {
                if (battle_ai_3e5ab(ui, budget, dx, dy, face)) {
                    di = (dx == (int)(int8_t)U().x && dy == (int)(int8_t)U().y &&
                          face == (int)U().facing) ? -2 : 1;
                }
            } else if (w == -1 || U().ai_decision_state == 6) {
                di = -2;
            } else if (type == 0) {
                const std::vector<int> hostiles = battle_ai_3e3bf(ui, S);
                if (!hostiles.empty() &&
                    battle_ai_tact_3e739(ui, 2, S, budget, hostiles, dx, dy, face, hostile) == 1) {
                    di = (dx == (int)(int8_t)U().x && dy == (int)(int8_t)U().y &&
                          face == (int)U().facing) ? -2 : 1;
                }
            } else {
                std::vector<int> wounded, pre;
                const int n = battle_ai_3e457(ui, S, wounded, pre);
                if (n < 0) {
                    di = -2;
                } else if (n > 0 &&
                           battle_ai_tact_3e739(ui, 1, S, budget, wounded, dx, dy, face, hostile, &pre) == 1) {
                    if (dx == (int)(int8_t)U().x && dy == (int)(int8_t)U().y &&
                        face == (int)U().facing)
                        di = -2;                                   // 438AE: di stays -1 when it differs
                }
            }
            trace("decide", di, S);
            if (di == 1) {                                         // 15523 route -> 1620A walk
                // 43526/438AE pass the selected unit reference to 14FA4. Use the
                // 3E739 hostile, else the 3E5AB watch target; the destination is the
                // last-resort reference when neither exists.
                int ref_x = -1, ref_y = -1;
                if (hostile >= 0 && (size_t)hostile < units_.size()) {
                    ref_x = (int)(int8_t)units_[(size_t)hostile].x;
                    ref_y = (int)(int8_t)units_[(size_t)hostile].y;
                } else {
                    const int wt = battle_ai_watch_unit(ui);
                    if (wt >= 0 && (size_t)wt < units_.size()) {
                        ref_x = (int)(int8_t)units_[(size_t)wt].x;
                        ref_y = (int)(int8_t)units_[(size_t)wt].y;
                    }
                }
                const Mv m = do_walk(dx, dy, face, S, ref_x, ref_y, false);
                if (m == Mv::Exit) return true;
                if (m == Mv::Stay) di = -2;
            } else if (di == -1) {
                // loc_436A6 / loc_43A0B: home/group move when no target was found.
                int w2 = battle_ai_watch_state(ui);
                if (w2 == -2) w2 = 0;
                int cx = -1, cy = -1, rx = -1, ry = -1, cf = -1;
                bool to_ref = true;
                std::vector<int> dest_e8 = battle_ai_build_diffusion(ui, budget);
                const std::vector<int>* dest_grid = dest_e8.empty() ? nullptr : &dest_e8;
                if (w2 != 0) {
                    if (!battle_ai_3e251_dest(ui, cx, cy, rx, ry, dest_grid)) return true;
                } else if (type == 0) {
                    if (!battle_ai_3df3b_dest(ui, 0x1E, cx, cy, rx, ry, dest_grid)) return true;
                    if (cx == (int)(int8_t)U().x && cy == (int)(int8_t)U().y) return true;  // 3DF3B returned 0
                } else {
                    if (!battle_ai_3e01b_dest(ui, 0x1E, cx, cy, cf)) return true;
                    rx = cx; ry = cy;
                    to_ref = false;                                // facing = the most common ally direction
                }
                if (cx == (int)(int8_t)U().x && cy == (int)(int8_t)U().y) {
                    di = -2;                                       // loc_43721 / loc_43A90
                } else {
                    const Mv m = do_walk(cx, cy, to_ref ? -1 : cf, S, rx, ry, to_ref);
                    if (m == Mv::Exit) return true;
                    di = (m == Mv::Stay) ? -2 : 1;
                    trace("wander", di, S);
                }
            }
        }
        if (!alive()) return true;
        if (type == 0) {
            if (di == -2) {
                if (battle_tp() < P) { end_turn(); return true; }
                P += 2;
            }
            if (di == -3) { end_turn(); return true; }
        } else {
            if (di == -2) {
                if (battle_tp() < P) { end_turn(); return true; }
                P += 2;
            }
            if (di != -3) {
                for (int n = 0; n < 9; ++n) {
                    const int r = battle_ai_mage_432a0(ui);
                    if (!alive()) return true;
                    if (r != 0) break;
                    if (battle_tp() == 0) break;
                }
                if (battle_tp() == 0) di = -3;
            }
            if (di == -3) { end_turn(); return true; }
        }
    }
    end_turn();
    return true;
}
