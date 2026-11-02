// combatii_aiturn: sub_40295 -> sub_4016A family (scan, attack, forced weapon, magic dispatch).
// dasm: 4016A(64562) 3FFD4(64420) 3F9AD(63868) 3F516/3F5E8/3F6C1/3F797/3F7F3/3F83C/3F8A7/3F8F6
//       (63400..63860) 3F4A1(63357) 3B9E1(58307) 3C30A(59065) 3F321(63212).
// The original is authoritative. F1474 vectors 13..24 and the swirl/curse effects are covered by
// the approximations and stubs in this file.
#include "combatii.h"
#include "combatii_internal.h"
#include "battle_tables.h"
#include "combat_class_stat_table.h"
#include "combat_tables.h"

namespace {

inline int s16(int v) { return (int16_t)v; }

// sub_1AC08/sub_1AC59 diamond (y ascending, x ascending within a row); radius 0 is the centre only.
template <typename Fn>
void ring_diamond(int cx, int cy, int r, Fn&& fn) {
    if (r < 0) return;
    for (int j = 0; j <= 2 * r; ++j) {
        const int y = cy - r + j;
        const int half = std::min(j, 2 * r - j);
        for (int x = cx - half; x <= cx + half; ++x) fn(x, y);
    }
}

}  // namespace

// sub_3C993: clear the AI action fields and consume `amount` TP.
void CombatII::battle_ai_3c993(int unit_index, int amount) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    u.combat_acc57 = 0;
    u.ai_field_7f = 0;
    u.ai_field_81 = 0;
    battle_tp_consume(s16(amount));
}

// sub_3B9E1: swirl. Collects hostile units into F0648 with sub_1A25F(actor) over the 1AC08(x,y,1)
// diamond and returns -1 when it is empty. Then plays the 12-frame swirl.nsp spin, sub_28728(6000,
// 20, 1) and sub_1ED45; the 12 rotations restore the facing.
int CombatII::battle_ai_3b9e1(int unit_index) {
    battle_target_list_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    const g2range::Unit actor = battle_range_unit(unit_index);
    rg.dia_init(actor.x, actor.y, 1);
    int cx = 0, cy = 0;
    while (rg.dia_next(cx, cy)) rg.add_by_actor(cx, cy, actor);
    battle_target_list_ = rg.list;
    if (battle_target_list_.empty()) return -1;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d swirl (sub_3B9E1) targets=%zu\n",
                units_[(size_t)unit_index].id, battle_target_list_.size());
    // 3B9E1 effect body: 1FEA4("swirl.nsp") -> 135EB -> 1033B(5) -> 12 frames (facing +1 per frame,
    // frame i/3) -> 1FFCD.
    battle_fx_3b9e1(unit_index, actor.x, actor.y, 0);
    if (should_exit_) return 1;
    const std::vector<int> victims = battle_target_list_;
    battle_apply_spell_hit(unit_index, victims, 0x1770, 0x14, 1);
    battle_target_list_.clear();
    return 1;
}

// sub_3C30A: curse. Checks the tile the unit faces and applies the debuff on a hit.
int CombatII::battle_ai_3c30a(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    int x = (int)(int8_t)u.x, y = (int)(int8_t)u.y;
    switch ((uint8_t)u.facing) {  // sub_49ABC
        case 0: ++y; break;
        case 1: --x; break;
        case 2: --y; break;
        case 3: ++x; break;
        default: break;
    }
    const int t = battle_ai_hostile_unit_at(x, y, unit_index);  // sub_11DB2 + sub_1201D
    if (t < 0) return 0;
    if (s16(units_[(size_t)t].sprite_type) >= 0x12C) return 0;
    u.anim_frame = 7;  // [64h]=7
    if (battle_trace_)
        fprintf(stderr, "COMBAT: AI unit=%d curse (sub_3C30A) target=%d\n", u.id, units_[(size_t)t].id);
    battle_unit_label(unit_index, "Curse");  // sub_48612(unit, "Curse", 0x11), 36 frames
    if (should_exit_ || (size_t)t >= units_.size()) return 1;
    CombatUnitPlacement& tg = units_[(size_t)t];
    tg.combat_flags_1e = (uint8_t)(tg.combat_flags_1e | 0x20);
    tg.ail_25 = 0x3C;
    reset_unit_pose(units_[(size_t)unit_index]);  // sub_12124
    return 1;
}

