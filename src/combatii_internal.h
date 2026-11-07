#pragma once
// Shared internal header for the combatii translation units: the include set formerly at the top
// of combatii.cpp plus the file-local helpers several TUs need (constants, army conversion, SCE
// records, HP and hostility tests).
#include "combatii.h"
#include "char_files.h"
#include "fileutil.h"
#include "palette.h"
#include "pcx.h"
#include "weapon_class_table.h"
#include "combat_class_stat_table.h"
#include "combat_tables.h"
#include "combat_hit_pattern.h"
#include "combat_mobility_table.h"
// unk_74144 (the 12x20 critical-hit pattern) is available from combat_hit_pattern.h, so the
// pattern path of battle_hit_kind (sub_1D6B1) is enabled. Index, bounds and step order were
// checked against the listing (dasm 19554..19622).
#define G2_HAVE_HIT_PATTERN 1
#include <functional>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <climits>
#include <algorithm>
#include <limits>
#include <SDL.h>

// State of the cone iterators (sub_1AF22/sub_1B00D) and diamond iterators (sub_1AC08/sub_1AC59),
// shared by combatii_ai.cpp (WC5 chain) and combatii_aimage.cpp (mage AI).
struct CombatII::Wc5Iter {
    int x = 0, y = 0;         // F0B92/F0B94 (signed word)
    int c_lo = 0, c_hi = 0;   // F0B7C low/high
    int n_lo = 0, n_hi = 0;   // F0B78 low/high
    int d96 = 0, d98 = 0;     // F0B96/F0B98
    int d9a = 0, d9c = 0;     // F0B9A/F0B9C
    int done = 0;             // F0B90
};

struct CombatII::Wc5Dia {
    int x = 0, y = 0;         // F0B92/F0B94
    int c_lo = 0, c_hi = 0;   // F0B7C low/high
    int n_lo = 0, n_hi = 0;   // F0B78 low/high
    int done = 0;             // F0B90
};



#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif


constexpr int TOWN_TILE_W = 24;
constexpr int TOWN_TILE_H = 16;
constexpr int TOWN_TILE_BYTES = TOWN_TILE_W * TOWN_TILE_H;

// ---- Screen anchors (original sub_10971 / sub_10988 convention) ----
//   sub_10971(x) = (tx - word@BA83C) * 24 + word@7001C + 4     <- tile top-left X
//   sub_10988(y) = (ty - word@BA83E) * 16 + word@BA838 + 4     <- tile top-left Y
// Each layer adds its own constant on top:
//   - Base tile layer (sub_10AEB) draws the first column (cam_x) and first row (cam_y) at
//     sub_5EB00(fineX + 4, fineY + 4), so the anchor is (+4, +4). sub_5F804 adds the page
//     target baseline dword_755D6 = 0xBB80 = 48000 = 480*100 rows, but the viewport
//     (sub_5EE9B) clips at that baseline, so screen coordinates stay at +4. The port's FPM
//     crop uses the same convention.
//   - Unit sprite (sub_12F2A): var_18 = sub_10971 + 0x0C, var_1C = sub_10988 - 2, so the
//     anchor is (+16, +2). x_off/y_off are added on top of it.
//   - Unit centre (sub_135EB) and weapon trail (sub_1E2EA): sub_10971 + 0x0C, sub_10988 + 2
//   - Dialogue circle origin (head, sub_482A5): sub_10971 + 0x0C, sub_10988 + 4
// At rest the camera fine values are (fineX 12, fineY 0) (see CAM_FINE_X_REST). The top-left of
// tile (cam+6, cam+6) is X = 6*24 + 12 + 4 = 160, Y = 6*16 + 0 + 4 = 100, exactly the screen
// centre (160, 100). sub_1124A's "camera = unit tile - 6" relies on that property, and it also
// matches the 12-tile window in both directions (camera limit = map width/height - 12).
// With fineX 0 the X value becomes 148, which breaks the property and shifts the map 12px left.
constexpr int TILE_ANCHOR_X = 4,  TILE_ANCHOR_Y = 4;    // base/top tile layers
constexpr int UNIT_ANCHOR_X = 16, UNIT_ANCHOR_Y = 2;    // unit sprite (var_18/var_1C)
// Offsets added to sub_10971/sub_10988 (= TILE_ANCHOR_X/Y)
constexpr int CENTER_ADD_X = 12, CENTER_ADD_Y = 2;      // sub_135EB, sub_1E2EA
constexpr int HEAD_ADD_Y = 4;                          // sub_482A5 (head = X+12, Y+4)

