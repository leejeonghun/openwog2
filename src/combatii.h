#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include "video.h"
#include "font.h"
#include "input.h"
#include "nc2_parser.h"
#include "spr_parser.h"
#include "data_structures.h"
#include "talktbl.h"
#include "portrait.h"
#include "combatii_range.h"
#include "combatii_move.h"

// ==== COMBATII: battle scenario module ====
// Pre-battle cutscenes, map rendering and scrolling, sprites, portraits and dialogue,
// plus the tactical battle driven by SCE scripts and the AI.

struct CombatMapInfo {
    int width = 0;   // tiles W
    int height = 0;  // tiles H (or scanlines for PACK-style)
    std::string map_filename; // without path, e.g. "IM05.MAP"
};

// 0x88-byte unit record offsets, confirmed against sub_4ED18's four-group loop and the
// references in sub_500ED, sub_501BA, sub_507BE, sub_52463 and sub_516D0:
//   +0x00 x, +0x01 y          SCE placement, within the MAP width/height
//   +0x02 12-byte Johab name string
//   +0x0B/+0x0E army (agent)  For outer groups 1..3, loc_4F02F increments [+0Eh] and
//       stores it in dword_CC968[(memory army-1)*8]. Static outer group 0 is remapped by
//       sub_4E81F through [+87h]/word_FDAF6; the caller strips the sign bit of the remap
//       byte and a result of 0 becomes [+0Eh]=1 / CC968 slot 0. The helper reuses an
//       army-1/ID candidate from CC990 or, when the source byte is 0, adds a [+0Eh]=1
//       copy. Dynamic reinforcement callers increment [+0Eh] after the same helper with
//       no sign check, so remap byte FFh can compute a CC960 address. sub_4E81F may run
//       for dynamic reinforcements too, but units_ models neither the effective remap
//       nor CC990 membership.
//   +0x0F unit ID  sub_500ED looks up a CC990 ID candidate and sub_501BA sel=0 checks the
//       same ID with the [+87h] owner in the candidate's [+0Eh]-1 army list. CC990 is not
//       the full group-0 unit list. 1-1.SCE's three units are 18/118/119, matching the
//       script's `02 00 12 ...` (p2=18) and `0C 00 76 00 0A` (idx=118) /
//       `0C 00 77 00 0A` (idx=119).
//   +0x77 link field (dword)  Non-zero means one more 0x88 record follows; sub_4ED18
//       loc_4EFCB tests `cmp dword ptr [ecx+77h],0`. The 0x03 reinforcement command
//       sub_50D9F instead tests [+0x5C].
//   +0x60 image slot the SCE wrote; loc_4F02F overwrites it with the sub_1198D result
//   +0x61 character type code (u16)  The word loc_4F02F reads with
//       `mov eax,[ebp+5Fh]; sar eax,10h`; sub_12567 tests the same word with
//       `mov bx,[esi+61h]; cmp bx,12Ch`. sub_1198D(type) -> sub_11575 picks
//       `CHARACTE\<name>.NC2` from off_70024/703A8/705A4 and loads it into the slot.
struct CombatUnitPlacement {
    int x = 0, y = 0;
    uint8_t sprite_id = 0;   // character index used as the CITIZEN fallback
    uint8_t team = 0;          // Coarse side hint used by VM area-condition approximation; not effective army/list membership.
    uint8_t code_a = 0, code_b = 0;
    uint8_t combat_class = 0;    // BattleUnit +0x11
    uint8_t combat_flags_14 = 0; // BattleUnit +0x14
    uint8_t combat_flags_15 = 0; // BattleUnit +0x15
    uint8_t combat_flags_1e = 0; // BattleUnit +0x1E
    uint8_t combat_flags_13 = 0; // BattleUnit +0x13
    uint8_t combat_str_40 = 0;   // BattleUnit +0x40 (sub_17D82 sel 3 cap, sub_18262 equip adjust)
    int16_t combat_value_42 = 0; // BattleUnit +0x42 (sub_1D770 does not read it; attack uses +0x44)
    int16_t combat_value_44 = 0; // BattleUnit +0x44 attack value (sub_1D770: `mov eax,[edi+42h]; sar eax,10h`)
    int16_t combat_value_46 = 0; // BattleUnit +0x46 defense value (sub_1D770 target baseline)
    int16_t combat_value_54 = 0; // BattleUnit +0x54 / high word of dword[+0x52], sub_19FAB shared AI gauge
    uint8_t combat_value_43 = 0; // BattleUnit +0x43
    uint8_t combat_stat_40 = 0;  // BattleUnit +0x40 (shown by sub_48BC1, adjusted by sub_181A6/18262)
    uint8_t combat_stat_3e = 0;  // BattleUnit +0x3E (sub_48863 detail-screen value)
    uint8_t unit_kind_0d = 0;    // BattleUnit +0x0D (sub_1D770: target kind 2 takes 0 from armor-ignore classes)
    uint8_t hit_seq_2c = 0;      // BattleUnit +0x2C critical pattern row (sub_1D6B1)
    uint8_t hit_seq_2d = 0;      // BattleUnit +0x2D critical pattern column counter
    std::shared_ptr<CombatUnitPlacement> stored_77; // [+77h] preserved record (pre-transform copy from sub_3C548, restored by sub_1808A). SCE link records are not loaded (linked_class only)
    int linked_class = -1;       // class [+11h] of the linked record ([+77h]); sub_174CF maps 0x17 to weapon class 5
    int hp_deficit = 0;          // how far [+48h] falls below hp*100 (0..99); original HP(x100) = hp*100 - deficit
    int max_hp_deficit = 0;      // same rule for max HP ([+4Ch])
    uint8_t combat_type_5a = 0;  // BattleUnit +0x5A, sub_184A4 lookup index
    uint8_t combat_type_5b = 0;  // BattleUnit +0x5B
    uint8_t combat_type_5d = 0;  // BattleUnit +0x5D
    uint16_t combat_mp_50 = 0;   // BattleUnit +0x50 (MP, sub_4C605/sub_432A0)
    uint16_t combat_mp_max_52 = 0; // BattleUnit +0x52 (max MP, sub_48734 info window `dword[+50h] sar 10h`)
    uint8_t combat_mag41 = 0;    // BattleUnit +0x41 (sub_285E4 caster attack)
    uint8_t combat_level_2b = 0; // BattleUnit +0x2B (sub_28947 status chance, sub_48734 level)
    uint16_t combat_acc57 = 0;   // BattleUnit +0x57 word (TP accumulation: added by loc_4D517, cleared by sub_45B4E, spent by sub_432A0)
    uint8_t combat_def59 = 0;    // BattleUnit +0x59 (sub_285E4 target defense)
    uint8_t spell_charge_2e[7] = {}; // BattleUnit +0x2E..+0x34 (per-group charge, sub_28429)
    uint8_t item_use_12 = 0;     // BattleUnit +0x12 (sub_4BD26 id-7 gate)
    uint8_t weap_cnt_38 = 0;     // BattleUnit +0x38 (sub_4BD26 WC1 id-1 gate)
    uint8_t weap_cnt_39 = 0;     // BattleUnit +0x39 (WC2 id-2)
    uint8_t weap_cnt_3a = 0;     // BattleUnit +0x3A (WC3 id-3)
    uint8_t weap_cnt_3b = 0;     // BattleUnit +0x3B (WC4 id-4)
    uint8_t weap_cnt_3c = 0;     // BattleUnit +0x3C (WC0 V count, sub_4C03E)
    uint8_t weap_35 = 0;         // BattleUnit +0x35 (sub_2CA40 threshold row select)
    uint8_t weap_36 = 0;         // BattleUnit +0x36 (sub_2CA40 pattern row; rand(12) when [37]==0)
    uint8_t weap_37 = 0;         // BattleUnit +0x37 (sub_2CA40 pattern column, +1 %20 per call)
    uint8_t flags_84 = 0;        // BattleUnit +0x84 (sub_4C19D dx=0x0E one-shot)
    uint16_t ail_1f = 0;         // BattleUnit +0x1F word (sub_29437 status duration, reset by sub_1E2B1)
    uint16_t buff_16 = 0;        // BattleUnit +0x16 word (Blade/Power UP duration, word_74488/7448A)
    uint16_t buff_18 = 0;        // BattleUnit +0x18 word (Speed UP duration)
    uint16_t buff_1a = 0;        // BattleUnit +0x1A word (Magic Armor duration)
    uint16_t buff_1c = 0;        // BattleUnit +0x1C word (Bless/Barrier duration)
    uint16_t ail_21 = 0;         // BattleUnit +0x21 word (duration slot k=1 of [1Fh+2k])
    uint16_t ail_23 = 0;         // BattleUnit +0x23 word (sub_1EBFE cx==3, 100; slot k=2 of [1Fh+2k])
    uint16_t ail_25 = 0;         // BattleUnit +0x25 word (sub_3C30A curse duration 3Ch)
    uint16_t ail_27 = 0;         // BattleUnit +0x27 word (sub_1EBFE cx==-2, 100)
    uint8_t auto_battle = 0;     // BattleUnit +0x83
    uint8_t battle_gauge = 0;    // BattleUnit +0x56, 0xFF = ready
    uint8_t inv_5e = 0;          // BattleUnit +0x5E (sub_18B2E tile-event item slot 1)
    uint8_t inv_5f = 0;          // BattleUnit +0x5F (sub_18B2E tile-event item slot 2)
    uint8_t exp_3d = 0;          // BattleUnit +0x3D (sub_18B2E tile-event exp, sub_17A20 level-up; capped at 100)
    uint8_t growth_3f = 0;       // BattleUnit +0x3F (sub_176F1 weapon/magic growth counter, one step at 100)
    uint16_t reward_money_85 = 0; // BattleUnit +0x85 (sub_17A20 kill reward gold)
    // Patrol/guard state machine of sub_40295 (wc1 AI, no-target branch); dasm 61565..62984
    // (sub_3E18B plus branches sub_3E65E/3E739/3EADD/3EBB4/3ED51/3EF12/3EE2B, off_40279).
    // [+0x6C] AI decision state (0..6, jump table off_40279). The VM writes the state; the
    // port dispatches it through battle_ai_wc1_pursuit / battle_ai_dispatch_plus6c (scan,
    // sub_3CA3C scoring and the loc_40470 k=1..6 table).
    uint8_t ai_decision_state = 0;  // BattleUnit +0x6C
    uint8_t ai_field_6d = 0, ai_field_6e = 0; // BattleUnit +0x6D/+0x6E, state bytes written by VM_AI
    uint8_t ai_field_7f = 0, ai_field_81 = 0; // BattleUnit +0x7F/+0x81, sub_3F321 AI mode gates
    uint8_t ai_random_29 = 0, ai_random_2a = 0; // BattleUnit +0x29/+0x2A, sub_3F321 patrol selector
    // [+0x6F]/[+0x70] home coordinates, written by VM_TARGET sub=0 and fixed up for static
    // SCE units; +6F uses FFh as sentinel. That fixup runs over effective CC968 list slots,
    // while units_'s effective-army scan cannot approximate the group-0 remap.
    uint8_t ai_home_x = 0xFF, ai_home_y = 0xFF; // BattleUnit +0x6F/+0x70
    // [+0x71]/[+0x72] list selector and unit ID that sub_3E18B resolves via sub_1379A(sel,id).
    // dword_CC960[sel*8] is a linked-list head. sub_4ED18/sub_1134F insert each unit at
    // CC968 + (effective_army-1)*8, so sel 0..5 are the effective-army lists 0..5
    // (CC960, CC968..CC988) and sel 6 is the global CC990 list. 7..254 point past the
    // list array (into CC998) and are not treated as lists.
    uint8_t ai_watch_sel = 0xFF, ai_watch_id = 0; // BattleUnit +0x71/+0x72
    // [+0x73] is a BattleUnit pointer in the original; the port stores a units_ index. A
    // non-zero dword marker in a static SCE is resolved like sub_4ED18 does, by looking up
    // the unit occupying [+6F/+70] (base tile only).
    int ai_target_unit = -1;
    bool ai_target_marker = false; // temporary: whether [+0x73] was non-zero during SCE load
    uint8_t ai_field_80 = 0xFF; // BattleUnit +0x80; VM_AI writes it when selector != 0xFF
    // Field names follow the offsets of the original 0x88-byte unit structure.
    // [unit+0x63] four-way facing. sub_52848 and sub_16D3E loc_16E1A compare in the same order:
    //   y first (`cmp dx(own y), bx(target y); jle` -> 2 when greater, 0 when smaller), then
    //   x when equal (`cmp dx(own x), si(target x)` -> 1 when greater, 3 when smaller).
    // So 0=down(south), 1=left(west), 2=up(north), 3=right(east).
    int8_t facing = 0;
    // [unit+0x64] walking pose number (0..10). Together with `[unit+0x60]` appearance and
    // `[unit+0x63]` facing it selects an object-type table entry:
    //   entry = unk_D66A8 + 572*appearance + 13*(11*facing + frame)
    //   entry[0]/[1] = occupied tile size (sub_11E04/sub_11EE4),
    //   entry[4]/[5] = x/y offsets, entry[9..12] = sprite pointer (sub_50216).
    //   Start: sub_16D3E prologue (loc_16D78) uses 2 for classes 0Fh..16h, otherwise 3.
    //   Step: sub_16AEF -> sub_16A6D advances unk_E5A70 by +1&3 every second subframe and
    //         re-reads the table, which differs per class:
    //           unk_7110D = {2,3,4,3} (normal) / unk_71111 = {3,4,3,4} (0Fh..16h)
    //   Arrival: [63h] = load, [64h] = (class != 0) ? 2 : 0 -> 0 on the next subframe
    //         (loc_16F39).
    // Standing is 0, walking is 2/3/4 (normal) or 3/4 (0Fh..16h).
    uint8_t anim_frame = 0;
    uint8_t anim_counter = 3;  // unk_E5A70[unit] pose table index (+1 & 3 every second subframe)
    uint8_t anim_phase = 0;    // unk_E5AB0[unit] subframe gate (0/1, reset at 2)
    bool walk_arrived = false; // one-shot flag for the arrival step (pose 2 -> 0)
    // [unit+0x65]/[unit+0x66] pixel offset inside a tile. Each subframe sub_16A6D adds
    // +/-6 horizontally and +/-4 vertically, crossing one 24x16 tile in four subframes.
    int walk_off_x = 0, walk_off_y = 0;
    // Mode-1 spawn drop (sub_508E6 -> sub_50216). 0..7 = EVENT.SYB step (6 px fall per step,
    // 7 = standing on landing), -1 = no drop (regular sprite). anim_counter doubles as
    // unk_E5A50, the drop counter (see play_spawn_fall).
    int drop_step = -1;
    // sub_50216 draws the drop sprite only while the counter (E5A50) is 0..7; outside that
    // range (waiting below 0, landed at 8+) it draws nothing. The port also hides the
    // regular sprite while waiting, so it counts the remaining even frames here; 0 = no wait.
    int drop_wait = 0;
    // sub_50216+82: solid colour passed to sub_5F517 (cl = 9-dx = 2+counter); 0 means no
    // silhouette (regular sprite). At dx <= 1 the unit itself is not drawn.
    int drop_sil = 0;
    // Pixel dissolve state for large-unit deaths (sub_46A11). While death_dissolve > 0 the
    // renderer draws the strip with the pixels marked in death_mask[] erased to 0xFF.
    // The original builds a W x H strip with sub_10083 (unk_F1BA8) and erases
    // `(int16)(W*H-2)/12` pixels per frame at `rand % (int16)(W*H-2)`.
    uint8_t death_dissolve = 0;
    std::vector<uint8_t> death_mask;   // W*H, 0 = kept, 1 = erased (unk_F1BAC = 0xFF)
    int death_w = 0, death_h = 0;      // strip size written by sub_10083 (entry[2]/[3])
    int hp = 100;        // [unit+0x48] sub_5211D(0x0A)
    int max_hp = 100;    // [unit+0x4C]
    bool occupancy_hp_known = false; // true only when the source record had an explicit positive [+0x48] HP value
    bool active = true;  // removed first by 0x04 (retreat) / 0x0A
    bool deploy_roster = false; // deployment phase (sub_55518): party record 4EA22 put in CC990, or its deploy copy
    int8_t ai_mode = -1; // [unit+0x82] AI state/control byte (VM_AI, SCE loader)
    // ---- Fields read directly from the SCE unit record ----
    int id = 0;       // +0x0F unit ID referenced by scripts
    int army = 0;     // Raw SCE/spawn/VM payload byte; not necessarily effective BattleUnit [+0x0E]
    uint8_t owner_87 = 0; // BattleUnit +0x87 owner byte; sub_501BA checks it in the army/CC990 list
    int memory_army = -1; // BattleUnit [+0x0E] when derivable; -1 means remap/list path unknown
    bool battle_list_membership_modeled = false; // static SCE outer group 1..3, normal byte-INC army 1..4, no linked record
    uint32_t list_seq = 0; // CC968 list insert order (0 = initial load order, units_ index; >0 = later sub_1134F tail insert)
    int group = 0;    // SCE outer record group (0..3), not effective army/list membership; port scheduler uses it, ASM walks CC968; dynamic spawns use synthetic group 3
    std::string name; // +0x02 NUL-terminated Johab name, shown verbatim on the dialogue name line
    int sprite_type = 0; // +0x61 (u16) character type code passed to sub_1198D
    int slot_hint = 0;   // +0x60 initial image slot written by the SCE
    // [unit+0x14] bit0 - flying unit (sub_12567+6259/sub_12F2A: y-0x18 in the air, no feet handling)
    bool flying = false;
    // [unit+0x12]/[unit+0x13] frame tile footprint (sub_49FE1: [edi]=horizontal nt, [edi+1]=vertical mt)
    // Used by sub_12F2A's clip_height and by the foot-effect repeat count (sub_16357 [edi]).
    // Tactical footprint comes from the raw D66A8 table (entry[0]/[1], battle_d66a8_footprint).
    int frame_nt = 1;  // horizontal tiles (render info; not a tactical substitute)
    int frame_mt = 1;  // vertical tiles (render info; not a tactical substitute)
    // byte_F9C08/byte_F9C0A[target i] knockback speed (px/frame). The original keeps these per
    // target slot; storing them per unit is equivalent because one effect puts a unit in only
    // one target slot at a time.
    int knock_dx = 0, knock_dy = 0;
    bool attacking = false; // [unit+0x6B] - set while sub_1E61D is playing
    int face = 0;     // +0x10 PORTRAIT.SYF index (sub_482A5 -> sub_48132 bx)
                      //  Observed: 1-1 (0x2d,0x4b,0x4c), 1-1A (0x54), 1-2 (0x03,0x04,0x54),
                      //  37-1 (0x2d,0x47), all below the u16@0x42 count (85) in PORTRAIT.SYF.
};

// Verdict sub_1DDF0 reaches for one target (passed to word_F0430 and sub_1D770).
struct BattleAttackRoll {
    int result = -1;      // word_F0430: -1 hit, 1 block, 2 dodge
    int hit_kind = 0;     // sub_1D770 ebx (sub_1D6B1: 0 normal / 1 / 2)
    int area_mode = 0;    // sub_1D770 ecx (0 for weapon classes 5/0xB/0xC)
    int def_bonus = 0;    // temporarily added to the target's [+46h] on a successful block
};

// ==== VM event timeline ====
// In the original, sub_52E55's handlers drive the screen and units directly and run their
// own wait loops where needed (scrolling in sub_52940, movement effects in sub_51449 and
// sub_516D0). The port records each command as an event and execute_cutscene() consumes
// them in order, which reproduces the same sequence.
enum class VmEventType {
    Message,        // 0x73 message-window text (one window per id)
    Dialogue,       // 0x02 speaker portrait + bottom dialogue box
    CameraScroll,   // 0x0F mode 0/1/4+ scroll to tile (x,y); immediate only when A==1
    CameraWait,     // 0x0F mode 2 wait for the scroll to finish (count==0 -> until done)
    CameraUnit,     // 0x0F mode 3 scroll until the unit is centred
    UnitMove,       // 0x05 walk along a path
    Spawn,          // 0x03 sub_50D9F activate units, then walk (sub_16D3E) or drop in (sub_508E6)
    OccupancyStateUnknown, // unported VM unit/list mutation; permanently closes the restricted WC1 occupancy gate
    AiSettings,     // 0x0B sub_522A7 set AI decision/state/target fields
    AiTarget,       // 0x6F sub_52377 set home coordinates or the target-unit pointer
    Faction,        // 0x0C act=0 change a unit's army
    Flag,           // 0x11 scenario flag
    Weather,        // 0x71
    Bgm,            // 0x77 / 0x13
    NextScenario,   // 0x12 load the next .SCE
    Victory,        // 0x01 sub_506F3 -> sub_44CCA fade out, then end the module
    Choice,         // 0x65 sub_50751: battle result menu / contmap handoff
    Attack,         // 0x07 sub_51C74 -> sub_1E61D/sub_1EF5C scripted attack/hit effect
    UnitPose,       // 0x07 no target and mode>=2 (loc_51E34): set attacker [63h]/[64h], hold 32 ticks
    Hp,             // 0x0A sub_5211D HP change (damage number/knockback); death at HP<=0 (sub_1DB1D)
    UnitLeave,      // 0x04 sub_51449 retreat: drop the record's units from tile occupancy
                    //  (the original clears the grid with sub_11EE4 here, which also removes
                    //   them from the render queue; the port collects events up front, so the
                    //   removal has to happen at playback time)
    UnitEffect,     // 0x09 sub_51FF7 unit effect subcommand 0..6 (hp_unit, value=sub, army=extra)
    Face,           // 0x66 sub_52848 make two units face each other; facing depends on the
                    //  unit's current tile, so it is computed at playback, not at parse time
    Magic,          // 0x08 / 0x6C sub_51E8B scripted spell/weapon effect
    Item,           // 0x06 sub_5196F give/equip an item to the forced/random receiver
    FactionTransform, // 0x0C sub_52463 action 1..4: inline 0x88 record / type change
    TileChange        // 0x10 sub_52C06: one [layer][x][y][u16 tile][terrain] record
};

// One 0x07 target (dword_F0648[i] + word_F0430[i]).
struct VmAttackTarget {
    int unit_index = -1;
    int mode = 0;      // script byte (2..0x32)
    int result = -1;   // word_F0430[i]: mode 2 -> 1 (block: sub_4609A), mode 3 -> 2 (dodge: sub_461C6),
                       //                 otherwise -> -1 (hit: sub_45ED2)
};
// One unit handled by a single sub_516D0 (0x05) command. The original stores a unit pointer in
// dword_F0648[record], the load in unk_E5AD0[record] and the path in unk_E5210[record*64], then
// walks all records at once (dword_F0B9C+2 = record count).
struct VmWalkUnit {
    int unit_index = -1;                   // units_ index
    int load = 0;                          // unk_E5AD0 facing after the walk [unit+63h]
    std::vector<std::pair<int,int>> path;  // tile coordinates (first point = current position)
    // For sub_508E6 (spawn effect): a mode-1 spawn is placed on its deployment tile and then
    // drops in. 0 = no drop, 1..8 = EVENT.SYB drop step (lands at 7, 8 = standing after landing).
    int drop = 0;
    // Count1 - group appear delay in even frames. sub_50D9F sets unk_E5A50[idx] = -count1;
    // sub_16D3E (K==0 walk) and sub_508E6 (K==1 drop) both wait out that negative counter.
    // 0 = start immediately.
    int spawn_delay = 0;
};
struct VmEvent {
    VmEventType type = VmEventType::Message;
    uint32_t offset = 0;              // diagnostic: body offset of the command
    std::string text;                 // Message / Dialogue
    bool is_overlay = false;          // whether this is a "<< ... >>" caption
    int msg_frames = -1;              // 0x73 first u16 = display frames (word_11057C)
    int portrait = -1;                // Dialogue speaker portrait (PORTRAIT.SYF index = unit+0x10)
    int speaker_id = -1;              // Dialogue speaker unit ID
    std::string speaker_name;         // Dialogue speaker name (unit+0x02)
    int speaker_tx = -1, speaker_ty = -1;  // Dialogue speaker tile (sub_1124A/loc_48368 circle effect)
    int speaker_unit = -1;            // Dialogue speaker units_ index. The original keeps the unit
                                      // Pointer (ebp) and reads the live [ebp+0/1] tile at
                                      // playback, because a line after a spawn/walk differs from
                                      // the tile at creation time.
    int cam_x = 0, cam_y = 0;         // CameraScroll/CameraUnit target tile (B, C)
    // The two remaining bytes of sub_52940's A/B/C/D. cam_mode = A (sub_50339 al; moves
    // immediately only when 1), cam_speed = D (pixels per frame; sub_50339 converts 0->1,
    // 3->8 and doubles anything else), cam_count = B of mode 2 (0 = wait until finished).
    int cam_mode = 0;
    int cam_speed = 1, cam_count = 0;
    // UnitMove (sub_516D0): per-record unit list plus group and frame count.
    //   walk_group = var_C (group byte of the first record). 0 branches to sub_16D3E (path
    //   walking), non-zero to sub_50BAF (two-point interpolation) at loc_51950.
    //   walk_frames = word_EDD74 = max(path length+1 per record) for group 0, else 2.
    int walk_group = 0;
    int walk_frames = 0;
    bool walk_sets_start_tile = false; // VM 0x05 group-0 start coordinates are applied at playback, not parse time.
    bool walk_occupancy_preserving_candidate = false; // Complete group-0/group!=0 records; runtime still proves source and destination footprints.
    std::vector<VmWalkUnit> walk_units;
    std::vector<int> leave_units;     // UnitLeave (0x04): units_ indices to remove, in record order
    // A 0x04 (sub_51449) record is five bytes [sel][idx][mode][step][dir]. sub_17071 uses
    //   unk_E5A50[record] = -step (when non-zero the original plays no effect either: it is
    //                             pinned to `if (unk_E5A50[i] != 0) state = 2`)
    //   dir = direction from the sub_49ABC jump table (0 = south y+1, 1 = west x-1,
    //         2 = north y-1, 3 = east x+1)
    // mode is 0 = sub_17071 (path walking), 1 = sub_508E6(4), otherwise sub_50AA7.
    int retreat_mode = 0;
    std::vector<int> leave_steps;     // -(record byte 4), same order as leave_units
    std::vector<int> leave_dirs;      // record byte 5, same order as leave_units
    int face_refs[4] = { -1, -1, -1, -1 };  // Face (0x66): [sel0][idx0][sel1][idx1]
    uint8_t target_sub = 0, target_a = 0, target_b = 0; // AiTarget: sub_52377 sub plus two argument bytes
    uint8_t ai_bytes[5] = {};        // AiSettings: the five sub_522A7 payload bytes
    // VM_AI/VM_TARGET/VM_FACTION/VM_FACE resolve selector references at playback time.
    uint8_t unit_ref_sel = 0, unit_ref_id = 0;
    // Preserves the dword_11024C unit index used by sel 10..0x7F at parse time.
    int forced_target_context = -1;
    int value = 0;                    // Weather/Bgm/Flag value (Magic: spell byte, FactionTransform: action)
    int army = 0;                     // Faction new army (Magic: level byte)
    // TileChange (0x10): layer 0 = word_78A04 base map, nonzero = word_83DB4 top map.
    // sub_52C06 applies every record and calls sub_4559F once after the loop, so only the
    // final record of a run carries tile_last.
    int tile_layer = 0, tile_x = 0, tile_y = 0, tile_value = 0, tile_attr_value = 0;
    bool tile_last = false;
    std::vector<uint8_t> record;      // inline 0x88 record for FactionTransform action 1/3
    // Spawn (0x03) effect mode - sub_50D9F var_10 (first payload byte).
    //   0 = walk the move-queue path (sub_16D3E), 1 = drop in (sub_508E6(3)).
    //   dword_F0B9C+2 = walk repeat count (sub_508E6 adds 8 in mode 1).
    int spawn_mode = 0;
    int spawn_steps = 0;
    std::vector<int> spawn_units; // all units activated by this 0x03 event (walk_units holds only the effect targets)
    // Attack / UnitPose (0x07 sub_51C74)
    int attacker_unit = -1;               // edi - unit of the first record
    int attacker_mode = 0;                // var_14[0] - 0 normal, 1 -> dword_F0B8A+2=1 (special/skill), >=2 -> pose
    std::vector<VmAttackTarget> attack_targets;
    // Hp (0x0A sub_5211D)
    int hp_unit = -1;
    int hp_delta = 0;                     // 0 = instant death (sub_1DB1D), 0x7FFF = full heal, negative = damage
    int hp_dir = 0;                       // tail byte = knockback direction (sub_45ED2 dl)
    int hp_killer = -1;                   // sub_1DB1D killer (units_ index, -1 = none); the other side of the death event at HP<=0
    // Item (0x06 sub_5196F): [kind][item] and the receiver resolved at parse time (forced
    // target, otherwise the sub_499BD(3) random walk of the CC968 list).
    int item_unit = -1;
    uint8_t item_kind = 0;
    uint8_t item_id = 0;
};