// sub_3F4A1: special-action gate for class groups 731B1 (swirl) and 731B8 (curse).
int CombatII::battle_ai_3f4a1(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    if (combat_rand(3) != 0) return 0;  // sub_499BD(3) != 0 -> 0
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const int cls = (int)u.combat_class;
    if (class_stat_731b1(cls) == 1) {
        const int x = (int)(int8_t)u.x, y = (int)(int8_t)u.y;
        int al = 0;  // sub_1093D (0 outside the map)
        if (x >= 0 && y >= 0 && x < (int)map_w_ && y < (int)map_h_ &&
            (size_t)y * map_w_ + (size_t)x < tile_attr_.size())
            al = (int)tile_attr_[(size_t)y * map_w_ + (size_t)x];
        const int ah = (al - 0x64) & 0x0F;
        if ((int8_t)al <= 0x64) return 0;  // `cmp al,64h; jle`
        if (ah != 2 && ah != 3) return 0;
        return battle_ai_3b9e1(unit_index) == 1 ? 1 : 0;
    }
    if (class_stat_731b8(cls) == 1) return battle_ai_3c30a(unit_index) == 1 ? 1 : 0;
    return 0;
}

// ---- Helpers used by 3F9AD ----
// sub_3F516(u, dx): 0 when [13h]&1Ch. Over the 2CBFB(u,dx) radius diamond it counts hostile units
// with [56h] >= speed_table/2 as n and returns 1 when rand(100) < 100n/16. (F0B9C+2=0 resets the
// F0648 count to zero.)
int CombatII::battle_ai_3f516(int unit_index, int dx) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (u.combat_flags_13 & 0x1C) return 0;
    battle_target_list_.clear();
    const int r = (int)(int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38) + s16(dx);  // sub_2CBFB
    int n = 0;
    ring_diamond((int)(int8_t)u.x, (int)(int8_t)u.y, r, [&](int x, int y) {
        const int t = battle_ai_hostile_unit_at(x, y, unit_index);
        if (t < 0) return;
        const CombatUnitPlacement& o = units_[(size_t)t];
        const int thr = (int)(kBattleSpeedTable[o.combat_class] >> 1);  // dword_71B6C[cls*8] >> 17
        if ((int)o.battle_gauge >= thr) ++n;
    });
    const int prob = (100 * n) / 16;
    return combat_rand(100) < prob ? 1 : 0;
}

// sub_3F5E8: 0 when [13h]&1Ch. In the radius-5 diamond it counts hostile units of weapon class
// {0,1,3,4} with [56h] >= speed_table/2 as n, sets s = 3n (50 when above 100) and returns 1 when
// rand(100) < s.
int CombatII::battle_ai_3f5e8(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (u.combat_flags_13 & 0x1C) return 0;
    int n = 0;
    ring_diamond((int)(int8_t)u.x, (int)(int8_t)u.y, 5, [&](int x, int y) {
        const int t = battle_ai_hostile_unit_at(x, y, unit_index);
        if (t < 0) return;
        const CombatUnitPlacement& o = units_[(size_t)t];
        const int wc = weapon_class(o) & 0xFF;
        if (wc == 2 || wc >= 5) return;
        const int thr = (int)(kBattleSpeedTable[o.combat_class] >> 1);
        if ((int)o.battle_gauge >= thr) ++n;
    });
    int s = n * 3;
    if (s16(s) > 100) s = 0x32;
    return combat_rand(100) < s ? 1 : 0;
}

// sub_3F6C1: store the first hostile found in the forward rectangle (range = 2CBFB(u, 0xA/6/7/5 by
// wc)) into F0648 and, when one exists, return 1 if rand(3) != 0.
int CombatII::battle_ai_3f6c1(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const int wc = weapon_class(u) & 0xFF;
    int add = 0;  // wc 2 and 5+ reuse edx's leftover in the original, but only 0/1/3/4 reach this arm
    switch (wc) {
        case 0: add = 0x0A; break;
        case 1: add = 6; break;
        case 3: add = 7; break;
        case 4: add = 5; break;
        default: break;
    }
    const int r = (int)(int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38) + add;
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    rg.rect_init((int)(int8_t)u.x, (int)(int8_t)u.y, 1, (int)u.facing & 3, s16(r));  // sub_1A452
    int cx = 0, cy = 0;
    if (wc < 4) rg.rect_next(cx, cy);  // skip the starting cell (`cmp di,4; jge`)
    rg.list.clear();                  // F0B9C+2 = 0
    const int army = battle_effective_army(u);
    while (rg.rect_next(cx, cy)) {
        if (rg.add_by_army(cx, cy, army)) break;  // stop when sub_1A1F8 returns 1
    }
    battle_target_list_ = rg.list;
    if (!battle_target_list_.empty() && combat_rand(3) != 0) return 1;
    return 0;
}