// Outer SCE groups 1..3 and nonzero dynamic-spawn records increment raw [+0x0E] as a byte.
// Group-0/remapped and raw-zero dynamic records need word_FDAF6/CC990 state not modeled here.
inline uint8_t battle_incremented_raw_army(int raw_army) {
    return (uint8_t)((unsigned)raw_army + 1u);
}

inline int battle_effective_army(const CombatUnitPlacement& unit) {
    if (unit.memory_army >= 0) return unit.memory_army;
    return battle_incremented_raw_army(unit.army); // Explicit fallback for remap paths not represented by the port.
}

// ---- SCE unit group record (sub_4ED18 loc_4EFCB .. loc_4F00E) ----
// Each unit in a group is read as one 0x88-byte record, but the original then inspects the link
// field (dword at +0x77) of the record it just read:
//     cmp dword ptr [ecx+77h], 0 ; jz loc_4F00E
// When it is non-zero, sub_112FD allocates a new block, another 0x88 bytes are read and the two
// are linked through the [+0x77] pointer (a rider or attached unit). That unit therefore occupies
// 0x110 bytes in the file. The field holds a (now meaningless) file-time pointer, so only "zero
// or not" matters, not the value itself.
// (The 0x03 reinforcement command in sub_50D9F tests a different field, [+0x5C]; see VM_SPAWN in
// run_vm. Mixing the two shifts every later offset for groups that have links, as in 1-4.SCE.)
namespace {
constexpr size_t kSceUnitRecSize = 0x88;
inline bool sce_unit_record_linked(const uint8_t* rec) {
    return (rec[0x77] | rec[0x78] | rec[0x79] | rec[0x7A]) != 0;
}
// Bytes one record occupies in the file; 0 means the remaining data is short (corrupt).
inline size_t sce_unit_record_span(const uint8_t* rec, size_t remaining) {
    if (remaining < kSceUnitRecSize) return 0;
    if (sce_unit_record_linked(rec)) {
        if (remaining < 2 * kSceUnitRecSize) return 0;
        return 2 * kSceUnitRecSize;
    }
    return kSceUnitRecSize;
}
} // namespace