struct CombatDialogueEntry {
    std::string text;
    int portrait_id = -1;
    std::string speaker_name;  // unit+0x02 name string (dialogue name line)
    int speaker_x = -1, speaker_y = -1; // speaker tile at parse time (re-read via speaker_unit when shown)
    int speaker_unit = -1; // units_ index for the speaker pointer sub_507BE keeps
    bool is_overlay = false; // true: "<< ... >>" location/caption overlay (center banner), false: portrait dialogue (bottom box)
};

// ==== Per-map trailer ====
// sub_105AD reads this right after the 768-byte palette. Layout, verified byte-for-byte
// against IM05.MAP:
//   [stat table A: base_tile_count * 20B]  (unk_7EBAC, skipped; unused in battle)
//   [stat table B: top_tile_count  * 20B]  (unk_7D824, skipped; unused in battle)
//   [9B]  MAP_EFFC registration slots, 3 x (start, count, mode) -> sub_5FFB5 / sub_601E5
//   [11B] MAP_EFFC.SYB per-index decal type selectors -> sub_10420
//   [16B] embedded base filename (e.g. "IM05.PCX\0...") used to derive "<name>.fpm"
//   [2B]  dword_BA834+2: FPM enable/parallax-bias value; 0xFFFF (-1) disables FPM
// These 9 bytes are tile effects, not weather: sub_105AD hands them to sub_5FFB5/sub_601E5
// to register map tile effects such as footprints. Snow and rain come from the byte_BA84D
// overlay (= weather_) selected by SCE header +0x04 and VM 0x71, drawn from the 72-frame
// weather.syb (sub_44AB6) by sub_4512E. IM05.MAP's 9 bytes are all zero.
struct MapTrailer {
    uint8_t weather_slots[3][3] = {}; // [i] = {start, count, mode} read raw from file (tile effects)
    uint8_t mapeffc[11] = {};
    std::string fpm_base_name;        // trimmed at '.', e.g. "IM05"
    // dword_BA834's high word equivalent. -1 (0xFFFF) => sub_10AEB skips the
    // FPM layer entirely for this map (see render_fpm_as_background()).
    // Otherwise it's the reference camera-Y (tile units) that splits
    // sub_10AEB's Y-parallax formula into its two branches (damped 4px/tile
    // above this row vs. 1px/tile at/below it).
    int16_t fpm_bias = -1;
    bool valid = false;

    bool any_weather_slot_enabled() const {
        for (int i = 0; i < 3; i++) if (weather_slots[i][0] != 0) return true;
        return false;
    }
};

// One frame of a MAP_EFFC.SYB terrain foot-effect sprite (sub_10420, unk_789AC entry).
struct TerrainOverlayFrame {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> rle;
};

// One Y-sorted draw entry built by render_battle_map (one original call to
// sub_12F2A/5F3FD/5F517/5F4AF). Declared here so sub_44DCC (highlight list) can redraw the
// same unit after the frame.
struct DrawEntry {
    int x,y,foot;
    int type; // 0=top tile, 1=CITIZEN SPR frame (idx), 2=NC2 frame (nc2), 3=solid silhouette (nc2+idx=colour)
              // 4=NC2 clipped (sub_5F4AF, clip_h), 5=terrain foot overlay RLE (ov)
    int idx;
    const NC2Frame* nc2 = nullptr;
    int clip_h = 0;                 // type 4: scanlines kept from the top
    int foot_x = 0;                 // type 2: foot-overlay start x (sub_12F2A var_C)
    const TerrainOverlayFrame* ov = nullptr; // type 5
};

class CombatII {
public:
    // References the single Video/Input instances owned by gene2.cpp.
    CombatII(Video& video, Input& input);
    ~CombatII();

    // Entry: scenario_name like "1-1", "IM05", or "1-1.SCE"
    // If name contains '.', treated as full filename. Else appended with .SCE
    int run(const std::string& scenario_param);

    // Exit code of the original process (the argument of sub_61CFF). MAIN.EXE returns to
    // loc_100EA whenever `spawnl` yields anything but 0xFF, so this drives the file IPC flow:
    //   0xFF = normal exit (end of main_ / no argument with contmap.dat missing / ESC)
    //   0x01 = sub_52D33 (0x12 next scenario) / sub_506F3 (0x01 stage clear)
    int exit_code() const { return return_code_ == 0 ? 0xFF : return_code_; }

private:
    Video& video_;
    Input& input_;
    Font font_;

    // ---- Paths ----
    std::string game_path(const char* filename) const;
    std::string scenario_path(const char* filename);
    std::string cstldata_path(const char* filename);

    // ---- Scenario loading ----
    bool load_scenario(const std::string& sce_filename);
    bool parse_sce_header(const uint8_t* data, size_t size);

    // ---- MAP loading (reuses PACK logic, simpler for battle maps) ----
    bool load_battle_map(const std::string& map_filename, bool use_sce_unit_records = false);
    void free_battle_map();

    // ---- FPM (Full Picture Map): optional cutscene background ----
    // The FPM is not a static 320x200 image. sub_10500 RLE-decodes the .FPM file into the
    // 480x400 (192,000-byte) static buffer unk_8B3E8, which is what fpm_decoded_ holds.
    // Every frame sub_10AEB computes a camera-dependent (col,row) into that buffer (see
    // render_fpm_as_background() for the formula) and sub_5EAE3 copies a flat 96,000-byte
    // (480x200) window into a separate 480x400 composite canvas (dword_755CC, allocated by
    // sub_5F7F5's nmalloc(0x2EE00)) at a fixed +48000 (row 100) baseline; the VGA-visible
    // 320x200 window is the centre of that canvas. The port skips the intermediate canvas
    // and crops fpm_decoded_ straight into the backbuffer with the equivalent offset.
    static constexpr int FPM_CANVAS_W = 480;
    static constexpr int FPM_CANVAS_H = 400; // 192000 / 480
    static constexpr int FPM_MARGIN_ROWS = 100; // dword_755D6(48000) / 480

    bool load_fpm(const std::string& fpm_filename);
    void render_fpm();
    void render_fpm_as_background(); // draw FPM behind tiles (sub_10AEB background path)
    // sub_10500 RLE decoder (control > 0xC0 repeats the next byte AL times, C0 escapes,
    // anything else is a literal). Decodes the FPM payload (file[0x3C .. filelen-0x300]) into
    // the full 480x400 (192,000-byte / 0x2EE00) canvas.
    static bool decode_fpm_sub10500(const std::vector<uint8_t>& file_data, std::vector<uint8_t>& out_canvas);

    // ---- Battle sprites (NC2 fallback via CITIZEN) ----
    bool load_battle_sprites();
    void init_dummy_units(bool parse_sce_snapshot);

    // ---- Weather (72-frame weather.syb + SCE header +0x04 / VM 0x71) ----
    // sub_44AB6 loads weather.syb (0x48 frames) through the dword_F8730 table, sub_44F30 dims
    // the palette, sub_4512E draws the per-frame overlay and sub_450F9 advances the phase
    // counter (word ptr dword_FA8B0).
    bool load_weather_effects(const std::string& map_filename);
    void reset_weather_palette_base();          // unk_FDC68 <- current palette (on SCE load)
    void apply_weather_palette(bool animate);   // sub_44F30 (+sub_49A3E fade, +sub_5F9AE commit)
    void weather_tick_phase();                  // sub_450F9: phase = (phase+1) % 9
    void render_weather_layer();                // sub_4512E: 3-column x 2-row overlay

    // ---- Top/bottom HUD frames and 16 px side bars (sub_45431) ----
    // sub_454B3 = sub_10AEB (map) -> sub_12BEE (units) -> sub_4512E (weather) and then
    // continues into sub_454D5, whose sub_45431 runs every frame:
    //   sub_44DF3 (top frame + morale gauge) -> sub_44EB0 (bottom frame + time dots) ->
    //   fill [0,16)x[0,200) and [304,320)x[0,200) with colour 0.
    // Map tiles start at (fine+4) and cover 13x13, so the HUD has to cover the 16 px sides
    // and the 4 px top and bottom to match the original.
    bool load_hud_frames();                     // sub_44AB6: sprites 14..17 of frame.syb
    void render_hud_frame();                    // sub_45431
    // Span blit from sub_5F3FD/5F42D/5F4AF. Coordinates are linear like the original
    // sub_5EB00 and wrap at the end of the buffer when they leave the 64K segment, so a pass
    // with negative y reappears at the bottom of the screen (the original does the same).
    // data_size is only a range guard. The original's row count (ecx) has no effect on pixel
    // output, so the port does not carry it (see render_weather_layer).
    void blit_weather_mask(int x, int y, const uint8_t* mask, int data_size = 0);
    // Draws only for 0x71/header weather 1..4 (sub_4512E: `dec al; cmp al,3; ja`).
    bool weather_layer_active() const { return weather_ >= 1 && weather_ <= 4 && weather_loaded_; }

    // ---- SCE script execution (pre-battle cutscene) ----
    void execute_cutscene();
    void dump_frame_if_requested();   // G2_DUMP_FRAME diagnostic (8-bit PGM)
    void dump_retreat_frame(int frame, int sub);  // G2_RETREAT_DUMP diagnostic (0x04 effect)
    // Plays the VM command timeline in order; dialogue/captions wait for input, camera/moves
    // advance per frame.
    void play_vm_events(const std::vector<VmEvent>& events);
    // sub_51211 activation: add the spawn event's pending units to the list and play their
    // walk/drop.
    void battle_spawn_activate(const VmEvent& ev);
    // S17 (sub_50D9F/sub_5069C/sub_51211): a 0x03 reinforcement checks each record's destination
    // with sub_16699 (footprint, occupancy, terrain). Records that pass appear in the same command;
    // a blocked record (mode 0 only) goes into the first free slot of sub_5069C's 11-byte queue
    // (unk_1006BC, 32 slots). sub_51211 re-tests slots in order each tick and spawns passes
    // together.
    void battle_spawn_queue_step();
    // sub_16699(x, y, facing, unit): returns 1 when every tile of the footprint (w x h, rows going
    // up from y) is inside the guard, free in CC998 and has terrain cost (EDD6C, the grid chosen by
    // sub_1665B) > 0. The unit itself is not in the occupancy grid yet.
    bool battle_16699(int x, int y, int facing, int unit_index);
    // Destination tile (path end, else start) and facing (load) of one unit in an event.
    void battle_spawn_dest(const VmEvent& ev, int unit_index, int& x, int& y, int& facing) const;
    // Partial event holding only the given units (walk list and frame count recomputed).
    VmEvent battle_spawn_subevent(const VmEvent& ev, const std::vector<int>& units) const;
    // Mirrors the 11-byte unk_1006BC queue (32 slots); used==false is a free slot (original
    // [+0]==0).
    struct PendingSpawn { VmEvent ev; bool used = false; };
    std::vector<PendingSpawn> battle_spawn_queue_;
    void battle_spawn_enqueue(const VmEvent& single);    // sub_5069C

    // ---- Camera scrolling (sub_50339 sets up, sub_5046B advances every frame) ----
    // The 0x0F handler only stores the target/speed globals (sub_50339) and returns
    // (non-blocking); sub_5046B, called each frame by the battle loop, takes one step.
    //   word_110584                      = remaining X frames
    //   word ptr dword_110580+2 (110582) = remaining Y frames
    //   word ptr dword_110580  (110580)  = X subpixel step (+/-cl)
    //   word_11057E                      = Y subpixel step (+/-cl)
    //   dword_7001C / dword_BA838        = sub-tile offsets (X: (-12,12], Y: (-16,0])
    // Sign convention (unchanged): world position is cam_tile*24 - cam_fine, so a target larger
    // than the current value uses step -cl (loc_5040A/loc_5045F). cam_fine therefore ranges over
    // (-12,12] in X and (-16,0] in Y, and the renderer adds `+ cam_fine_x_` the same way
    // (sub_10AEB: canvas_x = fineX + 4 + i*24).
    int16_t cam_scroll_frames_x_ = 0;
    int16_t cam_scroll_frames_y_ = 0;
    int16_t cam_step_x_ = 0;
    int16_t cam_step_y_ = 0;
    // loc_52A2C waypoint queue (the original is a 16-slot ring buffer dword_100659/100639/
    // 100619/1005F9 plus the byte_110695 index). It only queues when both the X and Y frame
    // counts are non-zero (the two `jz loc_52B24` in loc_52A2C); if either is 0 the call goes
    // straight to sub_50339 and overwrites the scroll in progress.
    struct CameraWaypoint { int mode = 0, x = 0, y = 0, speed = 0; };
    std::deque<CameraWaypoint> cam_waypoints_;
    void camera_setup(int mode, int x, int y, int speed); // sub_50339
    bool camera_scrolling() const;                        // word_110584 || 110582
    bool camera_fully_scrolling() const;                  // word_110584 && 110582 (queueing condition)
    int  camera_update_frame();                           // sub_5046B (0 = in progress, 1 = done, 2 = popped from the queue)
    void drain_camera_scroll();                           // loc_53026 (drain scrolls/windows at the end of the body)

    // ---- Cutscene frame/input ----
    // Currently shown UI, kept in state so it can be redrawn on re-render.
    std::string ui_text_;
    bool ui_is_overlay_ = false;
    int  ui_portrait_ = -1;
    std::string ui_speaker_;        // dialogue name line (unit+0x02)
    int  ui_speaker_tx_ = -1, ui_speaker_ty_ = -1;  // speaker tile (sub_1124A/loc_48368 circle effect)
    int  ui_speaker_unit_ = -1;     // speaker unit index - the original keeps a unit pointer, so
                                    // Playback resolves the tile through the unit, not this tile.
    int  last_dialogue_unit_ = -1;  // dword_1102B8: previous dialogue speaker unit. For
                                    // Consecutive lines by the same speaker sub_507BE sets
                                    // bx=0 and skips the circle animation (loc_48368).
    bool ui_visible_ = false;
    bool skip_cutscene_ = false;    // ESC skips the remaining effects
    void set_ui_text(const std::string& text, bool is_overlay, int portrait,
                     const std::string& speaker_name = std::string(),
                     int speaker_tx = -1, int speaker_ty = -1, int speaker_unit = -1);
    void render_overlay();
    // Draws one frame as background (FPM + tiles) -> weather -> units -> UI, which is
    // sub_454B3 + sub_454D5.
    void render_battle_frame();
    void present_cutscene_frame();
    // ---- Original frame chain (the order the spell drivers call every frame) ----
    void battle_fx_world();      // sub_10AEB (map/FPM) + sub_12D65 (units) + sub_4512E (weather)
    void battle_fx_world_keep(); // sub_454B3 without clearing the backbuffer (equip screens sub_48BC1/48863)
    void battle_fx_flush();      // sub_12E01: send queued spell sprites (5F570/5EE51/5F305)
    void battle_fx_overlay();    // sub_454D5: dim -> sub_12161 -> sub_44DCC -> sub_13546 -> sub_45431
    void battle_fx_commit();     // sub_5E9E8 (send including the dialogue box)
    // ---- sub_44D89 / sub_44D81 / sub_44D75: dword_F9F08 highlight list (max 59h) ----
    // sub_44DCC in 454D5 redraws these units with sub_12F2A, above spell effects and dimming
    // but below the HUD.
    void battle_highlight_add(int unit_index);
    void battle_highlight_pop();
    void battle_highlight_clear();
    void render_highlight_units();                 // sub_44DCC
    // sub_44DCC's units-only path: used when 341C7/37FA4 clear the screen with 5E9D0 and then
    // redraw the registered units with sub_12F2A (the draw-entry list is empty).
    void render_highlight_units_direct();
    // sub_2CBA1: 44D89 registration that inserts the caster at its row (y) position.
    //   Splits F0648 on [1] (y) and inserts the caster between the halves, i.e. depth sorting.
    void battle_highlight_add_depth(int caster_index);
    // sub_12F2A redraw of a single unit (sprite + foot shadow).
    void battle_draw_unit_now(int unit_index);
    // ---- dword_F1BA0/F153A/F156A/F14DC: 24-slot afterimage trail (sub_30544 / sub_305A2) ----
    //   F153A/F156A = per-slot x/y, F14DC = per-slot sprite, F1BA0/+2 = start/end,
    //   F1B9C = last unit pushed. 305A2 draws the unit with pose [unit+64h] and pushes the
    //   D66A8 entry ([4]/[5] offsets, [9] sprite) into the next slot.
    struct FxTrailSlot { int x = 0, y = 0; const NC2Frame* fr = nullptr; };
    FxTrailSlot fx_trail_[24];
    int fx_trail_start_ = 0, fx_trail_end_ = 0;
    int fx_trail_last_unit_ = -1;
    void fx_trail_reset();                         // sub_30544 (start = end = 0)
    void fx_trail_step(int unit_index, int pose);  // sub_305A2 (unit<0 consumes trail only)
    // 30824 (start=0x60) / 307A6 (start=0x00): blend a master-palette range pct% toward colour K.
    void fx_pal_mix(int start, int end, int color_index, int pct);
    // sub_5EE51: transparent-blit one unk_F0DAA frame at (x, y).
    void fx_blit_nsp(int frame, int x, int y);
    // sub_6106C: builds a palette LUT page (341C7 uses page 3, colour 11h, brightness 46h).
    // The original selects a 256-byte row inside dword_128554 by page number; page 3 is used only
    // by this effect, so the port keeps a single row (6104A(3) selects it, the tail's 5ECC7(0)
    // clears mode 1).
    void fx_shade_lut_build(int color_index, int shade_pct);
    // sub_5EDDE + sub_5EE2B (5ECC7 mode 1): recolour the existing pixels inside a rectangle via the
    // LUT.
    void fx_shade_rect(int x0, int y0, int x1, int y1);
    uint8_t fx_shade_lut_[256] = {};   // dword_128554 + 3*256 (page 3)
    // sub_308A3: lightning column, stacking the same frame four times at (x, y + 40h*k).
    void fx_bolt_strip(int x, int y, int frame);
    // ---- V-weapon (normal attacks) effects (2CC3A / 341C7 / 37FA4) ----
    // sfx_id is the hit sound id the caller passes in cx (sub_2F47F/2F68C = 1, sub_2FED7 = 2).
    // 2CC3A forwards `[esp+66h]>>16` (the cx saved by its prologue) straight to 4F4CD.
    int  battle_fx_2cc3a_anim(int unit_index, int variant, int sfx_id);   // sub_2CC3A effect body
    void battle_fx_341c7_anim(int unit_index);                // sub_341C7 f_quake
    void battle_fx_37fa4_anim(int unit_index);                // sub_37FA4 irus (steps 1-7)
    void battle_fx_32589_anim(int unit_index, int target_index); // sub_32589 arcion (129 frames)
    void battle_fx_34c48_anim(int unit_index);                // sub_34C48 shak2 (106 frames)
    bool battle_fx_32bcb_anim(int unit_index);                // sub_32BCB icer.nsp (429 frames)
    void battle_fx_367ee_anim(int unit_index);                // sub_367EE nang.nsp (40 shockwaves)
    void battle_fx_3aee8_anim(int unit_index);                // sub_3AEE8 io.nsp (3ADBE 16 frames x 28)
    // ---- Own NSP effects for the remaining 6 of the 13 weapon bodies ----
    // The return value matches battle_weapon_XXXXX (1 on success, -1 when there is no target);
    // targeting, damage and triggers keep the existing port behaviour, only the effect itself
    // follows the ASM.
    int battle_fx_308ee_anim(int unit_index);                 // sub_308EE bird.nsp (45045..45404)
    int battle_fx_31283_anim(int unit_index);                 // sub_31283 asura2.nsp (45691~46913)
    int battle_fx_356db_anim(int unit_index);                 // sub_356DB sarah.nsp (50610~51300)
    int battle_fx_35fc2_anim(int unit_index);                 // sub_35FC2 stormaxe.nsp (51301~51720)
    int battle_fx_377a6_anim(int unit_index);                 // sub_377A6 shu.nsp (52968~53597)
    int battle_fx_39a49_anim(int unit_index);                 // sub_39A49 rain.nsp (55981~56960)
    // sub_306E6: push the unit's current pose sprite into the trail ring at explicit coordinates.
    void fx_trail_add_at(int unit_index, int x, int y);
    // sub_30784: advance the trail ring start by one slot (consumes the oldest afterimage).
    void fx_trail_drop();
    // Step 8, loc_381EF: camera midpoint, 12-frame afterimage interpolation (306E6), tile move
    //   (11EE4/11E04), 305A2, 5E9E8, 4F4CD(32h). dash_x/dash_y = precomputed landing tile.
    void fx_37fa4_rush(int unit_index, int dash_x, int dash_y);
    // Step 9, loc_383A2 family: one block of 5E9D0 -> 5EE51(F0DAA[irus_frame], +A0/+64) -> 30784
    //   -> 305A2(pose) -> 44DCC -> 5E9E8 -> 499E4, repeated nframes times.
    void fx_37fa4_hit_block(int unit_index, int irus_frame, int pose, int nframes);
    // Draw switch shared by render_battle_map and 44DCC playback.
    void blit_draw_entry(const DrawEntry& e);
    // sub_508E6(3): mode-1 spawn drop (EVENT.SYB step table + landing dust effect).
    void play_spawn_fall(const VmEvent& ev);
    // sub_5004F: load EVENT.SYB as 8 NC2Frames (step sprites for the drop). It is a member
    // function because it needs game_path.
    void load_event_syb();
    // Bank filled by load_event_syb; the renderer reads the step sprites from it.
    const std::vector<NC2Frame>* event_syb_ = nullptr;
    // Wait for input for at least min_ms while advancing camera scrolling every frame.
    // Returns true when an exit or skip was requested.
    bool wait_for_input(uint32_t min_ms);
    // sub_516D0(0x05) walk animation. The original walks all records of the command together:
    //   group 0   -> sub_16D3E: repeats word_EDD74 = max(path length + 1) times, each time
    //                four subframes of `sub_1033B(9)` (9/175.47 = 51.3 ms) = 205 ms per tile.
    //               Every subframe runs sub_4559F (camera/window) + sub_5E9E8 (present) + sub_499E4 (tick).
    //   group != 0 -> sub_50BAF: word_EDD74 = (2 + 8) * 2 = 20 frames, stepped by
    //               `sub_1033B(0x0C)` (12/175.47 = 68.4 ms).
    void animate_unit_walk(const VmEvent& ev);
    // Facing while walking: own tile -> next path point, in the same order as sub_16D3E loc_16E1A.
    static int8_t walk_facing_toward(int x, int y, int tx, int ty);

    // ---- Game timer (sub_1033B / sub_10364 / sub_499E4 / sub_499EF) ----
    // Sound driver init (sub_5C665) reprograms PIT channel 0 to 0x06A4 (1700):
    // `mov [esp+48h+var_10], 6A4h` -> outp(0x43,0x36) + outp(0x40, lo/hi). The PIT runs at
    // 1193182/1700 = 701.87 Hz, but the sound ISR (loc_5BCED) calls the game ISR
    // (loc_101F4, chained via byte_123744) only once per four invocations (dword_755B8 = 4,
    // the value sub_5BCAA(160) computes), so the game sees 175.47 Hz.
    // loc_101F4 counts word_789A2 and sets byte_789A7 on reaching word_70004.
    //   sub_1033B(period): word_70004 = max(period,1), byte_789A7 = 0, word_789A2 = 0
    //   sub_10364():       returns 1 when byte_789A7 is set and clears counter/flag (one tick)
    //   sub_499E4():       waits until sub_10364 returns 1, i.e. one tick
    //   sub_499EF(ticks):  waits for a key or ticks ticks (dialogue auto-advance = 700 ~ 3.99 s)
    static constexpr double kTimerHz = 1193182.0 / 1700.0 / 4.0;
    int  timer_period_ = 1;      // word_70004
    // The original counter (word_789A2) and flag (byte_789A7) are tick-resolution ISR state.
    // The port keeps the same meaning as a "next tick deadline" timestamp so polling and
    // SDL_Delay error does not accumulate per wait (equivalent to sub_10364 resetting the
    // counter to 0).
    double timer_deadline_us_ = 0.0;
    bool auto_advance_ = false;  // G2_AUTOADVANCE test hook (consumes ticks immediately)
    bool frame_trace_ = false;   // G2_FRAME_TRACE: logs battle-viewer frame ticks (byte_789A8)
    double timer_now_us();            // SDL high-resolution counter, in microseconds
    void game_timer_set(int period);  // sub_1033B
    bool game_timer_poll();           // sub_10364
    void wait_game_tick();            // sub_499E4
    bool wait_key_or_ticks(int ticks); // sub_499EF (true = exit or skip requested)