// sub_3F797: 0 when [13h]&60h, 1 when [13h]&1Ch (no random draw). Otherwise 1 when the 1D124 list
// is non-empty and rand(100) < TP-150.
int CombatII::battle_ai_3f797(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const uint8_t f13 = units_[(size_t)unit_index].combat_flags_13;
    if (f13 & 0x60) return 0;
    if (f13 & 0x1C) return 1;
    if (battle_range_collect(unit_index) <= 0) return 0;
    const int tp = s16(battle_tp());
    return combat_rand(100) < tp - 0x96 ? 1 : 0;
}

// sub_3F7F3: rand(100) < 0xAA - morale (low word of F0BA0 when the army [0Eh] <= 2, else high
// word).
int CombatII::battle_ai_3f7f3(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const int army = battle_effective_army(units_[(size_t)unit_index]);
    const int r = combat_rand(100);
    const int morale = army > 2 ? (int)morale_hi_ : (int)morale_lo_;
    return r < 0xAA - morale ? 1 : 0;
}

// sub_3F83C: skip the starting cell of the forward rectangle (2CBFB(u,5)) and scan until
// sub_1A25F(actor) returns 1.
int CombatII::battle_ai_3f83c(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const int r = (int)(int8_t)battle_lookup_184a4(u.combat_type_5b, 0x38) + 5;
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    rg.list = battle_target_list_;  // F0648 is global, so the previous list still feeds the duplicate check
    const g2range::Unit actor = battle_range_unit(unit_index);
    rg.rect_init(actor.x, actor.y, 1, (int)u.facing & 3, s16(r));
    int cx = 0, cy = 0;
    rg.rect_next(cx, cy);
    int result = 0;
    while (rg.rect_next(cx, cy)) {
        if (rg.add_by_actor(cx, cy, actor)) { result = 1; break; }
    }
    battle_target_list_ = rg.list;
    return result;
}

// sub_3F8A7: scan the radius-5 diamond until sub_1A1F8 (allegiance) returns 1.
int CombatII::battle_ai_3f8a7(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    rg.list = battle_target_list_;
    rg.dia_init((int)(int8_t)u.x, (int)(int8_t)u.y, 5);
    const int army = battle_effective_army(u);
    int cx = 0, cy = 0, result = 0;
    while (rg.dia_next(cx, cy)) {
        if (rg.add_by_army(cx, cy, army)) { result = 1; break; }
    }
    battle_target_list_ = rg.list;
    return result;
}

// sub_3F8F6: collect every hostile in the radius-1 diamond into F0648 (starting from an empty
// list) and, when there are n, return 1 with rand(100) < 25n.
int CombatII::battle_ai_3f8f6(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 0;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    const g2range::Env env = battle_range_env_make();
    g2range::Range rg(env);
    rg.dia_init((int)(int8_t)u.x, (int)(int8_t)u.y, 1);
    rg.list.clear();
    const int army = battle_effective_army(u);
    int cx = 0, cy = 0;
    while (rg.dia_next(cx, cy)) rg.add_by_army(cx, cy, army);
    battle_target_list_ = rg.list;
    const int n = (int)battle_target_list_.size();
    if (n == 0) return 0;
    return combat_rand(100) < (100 * n) / 4 ? 1 : 0;
}