// In a unit record (the 0x88-byte BattleUnit snapshot) [+0x48]/[+0x4C] hold current and max HP
// scaled by 100 (sub_5211D adds and subtracts script values times 100; sub_13498 displays
// ceil(HP/100)). The port keeps hp/max_hp in display units, so convert here; a non-positive
// value keeps the default of 100.
inline void apply_record_hp(CombatUnitPlacement& p, const uint8_t* rec) {
    auto le16 = [&](int o) -> int16_t {
        return (int16_t)(uint16_t(rec[o]) | (uint16_t(rec[o + 1]) << 8));
    };
    auto le32 = [&](int o) -> int32_t {
        return (int32_t)((uint32_t)rec[o] | ((uint32_t)rec[o + 1] << 8) |
                         ((uint32_t)rec[o + 2] << 16) | ((uint32_t)rec[o + 3] << 24));
    };
    const int32_t hp_raw = le32(0x48), max_raw = le32(0x4C);
    p.occupancy_hp_known = hp_raw > 0;
    if (hp_raw > 0) { p.hp = (hp_raw + 99) / 100; p.hp_deficit = p.hp * 100 - hp_raw; }
    if (max_raw > 0) { p.max_hp = (max_raw + 99) / 100; p.max_hp_deficit = p.max_hp * 100 - max_raw; }
    if (p.max_hp < p.hp) p.max_hp = p.hp;
    p.combat_str_40 = rec[0x40];
    p.combat_value_42 = le16(0x42);
    p.combat_value_44 = le16(0x44);
    p.combat_value_46 = le16(0x46);
    p.combat_value_54 = le16(0x54); // sub_19FAB: [+54] baseline; flag 1Eh&40h reads same word as dword[+52h] high half
    p.combat_value_43 = rec[0x43];
    p.combat_flags_13 = rec[0x13];
    p.combat_mp_50 = (uint16_t)(uint16_t(rec[0x50]) | (uint16_t(rec[0x51]) << 8)); // MP
    p.combat_mp_max_52 = (uint16_t)(uint16_t(rec[0x52]) | (uint16_t(rec[0x53]) << 8)); // max MP (sub_48734)
    p.combat_stat_3e = rec[0x3E]; // sub_48863 detail screen
    p.combat_stat_40 = rec[0x40]; // sub_48BC1 equipment screen display
    p.combat_mag41 = rec[0x41];  // sub_285E4 caster attack
    p.combat_level_2b = rec[0x2B]; // sub_28947/48734 level
    p.reward_money_85 = (uint16_t)(uint16_t(rec[0x85]) | (uint16_t(rec[0x86]) << 8)); // sub_17A20 kill reward
    p.combat_acc57 = 0;          // [+0x57] accumulates only in battle; the SCE initial value is ignored
    p.combat_def59 = rec[0x59];  // sub_285E4 target defense
    for (int i = 0; i < 7; ++i) p.spell_charge_2e[i] = rec[0x2E + i]; // per-group charge
    p.item_use_12 = rec[0x12];
    p.weap_35 = rec[0x35]; // sub_2CA40 threshold row
    p.weap_36 = rec[0x36]; // sub_2CA40 pattern row
    p.weap_37 = rec[0x37]; // sub_2CA40 pattern column
    p.weap_cnt_38 = rec[0x38];
    p.weap_cnt_39 = rec[0x39];
    p.weap_cnt_3a = rec[0x3A];
    p.weap_cnt_3b = rec[0x3B];
    p.weap_cnt_3c = rec[0x3C]; // WC0 V count (sub_4C03E)
    p.flags_84 = rec[0x84];
    p.ail_1f = (uint16_t)(uint16_t(rec[0x1F]) | (uint16_t(rec[0x20]) << 8));
    p.ail_23 = (uint16_t)(uint16_t(rec[0x23]) | (uint16_t(rec[0x24]) << 8)); // sub_1EBFE cx==3 duration
    p.ail_27 = (uint16_t)(uint16_t(rec[0x27]) | (uint16_t(rec[0x28]) << 8)); // sub_1EBFE cx==-2 duration
    p.combat_type_5a = rec[0x5A];
    p.combat_type_5b = rec[0x5B];
    p.combat_type_5d = rec[0x5D];
    p.unit_kind_0d = rec[0x0D];   // sub_1D770: [target+0x0D] == 2
    p.hit_seq_2c = rec[0x2C];     // sub_1D6B1 pattern row/column counter
    p.hit_seq_2d = rec[0x2D];
    p.inv_5e = rec[0x5E];         // sub_18B2E item slot 1
    p.inv_5f = rec[0x5F];         // sub_18B2E item slot 2
    p.exp_3d = rec[0x3D];         // sub_18B2E/17A20 exp (capped at 100)
    p.growth_3f = rec[0x3F];      // sub_176F1 weapon/magic growth counter
}

// ---- Combat core tables (sub_174CF / sub_17531 / sub_1762C / sub_1D6B1 / sub_1D770 / sub_1DDF0)
// ----
// All of these index their tables with the class byte [+0x11], never the type code [+0x61]:
//   sub_174CF: unk_72793[[+11h]*3], sub_17531: off_72794[[+11h]*3],
//   sub_1DDF0: off_72794+1[[+11h]*3]. Damage is (attack - defense) * 100, with the attack value
// read from the word at [+0x44] (`mov eax,[edi+42h]; sar eax,10h`). sub_1DDF0's block chance is
// (q+1)*20+10 at loc_1E09B.., and evasion and block share one random draw (var_10).
// sub_1D770 takes ebx = sub_1D6B1(attacker) (critical kind) and
// ecx = (weapon class in {5, 0xB, 0xC}) ? 0 : 1 (neighbour correction).

// sub_195AE (item [+0x5B]): ranged special item set, transcribed from the ASM compare chain.
inline bool battle_item_195ae(int item) {
    switch (item) {
    case 0x19: case 0x1A: case 0x1B: case 0x1D: case 0x22: case 0x24: case 0x28:
    case 0x2D: case 0x30: case 0x31: case 0x3B: case 0x43: case 0x46:
        return true;
    default:
        return false;
    }
}