    // ---- INT8 frame ticks (loc_101F4 / sub_1031D / sub_102FF) ----
    // word_7899E counts the 175.47 Hz game ticks:
    //   counter == 0x0F (15) -> byte_789A8 = 1                          (consumed by sub_1031D)
    //   counter == 0x1E (30) -> byte_789A8 = byte_789A6 = 1, counter = 0 (sub_102FF is 30 ticks)
    // so byte_789A8 rises every 15 ticks = 85.5 ms = 11.7 Hz and byte_789A6 every 30 ticks =
    // 171 ms = 5.85 Hz.
    //   sub_1031D: returns 1 when byte_789A8 is set and clears it -> one battle-viewer frame
    //              (sub_5580F)
    //   sub_102FF: same for byte_789A6                    -> battle turn logic (sub_4BA7F/sub_54C2C)
    static constexpr int kFrameTickPeriod = 15; // byte_789A8 period (game ticks)
    static constexpr int kHalfTickPeriod  = 30; // byte_789A6 period
    int  isr_counter_ = 0;         // word_7899E
    bool isr_frame_flag_ = false;  // byte_789A8
    bool isr_half_flag_ = false;   // byte_789A6
    double isr_base_us_ = 0.0;     // tick reference time
    void isr_advance();            // loc_101F4 - consume elapsed game ticks and raise the flags
    bool frame_poll();             // sub_1031D - consume byte_789A8
    bool half_tick_poll();         // sub_102FF - consume byte_789A6
    // ---- Battle tick engine (sub_19652 init / sub_19D53 update / sub_19F5D pause ISR hook) ----
    // One sub_19D53 call is one battle tick. The original viewer loop (sub_5580F) calls it on
    // every iteration and gates only drawing and camera work on byte_789A8. sub_5580F reads
    // the return value (ax):
    //   1 = paused/waiting or nothing happened, 2 = player turn started (wait for input after
    //   the camera move), 3 = the enemy AI acted (re-read mouse/screen).
    void battle_reset();                 // sub_19652 (sub_4FD25 calls it before loading a scenario)
    int  battle_update();                // sub_19D53
    bool battle_tick_due();              // port-only pacing gate; the original does no gating (see the note below)
    void battle_pause_isr_hook();        // sub_19F5D (per INT8: Space held sets F042E = 2)
    void battle_resume_key();            // sub_5580F loc_55B19 (Space key: leave pause)
    void battle_time_of_day_step();      // sub_197CA (advance the time-of-day phase and dim the palette)
    void battle_load_time_of_day();      // sub_4E4DD + sub_4ED18 (FDF82/FDF84 initial values)
    void battle_load_item_table();       // sub_18115: ITEM.DAT -> unk_EDD88, which is game data absent from the EXE;
                                         // The class/speed/adjust/morale tables live in
                                         // combat_tables.h
    void battle_atb_step();              // sub_19AEF: gauge/ready queue plus the ailment-duration engine
    void battle_status_timer_19892(CombatUnitPlacement& u); // sub_19892: decrement and expire buff/ailment durations
    int  battle_status_ailment_1992d(int unit_index, int slot, int* out_damage); // sub_1992D: poison/slow damage, ATB stop
    void battle_status_damage_anim(const std::vector<std::pair<int,int>>& targets); // sub_19AEF damage list animation + sub_1ED90
    void battle_recalc_summon_attack(int unit_index); // sub_17E17: recompute summon attack power
    // ---- sub_17A20 / sub_1767E / sub_176F1 / sub_1796B: kill EXP, level-up and gold ----
    void battle_gain_exp(int unit_index, int victim_index); // sub_17A20 (unit = killer; victim=-1 means tile-EXP level-up only)
    int  battle_growth_1767e(const CombatUnitPlacement& u, int stat) const; // growth amount (0 = none)
    bool battle_growth_176f1(CombatUnitPlacement& u, int stat);             // weapon/magic growth (false = stop the loop)
    void battle_recalc_stats_1796b(CombatUnitPlacement& u);                 // recompute attack/defense/TP/def59 after a level-up
    int  battle_item_col_sum(const CombatUnitPlacement& u, int column) const; // sub_184F9
    bool battle_unit_is_player(const CombatUnitPlacement& u) const; // modeled effective army/flags; unknown remaps use raw+1 fallback
    bool battle_unit_is_ai_controlled(const CombatUnitPlacement& u) const; // modeled effective army/flags; unknown remaps use raw+1 fallback
    int battle_turn_scheduler();         // sub_19D53 loc_19E02~loc_19F56
    bool battle_player_input();          // sub_4D674 state machine (true = Esc consumed, do not end the module)
    void render_ai_ready_markers();      // sub_12161: ETC.ICO marker over units with gauge [+0x56] == 0xFF
    void battle_unit_idle_pose_step();   // sub_12065: toggle idle pose [64h] 0/1 and advance E518E/E4FB6 phases
    // sub_123C1: queue a chr_effc.syb overlay according to the [+1Eh]/[+15h] status bits.
    void unit_status_overlay_123c1(const CombatUnitPlacement& u, std::vector<DrawEntry>& out,
                                   int base_x, int base_y, int foot);
    void render_player_move_overlay();
    void battle_set_overlay(uint16_t m) { battle_overlay_mode_ = m; }   // sub_44DEC
    uint8_t battle_get_state() const { return battle_state_; }           // sub_19FA3
    void battle_set_state(uint8_t v) { battle_state_ = v; }              // sub_19F9D
    bool load_etc_icons();               // sub_44A4C: etc.ico 0..7 ready markers, 8..10 player turn/pause
    // MENUSTAT.SYB (sub_4E448: sub_4F25F loads 0x37 frames + sub_10031). The info-window panel is
    // frame #36 (FD7AC/FD7AE/FD7B0 in sub_48734).
    SPRParser menustat_;
    bool menustat_loaded_ = false;
    bool menustat_tried_ = false;
    bool load_menustat();
    void render_battle_state_overlay();  // late sub_454D5: state icons when dword_75008 == 1

    // ---- Message window queue (filled by sub_52B4F, consumed one frame at a time by sub_5046B)
    // ----
    // dword_110250 != 0 means a window is showing and word_11057C counts its remaining frames.
    // At 0 the next queued window is shown (unk_FF0BC slots + frame counts in word_10016A,
    // indices byte_110691/110692); an empty queue closes the window (loc_504F1).
    struct MessageWindow { std::string text; int frames = 0; };
    bool msg_active_ = false;
    MessageWindow msg_cur_;
    std::deque<MessageWindow> msg_queue_;
    int msg_timer_ = 0;           // word_11057C
    void message_window_set(const std::string& text, int frames); // sub_52B4F
    void render_message_window(); // sub_5E048(x=0x20, y=0x24, text) from the head of sub_5046B
    void show_dialogue_line(const std::string& text, int portrait_id,
                            const std::string& speaker_name = std::string(),
                            int speaker_tx = -1, int speaker_ty = -1, int speaker_unit = -1);
    void show_overlay_message(const std::string& text); // sub_5046B -> sub_5E048(x=0x20, y=0x24): top-left text, no box or dimming
    void show_portrait(int portrait_id, int x, int y); // y is the bottom edge of the portrait (same rule as PACK)

    // ---- Speaker selection frame: box around the sprite plus HP at the bottom ----
    //   sub_49B16: load MAIN.CUR (first 4 = cursor, dword_FB080; last 4 = corners, dword_FB070)
    //   sub_49FE1: speaker unit clip size/offset (at least 24x24) -> start of draw_speaker_frame
    //   sub_4A106: screen-space transform + clipping             -> middle
    //   sub_49CB1: border (colour cycling 9->10->11) + 4 corners -> later part
    //   sub_13498: HP value (shadow + faction colour)             -> end
    bool load_main_cur();
    void draw_speaker_frame(int unit_index);

    // ---- Mouse cursor (first 4 of sub_49B16 / sub_49C05) ----
    // The original stores the first four MAIN.CUR records in dword_FB080; sub_49C05(1) picks
    // frame 0..3 from whether the mouse is right of 0x118 and below 0xA4 (0=TL, 1=TR, 2=BL,
    // 3=BR). sub_49C05(0) is the initialisation path and always uses frame 0. The hotspot is
    // word_75014/75016.
    bool init_mouse();
    void shutdown_mouse();
    void cursor_apply(bool quadrant);                 // sub_49C05
    std::vector<CursorFrame> main_cursor_frames_;     // MAIN.CUR 0..3 (dword_FB080)
    int cursor_ids_[4] = {-1, -1, -1, -1};            // result of Video::cursor_register
    bool mouse_initialized_ = false;
    std::vector<uint8_t> sel_corner_[4];        // dword_FB070[0..3]: TL/TR/BL/BR RLE streams
    bool sel_corner_tried_ = false;
    bool sel_corner_loaded_ = false;
    // High byte of dword_75021 (= 0x75024): sub_49CB1 cycles the colour 9 -> 10 -> 11 -> 9 on
    // every draw. Initial value 9, read from the COMBATII.EXE LE object 2 (base 0x70000) byte
    // 0x75024.
    uint8_t sel_frame_color_ = 9;

    // ---- Small font (TEXT.CHR): loaded by sub_5F874, strings by sub_608E6, glyphs by sub_5F093
    // ----
    // Separate from the dialogue font (NEWASCII.FNT); HUD text such as HP and damage numbers uses
    // this 5-column mask font.
    struct SmallGlyph { int w = 0, h = 0; std::vector<uint8_t> px; };   // px: w*h, 0xFF = transparent
    SmallGlyph small_font_[100];                // dword_127D4C[0..99]
    bool small_font_tried_ = false;
    bool small_font_loaded_ = false;
    bool load_small_font();
    void draw_small_text(int x, int y, const char* s, uint8_t color, int spacing);
    // Draws with the sub_5DD5B rule (body of sub_5E048): 16 px cells, and when
    // `cmp x,0x130; jle` falls through (x > 304) it wraps to x=0 with y += word_755C6 (0x10).
    void draw_text_wrapped(int x, int y, const std::string& text);

    // Dialogue helpers (PACK dimming/frame reuse, simplified)
    void render_dialog_dimming(int x, int y, int w, int h);   // sub_5EDDE (mode 1: dim fill)
    // ---- Dialogue dimming primitives (sub_6104A(0) table + sub_5ECC7(1) fill mode) ----
    const uint8_t* dialog_shade_lut();                 // dword_128550 (row 0), rebuilt when the palette changes
    void shade_span(int x, int y, int w);              // sub_5EE2B: dim w pixels one step through the table
    void ellipse_shade_fill(int cx, int cy, int rx, int ry); // sub_6075A: filled ellipse (rx horizontal, ry vertical)
    void ellipse_solid_fill(int cx, int cy, int rx, int ry, uint8_t color); // sub_6075A solid mode (sub_5EC44)
    void circle_fill(int cx, int cy, int r) { ellipse_shade_fill(cx, cy, r, r); }  // compatibility wrapper
    uint8_t dlg_shade_lut_[256] = {};                  // dim table (index -> one step darker index)
    // Palette captured when the table was built: unk_12824C at the time sub_55B59 calls
    // sub_6106C. sub_4ED18+119 has already run sub_44F30 (SCE header weather) on unk_12824C, so
    // this is the weather-adjusted palette. Later VM weather (0x71 -> sub_44F30) changes the
    // palette but does not rebuild the table.
    uint8_t dlg_shade_pal_[256][3] = {};
    bool    dlg_shade_pal_set_ = false;
    bool    dlg_shade_valid_ = false;
    void snapshot_dialog_shade_palette();              // called right after scenario load (initial weather applied)
    void render_dialog_frame(int x, int y, int w, int h);

    // ---- Map rendering and scrolling ----
    void render_battle_map();
    void render_sprites();
    void render_battle_viewer(); // one battle-viewer frame (map -> weather -> dim -> markers -> HUD -> sprites)
    void update_scroll();
    // Manual scroll (sub_4A32C mouse edge, sub_4A3FF arrow keys). Like the original it moves one
    // tile every 3 game ticks in sub-steps of a stage width (word_70020 = 4/8 px), drawing one
    // frame per sub-step (sub_10DA7 -> sub_45577). now_us takes timer_now_us().
    // draw_frame: how to draw one sub-step frame (default: the whole battle viewer); callers with
    //   a different composite, such as deployment, pass their own (sub_45577 = sub_454B3 + present).
    // Returns true when the camera actually moved and at least one frame was drawn.
    bool handle_scroll_input(double now_us, const std::function<void()>& draw_frame = {});

    // ---- Transition / IPC ----
    void do_transition(const std::string& cmd);
    void write_dollar(const std::string& cmd);
    void write_contmap_dat(const std::string& name, uint8_t flag);  // sub_4FD6B
    void write_blackbox_state(uint8_t value);                       // sub_4EC9A

    // ---- State ----
    CombatMapInfo map_info_;
    std::vector<uint8_t> fpm_data_;
    int fpm_w_ = 320, fpm_h_ = 200;
    std::string loaded_map_filename_; // e.g. IM05.MAP for weather heuristic

    // Per-map trailer parsed straight after the palette in load_battle_map()
    // (weather zone slots, MAP_EFFC indices, embedded FPM name, FPM bias).
    MapTrailer map_trailer_;

    // PACK-style map layers (reused for battle maps where applicable)
    struct CombatTopTile {
        uint16_t w=0,h=0;
        int16_t yoff=0;
        uint16_t foot=0;
        uint32_t dsize=0;
        std::vector<uint8_t> data;
    };
    std::vector<std::vector<uint8_t>> base_tiles_data_; // 24x16
    std::vector<CombatTopTile> top_tile_sprites_;
    uint16_t* layer_base_tiles_ = nullptr;
    uint16_t* layer_top_tiles_ = nullptr;
    uint8_t* layer_passable_ = nullptr;
    uint8_t  battle_palette_[256][3] = {};
    uint32_t map_w_ = 0, map_h_ = 0, tile_count_ = 0;

    // Camera (PACK compatible + COMBATII fixed-point variants)
    int cam_tile_x_ = 0, cam_tile_y_ = 0;
    // Manual camera scroll hold state (sub_4A32C mouse edge, sub_4A3FF arrow-key stepping).
    // The original moves one tile per direction (X 24 px / Y 16 px) every 3 game ticks and
    // redraws through sub_45577 at every word_70020 (4/8 px) sub-step inside that tile. The outer
    // band (x<40/x>280/y<20/y>180) starts the first tile; the inner band (x<=22/x>=298/y<=8/
    // y>=192, the cmp 8/C0h/16h/12Ah tail of 4A32C) gates the following ones. The port keeps the
    // same period (3/kTimerHz s ~ 17.1 ms) and sub-step width per axis.
    // Keys repeat while held (4A3FF, no inner band); the mouse repeats only on the inner band
    // after entering the outer one for a tile. Outer-held with the pointer outside the inner band
    // retries one tile per period (the original caller loop re-calls one step at a time).
    double scroll_resid_x_ = 0.0;         // unused (leftover of the continuous approximation; stays 0)
    double scroll_resid_y_ = 0.0;         // unused (same)
    double scroll_last_us_ = 0.0;         // last scroll decision time (unused, kept for compatibility)
    int scroll_hold_x_ = 0;               // X hold direction: +1 left (4B00h) / -1 right (4D00h) / 0 none
    int scroll_hold_y_ = 0;               // Y hold direction: +1 up (4800h) / -1 down (5000h) / 0 none
    double scroll_next_x_us_ = 0.0;       // earliest time of the next X step
    double scroll_next_y_us_ = 0.0;       // earliest time of the next Y step
    // Rest values of the original sub-tile camera offsets. While the battle screen is idle
    // dword_7001C (fineX) = 0Ch and dword_BA838 (fineY) = 0: sub_21BE8/sub_22D18/sub_2799A/
    // sub_35FC2 always restore `mov ecx,0Ch; mov word ptr dword_7001C,cx; mov word ptr
    // dword_BA838,si(=0)` after a transition or shake. Not 0: 0 shifts the whole map 12 px left.
    static constexpr int CAM_FINE_X_REST = 12;
    static constexpr int CAM_FINE_Y_REST = 0;
    int cam_fine_x_ = CAM_FINE_X_REST, cam_fine_y_ = CAM_FINE_Y_REST;
    // COMBATII 16.16 fixed-point cameras (dword_BA838, dword_BA83C, dword_BA842)
    int32_t cam_fixed_x_ = 0; // dword_BA838
    int32_t cam_fixed_y_ = 0; // dword_BA83C
    int32_t map_width_fixed_ = 0; // dword_BA842 (W << 16)

    std::vector<CombatUnitPlacement> units_;
    // True only when all four static SCE groups and every declared direct, unlinked placement were
    // parsed.
    bool battle_sce_snapshot_complete_ = false;
    // Cleared once an unmodeled operation can desynchronize the vector from CC968/CC998.
    bool battle_wc1_occupancy_state_known_ = false;
    std::vector<CombatDialogueEntry> dialogue_queue_;

    // Battle sprite cache - CITIZEN1/2.SPR (the PACK citizen bank) is the fallback.
    SPRParser battle_sprites_;
    bool battle_sprites_loaded_ = false;

    // ---- NC2 character sprites (sub_11575 / sub_1198D) ----
    //   The type code in unit record [+0x60] selects `CHARACTE\<name>` through off_70024
    //   (normal) / 703A8 (large) / 705A4 (cavalry), and sub_11575 puts that file's 44 records
    //   (4 facings x 11 poses) into the slot table. The port loads each type code once and caches
    //   it. The raw D66A8 table is read from this cache with the `combatii_d66a8.h` layout
    //   (slot = per-type block, minus the [9..12] sprite pointer).
    mutable std::unordered_map<int, std::unique_ptr<NC2Parser>> nc2_chars_;
    std::string characte_path(const char* filename) const;
    // Loads the NC2 file for a type code on demand and returns the (facing, pose) frame.
    const NC2Frame* nc2_frame_for(int type_code, int dir, int pose) const;
    // Raw 13-byte D66A8 entry (the sub_11575 loader, minus [9..12]); false when the frame is
    // missing.
    bool battle_d66a8_raw(int type_code, int dir, int pose, uint8_t out13[13]) const;
    // D66A8 footprint (entry[0]/[1], facing pose 0). Falls back to 1x1 and false when missing.
    bool battle_d66a8_footprint(int type_code, int dir, int* w, int* h) const;
    // Same lookup for a specific D66A8 pose (sub_11E04 uses the current [unit+64h]).
    bool battle_d66a8_footprint_pose(int type_code, int dir, int pose, int* w, int* h) const;
    // Draws a w*h strip, skipping 0xFF (transparent) pixels (the sub_10083 convention).
    void blit_nc2_frame(int x, int y, const NC2Frame& fr);
    // sub_5F517: ignore the frame pixels and fill every opaque pixel with a solid colour (the
    // unit silhouette of the drop effect), painting the whole (facing, 9-dx) frame.
    void blit_nc2_frame_solid(int x, int y, const NC2Frame& fr, uint8_t color);

    // ---- Weather state (sub_44AB6 / sub_44F30 / sub_4512E / sub_450F9) ----
    SPRParser weather_sprites_;                 // weather.syb 72 frames (144x96 masks)
    bool weather_loaded_ = false;

    // ---- HUD state (sub_44AB6 frame.syb / sub_44DF3 / sub_44EB0) ----
    SPRParser hud_frames_;                      // frame.syb (14 = top, 15 = bottom, 16/17 = time dots left/right)
    bool hud_frames_loaded_ = false;
    bool hud_frames_tried_ = false;
    // byte_74138: initialised to 0x0A by sub_19652 (changed by sub_1A116). Bottom time dots =
    // ((dword_F0418 % byte_74138) + 1) >> 1.
    int hud_time_divisor_ = 10;
    // byte_BA84D (dword_BA84A+3): 0 = none, 1..4 = weather type.
    //   Read from SCE header +0x04 (sub_4ED18) and changed by VM 0x71 (sub_527FF).
    //   1: x0.80 / 2: x0.80 / 3: x0.80+0x0C / 4: x0.50+0x15 (sub_44F30)
    uint8_t weather_ = 0;
    // unk_FDC68 (base palette before dimming) / unk_F9188 (current DAC palette)
    uint8_t weather_base_palette_[256][3] = {};
    uint8_t weather_cur_palette_[256][3] = {};
    // Word ptr dword_FA8B0: overlay phase 0..8, advanced twice per 15 ticks by the ISR.
    uint16_t weather_phase_ = 0;
    // byte_70006 (default 1): when 0 the phase stops advancing (sub_103A0/sub_103A8).
    bool weather_tick_enabled_ = true;
    // SCE header (read in order by sub_4ED18): +0x00 camera X, +0x02 camera Y,
    //   +0x04 weather, +0x06 byte_BA84E (map kind), +0x08 13-byte map filename.
    uint16_t sce_cam_x_ = 0, sce_cam_y_ = 0;
    uint8_t sce_map_kind_ = 0;
    // ---- Foot sink/clipping/foot effects (sub_12F2A + sub_10420/unk_70A58/unk_789AC) ----
    static constexpr int TERRAIN_DEPTH[16] =
        { 1, 2, 3, 2, 2, 2, 2, 2, 4, 4, 4, 4, 3, 3, 2, 2 }; // unk_70A58[16]
    TerrainOverlayFrame terrain_overlay_[32];  // unk_789AC: 16 terrain types x 2 frames
    // sub_44AB6: first 0x4C frames of battle.syb (projectiles etc.); sub_11BB8: wea_effc/chr_effc.
    // All three are standard SPR files; the port caches their 12-byte metadata and RLE streams.
    std::vector<TerrainOverlayFrame> battle_effects_, wea_effc_, chr_effc_;
    bool effect_sprites_loaded_ = false;
    uint32_t combat_rng_ = 0x12345678;          // sub_499BD equivalent (deterministic LCG)
    bool battle_hostility_inverted_ = false;    // byte_E518D: temporary hostility flip during sub_44913 AI dispatch
    TerrainOverlayFrame shadow_sprites_[7];    // dword_D6658: unit foot shadows (SHADOW.SYB)
    int terrain_anim_frame_ = 0;               // dword_BA84A+2 (toggled by sub_10A30 every 30 ticks)
    // ---- Base/top tile animation (sub_105AD stat load -> sub_109EE/sub_10A0F resolve -> sub_10A30
    // advance) ----
    //   Keeps the per-tile 20-byte stat tables of the MAP file (unk_7EBAC/unk_7D824, 10 words per
    //   tile). The frame counters (unk_839CC = high-byte alias of unk_839C9 / unk_BA6DA) start at 0
    //   and advance by the sub_10A30 rule: next word is -1 or >= 8 -> 0, else +1. When a table is
    //   empty (short file) the original index is drawn unmodified.
    std::vector<int16_t> map_base_anim_;        // unk_7EBAC: base_cnt x 10 words
    std::vector<int16_t> map_top_anim_;         // unk_7D824: top_cnt x 10 words
    std::vector<uint8_t> map_base_anim_frame_;  // unk_839CC
    std::vector<uint8_t> map_top_anim_frame_;   // unk_BA6DA
    void battle_tile_anim_advance();            // sub_10A30
    int battle_base_tile_frame(int file_id) const; // sub_109EE (file_id 1-based) -> 0-based tile index
    int battle_top_tile_frame(int file_id) const;  // sub_10A0F
    int auto_tile_div_ = 0;                     // divides the 15-tick auto_advance calls into 30-tick periods
    // ---- Player move range display (sub_16834 diffusion + sub_44D67(1) + sub_1547F shading) ----
    //   At the start of the player turn the port fills the E8F46-equivalent diffusion grid
    //   (remaining budget, -1 unreachable, -(army+1) hostile blocker) and shades the 24x16 front of
    //   every reached tile (E8>=0) with LUT row 1 (sub_6104A(1): tint=1, 30%). An empty diffusion
    //   (mover gate unmet) falls back to the old radius-3 display.
    std::vector<int> battle_player_diffusion_;  // dword_E8F46 equivalent (current player turn)
    bool battle_player_diffusion_valid_ = false;// word_FA8BC equivalent
    uint8_t move_shade_lut_[256] = {};          // LUT row 1
    bool move_shade_valid_ = false;
    const uint8_t* battle_move_shade_lut();     // row 1 (from dlg_shade_pal_, tint=1, bl=0x1E)
    void battle_player_begin_overlay(int unit_index); // sub_19FAB shared word, then sub_14032+143F3
    void render_player_move_range();            // sub_1547F (after base tiles, before units)
    // unk_88BD4: third map layer = tile terrain attributes. 0..100 = normal, 101+ = buried terrain
    // (sub_12F2A tests it with `cmp al,64h; jbe`). Filled by load_battle_map.
    std::vector<uint8_t> tile_attr_;
    bool sce_header_parsed_ = false;            // guards a MAP-only run whose header could not be read
    // ---- Scenario condition (sub_49411 / byte_FDBC0) ----
    //   NUL-terminated Johab string of 0xA8 bytes at SCE offset 0xE1; an empty first byte means
    //   nothing is drawn. After loading (4FD25/4FCD9) and before the battle loop (5580F), main_
    //   draws "[ Scenario Condition ]" (0x30,0x22) and the body (0x40,0x37) via sub_49411, then
    //   waits 0xBB8 ticks.
    std::string scenario_condition_;            // byte_FDBC0 equivalent (up to the NUL, incl. 0x0A)
    bool show_scenario_condition();             // sub_49411 equivalent, true = shown (returned 1)