// sub_3F9AD: pick a weapon index (0..0x18). Class arm: wc1 0..6 ([38h]) / wc3 7..10 ([39h]) /
// wc4 11..14 ([3Ah]) / wc5 15..18 ([3Bh]) / wc0 19..24 ([3Ch]). bx != 0xFF restricts the candidate
// to that weapon; flags[] is relative to the arm base (idx-base). The call order matches the
// original, including the random draws: the helpers run before the flags check.
int CombatII::battle_ai_3f9ad(int unit_index, int si, int bx) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    int16_t flags[7] = {0, 0, 0, 0, 0, 0, 0};
    const int tp = s16(battle_tp());
    const bool full = (tp == (int)(int16_t)u.combat_value_54);  // dl
    const int wc = weapon_class(u) & 0xFF;
    if (wc > 5) return -1;
    auto T = [&](int idx) { return (int)battle_weapon_tp_any(idx); };
    // Shared arm setup; false -> -1.
    auto setup = [&](int count, int base) {
        if (count == 0) return false;
        if (bx != 0xFF) {
            if (bx - base >= 0 && bx - base < 7) flags[bx - base] = 1;
            if (tp < T(bx) && !full) return false;
        } else if (!full) {
            for (int i = 0; i < count && i < 7; ++i)
                if (tp >= T(base + i)) flags[i] = 1;
        } else {
            for (int i = 0; i < count && i < 7; ++i) flags[i] = 1;
        }
        return true;
    };
    switch (wc) {
        case 1: {  // loc_3F9F0
            if (!setup(u.weap_cnt_38, 0)) return -1;
            if (battle_ai_3f5e8(unit_index) == 1 && flags[2] == 1) return 2;
            if (battle_ai_3f516(unit_index, si) == 1 && flags[1] == 1) return 1;
            if (battle_ai_3f797(unit_index) == 1 && flags[4] == 1) return 4;
            if (battle_ai_3f7f3(unit_index) == 1) {
                if (combat_rand(3) != 0 && flags[5] == 1) {
                    if (battle_ai_3f83c(unit_index) == 1) return 5;
                } else if (flags[6] == 1) {
                    if (battle_ai_3f8a7(unit_index) == 1) return 6;
                }
            }
            if (battle_ai_3f6c1(unit_index) == 1 && flags[3] == 1) return 3;
            if (battle_range_collect(unit_index) > 0 && flags[0] == 1) return 0;
            return -1;
        }
        case 3: {  // loc_3FB71
            if (!setup(u.weap_cnt_39, 7)) return -1;
            if (battle_ai_3f516(unit_index, si) == 1 && flags[0] == 1) return 7;
            if (battle_ai_3f7f3(unit_index) == 1) {
                if (combat_rand(3) != 0 && flags[2] == 1) return 9;
                if (flags[3] == 1 && battle_ai_3f83c(unit_index) == 1) return 0x0A;
            }
            if (battle_range_collect(unit_index) <= 0) return -1;
            if (flags[1] == 1) return 8;
            return -1;
        }
        case 4: {  // loc_3FC97
            if (!setup(u.weap_cnt_3a, 11)) return -1;
            if (battle_ai_3f797(unit_index) == 1 && flags[1] == 1) return 0x0C;
            if (battle_ai_3f8f6(unit_index) == 1 && flags[3] == 1) return 0x0E;
            if (battle_ai_3f6c1(unit_index) == 1 && flags[2] == 1) return 0x0D;
            if (battle_range_collect(unit_index) > 0 && flags[0] == 1) return 0x0B;
            return -1;
        }
        case 5: {  // loc_3FDA4
            const int count = u.weap_cnt_3b;
            if (count == 0) return -1;
            if (bx != 0xFF) {  // forced weapon: return it when TP suffices or the gauge is full
                if (tp < T(bx) && !full) return -1;
                return bx;
            }
            if (!full) {
                for (int i = 0; i < count && i < 7; ++i)
                    if (tp >= T(15 + i)) flags[i] = 1;
            } else {
                for (int i = 0; i < count && i < 7; ++i) flags[i] = 1;
            }
            if (battle_ai_3f7f3(unit_index) == 1 && flags[2] == 1) return 0x11;
            if (battle_range_collect(unit_index) <= 0) return -1;
            if (combat_rand(3) != 0 && flags[0] == 1) return 0x0F;
            if (flags[1] == 1) return 0x10;
            return -1;
        }
        case 0: {  // loc_3FE94
            if (!setup(u.weap_cnt_3c, 19)) return -1;
            if (battle_ai_3f797(unit_index) == 1 && flags[1] == 1) return 0x14;
            if (battle_ai_3f7f3(unit_index) == 1) {
                if (battle_range_collect(unit_index) > 0) {
                    const int e = combat_rand(2) + 4;
                    if (flags[e] == 1) return e + 0x13;
                }
            }
            if (battle_ai_3f6c1(unit_index) == 1 && flags[2] == 1) return 0x15;
            if (battle_range_collect(unit_index) <= 0) return -1;
            if (flags[0] == 1) return 0x13;
            return -1;
        }
        default: break;  // wc 2 always fails
    }
    return -1;
}