// sub_18F52 (item [+0x5B]): damage class for the physical-hit ailment (sub_1EBFE),
// transcribed from the ASM compare chain, including the dead-cmp+jmp flags for 0x13/0x30
// (`cmp al,13h; jmp; jz` means al==0x13 -> 1, `cmp al,30h; jmp; jz` means al==0x30 -> 2).
//   0 -> {0x24, 0x27..0x2D, 0x3B, 0x43}, 1 -> {0x13, 0x20},
//   2 -> {0x10, 0x16, 0x30, 0x3C, 0x42}, 3 -> {0x18, 0x1B}, otherwise -1.
inline int battle_item_18f52(int item) {
    const unsigned al = (unsigned)item & 0xFF;
    if (al < 0x24) {
        if (al < 0x16) {
            if (al < 0x10) return -1;
            if (al <= 0x10) return 2;
            return (al == 0x13) ? 1 : -1;
        }
        if (al == 0x16) return 2;
        if (al < 0x1B) return (al == 0x18) ? 3 : -1;
        if (al <= 0x1B) return 3;
        return (al == 0x20) ? 1 : -1;
    }
    if (al == 0x24) return 0;
    if (al <= 0x3B) {
        if (al < 0x2D) return -1;
        if (al <= 0x2D) return 0;
        return (al == 0x30) ? 2 : -1;
    }
    if (al < 0x42) return (al == 0x3C) ? 2 : -1;
    if (al == 0x42) return 2;
    if (al == 0x43) return 0;
    return -1;
}

// sub_4BC3C (unit ID): V-weapon slot index (0..12, else -1), transcribed from the compare chain.
inline int battle_weapon_match_4bc3c(int id) {
    const unsigned al = (unsigned)id & 0xFF;
    if (al < 0x43) {
        if (al < 0x12) {
            if (al < 1) return -1;
            if (al == 1) return 12;
            return 3; // al==2: `cmp al,2; jmp loc_4BCA7; jz` keeps the ZF and returns 3
        }
        if (al == 0x12) return 1;
        if (al < 0x1D) return (al == 0x16) ? 5 : -1;
        if (al == 0x1D) return 2;
        return (al == 0x30) ? 3 : -1;
    }
    if (al == 0x43) return 0;
    if (al == 0x44) return 8;
    if (al < 0x5B) {
        if (al < 0x4B) return -1;
        if (al <= 0x4C) return 11;
        return (al == 0x51) ? 9 : -1;
    }
    if (al == 0x5B) return 7;
    if (al < 0x64) return -1;
    if (al == 0x64) return 4;
    if (al < 0x77) return (al == 0x65) ? 10 : -1;
    if (al == 0x77) return 6;
    return -1;
}

// sub_4C03E: V-weapon list. Per WC: start index, count source and id/item match. A match adds
// entry 0 (the special slot) before the counted entries; otherwise only the counted entries.
// Entry values are display identifiers; execution branches on the position si. wc2 and wc>5 read
// an indeterminate count from ambient ecx, so they return an empty list (no matching player unit
// in 1-7).
inline int battle_weapon_list_4c03e(const CombatUnitPlacement& u, std::vector<int>& out) {
    out.clear();
    const int wc = (u.code_b >= 1 && u.code_b <= 4 && u.linked_class == 0x17)
        ? 5 : weapon_class_for_type(u.combat_class); // sub_174CF
    int start = 0, count = 0;
    bool matched = false;
    const int id = u.id, item = (int)u.combat_type_5b;
    auto match2 = [&](int want_id, int want_item) {
        return id == want_id && item == want_item;
    };
    switch (wc) {
        case 0: start = 0x13; count = (int)u.weap_cnt_3c; break;
        default: break;
    }
    if (wc == 1) {
        start = 0; count = (int)u.weap_cnt_38;
        matched = match2(0x43, 0x1C) || match2(0x12, 0x24) || match2(0x1D, 0x21) ||
                  ((id == 0x30 || id == 0x02) && item == 0x20) ||
                  match2(0x64, 0x1D) || match2(0x77, 0x16) ||
                  match2(0x44, 0x23) || match2(0x01, 0x19);
    } else if (wc == 2) { count = 0; } // ambient ecx: empty list
    else if (wc == 3) {
        start = 7; count = (int)u.weap_cnt_39;
        matched = match2(0x16, 0x37) || match2(0x51, 0x3C);
    } else if (wc == 4) {
        start = 0x0B; count = (int)u.weap_cnt_3a;
        matched = match2(0x5B, 0x2F);
    } else if (wc == 5) {
        start = 0x0F; count = (int)u.weap_cnt_3b;
        matched = (id == 0x4B || id == 0x4C) && item == 0x43;
    } else if (wc != 0) { count = 0; } // wc>5 reads ambient ecx
    if (matched) out.push_back(0);
    for (int i = 1; i <= count; ++i) out.push_back(start + i);
    return (int)out.size();
}
inline bool battle_item_18ef9(int item) {
    switch (item) {
    case 0x04: case 0x0C: case 0x16: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
    case 0x20: case 0x22: case 0x23: case 0x24: case 0x2C: case 0x31: case 0x37: case 0x3C:
    case 0x43: case 0x46:
        return true;
    default:
        return false;
    }
}