    // PACK blitters
    void blit_tile_24x16(int x, int y, const uint8_t* src_pixels);
    // NC2 strip blitter: outputs only the top scan_limit scanlines (sub_5F4AF, foot-burial
    // clipping)
    void blit_nc2_frame_clipped(int x, int y, const NC2Frame& fr, int scan_limit);
    void blit_sprite_rle(int x, int y, const uint8_t* rle_data);
    // sub_5EE51 equivalent: flat w/h + pixels (0xFF transparent) blit, used only for TIME_GRA.SYB
    // (not RLE). data[0..1]=w, data[2..3]=h, data[4..]=w*h flat, clipped to the screen.
    void blit_flat_transparent(int x, int y, const uint8_t* flat_data);
    // sub_5EFB9: flat w/h + pixels, 0xFF transparent, clipped at 13Fh/C7h (columns 319 and
    // rows 199 are never written); a negative x/y becomes a source offset.
    void blit_flat_clip319(int x, int y, const uint8_t* flat_data);
    // RLE blitter: outputs only the top scan_limit scanlines (sub_5F4AF, foot-burial clipping)
    void blit_sprite_rle_clipped(int x, int y, const uint8_t* rle_data, int scan_limit);
    // sub_5F3FD equivalent: draws RLE with the 480 px scratch-canvas stride (WEA_EFFC/BATTLE
    // projectiles and trails). Unlike the 320-stride blit_sprite_rle it divides by 480 to find the
    // row (the 11BB8+10031 precondition).
    void blit_sprite_480(int x, int y, const uint8_t* rle_data);
    bool load_terrain_overlays(const std::string& map_filename); // sub_10420 (MAP_EFFC.SYB -> unk_789AC)
    void load_effect_sprites();                                  // sub_11BB8 + sub_44AB6 effect frames
    void play_weapon_class5_projectile(const CombatUnitPlacement& attacker,
                                       const CombatUnitPlacement& target, bool reaches_target); // sub_46E27
    // ---- loc_1E8E5 per-class hit effects (BATTLE.SYB 480-stride frames) ----
    //   bank 0 = unk_FA070 (first 0x4C), bank 1 = unk_FA400 (last 0x39, battle_effects_[0x4C+i]).
    void fx_blit_battle_spr(int bank, int idx, int x, int y);        // sub_5F3FD
    void battle_fx_47372(int unit_index);                            // wc 0Bh
    void battle_fx_47712(int unit_index);                            // class 0A1h
    void battle_fx_4798a(int unit_index, int mode);                  // 7Fh/80h(1), wc 0Eh(0)
    void battle_fx_47a96(int unit_index);                            // wc 0Ch
    void battle_fx_47e79(int unit_index);                            // wc 10h
    void battle_fx_47f3c(int unit_index);                            // wc 13h
    void battle_fx_47fdc(int unit_index);                            // class 5Ch
    int  combat_rand(int n);                                     // sub_499BD
    void sfx(int id);                                            // sub_4F4CD (port: log only)
    void unit_screen_center(const CombatUnitPlacement& u, int& cx, int& cy); // sub_135EB
    int  weapon_class(const CombatUnitPlacement& u) const;       // sub_174CF
    int  battle_lookup_184a4(int type, int column) const;        // sub_184A4
    int  battle_damage_class(const CombatUnitPlacement& u) const; // sub_17531
    int  battle_damage_adjustment(const CombatUnitPlacement& u, int damage_class) const; // sub_1762C
    int  battle_damage(int attacker_index, int target_index, int hit_kind, int area_mode, int def_bonus); // sub_1D770 (x100)
    BattleAttackRoll battle_attack_roll(int attacker_index, int target_index, int shared_hit_kind = -1); // sub_1DDF0 (dx != 1 part) + sub_1D6B1
    int  battle_hit_kind(CombatUnitPlacement& attacker);          // sub_1D6B1
    int  battle_unit_index_at(int x, int y) const;                // sub_11DB2 guarded CC998 footprint lookup
    // sub_11DB2/sub_11E04 cell lookup over the modeled CC998 occupancy. apply_guard=false is for
    // direct grid scans (sub_530F1/sub_53C12) that do not apply the (x-2)/(y-4) filter.
    int  battle_cc998_occupant(int x, int y, bool apply_guard) const;
    // Compatibility name for callers that need the guarded sub_11DB2 lookup.
    int  battle_unit_index_at_footprint(int x, int y) const;
    bool battle_wc1_static_occupancy_unambiguous() const;          // complete static SCE, ordinary armies, unique D66A8 footprints
    bool battle_wc1_group0_move_preserves_occupancy(const VmEvent& ev, const char** reject_reason = nullptr) const;
    void battle_wc1_occupancy_unknown(const char* reason, uint32_t offset = 0xFFFFFFFFu); // fail-closed latch; reopened only by next complete SCE snapshot
    // Keeps the latch when the gate still passes after a verified 1x1 add/remove/move; closes it as
    // unknown otherwise.
    void battle_wc1_occupancy_preserve_or_close(const char* reason, uint32_t offset = 0xFFFFFFFFu);
    bool battle_resolve_attack(int attacker_index, int target_index, bool allow_ranged_ai = false, bool declare = true, BattleAttackRoll* out_roll = nullptr); // player: adjacent; wc5 AI: ranged path
    bool battle_warp_unit(int unit_index, int destination_x, int destination_y); // sub_3BF00: Warp! + 12-frame position move
    // sub_48612(unit, text, bx=0x11): shows a label above a unit for 36 frames (3 game ticks each).
    // The position is fixed once on entry (sub_10971(x)+0Ch, sub_10988(y)-10h) and sub_484F9
    // centres
    // the string. The background is the frozen screen of the entry moment (the original never
    // redraws frames). Text uses the TEXT.CHR small font (sub_608E6), lower case mapped to upper.
    // draw_shield=true reproduces sub_3C4C1 (Defense!): sub_45559 -> sub_1E460 -> sub_5E9E8, so the
    // shield icon is part of the label background snapshot.
    void battle_unit_label(int unit_index, const char* text, bool draw_shield = false);
    // sub_1E460(unit): when [5Ah]!=0 and sub_184A4([5Ah],0)==6, draws ITEM.SPR[[5Ah]-1] at
    // (sub_10971+10h+[65h]+xo, sub_10988+4+[66h]+18h+yo) through sub_5F3FD (480 stride).
    bool load_item_spr();
    void battle_draw_shield_icon(int unit_index);
    // sub_28889: spell-name announce, called by the handlers (F13B0 -> sub_28982/29273/29514/
    // 29722/29F4D/2A75A ...) once the target is fixed. The name is off_7423C[R], stored in
    // dword_F13AC by sub_28883 (battle_spell_name_).
    //   sub_1124A(caster); if sprite<0x12C set [64h]=7, sub_1033B(20h), twice {sub_45559; [64h]++;
    //   sub_499E4}; sub_1033B(4); for 36 frames {sub_454B3; sub_484F9(x+0Ch, y-14h, name, frame,
    //   0x11); sub_5E9E8; sub_499E4}.
    void battle_spell_announce(int unit_index, const char* name);
    const char* battle_spell_name_ = "";
    // sub_484F9(x, y, text, ecx=frame 1..36, [stack]=bx): draws one label frame. var_4 survives
    // between frames in a stack slot, so the caller owns it (label_var4 in battle_unit_label).
    void battle_label_draw_frame(int x, int y, const char* text, int frame, int color_arg, int16_t& label_var4);
    bool battle_player_attack(int target_index);                  // minimal single-target path of sub_1F738
    bool battle_ai_step(int unit_index);                           // minimal chase/attack path of sub_44913
    int battle_ai_43ca9(int combat_class) const; // sub_43CA9 (wc2 split: 0 -> 43526, 1 -> 438AE, 2 -> end)
    void battle_ai_43c5d(int unit_index); // sub_43C5D (AI auto-guard: TP>150, cap+14, guard-item gate)
    // sub_3C4C1 (Defense!): when 184A4([5Ah],0)==6 and ([1Eh]&6)==0, set facing to 0, play the
    // label effect, restore the facing and set [14h]|=4. true = original eax==1.
    bool battle_defense_3c4c1(int unit_index);
    // sub_43CF0 (class A1/5C/5D/5B specials): south-facing best column with 3x3 (A1), front + 3x6
    // (5C), diamond r7 (5D), sprites 184 Sea Blizzard / 185..187 Sea Mist (5B). Fills the F0648
    // equivalent battle_target_list_ and runs battle_resolve_list (1DDF0 dx=1). Unported effects
    // log.
    bool battle_ai_class_special_43cf0(int unit_index);
    bool battle_ai_class_a4_4426d(int unit_index);  // sub_4426D: A4 clone summon/spell
    int battle_ai_40bfa_pick_target(int unit_index, int selected); // sub_40BFA/4088C/409AD target range
    // sub_41515 (low-HP retreat guard): true = eax 1 (skip the normal AI), false = continue.
    // Covers the low/high bands, the 3EF12 retreat walk loop, 4D3BA recovery and the [57]/[7F]/[81]
    // reset.
    bool battle_ai_41515_retreat(int unit_index);
    void battle_prepare_ai_gauge_baseline(CombatUnitPlacement& u); // sub_19FAB: baseline of the shared dword_F0426+2
    // sub_3E18B: -1 = watch target not found or distance <= 1, 0 = +71h (FFh) equals the home x,
    // 1 = target distance > 1, 2 = signed x/y differ from both home coordinates. AI selectors 0..6
    // resolve through the CC960/CC968..CC990 lists (effective armies 0..5 and the global
    // roster); 7..254 are outside the list array and return the port-only -2 diagnostic.
    int battle_ai_watch_state(int unit_index) const;
    // sub_1379A(sel,id): walk dword_CC960[sel*8] and return the first active unit with
    // [+0Fh]==id. Selectors 0..5 select effective-army lists 0..5, selector 6 the CC990 global
    // list. Only on-map units_ records are represented; list order follows list_seq.
    int battle_ai_cc960_member(int selector, int id, int owner = -1,
                               bool include_inactive = false) const;
    // sub_3F321 (dasm 63212..63306): computes the whole AI action state 0/1/2/-1. Signed map kind
    // >= 4 -> 0, [+81h] -> 1, [+7Fh] -> 2, [+82h]!=FFh && HP>0x3E8 -> 1, [+80h]!=FFh -> 2,
    // [+6D]==4 -> -1, [+6D]==0 -> 0; any other [+6D] passes the type/link gate and splits into
    // 0/1/2 through the [+29h]/[+2Ah] update and the unk_74144 lookup (table + [+29h]*20 + [+2Ah]).
    // The writes and the rand(12) call order match the original, but combat_rand's LCG differs from
    // DOS rand, so states on the +6D=1/2 path can differ from a DOS run. The scan gate only opens
    // on
    // the explicit [+6D]==0 path, so that difference does not affect it; it is used only to log WC1
    // unit state outside the gate.
    int battle_ai_3f321_state(CombatUnitPlacement& u);
    // sub_1665B: combat_class -> mobility selector (unk_70A68, falling back to [+14h]&1).
    // -1 means unmodelled (out of range or outside the grid).
    int battle_ai_mobility_selector(const CombatUnitPlacement& u) const;
    // Footprint-aware sub_16699 check (name kept for compatibility): every D66A8 cell must be
    // in bounds, free in CC998 (excluding self), and have a positive unk_70B0D terrain cost.
    // The flying-bit gate and the full original descriptor remain outside this helper.
    bool battle_ai_candidate_tile_legal_1x1(int x, int y, int unit_index, int mobility_selector) const;
    // Diagnostic for the home-referenced path of sub_3E251 only (radius-9 diamond, sub_1AC08/1AC59/
    // 1AD2E plus sub_49ADF scoring). With no [+73h] target and no [+71h] watch it picks a legal
    // candidate around home and synthesizes no movement. On true, out_* holds the best candidate
    // (first wins ties, score = dist + 18 - 2*ad2e).
    bool battle_ai_no_target_home_candidate(int unit_index, int& out_x, int& out_y,
                                            int& out_score, int& out_ad2e, int& out_dist,
                                            const char** reject_reason) const;
    // AI walk animation, played like the original AI callers (sub_40295 loc_407BF etc.):
    // sub_1124A (centre the camera on the unit) -> sub_1620A (scroll-following walk) ->
    // sub_12124 (reset the pose). full_path[0] is the current tile, the last is the destination.
    // Returns false when should_exit_ is set.
    bool battle_ai_play_walk(int unit_index, const std::vector<std::pair<int,int>>& full_path,
                             int attack_facing = -1);
    // sub_1A2D7/sub_1A2FE terrain bucket gate: a candidate tile is allowed when it lies in
    // [bucket, bucket+0x10) of the base tile's bucket. The original sub_1093D reads the raw unk_88BD4
    // byte; 1A2D7 subtracts 0x64 once when >= 0x64 and clears the low nibble to get the bucket
    // (F0BA4); 1A2FE subtracts once from the candidate too and tests
    // (candidate - base - bucket) < 0x10 unsigned. The 1A2D7 base at the sub_3CA3C/sub_3D7B2 entry
    // is
    // a hostile coordinate (not the actor), and only the WC>=3 branch of sub_1D124 uses the actor,
    // so
    // this is not generalized. tile_attr_ mirrors unk_88BD4; out of range fails closed (false).
    bool battle_ai_terrain_bucket_compatible(int base_x, int base_y, int cand_x, int cand_y) const;
    // Port of the sub_14032 + sub_143F3 decision-phase diffusion (E8F46 remaining-budget map).
    // Returns a whole-map int grid: -1 unreachable (0xFFFF), -(army+1) for a hostile blocker,
    // 0 or more = remaining budget. The blocker set uses the original fixed table on the actor's
    // effective army (1/2 -> {3,4}, 3 -> {1,2,4}, 4 -> {1,2,3}, 9 -> {8}, nothing otherwise).
    // The four neighbours of a blocker get E65B0 = 0xF9, which stops propagation (the value is
    // still written) but stays a legal destination. The actor cell folds in the sub_13D02
    // refresh (+1), so it is budget+1. Call only inside the 1x1 ordinary gate.
    // Neighbour propagation is limited to x in [2,w-3], y in [2,h-3] (143F3 south y+1<h-2,
    // north y-1>=2, ...), matching the 2-cell border of -1 that 13FB5 writes at the end of 14032.
    // The queue (E8CC0 LIFO, cap 0xA0) reproduces sub_13AB1: on overflow drop everything and
    // reset count to 0 (values match only when no overflow happens).
    // Actor army 9 uses the selector-5 grid (blocker 8). Path reconstruction and 165C2 are not
    // included. When out_fringe is given it receives unk_E65B0 right after the sub_143F3
    // propagation (0xFF default from sub_14032, 0xF9 next to blockers); that is before the actor
    // cell refresh (+1), so it matches the post-diffusion fringe read by sub_3E739 phase B and
    // sub_3EF12.
    std::vector<int> battle_ai_build_diffusion(int unit_index, int budget,
                                               std::vector<char>* out_fringe = nullptr) const;
    // sub_1419C(actor): clears E8F46 and E65B0 to -1, then walks the CC998 occupants in index
    // order and writes E8 = -(army+1) on the cells of the actor's blocker triple (1/2 -> {3,4,4},
    // 3 -> {1,2,4}, 4 -> {1,2,3}, 9 -> {8,8,8}). For each of their four neighbours (N,W,E,S) whose
    // E8 is still -1 it subtracts 0x1E from E65B0 (stacks when blockers overlap, byte wrap).
    void battle_ai_1419c_tail(int unit_index, std::vector<int>& grid,
                              std::vector<int8_t>& fringe) const;
    // sub_1499C(actor, dx=dest_x, bx=dest_y): 147BB presets the actor cell to marker (EDD72)
    // and spreads through four LIFO buckets (cap 0xA0). Fixed neighbour order S,W,N,E; rank key
    // 0 = toward the destination, 1 = away and axis order broken, 2 = away with axis order kept;
    // buckets pop from the lowest number. New budget = budget - terrain cost + E65B0[current+W]
    // (0 when -1), updated on strict maximum; stop expanding at budget <= -1 and push only when
    // the new budget is > 0. Success (1) when E8[dest] >= 0 right after a pop, 0 when all buckets
    // drain. grid/fringe start from the 1419C state and are updated in place.
    int battle_ai_1499c_wave(int unit_index, int dest_x, int dest_y,
                             std::vector<int>& grid, const std::vector<int8_t>& fringe,
                             int marker) const;
    // sub_15342(actor, &dest_x, &dest_y): fallback destination when 1499C fails. Over a diamond
    // of radius (manhattan>>1)+7 centred on (dx+((ax-dx)>>1), dy+((ay-dy)>>1)) it takes the strict
    // minimum of E8 + 4*|cell-mid| among cells that are in the map, have E8 >= 0 and pass 165C2
    // si=0/1 (ties go to the first).
    bool battle_ai_15342_fallback(int unit_index, int dest_x, int dest_y,
                                  const std::vector<int>& grid,
                                  int& out_x, int& out_y) const;
    // sub_15523(actor): greedy ascent from dest back toward the actor, always taking the strict
    // maximum E8 neighbour. Neighbour order follows 49ABC per quadrant (NE/E [W,S,E,N],
    // SE [W,N,E,S], SW [E,N,W,S], NW/W [E,S,W,N], N [S,E,N,W], S [N,E,S,W]), first win.
    // When E8[neighbour] == marker (the actor cell) it returns 1 with route (dest-first, actor
    // cell excluded) and EDD76 (reverse of the first step through 49ABC: 0=S, 1=W, 2=N, 3=E).
    // dest == actor gives route={actor}, EDD76=[+63h] and returns 2. Returns 0 when the dest
    // occupant is not the actor, when the ascent fails, or when it exceeds 0xA0.
    int battle_ai_15523_route(int unit_index, int dest_x, int dest_y,
                              const std::vector<int>& grid, int marker,
                              std::vector<std::pair<int,int>>& route,
                              int& out_edd76) const;
    // sub_14FA4 body (dasm 7388..7695). Copies the route 15523 built (dest-first, E8CC0) into
    // E5E30, keeps only cells with E8 >= 0 in bucket 0 and tests 165C2 (si=0/1) from the farthest
    // seed. On failure it picks a 1AD2E strict maximum that passes 165C2 inside a radius-3 diamond
    // (1AC08) around the seed, reruns 15523, and when nothing qualifies it scans the remaining
    // seeds again. Finally it reverses bucket 0 back into the route, dropping the tail beyond the
    // budget. The return value is the facing handed to 1620A (0..3, -1 on failure) and matches
    // EDD76. 11EE4/11E04 (occupancy release/re-register) are omitted because the port models
    // occupancy as a vector.
    int battle_ai_14fa4_route(int unit_index, int ref_x, int ref_y, int budget,
                              const std::vector<int>& budget_grid,
                              std::vector<std::pair<int,int>>& route,
                              int* out_cost = nullptr, bool trace = true) const;
    // Port of the sub_40295 loc_406DB tail (14300/1419C/1499C/15342/15523/14FA4).
    // use_wave=true checks destination reachability through the EDD72=0xD2 wave (falling back
    // to 15342), builds a 15523 route on the D2 diffusion and trims it with 14FA4. false builds
    // only a 15523 route on the current turn budget (var_18*4); the di==1 selection path never
    // calls 14FA4. out_facing takes the caller's af as input and is updated from the 14FA4
    // return. dest == actor gives out_walk={actor} and true (the caller handles the zero-step
    // turn).
    bool battle_ai_route_walk_path(int unit_index, int mobility, int budget,
                                   int& dest_x, int& dest_y, bool use_wave,
                                   int ref_x, int ref_y, int& out_facing,
                                   std::vector<std::pair<int,int>>& out_walk,
                                   int* out_cost = nullptr, bool trace = true) const;
    // Port of the WC1 sub_3CA3C best-score search. For every hostile tile inside the budget
    // diamond (y-x order) it scans the four adjacent cells in facing order 0,1,2,3 (S,W,N,E).
    // The inner loop takes, per hostile, the strict maximum (first wins ties) among cells that
    // pass the hostile base bucket gate, E8F46 maximum and the 165C2 gate (unoccupied or self
    // with diffusion >= 0; terrain cost is not tested), yielding the best (f,v). The outer loop
    // picks the hostile with the strict maximum of score = v + ((actor facing == (f+2)%4) ? 4 : 0).
    // The epilogue's (f+2)%4 attack facing and the +4 facing-match bonus are included. The E8F46
    // value of an OOB tile is -128 (sub_13CBD). dest == actor returns true with a zero-step turn
    // that updates only the facing.
    bool battle_ai_3ca3c_wc1_best(int unit_index, const std::vector<int>& diffusion,
                                   int budget_radius, int& out_hx, int& out_hy,
                                   int& out_dx, int& out_dy, int& out_score,
                                   int& out_attack_facing) const;
    // Port of the WC5 sub_3CA3C branch (loc_3CC91). Reproduces the wc5 hostile selection of
    // sub_3E65E/3EADD/3ED51: sub_1AEF3 (5/7) -> sub_1CB53 diffusion snapshot -> sub_1B7D4 state ->
    // sub_1C685 terrain-bucket flood and score -> sub_1CBB1 restore -> per-facing strict maximum
    // (first wins ties). The 1AF22/1B00D triangle iterator and the 1AC08/1AC59 diamond are kept as
    // in the original. The outer loop picks the hostile with the strict maximum of
    // score = si + (90 - dist) + (facing match ? 4 : 0). The E8F46 scratch is snapshotted and
    // restored per facing/hostile from the caller's diffusion copy, and the 1C685 flood residue
    // is left behind exactly as DOS leaves it. 165C2 walks the actor's full D66A8 footprint
    // (base row upward); every cell must be unoccupied (or self) with diffusion >= 0.
    // F0B8E==2 (E8 score) and ==3 (sub_3D7B2 distance) are supported. Machine-code emulation matched
    // the E8F46 end state. On the 1-5 sample the DOS
    // observation (A -> id3, C -> Hammil) differs for A (Mark instead of id3) and C (keeps
    // Hammil instead of id4), so the port keeps the verified original target choice and records
    // this as a deviation.
    struct Wc5Iter;  // sub_1AF22/sub_1B00D triangle iterator state (F0B92..F0B90)
    struct Wc5Score;  // F0B74/F0B82/F0B84/F0B8E/F0B8A plus the F0A46 snapshot state
    struct Wc5Dia;    // sub_1AC08/sub_1AC59 diamond iterator state
    void battle_ai_wc5_iter_init(Wc5Iter& it, int ax, int dx, int cx, int bx,
                                 int stack) const;
    // Writes the next tile to out and returns true; false when the iterator is exhausted.
    bool battle_ai_wc5_iter_next(Wc5Iter& it, int& out_x, int& out_y) const;
    void battle_ai_wc5_dia_init(Wc5Dia& it, int ax, int dx, int bx) const;
    bool battle_ai_wc5_dia_next(Wc5Dia& it, int& out_x, int& out_y) const;
    int battle_ai_wc5_aef3(int combat_class) const;  // sub_1AEF3: 5/7
    // sub_13CBD/sub_13D02 (OOB reads -128; gate x in [2,W-3], y in [4,H-5]).
    int battle_ai_wc5_diff_read(const std::vector<int>& grid, int x, int y) const;
    void battle_ai_wc5_diff_write(std::vector<int>& grid, int x, int y, int v) const;
    // sub_1AD2E: horizontal edge distance within the row of the diamond iterator's next cell.
    // It runs after 1AC59 advanced the state, so fold = min(counter, width-1-counter), i.e.
    // width-counter-1 when counter > width/2. A negative width (exhausted) can yield -2.
    int battle_ai_wc5_1ad2e(const Wc5Dia& it) const;
    // sub_169D7: resets E8F46 to -1 on cells that are unoccupied and have terrain cost > 0 for
    // the actor's move selector, and to -2 everywhere else (the reset that precedes 3F0E4/3F247).
    void battle_ai_169d7_reset(int unit_index, std::vector<int>& grid) const;
    // sub_3F0E4(actor, edx=radius, ebx=band): after the 169D7 reset it walks the 1AC08/1AC59
    // diamond (r=radius) and writes the band value on every cell with E8 == -1. The value is the
    // low 16 bits of (EDD72 & 0xFFFF) + 1 - ebx (accumulated), where ebx is updated from the 1AD2E
    // fold: EDD72+2 when fold >= radius-band, else radius-fold.
    void battle_ai_3f0e4(int unit_index, std::vector<int>& grid, int radius,
                         int band, int edd72) const;
    // sub_3F247(actor, edx=&dest_x, ebx=&dest_y, ecx=radius): after the 3F0E4 flood it takes the
    // strict minimum of the manhattan distance to home ([+6Fh]/[+70h]) or to the 3E21B target
    // among cells with E8 >= 0 inside the same radius-r diamond. true means dest is meaningful.
    bool battle_ai_3f247(int unit_index, int radius, int edd72,
                         int& out_x, int& out_y) const;
    // sub_3E21B: units_ index of the [+71h] list member (1379A) or of the direct [+73h]
    // target. -1 when there is none; all list selectors 0..6 are resolved.
    int battle_ai_3e21b_target(int unit_index) const;
    // Grid-taking form of sub_3EBB4 (early path): for every hostile inside the sec-radius
    // diamond it scores wc<6 with 3D3E5 and wc>=6 with 3CA3C. grid is the E8 left by 3F0E4 and
    // edd72 is the 3CA3C/3D3E5 entry (+1) / exit refresh value. out_hostile is a units_ index.
    bool battle_ai_3ebb4_grid(int unit_index, int sec,
                              const std::vector<int>& grid, int edd72,
                              int& out_dx, int& out_dy, int& out_facing,
                              int& out_hostile, int* out_metric = nullptr) const;
    // sub_3F184(actor, edx=&dest_x, ebx=&dest_y, ecx=&af, stack=sec): temporarily overwrites the
    // 3E21B target's unk_72982[class*8+dmg] with 0x64 (or [+46h] with 0xF830 when dmg < 0), runs
    // 3EBB4, then restores it. Returns the unit index when 3EBB4 picked the same target as 3E21B,
    // else -1 (dest/af keep whatever 3EBB4 wrote).
    int battle_ai_3f184(int unit_index, int sec, const std::vector<int>& grid,
                        int edd72, int& out_dx, int& out_dy, int& out_af);
    // Hold for the temporary 72982 overwrite in sub_3F184, so the global const table is untouched.
    mutable int battle_ai_damage_adjust_index_ = -1;
    mutable int battle_ai_damage_adjust_value_ = 0;
    // sub_1BA9C/sub_1C5CE/sub_1C62B/sub_1C046/sub_1BB62/sub_1BD1E/sub_1BF2E/
    // sub_1C1B2/sub_1C31A. mode comes from st.b8a (F0B8A).
    void battle_ai_wc5_mark_cell(std::vector<int>& grid, Wc5Score& st,
                                 int x, int y) const;
    void battle_ai_wc5_flood_neg(std::vector<int>& grid, int x, int y,
                                 int add) const;
    void battle_ai_wc5_m_c046(std::vector<int>& grid, Wc5Score& st, int eax0,
                               int edx0, int f, int ecx0) const;
    void battle_ai_wc5_m_bb62(std::vector<int>& grid, Wc5Score& st, int eax0,
                               int edx0, int f, int ecx0, int arg0) const;
    void battle_ai_wc5_m_bd1e(std::vector<int>& grid, Wc5Score& st, int eax0,
                               int edx0, int f, int ecx0, int arg0) const;
    void battle_ai_wc5_m_bf2e(std::vector<int>& grid, Wc5Score& st, int eax0,
                               int edx0, int f, int ecx0, int arg0) const;
    void battle_ai_wc5_m_c1b2(std::vector<int>& grid, Wc5Score& st, int eax0,
                               int edx0, int f, int ecx0) const;
    void battle_ai_wc5_m_c31a(std::vector<int>& grid, int eax0, int edx0,
                               int f, int ecx0) const;
    // 165C2 gate: the whole D66A8 footprint is unoccupied (or self) and every cell has
    // diffusion >= 0 (includes the 11DB2 gate). battle_d66a8_footprint supplies the footprint.
    bool battle_ai_wc5_165c2_gate(const std::vector<int>& grid, int nx, int ny,
                                  int unit_index, int facing) const;
    // sub_1CB53/sub_1CBB1/sub_1B7D4.
    void battle_ai_wc5_snapshot(const std::vector<int>& grid, Wc5Iter& it,
                                Wc5Score& st, int ax, int dx, int cx, int bx,
                                int stack) const;
    void battle_ai_wc5_restore(std::vector<int>& grid, Wc5Iter& it,
                               Wc5Score& st, int ax, int dx, int cx, int bx,
                               int stack) const;
    void battle_ai_wc5_state_init(Wc5Score& st, int score_init, int actor) const;
    // sub_1C685 body. Leaves the best per-facing (dest, score) in st.b84x/b84s/b82.
    void battle_ai_wc5_1c685(std::vector<int>& grid, Wc5Iter& it, Wc5Score& st,
                             int adj_x, int adj_y, int f, int rng,
                             int unit_index) const;
    // sub_3CA3C WC5 branch: facing loop plus epilogue for one hostile. Returns the score and the
    // dest/attack facing (score = -1 when nothing was found).
    int battle_ai_wc5_hostile_score(std::vector<int>& grid, Wc5Iter& it,
                                    Wc5Score& st, int unit_index, int hx,
                                    int hy, int& out_dx, int& out_dy,
                                    int& out_attack_facing) const;
    // sub_3E65E: scores every hostile inside the radius diamond with sub_3CA3C and takes the
    // strict maximum (first wins ties). The per-wc arms split inside 3CA3C. budget is the var_18*4
    // diffusion budget and radius is the Q+var_8 diamond radius.
    bool battle_ai_3e65e_select(int unit_index, int budget, int radius,
                                int& out_hostile, int& out_dx,
                                int& out_dy, int& out_attack_facing,
                                int& out_score) const;
    // Shared walk tail of the WC1 approach: builds a sub_15523 route (including the
    // 1419C/1499C/15342 pipeline when use_wave is set), lets sub_14FA4 fix the destination and
    // facing through E8/165C2/buckets/rediamond, and walks with sub_1620A. use_wave=false (the
    // di==1 selection path) uses the 15523 route alone. After moving it fires the trigger once
    // and keeps the latch. attack_facing >= 0 uses that facing like sub_1620A does (a facing
    // returned by 14FA4 takes priority), and dest == actor only updates the facing through a
    // zero-step turn. true when it moved or turned.
    bool battle_ai_wc1_walk_to(int unit_index, int actor_id, int actor_x, int actor_y,
                               int mobility, int dest_x, int dest_y, int ref_hx, int ref_hy,
                               int attack_facing = -1, bool use_wave = false);
    // Tests whether a tile is occupied by a hostile, using the sub_11DB2 approximation and the
    // effective-army hostility rule. sub_3E65E/sub_3DF3B do not test the [+14h] flying gate, so
    // neither does this. The self tile is excluded because a unit cannot be its own target.
    bool battle_ai_hostile_occupied(int x, int y, int unit_index) const;
    // Shortest legal 4-direction path (BFS; every cell must pass sub_16699 1x1/cost legality and
    // avoid other units; adaptive neighbour order). out_steps holds the cells up to the
    // destination, excluding the start. false when no path exists.
    bool battle_ai_find_legal_path(int unit_index, int mobility, int dest_x, int dest_y,
                                   std::vector<std::pair<int,int>>& out_steps) const;
    // Approximation of the sub_40295 -> sub_14FA4 move budget: one turn walks only the prefix
    // whose accumulated terrain cost stays within var_18*4 (var_18 = trunc((shared_gauge-10)/10)).
    // That product is exactly what sub_40295 passes to sub_14FA4 in ecx; word_EDD72=0xD2 is a
    // separate route-diffusion cap. Tile costs come from the mobility grid. full_path[0] is the
    // current tile. Reports the walked cell count and cost in out, false when not even one cell
    // can be walked.
    bool battle_ai_truncate_to_move_budget(const std::vector<std::pair<int,int>>& full_path,
                                           int mobility, int* out_cost,
                                           std::vector<std::pair<int,int>>& out_walk) const;
    // watch_state <= 0 handles branch 0 of the [+6Ch] switch (the sub_3E65E/sub_3DF3B approach).
    // The budget-radius test (trunc((shared_gauge-10)/10)) runs first regardless of the state
    // sign (the common [+6Ch] prologue). With a hostile inside, it lays down the decision-phase
    // diffusion (sub_14032/sub_143F3, budget var_18*4) and picks the destination with the WC1
    // sub_3CA3C inner loop (facing order, bucket, E8F46 maximum, 165C2), then walks. With no
    // hostile it follows the second sub_3E18B branch (loc_40664): state == 0 approaches hostiles
    // within radius 30 using the sub_3DF3B outer loop and the WC1 sub_3D7B2 inner loop, and
    // state == -1 ends the turn. The path is rebuilt through the whole
    // sub_1419C/1499C/15342/15523/14FA4 pipeline. The walk, trigger and latch afterwards match
    // the home path.
    void battle_ai_wc1_pursuit(int unit_index, int watch_state);
    // Radius-30 approach of the second sub_3E18B state == 0 branch (sub_3DF3B outer loop, WC1
    // sub_3D7B2 inner loop). It scans radius 30 directly with no budget-radius test (watch_state
    // must be <= 0).
    void battle_ai_wc1_radius30_pursuit(int unit_index);
    // Units_ index of the unit occupying (x,y) when it is hostile to the actor, else -1.
    // sub_11DB2 + sub_1201D approximation (same guard and rule as battle_ai_hostile_occupied),
    // but this also tests the self tile: 1201D applies to the unit itself, so an E518D
    // inversion makes it true (sub_3E3BF includes the centre cell, so sub_3EF12 hits this quirk).
    int battle_ai_hostile_unit_at(int x, int y, int unit_index) const;
    // sub_165C2 for a specific facing: walk the D66A8 w x h footprint (rows go up from the
    // base tile), require E8F46 >= 0 on every cell and no sub_11DB2 occupant other than self.
    // The sub_11DB2 guard leaves cells outside x in [2,w-3], y in [4,h-5] unoccupied, while
    // sub_13CBD returns -128 outside the map; both cases are reproduced here.
    bool battle_ai_165c2_ok(int x, int y, int unit_index, const std::vector<int>& e8,
                            int facing) const;
    // sub_40295 [+6Ch]==1 branch (sub_3E739, mode=2, sec=var_18). Rebuilds the 143F3 end state and
    // picks destination/facing through phases A (r=mode diamond of -1 per hostile), B (E65B0
    // ordering), C (E8 += 0x320 plus fringe) and D (actor r=sec strict maximum + 165C2 si=0/1),
    // then runs the nearest-hostile (sub_49B01) and the sub_14F52 facing / 165C2 re-gate fallback.
    // Returns 1 on success (out_hostile = units_ index), 0 on failure (original eax == 0).
    // hostiles is the F0648-equivalent list. pre_block holds the targets that sub_3E457 marked
    // with E8F46 = -1 in the hostile r=2 diamond beforehand (used only by 438AE).
    int battle_ai_tact_3e739(int unit_index, int mode, int sec, int diff_budget,
                             const std::vector<int>& hostiles, int& out_dx, int& out_dy,
                             int& out_facing, int& out_hostile,
                             const std::vector<int>* pre_block = nullptr) const;
    // sub_40295 [+6Ch]==5 branch (sub_3EF12, push 3): sub_3E3BF rebuilds the hostile list inside a
    // max(radius,3) diamond (replacing F0648), then phases 1 (r=4 diamond of -1 per hostile),
    // 2 (actor r=radius strict maximum starting at 0xFFFF + 165C2) and 3 (14F52 facing from
    // F0648[0] plus the 165C2 re-gate fallback) choose the retreat destination. 1 = success,
    // -1 = failure.
    int battle_ai_flee_3ef12(int unit_index, int radius, int diff_budget,
                             int& out_dx, int& out_dy, int& out_facing) const;
    // Branch-only walk tail of sub_40295 loc_405CA..loc_4084E. Walks the 15523 route with 1620A
    // (BFS approximation in the port), fires the trigger once, then reads E8F46 at the arrival
    // cell from the clean diffusion laid down before the walk (settle_budget = var_18*4) to get
    // S = budget - E8F46[arrival]; sub_3C993 clears [57h]/[7Fh]/[81h] and the shared gauge loses
    // (S>>2)*10 + var_4. 5401B/17310 are reproduced by battle_after_move/battle_terrain_17310 and
    // sub_167C9 is dropped as a loop-local value (the di==1 single-action approximation).
    bool battle_ai_wc1_branch_walk(int unit_index, int dest_x, int dest_y, int facing,
                                   int settle_budget, int var_4,
                                   int ref_hx, int ref_hy, bool use_wave = false);
    // Runs jump-table entries k=1 (sub_3E739) and k=5 (sub_3EF12) of sub_40295 loc_40470,
    // including the turn cost. false when [+6Ch] is neither 1 nor 5. Requires watch_state <= 0
    // (the caller routes a first sub_3E18B result > 0 to the home path). When the
    // gauge > var_4+0x18 gate closes or the branch fails, di becomes -1 and the second
    // sub_3E18B runs: k=1..4 use the 3DF3B radius-30 scan only when watch_state == 0, k=5/6 end.
    bool battle_ai_dispatch_plus6c(int unit_index, int watch_state);