// sub_3FFD4: choose a weapon from [81h] (pending weapon index + 1), [82h] (player-forced slot) or
// automatic selection (3F9AD). When TP + [57h] >= the threshold (word_753C4[idx]) it runs
// F1474[idx]
// (dx from the 2CA40 stage), then the self-damage (301BB) and the TP cost.
// Returns 1 = used, 2 = waiting for TP (reserved, [81h] = idx + 1), 0/-1 = unusable.
int CombatII::battle_ai_3ffd4(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    {
        const CombatUnitPlacement& u = units_[(size_t)unit_index];
        if (battle_hp_raw(u) < 0xC8 && s16(u.combat_value_54) < 0x32) return -1;
    }
    const int si = battle_weapon_2ca40(unit_index);  // 1..3
    int var4;
    {
        const CombatUnitPlacement& u = units_[(size_t)unit_index];
        if (u.ai_field_81 != 0) {
            var4 = (int)u.ai_field_81 - 1;
        } else if ((uint8_t)u.ai_mode != 0xFF) {
            var4 = battle_ai_3f9ad(unit_index, si, (uint8_t)u.ai_mode);
            if (var4 == -1 && (u.ai_decision_state == 1 || u.ai_decision_state == 2))
                var4 = battle_ai_3f9ad(unit_index, si, 0xFF);
        } else {
            var4 = battle_ai_3f9ad(unit_index, si, 0xFF);
        }
    }
    if (var4 == -1) return 0;
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    const int gate = (int)battle_weapon_tp_any(var4);
    const int tp = s16(battle_tp());
    int handler_res = -1;
    if (s16((int)u.combat_acc57) + tp >= gate) {
        camera_center_on((int)(int8_t)u.x, (int)(int8_t)u.y);  // sub_1124A
        const int saved_f14d8 = battle_f14d8_;
        battle_f14d8_ = 1;                                     // sub_2CA39(1): the AI targets automatically
        handler_res = battle_weapon_f1474(unit_index, var4, si);
        battle_f14d8_ = saved_f14d8;
        if (should_exit_ || (size_t)unit_index >= units_.size()) return 1;
        CombatUnitPlacement& u2 = units_[(size_t)unit_index];
        u2.combat_acc57 = 0;
        u2.ai_field_81 = 0;
        if (handler_res == -1) return 0;
    } else {
        if (tp != s16(u.combat_value_54)) return 0;
        if (s16((int)u.combat_acc57) < 0x1F4) u.combat_acc57 = (uint16_t)(u.combat_acc57 + 0x14);
        u.ai_field_81 = (uint8_t)(var4 + 1);
        battle_ai_3c993(unit_index, tp);  // 3C993 clears [81h], so rewrite it below
        units_[(size_t)unit_index].ai_field_81 = (uint8_t)(var4 + 1);
        return 2;
    }
    CombatUnitPlacement& u3 = units_[(size_t)unit_index];
    u3.combat_acc57 = 0;
    u3.ai_field_81 = 0;
    if (gate > 0) battle_tp_consume(gate);  // sub_1A0C6(753C4[idx] - [57h]=0)
    battle_weapon_301bb(unit_index, var4, 0);  // fixed self-damage (only idx 0..12 implemented)
    return 1;
}