// sub_1201D(a, b): effective memory armies are hostile for {1,2} vs {3,4}, plus 3 vs 4.
// The return is XORed with byte_E518D; sub_44913 may set that byte to 1 for one AI action.
// battle_effective_army() uses the exact loader increment for groups 1..3 and a raw+1 fallback
// otherwise.
inline bool battle_armies_hostile(int a, int b, bool inverted = false) {
    bool hostile = false;
    if (a == 1 || a == 2)      hostile = (b == 3 || b == 4);
    else if (a == 3)           hostile = (b == 1 || b == 2 || b == 4);
    else if (a == 4)           hostile = (b == 1 || b == 2 || b == 3);
    return hostile != inverted; // sub_1201D: movsx dx, byte_E518D; xor eax, edx
}

// Unit [+0x48] current HP (x100). hp_deficit (0..99) restores the original value from the port's
// ceil display unit.
inline int battle_hp_raw(const CombatUnitPlacement& u)     { return u.hp * 100 - u.hp_deficit; }
inline int battle_max_hp_raw(const CombatUnitPlacement& u) { return u.max_hp * 100 - u.max_hp_deficit; }

// Single-target path: sub_1F738 -> sub_1DDF0(dx=1) -> sub_1E61D/sub_1EF5C -> sub_1ED45.
//   Before: sub_53797(attacker, first target, mode 0), the attack declaration event. Abort when
//   the attacker or target disappears through an event (sub_1DDF0 returns -1/-2). Table 5/6
//   (contact, sub_5394D) is evaluated before the declaration and after a move.
//   After (sub_1ED45): target HP <= 0 calls sub_1DB1D(attacker, target), otherwise
//   sub_53797(target, attacker, mode 1).
// The event VM can add units (0x03) and reallocate units_, so references are re-resolved by
// index instead of being held across calls.
//
// sub_40DFD: var_8 comes from 40E48..40EA7 and the radius argument is Q+var_8 (410D0/410DE).
// wc5 uses 5 for class 0x15/0x16 and 7 otherwise; wc0xB=9, wc0xC=3, wc0x12/0x13=5, else 7.
// Q = (gauge - 10) / 10 (40EC4 stores Q at [esp+28h]).
// Class 53 (WC5, var_8=7): radius = Q+7, so gauge 85 -> 14 and 45 -> 10.
// wc0xB (class 103): Q+9. wc0xC (class 104): Q+3.
// The original pushes var_18(Q)+var_8, recomputed at 40EC4 on every iteration.
inline int battle_40dfd_scan_radius(int gauge, int wc, uint8_t combat_class) {
    int q = (gauge - 10) / 10;
    int var_8 = 7;
    switch (wc) {
        case 5:    var_8 = (combat_class == 0x15 || combat_class == 0x16) ? 5 : 7; break;
        case 0x0B: var_8 = 9; break;
        case 0x0C: var_8 = 3; break;
        case 0x12:
        case 0x13: var_8 = 5; break;
        default:   var_8 = 7; break;
    }
    return (q < 0 ? 0 : q) + var_8;
}

// sub_44913 loc_449CC: wc 5/0xB/0xC/0xF/0x10..0x13 take sub_40DFD, everything else sub_40295.
// Classes other than wc5 also run the shared 40DFD body; only the var_8 map differs.
inline bool ai_uses_class40dfd_path(int weapon_class_value) {
    switch (weapon_class_value) {
        case 5: case 0x0B: case 0x0C: case 0x0F:
        case 0x10: case 0x11: case 0x12: case 0x13:
            return true;
        default:
            return false;
    }
}

// Johab -> UTF-8 for diagnostics, truncated at max_bytes on a character boundary so a multi-byte
// syllable is never split. Defined in combatii_script.cpp.
std::string log_text(const std::string& raw, size_t max_bytes = 60);

// One row of sub_6106C (sub_6098B + sub_609F9 mode 0), defined in combatii_map.cpp.
// The `sub_6106C(3, 0x11, 0x46)` call between steps 7 and 8 of 341C7 also builds its table here;
// when sub_5EDDE runs in mode 1 it recolours the pixels inside the rectangle through it.
void build_shade_row_original(const uint8_t pal[256][3], int tint, int bl, uint8_t out[256]);