    // ---- Mage AI (wc==2, combatii_aimage.cpp; dasm 68230..69404 + 61343..62060 + 63307) ----
    // sub_3E21B: watched unit as a units_ index (-1 when none). [+71h] != 0xFF resolves through
    // sub_1379A(sel,id) for selectors 0..6, otherwise [+73h] is used.
    int battle_ai_watch_unit(int unit_index) const;
    // sub_42B77(u, mode, radius): scans the caster's facing triangle (1AF22/1B00D, start cell
    // excluded) for mode 0 = allied unit with an ailment, 1 = allied unit below 70% HP or an
    // enemy with [0Dh] == 2, 2 = unit with [1Eh] & 7. 1 = found, -1 = none.
    int battle_ai_mage_42b77(int unit_index, int mode, int radius) const;
    // sub_42D51: 1 when the same army list holds no [12h]==[0Fh]+10 unit, or exactly one and its
    // HP is below half; otherwise 0.
    int battle_ai_mage_42d51(int unit_index) const;
    // sub_42C97(army): wounded (<70% HP) ratio of the army list. The original test
    // (100*total/wounded < 30) is always false, so this always returns -1.
    int battle_ai_mage_42c97(int army) const;
    // sub_42DC1(u, mode 0..3): picks spell R = group*21 + d*3 + kind. -1 = no spell available,
    // -2 = not enough MP/TP.
    int battle_ai_mage_42dc1(int unit_index, int mode);
    // sub_432A0: spell selection, TP wait ([7Fh] reservation), casting and MP/TP settlement.
    // 1 = cast, 2 = reserved (waiting for TP), 0/-1 = nothing.
    int battle_ai_mage_432a0(int unit_index);
    // F13B0[q1] AI auto-target path (dword_74492 == 1). All 49 handlers live in battle_cast_magic,
    // so only handlers that need a target tile go through the sub_41715 approximation; the rest
    // cast without a tile. Returns -1 on failure (no cost paid), anything else on success.
    int battle_ai_cast_spell(int unit_index, int r_code);
    // Approximation of the sub_41715/419F1/4189C target selection. Sets tx,ty on success.
    bool battle_ai_spell_pick_target(int unit_index, int hq1, int kind, int& tx, int& ty);
    // sub_1B7BF + sub_1B7FA: AI spell line-of-sight collection and marking. Walks the actor's
    //   forward 1AF22 cone, cuts the sight at terrain brightness (sub_1093D) and casts shadows into
    //   the E8F46 grid with 13D02 on occluded cells.
    //   mode 0 marks visible cells with 1 in E8F46 (centre candidates); mode 1 collects, in F0648
    //   order, the units whose 11DB2 occupant and 1201D verdict equal expect (rng.list). Cells that
    //   are already shadowed (-2) are skipped.
    void battle_ai_los_1b7fa(g2range::Range& rng, int caster_index, int expect, int mode, int los_range);
    // Mode 1 of the function above: sets the mode/expect values and returns F0648 in out.
    // The duplicate exclusion of sub_28596 is not part of 1B7FA itself.
    void battle_ai_collect_1b7fa(int caster_index, bool want_hostile, int los_range,
                                 std::vector<int>& out);
    // sub_3F415(u, edx): AI turn-end settlement (defense flag [13h], TP spend, rest recovery).
    void battle_ai_turn_end_3f415(int unit_index, int edx);
    // sub_3E3BF: hostile-occupied units inside a max(r,3) diamond, in y-x order.
    std::vector<int> battle_ai_3e3bf(int unit_index, int radius) const;
    // sub_3E457: collects approach targets for a healing mage. -1 = the caster itself is wounded or
    // afflicted, 0 = no target, n = list size. out_pre_hostiles receives the targets pre-marked
    // with E8F46 = -1 in the hostile r=2 diamond (the initial F0648 list).
    int battle_ai_3e457(int unit_index, int radius, std::vector<int>& out_wounded,
                        std::vector<int>& out_pre_hostiles) const;
    // sub_3E5AB: approach destination toward the watched target (3CA3C/3D3E5). 1 = success.
    bool battle_ai_3e5ab(int unit_index, int budget, int& out_dx, int& out_dy,
                         int& out_facing) const;
    // sub_3E251: target/watch through sub_3D7B2 first, otherwise the radius-9 candidate
    // with the smallest dist + 18 - 2*ad2e; the reference cell is the fallback.
    bool battle_ai_3e251_dest(int unit_index, int& out_x, int& out_y, int& out_rx, int& out_ry,
                              const std::vector<int>* e8 = nullptr) const;
    // sub_3DF3B approximation: among hostile-occupied cells within radius r, the one with the
    // lowest approach score (false when none).
    bool battle_ai_3df3b_dest(int unit_index, int radius, int& out_x, int& out_y,
                              int& out_hx, int& out_hy,
                              const std::vector<int>* e8 = nullptr) const;
    // sub_3D7B2: minimum sub_49ADF distance from the actor to a destination next to the
    // hostile at (hx,hy). Returns 0x7FFF when no candidate passes the occupancy/16699 gate.
    // e8 is the caller's F0B8E/F0B84 diffusion; the wc5 arm needs it for the 1CB53/1C685
    // distance-mode chain.
    int battle_ai_3d7b2_dest(int unit_index, int hx, int hy, int& out_x, int& out_y,
                             const std::vector<int>* e8 = nullptr) const;
    // sub_3D7B2 loc_3D9D7 wc5 distance mode (F0B8E==3) over the supplied E8 diffusion.
    int battle_ai_wc5_3d7b2(int unit_index, int hx, int hy,
                            const std::vector<int>& e8, int& out_x, int& out_y) const;
    // sub_1A7BA F0B8E==3 (sub_3D7B2 wc 0xB/0xC): two-pass sub_1A452 band geometry, scored
    // by the strict minimum sub_49ADF distance and gated by sub_16699. chain is F0B84_hi
    // and is shared across facings (7FFFh on facing 0).
    int battle_ai_1a7ba_distance(int unit_index, int x0, int y0, int d, int K,
                                 int& chain, int& bx, int& by) const;
    // sub_3E01B: among empty cells within radius r, the one with the best allied-density (+10 per
    // cell) and distance score, plus the most common facing among allies.
    bool battle_ai_3e01b_dest(int unit_index, int radius, int& out_x, int& out_y,
                              int& out_facing) const;
    // Body of sub_43526 (type 0) / sub_438AE (type 1). true = the turn is consumed.
    bool battle_ai_mage_turn(int unit_index, int type);
    // Equivalent of sub_28422: dword_74492 == 1 means AI auto-targeting is on.
    bool battle_spell_auto_ = false;
    // Mage AI move-budget override: sub_14FA4 ecx = S*4 with S = (TP-T)/10 and T = [167C9].
    // -1 keeps the usual var_18*4.
    int battle_ai_budget_override_ = -1;
    // sub_40295 var_4 (starts at 0x0A, becomes 10+30n after a walk via sub_167C9); it offsets the
    // budget var_18 = (TP - var_4)/10.
    int battle_ai_var4_ = 10;
    // sub_167C9: n = hostiles on the own cell plus its four neighbours -> 10 + 30n.
    int battle_ai_167c9(int unit_index) const;
    // sub_40295 move step (the -1 path after the 4016A attack scan): one WC1 detailed or generic
    // approximate pursuit.
    bool battle_ai_40295_move(int unit_index, int wc);
    // Literal translation of the sub_40295 body loop (combatii_ai_approach.cpp). true = handled.
    bool battle_ai_40295_body(int unit_index, int wc);
    // sub_40295 loc_407BF walk arm. 0 = no path, 1 = done, 2 = exit through loc_40882.
    int battle_ai_40295_walk(int unit_index, int dest_x, int dest_y, int facing, int budget,
                             int ref_hx, int ref_hy, bool use_wave = false);

    // ---- sub_4016A family (combatii_aiturn.cpp; dasm 64562..64668, 64420..64561, 63357..63400,
    // ----
    //      58307..58399, 59065..59116) ----
    // sub_3C993(u, amount): clears [57h], [7Fh] and [81h], then spends TP through
    // sub_1A0C6(amount).
    void battle_ai_3c993(int unit_index, int amount);
    // sub_4016A(u): dispatch on sub_3F321 state 0/1/2/-1, the 1D124 threat scan, then a
    // sub_3F4A1/sub_1F738 attack. Returns 1/0 = acted (edi holds the state), -1 = nothing to do
    // (fall through to movement), -2 = turn over (leave sub_40295 without TP settlement).
    // allow_scan=false blocks the 1D124 scan, protecting WC1 runs whose occupancy is unproven.
    int battle_ai_4016a(int unit_index, bool allow_scan);
    // sub_3BC17 Berserk: leaves the ready queue (19718) and sets [83h]=2, [6Dh]=0, [6Eh]=4,
    // [14h]|=2, [82h]=FF, [6Ch]=0, [80h]=FF.
    void battle_berserk_3bc17(int unit_index);
    // sub_3F4A1: when rand(3)==0, a class special attack (unk_731B1 -> sub_3B9E1 whirl, or
    // unk_731B8 -> sub_3C30A curse). 1 = fired.
    int battle_ai_3f4a1(int unit_index);
    int battle_ai_3b9e1(int unit_index);   // whirl: every hostile in radius 1, 28728(6000,20,1). 1 = success, -1 = no target.
    int battle_ai_3c30a(int unit_index);   // curse: one hostile in front, [1Eh]|=20h and [25h]=3Ch. 1 = success, 0 = failure.
    // sub_3FFD4: force-use the weapon named by [81h]/[82h] (2CA40 steps + F1474 handler + 301BB
    // self-damage + TP settlement). 1 = used, 2 = waiting for TP (reserved), 0/-1 = cannot use
    // (falls through to the scan).
    int battle_ai_3ffd4(int unit_index);
    // sub_3F9AD(u, si, bx): pick a weapon candidate (0..0x18 weapon index, -1 = none). bx=0xFF
    // means auto-select.
    int battle_ai_3f9ad(int unit_index, int si, int bx);
    // 3F9AD helpers: 3F516/3F5E8/3F6C1/3F797/3F7F3/3F83C/3F8A7/3F8F6 (1 = condition met).
    int battle_ai_3f516(int unit_index, int dx);
    int battle_ai_3f5e8(int unit_index);
    int battle_ai_3f6c1(int unit_index);
    int battle_ai_3f797(int unit_index);
    int battle_ai_3f7f3(int unit_index);
    int battle_ai_3f83c(int unit_index);
    int battle_ai_3f8a7(int unit_index);
    int battle_ai_3f8f6(int unit_index);

    // ---- sub_40295 jump table k=2/3/4/6 ----
    // Evaluates the wc<=3 (loc_3CB73) / wc==4 (loc_3CC3A) branch of sub_3CA3C for one input
    // cell (the hostile position). The entry/exit actor refresh (budget+1) is already baked in by
    // battle_ai_build_diffusion. Returns the score (-1 when none); out receives dest and (f+2)%4.
    int battle_ai_3ca3c_cell(int unit_index, int wc, int hx, int hy,
                             std::vector<int>& e8, int budget,
                             int& out_dx, int& out_dy, int& out_facing) const;
    // Wc<=3 (loc_3D549) / wc==4 (loc_3D613) branch of sub_3D3E5. Returns the score (0x7FFF when
    // none). var1c = sub_1762C(occupant, sub_17531(actor)) or 0x0A; occ_* are occupant fields.
    int battle_ai_3d3e5_cell(int unit_index, int wc, int hx, int hy,
                             std::vector<int>& e8, int budget, int var1c,
                             int occ_def, int occ_atk, int occ_facing,
                             int& out_dx, int& out_dy, int& out_facing) const;
    // K=2 (sub_3EADD, state==2) / k=4 (sub_3ED51, state==4): scores every hostile inside the
    // budget-radius diamond with 3CA3C, then k=2 picks the strict minimum HP ([+48h]) and k=4 the
    // strict maximum level ([+43h]).
    bool battle_ai_jump_approach(int unit_index, int state, int sec, int diff_budget,
                                 int& out_dx, int& out_dy, int& out_facing,
                                 int& out_hostile) const;
    // K=3 (sub_3EBB4): wc<6 takes the strict minimum 3D3E5 score (ties broken by 49ADF distance
    // <= the current best distance), wc>=6 requires a 3CA3C score > 0 and the strict minimum
    // [+46h] defense. All wc 0..0x13 arms are ported.
    bool battle_ai_jump_3ebb4(int unit_index, int sec, int diff_budget,
                              int& out_dx, int& out_dy, int& out_facing,
                              int& out_hostile, int* out_metric = nullptr) const;
    // K=6 (sub_3EE2B): for each dir 0..3, when 165C2 plus 1D124(dir) yields exactly one
    // unit, F0648[0] becomes a candidate and the strict minimum HP wins. dest is the actor
    // position (rotation only), facing is the winning direction (own facing when none) and the
    // return is the candidate unit (-1 when none). A negative return means the branch failed
    // (no move). All wc arms of 1D124 are ported, including the no-op wc 0x0A/0x14+ cases.
    bool battle_ai_jump_3ee2b(int unit_index, int diff_budget,
                              int& out_dx, int& out_dy, int& out_facing,
                              int& out_hostile);

    void setup_knockback_hit(CombatUnitPlacement& t, int facing);   // sub_45ED2
    void setup_knockback_guard(CombatUnitPlacement& t, int facing); // sub_4609A
    void setup_knockback_dodge(CombatUnitPlacement& t, int facing); // sub_461C6
    void apply_knockback_step(CombatUnitPlacement& t);               // sub_46108: add and decay
    bool apply_knockback_hit(CombatUnitPlacement& t, int tidx, int frame); // sub_45F29
    void reset_unit_pose(CombatUnitPlacement& u);                    // sub_12124
    void play_attack_sequence(const VmEvent& ev);                    // sub_1E61D + sub_1EF5C
    void battle_crit_19168(int attacker_index, const std::vector<int>& targets); // sub_19168 critical-item effect
    void battle_crit_collect_28596(int unit_index);                 // sub_28596 (extra F0648 gate)
    void battle_crit_collect_19061(int attacker_index, int radius);  // sub_19061 diamond collection
    void battle_crit_collect_190c3(int attacker_index);              // sub_190C3 front 3x10 collection
    void play_retreat(const VmEvent& ev);                            // late sub_51449 + sub_17071
    void play_retreat_pillar_508e6(const VmEvent& ev, int edd74);    // sub_508E6(4) light-pillar retreat
    void battle_force_retreat_50aa7(const VmEvent& ev);              // sub_50AA7 removal modes 2/3/4
    void camera_center_on(int tx, int ty);                           // sub_1124A (camera alignment)
    void play_unit_pose(const VmEvent& ev);                          // sub_51C74 loc_51E34
    void play_hp_change(const VmEvent& ev);                          // sub_5211D
    // ---- Death, wipe-out and HP triggers (sub_1DB1D / sub_53797 / sub_53C12 / sub_53E0F /
    // sub_53F94) ----
    static constexpr int kTriggerRecordMax = 128;
    uint8_t hp_trigger_fired_[2][kTriggerRecordMax] = {};    // unk_110514 (mode 0) / unk_110320 (mode 1)
    uint8_t region_trigger_fired_[kTriggerRecordMax] = {};   // unk_110254
    // unk_110384 (table 5, attack declaration) / unk_1103E8 (table 6, contact after a
    // move): sub_50086 clears 100 bytes for each table and sub_5394D uses
    // `unk_110384[record]` / `unk_1103E8[record]` as the one-shot flag for that record.
    uint8_t contact_trigger_fired_[2][100] = {};
    // word_F14D8: 0 = player (ring) path, 1 = AI (sub_3FFD4+10D) or script (loc_51F86) auto-target.
    // It decides whether a weapon special opens the 4A483 manual cursor or collects targets
    // automatically (sub_2CA39(1)).
    int battle_f14d8_ = 0;
    // Manual targeting for weapon specials. With F14D8 == 0 (player), the submenu commit
    // opens the 4A483 cursor and the unit on the clicked tile becomes the target.
    // battle_weapon_pending_si_ >= 0 means a click is pending.
    int  battle_weapon_pending_si_ = -1;
    int  battle_weapon_pending_kind_ = 0;   // pending 4A483 cursor kind (0 = none)
    int  battle_pick_tx_ = -1;      // tile chosen by the player (-1 = none)
    int  battle_pick_ty_ = -1;
    bool battle_weapon_needs_cursor(int idx) const;              // F1474 handler that opens the 4A483 cursor
    // Cursor kind for the si==0 (302E6 body) path.
    //   0 = no cursor (automatic), 1 = F1474 target unit, 2 = 308EE landing cell (empty, distance >= 2),
    //   3 = occupied cell within 7 cells in front (356DB), 4 = centre cell of the radius-2 diamond (39A49).
    int  battle_weapon_cursor_kind(int unit_index, int si) const;
    bool battle_weapon_pick_valid(int unit_index, int kind);   // validates battle_pick_tx_/ty_
    int  battle_weapon_pick_target(int unit_index, bool hostile_only) const;
    bool battle_weapon_apply_pick(int unit_index);               // moves the picked unit to the head of F0648
    // ---- CC990 roster list (sub_500ED/sub_50105/sub_50D9F loc_50EFF) ----
    //   A reinforcement (0x03) with raw army 0 looks its roster record up by ID and overwrites
    //   all 0x88 bytes (sub_50105 restores only the coordinates and facing, then
    //   sets [48h]=[4Ch] and [50h]=[52h]); after processing, the updated record is re-inserted at
    //   the **front** of the list (sub_112FD + sub_1134F). A linked unit ([5Ch]!=0) writes the
    //   same data into the [77h] copy and sub_17E6D turns the live unit into its transformed form.
    std::vector<CombatUnitPlacement> battle_roster_cc990_;
    CombatUnitPlacement* battle_roster_find_500ed(int id);
    void battle_roster_apply_50105(CombatUnitPlacement& spawn, const CombatUnitPlacement& roster_rec);
    void battle_roster_push_cc990(const CombatUnitPlacement& rec);
    // sub_4E81F: reloads a raw-army-0 spawn that is missing from the roster from
    //   BLACKBOX.DAT 0x4E26+(ID-1)*0x88, applies the [+87h]/word_FDAF6 army remap and copies it to
    //   CC990. Also covers the [0Eh] +1 of sub_50D9F loc_50EDD.
    void battle_reload_record_4e81f(CombatUnitPlacement& u);
    uint8_t morale_trigger_fired_ = 0;                        // byte_1104B0 (only [0] is used)
    uint32_t script_table_base(int table) const;              // sub_53512(0, table) = script_offsets_[table-1]
    void dump_script_tables() const;                          // G2_BATTLE_TRACE verification dump
    bool run_unit_hp_trigger(int victim_index, int other_index, int mode); // sub_53797
    bool run_contact_trigger(int unit_index, int target_index, int mode);  // sub_5394D (tables 5/6)
    bool run_region_count_trigger();                          // sub_53C12
    bool run_morale_trigger();                                // sub_53E0F
    bool run_army_wiped_trigger(int group);                   // sub_53F94
    void battle_after_move(int unit_index);                   // sub_5401B (contact triggers excluded)
    int battle_terrain_17310(int unit_index);                 // sub_17310 (terrain damage/heal: 0 = death, -2 equivalent, 1 = survived)
    void battle_remove_from_ready_queue(int unit_index);      // sub_197AD
    int  battle_unit_group(const CombatUnitPlacement& u) const;     // modeled effective [+0x0E]-1; unknown remaps fall back to raw+1
    int  battle_group_alive_count(int group) const;                 // active-vector count fallback; CC968 membership is not stored
    void battle_apply_death_morale(const CombatUnitPlacement& dead); // sub_1DB1D loc_1DC0D
    void play_unit_death(int unit_index, int killer_index = -1);     // full sub_1DB1D sequence
    void play_unit_death_anim(int unit_index);                        // sub_463FB / sub_46A11 / sub_46B2C
    void play_unit_death_46b2c(int unit_index);                       // sub_46B2C (large classes 0x86..0x91)
    void put_pixel(int x, int y, uint8_t col);