// sub_4016A: AI turn dispatch by decision state (1 = weapon, 2 = magic, 0 = scan).
int CombatII::battle_ai_4016a(int unit_index, bool allow_scan) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return -1;
    const int st = battle_ai_3f321_state(units_[(size_t)unit_index]);
    int edi = st;
    int var4 = 0;
    bool var4_set = false;
    if (st == 1) {
        var4 = battle_ai_3ffd4(unit_index);
        var4_set = true;
        if (should_exit_) return -2;
        if (var4 == 1) return edi;
        if (var4 == 2) return -2;
    } else if (st == 2) {
        int tries = 0, r = 0;
        while (tries < 9) {  // loc_40246: at most 9 calls to sub_432A0
            r = battle_ai_mage_432a0(unit_index);
            if (should_exit_) return -2;
            if (r > 0) break;
            ++tries;
        }
        if (tries == 9) return -1;
        return r == 2 ? -2 : r;
    } else if (st != 0) {
        return st;  // -1
    }
    // loc_401C0: 1D124 threat scan -> TP >= 0x28 and [6Dh] != 4 -> 1124A -> 3F4A1 -> 1F738.
    if (units_[(size_t)unit_index].sprite_type < 0 || !allow_scan) return -1;
    if (battle_range_collect(unit_index) <= 0) return -1;
    if (battle_tp() < 0x28) return -1;
    if (units_[(size_t)unit_index].ai_field_6d == 4) return -1;
    camera_center_on((int)(int8_t)units_[(size_t)unit_index].x, (int)(int8_t)units_[(size_t)unit_index].y);
    const int special = battle_ai_3f4a1(unit_index);
    if (should_exit_ || (size_t)unit_index >= units_.size()) return -2;
    int res;
    if (special == 0) {
        battle_atk_wc_ = weapon_class(units_[(size_t)unit_index]);
        res = battle_attack_execute(unit_index);  // sub_1F738 (non-targeted weapon: use the 1D124 list)
    } else {
        // The original reads [esp+var_4] here: the 3FFD4 result in state 1, otherwise an undefined
        // stack leftover.
        res = var4_set ? var4 : 1;
    }
    if (should_exit_) return -2;
    if (s16(res) == 0) return -1;
    if (s16(res) == 2) {
        battle_tp_set(0);  // sub_1A0BF(0)
        return -2;
    }
    battle_ai_3c993(unit_index, 0x28);
    return edi;
}

// sub_167C9: 10 + 30n, where n is the hostile count on the unit's own tile and its four
// neighbours (S,W,N,E), using the sub_11DB2 guard and sub_1201D.
int CombatII::battle_ai_167c9(int unit_index) const {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return 10;
    const CombatUnitPlacement& actor = units_[(size_t)unit_index];
    const int actor_eff = battle_effective_army(actor);
    static constexpr int kAdjX[5] = { 0, 0, -1, 0, 1 };
    static constexpr int kAdjY[5] = { 0, 1, 0, -1, 0 };
    int n = 0;
    for (int dir = 0; dir < 5; ++dir) {
        const int cx = (int)(int8_t)actor.x + kAdjX[dir];
        const int cy = (int)(int8_t)actor.y + kAdjY[dir];
        if (cx < 2 || cx >= (int)map_w_ - 2 || cy < 4 || cy >= (int)map_h_ - 4) continue;
        const int occ = battle_unit_index_at(cx, cy);
        if (occ < 0) continue;
        if (battle_armies_hostile(battle_effective_army(units_[(size_t)occ]), actor_eff,
                                  battle_hostility_inverted_))
            ++n;
    }
    return 10 + 30 * n;
}

// sub_3BC17: Berserk. Called from 4CB5E dx=7 (player item) and 51FF7 subcmd 0 (VM). Shows the
// "Berserk!" label (48612), removes the unit from the ready queue through sub_19718 without
// touching
// its gauge, then sets the auto-battle fields.
void CombatII::battle_berserk_3bc17(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    battle_unit_label(unit_index, "Berserk!");  // sub_48612(unit, "Berserk!", 0x11), 36 frames
    if (should_exit_ || unit_index >= (int)units_.size()) return;
    battle_ready_queue_.erase(std::remove(battle_ready_queue_.begin(), battle_ready_queue_.end(), unit_index),
                              battle_ready_queue_.end());  // sub_19718
    CombatUnitPlacement& u = units_[(size_t)unit_index];
    u.auto_battle = 2;
    u.ai_field_6d = 0;
    u.ai_field_6e = 4;
    u.combat_flags_14 = (uint8_t)(u.combat_flags_14 | 2);
    u.ai_mode = (int8_t)0xFF;
    u.ai_decision_state = u.ai_field_6d;
    u.ai_field_80 = (uint8_t)u.ai_mode;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: Berserk unit=%d [83h]=2 [6Dh]=0 [6Eh]=4 [14h]=%02X\n", u.id, (unsigned)u.combat_flags_14);
}