    // Cutscene state
    size_t cutscene_cursor_ = 0;
    std::vector<uint8_t> sce_raw_;
    std::vector<CombatDialogueEntry> cutscene_dialogues_;
    std::vector<uint8_t> fpm_decoded_; // full 480x400 (192,000B) canvas decoded via sub_10500, if applicable
    bool should_exit_ = false;
    int return_code_ = 0;
    std::string pending_command_;

    // ---- SCE script VM (sub_4ED18/sub_5306A/sub_500C9/sub_530F1/sub_52E55) ----
    // sub_5306A reads 12 dword "slot" offsets (unk_10081C) and then copies the
    // rest of the .SCE into the bytecode buffer unk_10084C.
    static constexpr int kScriptSlots = 12;
    std::vector<uint8_t> script_buffer_;          // unk_10084C (body-relative 0 = first byte after the slot table)
    uint32_t script_offsets_[kScriptSlots] = {};  // unk_10081C
    bool script_loaded_ = false;

    // Legacy headers/method-name aliases kept for readability of the port.
    static constexpr int kScriptSlots_ = kScriptSlots;

    // VM-visible game state (globals in the original)
    uint32_t battle_time_ = 0;   // dword_F0418 (sub_19D47, incremented each battle update)
    // dword_F0418 value shown by the bottom time gauge (sub_44EB0).
    //   The original redraws it on every battle tick (inside sub_19D53, right after the inc) and
    //   on every HUD update (454D5). Its loop has no gating, so ticks run at CPU speed (5E9E8 is a
    //   plain rep movsd with no vsync wait) and the 0/1 alternation of
    //   n = ((F0418 % byte_74138) + 1) >> 1 is invisible on screen: a 1-7 DOS capture showed it
    //   only at intro dialogue boundaries and constant for 48 s during a player turn.
    //   Port battle ticks default to G2_BATTLE_TICK half (30 game ticks = 171 ms = 5.85 Hz), which
    //   would turn the same formula into a 5.85 Hz blink (rendering runs at 11.7 Hz, so two frames
    //   per value). The displayed value is therefore synced to battle_time_ only at the points
    //   where the original definitely redraws the HUD (player turn entry, cutscene/message frames)
    //   and keeps the previous value during free ATB/AI progress, reproducing the gauge that looks
    //   frozen in the original.
    uint32_t battle_hud_time_ = 0;
    // dword_74134 (argument of sub_5358F). sub_19652 initialises it to -1 (sequence triggers off)
    // and VM 0x0E (sub_5283D) sets it to 0. Once enabled, triggers run and the value increments
    // every tick.
    int32_t  script_seq_ = -1;
    // ---- Battle tick state initialised by sub_19652 ----
    uint8_t  battle_state_ = 0;          // byte_F042E: 0 = running, 1 = waiting for player input, 2 = pause requested, 3 = paused
    bool battle_dim_row0_ = false;       // dword_FA8B6 equivalent: sub_454D5 dim over (0x10,4)-(0x130,0xC4), row 0; true only while the scenario condition is shown.
    // The original sub_10AEB never clears the backbuffer, it only covers the 13x13 tiles. When
    // true, render_battle_map skips its clear so the border strip the map does not cover (16 px on
    // the sides, 4 px top and bottom) keeps the previous frame's pixels. The equipment screen
    // (sub_48BC1) and detail screen (sub_48863) never call 454D5, so that strip is the previous
    // battle HUD.
    bool battle_keep_backbuffer_ = false;
    bool battle_ring_dim_active() const; // dim while the ring/sub-ring is up (FA8B6 equivalent, in sub_454D5 order); used by render_battle_frame.
    uint16_t battle_overlay_mode_ = 0;   // word ptr dword_75008 (sub_44DEC): 1 makes sub_454D5 draw the state icons
    // ---- All-unit HP, TP gauge and clock (sub_13546/sub_45BBF family) ----
    //   word_FA8BA (sub_44DC5): set to 1 in the 4B767/4B8EB/4D674 loops and to 0 in 4B842/4B9D7.
    //   That matches ring visibility, so it is derived from ring_open_ || ring_sub_open_.
    bool battle_show_all_hp() const { return ring_open_ || ring_sub_open_; } // word_FA8BA equivalent (sub_13546 gate)
    void draw_unit_hp_number(const CombatUnitPlacement& u); // sub_13498: HP number only, no box
    void render_all_unit_hp();           // sub_13546: HP of every visible unit (FA8BA gate)
    // dword_75008+2 / word_7500C / word_7500E (sub_455BD slot, gates in sub_45686/45844).
    // Default slot (0xFA,0x26) = (250,38) with display on (COMBATII.ASM 75008 = 0FA0001h).
    // sub_455BD moves between three slots: 0 = (0x4B,0x26), 1 = (0xFA,0x26), 2 = (0xFA,0x9B).
    int battle_gauge_x_ = 0xFA;          // dword_75008 high word (75008+2) equivalent
    int battle_gauge_y_ = 0x26;          // word_7500C equivalent
    // sub_455BD: picks another slot when the mouse covers the panel rectangle (0x4C x 0x3E).
    //   true = hit (slot relocated). Callers in the original: the 4A483 destination cursor loop
    //   and the 4D674 entry/ring-display/target-cursor loops (78827/83210/83284/83407).
    bool battle_hud_panel_relocate(int mx, int my);
    bool battle_gauge_visible_ = true;   // word_7500E equivalent (0 only during the sub_45A41/45ACF animations, 1 afterwards)
    void ellipse_solid_span(int x, int y, int w, uint8_t color); // sub_5EC44 equivalent
    void render_tp_gauge();              // sub_45686: TP gauge disc (F042F != 1)
    void render_battle_clock();          // sub_45844: clock dial, hands, date and time (F042F == 1)
    bool load_time_gra();                // TIME_GRA.SYB (sub_44AB6 entry 0x0D; flat 5EE51: #0 tail, #4 dial, #5..12 hands)
    SPRParser time_gra_;                 // TIME_GRA.SYB, 13 frames (flat w/h + pixels, 0xFF transparent, not RLE)
    bool time_gra_tried_ = false;
    bool time_gra_loaded_ = false;
    uint16_t battle_scan_group_ = 0;     // word_F0424: current group walk position (0..3)
    int      battle_scan_unit_ = -1;     // dword_F0410: unit being visited (units_ index, -1 = null)
    uint64_t battle_scan_next_key_ = ~0ull; // dword_F0410 resume key: next list node to visit ((list_seq<<32)|index), ~0 = null
    uint32_t battle_list_seq_counter_ = 0;  // sub_1134F tail-insert sequence allocator
    uint32_t battle_next_list_seq() { return ++battle_list_seq_counter_; }
    uint16_t battle_ai_acted_ = 0;       // word_F0420: did the AI act this tick
    uint16_t battle_scan_active_ = 0;    // word_F0422: group walk in progress (resume skips the tick increment)
    std::vector<int> battle_ready_queue_;// dword_F041C linked list (+F0414 tail): player units waiting for a turn. dword_F0426 = size()
    int16_t battle_ai_gauge_baseline_ = 0; // word ptr dword_F0426+2: shared threshold set around sub_44913/sub_19FAB
    int battle_player_unit_ = -1;
    int battle_player_move_range_ = 3;  // P3 fallback move radius (used when diffusion is unavailable)
    bool battle_attack_select_ = false; // sub_1F738 target phase in progress (battle_atk_phase_ != 0)
    // ---- Move command state (sub_4D674 loc_4D98E -> sub_16834) ----
    //   The original enters sub_16834 as a blocking loop when the map is clicked in the ring
    //   (sub_4BA7F -2): (1) sub_4A483 destination click, (2) sub_15523 path trace test,
    //   (3) sub_16357 facing click, (4) sub_165C2 -> sub_1620A walk.
    //   The port runs a frame loop, so the same order is split into battle_move_phase_ steps.
    //   1 = (1) destination selection, 2 = (3) facing selection.
    int battle_move_phase_ = 0;
    int battle_move_dest_x_ = 0, battle_move_dest_y_ = 0;  // tile returned by sub_4A483
    int battle_move_e8dest_ = -1;      // E8F46[destination]; sub_16834 returns it in edx for TP settlement
    int battle_move_budget_ = 0;       // word_EDD72 (budget passed to sub_14300)
    std::vector<g2move::PathNode> battle_move_path_;   // sub_13AB1 queue (E8CC0), in push order (destination -> start)
    void battle_move_walk(int unit_index, int facing); // sub_1620A: walk animation
    // sub_1620A body taking the node stack directly; shared by battle_move_walk and
    // battle_ai_play_walk.
    void battle_walk_nodes(int unit_index, std::vector<g2move::PathNode> stack, int facing);
    int battle_move_edd76_ = 0;        // word_EDD76: last move direction chosen by sub_15523
    int battle_face_choice_ = 0;       // sub_16357 var_4 (currently selected facing)
    int battle_face_blink_ = 0;        // sub_16357 var_8 (toggles every 0x1E ticks)
    int battle_face_mx_ = 0, battle_face_my_ = 0;  // last event position, clamped into the confinement box
    bool battle_face_init_ = false;
    std::vector<TerrainOverlayFrame> move_cursor_;   // MOVE.CUR: facing*2 + blink frame (24x24 RLE, hotspot 8,8)
    bool move_cursor_tried_ = false;
    void battle_move_begin(int unit_index);          // map click in the ring -> enter sub_16834
    bool battle_move_input(int unit_index);          // per-phase input (true = Esc consumed)
    void battle_move_cancel();                       // drop the phase and restore the cursor
    void battle_move_commit(int unit_index, int facing); // after 165C2: sub_1620A walk + TP settlement
    void battle_ring_return(int unit_index);         // return to the ring (turn ends when TP < 10)
    void load_move_cursor();                         // sub_13909: MOVE.CUR
    void render_move_overlay();                      // hover marker / facing arrow cursor
    // ---- Attack command state (sub_1F738): a blocking loop in the original, split into phases
    // ----
    //   1 = sub_1F19C(edx=1) single target (weapons 5/0Fh/13h)   2 = sub_1F19C(edx=0) + confirm (11h/12h)
    //   3 = sub_1F466 straight 2x2 (0Bh/0Ch)                    4 = sub_4A815 confirm wait (11h/12h)
    int battle_atk_phase_ = 0;
    int battle_atk_wc_ = 0;                       // sub_174CF, sampled when the attack starts
    std::vector<int> battle_target_list_;         // dword_F0648 / dword_F0B9C+2 (units_ indices)
    std::vector<int16_t> battle_range_e8_;        // E8F46+2 range grid (sub_1CC09)
    int battle_atk_tx_ = 0, battle_atk_ty_ = 0;   // dword_FA8B0 / word_FA8B4 (selected tile)
    int battle_line_cx_ = 0, battle_line_cy_ = 0; // sub_1F466 var_10/var_C (2x2 cursor origin)
    bool battle_line_valid_ = false;              // sub_1F466 var_4 (a valid hover has been seen)
    // ---- Player turn state (sub_4D674 locals) ----
    //   The var_18 budget is recomputed from TP on every diffusion call, so it is not stored.
    int16_t battle_turn_start_tp_ = 0;       // dword_F042A (snapshot taken by sub_19FAB, compared by sub_1A0AD)
    uint8_t battle_saved_gauge_ = 0;         // var_8 ([56h] saved on entry, restored on exit)
    uint16_t battle_used_bits_ = 0;          // var_28 (used-menu bits, ORed into [+13h] on exit)
    bool battle_var24_action_end_ = false;   // var_24 (action ended through the -4/-2 path, short exit)
    // sub_4D674 var_18 (move-budget subtrahend, starts at 0x0A and is refreshed by 167C9).
    // budget = ((TP-var18)/10)*4; TP < var18 takes the ring/exit branch (4DA7B). 10 on turn entry.
    int battle_move_var18_ = 10;
    // word_FDAF2 (short ring, 83160..83167): 1 when ([56h]!=FF) or ([56h]==FF and 19FA3()==3).
    // When set the ring offers only {0Dh,0Bh}.
    bool battle_simple_ring_ = false;        // word_FDAF2 equivalent
    // sub_5580F free wait: F042E == 1 or the ring is closed. true means waiting for a unit click:
    // left click on an ally -> 4D674, on a non-ally -> 493A3, right click -> 1979F + 4D674.
    bool battle_free_wait_ = false;
    int battle_free_hover_ = -1;             // hovered unit in the free wait (49FE1 equivalent, -1 = none)
    // word_FDAEA (instant-move option from game.cfg / 4E0EE): 0 = smooth (11003), non-zero =
    // instant (111BE).
    bool battle_instant_camera_ = false;     // word_FDAEA equivalent (default 0)
    uint8_t battle_flag_420f_ = 0;           // unk_F042F (toggled by sub_1A0F0; read by 1A110 -> 45BBF: 1 = clock, else TP gauge)
    std::vector<int> battle_menu_ids_;       // current ring options (sub_4BD26 order)
    // ---- Ring interface (sub_4B0A7/4B767/4B8EB/4B9D7/4BA7F/4B0FB/4AC8A) ----
    //   Final layout: an ellipse of radius 36 px (ry = 36*0.83) starting at 90 degrees (top,
    //   y = center - y_off) and stepping 2*pi/N counter-clockwise (4B0FB subtracts y, so increasing
    //   angles turn counter-clockwise on screen).
    //   Icon art maps a logical id in the ICO descriptor table to file frame id*4+frame
    //   (sub_4AF61: count/4 descriptors; sub_4A96E: di/4 = idx, di%4 = frame; sub_4AA42:
    //   [ring+pos+6] = id -> table+id*37 -> [desc+4] = frame -> pixels at [desc+frame*4+15h]).
    //   MAINRING has 56 files = 14 logical icons, MAGIC 88 = 22, SPECIAL 104 = 26, SKILL 72 = 18,
    //   AI_CTRL 56 = 14. Menu ids 0..13 map straight onto logical 0..13.
    //   Hit test is centre +/-12 px (sub_4AC8A). Only the hovered ([ecx+5]) descriptor cycles
    //   frames 0..3 (sub_4AD6F every 30 ticks = 171 ms; sub_4AC8A resets both sides to frame 0).
    //   Open (4B767) 276 -> 36 at -16, 15 frames (300 deg + 15*10); show (4B8EB) 0 -> 36 at +4,
    //   9 frames (2*pi + 9*10 deg); close (4B842) 36 -> 276 at +16, 15 frames; hide (4B9D7)
    //   36 -> 0 at -4, 9 frames. Every frame: sfx 0x1F + game_timer(2) + 4AEB4 (advance radius and
    //   angle) + 4B0FB + 45431 + 5E9E8 + 499E4.
    //   dbl_77E40/77E48/77E50/77E58 = 2*pi/0.83/2*pi/-2*pi (measured from COMBATII.LST).
    //   Text hotkeys (A/M/I and digits) are kept as verification shortcuts; the original only has
    //   the mouse ring.
    SPRParser ring_main_, ring_magic_, ring_special_, ring_skill_, ring_aictrl_;
    bool ring_icons_tried_ = false;
    bool ring_icons_loaded_ = false;
    struct RingIconPos { int id = -1; int x = 0; int y = 0; };
    std::vector<RingIconPos> ring_layout_;  // screen coordinates of the current ring icons (sub_4B0FB equivalent)
    int ring_center_x_ = 0, ring_center_y_ = 0;
    int ring_hover_pos_ = -1;  // layout index, -1 = none (sub_4AC8A [ecx+5] equivalent)
    int ring_hover_frame_ = 0; // [desc+4] equivalent: only the hovered descriptor cycles 0..3, the rest stay 0
    double ring_hover_last_us_ = 0.0; // gate for the 30-tick (171 ms) sub_4AD6F timer
    bool ring_open_ = false;   // dword_FDAEE equivalent (0 = closed, 1 = open)
    bool ring_opened_this_turn_ = false; // whether 4B767 (276 -> 36) already ran this turn; 4D674 uses 4B767 on the first show only, later loops use 4B8EB
    bool ring_load_icons();                       // sub_4B0A7 equivalent
    void ring_compute_layout(int unit_index);     // final layout of sub_4B0FB
    void ring_compute_layout_at(int unit_index, double radius, double start_angle); // intermediate layout of 4AEE5
    int ring_hit_test(int mx, int my) const;      // sub_4AC8A equivalent; layout index or -1
    void ring_open_anim(int unit_index);          // 4B767 (first open 276 -> 36) / 4B8EB (show 0 -> 36)
    void ring_close_anim();                       // 4B842 (close 36 -> 276) / 4B9D7 (hide 36 -> 0)
    void ring_close_anim_full();                  // sub_4B842 equivalent (turn-end 36 -> 276 fly-out over 15 frames)
    void ring_animate_radius(int unit_index, double r0, double a0, double dr, double da, int target); // 4AEB4 + 4B0FB loop
    void ring_hover_advance();                     // sub_4AD6F equivalent (hover frame +1 every 30 ticks)
    void ring_set_hover(int pos);                  // sub_4AC8A equivalent (both sides reset to frame 0 on a change)
    void render_ring();                           // ring icons and hover (sub_4B0FB/4AB72/4AC28)
    bool ring_handle_click(int unit_index, int mx, int my, int& out_menu_id); // 4BA7F click equivalent
    // ---- Sub-ring graphics (sub_4C72A/4CDBD/4CAA3) and the two-level 4D06E ring ----
    //   Same final layout as the original: 36 px radius ellipse (ry = 36*0.83), start angle 90
    //   degrees (top), 2*pi/N counter-clockwise, hit test centre +/-12 px (sub_4AC8A). Icons use the
    //   same logical-to-file rule as the main ring (logical*4+frame; MAGIC uses the
    //   sub_4BFCA ids 1..7 / {0,8..14} / {0,15..21}; SKILL dx; SPECIAL entry/si; AI_CTRL id).
    //   Open (4B8EB) 0 -> 36 over 9 frames and hide (4B9D7) 36 -> 0 over 9 frames use the
    //   same sfx, timing and +10 deg/frame rotation as the main ring.
    //   sub_4C72A is three-level (sub_4BFCA ids -> unk_74FA8 table; R = (v-1)+kind);
    //   sub_4CDBD/sub_4CAA3 are one-level
    //   (dx/si); sub_4D06E is two-level (level-1 ids -> level-2 values, with
    //   [6Ch]/[6Dh]/[6Eh]/[80h]/[82h] and 4CE1F).
    //   The older text paging and hotkeys (M/I/V/digits) remain as a verification fallback.
    enum class SubRingKind {
        None = 0,
        MagicG = 1,   // sub_4C72A level 1: sub_4BFCA ids 1..7 (group = id-1)
        MagicD = 2,   // sub_4C72A level 2: sub_4BFCA ids {0,8..14} (b = 0->7, id-8)
        MagicK = 3,   // sub_4C72A level 3: sub_4BFCA ids {0,15..21} (c = 0->7, id-15)
        Item = 4,     // sub_4CDBD: dx (4C19D list)
        Weapon = 5,   // sub_4CAA3: si (4C03E position)
        AI1 = 6,      // sub_4D06E level 1: 4C4F9 ids
        AI2 = 7,      // sub_4D06E level 2: 2..3 values per level-1 id
        AITarget = 8, // sub_4CE1F: map target selection ([6Fh]/[70h]/[71h]/[73h])
        AIMagic = 9,  // sub_4D06E loc_4D337: 4C72A for [80h] (R/3 = q1)
        AIWeapon = 10 // sub_4D06E loc_4D37B: 4CAA3 for [82h]
    };
    SubRingKind ring_subkind_ = SubRingKind::None;
    std::vector<RingIconPos> ring_sub_layout_; // screen coordinates of the current sub-ring icons
    int ring_sub_center_x_ = 0, ring_sub_center_y_ = 0;
    int ring_sub_hover_ = -1;
    int ring_sub_hover_frame_ = 0; // frame of the hovered sub-ring descriptor (sub_4AD6F equivalent)
    double ring_sub_hover_last_us_ = 0.0;
    bool ring_sub_open_ = false;
    int ring_sub_magic_g_ = -1;   // group 0..6 (sub_4BFCA id - 1) chosen at level 1
    int ring_sub_magic_d_ = -1;   // b 0..7 chosen at level 2 (-2 = level-2 stage active)
    int ring_sub_ai_first_ = -1;  // id chosen at AI level 1 (selects the level-2 options)
    const SPRParser* ring_sub_icons() const; // ICO per kind (MAGIC/SPECIAL/SKILL/AI_CTRL)
    SPRParser* ring_sub_icons_mut();
    void ring_sub_compute_layout(int unit_index); // final sub-ring layout of sub_4B0FB
    void ring_sub_compute_layout_at(int unit_index, double radius, double start_angle);
    int ring_sub_hit_test(int mx, int my) const;  // sub_4AC8A equivalent (sub-ring layout index or -1)
    void ring_sub_open(SubRingKind kind, int unit_index); // sub_4B8EB equivalent (0 -> 36 over 9 frames)
    void ring_sub_close();                        // sub_4B9D7 equivalent (36 -> 0 over 9 frames)
    void ring_sub_animate(int unit_index);         // sub_4B8EB loop (sub-ring display)
    void ring_sub_hover_advance();                 // sub-ring equivalent of sub_4AD6F
    void ring_sub_set_hover(int pos);              // sub-ring equivalent of sub_4AC8A
    void render_sub_ring();                       // sub-ring icons and hover
    bool ring_sub_handle_click(int unit_index, int mx, int my, int& out_opt); // equivalent of the 4BA7F click
    std::vector<int> battle_magic_group_options(int unit_index) const; // learned groups for the magic menu gate
    std::vector<int> battle_ai_first_ids(int unit_index) const; // sub_4C4F9 equivalent
    std::vector<int> battle_ai_second_ids(int unit_index, int first_id) const; // 4D06E level 1 -> level 2
    void battle_ai_apply_second(int unit_index, int second_id); // 4D06E off_4D03A equivalent (apply level-2 value)
    void battle_subring_pick(int unit_index, int opt); // shared sub-ring confirm (click and digits take one path)
    bool battle_ai_target_input(int unit_index);  // sub_4CE1F equivalent (click confirm, true = input consumed)
    void battle_ai_enter_target(int unit_index);  // 4CE1F entry ([71h]=FF, [73h]=0, [6Fh]=FF)
    void ring_handle_ai_config(int unit_index);   // sub_4D06E entry (set [83h]=1, then the two-level ring)
    bool battle_ring_dispatch(int unit_index, int menu_id); // sub_4D442 equivalent; true = turn ended/consumed
    bool battle_magic_select_ = false;       // spell selection in progress (sub_4C72A)
    std::vector<int> battle_magic_options_;  // R codes (g*21+d*3+k) for the 7 entries of the current page
    std::vector<int> battle_magic_groups_;   // learned groups; replaces the level-3 4C72A pick (M pages through them)
    int battle_magic_page_ = 0;              // current group page
    int battle_magic_pending_r_ = -1;        // chosen R waiting for a target click (-1 = none)
    bool battle_item_select_ = false;        // item selection in progress (sub_4CDBD)
    std::vector<int> battle_item_options_;   // dx list (sub_4C19D order), 7 entries of the current page
    std::vector<int> battle_item_full_options_; // full dx list, kept for page changes
    int battle_item_page_ = 0;               // current item page
    int battle_item_pending_dx_ = -1;        // chosen dx waiting for a target click (-1 = none)
    bool battle_weapon_select_ = false;      // weapon selection in progress (sub_4CAA3/4C98A)
    std::vector<int> battle_weapon_options_; // 4C03E entry list (choose a position si)
    int battle_tp() const { return (int)battle_ai_gauge_baseline_; } // sub_1A0E9
    void battle_tp_set(int v) { battle_ai_gauge_baseline_ = (int16_t)v; } // sub_1A0BF
    void battle_tp_consume(int n);           // sub_1A0C6 (floors at 0)
    void battle_consume_tp(int unit_index, int amount); // sub_45B4E ([57h]=0 plus the TP deduction)
    bool battle_tp_changed() const { return battle_turn_start_tp_ != battle_ai_gauge_baseline_; } // sub_1A0AD
    std::vector<int> battle_player_menu_ids(int unit_index); // sub_4BD26 (ring options)
    // sub_1D124: builds F0648 from g2range (cross-checked against the original machine code); the
    // result lands in battle_target_list_.
    int battle_range_collect(int unit_index);                // -> list size
    int battle_range_collect_dir(int unit_index, int dir);    // facing-argument variant (sub_3EE2B)
    // Facing-loop form for sub_3EE2B: the original shares the global E8F46, so the 13F8A clear
    // and shadow of one wc5 LOS run survive into the 165C2 test of the next facing. The grid is
    // therefore passed in and out; on return grid is the end state of this 1D124 run.
    int battle_range_collect_dir_grid(int unit_index, int dir,
                                      std::vector<int16_t>& grid);
    void battle_range_mark(int unit_index);                  // sub_1CC09 -> battle_range_e8_
    bool battle_player_can_attack(int unit_index);           // sub_1D124 gate (count > 0)
    void battle_attack_begin(int unit_index);                // sub_4D442 id 0 -> start sub_1F738
    bool battle_attack_input(int unit_index);                // target-phase input (true = input handled)
    void battle_attack_cancel();                             // 1F738 returned 0 (the caller returns to the ring)
    int  battle_attack_execute(int unit_index);              // late sub_1F738 -> 0/1/2
    int  battle_resolve_list(int attacker_index, bool declare, std::vector<std::pair<int, BattleAttackRoll>>* out); // sub_1DDF0 (+1E61D/1EF5C/1ED45)
    void battle_attack_finish(int unit_index, int ret);      // handles the sub_4D442 id 0 result
    void render_attack_range();                              // sub_44D67(1) range shading
    g2range::Env battle_range_env_make();                    // BattleUnit -> g2range::Env callbacks
    g2range::Unit battle_range_unit(int unit_index) const;
    int battle_spell_kind(const CombatUnitPlacement& u, int group) const; // sub_28429
    int battle_spell_thr(const CombatUnitPlacement& u, int d) const;      // sub_28480
    int battle_spell_damage(const CombatUnitPlacement& caster, const CombatUnitPlacement& target,
                            int base, int p1, int p2);                    // sub_285E4 (raw x100)
    void battle_apply_spell_hit(int caster_index, const std::vector<int>& targets,
                                int base, int p1, int p2);                // sub_28728 equivalent
    void battle_spell_status_29437(int caster_index, const std::vector<int>& targets,
                                    int status, int chance, int edi);               // sub_29437
    void battle_heal_462a7(int unit_index, int amount_raw);                          // sub_462A7
    void battle_spell_heal_2accb(int caster_index, const std::vector<int>& targets, int base, int p1); // sub_2ACCB
    void battle_item_2a75a(int unit_index, int kind, int sub);                       // sub_2A75A kind 3/4
    // sub_2A75A off_2A746 dispatch: per kind, a 4F460 (SSP) plus NSP driver (Meteor, Star Tearer,
    // Mega Thunder, Heal Wind, Sun Light). Shared by battle_fx_driver and the 3BDC7 item path.
    bool battle_item_2a75a_fx(int unit_index, int kind, int k, int tx, int ty);
    std::vector<int> battle_spell_2a500_extras(int caster_index, const std::vector<int>& listed,
                                               int base, int p1, int p2);            // sub_2A500
    void battle_spell_1ed90_settle(int caster_index, const std::vector<int>& units); // sub_1ED90 (settle units outside the list)
    void battle_clone_3bc67(int unit_index);                                         // sub_3BC67
    void battle_item_3c09f(int unit_index);                                          // sub_3C09F
    void battle_item_3c3a5(int unit_index);                                          // sub_3C3A5
    void play_unit_effect(const VmEvent& ev);                                        // VM 0x09 sub_51FF7
    void battle_item_3c12d(int unit_index);                                          // sub_3C12D (dx 12)
    int  battle_form_17d82(const CombatUnitPlacement& u, int sel) const;             // sub_17D82
    void battle_form_18262(CombatUnitPlacement& u, int slot) const;                  // sub_18262
    void battle_form_17e6d(CombatUnitPlacement& u, int form) const;                  // sub_17E6D
    int  battle_item_3c548(int unit_index, int form);                                // sub_3C548 (transform summon, 1 = success)
    void battle_item_3c70c(int unit_index);                                          // sub_3C70C + sub_1808A (undo transform)
    // sub_1808A (restore the [77h] stored record). preserve_facing=false keeps the facing stored
    // with the record ([63h]) as 3B614 does; present=false skips sub_45559 of 3C70C.
    void battle_restore_stored_1808a(int unit_index, bool preserve_facing, bool present);
    int  battle_item_3b7ce(int unit_index, int tx, int ty);                          // sub_3B7CE (mount: 1 = success, 0 = none, 3 = reselect)
    int  battle_item_3b614(int unit_index, int tx, int ty);                          // sub_3B614 (dismount: 1 = success, 0 = none, 3 = reselect)
    uint16_t battle_fd8bc_ = 0, battle_fd8be_ = 0, battle_fd8c0_ = 0;                // word_FD8BC/BE/C0 (one-shot flags for dx 16)
    void battle_item_3bdc7(int unit_index);                                          // sub_3BDC7 (dx 8)
    void battle_thunder_sweep_2a5be(int caster_index, const std::vector<int>& victims,
                                    int status, int chance, int edi);               // sub_2A5BE
    // Ring display (sub_4B767/4B8EB). first_entry=true is the first entry of sub_4D674 (after
    // 4B0F5/4BD26/4AD69 and including the sub_1A0F0(2) clock-to-TP transition before 4B767).
    // The 4D7EA redisplay loop does not call 1A0F0.
    void battle_player_show_menu(int unit_index, bool first_entry = false);
    bool battle_player_enter_ring(int unit_index); // sub_4D674 entry (4D582+1124A+455BD+1CC09+ring, FDAF2 branch)
    std::pair<int,int> battle_4d582_tile(int unit_index); // sub_4D582 (D66A8 visual centre + wc5/0x0B facing 3 px -> tile)
    void battle_4d582_center(int unit_index); // sub_4D582+1124A (large units use the raw footprint)
    bool battle_free_input();                    // sub_5580F free wait (hover, click, right click, info, Space)
    void battle_ring_cancel_to_free(int unit_index); // ring cancel -1 -> free wait (queue kept, gauge restored)
    void battle_show_info_modal(int unit_index); // sub_493A3 equivalent (banner off + dim + blocking 499EF(4B0h) wait)
    void battle_draw_unit_info_panel(int unit_index); // sub_48734 equivalent (portrait, name, MENUSTAT #36, small-font values)
    void draw_small_text_outlined(int x, int y, const char* s, uint8_t color); // sub_4868A (4-way outline in colour 6 plus the body colour)
    void battle_text_band(const std::string& text, bool redraw_frame = true); // sub_48083 (false = draw on the current backbuffer)
    void battle_unit_message(int unit_index, const std::string& johab_text); // sub_482A5 + sub_499EF(2BCh)
    // sub_482A5(eax = record pointer): reads portrait, name and coordinates from that record
    // rather than from an index. sub_17A20 passes the [77h] stored record instead when it exists.
    void battle_unit_message(const CombatUnitPlacement& rec, const std::string& johab_text);
    void battle_search_marker(int unit_index, int tx, int ty);          // sub_18D36 success animation
    void battle_draw_tile_marker(int tx, int ty, int tw = 24, int th = 16); // sub_49EEE
    bool wait_info_dismiss(int ticks);           // sub_499EF equivalent (any key, left click or timeout; Esc leaves skip_cutscene_ alone)
    void battle_render_ring_panels(int unit_index); // sub_4B744 (magic level 1 -> 4B18F, AI levels 1/2 -> 4B4CA)
    void battle_panel_magic_4b18f(int unit_index);  // sub_4B18F: magic list panel (labels and coordinates from 484F9)
    void battle_panel_ai_4b4ca(int unit_index);     // sub_4B4CA: AI settings summary band
    int16_t battle_panel_label(int x, int y, const char* text, int color_arg, int16_t residue); // sub_484F9(frame 0x24)
    void ring_draw_icon_4ac28(const SPRParser& set, int idx, int x, int y);  // sub_4AC28 (frame 0 of logical idx)
    void battle_render_free_hover();             // free-wait hover box and HP (49FE1 + 4A106 equivalent)
    void battle_magic_show_page(int unit_index); // test key M: prepare the spell list of the current group page (not drawn)
    void battle_player_end_turn(bool action_end); // sub_4D674 loc_4DAAF equivalent
    bool battle_wait_heal(int unit_index);   // sub_4D3BA + sub_462A7
    void battle_counterattack_list(int attacker_index);              // sub_1EF9A (whole F0648 list)
    void battle_clear_ailment_flags(int a_index, int b_index); // sub_1E2B1 (one unit at a time; negative indices are skipped)
    void battle_apply_1ebfe(int attacker_index, int target_index, int hit_kind, int result); // sub_1EBFE (physical-hit ailment)
    int battle_summon_djinni(int caster_index); // sub_2B97F -> sub_29F4D/299DA/29C5A (Djinni summon)
    // ---- F13B0 spell handlers (dword_F13B0[49], q1 = g*7+d) ----
    bool battle_spell_can_target(const CombatUnitPlacement& o) const;            // sub_28596
    int  battle_spell_pick_single(int unit_index, int tx, int ty, int range_add, int mode); // 28982/2AE44
    std::vector<int> battle_spell_diamond(int unit_index, int cx, int cy, int radius,
                                          int mode, bool undead_only = false);    // 28A97/2AF53/2AFE0
    int battle_spell_28ed6(int unit_index, int row, int k, int tx, int ty);        // 28ED6/28F0D/28F4C
    int battle_spell_29273(int unit_index, int bit, int k, int tx, int ty);        // 29273/29636/29675
    int battle_spell_2a1c6(int unit_index, int kind, int k);                       // 2A1C6/2A3E4/2A21A
    int battle_spell_2b1d0(int unit_index, int kind, int k, int tx, int ty);       // 2B1D0/2BFC5
    int battle_spell_2b391(int unit_index, int bit, int k, int tx, int ty);        // 2B391 (row 4)
    int battle_spell_2b9c8(int unit_index, int k, int tx, int ty);                 // 2B9C8 Heal
    int battle_spell_2ba8e(int unit_index, int k, int tx, int ty);                 // 2BA8E Wide Heal
    int battle_spell_2bc3b(int unit_index, int k);                                 // 2BC3B Revible
    int battle_spell_2c00e(int unit_index, int k, int tx, int ty);                 // 2C00E Melt Item
    int battle_spell_2c28f(int unit_index, int k, int tx, int ty);                 // 2C28F Death
    int battle_spell_2c36a(int unit_index, int k);                                 // 2C36A Soul Recall
    int battle_spell_2c468(int unit_index, int k, int tx, int ty);                 // 2C468 DNA Change
    int battle_spell_2c566(int unit_index, int k, int tx, int ty);                 // 2C566 Black Hole
    int battle_spell_field(int unit_index, int g, int k, int tx, int ty);          // 29514
    int battle_spell_wave(int unit_index, int g, int k, int tx, int ty,
                          bool play_visual = false, bool* out_had_targets = nullptr); // 29722
    // ---- Spell effects (sub_4F460 SSP / sub_1FEA4 NSP / sub_287AF hit pacing) ----
    // An NSP frame is a flat block of "[w][h] plus w*h pixels (0FFh transparent)", the same layout
    // sub_5EE51 takes.
    struct MagicFxFrame {
        int w = 0, h = 0;
        int16_t xo = 0, yo = 0;
        // Offsets as parsed. Step 9 of 341C7 (falling rocks) advances [+4]/[+6] itself, so these
        // values must be restored to make every casting behave like a fresh read.
        int16_t xo0 = 0, yo0 = 0;
        std::vector<uint8_t> flat;
    };
    std::vector<MagicFxFrame> magic_frames_;   // unk_F0DAA table, filled by sub_1FEA4
    std::string magic_frames_name_;
    int magic_fx_frame_ = -1;                  // frame being drawn (-1 = none); shared player only
    int magic_fx_x_ = 0, magic_fx_y_ = 0;
    // Sprites queued for this frame by battle_fx_blit / battle_fx_blit_tinted.
    // render_magic_effect_layer draws them in order (later on top) and battle_fx_present clears
    // them.
    //   expand_only = true forces the 5F570 upscale even when the destination is smaller than the
    //   source; 276B9/2799A/27C5A rely on that.
    //   unclipped = true selects sub_5F29D (same tint LUT as 5F305, but no 13Fh/C7h clip).
    //   shrink_only = true forces the 5F611 downscale even when the destination is larger than
    //   the source (5A267/2690B call 5F611 explicitly).
    //   silhouette >= 0 selects sub_5F093: every non-0FFh source pixel becomes that colour.
    struct MagicFxDraw { int frame; int x, y; int tint_pct; int dst_w, dst_h; bool expand_only = false; bool unclipped = false; bool clip319 = false; bool shrink_only = false; int silhouette = -1; };
    std::vector<MagicFxDraw> magic_fx_draws_;
    // dword_F9F08 + word ptr dword_FA8B6+2: units that sub_44DCC redraws inside 454D5.
    // The original stores raw unit pointers and does not deduplicate (at most 59h entries).
    std::vector<int> battle_highlight_units_;
    // Highlighted-unit draw entries produced by render_battle_map this frame, replayed by 44DCC.
    std::vector<DrawEntry> highlight_draws_;
    bool battle_nsp_load(const char* name);    // sub_1FEA4 (MAGICSPR\<name>)
    void battle_nsp_release();                 // sub_1FFCD (release the table)
    void battle_ssp_play(const char* name);    // sub_4F460 (play MAGICSPR\<name>)
    const char* battle_spell_ssp(int r_code) const;   // SSP the handler loads through 4F460
    const char* battle_spell_nsp(int r_code) const;   // NSP that handler's driver loads through 1FEA4
    void battle_spell_pace_287af();            // sub_287AF (24-frame hit, pose reset, cadence restore)
    void battle_spell_2c8a4(int effect);        // sub_2C8A4 (VM spell >= 0x64 screen damage)
    void battle_magic_fx(int r_code, int unit_index, int tx, int ty); // SSP + NSP playback + 287AF
    bool battle_fx_projectile_2016a(int unit_index, int tx, int ty, int subframe); // sub_2016A (f_arrow)
    bool battle_fx_blast_203cc(int unit_index, int tx, int ty);                     // sub_203CC (f_blast + shake)
    // ---- Shared effect blitters and present (used by every driver) ----
    // battle_fx_blit: sub_5EE51, one flat [w][h]+px frame (0FFh transparent).
    // battle_fx_blit_clipped: sub_5EFB9, the same flat frame but clipped at 13Fh/C7h.
    // battle_fx_blit_tinted: sub_5F305 (scale the palette entry to tint_pct%% brightness, then
    // alpha-blend it over the background; clips at 13Fh/C7h).
    // battle_fx_blit_tinted_unclipped: sub_5F29D (same LUT, but walks the framebuffer stride
    // without the 13Fh/C7h clip, like the original).
    void battle_fx_blit(int frame, int x, int y);
    void battle_fx_blit_clipped(int frame, int x, int y);
    void battle_fx_blit_tinted(int frame, int x, int y, int tint_pct);
    void battle_fx_blit_tinted_unclipped(int frame, int x, int y, int tint_pct);
    void battle_fx_blit_silhouette(int frame, int x, int y, int color); // sub_5F093
    // sub_13669: fill the live unit's pose-0 frame with `color` (5F517). When [77h] exists and
    // [5Ch] <= 6, the stored record's sprite is filled at the live unit's anchor too.
    void battle_fx_silhouette_13669(int unit_index, int color);
    void battle_fx_present();                                  // sub_454B3 + sub_5E9E8
    void blit_frame_tinted(int x, int y, const MagicFxFrame& fr, int tint_pct,
                           bool clip = true); // sub_5F305 (clip) / sub_5F29D (no clip)
    int  battle_fx_frame_count() const { return (int)magic_frames_.size(); }
    bool battle_fx_loaded() const { return !magic_frames_.empty(); }
    // ---- Individual drivers (defined in their own .cpp; arguments are caster index, target tile
    // ----
    //      x/y and handler k) ----
    // ---- Field and wave family (sub_29514/29722) ----
    int battle_fx_field_engine(int unit_index, int tx, int ty, int count,
                               const int8_t* xo, const int8_t* yo, const uint8_t* life,
                               bool energy, bool facing_layout, int camera_dy,
                               bool respawn_on_new_zero, int death_rand, int respawn_mode);
    int battle_fx_20620(int unit_index, int tx, int ty, int energy);   // f_field.nsp, 25 particles
    int battle_fx_2095a(int unit_index, int tx, int ty, int energy);   // f_field.nsp, 27 particles
    int battle_fx_20ddf(int unit_index, int tx, int ty, int energy);   // f_field.nsp, 61 particles
    int battle_fx_21114(int unit_index, int tx, int ty, int energy);   // boom.nsp, 5 particles
    int battle_fx_2132b(int unit_index, int tx, int ty, int energy);   // boom.nsp, 24 particles
    int battle_fx_215fe(int unit_index, int tx, int ty, int energy);   // boom.nsp, 29 particles
    int battle_fx_22af9(int unit_index, int tx, int ty, int k);        // icewind/icestor/icetypho
    int battle_fx_22172(int unit_index, int tx, int ty, int k);   // icemiss.nsp (Ice Missile)
    int battle_fx_22486(int unit_index, int tx, int ty, int k);   // diamondd.nsp (Diamond Dust)
    int battle_fx_23882(int unit_index, int tx, int ty, int k);   // lbolt.nsp (L.Bolt)
    int battle_fx_5666e(int unit_index, int tx, int ty, int k);   // explosio.nsp (Explosion)
    int battle_fx_56896(int unit_index, int tx, int ty, int k);   // firestor.nsp (Fire Storm)
    int battle_fx_56bd4(int unit_index, int tx, int ty, int k);   // hellfire.nsp (Hell Fire)
    int battle_fx_57766(int unit_index, int tx, int ty, int k);   // leopard.nsp (Leopard)
    int battle_fx_57ac4(int unit_index, int tx, int ty, int k);   // icestorm.nsp (Ice Storm)
    int battle_fx_57f18(int unit_index, int tx, int ty, int k);   // blizzard.nsp (Blizard)
    int battle_fx_241f7(int unit_index, int tx, int ty, int k);   // p_light.nsp (P.Lighting)
    int battle_fx_24466(int unit_index, int tx, int ty, int k);   // p_light.nsp variant
    int battle_fx_2476e(int unit_index, int tx, int ty, int k);   // Heal effect
    int battle_fx_25047(int unit_index, int tx, int ty, int k);   // Heal effect 2
    int battle_fx_25254(int unit_index, int tx, int ty, int k);   // Wide Heal effect
    int battle_fx_253dc(int unit_index, int tx, int ty, int k);   // Wide Heal effect 2
    int battle_fx_25591(int unit_index, int tx, int ty, int k);   // Wide Heal effect 3
    int battle_fx_59675(int target_index);                        // Turn Undead unit effect
    int battle_fx_261fc(int unit_index, int tx, int ty, int k);   // Power UP effect
    int battle_fx_26391(int unit_index, int tx, int ty, int k);   // Speed UP effect
    int battle_fx_26584(int unit_index, int tx, int ty, int k);   // Magic Armor effect
    int battle_fx_26672(int unit_index, int tx, int ty, int k);   // Bless effect
    int battle_fx_26810(int unit_index, int tx, int ty, int k);   // Silence effect
    int battle_fx_26b1f(int unit_index, int tx, int ty, int k);   // Curse effect
    int battle_fx_204e8(int unit_index, int tx, int ty, int k);   // X.Blade enchant effect
    int battle_fx_5637d(int unit_index, int tx, int ty, int k);   // T.Blade effect
    int battle_fx_22767(int unit_index, int tx, int ty, int k);   // blade variant 1
    int battle_fx_229cc(int unit_index, int tx, int ty, int k);   // blade variant 2
    int battle_fx_2262c(int unit_index, int tx, int ty, int k);   // blade variant 3
    int battle_fx_2690b(int unit_index, int tx, int ty, int k);   // Barrier effect
    int battle_fx_275a4(int unit_index, int tx, int ty, int k);   // DNA Change effect
    int battle_fx_272ad(int unit_index, int tx, int ty, int k);   // summon landing effect
    int battle_fx_276b9(int unit_index, int tx, int ty, int k);   // Dark Zone family 1
    int battle_fx_2799a(int unit_index, int tx, int ty, int k);   // Dark Zone family 2
    int battle_fx_27c5a(int unit_index, int tx, int ty, int k);   // Dark Zone family 3
    int battle_fx_26c9a(int unit_index, int tx, int ty, int k);   // Death effect
    int battle_fx_26e1f(int unit_index, int tx, int ty, int k);   // Black Hole effect
    // ---- Item, wave, revival and special effects (sub_2A75A, 29722, 2BC3B, 2EB61 tail) ----
    int battle_fx_21e16(int unit_index, int tx, int ty, int k);   // sub_21E16 f-meteo (Meteor)
    int battle_fx_23074(int unit_index, int tx, int ty, int k);   // sub_23074 i-star/cure (Star Tearer)
    int battle_fx_570a1(int unit_index, int tx, int ty, int k);   // sub_570A1 pro/explosio (Mega Thunder)
    int battle_fx_58619(int unit_index, int tx, int ty, int k);   // sub_58619 moon (Heal Wind)
    int battle_fx_58c87(int unit_index, int tx, int ty, int k);   // sub_58C87 thun7 (Mega Thunder 0)
    int battle_fx_58e95(int unit_index, int tx, int ty, int k);   // sub_58E95 thun72 (1)
    int battle_fx_592b0(int unit_index, int tx, int ty, int k);   // sub_592B0 thun73 (2)
    int battle_fx_5a267(int unit_index, int tx, int ty, int k);   // sub_5A267 sunl (Sun Light 0)
    int battle_fx_5a566(int unit_index, int tx, int ty, int k);   // sub_5A566 sunl2 (1)
    int battle_fx_23aba(int unit_index, int tx, int ty, int k);   // sub_23ABA thundwav 0
    int battle_fx_23c36(int unit_index, int tx, int ty, int k);   // sub_23C36 thundwav 1
    int battle_fx_23f3a(int unit_index, int tx, int ty, int k);   // sub_23F3A thundwav 2
    int battle_fx_2da5e(int unit_index, int tx, int ty, int k);   // sub_2DA5E ffall/slight/spearfly
    int battle_fx_21944(int unit_index, int tx, int ty, int k);   // sub_21944 shared summon/appear effect
    int battle_fx_59a70(int caster_index, int revived_index);     // sub_59A70 revible (revival)
    int battle_fx_21be8(int unit_index, int tx, int ty, int k);   // sub_21BE8 b_dragon (2C8A4 meteor effect)
    int battle_fx_25dfe(int unit_index, int k);                   // sub_25DFE healwind (Wide Heal effect)
    int battle_fx_25c88(int unit_index, int tx, int ty, int k);   // sub_25C88 cure.nsp particles
    int battle_fx_2d5a3(int unit_index);                          // sub_2D5A3 counter.nsp (counterattack)
    int battle_fx_2d527(int unit_index);                          // sub_2D527 restrain.nsp (F1474 restrain)
    int battle_fx_2d684(int unit_index);                          // sub_2D684 critical.nsp (critical hit)
    int battle_fx_2d7d2(int unit_index, int k);                   // sub_2D7D2 swstr.nsp (sword strike)
    int battle_fx_2dfc1(int unit_index, int k);                   // sub_2DFC1 madaxe.nsp
    int battle_fx_2f3b2(int unit_index, int k);                   // sub_2F3B2 fistlv5/6.nsp
    int battle_fx_22d18(int unit_index, int tx, int ty, int k);   // sub_22D18 p_light.nsp (Ice Dragon)
    int battle_fx_2dd7a(int unit_index, int tx, int ty, int k);   // sub_2DD7A axefly.nsp (2F80B idx13)
    int battle_fx_3b9e1(int unit_index, int tx, int ty, int k);   // sub_3B9E1 swirl.nsp (whirl)
    int battle_fx_3c12d(int unit_index, int tx, int ty, int k);   // sub_3C12D Wish to Win (heal/sun)
    // R_code -> driver dispatch table, in kSpellNames order. true = that driver drew something.
    bool battle_fx_driver(int r_code, int unit_index, int tx, int ty);
    // ---- Low-level primitives (combatii_fx_gfx.cpp) ----
    void blit_scale_expand(int x, int y, const uint8_t* data, int dst_w, int dst_h); // sub_5F570
    void blit_scale_shrink(int x, int y, const uint8_t* data, int dst_w, int dst_h); // sub_5F611
    void battle_fx_46710(int unit_index);                          // sub_46710 8-pass pixel wipe
    void battle_fx_468BC(int unit_index);                          // sub_468BC horizontal shrink wipe
    void battle_fx_46a11(int unit_index);                          // sub_46A11 large-unit dissolve
    void battle_fx_blit_scaled(int frame, int x, int y, int dst_w, int dst_h,
                               bool expand_only = false, bool shrink_only = false);                            // 5F570/5F611 draw request
    void pal_snapshot_range(int start, int end, uint8_t* out);         // sub_5F945 (DAC -> buffer)
    void pal_load_range(int start, int end, const uint8_t* src);       // sub_5F9E9 (buffer -> DAC)
    void pal_scale_range(int start, int end, const uint8_t* src, int mul_pct); // sub_5FA2C
    void pal_sub_range(int start, int end, const uint8_t* src, uint8_t* delta); // sub_5FC21
    void pal_ramp_set_width(int color_index);                          // sub_5623E (word_11233C)
    void pal_ramp_apply(int pct);                                      // sub_56245 + sub_5FD1C
    int  pal_ramp_step(int base_pct, int dir_start_pct);               // sub_562C1 (returns 1/0/2)
    void pal_shift_fade(int mul_pct);                                  // 5F945+5FC21+5FD1C (brighten toward the next colour)
    void pal_cycle_entries(uint8_t* buf, int count);                   // loc_27B88 (2799A palette cycle)
    uint8_t fx_pal_cycle_[256 * 3] = {};                               // 5F945 copy cycled by step 2 of 2799A
    int pal_ramp_color_ = 0;        // word_11233C = high word of unk_11233A (palette entry the ramp targets)
    int pal_ramp_pos_ = 0;          // word_7550C (current ramp %)
    int pal_ramp_dir_ = 0;          // word_7550E (0 = rising, 1 = falling)
    int pal_ramp_mark_ = 0;         // byte_755E8 (position marker left by sub_5FA2C)
    void render_magic_effect_layer();          // sub_5EE51 frame blit
    int battle_spell_summon(int unit_index, int k, int row, int sprite, int combat_class); // 29F4D/299DA (class comes from the 2B7xx wrapper)
    void battle_remove_old_summons(int unit_index);                                // sub_29EE4
    int battle_corpse_take(int unit_index, bool random_pick);                      // sub_137E7
    int battle_cast_magic(int unit_index, int r_code, int tx, int ty); // sub_4C605 + F13B0
    int battle_use_item(int unit_index, int dx, int tx, int ty); // sub_4CB5E
    int battle_use_weapon(int unit_index, int si); // sub_4C98A (0 = ID weapon, 1+ = 753C2/F1474)
    void battle_weapon_301bb(int unit_index, int weapon_idx, int is_leader); // sub_301BB (V-weapon self-damage and effect)
    int battle_weapon_2caca(int attacker_index, int damage); // sub_2CACA (ITEM.DAT 0x39 percentage adjustment)
    // sub_2CC3A (V-normal attack, 5/7/0x0C). sfx_id is the hit sound the dispatcher passes in cx
    //   (sub_2F47F/2F68C = 1, sub_2FED7 = 2); the original reads it as [esp+66h]>>16 and hands it
    //   to 4F4CD.
    int battle_weapon_2cc3a(int unit_index, int variant, int sfx_id = 1);
    int battle_weapon_2ca40(int unit_index); // sub_2CA40 (V-weapon branch 1..3, updates [+36]/[+37])
    int battle_weapon_f1474(int unit_index, int idx, int dx); // sub_2F47F.. (all 25 F1474 vectors ported)
    int battle_weapon_302e6(int unit_index, int edi); // sub_302E6 (V-lead branch over 13 bodies; plays the SSP and returns)
    // The 13 sub_302E6 bodies (308EE..3AEE8). Gameplay (targeting, damage, death, triggers) is
    // identical across them and all 13 are ported as literal translations of the effect body:
    // 341C7 (f_quake), 37FA4 (irus), 32589 (arcion), 32BCB (icer), 34C48 (shak2), 367EE (nang) and
    // 3AEE8 (io) in combatii_fx_weapon.cpp; 308EE (bird), 31283 (asura2), 356DB (sarah),
    // 35FC2 (stormaxe), 377A6 (shu) and 39A49 (rain) in combatii_wbody_*.cpp (the latter six own
    // collection, damage and the tail inside their anim functions).
    // battle_weapon_302e6 plays the SSP (4F460) according to the sub_302E6 dispatch table.
    // Screen sweep, diamond, front and nearest collection approximate 1A1F8/1A452/1AC08/49ABC;
    // the 4A483 cursor selection is approximated automatically.
    int battle_weapon_collect_screen(int unit_index, int cap); // 1A1F8 screen 12x12 sweep (F0648 approximation)
    int battle_weapon_collect_diamond(int unit_index, int radius); // 1AC08/1AC59 diamond (manhattan)
    // F1474 idx 13..24 (2F80B..30049): Range-module collectors (1A452/1AC08/1A1F8), the 2E896
    // knockback, 2ED9A centre selection and the 2EF4A body.
    int battle_weapon_2cbfb(int unit_index, int delta);              // sub_2CBFB (16-bit)
    int battle_weapon_collect_rect(int unit_index, int x, int y, int count, int range,
                                   int* last_x = nullptr, int* last_y = nullptr); // 1A452/1A552 + 1A1F8
    int battle_weapon_collect_dia_at(int unit_index, int x, int y, int r); // 1AC08/1AC59 + 1A1F8
    void battle_weapon_front_tile(int unit_index, int& fx, int& fy); // 1A1DE + 1A146
    bool battle_weapon_hit_target(int unit_index, int ti, int fin);  // 45BFB/HP/45ED2 on one target
    void battle_knockback_2e896(int target_index, int dir);          // sub_2E896
    bool battle_weapon_2ed9a_pick(int unit_index, int radius, int& out_x, int& out_y); // sub_2ED9A
    int battle_weapon_2ef4a(int unit_index, int di, int type, int add); // sub_2EF4A
    int battle_weapon_f1474_ext(int unit_index, int idx, int dx);    // idx 13..24
    int battle_weapon_collect_front(int unit_index); // 49ABC + 1A1F8: single cell in front
    int battle_weapon_collect_nearest(int unit_index); // 4A483/356DB single-target automatic approximation (nearest hostile)
    int battle_weapon_collect_2ea0c(int unit_index, int radius); // sub_2EA0C automatic: front ray, dist 2..R, lowest 17531/1762C score, one unit
    int battle_weapon_collect_line(int unit_index, int range); // 37FA4 single target on a 7-cell straight line (first hostile)
    int battle_weapon_collect_2e0f0(int unit_index, int radius); // sub_2E0F0 automatic: front ray dist 1..R, every hostile, far to near
    void battle_weapon_apply_status(int target_index, int bit, int duration); // [+1Eh]|=bit, [+1Fh]=dur
    int battle_weapon_2caf7_loop(int unit_index, int mul, int add); // sub_2CAF7 gameplay core
    int battle_weapon_308ee(int unit_index); // edi 0: fire.ssp + 308EE (2, 10000 + [1E]1)
    int battle_weapon_31283(int unit_index); // edi 1: kang + f-blast + slash (10, 1000)
    int battle_weapon_32589(int unit_index); // edi 2: chang + slash (instant kill on one front target, 1DB1D)
    int battle_weapon_32bcb(int unit_index); // edi 3: i-star2 + slash + ice (4, 0, cap 50)
    int battle_weapon_341c7(int unit_index); // edi 4: thunder1 + f-blast + thunder3 + boom (2, 20000)
    int battle_weapon_34c48(int unit_index); // edi 5: ju (base*4 + 6000 + 2CACA loop)
    int battle_weapon_356db(int unit_index); // edi 6: f-arrow + growl (single target, base*5 + 3000 + 2CACA)
    int battle_weapon_35fc2(int unit_index); // edi 7: i-fist + m-wind (5, 3000, radius-2 diamond)
    int battle_weapon_367ee(int unit_index); // edi 8: shoot + f-arrow + bomb1 (4, 6000)
    int battle_weapon_377a6(int unit_index); // edi 9: pazizik + f-blast (2, 10000 + [1E]4)
    int battle_weapon_37fa4(int unit_index); // edi 10: elect (base+5000, 10 hits, returns 0)
    int battle_weapon_39a49(int unit_index); // edi 11: fire + effect2 + f-wave (2, 7000 + [1E]1)
    int battle_weapon_3aee8(int unit_index); // edi 12: f-blast (3, 7000 + [1E]2 + 197AD)
    std::vector<int> battle_item_menu_ids(int unit_index); // sub_4C19D (dx list)
    void battle_show_info(int unit_index);   // sub_493A3 (ring id 11)
    int battle_move_tp_cost(int budget, int e8dest) const; // loc_4D98E cost formula
    int battle_move_again_threshold(int unit_index) const; // sub_167C9 (10+30n)
    void battle_set_420f(int v); // sub_1A0F0: play the 45ACF/45A41 transition from the previous F042F, then set F042F = v
    void battle_gauge_transition(bool ascending); // sub_45A41 (TIME_GRA #1 -> #3) / sub_45ACF (#3 -> #1) clock-to-TP transition
    // ---- Scripted input injection (G2_INPUT_SCRIPT verification) ----
    //   Injects "AT <tick> KEY <name> | CLICK_TILE <x> <y>" lines gated on battle_time_. CLICK_TILE
    //   converts tile to screen with the cam/fine values at fire time, so it resists drift.
    //   Does nothing when unset.
    std::vector<std::string> test_script_lines_;
    bool test_script_loaded_ = false;
    size_t test_script_pos_ = 0;
    void battle_script_poll();
    // ---- Tile events (sub_187B1/sub_18B2E, 12-byte F037E records loaded by sub_4ED18) ----
    // The SCE file's n12 * 12 bytes are kept verbatim (f0 = x, f1 = y, f2 = item (F0382),
    // f3-4 = exp (F0383), f5-8 = money (F0385 dword), f9 = type (F0388+1: 0..3),
    // f10-11 = FE04F index). state (F0382) is mutable: item at first, 0xFF once consumed;
    // 0xFE means hidden (the 18605 skip).
    struct BattleTileEvent {
        uint8_t x = 0, y = 0, item = 0;
        uint16_t exp = 0;
        uint32_t money = 0;
        uint8_t type = 0;
        uint16_t fe_index = 0;
        uint8_t state = 0; // byte_F0382 mirror (0 = empty, 0xFF = consumed, 0xFE = hidden)
    };
    std::vector<BattleTileEvent> battle_tile_events_; // at most 10 (sub_18539/4ED18)
    uint8_t battle_fe04f_[0x1F4] = {}; // unk_FE04F (loaded by sub_4E4DD from 0x49D4, 0x1F4B; written back by 4E622; consumed flags)
    void battle_load_fe04f(); // sub_4E4DD: BLACKBOX.DAT 0x49D4 0x1F4B -> unk_FE04F
    // Stash (byte_FE4FF, 100 records of 30 bytes): [0] = count, [1..] = item number - 1.
    // sub_4ECE4 appends to the word_FDF85 record and sub_4E622 writes the block back to
    // BLACKBOX.DAT after 0x4158 (battle_stash_save).
    uint8_t battle_stash_[100 * 30] = {};
    uint32_t battle_stash_money_[100] = {}; // dword_FE36F[100] (u32 head of each BLACKBOX 0x4158 record)
    uint8_t battle_fdf87_[0xC8] = {};      // unk_FDF87 (BLACKBOX 0x190 block)
    void battle_stash_add(int item);   // sub_4ECE4 (al = item number)
    void battle_stash_save();          // sub_4E622: whole BLACKBOX block (day/phase/FDF87/FE04F/stash and money)
    void battle_blackbox_save(int open_mode = 2); // sub_4E622 in full
    void battle_blackbox_close();      // sub_4ECD7
    void battle_battle_cleanup_4ec51(); // sub_4EC51 + sub_4EAF3: remove and restore battle units
    void battle_write_blackbox_record(const CombatUnitPlacement& u); // 0x88 record write of sub_4EAF3
    // sub_1888D(eax = unit, dl = new item): two-way prompt when the bag is full.
    // true = confirmed (1), false = cancelled (0).
    bool battle_item_full_select(int unit_index, int new_item);
    // ---- Party equipment screen (sub_493A3 -> sub_48E90) ----
    void battle_equipment_screen(int unit_index);            // sub_48E90 (drag-and-drop equipment, exits on Esc/right click)
    void battle_equipment_render(int unit_index);            // sub_48BC1 (frame + 48734 panel + MENUSTAT #37/38/39 + stats + item icons)
    void battle_equipment_detail(int unit_index);            // sub_48863 (bottom-right button: weapon count and magic charge detail, waits 499EF(BB8h))
    bool battle_equip_check(int unit_index, int item, int kind); // sub_1832D (kind 0 = [5Ah], 1 = [5Bh], 2 = [5Dh]; prints the message band on failure)
    void battle_equip_apply(int unit_index, int item, int kind); // sub_181A6 (write the slot and add the stat bonus)
    void battle_equip_remove(int unit_index, int kind);          // sub_18262 (clear the slot and subtract the stat bonus)
    void battle_equip_draw_ring_icon(int idx, int x, int y);     // sub_4AC28 (frame 0 of logical idx in SPECIAL.ICO)
    int battle_tile_187b1(int unit_index) const; // sub_187B1: event index 0..9 or -1
    void battle_tile_18b2e(int unit_index, int event_index); // sub_18B2E: item/exp/money/consume
    void battle_tile_load_events(); // sub_4ED18 tail: parse sce_raw_ n12, init 18539, apply FE04F consumption
    uint8_t battle_search_slots_[10] = {}; // byte_F0382 (sub_18539: zeroed on scenario load; mirrors the tile-event state)
    int battle_search_cursor_ = 0;         // word_F040E (search cursor)
    // Class speed/data, damage adjustment and death morale tables are extracted from COMBATII.EXE
    // into combat_tables.h. unk_EDD88 is not in the EXE (it lives in object 2's BSS): sub_18115
    // fills
    // it from the first 0x1FF8 bytes of ITEM.DAT (124 records of 66 bytes).
    std::vector<uint8_t> battle_lookup_table_; // unk_EDD88: 66-byte records, sub_184A4
    bool battle_item_table_tried_ = false;
    uint16_t battle_pause_cooldown_ = 0; // word_789A4 (sub_1038B/sub_10392): decremented once per INT8
    bool     battle_loop_active_ = false;// port only: runs the pause ISR hook only inside the battle viewer loop
    // sub_4E0EE system menu and quick save/load (gameplay options stay in the port settings)
    std::string current_scenario_name_;   // last load_scenario filename (quick save header)
    bool battle_music_on_ = true;         // word_FDAE8
    bool battle_sfx_on_ = true;           // word_FDAE6
    int  battle_scroll_speed_ = 4;        // word_FDAEA: 0 -> 4, 1 -> 8
    int  battle_turn_interval_ = 10;      // sub_4DE0A TURN interval (2..30); byte_74138 initial value is 0x0A
    int  battle_system_menu_4e0ee(int no_saveload = 0); // sub_4E0EE (ESC menu; a non-zero argument disables QUICK SAVE/LOAD). 2 = quick load, 7 = restart, 8 = title, -1 = cancel
    bool battle_quick_save_4f4ef();       // sub_4F4EF: genesis.qsv
    bool battle_quick_load_4fd25();       // sub_4FD25: restore genesis.qsv
    // Time of day, taken from BLACKBOX.DAT or from byte +5 of the SCE header (sub_4E4DD /
    // sub_4ED18)
    uint8_t  tod_phase_ = 0;             // byte_FDF84: 0..15 (sub_197CA adds 1 and masks with 15)
    uint16_t tod_day_ = 0;               // word_FDF82: incremented each time the phase reaches 8
    uint16_t tod_factor_ = 100;          // word ptr dword_F042A+2: palette brightness %
    int8_t   sce_time_byte_ = -1;        // SCE header +5 (`test ah,ah; jl`); negative means use the BLACKBOX.DAT value
    // Port-only pacing: the original calls sub_19D53 on every loop iteration with no gating, so
    // its speed depends on the CPU. 0 = no gating (as the original), 15 = frame tick (11.7 Hz),
    // 30 = half tick (5.85 Hz). Set with G2_BATTLE_TICK.
    int      battle_tick_period_ = 30;
    double   battle_tick_next_us_ = 0.0;
    bool     battle_trace_ = false;      // G2_BATTLE_TRACE: log the tick order (sequence -> sub_51211 -> time)
    // ETC.ICO #0..#7 are the ready-unit markers (sub_12161), #8..#10 are battle state icons.
    SPRParser etc_icons_;
    bool etc_icons_loaded_ = false;
    bool etc_icons_tried_ = false;
    uint8_t ai_marker_phase_ = 0;        // byte_E518E: ETC.ICO ready-marker animation frame (0..3)
    // dword_E4FB6+2: three-frame phase (0..2) of the chr_effc.syb status overlay. The tail of
    // sub_12065 advances it together with byte_E518E (`(phase+1) % 3`, dasm 3237..3253).
    uint8_t effc_phase_ = 0;
    // ITEM.SPR (sub_18115: sub_4F25F -> unk_EFDB0 table, 124 frames). Frame = item number - 1;
    // the sub_1E460 shield icon uses it.
    SPRParser item_spr_;
    bool item_spr_loaded_ = false;
    bool item_spr_tried_ = false;
    // AI_CTRL.ICO #13 in DOS is the unit-command ring entry that toggles [+0x83].
    uint8_t  scenario_flags_[256] = {}; // word_FDF85 (byte array indexed by the condition block)
    uint8_t  script_fired_[100] = {};
    uint8_t  area_trigger_fired_[100] = {};
    int      script_forced_target_ = -1; // dword_11024C: units_ index (-1 = none); unit selectors sel >= 10 in scripts point here (sub_5019C)
    bool     victory_fired_ = false;    // sub_506F3 one-shot: 0x01 runs only once (the process exits
                                        //   after unlinking, so the original never re-enters)

    // ---- Opcode table and dispatch ----
    // dword_52DCF (26 bytes) is scanned with `mov ecx,1Bh; repne scasb`, so the
    // matching handler index is (26 - i); off_52DE9[i=0] is the loop head used
    // for unknown opcodes. kVmOpcodeOrder below is that table in index order,
    // i.e. kVmOpcodeOrder[26 - handler_index]. Verified against the disassembly
    // (loc_52E7D = dialogue, loc_52EC9 = spawn, ...).
    enum VmCmd {
        VM_END        = 0,   // 0x00 - end of script block (loc_53026)
        VM_EVT01      = 1,   // 0x01 sub_506F3 - stage clear / battle end
        VM_DIALOGUE   = 2,   // 0x02 sub_507BE - dialogue (extra p1/p2 prefix)
        VM_SPAWN      = 3,   // 0x03 sub_50D9F - reinforcement spawn
        VM_RETREAT    = 4,   // 0x04 sub_51449 - unit leaves battle
        VM_MOVE       = 5,   // 0x05 sub_516D0 - forced move / warp
        VM_ITEM       = 6,   // 0x06 sub_5196F - give/equip item
        VM_COMBAT     = 7,   // 0x07 sub_51C74 - scripted event combat
        VM_MAGIC      = 8,   // 0x08 sub_51E8B (edx=0) - magic effect
        VM_EVT09      = 9,   // 0x09 sub_51FF7 - unit animation/command
        VM_HP         = 10,  // 0x0A sub_5211D - hp/mp change
        VM_AI         = 11,  // 0x0B sub_522A7 - AI state/target bytes
        VM_FACTION    = 12,  // 0x0C sub_52463 - faction/class change
        VM_MORALE     = 13,  // 0x0D sub_52757 - morale / scroll limits
        VM_EVT0E      = 14,  // 0x0E sub_5283D - reset dword_74134
        VM_CAMERA     = 15,  // 0x0F sub_52940 - camera focus / scroll
        VM_TILE       = 16,  // 0x10 sub_52C06 - dynamic map tile change
        VM_FLAG       = 17,  // 0x11 sub_52CF1 - scenario flag store
        VM_SCENARIO   = 18,  // 0x12 sub_52D33 - transition to next scenario (16B name)
        VM_BGM2       = 19,  // 0x13 sub_52D7D - bgm/effect change
        VM_CHOICE     = 20,  // 0x65 sub_50751 - choice / battle branch
        VM_FACE       = 21,  // 0x66 sub_52848 - turn towards each other
        VM_MAGIC2     = 22,  // 0x6C sub_51E8B (edx=1) - magic effect variant
        VM_TARGET     = 23,  // 0x6F sub_52377 - set target unit / point
        VM_WEATHER    = 24,  // 0x71 sub_527FF - weather change
        VM_MESSAGE    = 25,  // 0x73 sub_52B4F - set message-window text
        VM_BGM        = 26   // 0x77 sub_52DA4 - bgm/effect change
    };
    // KVmOpcodeOrder[i] = dword_52DCF byte i; handler index = 26 - i.
    static const uint8_t kVmOpcodeOrder[26];
    // Returns the handler enum for an opcode byte, or (VmCmd)-1 for a byte that
    // is not in dword_52DCF (the original simply skips such a byte, because
    // repne scasb wraps onto off_52DE9[0] = loc_52F01 = the loop head).
    static VmCmd vm_opcode_to_cmd(uint8_t opcode);

    // Result of one VM walk (sub_52E55).
    struct VmResult {
        std::vector<CombatDialogueEntry> lines; // dialogue (0x02) + message-window captions (0x73)
        std::vector<VmEvent> events;            // playback timeline in command order
        std::string next_scenario;              // 0x12 payload
        uint32_t end_offset = 0;                // offset the walk stopped at
        bool ended = false;   // reached opcode 0x00 / a terminal command
        // Opcode the walk stopped on (0x00 = end of body, 0x12 = scenario switch, 0x01 = battle
        // end). The camera drain loop of loc_53026 runs only for 0x00, so the two must be told
        // apart.
        uint8_t end_opcode = 0xFF;
        bool aborted = false; // unsupported opcode (battle-only handler)
        bool choice = false;  // passed 0x65 (sub_50751); the original waits for input and continues
        bool range_error = false; // payload left the body range (corrupted count of a variable-length command)
        uint8_t abort_opcode = 0;
        int instructions = 0;
    };

    bool script_trace_ = false;                    // dump one decoded instruction trace
    std::string script_next_scenario_;             // 0x12 payload (next .SCE name)

    bool load_script_engine(); // sub_5306A (12 slot offsets + body buffer)
    // sub_500C9: copy len bytes of the script buffer at `offset` into dst.
    bool script_fetch(uint32_t offset, void* dst, uint32_t len) const;
    uint8_t  script_fetch_u8(uint32_t& off) const;
    uint16_t script_fetch_u16(uint32_t& off) const;
    uint32_t script_fetch_u32(uint32_t& off) const;
    bool script_in_range(uint32_t off, uint32_t len) const;

    // sub_530F1: walk the condition block at `offset`; returns the offset just
    // past it, or 0 when the condition failed (satisfied == false).
    uint32_t eval_condition(uint32_t offset, bool& satisfied) const;
    // sub_52E55: run the bytecode block starting at `offset`.
    VmResult run_vm(uint32_t offset);
    // Checks whether the position returned by a variable-length command (0x04/0x05/0x07/0x10)
    // looks sane. It keeps walking when the next byte is 0x00 (end of block) or an opcode present
    // in
    // dword_52DCF; otherwise it reverts that command to "not ported" so the offset cannot drift.
    bool vm_position_plausible(uint32_t off) const;
    // Variable form of sub_501BA: resolves a (sel, idx) unit reference to a real units_ entry.
    // sel==0 -> a CC990 candidate followed by the army-1 CC968 list and the [+87h] owner check.
    // A non-zero sel addresses CC968+8*sel, i.e. CC960[(sel+1)*8]: sel 1..5 -> effective army
    // lists 2..6 (5 = CC990), sel 6..9 -> CC998/BSS (not lists), and the signed-AL path
    // 0x80..0xFF treats 0xFF (-1) as CC960[0]; other negative indices have no modelled head.
    // sel 10..0x7F runs the sub_5019C forced-target check. This is separate from the AI watch
    // selector path of BattleUnit [+0x71] (sub_1379A, 0xFF sentinel).
    int find_unit_index_ctx(uint8_t sel, uint8_t idx, int forced_context) const;
    CombatUnitPlacement* find_unit_ref_mut(uint8_t sel, uint8_t idx);
    int find_unit_index(uint8_t sel, uint8_t idx) const;
    // The two words of dword_F0BA0 (morale and scroll limits), set by sub_52757 (0x0D)
    int16_t morale_lo_ = 0;
    int16_t morale_hi_ = 0x64;
    uint8_t script_flag_word_[256] = {};
    // sub_4ED18 copies FDAF8 (SCE 0x19, 0xC8 = 64 words) into word_FDAF6: FDAF6[0] is the BSS zero
    // and FDAF6[i] = SCE word[i-1] for i >= 1. 4E81F/52463 read the low byte.
    std::vector<int> sce_fdaf6_;
    uint8_t fdf85_lo_ = 0;
    uint16_t fdf85_word_ = 0;   // whole word_FDF85 (BLACKBOX 0x4BC8); the stash record number of sub_4ECE4.
    int sce_fdaf6_lookup(int idx) const;
    int blackbox_owner_for_id(int id);
    void battle_restore_blackbox_party(); // sub_4EA22: restore the BLACKBOX 0x88 records with FDAF6[slot]==1 into CC990

    // Trigger walkers
    // sub_53523: time-based table stored at the very start of the script body
    //           (compared against dword_F0418 = battle_time_).
    bool run_time_trigger(uint32_t time, VmResult& out);
    // sub_5358F: sequence table stored at script_offsets_[0] (compared against
    //           dword_74134 = script_seq_).
    bool run_seq_trigger(uint32_t seq, VmResult& out);
    bool run_area_trigger(const CombatUnitPlacement& moved_unit, VmResult& out);
    // sub_44CCA: screen fade-out for victory handling (called at the end of 0x01 playback).
    void victory_fade_out();
    void next_scenario_fade_out();   // sub_52D33 -> sub_55EBC: fade-out for the scenario switch
    // sub_52D33 (VM 0x12): restore units, save BLACKBOX, fade out, write contmap.dat, then end the
    // module through sub_61CFF(1). MAIN.EXE restarts COMBATII from contmap.dat.
    void vm_next_scenario_52d33(const std::string& name);
    // Fallback for scenarios whose trigger tables are empty: finds the display blocks in the body
    // and interprets them as VM bytecode. Blocks are separated by 0x00 (opcode 0); each starts with
    // a
    // display or effect command and ends with 0x00 (or stops at an unported battle command), which
    // is enough to collect the dialogue and captions.
    int scan_script_blocks(std::vector<CombatDialogueEntry>& out);

    // Shared record walk: layout (verified against sub_53523/sub_5358F)
    //   rec+0 u16  (unused by the walkers)      rec+2 u16 id
    //   rec+4 u16  payload size                 rec+6 u16 0 = end of table
    //   rec+8 condition block + bytecode        next rec = rec + 8 + size
    // continue_on_false: whether to keep walking to the next record when the condition block is
    //   false. sub_53523 (time) jumps to loc_53584 and continues; sub_5358F (sequence) leaves
    //   through loc_5236F and ends the table.
    bool walk_trigger_table(uint32_t base, uint32_t match_id, uint8_t* fire_counter,
                            bool continue_on_false, VmResult& out);
    // ---- Deployment phase (sub_55518, right after 4ED18 in main_ and before the battle loop) ----
    //      combatii_deploy.cpp ----
    //   dword_11209E low word: non-zero when the SCE zone grid (E8F46+2, value-1 at load) holds at
    //   least one 1 or 2. When 0, sub_55518 does nothing. The targets are the CC990 party records
    //   with [56h] != 0 (540CB).
    void battle_deploy_zone_parse(const uint8_t* data, size_t size);   // sub_4ED18: load E8F46+2 and the flag
    int  battle_deploy_55518();                                        // sub_55518
    bool battle_deploy_551e5(size_t roster_index, std::vector<int>& slot);  // sub_551E5
    void battle_deploy_54a82(const std::vector<size_t>& party, std::vector<int>& slot);  // sub_54A82 automatic placement
    int  battle_deploy_list(const std::vector<size_t>& party, std::vector<int>& slot, int& scroll, int& sel);  // sub_54C2C list screen (1 = selected, 0 = exit)
    bool battle_deploy_pick_tile(int unit_index, int& tx, int& ty);   // sub_4A483 (ecx=1): placement cell cursor
    int  battle_deploy_pick_facing(int unit_index, int tx, int ty);   // sub_16357: facing arrow (-1 = cancel)
    int  battle_deploy_zone_at(int x, int y) const;                    // sub_13CBD
    void battle_deploy_prepare(CombatUnitPlacement& nu, const CombatUnitPlacement& edi);  // sub_551E5 copy/mounted form
    bool battle_deploy_enabled_ = false;      // word dword_11209E
    uint8_t sce_deploy_facing_ = 0;           // dword_11209E+2 (SCE header +7): auto-placement facing; EDD76 for map kind 5
    std::vector<int16_t> sce_zone_grid_;      // E8F46+2 (raw SCE word - 1), stride = map_w_
    uint16_t sce_zone_w_ = 0, sce_zone_h_ = 0; // SCE header width/height (grid stride)
    bool battle_deploy_active_ = false;       // deployment phase active: draws the zone shading
    // sub_53F2C: victory/defeat event table walker. al=0 looks for id=9 in the slot[9] (edx=9)
    // table, al=1 for id=10 in slot[10] (edx=10); when the condition passes it runs the
    // victory/defeat cutscene through sub_52E55. Called by sub_506F3 (0x01 victory) and
    // sub_50751 (0x65 choice).
    bool run_victory_script(int which, VmResult& out);

    // Helpers for the condition block
    const CombatUnitPlacement* find_unit_ref(uint8_t sel, uint8_t idx) const;

    // One-shot trace of the decoded script (verification aid, logged once).
    void dump_script_engine();
};
