// combatii_script: SCE script VM, trigger evaluation and cutscene timeline playback.
// Split out of combatii.cpp; behavior is unchanged.
#include <algorithm>
#include "combatii.h"
#include "devutil.h"
#include "combatii_internal.h"
#include "combat_class_names.h"
#include <cstdio>

// ==== SCE script engine (COMBATII VM) ====
// Ported instruction by instruction from dasm/COMBATII_code.ASM:
//   sub_5306A  script slot table + body buffer load
//   sub_500C9  byte fetcher
//   sub_530F1  condition evaluator
//   sub_52E55  VM dispatch loop
//   dword_52DCF / off_52DE9  26 opcodes + 27-entry jump table
//   sub_53523 / sub_5358F  trigger workers
// Opcode-to-handler mapping: loc_52E7D = dialogue, loc_52EC9 = reinforcement,
// loc_52EDB = forced move, and so on.

// dword_52DCF holds the opcode bytes in reverse handler order:
//   77 73 71 6F 6C 66 65 13 12 11 10 0F 0E 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01
// sub_52E55 scans it with `mov ecx,1Bh; repne scasb` and jumps through off_52DE9[ecx*4].
// On a hit ecx = 26 - index, so the handler number is 26 - index. A miss leaves
// ecx = 0 and lands on off_52DE9[0] = loc_52F01 (loop head), ignoring that byte.
const uint8_t CombatII::kVmOpcodeOrder[26] = {
    0x77, 0x73, 0x71, 0x6F, 0x6C, 0x66, 0x65, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01
};

CombatII::VmCmd CombatII::vm_opcode_to_cmd(uint8_t opcode) {
    for (int i = 0; i < 26; ++i)
        if (kVmOpcodeOrder[i] == opcode) return (VmCmd)(26 - i);
    return (VmCmd)-1; // byte not in the table: the original simply skips it
}

namespace {
// C++ counterpart of the sub_500C9 cursor. Every read is bounds-checked against
// the script buffer.
struct ScriptCursor {
    const uint8_t* p = nullptr;
    uint32_t off = 0;
    uint32_t size = 0;
    bool ok = true;
    uint8_t u8() { if (off + 1 > size) { ok = false; return 0; } return p[off++]; }
    uint16_t u16() { uint16_t lo = u8(); uint16_t hi = u8(); return (uint16_t)(lo | (hi << 8)); }
    uint32_t u32() { uint32_t lo = u16(); uint32_t hi = u16(); return lo | (hi << 16); }
    bool skip(uint32_t n) { if (off + n > size) { ok = false; return false; } off += n; return true; }
    std::string str(uint32_t n) {
        if (off + n > size) { ok = false; return std::string(); }
        std::string s((const char*)p + off, n);
        off += n;
        return s;
    }
};

// Normalize a dialogue/message string: drop trailing NUL and CR only, because the
// original renders leading bytes verbatim. '/' (0x2F) inside dialogue is a real glyph
// in NEWASCII.FNT (a 2x2 dot on rows 7-8), so it must not be stripped: 1-1.SCE 0x1D1F
// carries six '/' bytes inside the VM_DIALOGUE payload, and sub_5DD5B draws every
// byte <= 0x7F except NUL/0x0A from that font table (loaded by sub_5DAA8).
std::string clean_sce_text(std::string t) {
    // SCE strings are NUL-terminated Johab text. sub_5DD5B draws up to the NUL and
    // applies 0x0A as y += word_755C6, so leading newlines/spaces must stay: the 1-1
    // caption data starts with `0A 0A` ("\n\n<< Bifrost ... >>") and lands two lines
    // down in the window (y = 0x24 + 0x20 = 0x44), which is where the original shows it.
    while (!t.empty() && (t.back() == '\0' || t.back() == '\r')) t.pop_back();
    return t;
}
// Caption test, ignoring surrounding white space (SCE prefixes `0A 0A`).
bool is_caption_text(const std::string& t) {
    size_t b = 0, e = t.size();
    while (b < e && (t[b] == ' ' || t[b] == '\n' || t[b] == '\r' || t[b] == '\t' || t[b] == '\0')) ++b;
    while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\n' || t[e - 1] == '\r' || t[e - 1] == '\t' || t[e - 1] == '\0')) --e;
    return e - b >= 4 && t.compare(b, 2, "<<") == 0 && t.compare(e - 2, 2, ">>") == 0;
}
// True when the text has any visible character; the original's empty "\n\n" window
// is excluded from the dialogue list.
bool text_has_visible_content(const std::string& t) {
    for (unsigned char c : t)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != '\0') return true;
    return false;
}
} // namespace

// Log helper: convert Johab to UTF-8 and cut on a character boundary. A plain
// `%.60s` would split a multi-byte sequence and print a broken glyph.
std::string log_text(const std::string& raw, size_t max_bytes) {
    std::string s = Font::johab_to_utf8(raw);
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut; // step back over continuations
    return s.substr(0, cut) + "..";
}

// sub_5306A: twelve dword slots (unk_10081C) followed by the body, which runs to EOF
// (unk_10084C). The file position at entry is the one left by sub_4ED18 after the four
// unit groups and the (u16 n, n*12B) table, and that position becomes body offset 0.
bool CombatII::load_script_engine() {
    script_loaded_ = false;
    script_buffer_.clear();
    script_next_scenario_.clear();
    victory_fired_ = false;   // reloading a scenario re-arms the 0x01 exit code
    std::fill(script_offsets_, script_offsets_ + kScriptSlots, 0u);
    std::fill(std::begin(script_fired_), std::end(script_fired_), 0);
    std::fill(std::begin(area_trigger_fired_), std::end(area_trigger_fired_), 0);
    std::fill(&hp_trigger_fired_[0][0], &hp_trigger_fired_[0][0] + 2 * kTriggerRecordMax, (uint8_t)0);
    std::fill(std::begin(region_trigger_fired_), std::end(region_trigger_fired_), (uint8_t)0);
    morale_trigger_fired_ = 0;
    script_forced_target_ = -1;
    if (sce_raw_.size() < 0x19C) return false;
    auto rd16 = [&](size_t p) { return (uint16_t)(sce_raw_[p] | (sce_raw_[p + 1] << 8)); };
    auto rd32 = [&](size_t p) {
        return (uint32_t)(sce_raw_[p] | (sce_raw_[p + 1] << 8) | (sce_raw_[p + 2] << 16) |
                          ((uint32_t)sce_raw_[p + 3] << 24));
    };
    uint32_t w = rd16(0x196), h = rd16(0x198);
    if (!w || !h) return false;
    size_t pos = 0x19A + (size_t)w * h * 2;   // SCE dummy tile buffer
    if (pos + 2 > sce_raw_.size()) return false;
    for (int g = 0; g < 4; ++g) {             // four unit groups
        if (pos + 2 > sce_raw_.size()) return false;
        uint32_t cnt = rd16(pos);
        pos += 2;
        // sub_4ED18 loc_4EFCB: when a record's [+0x77] link field is non-zero, the
        // record spans one extra 0x88 bytes. Skipping cnt*0x88 unconditionally misreads
        // every following n12/slot offset in scenarios that have linked units (1-4.SCE),
        // which makes the script engine fail to load and jumps straight to the viewer.
        for (uint32_t i = 0; i < cnt; ++i) {
            const size_t span = sce_unit_record_span(sce_raw_.data() + pos, sce_raw_.size() - pos);
            if (span == 0) return false;
            pos += span;
        }
    }
    if (pos + 2 > sce_raw_.size()) return false;
    uint32_t n12 = rd16(pos);                 // dword_F037E: n12 records of 12 bytes
    pos += 2 + (size_t)n12 * 12;
    if (pos + kScriptSlots * 4 > sce_raw_.size()) return false;
    for (int i = 0; i < kScriptSlots; ++i) script_offsets_[i] = rd32(pos + i * 4);
    pos += kScriptSlots * 4;
    script_buffer_.assign(sce_raw_.begin() + pos, sce_raw_.end());
    script_loaded_ = true;
    battle_tile_load_events(); // sub_4ED18 tail: load the F037E 12-byte tile events
    fprintf(stderr, "COMBAT: script engine loaded: body %zu bytes @file 0x%zX, slots=[",
            script_buffer_.size(), pos);
    for (int i = 0; i < kScriptSlots; ++i)
        fprintf(stderr, "%s0x%X", i ? "," : "", script_offsets_[i]);
    fprintf(stderr, "]\n");
    if (battle_trace_) dump_script_tables();
    return true;
}

bool CombatII::script_in_range(uint32_t off, uint32_t len) const {
    if (!script_loaded_) return false;
    if (off > script_buffer_.size()) return false;
    return len <= script_buffer_.size() - off;
}

bool CombatII::script_fetch(uint32_t offset, void* dst, uint32_t len) const {
    if (!script_in_range(offset, len)) return false;
    if (len) memcpy(dst, script_buffer_.data() + offset, len);
    return true;
}

uint8_t CombatII::script_fetch_u8(uint32_t& off) const {
    if (!script_in_range(off, 1)) { off = 0xFFFFFFFFu; return 0; }
    return script_buffer_[off++];
}

uint16_t CombatII::script_fetch_u16(uint32_t& off) const {
    uint8_t lo = script_fetch_u8(off), hi = script_fetch_u8(off);
    return (uint16_t)(lo | (hi << 8));
}

uint32_t CombatII::script_fetch_u32(uint32_t& off) const {
    uint32_t lo = script_fetch_u16(off), hi = script_fetch_u16(off);
    return lo | (hi << 16);
}

// sub_501BA resolves a (sel, idx) unit reference. Port model:
//   sel == 0   : sub_500ED(idx) picks a CC990 candidate and the matching unit is looked up
//                in the candidate's [+0Eh]-1 CC968 list by ID and [+87h] owner. The port
//                approximates this with a group-0 ID scan.
//   sel 1..3   : CC968+8*sel addresses army lists 1..3, compared through effective army.
//   sel 4..9   : those slots alias CC988/CC990/CC998, not army lists; the port compares
//                the raw army byte.
//   sel 10..7F : sub_5019C checks the effective army [+0x0E] of dword_11024C against
//                sel-9 (raw army + 1 is the fallback).
//   sel 80..FF : the original reads CC968[sel] with a signed AL (0xFF maps to CC960);
//                not modeled here. Unlike the AI [+0x71] path, VM sel==0 walks CC990.
int CombatII::find_unit_index(uint8_t sel, uint8_t idx) const {
    if (sel == 0xFF || sel > 0x7F) return -1; // outside the VM unit-ref model (not the CC960 AI path)
    if (sel > 9) {
        const int t = script_forced_target_;
        // The original checks the effective army [+0x0E]; raw+1 is a fallback that can
        // differ after group-0/VM remapping.
        if (t >= 0 && (size_t)t < units_.size() && battle_effective_army(units_[(size_t)t]) == (int)sel - 9) return t;
        return -1;
    }
    if (units_.empty()) return -1;
    if (sel == 0) {
        // sub_500ED supplies a CC990 ID candidate and the original then searches that
        // candidate's CC968 list for the same ID and [+87h] owner. Group remapping and
        // copy membership are not modeled, so this group-0 ID scan is an approximation.
        for (size_t i = 0; i < units_.size(); ++i)
            if (units_[i].group == 0 && units_[i].id == (int)idx) return (int)i;
        return -1;
    }
    // Selectors 1..3 address CC968 army lists 1..3 (effective army 2..4); selectors 4..9
    // alias other arrays and fall back to the raw army byte. Neither scan reproduces list
    // membership or order.
    for (size_t i = 0; i < units_.size(); ++i) {
        const bool army_matches = (sel <= 3)
            ? battle_effective_army(units_[i]) == (int)sel + 1
            : units_[i].army == (int)sel;
        if (army_matches && units_[i].id == (int)idx) return (int)i;
    }
    return -1;
}

const CombatUnitPlacement* CombatII::find_unit_ref(uint8_t sel, uint8_t idx) const {
    int i = find_unit_index(sel, idx);
    return i < 0 ? nullptr : &units_[(size_t)i];
}

// Returns a mutable unit pointer so script commands can act on the unit.
CombatUnitPlacement* CombatII::find_unit_ref_mut(uint8_t sel, uint8_t idx) {
    int i = find_unit_index(sel, idx);
    return i < 0 ? nullptr : &units_[(size_t)i];
}

// Position check for variable-length commands: the offset must hold 0x00 (end of block)
// or a byte present in dword_52DCF. The original skips unknown bytes too, but a
// miscalculated command boundary almost always shows up here.
bool CombatII::vm_position_plausible(uint32_t off) const {
    if (off >= script_buffer_.size()) return false;
    uint8_t b = script_buffer_[off];
    if (b == 0x00) return true;
    return (int)vm_opcode_to_cmd(b) >= 0;
}

// sub_530F1: read the condition block in order and evaluate every condition.
// Returns the end offset of the block on success (satisfied = true), or 0 on failure.
uint32_t CombatII::eval_condition(uint32_t offset, bool& satisfied) const {
    satisfied = false;
    if (!script_loaded_) return 0;
    ScriptCursor c{script_buffer_.data(), offset, (uint32_t)script_buffer_.size()};
    // 1) Turn/time condition: u32 = (flag_index << 16) | compare value; 0 disables it
    //    but still consumes 5 bytes.
    uint32_t hdr = c.u32();
    if (!c.ok) return 0;
    if (hdr != 0) {
        uint8_t cmp_type = c.u8();
        uint32_t t = battle_time_; // sub_19D47() == dword_F0418
        bool pass = (cmp_type == 0) ? (t == hdr) : (cmp_type == 1) ? (t < hdr) : (cmp_type == 2) ? (t > hdr) : false;
        if (!pass) {
            if (script_trace_) fprintf(stderr, "COMBAT: cond turn fail (time=%u need=%u type=%u)\n", t, hdr, cmp_type);
            return 0;
        }
    } else {
        // The fetch did not advance past the value, so the original's `add edi, 5`
        // (4-byte value + 1-byte compare type) still leaves one byte to skip.
        if (!c.skip(1)) return 0;
    }
    if (script_trace_) fprintf(stderr, "COMBAT: cond @0x%X turn/hdr=%u -> off 0x%X\n", offset, hdr, c.off);
    // 2) Unit presence: [u8 sel][u8 idx][u8 want]; idx == 0 skips the check.
    uint32_t unit_pos = c.off;
    uint8_t u_sel = c.u8(), u_idx = c.u8();
    if (!c.ok) return 0;
    if (u_idx != 0) {
        uint8_t want = c.u8();
        const CombatUnitPlacement* u = find_unit_ref(u_sel, u_idx);
        if ((u != nullptr ? 1 : 0) != (int)want) {
            if (script_trace_) fprintf(stderr, "COMBAT: cond unit fail (sel=%u idx=%u want=%u found=%d)\n", u_sel, u_idx, want, u != nullptr);
            return 0;
        }
    }
    c.off = unit_pos + 3; // the original does `mov edi, esi` (start + 3) either way
    c.ok = true;
    if (script_trace_)
        fprintf(stderr, "COMBAT: cond unit raw=%u/%u -> off 0x%X\n", u_sel, u_idx, c.off);
    // 3) blackbox.dat story flag: [u8 sel][u8 rec][u8 want]. The listing does
    //    lseek(0x3EE) + lseek(rec*0x88 - 1, SEEK_CUR), i.e. byte 0x3EE+(rec-1)*0x88+0x87;
    //    non-zero means the record exists, and that is compared with want.
    uint8_t bb_sel = c.u8(), bb_rec = c.u8(), bb_want = c.u8();
    if (!c.ok) return 0;
    if (script_trace_)
        fprintf(stderr, "COMBAT: cond blackbox raw=%u/%u/%u -> off 0x%X\n", bb_sel, bb_rec, bb_want, c.off);
    if (bb_rec != 0) {
        int flag = 0;
        std::string bp = game_path("BLACKBOX.DAT");
        if (!file_exists(bp.c_str())) bp = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "BLACKBOX.DAT");
        if (!bp.empty()) {
            if (FILE* bf = fopen(bp.c_str(), "rb")) {
                const long off = 0x3EEL + (long)bb_rec * 0x88L - 1L;
                if (fseek(bf, off, SEEK_SET) == 0) {
                    const int ch = fgetc(bf);
                    if (ch >= 0) flag = ch != 0 ? 1 : 0;
                }
                fclose(bf);
            }
        }
        if (flag != (int)(bb_want != 0 ? 1 : 0)) {
            if (script_trace_)
                fprintf(stderr, "COMBAT: cond blackbox fail rec=%u flag=%d want=%u\n", bb_rec, flag, bb_want);
            return 0;
        }
    }
    // 4) Equipment/item check on the forced target (dword_11024C): [u8 field][u8 value].
    //    Passes when value is 1 and one of [5Bh]/[5Ah]/[5Dh]/[5Eh]/[5Fh] equals field,
    //    or when value is 0 and none does. field == 0xFF skips the check.
    uint8_t iv_field = c.u8(), iv_value = c.u8();
    if (!c.ok) return 0;
    if (script_trace_)
        fprintf(stderr, "COMBAT: cond item raw=%u/%u -> off 0x%X\n", iv_field, iv_value, c.off);
    if (iv_field != 0xFF) {
        bool has = false;
        if (script_forced_target_ >= 0 && (size_t)script_forced_target_ < units_.size()) {
            const auto& t = units_[(size_t)script_forced_target_];
            has = (t.combat_type_5b == iv_field || t.combat_type_5a == iv_field ||
                   t.combat_type_5d == iv_field || t.inv_5e == iv_field || t.inv_5f == iv_field);
        }
        if (has != (iv_value != 0)) {
            if (script_trace_)
                fprintf(stderr, "COMBAT: cond target-item fail field=%u has=%d value=%u\n",
                        iv_field, has ? 1 : 0, iv_value);
            return 0;
        }
    }
    // 5) Two-pass area unit count (cl = 1 then 3), 6-byte record:
    //    [x1][y1][x2][y2][expected][compare] or [0xFF][5 bytes] to skip this pass.
    for (int pass = 0; pass < 2; ++pass) {
    // The listing sets CL=1 then 3, sign-extends effective [+0Eh] and CL, subtracts and
    // unsigned-skips differences >= 2, which counts armies {1,2} then {3,4};
    // the port substitutes a coarse team hint instead of that army-list test.
        uint8_t team = (pass == 0) ? 1 : 3;
        uint8_t first = c.u8();
        if (!c.ok) return 0;
        if (first == 0xFF) {
            if (script_trace_) fprintf(stderr, "COMBAT: cond area pass%d skipped -> off 0x%X\n", pass, c.off + 5);
            if (!c.skip(5)) return 0;
            continue;
        }
        uint8_t y1 = c.u8(), x2 = c.u8(), y2 = c.u8();
        uint32_t expected = c.u8();
        uint8_t cmp_type = c.u8();
        if (!c.ok) return 0;
        uint8_t x1 = first;
        if (x1 > x2) std::swap(x1, x2);
        if (y1 > y2) std::swap(y1, y2);
        int count = 0;
        for (const auto& u : units_) {
            // Static team comes from outer-group parity, spawned/Faction team from raw
            // army == 0 vs non-zero. The port counts every units_ base-tile record,
            // including inactive ones, rather than CC998 cells or list membership.
            const bool team_match = (pass == 0) ? (u.team == 0) : (u.team != 0);
            (void)team;
            if (team_match && u.x >= x1 && u.x <= x2 && u.y >= y1 && u.y <= y2) count++;
        }
        bool pass_ok = true;
        if (cmp_type == 0) pass_ok = (count == (int)expected);
        else if (cmp_type == 1) pass_ok = (count >= (int)expected);
        else if (cmp_type == 2) pass_ok = (count <= (int)expected);
        if (!pass_ok) {
            if (script_trace_)
                fprintf(stderr, "COMBAT: cond area fail (rect %u,%u-%u,%u count=%d type=%u expect=%u)\n",
                        x1, y1, x2, y2, count, cmp_type, expected);
            return 0;
        }
    }
    // 6) Scenario flag: [u16 index][u16 value]; index != 0 compares the flag word at 0xFDF86 +
    // index.
    uint16_t flag_idx = c.u16();
    uint16_t flag_val = c.u16();
    if (!c.ok) return 0;
    if (flag_idx != 0 && scenario_flags_[flag_idx & 0xFF] != (uint8_t)flag_val) {
        if (script_trace_)
            fprintf(stderr, "COMBAT: cond flag fail (idx=%u have=%u need=%u)\n",
                    flag_idx, scenario_flags_[flag_idx & 0xFF], flag_val & 0xFF);
        return 0;
    }
    satisfied = true;
    return c.off;
}

// sub_52E55: VM loop that reads one opcode at a time and dispatches through off_52DE9.
// Payload sizes come only from what the listing confirms; commands whose size is not
// established stop the walk with aborted set, instead of jumping to a bad offset.
CombatII::VmResult CombatII::run_vm(uint32_t offset) {
    VmResult r;
    if (!script_loaded_) { r.aborted = true; return r; }
    ScriptCursor c{script_buffer_.data(), offset, (uint32_t)script_buffer_.size()};
    uint32_t op_pos = offset;
    uint8_t op = 0;
    // Stop quietly when a variable-length count runs past the body; the original has no
    // bounds check and would read into the next segment.
    auto abort_range = [&]() { r.aborted = true; r.range_error = true; r.abort_opcode = op; };
    while (true) {
        op_pos = c.off;
        op = c.u8();
        if (!c.ok) { r.aborted = true; break; }
        if (op == 0x00) { r.ended = true; r.end_opcode = 0x00; break; } // loc_53026: end of block
        r.instructions++;
        uint8_t p1 = 0, p2 = 0;
        if (op == 0x02) { // dialogue: fetch the two payload bytes first (loc_52F2B)
            p1 = c.u8();
            p2 = c.u8();
            if (!c.ok) { abort_range(); break; }
        }
        VmCmd cmd = vm_opcode_to_cmd(op);
        if (script_trace_)
            fprintf(stderr, "COMBAT: vm @0x%X op=0x%02X cmd=%d\n", op_pos, op, (int)cmd);
        if ((int)cmd < 0) continue; // ignore bytes not in the table
        switch (cmd) {
        case VM_DIALOGUE: { // sub_507BE: [p1 sel][p2 idx][u16 len][len text]
            uint16_t len = c.u16();
            std::string txt = c.str(len);
            if (!c.ok) {
                // The block scanner read a 0x02 inside dialogue text. The original VM only
                // walks the trigger-table path, so it never reaches such a misread (2-6 @0x7BC
                // has payload "02 09 03 21 0a 00 48 00...", where sel/idx/len are all
                // implausible: they are bytes of the Johab text "H\n"). Skip quietly.
                if (script_trace_)
                    fprintf(stderr, "COMBAT: vm @0x%X op=0x02 misdecode (scan path ignored)\n", op_pos);
                r.aborted = false;
                c.ok = true;
                c.off = op_pos + 1;  // skip this byte only and keep walking (unknown-byte rule)
                break;
            }
            txt = clean_sce_text(txt);
            if (text_has_visible_content(txt)) {
                CombatDialogueEntry e;
                e.text = txt;
                e.is_overlay = false;
                // sub_507BE(al=p1, dl=p2): the two bytes are a (sel, idx) speaker reference,
                // not tile coordinates. sel 0 uses the CC990 candidate lookup; non-zero
                // selectors address CC968+8*sel, and only 1..3 are ordinary army slots
                // (1-1 sends p1=0, p2=18, i.e. unit ID 18 through the CC990 candidate).
                // The portrait comes from speaker unit [+10h] (`mov bl,[ebp+10h]`).
                int ui = find_unit_index(p1, p2);
                e.speaker_x = (ui >= 0) ? units_[(size_t)ui].x : -1;
                e.speaker_y = (ui >= 0) ? units_[(size_t)ui].y : -1;
                e.speaker_unit = ui;
                e.portrait_id = (ui >= 0) ? units_[(size_t)ui].face : -1;
                e.speaker_name = (ui >= 0) ? units_[(size_t)ui].name : std::string();
                r.lines.push_back(e);
                VmEvent ev;
                ev.type = VmEventType::Dialogue;
                ev.offset = op_pos;
                ev.text = txt;
                ev.portrait = e.portrait_id;
                ev.speaker_id = (ui >= 0) ? units_[(size_t)ui].id : -1;
                ev.speaker_name = e.speaker_name;
                // sub_507BE passes the speaker unit pointer (ebp) from sub_501BA to
                // sub_482A5; p1/p2 are a sel/idx reference, not coordinates. Record the
                // index and read the unit tile when the event is played.
                if (ui >= 0) {
                    ev.speaker_tx = units_[(size_t)ui].x;
                    ev.speaker_ty = units_[(size_t)ui].y;
                    ev.speaker_unit = ui;
                }
                r.events.push_back(ev);
                if (script_trace_)
                    fprintf(stderr, "COMBAT: vm dialogue sel=%u idx=%u -> unit %d (face %d): %s\n",
                            p1, p2, ev.speaker_id, ev.portrait, log_text(txt).c_str());
            } else {
                // Len == 0 or whitespace only: the original shows no empty box either.
                VmEvent ev; ev.type = VmEventType::Dialogue; ev.offset = op_pos; ev.text = ""; ev.portrait = -1;
                r.events.push_back(ev);
            }
            break;
        }
        case VM_MESSAGE: { // sub_52B4F: [u16 frames][u16 len][len text] message window text
            uint16_t frames = c.u16();
            uint16_t len = c.u16();
            std::string txt = c.str(len);
            if (!c.ok) { abort_range(); break; }
            txt = clean_sce_text(txt);
            // sub_52B4F only copies the text into the window buffer (unk_110586 /
            // unk_FF0BC slot) and stores the first u16 in word_11057C (display frames
            // left); it draws nothing. sub_5046B advances the window queue every frame,
            // so this command does not wait and the following 0x0F mode 2 wait lets the
            // window open and close in time order.
            VmEvent ev;
            ev.type = VmEventType::Message;
            ev.offset = op_pos;
            ev.text = txt;
            ev.is_overlay = txt.empty() ? false : is_caption_text(txt); // 1-1/38-1 "<< ... >>" caption
            ev.msg_frames = (int)frames;
            r.events.push_back(ev);
            if (text_has_visible_content(txt)) {
                CombatDialogueEntry e;
                e.text = txt;
                e.is_overlay = ev.is_overlay;
                e.portrait_id = -1;
                r.lines.push_back(e);
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm message frames=%u len=%u %s: %s\n",
                        frames, len, txt.empty() ? "(blank)" : "text", log_text(txt).c_str());
            break;
        }
        case VM_CAMERA: { // sub_52940 (0x0F): [A][B][C][D], always 4 bytes
            // Register accounting (COMBATII_code.ASM 90155-): var_8 = A (the byte after
            // the opcode), var_4 = B, [esp+0] = C, var_C = D; the common path loc_52B24
            // calls sub_50339(ecx=D, ebx=C, edx=B, eax=A). sub_50339 compares dl (=B) with
            // the low word of dword_BA83C (camera X tile) and bl (=C) with the high word
            // (camera Y tile), so B = target X tile, C = target Y tile and D = scroll speed
            // in pixels per frame. A (al) selects the mode:
            //   A==0 interpolated scroll to (B,C) at speed D
            //   A==1 jump immediately
            //   A==2 wait for the camera scroll (loc_52AA0); B==0 waits until done,
            //        otherwise B iterations
            //   A==3 move relative to unit (sel=B, idx=C): sub_52940 targets unit - 6 and
            //        clamps to the map bounds
            //   A>3  the original consumes the 4 bytes and does nothing
            uint8_t mode = c.u8(), b = c.u8(), cc = c.u8(), dd = c.u8();
            if (!c.ok) { abort_range(); break; }
            VmEvent ev;
            ev.offset = op_pos;
            ev.cam_mode = mode;
            bool emit = true;
            if (mode == 3) {
                // sub_52940: unit.x-6 / unit.y-6, clamped to (map limit - 12)
                int ui = find_unit_index(b, cc);
                if (ui >= 0) {
                    int tx = units_[(size_t)ui].x - 6;
                    int ty = units_[(size_t)ui].y - 6;
                    int maxx = (int)map_w_ - 12; if (maxx < 0) maxx = 0;
                    int maxy = (int)map_h_ - 12; if (maxy < 0) maxy = 0;
                    if (tx < 0) tx = 0; if (tx > maxx) tx = maxx;
                    if (ty < 0) ty = 0; if (ty > maxy) ty = maxy;
                    ev.type = VmEventType::CameraUnit;
                    ev.cam_x = tx; ev.cam_y = ty; ev.cam_speed = dd;
                } else {
                    // Without a unit the original jumps straight to loc_52B45 (no effect).
                    emit = false;
                }
            } else if (mode == 2) {
                // loc_52AA0: B (var_4) is the wait count (0 = until sub_5046B finishes)
                ev.type = VmEventType::CameraWait;
                ev.cam_count = b;
                ev.cam_x = b; ev.cam_y = cc;
            } else {
                // Mode 0/1/other(4+): join at loc_52A2C, i.e. queue the request or call
                // sub_50339(A,B,C,D) directly. A is passed through and only 1 means
                // immediate move; D is not adjusted here (sub_50339 maps 0->1, 3->8,
                // everything else x2).
                ev.type = VmEventType::CameraScroll;
                ev.cam_x = b; ev.cam_y = cc;
                ev.cam_speed = dd;
            }
            if (emit) r.events.push_back(ev);
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm camera mode=%u (B=%u X, C=%u Y, D=%u speed) -> event type %d\n",
                        mode, b, cc, dd, (int)ev.type);
            break;
        }
        case VM_FLAG: { // sub_52CF1: [u16 index][u8 value] into the word_FDF85 area
            uint16_t idx = c.u16();
            uint8_t val = c.u8();
            if (!c.ok) { abort_range(); break; }
            scenario_flags_[idx & 0xFF] = val;
            if (script_trace_) fprintf(stderr, "COMBAT: vm flag[%u] = %u\n", idx, val);
            break;
        }
        case VM_WEATHER: { // sub_527FF: [u8 weather] -> byte_BA84D + sub_44F30 (palette shade)
            uint8_t wv = c.u8();
            if (!c.ok) { abort_range(); break; }
            // Original: `cmp al, byte ptr dword_BA84A+3; jz loc_52834`, so an unchanged
            // value skips sub_44F30 entirely (no fade); otherwise the palette switches
            // immediately.
            VmEvent ev;
            ev.offset = op_pos;
            ev.type = VmEventType::Weather;
            ev.value = wv;
            r.events.push_back(ev);
            if (script_trace_) fprintf(stderr, "COMBAT: vm weather = %u\n", wv);
            break;
        }
        case VM_BGM:   // sub_52DA4: [u8] -> byte_FF0B8
        case VM_BGM2: { // sub_52D7D: [u8] -> byte_FF0B7
            uint8_t val = c.u8();
            if (!c.ok) { abort_range(); break; }
            if (script_trace_) fprintf(stderr, "COMBAT: vm %s = %u\n", cmd == VM_BGM ? "bgm" : "bgm/sfx", val);
            break;
        }
        case VM_EVT0E: // sub_5283D: dword_74134 = 0 (no payload)
            script_seq_ = 0;
            break;
        case VM_MORALE: { // sub_52757 (0x0D): [flag][i8 delta], adjusts one dword_F0BA0 word
            uint8_t which = c.u8();
            int8_t delta = (int8_t)c.u8();
            if (!c.ok) { abort_range(); break; }
            if (which == 0) morale_lo_ = (int16_t)(morale_lo_ + delta);
            else morale_hi_ = (int16_t)(morale_hi_ + delta);
            // The original always clamps dword_F0BA0+2 to 0x32..0x96 (loc_527A5/loc_527CD).
            if (morale_hi_ < 0x32) morale_hi_ = 0x32;
            if (morale_hi_ > 0x96) morale_hi_ = 0x96;
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm morale which=%u delta=%d -> lo=%d hi=%d\n", which, delta, morale_lo_, morale_hi_);
            break;
        }
        case VM_FACE: { // sub_52848 (0x66): [sel0][idx0][sel1][idx1] makes the pair face each other
            uint8_t s0 = c.u8(), i0 = c.u8(), s1 = c.u8(), i1 = c.u8();
            if (!c.ok) { abort_range(); break; }
            // Facing depends on the unit's current tile, so it must be computed at playback:
            // the original VM runs alongside the battle loop and the preceding walk
            // (0x03/0x05) has already finished. The port scans the script before playback,
            // and the parse-time tile is the spawn tile, which would give a different
            // facing (1-1A Ragnum ended up facing 0, south, at his pre-walk tile).
            VmEvent fe;
            fe.type = VmEventType::Face;
            fe.offset = op_pos;
            fe.face_refs[0] = s0; fe.face_refs[1] = i0;
            fe.face_refs[2] = s1; fe.face_refs[3] = i1;
            fe.forced_target_context = script_forced_target_;
            r.events.push_back(std::move(fe));
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm face (%u,%u)<->(%u,%u)\n", s0, i0, s1, i1);

            break;
        }
        case VM_ITEM: { // sub_5196F (0x06): [kind 0..3][item id], always 2 payload bytes
            uint8_t kind = c.u8();
            uint8_t item = c.u8();
            if (!c.ok) { abort_range(); break; }
            // sub_5196F resolves the receiver while the command runs: dword_11024C when a
            // unit is forced, otherwise the dword_CC968 list walk keeps the first node and
            // replaces it with probability 2/3 at every following node (sub_499BD(3) != 0).
            int target = script_forced_target_;
            if (target < 0 || (size_t)target >= units_.size()) {
                std::vector<int> list;
                for (size_t i = 0; i < units_.size(); ++i) {
                    const CombatUnitPlacement& lu = units_[i];
                    if (!lu.active) continue;
                    // dword_CC968 is the head of the army-1 list (word_F0424[0]); the walk at
                    // sub_5196F+0xB5 follows only that list, not the four army lists.
                    if (battle_effective_army(lu) != 1) continue;
                    list.push_back((int)i);
                }
                std::sort(list.begin(), list.end(), [&](int a, int b) {
                    const uint32_t sa = units_[(size_t)a].list_seq, sb = units_[(size_t)b].list_seq;
                    return sa != sb ? sa < sb : a < b;             // sub_1134F tail insert order
                });
                target = -1;
                if (!list.empty()) {
                    target = list[0];
                    for (size_t k = 1; k < list.size(); ++k)
                        if (combat_rand(3) != 0) target = list[k];
                }
            }
            VmEvent ev;
            ev.type = VmEventType::Item;
            ev.offset = op_pos;
            ev.item_kind = kind;
            ev.item_id = item;
            ev.item_unit = target;
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm item kind=%u id=%u unit=%d\n", kind, item, target);
            r.events.push_back(std::move(ev));
            break;
        }
        case VM_COMBAT: { // sub_51C74 (0x07): up to 2 x [sel][idx][mode] + separator 7
            // Original accounting (esi = cursor, cl = entry count):
            //   entry 0 = attacker (edi), entry 1 = target (dword_F0648[0],
            //   dword_F0B9C+2 = 1).
            //   No unit -> `lea eax,[esi+1]`, i.e. only the 3 bytes are consumed.
            //   Attacker mode > 1 -> esi++ then loc_51D4F (one extra byte, no target, pose
            //   is set). Target mode outside 2..0x32 -> loc_51D16: target count -1, esi -= 3.
            //   A separator other than 7 ends the command. loc_51D4F does `dec esi`, so the
            //   last byte stays unconsumed.
            uint32_t start = c.off;
            VmEvent ev; ev.offset = op_pos;
            int entries = 0;
            bool unit_missing = false;
            while (entries < 2) {
                if (!script_in_range(c.off, 3)) { abort_range(); break; }
                uint8_t sel = c.u8(), idx = c.u8(), mode = c.u8();
                int ui = find_unit_index(sel, idx);
                // loc_51C8F: after sel/idx (esi=+2) a missing unit does `lea eax,[esi+1]`,
                // consuming only the 3 bytes up to mode.
                if (ui < 0) { unit_missing = true; break; }
                if (entries == 0) {
                    ev.attacker_unit = ui; ev.attacker_mode = mode;
                    entries++;
                    if (mode > 1) { c.off += 1; break; }            // loc_51CE8: inc esi; jmp loc_51D4F
                } else {
                    entries++;
                    if (mode < 2 || mode > 0x32) {                   // loc_51D16
                        c.off -= 3; break;
                    }
                    VmAttackTarget t; t.unit_index = ui; t.mode = mode;
                    t.result = (mode == 2) ? 1 : (mode == 3) ? 2 : -1;   // loc_51D63 word_F0430[i]
                    ev.attack_targets.push_back(t);
                }
                if (!script_in_range(c.off, 1)) { abort_range(); break; }
                uint8_t term = c.u8();
                if (term != 7) break;                                // loc_51D2A: jnz loc_51D4F
            }
            if (r.aborted) break;
            if (!unit_missing) c.off -= 1;                           // loc_51D4F: dec esi
            if (!vm_position_plausible(c.off)) {
                c.off = start;
                r.aborted = true;
                r.abort_opcode = op;
                break;
            }
            if (unit_missing || ev.attacker_unit < 0) {
                if (script_trace_)
                    fprintf(stderr, "COMBAT: warning: vm event-combat: unit not found -> off 0x%X\n", c.off);
                break;
            }
            ev.type = (ev.attack_targets.empty() && ev.attacker_mode >= 2)
                          ? VmEventType::UnitPose                    // loc_51E34
                          : VmEventType::Attack;                     // loc_51DFE / loc_51E0F
            if (script_trace_) {
                const auto& a = units_[(size_t)ev.attacker_unit];
                fprintf(stderr, "COMBAT: vm event-combat attacker=unit %d (type %d, mode %d) targets=%zu",
                        a.id, a.sprite_type, ev.attacker_mode, ev.attack_targets.size());
                for (const auto& t : ev.attack_targets)
                    fprintf(stderr, " [unit %d mode %d -> %s]", units_[(size_t)t.unit_index].id, t.mode,
                            t.result == 1 ? "guard" : t.result == 2 ? "dodge" : "hit");
                fprintf(stderr, " -> off 0x%X\n", c.off);
            }
            r.events.push_back(std::move(ev));
            break;
        }
        case VM_MAGIC:   // sub_51E8B (0x08, edx=0): [sel][idx][spell][level], always 4 bytes
        case VM_MAGIC2: { // sub_51E8B (0x6C, edx=1)
            uint8_t sel = c.u8(), idx = c.u8(), spell = c.u8(), level = c.u8();
            if (!c.ok) { abort_range(); break; }
            // sub_51E8B: eax = stream, edx = mode (0 = 0x08, 1 = 0x6C). The payload is
            // always [sel][idx][spell][level]; the effect is applied at playback.
            VmEvent ev;
            ev.type = VmEventType::Magic;
            ev.offset = op_pos;
            ev.unit_ref_sel = sel; ev.unit_ref_id = idx;
            ev.value = (int)spell;          // var_C
            ev.army = (int)level;           // var_10
            ev.target_sub = (cmd == VM_MAGIC2) ? 1 : 0;   // edx of sub_51E8B
            r.events.push_back(std::move(ev));
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm magic%s (%u,%u) spell=%u level=%u -> event\n",
                        cmd == VM_MAGIC2 ? "2" : "", sel, idx, spell, level);
            break;
        }
        case VM_EVT09: { // sub_51FF7 (0x09): always [sel][idx][subcmd][1B]; only subcmd 5 reads the last byte
            uint8_t sel = c.u8(), idx = c.u8(), sub = c.u8();
            uint8_t extra = 0;
            if (sub == 5) extra = c.u8(); // extra read at loc_520E0
            else c.skip(1);                // loc_52115 inc ebp: consume the fourth byte
            if (!c.ok) { abort_range(); break; }
            // sub_51FF7: a missing unit does nothing. Subcommands 5/6 (clone/transform)
            // change the list, so they close the WC1 occupancy gate.
            const int ui = find_unit_index(sel, idx);
            if (sub == 5 || sub == 6) {
                VmEvent occupancy_event;
                occupancy_event.type = VmEventType::OccupancyStateUnknown;
                occupancy_event.offset = op_pos;
                occupancy_event.text = sub == 5 ? "VM 0x09 subcmd 5 transform/list insertion" :
                                                  "VM 0x09 subcmd 6 clone/list insertion";
                r.events.push_back(std::move(occupancy_event));
            }
            if (ui >= 0 && sub <= 6) {
                VmEvent ev; ev.type = VmEventType::UnitEffect; ev.offset = op_pos;
                ev.hp_unit = ui; ev.value = sub; ev.army = extra;
                r.events.push_back(std::move(ev));
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm evt09 (%u,%u) sub=%u extra=%u -> unit %d\n",
                        sel, idx, sub, extra, ui >= 0 ? units_[(size_t)ui].id : -1);
            break;
        }
        case VM_HP: { // sub_5211D (0x0A): [sel][idx][i16 delta][1B], always 5 bytes
            uint8_t sel = c.u8(), idx = c.u8();
            uint16_t raw = c.u16();
            uint8_t tail = c.u8();
            if (!c.ok) { abort_range(); break; }
            int16_t delta = (int16_t)raw;
            // sub_5211D: no unit -> nothing. delta == 0 -> sub_1DB1D (death animation and
            // removal), 0x7FFF -> heal to max (sub_462A7), > 0 -> heal, < 0 -> damage number
            // (sub_45BFB) plus knockback (sub_45ED2, direction = tail), and death when HP
            // drops to 0 within 25 frames. The port applies HP and effects in the same order
            // at playback (play_hp_change), matching the synchronous original handler.
            int ui = find_unit_index(sel, idx);
            if (ui >= 0) {
                VmEvent ev; ev.type = VmEventType::Hp; ev.offset = op_pos;
                ev.hp_unit = ui; ev.hp_delta = delta; ev.hp_dir = tail;
                r.events.push_back(std::move(ev));
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm hp (%u,%u) delta=%d dir=%u -> unit %d hp=%d%s\n",
                        sel, idx, delta, tail, ui >= 0 ? units_[(size_t)ui].id : -1,
                        ui >= 0 ? units_[(size_t)ui].hp : -1,
                        delta == 0 ? " (death)" : "");
            break;
        }
        case VM_AI: { // sub_522A7 (0x0B): [sel][idx][5B], always 7 bytes; only selector 0xFF skips +80/+82
            uint8_t sel = c.u8(), idx = c.u8();
            uint8_t body[5];
            if (!script_fetch(c.off, body, 5)) { abort_range(); break; }
            c.skip(5);
            // The VM collects events first, so ordinary selectors (0..9) are resolved at
            // playback; selectors 10..0x7F keep the dword_11024C context and re-check that
            // army then. A selector outside the ordinary/forced range is not ported (the
            // AI CC960 path is separate).
            if (sel <= 0x7F) {
                VmEvent ev; ev.type = VmEventType::AiSettings; ev.offset = op_pos;
                memcpy(ev.ai_bytes, body, sizeof(body));
                ev.unit_ref_sel = sel; ev.unit_ref_id = idx;
                ev.forced_target_context = script_forced_target_;
                r.events.push_back(ev);
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm ai (%u,%u) payload=(%u,%u,%u,%u,%u) -> %s%s\n",
                        sel, idx, body[0], body[1], body[2], body[3], body[4],
                        sel <= 9 ? "queued (list lookup deferred)" :
                        sel <= 0x7F ? "queued (forced-target context captured)" : "unresolved",
                        sel > 0x7F ? " (VM unit-ref selector outside ordinary/forced range)" : "");
            break;
        }
        case VM_FACTION: { // sub_52463 (0x0C): [sel][idx][action] plus action-specific payload
            uint8_t sel = c.u8(), idx = c.u8(), act = c.u8();
            if (!c.ok) { abort_range(); break; }
            // Payload length depends on whether the owner was found (loc_52505).
            CombatUnitPlacement* u = find_unit_ref_mut(sel, idx);
            // Consumption follows the jump table at loc_5250C, off_5244F =
            // {loc_5252E, loc_525C3, loc_5267C, loc_5270B}, with cursor H = opcode + 1:
            //   owner miss: action==1 -> H+0x8B, otherwise H+4 (loc_52505)
            //   owner hit : action 1/3 -> H+0x8B, 0/2 -> H+4, 4 -> H+5, >4 -> H+3
            // Action 0 reads the new army byte at H+3, actions 1/3 read an inline 0x88
            // record, and actions 2/4 read a type byte/word. Other visual effects are not
            // ported yet.
            uint32_t adv;
            if (u == nullptr) adv = (act == 1) ? 0x8B : 4;
            else if (act == 1 || act == 3) adv = 0x8B;
            else if (act == 4) adv = 5; // loc_5270B: `add edi, 2` (var_A0 = H+3 -> H+5)
            else if (act == 0 || act == 2) adv = 4;
            else adv = 3;
            if (!script_in_range(c.off, adv - 3)) { abort_range(); break; }
            VmEvent occupancy_event;
            occupancy_event.type = VmEventType::OccupancyStateUnknown;
            occupancy_event.offset = op_pos;
            occupancy_event.text = "VM faction/descriptor mutation";
            r.events.push_back(std::move(occupancy_event));
            if (act == 0 && sel <= 0x7F) {
                // loc_5252E: the next byte is the new army and the unit moves to that list.
                // The change must be applied in event order so later AI selectors see the
                // list as of that moment, so nothing is modified while parsing. Ordinary
                // selectors are resolved at playback; forced selectors keep the
                // dword_11024C context.
                VmEvent ev;
                ev.type = VmEventType::Faction;
                ev.offset = op_pos;
                ev.army = (int)script_buffer_[c.off];
                ev.unit_ref_sel = sel; ev.unit_ref_id = idx;
                ev.forced_target_context = script_forced_target_;
                r.events.push_back(ev);
            }
            // Actions 1/3 overwrite an inline 0x88 record on the target unit (1 = linked
            // unit, 3 = itself); actions 2/4 update type/slot (4 also refills HP). Nothing
            // is applied while parsing: these are queued as events to keep playback order.
            if (act >= 1 && act <= 4 && u != nullptr) {
                VmEvent ev;
                ev.type = VmEventType::FactionTransform;
                ev.offset = op_pos;
                ev.value = (int)act;
                ev.unit_ref_sel = sel; ev.unit_ref_id = idx;
                ev.forced_target_context = script_forced_target_;
                if (act == 1 || act == 3) {
                    const size_t base = (size_t)c.off;
                    ev.record.assign(script_buffer_.begin() + (long)base,
                                     script_buffer_.begin() + (long)base + kSceUnitRecSize);
                } else {
                    ev.record.assign(script_buffer_.begin() + (long)c.off,
                                     script_buffer_.begin() + (long)c.off + (adv - 3));
                }
                r.events.push_back(std::move(ev));
            }
            c.skip(adv - 3);
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm faction (%u,%u) action=%u -> +%u%s\n",
                        sel, idx, act, adv,
                        act == 0 && sel <= 0x7F ? " [legion change event]" :
                        (act >= 1 && act <= 4 && u != nullptr) ? " [transform event]" : " (no target: payload only consumed)");
            break;
        }
        case VM_TARGET: { // sub_52377 (0x6F): [sel][idx][sub][a][b], always 5 bytes
            uint8_t sel = c.u8(), idx = c.u8(), sub = c.u8(), a = c.u8(), b2 = c.u8();
            if (!c.ok) { abort_range(); break; }
            // Both the owner and the sub=2 target are resolved at playback for ordinary
            // list selectors. Forced selectors capture the dword_11024C context and check
            // the army byte as of that point.
            if (sel <= 0x7F && sub <= 2) {
                VmEvent ev; ev.type = VmEventType::AiTarget; ev.offset = op_pos;
                ev.unit_ref_sel = sel; ev.unit_ref_id = idx;
                ev.forced_target_context = script_forced_target_;
                ev.target_sub = sub; ev.target_a = a; ev.target_b = b2;
                r.events.push_back(ev);
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm target (%u,%u) sub=%u %u %u -> %s%s\n",
                        sel, idx, sub, a, b2,
                        sel <= 9 ? "queued (list lookup deferred)" :
                        sel <= 0x7F ? "queued (forced-target context captured)" : "unresolved",
                        sel > 0x7F ? " (VM unit-ref selector outside ordinary/forced range)" : "");
            break;
        }
        case VM_RETREAT: { // sub_51449 (0x04): [sel][idx][mode][b][b] records; continues while the next byte is 4
            uint32_t start = c.off;
            int ref_mode = -1;
            int guard = 0;
            bool stop = false;
            bool saw_record = false;
            VmEvent ev;
            ev.type = VmEventType::UnitLeave;
            ev.offset = op_pos;
            while (!stop && guard++ < 512) {
                if (!script_in_range(c.off, 6)) { abort_range(); break; }
                saw_record = true;
                const uint8_t* p = script_buffer_.data() + c.off;
                uint8_t sel = p[0], idx = p[1], mode = p[2];
                uint8_t step = p[3], dir = p[4];
                if (ref_mode < 0) { ref_mode = mode; ev.retreat_mode = mode; }
                else if (mode != ref_mode) { stop = true; break; } // loc_514D8 mode mismatch
                if (CombatUnitPlacement* u = find_unit_ref_mut(sel, idx)) {
                    ev.leave_units.push_back((int)(u - units_.data()));
                    // loc_514FA: `mov al,[var_8]; neg al; mov unk_E5A50[i], al`; the value is a
                    // signed byte.
                    ev.leave_steps.push_back((int)(int8_t)(uint8_t)(0u - step));
                    ev.leave_dirs.push_back((int)dir);
                }
                c.off += 5;
                uint8_t term = script_buffer_[c.off];
                if (term == 4) { c.off += 1; continue; } // loc_51671: 4 means another record follows
                stop = true;                              // the final separator is not consumed
            }
            if (r.aborted) break;
            if (!stop || !vm_position_plausible(c.off)) {
                c.off = start;
                r.aborted = true;
                r.abort_opcode = op;
                break;
            }
            // loc_5168C: with no unit (var_10 <= 0) there is neither animation nor removal.
            // Otherwise mode selects the path: 0 = sub_17071 (walk), 1 = sub_508E6(4),
            // anything else = sub_50AA7. The original erases the unit from the tile grid here
            // (sub_11EE4), but the port collects events before playback, so it applies the
            // removal at playback time.
            const size_t n_leave = ev.leave_units.size();
            if (saw_record) {
                VmEvent occupancy_event;
                occupancy_event.type = VmEventType::OccupancyStateUnknown;
                occupancy_event.offset = op_pos;
                occupancy_event.text = "VM retreat/removal";
                r.events.push_back(std::move(occupancy_event));
            }
            if (n_leave) r.events.push_back(std::move(ev));
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm retreat -> off 0x%X (mode=%d, %zu unit(s))\n",
                        c.off, ref_mode, n_leave);
            break;
        }
        case VM_MOVE: { // sub_516D0 (0x05): per record [sel][idx][group][load][count] + path
            // The first record's group becomes var_C for the whole command and later records
            // must match it (loc_5175A `cmp al,bl; jz`). loc_51950 branches on the group:
            // 0 -> sub_16D3E (path walk), otherwise sub_50BAF (two-point interpolation).
            // All records register with dword_F0B9C+2 as the record count and walk at once.
            uint32_t start = c.off;
            int ref_group = -1;
            int guard = 0;
            bool stop = false;
            bool saw_record = false;
            bool all_group0_records_supported = true;
            size_t parsed_record_count = 0;
            std::vector<int> move_unit_indices;
            VmEvent ev;
            ev.type = VmEventType::UnitMove;
            ev.offset = op_pos;
            while (!stop && guard++ < 512) {
                if (!script_in_range(c.off, 5)) { abort_range(); break; }
                uint8_t sel = script_buffer_[c.off + 0];
                uint8_t idx = script_buffer_[c.off + 1];
                uint8_t group = script_buffer_[c.off + 2]; // var_C / var_10 comparison byte
                uint8_t load = script_buffer_[c.off + 3];  // -> unk_E5AD0[record]
                uint8_t count = script_buffer_[c.off + 4]; // -> var_4 = number of path points
                if (ref_group < 0) {
                    ref_group = group;
                    ev.walk_group = group;
                    ev.walk_sets_start_tile = group == 0;
                } else if (group != ref_group) {
                    // The mismatched record's selector is resolved and occupancy cleared before ASM
                    // stops the batch.
                    all_group0_records_supported = false;
                    stop = true;
                    break;
                } // loc_5175A: records whose group differs are left out of the animation list
                c.off += 5;
                // loc_5177F..loc_51827: with group == 0 the first 2 bytes are the unit's x/y
                // and the remaining (count-1)*2 bytes are the path array (unk_E5210), so
                // count*2 bytes in total. With group != 0, var_4 is fixed at 2 and only one
                // destination (2 bytes) is read.
                uint32_t points = (uint32_t)(count ? count : 1);
                uint32_t extra = (group == 0) ? points * 2u : 2u;
                if (!script_in_range(c.off, extra + 1)) { abort_range(); break; }
                saw_record = true;
                ++parsed_record_count;
                int move_idx = find_unit_index(sel, idx);
                VmWalkUnit wu;
                wu.unit_index = move_idx;
                wu.load = load;
                // For group 0 the first path point is the start tile. State is updated when
                // the event plays: changing coordinates in the parser would apply the move
                // before earlier VM events.
                for (uint32_t k = 0; k + 1 < extra; k += 2)
                    wu.path.push_back({ script_buffer_[c.off + k], script_buffer_[c.off + k + 1] });
                // word_EDD74 = max(var_8) over the records: loc_5180F uses
                // var_8 = (count-1)+2 = count+1 and loc_5181D (group != 0) uses var_8 = var_4 = 2.
                // loc_51877 compares with `cmp ax, word_EDD74; jle`, so the maximum survives.
                int frames = (group == 0) ? (int)points + 1 : 2;
                if (frames > ev.walk_frames) ev.walk_frames = frames;
                if (group != 0 || sel < 1 || sel > 3 || count == 0 || points > 31)
                    all_group0_records_supported = false;
                if (move_idx >= 0) {
                    if (sel >= 1 && sel <= 3) {
                        int exact_list_index = -1;
                        size_t exact_list_matches = 0;
                        for (size_t candidate = 0; candidate < units_.size(); ++candidate) {
                            const CombatUnitPlacement& listed = units_[candidate];
                            if (!listed.active || !listed.battle_list_membership_modeled ||
                                listed.group < 1 || listed.group > 3 ||
                                battle_effective_army(listed) != (int)sel + 1 || listed.id != (int)idx)
                                continue;
                            exact_list_index = (int)candidate;
                            ++exact_list_matches;
                        }
                        if (exact_list_matches != 1 || exact_list_index != move_idx)
                            all_group0_records_supported = false;
                    } else {
                        all_group0_records_supported = false;
                    }
                    if (std::find(move_unit_indices.begin(), move_unit_indices.end(), move_idx) != move_unit_indices.end())
                        all_group0_records_supported = false;
                    move_unit_indices.push_back(move_idx);
                    ev.walk_units.push_back(std::move(wu));
                } else {
                    all_group0_records_supported = false;
                }
                c.off += extra;
                uint8_t term = script_buffer_[c.off];
                if (term == 5) { c.off += 1; continue; } // loc_51912: 5 means another record follows
                stop = true;                             // loc_5192F: final byte stays unconsumed
            }
            if (r.aborted) break;
            if (!stop || !vm_position_plausible(c.off)) {
                c.off = start;
                r.aborted = true;
                r.abort_opcode = op;
                break;
            }
            // Only a complete group-0 batch with uniquely modeled ordinary-list references can try
            // the runtime path/occupancy proof. Other valid VM 0x05 records retain fail-closed
            // behavior.
            ev.walk_occupancy_preserving_candidate = saw_record && stop && ref_group == 0 &&
                all_group0_records_supported && parsed_record_count != 0 &&
                parsed_record_count == ev.walk_units.size();
            if (saw_record && !ev.walk_occupancy_preserving_candidate) {
                VmEvent occupancy_event;
                occupancy_event.type = VmEventType::OccupancyStateUnknown;
                occupancy_event.offset = op_pos;
                occupancy_event.text = "VM 0x05 scripted movement";
                r.events.push_back(std::move(occupancy_event));
            }
            // loc_5192F: `test ah,ah; jle`; with no resolved record unit no animation is played.
            if (ev.walk_units.empty()) {
                if (script_trace_)
                    fprintf(stderr, "COMBAT: vm move -> off 0x%X (start 0x%X, group=%d) no unit%s\n",
                            c.off, start, ref_group, saw_record ? " (occupancy gate closed)" : "");
                break;
            }
            r.events.push_back(std::move(ev));
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm move -> off 0x%X (start 0x%X, group=%d, EDD74=%d)\n",
                        c.off, start, ref_group, r.events.back().walk_frames);
            break;
        }
        case VM_TILE: { // sub_52C06 (0x10): [b0][b1][b2][u16][b4]; continues while the next byte is 0x10
            uint32_t start = c.off;
            int guard = 0;
            bool more = true;
            while (more && guard++ < 512) {
                if (!script_in_range(c.off, 7)) { abort_range(); break; }
                const uint8_t* p = script_buffer_.data() + c.off;
                // Original: [b0] is the layer, word_78A04/word_83DB4 tile maps are updated
                // here, and [b4]+[u16] drives sub_10957 to change the terrain buffer (word_6F7xx).
                (void)p;
                c.off += 6;
                uint8_t term = c.u8();
                more = (term == 0x10);
                if (!more) c.off -= 1; // final separator stays unconsumed
            }
            if (!c.ok || !vm_position_plausible(c.off)) {
                c.off = start;
                r.aborted = true;
                r.abort_opcode = op;
                break;
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm tile change -> off 0x%X\n", c.off);
            break;
        }
        case VM_SCENARIO: { // sub_52D33: [16-byte scenario name], then switch to the next scenario
            uint8_t buf[16];
            if (!script_fetch(c.off, buf, 16)) { abort_range(); break; }
            c.skip(16);
            std::string name;
            for (int i = 0; i < 16 && buf[i]; ++i) name.push_back((char)buf[i]);
            r.next_scenario = name;
            {
                VmEvent ev;
                ev.type = VmEventType::NextScenario;
                ev.offset = op_pos;
                ev.text = name;
                r.events.push_back(ev);
            }
            // This is the only handler whose original dispatch returns through
            // `jmp loc_52F01` instead of `jmp loc_52EFF` (esi = eax), so esi is not updated.
            // sub_52D33 loads the next scenario through sub_4FD6B, leaving this one, so the
            // current script walk ends here.
            r.ended = true;
            r.end_opcode = 0x12;
            if (script_trace_) fprintf(stderr, "COMBAT: vm next scenario = '%s'\n", name.c_str());
            break;
        }
        case VM_SPAWN: { // sub_50D9F (0x03): reinforcement entry, variable length (repeating groups)
            // Layout (listing 87689-87888 plus 1-1A.SCE 0x2A):
            //   H+0      K        repeat marker; 0 means tile rows follow
            //   H+1      count1   group unit count; unk_E5A50[idx] = -count1
            //   H+2      0x88 unit record (x, y, name, army, ID, ...)
            //   +0x88    0x88 link record, only when the preceding record [0x5C] != 0
            //   next 1B  load     -> unk_E5AD0[idx]
            //   next 1B  count2   number of tile rows
            //   next 1B  x, next 1B y, overwriting the record's x/y
            //   K==0: 2*(count2-1) bytes of tile rows
            //   last 1B  separator: 3 repeats a new group, anything else ends the walk and
            //            this byte is pushed back
            // 1-1A.SCE: K=0, count1=0, unit (22,7), load=0, count2=0x0D, override (12,4),
            // 24 bytes of tile rows, separator 0x02, so the next opcode is dialogue (0x02).
            uint32_t cur = c.off;
            const size_t units_before_spawn = units_.size();
            const size_t script_size = script_buffer_.size();
            if ((size_t)cur >= script_size) { abort_range(); break; }
            const uint8_t K = script_buffer_[cur];
            bool done = false;
            int spawned = 0;
            // sub_50D9F+41C registers every unit in the move queue through
            // sub_5069C(unit, dl=self, cl=load, bl=code_b+89h, arg=load offset). At the end,
            // K==0 runs sub_16D3E and K==1 runs the sub_508E6(3) drop animation synchronously
            // before the VM continues. The port keeps both paths in the Spawn event so they
            // play in command order, and the units stay inactive/hidden until then.
            VmEvent sev;
            sev.type = VmEventType::Spawn;
            sev.offset = op_pos;
            uint32_t load_off = 0;
            while (!done) {
                if ((size_t)cur > script_size || script_size - (size_t)cur < 2) { abort_range(); break; }
                uint8_t count1 = script_buffer_[cur + 1];
                (void)count1;
                if (script_size - (size_t)cur < 2 + kSceUnitRecSize) { abort_range(); break; }
                const uint8_t* rec = script_buffer_.data() + cur + 2;
                CombatUnitPlacement nu;
                nu.x = rec[0]; nu.y = rec[1];
                nu.id = rec[0x0F];
                nu.army = (int)rec[0x0E]; // raw Spawn payload; effective army is byte-incremented when nonzero, remapped for raw zero.
                // The type code is the u16 at +0x61 (same slot as loc_4F02F / sub_12567).
                nu.sprite_type = (int)rec[0x61] | ((int)rec[0x62] << 8);
                nu.slot_hint   = rec[0x60];
                nu.face     = rec[0x10]; // portrait from [+10h] (bx of sub_482A5 -> sub_48132)
                for (int ci = 2; ci < 0x10 && rec[ci]; ++ci) nu.name.push_back((char)rec[ci]);
                nu.sprite_id = (uint8_t)(nu.sprite_type % 222);
                nu.team = (nu.army == 0) ? 0 : 1;
                nu.code_b = rec[0x5C];
                // Must mirror init_dummy_units (static groups 0..3). Without these fields
                // reinforcement units (every one enters through this path, e.g. the dark
                // assassins of 1-5.SCE) kept combat_class 0, so weapon_class() returned the
                // wrong value and the P6 AI dispatch never ran.
                nu.code_a = rec[0x3F];
                nu.combat_class = rec[0x11];
                nu.combat_flags_14 = rec[0x14];
                nu.combat_flags_15 = rec[0x15];
                nu.combat_flags_1e = rec[0x1E];
                nu.auto_battle = rec[0x83];
                nu.ai_decision_state = rec[0x6C];
                nu.ai_field_6d = rec[0x6D]; nu.ai_field_6e = rec[0x6E];
                nu.ai_home_x = rec[0x6F]; nu.ai_home_y = rec[0x70];
                nu.ai_watch_sel = rec[0x71]; nu.ai_watch_id = rec[0x72];
                nu.ai_field_80 = rec[0x80];
                nu.ai_mode = (int8_t)rec[0x82];
                // sub_50D9F does not run sub_4ED18's [+73h] tile-to-pointer normalization loop,
                // so an inline record's [+73h] is not a pointer or units_ index. When
                // sub_4E81F runs for raw army 0 the effective army and CC990 membership can
                // change too, so memory_army stays -1 to force the raw+1 fallback.
                nu.facing = (int8_t)(rec[0x63] & 3);
                if (rec[0x5C] != 0 &&
                    script_size - (size_t)cur >= 2 + kSceUnitRecSize + kSceUnitRecSize + 0x12)
                    nu.linked_class = (int)rec[kSceUnitRecSize + 0x11]; // sub_174CF: [[+77h]+11h], valid only with a link record
                apply_record_hp(nu, rec);         // [+0x48]/[+0x4C] = HP x100
                nu.group = 3; // Synthetic scheduler group; this is not an original CC968 slot.
                // sub_50D9F loc_50E71..50EDD: with raw army 0, sub_500ED(ID) looks up a CC990
                // roster record. When found, sub_50105(spawn, roster) copies the roster's 0x88
                // bytes over the spawn record, restores only coordinates and facing, and sets
                // [48h]=[4Ch], [50h]=[52h] (HP/MP to full). Without a record, sub_4E81F runs.
                // The port has no CC990 roster list, so it approximates the candidate as an
                // allied unit (effective army 1, not a summon) with the same ID. The
                // [5Ch] != 0 link path keeps the existing linked_class projection.
                const bool raw_zero = (nu.army == 0);
                if (raw_zero) {
                    // With no CC990 record, sub_4E81F re-reads the (ID-1)*0x88+0x4E26 record
                    // from BLACKBOX.DAT, overwrites the fields, and remaps the army through
                    // [87h] -> word_FDAF6.
                    if (nu.code_b != 0) nu.stored_77 = std::make_shared<CombatUnitPlacement>(nu);
                    CombatUnitPlacement* roster = battle_roster_find_500ed(nu.id);
                    if (roster) {
                        battle_roster_apply_50105(nu, *roster);
                        // The original does roster [0Eh] (=1) -> dec -> 0 -> inc (=1), which is
                        // raw 0 / effective 1 under the port's raw convention.
                        nu.army = 0;
                        nu.memory_army = 1;
                        if (script_trace_)
                            fprintf(stderr, "COMBAT: spawn raw-army0 id=%d -> CC990 roster (sub_50105)\n", nu.id);
                    } else {
                        // sub_4E81F: reload BLACKBOX and remap [87h]. Name, class, HP/MP, level
                        // and equipment are taken from the persistent sheet; position, facing
                        // and AI fields are kept.
                        battle_reload_record_4e81f(nu);
                        if (script_trace_)
                            fprintf(stderr, "COMBAT: spawn raw-army0 id=%d -> no CC990 record, sub_4E81F reload (army=%d)\n",
                                    nu.id, nu.memory_army);
                    }
                }
                // sub_50D9F increments nonzero raw army bytes directly. Raw zero may remap/copy via
                // sub_4E81F/CC990.
                if (nu.memory_army < 0)
                    nu.memory_army = nu.army != 0 ? (int)battle_incremented_raw_army(nu.army) : -1;
                // sub_50D9F loc_50EFF..loc_50F25: records that started with army 0 are copied
                // back to the front of the CC990 list after processing. When [+77h] holds a
                // link, the link (base form) record is used as the copy.
                if (raw_zero) {
                    battle_roster_push_cc990(nu.stored_77 ? *nu.stored_77 : nu);
                    if (script_trace_)
                        fprintf(stderr, "COMBAT: spawn id=%d -> CC990 insert (%zu records)\n",
                                nu.id, battle_roster_cc990_.size());
                }
                // A single unlinked record whose raw army is nonzero and effective 1..4 (the
                // assassins in 1-5, for example) follows the same byte-INC path as a static
                // outer 1..3, so it is treated as a member of the 1x1 uniqueness gate. Raw
                // zero/remap, linked and large records are not modelled and stay false.
                nu.battle_list_membership_modeled =
                    nu.army != 0 && nu.memory_army >= 1 && nu.memory_army <= 4 &&
                    nu.linked_class < 0 && rec[0x5C] == 0;
                // run_vm parses events ahead of playback, so the unit record is staged here and
                // kept out of the battle list/map until its 0x03 spawn command replays, as in
                // the original.
                nu.active = false;
                // [64h] initial pose, from the activation rule (loc_16D78 / loc_17096):
                // code_b 0Fh..16h -> 2, otherwise -> 3 (idle frame without a weapon).
                nu.anim_frame = (nu.code_b >= 0x0F && nu.code_b <= 0x16) ? 2 : 3;
                size_t p = (size_t)cur + 2 + kSceUnitRecSize;
                if (rec[0x5C] != 0) { // linked unit record
                    if (script_size - p < kSceUnitRecSize) { abort_range(); break; }
                    p += kSceUnitRecSize;
                }
                if (p > script_size || script_size - p < 4) { abort_range(); break; }
                load_off = (uint32_t)p;           // sub_5069C arg_0: stream offset of the load byte
                p += 1;                           // load -> unk_E5AD0
                uint8_t count2 = script_buffer_[p]; p += 1;
                nu.x = script_buffer_[p]; p += 1;
                nu.y = script_buffer_[p]; p += 1;
                // sub_50D9F -> sub_50D3A: a K==1 drop does not keep the payload position. It
                // scans a diamond r=1..0x31 in sub_1AC08/sub_1AC59 order and takes the first
                // cell whose sub_13CBD value is -1. At this point the original's shared E8 grid
                // is in its cleared (-1) state (the alternative is that north of the payload
                // would not be the position measured on the 1-5 saves), so the port reproduces
                // the scan against an all -1 grid. sub_13CBD returns -128 outside the map, so
                // those cells are skipped exactly as in the original; this also keeps a spawn
                // on the top row on its own tile instead of decrementing y out of bounds.
                if (K == 1) {
                    std::vector<int> empty_e8((size_t)map_w_ * (size_t)map_h_, -1);
                    bool drop_found = false;
                    for (int r = 1; r < 0x32 && !drop_found; ++r) {
                        Wc5Dia dia;
                        battle_ai_wc5_dia_init(dia, nu.x, nu.y, r);
                        int dx = 0, dy = 0;
                        while (battle_ai_wc5_dia_next(dia, dx, dy)) {
                            if (battle_ai_wc5_diff_read(empty_e8, dx, dy) != -1) continue;
                            nu.x = dx;
                            nu.y = dy;
                            drop_found = true;
                            break;
                        }
                    }
                }
                const size_t path_bytes = (K == 0 && count2 >= 1)
                    ? (size_t)(count2 - 1) * 2u : 0u;
                if (path_bytes > script_size - p) { abort_range(); break; }
                p += path_bytes; // tile row
                if (p >= script_size) { abort_range(); break; }
                uint8_t term = script_buffer_[p]; p += 1;
                int spawn_idx = -1;
                if (nu.x < (int)map_w_ && nu.y < (int)map_h_) {
                    units_.push_back(nu);
                    spawn_idx = (int)units_.size() - 1;
                    sev.spawn_units.push_back(spawn_idx);
                    ++spawned;
                    if (script_trace_)
                        fprintf(stderr, "COMBAT: vm spawn unit id=%d army=%d pos=(%d,%d) type=%d(%s) class=%u weapon_class=%d\n",
                                nu.id, nu.army, nu.x, nu.y, nu.sprite_type,
                                char_file_name_for_type(nu.sprite_type),
                                (unsigned)nu.combat_class, weapon_class(nu));
                }
                if (spawn_idx >= 0 && K == 0 && count2 >= 2) {
                    // sub_51211: count2 - (-count1) + 1 = path points + 1. As in the group 0
                    // convention, var_8 = points+1 and word_EDD74 = max -> walk_frames = points+1.
                    VmWalkUnit wu;
                    wu.unit_index = spawn_idx;
                    wu.load = script_buffer_[load_off];
                    const uint32_t pts = (uint32_t)(count2 - 1);
                    for (uint32_t k = 0; k < pts; ++k) {
                        const size_t q = (size_t)load_off + 4u + (size_t)k * 2u;
                        if (q > script_size || script_size - q < 2) break;
                        wu.path.push_back({ script_buffer_[q], script_buffer_[q + 1] });
                    }
                    if (!wu.path.empty()) {
                        sev.walk_units.push_back(std::move(wu));
                        if ((int)(sev.walk_frames) < (int)pts + 1)
                            sev.walk_frames = (int)pts + 1;
                    }
                }
                if (spawn_idx >= 0 && K == 1) {
                    // Mode 1: appear on the placement tile with no tile path (loc_51035
                    // branch, sub_508E6(3) drop). Only the unit index and load go into the
                    // event. count1 is the appearance delay in even frames: E5A50 = -count1,
                    // and the unit lands at 8.
                    VmWalkUnit wu;
                    wu.unit_index = spawn_idx;
                    wu.load = script_buffer_[load_off];
                    wu.drop = 1;
                    wu.spawn_delay = (int)count1;
                    sev.walk_units.push_back(std::move(wu));
                }
                if (term == 3) { cur = p; continue; }   // loc_50DD4
                // loc_511DE: after `dec ebp` the separator byte is pushed back and reused as
                // the next opcode
                cur = p - 1;
                done = true;
            }
            if (r.aborted) {
                // This parser stages pending Spawn units in units_. Roll back the whole command if
                // a later record/payload check fails.
                units_.resize(units_before_spawn);
                break;
            }
            c.off = cur;
            // At the end of sub_50D9F the original calls sub_16D3E for K==0 or sub_508E6(3)
            // for K==1 synchronously. The port stores a Spawn event and replays the walk/drop
            // at the same VM command position.
            const size_t spawned_walk = sev.walk_units.size();
            sev.spawn_mode = (int)K;      // sub_50D9F var_10: 0 = walk queue, 1 = drop animation
            VmEvent occupancy_event;
            occupancy_event.type = VmEventType::OccupancyStateUnknown;
            occupancy_event.offset = op_pos;
            occupancy_event.text = "VM 0x03 spawn/list insertion";
            r.events.push_back(std::move(occupancy_event));
            if (!sev.spawn_units.empty()) r.events.push_back(std::move(sev));
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm spawn -> %d unit(s), off 0x%X mode=%d walk=%zu\n", spawned, cur,
                        (int)K, spawned_walk);
            break;
        }
        case VM_EVT01: { // sub_506F3: stage clear
            // Original order:
            //   1) sub_53F2C(0): if the id=9 record in the slot[8] table passes its condition,
            //      run the victory cutscene first through sub_52E55.
            //   2) sub_44CCA(0) when dword_11209E == 1: screen fade-out.
            //   3) unit/stat cleanup (sub_4E4C4(2)/4E622/4EC51/4ECD7), sub_4EC9A(1) writes
            //      BLACKBOX.DAT, `unlink contmap.dat`, " " goes to `$`, then sub_61CFF exits.
            r.ended = true;
            r.end_opcode = 0x01;
            if (victory_fired_) break;                 // one-shot: the process ends, so the original never re-enters
            victory_fired_ = true;
            VmResult vic;
            bool fired = run_victory_script(0, vic);
            if (script_trace_)
                fprintf(stderr, "COMBAT: vm stage-clear (0x01): victory script fired=%d events=%zu lines=%zu\n",
                        fired ? 1 : 0, vic.events.size(), vic.lines.size());
            else if (fired)
                fprintf(stderr, "COMBAT: vm stage-clear (0x01): victory script %zu event(s), %zu line(s)\n",
                        vic.events.size(), vic.lines.size());
            if (fired) {
                r.events.insert(r.events.end(), vic.events.begin(), vic.events.end());
            }
            // sub_44CCA(0): fade out -> unlink contmap.dat -> write " " to `$` -> exit.
            // 0x01 always ends the module, even when the table is empty or the condition fails.
            {
                VmEvent ev;
                ev.type = VmEventType::Victory;
                ev.offset = op_pos;
                r.events.push_back(ev);
            }
            break;
        }
        case VM_CHOICE: // sub_50751: no payload. Waits for input, then returns normally; the original keeps walking too.
            r.choice = true;
            if (script_trace_) fprintf(stderr, "COMBAT: vm choice/branch (0x65, payload 0)\n");
            break;
        default: // battle-only compound command: payload size unknown
            r.aborted = true;
            r.abort_opcode = op;
            break;
        }
        if (r.aborted || r.ended) break;
    }
    r.end_offset = c.ok ? c.off : 0xFFFFFFFFu;
    if (r.aborted) {
        if (r.range_error)
            fprintf(stderr, "COMBAT: vm @0x%X op=0x%02X aborted (payload outside body range 0x%zX)\n",
                    op_pos, op, script_buffer_.size());
        else if (r.abort_opcode)
            fprintf(stderr, "COMBAT: vm @0x%X op=0x%02X aborted (payload size unresolved, not ported)\n", op_pos, op);
        else
            fprintf(stderr, "COMBAT: warning: vm @0x%X : past the end of the script (corrupt/unmatched offset)\n", op_pos);
    }
    return r;
}

// Fallback path for scenarios whose trigger tables are empty (1-1.SCE, for example).
// Walks the body and runs every 0x00-separated block that starts with a display or
// presentation command through the VM: the bytecode is executed instead of pattern-matching
// dialogue strings.
int CombatII::scan_script_blocks(std::vector<CombatDialogueEntry>& out) {
    if (!script_loaded_) return 0;
    int blocks = 0;
    uint32_t consumed = 0; // bytes already parsed are not used as a start point again
    auto parse_vm_for_display_only = [this](uint32_t offset) {
        const uint32_t battle_time_before = battle_time_;
        const int32_t script_seq_before = script_seq_;
        const int16_t morale_lo_before = morale_lo_, morale_hi_before = morale_hi_;
        const int forced_target_before = script_forced_target_;
        const bool victory_fired_before = victory_fired_;
        const bool occupancy_known_before = battle_wc1_occupancy_state_known_;
        const size_t units_before = units_.size();
        std::array<uint8_t, sizeof(scenario_flags_)> scenario_flags_before;
        memcpy(scenario_flags_before.data(), scenario_flags_, sizeof(scenario_flags_));

        VmResult parsed = run_vm(offset);
        if (units_.size() > units_before) units_.resize(units_before);
        battle_time_ = battle_time_before;
        script_seq_ = script_seq_before;
        morale_lo_ = morale_lo_before;
        morale_hi_ = morale_hi_before;
        script_forced_target_ = forced_target_before;
        victory_fired_ = victory_fired_before;
        battle_wc1_occupancy_state_known_ = occupancy_known_before && battle_sce_snapshot_complete_;
        memcpy(scenario_flags_, scenario_flags_before.data(), sizeof(scenario_flags_));
        return parsed;
    };
    for (uint32_t s = 0; s < script_buffer_.size(); ++s) {
        if (s < consumed) continue;
        if (s != 0 && script_buffer_[s - 1] != 0x00) continue; // block separator
        // Leftover bytes after a consumed block (run_vm does not consume the trailing NUL,
        // so the next block head still passes the test) also become start points. That is fine.
        uint8_t op = script_buffer_[s];
        if (op == 0x00) continue;
        // Dialogue mis-detection guard: an 0x02 payload is sel/idx/len and idx must reference a
        // valid unit. A Johab lead byte (>= 0x0A) in the len position means this is text data,
        // not a command (measured at 2-6 @0x7BC: `02 09 03 21` sits inside the "48 00\n" text
        // and sel/idx/len would be 0x09, 0x03, 0x210A - clearly wrong).
        if (op == 0x02) {
            uint8_t d_sel = (s + 1 < script_buffer_.size()) ? script_buffer_[s + 1] : 0;
            uint8_t d_idx = (s + 2 < script_buffer_.size()) ? script_buffer_[s + 2] : 0;
            uint8_t d_len0 = (s + 3 < script_buffer_.size()) ? script_buffer_[s + 3] : 0;
            uint8_t d_len1 = (s + 4 < script_buffer_.size()) ? script_buffer_[s + 4] : 0;
            (void)d_sel;
            uint16_t d_len = (uint16_t)(d_len0 | (d_len1 << 8));
            bool idx_bad  = (d_idx == 0 || d_idx >= 0x80);           // unit ID / legion index out of range
            bool len_over = (d_len == 0 || d_len > 0x400 ||
                             !script_in_range((uint32_t)s + 5, d_len)); // implausible text length
            if (idx_bad && len_over) {
                if (DevUtil::script_trace())
                    fprintf(stderr, "COMBAT: block scan skip @0x%X (0x02 misdecode guard: idx=%u len=%u)\n",
                            s, d_idx, d_len);
                continue;
            }
        }
        VmCmd cmd = vm_opcode_to_cmd(op);
        if ((int)cmd < 0) continue;
        // Blocks that start with a battle command (reinforcement, retreat, ...) are not cutscenes.
        switch (cmd) {
        case VM_DIALOGUE: case VM_MESSAGE: case VM_CAMERA: case VM_FLAG:
        case VM_WEATHER: case VM_BGM: case VM_BGM2: case VM_SCENARIO:
        case VM_CHOICE: case VM_EVT01: case VM_EVT0E:
            break;
        default:
            continue;
        }
        VmResult r = parse_vm_for_display_only(s);
        if (r.lines.empty()) continue;
        blocks++;
        uint32_t end = (r.end_offset != 0xFFFFFFFFu && r.end_offset < script_buffer_.size()) ? r.end_offset : script_buffer_.size();
        consumed = end;
        if (r.aborted)
            fprintf(stderr, "COMBAT: block @0x%X -> %zu lines (ops=%d, stopped at unported op 0x%02X, next=0x%X)\n",
                    s, r.lines.size(), r.instructions, (unsigned)r.abort_opcode, end);
        else
            fprintf(stderr, "COMBAT: block @0x%X -> %zu lines (ops=%d, terminating opcode 0x00, next=0x%X)\n",
                    s, r.lines.size(), r.instructions, end);
        for (auto& e : r.lines) out.push_back(e);
    }
    return blocks;
}

// Common table walk for sub_53523 / sub_5358F.
// Record: [rec+0 u16 (skipped by the worker)][rec+2 u16 id][rec+4 u16 id high word]
//         [rec+6 u16 size and end marker][rec+8 condition block + bytecode]
// Next record = rec + 8 + size.
// The original compares the dword at rec+2 (id | high_word << 16) with the time/sequence
// through `cmp esi,[var_8]`; the skip amount and the end test use the word at rec+6
// (`mov eax,[esp+8+var_8+2]; sar eax,10h` = the word at esp+4), and the high word must be 0.
// Measured: 1-2.SCE has a second record with time=20 at 0x48B, and 1-4/1-5 place the end
// record (FA 00 FA 00 00 00 00 00) exactly at rec+8+len.
bool CombatII::walk_trigger_table(uint32_t base, uint32_t match_id, uint8_t* fire_counter,
                                  bool continue_on_false, VmResult& out) {
    if (!script_loaded_) return false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    // sub_5358F's esi is incremented (`inc esi`, loc_53610) for every record whose id does not
    // match. When a matching record's condition is true, `inc byte ptr unk_11044C[esi]` fires,
    // so the counter is the record index within the table. sub_53523 (time) has no counter
    // (fire_counter == nullptr).
    int rec_index = 0;
    int guard = 0;
    while (c.ok && guard++ < 512) {
        uint32_t rec = c.off;
        c.skip(2); // the worker does `add ecx, 2` at the loop head
        uint16_t id = c.u16();
        uint16_t id_hi = c.u16();   // rec+4: high word of the dword compare (0 to match)
        uint16_t size = c.u16();    // rec+6: record size and end marker (0 = end of table)
        if (!c.ok) break;
        if (script_trace_)
            fprintf(stderr, "COMBAT: trigger rec @0x%X id=%u hi=%u size=%u\n", rec, id, id_hi, size);
        if (size == 0) break; // end of table
        if (id == match_id && id_hi == 0) {
            bool ok = false;
            uint32_t next = eval_condition(c.off, ok);
            if (ok && next != 0) {
                if (fire_counter && rec_index < 16) fire_counter[rec_index]++; // inc byte ptr unk_11044C[esi]
                out = run_vm(next);
                return true;
            }
            // Condition false: sub_53523 (time) jumps to loc_53584 and continues with the
            // next record, while sub_5358F (sequence) leaves through loc_5236F and ends the table.
            if (!continue_on_false) break;
        } else {
            rec_index++;   // loc_53610: inc esi (sequence worker only; the time worker never uses esi)
        }
        c.off = rec + 8 + size;
    }
    return false;
}

// sub_53523: test the time trigger table at body offset 0 against dword_F0418 (= battle_time_).
bool CombatII::run_time_trigger(uint32_t time, VmResult& out) {
    return walk_trigger_table(0, time, /*fire_counter=*/nullptr, /*continue_on_false=*/true, out);
}

// sub_5358F: test slot 0's sequence table against dword_74134 (= script_seq_).
bool CombatII::run_seq_trigger(uint32_t seq, VmResult& out) {
    return walk_trigger_table(script_table_base(1), seq, script_fired_, /*continue_on_false=*/false, out);
}

bool CombatII::run_area_trigger(const CombatUnitPlacement& moved_unit, VmResult& out) {
    out = VmResult();
    if (!script_loaded_) return false;
    const int moved_unit_index = (int)(&moved_unit - units_.data());
    const int moved_unit_id = moved_unit.id;
    const int moved_x = moved_unit.x, moved_y = moved_unit.y;
    const int saved_target = script_forced_target_;
    script_forced_target_ = -1;
    const uint32_t base = script_table_base(2);   // sub_53512(0, 2)
    if (base >= script_buffer_.size()) {
        script_forced_target_ = saved_target;
        return false;
    }

    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    for (int rec_index = 0; c.ok && rec_index < 100; ++rec_index) {
        const uint32_t rec = c.off;
        c.skip(2);   // the worker does `add ecx, 2` at the record head
        const uint8_t x1 = c.u8(), y1 = c.u8(), x2 = c.u8(), y2 = c.u8();
        const uint8_t selector = c.u8(), index = c.u8();
        const uint16_t payload_size = c.u16();
        if (!c.ok || payload_size == 0) break;
        const uint32_t payload = c.off;
        if (!script_in_range(payload, payload_size)) break;

        if (area_trigger_fired_[rec_index] != 0) {
            c.off = payload + payload_size;
            continue;
        }

        const CombatUnitPlacement* watched = nullptr;
        if (selector < 10) {
            watched = find_unit_ref(selector, index);
        } else if (battle_effective_army(moved_unit) == (int)selector - 9) { // Original compares effective BattleUnit [+0x0E]; remap fallback is approximate.
            watched = &moved_unit;
        }
        if (watched != &moved_unit || moved_unit.x < x1 || moved_unit.x > x2 ||
            moved_unit.y < y1 || moved_unit.y > y2) {
            c.off = payload + payload_size;
            continue;
        }

        script_forced_target_ = (selector >= 10) ? moved_unit_index : -1;
        bool satisfied = false;
        const uint32_t vm_offset = eval_condition(payload, satisfied);
        if (!satisfied || vm_offset == 0) {
            if (battle_trace_)
                fprintf(stderr, "COMBAT: warning: area trigger rec=%d unit=%d condition failed\n", rec_index, moved_unit_id);
            script_forced_target_ = saved_target;
            return false;
        }
        area_trigger_fired_[rec_index]++;
        out = run_vm(vm_offset);
        if (battle_trace_)
            fprintf(stderr, "COMBAT: area trigger rec=%d @0x%X unit=%d pos=(%d,%d) rect=(%u,%u)-(%u,%u) events=%zu\n",
                    rec_index, rec, moved_unit_id, moved_x, moved_y, x1, y1, x2, y2, out.events.size());
        return true;
    }
    script_forced_target_ = saved_target;
    return false;
}

// sub_53F2C(al): victory/defeat event table walk (LST 00053F2C..00053F92).
//   Head: dword_11024C = 0, dx = (al==0 ? 9 : 10), ecx = dword_100818[dx] (start of the slot table).
//         dword_100818 is the base of sub_5306A's 12-dword slot array (unk_10081C), so
//         al=0 (edx=9) -> slot[9] and al=1 (edx=10) -> slot[10].
//   Record layout differs from the time/sequence triggers (there is no id comparison):
//     ecx += 3 reads a u16 len, then ecx += 2 (5-byte header: u8[3] + u16 len)
//     len == 0 -> end of table (loc_52836)
//     len != 0 -> sub_530F1 evaluates the condition; true runs the bytecode through
//                 sub_52E55 and returns, false adds len (skipping condition and bytecode)
//   Measured: slot[9]/slot[10] of all 294 SCE files are empty (`FA 00 FA 00 00`, len=0), so the
//   original also goes straight to the exit path without a victory cutscene. The worker is
//   ported exactly in case a table is populated.
bool CombatII::run_victory_script(int which, VmResult& out) {
    out = VmResult();
    if (!script_loaded_) return false;
    // sub_53F2C head: `mov dword_11024C, 0` clears the forced target
    script_forced_target_ = -1;
    const int slot = 9 + which;                  // edx = 9 + which -> dword_100818[edx]
    const uint32_t base = script_table_base(slot);   // sub_53512(0, 9+which)
    if (base >= script_buffer_.size()) return false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    int guard = 0;
    while (c.ok && guard++ < 512) {
        uint32_t rec = c.off;
        c.skip(3);              // the worker skips the u8[3] header
        uint16_t len = c.u16(); // u16 len: 0 ends the table, otherwise the length of condition + bytecode
        if (!c.ok) break;
        if (script_trace_)
            fprintf(stderr, "COMBAT: victory rec @0x%X len=%u (slot=%d)\n", rec, len, slot+1);
        if (len == 0) break;    // loc_52836: end of table
        bool ok = false;
        uint32_t next = eval_condition(c.off, ok);
        if (ok && next != 0) {
            out = run_vm(next);
            return true;
        }
        c.off = rec + 5 + len;  // loc_53F89: `ecx += len`
    }
    return false;
}

// ==== CC990 roster (sub_500ED / sub_50105 / sub_50D9F loc_50EFF) ====
// sub_500ED(id): scan dword_CC990 from the front and return the first record with the same [0Fh].
CombatUnitPlacement* CombatII::battle_roster_find_500ed(int id) {
    for (auto& r : battle_roster_cc990_)
        if (r.id == id) return &r;
    return nullptr;
}

// sub_50105(eax = spawn record, edx = roster record): save [0], [1], [63h] and [5Ch], overwrite
//   the 0x88 bytes with the roster record, then restore position and facing. [48h]=[4Ch] and
//   [50h]=[52h] refill HP and MP. When the saved [5Ch] is nonzero the new data is also copied to
//   the [77h] stored record and sub_17E6D turns the current unit into that form (ASM 86634~86658).
void CombatII::battle_roster_apply_50105(CombatUnitPlacement& spawn, const CombatUnitPlacement& roster_rec) {
    const int saved_x = spawn.x, saved_y = spawn.y;          // var_C / var_4
    const int8_t saved_facing = spawn.facing;                // var_8
    const uint8_t old_form = spawn.code_b;                   // var_10 = [5Ch]
    auto old_77 = spawn.stored_77;                           // edx = [77h]
    const bool was_active = spawn.active;
    CombatUnitPlacement src = roster_rec;
    src.x = saved_x; src.y = saved_y;
    src.facing = saved_facing;
    src.active = was_active;
    src.group = spawn.group;
    src.memory_army = spawn.memory_army;
    src.battle_list_membership_modeled = spawn.battle_list_membership_modeled;
    src.stored_77 = old_77;
    src.ai_target_unit = -1;
    src.hp = src.max_hp;                                     // [48h] = [4Ch]
    src.hp_deficit = 0;
    src.combat_mp_50 = src.combat_mp_max_52;                 // [50h] = [52h]
    spawn = src;
    if (old_form != 0 && old_77) {
        old_77->active = false;
        old_77->stored_77.reset();
        old_77->x = spawn.x; old_77->y = spawn.y;
        battle_form_17e6d(spawn, old_form);                  // sub_17E6D(unit, [5Ch])
        spawn.stored_77 = old_77;
    }
}

// sub_112FD (allocate 0x88) + sub_1134F (append to the end of the list: the tail link is
//   spliced, not the head): a processed record goes to the end of CC990.
//   [0Eh] = 1 and [56h] = 0 per loc_4E942 / loc_50F25.
void CombatII::battle_roster_push_cc990(const CombatUnitPlacement& rec) {
    CombatUnitPlacement c = rec;
    c.active = false;
    c.stored_77.reset();
    c.memory_army = 1;                                       // [0Eh] = 1
    c.battle_gauge = 0;                                      // [56h] = 0
    battle_roster_cc990_.push_back(c);
}

// (the sub_55518 deployment step lives in combatii_deploy.cpp; the original runs it at battle
// start.)

// ==== table number to slot, unit HP / area / morale / legion-wipe triggers ====
// sub_53512(eax=0, dx=table number) = dword_100818[dx]. sub_5306A reads the file's 12 dwords
// from unk_10081C onward, so 0x100818 = 0x10081C - 4 and dword_100818[k] = script_offsets_[k-1]
// (k = 1..12). Watcom folded the -1 of `slot[k-1]` into the displacement.
//   1 sequence (sub_5358F)     2 area (sub_5361C)        3/4 unit HP (sub_53797 mode 0/1)
//   5/6 contact (sub_5394D)    7 area unit count (sub_53C12)  8 morale (sub_53E0F)
//   9/10 victory/defeat (sub_53F2C)  11 legion wipe (sub_53F94)
// Only the time trigger (sub_53523) has no table number and starts at body offset 0.
uint32_t CombatII::script_table_base(int table) const {
    if (!script_loaded_ || table < 1 || table > kScriptSlots) return 0xFFFFFFFFu;
    return script_offsets_[table - 1];
}

// Diagnostics (G2_BATTLE_TRACE=1): walk each table to its end (length 0) using the record layout
//   in the ASM. Record = [skip bytes][fixed bytes][u16 length][length bytes]. A wrong offset-table
//   interpretation either runs out of range or never finds the end, and prints "BAD".
void CombatII::dump_script_tables() const {
    if (!script_loaded_) return;
    struct Layout { int table; const char* name; int skip; int fixed; };
    static const Layout kLayouts[] = {
        {1, "seq", 2, 4},    {2, "area", 2, 6},   {3, "hp0", 2, 3},     {4, "hp1", 2, 3},
        {5, "hit0", 2, 4},   {6, "hit1", 2, 4},   {7, "region", 2, 7},  {8, "morale", 2, 3},
        {9, "victory", 3, 0},{10, "defeat", 3, 0},{11, "wiped", 2, 1},
    };
    for (const Layout& l : kLayouts) {
        const uint32_t base = script_table_base(l.table);
        int records = 0;
        bool ok = false;
        uint32_t off = base;
        for (int guard = 0; guard < 512 && base < script_buffer_.size(); ++guard) {
            const uint32_t hdr = (uint32_t)(l.skip + l.fixed + 2);
            if (off + hdr > script_buffer_.size()) break;
            const uint32_t size = (uint32_t)(script_buffer_[off + l.skip + l.fixed] |
                                             (script_buffer_[off + l.skip + l.fixed + 1] << 8));
            if (size == 0) { ok = true; break; }
            if (off + hdr + size > script_buffer_.size()) break;
            off += hdr + size;
            ++records;
        }
        fprintf(stderr, "COMBAT: table[%2d] %-7s @0x%X records=%d %s\n",
                l.table, l.name, base, records, ok ? "ok" : "BAD");
    }
}

// sub_53797(eax=victim, edx=other, ebx=mode): unit HP event.
//   mode 0 -> table 3 (unk_110514): from the attack declaration (sub_1DDF0 dx=1), keyed on the
//            attacker with other = first target
//   mode 1 -> table 4 (unk_110320): from hit/death (sub_1ED45, sub_1DB1D), keyed on the victim
//            with other = attacker/killer
// Record: [skip 2][sel][idx][condition][u16 length][condition block + bytecode]
//   condition 0 = always, 1 = HP < max/2, 2 = HP < max/10, 3 = HP <= 0
// Each record fires once (counter), and the firing record ends the table walk.
// On fire, dword_11024C = other (a script sel >= 10 refers to that unit) and the two units face
// each other.
bool CombatII::run_unit_hp_trigger(int victim_index, int other_index, int mode) {
    if (!script_loaded_ || victim_index < 0 || (size_t)victim_index >= units_.size()) return false;
    if (other_index < 0 || (size_t)other_index >= units_.size()) other_index = -1;
    const int table = (mode == 0) ? 3 : 4;
    uint8_t* fired = hp_trigger_fired_[mode == 0 ? 0 : 1];
    const uint32_t base = script_table_base(table);
    if (base >= script_buffer_.size()) return false;

    const int saved_target = script_forced_target_;            // var_20
    script_forced_target_ = -1;                                // mov dword_11024C, 0
    bool ran = false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    for (int rec = 0; c.ok && rec < kTriggerRecordMax; ++rec) {
        c.skip(2);
        const uint8_t sel = c.u8();
        const uint8_t idx = c.u8();
        const uint8_t cond = c.u8();
        const uint16_t size = c.u16();
        if (!c.ok || size == 0) break;                         // end of table
        const uint32_t payload = c.off;
        if (!script_in_range(payload, size)) break;
        c.off = payload + size;                                // loc_5393E: ecx += length
        if (fired[rec] != 0) continue;
        if (find_unit_index(sel, idx) != victim_index) continue;

        const CombatUnitPlacement& v = units_[(size_t)victim_index];
        bool hit = false;
        switch (cond) {
            case 0: hit = true; break;                         // loc_538A2
            case 1: hit = (v.max_hp >> 1) > v.hp; break;       // loc_53874
            case 2: hit = (v.max_hp / 10) > v.hp; break;       // loc_53882
            case 3: hit = v.hp <= 0; break;                    // loc_5389C
            default: break;
        }
        if (!hit) continue;

        script_forced_target_ = other_index;                   // loc_538A7
        bool satisfied = false;
        const uint32_t vm_offset = eval_condition(payload, satisfied);
        if (!satisfied || vm_offset == 0) break;               // test eax,eax; jz loc_5392E

        if (other_index >= 0) {                                // face each other ([63h] = (other facing + 2) % 4)
            if (mode == 0) units_[(size_t)other_index].facing = (int8_t)((units_[(size_t)victim_index].facing + 2) & 3);
            else           units_[(size_t)victim_index].facing = (int8_t)((units_[(size_t)other_index].facing + 2) & 3);
        }
        fired[rec]++;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: hp trigger mode=%d rec=%d cond=%u unit=%d other=%d\n",
                    mode, rec, (unsigned)cond, units_[(size_t)victim_index].id,
                    other_index >= 0 ? units_[(size_t)other_index].id : -1);
        VmResult r = run_vm(vm_offset);                        // sub_4FFEF; sub_52E55; sub_5001F
        if (!r.events.empty()) play_vm_events(r.events);
        ran = true;
        break;
    }
    script_forced_target_ = saved_target;
    return ran;
}

// sub_5394D(eax=unit, edx=other unit, ebx=mode): contact trigger. mode 0 = attack declaration
//   (table 5), mode 1 = unit in front after a move (table 6).
// Record: [skip 2][A][B][C][D][u16 length][condition block + bytecode]
//   (A,B) is the first reference and (C,D) the second. When A (C) < 0Ah the unit comes from
//   sub_501BA(A, B); when A >= 0Ah the unit whose legion is A-9 is chosen (self or other), and
//   dword_11024C becomes that unit too.
//   mode 0 requires (unit==ref1 && other==ref2); mode 1 accepts either order.
//   On fire the other unit's facing is turned to (unit facing + 2) % 4, the table's fire flag
//   (one byte per table) is set, the sub_530F1 condition is evaluated and sub_52E55 runs.
// Deviation: the original's mode 0 saves and restores the F0648 list through
//   sub_4FFEF/sub_5001F, but the port VM does not use that list.
bool CombatII::run_contact_trigger(int unit_index, int target_index, int mode) {
    if (!script_loaded_ || unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (target_index < 0 || (size_t)target_index >= units_.size()) return false;
    const int table = (mode == 0) ? 5 : 6;
    const uint32_t base = script_table_base(table);            // sub_53512(0, 5/6)
    if (base >= script_buffer_.size()) return false;

    uint8_t& fired = contact_trigger_fired_[mode == 0 ? 0 : 1];   // byte[table base + mode]
    const int saved_target = script_forced_target_;            // var_20
    script_forced_target_ = -1;                                // mov dword_11024C, 0
    bool ran = false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    for (int rec = 0; c.ok && rec < kTriggerRecordMax; ++rec) {
        c.skip(2);
        const uint8_t sel_a = c.u8();
        const uint8_t val_b = c.u8();
        const uint8_t sel_c = c.u8();
        const uint8_t val_d = c.u8();
        const uint16_t size = c.u16();
        if (!c.ok || size == 0) break;                         // loc_53C01 (end of table)
        const uint32_t payload = c.off;
        if (!script_in_range(payload, size)) break;
        c.off = payload + size;                                // loc_53BE9: edi += length
        if (fired != 0) continue;                              // table already fired

        // ---- reference resolution (sub_501BA or legion compare) ----
        auto resolve = [&](uint8_t sel, uint8_t val, int& forced) {
            if (sel < 0x0A) return find_unit_index(sel, val);
            const int army = (int)sel - 9;
            if (battle_effective_army(units_[(size_t)unit_index]) == army) {
                forced = unit_index;
                return unit_index;
            }
            if (battle_effective_army(units_[(size_t)target_index]) == army) {
                forced = target_index;
                return target_index;
            }
            return -1;
        };
        int forced = -1;
        const int ref1 = resolve(sel_a, val_b, forced);
        if (forced >= 0) script_forced_target_ = forced;       // dword_11024C = that unit
        const int ref2 = resolve(sel_c, val_d, forced);

        bool hit = false;
        if (mode == 0) {
            hit = (ref1 == unit_index && ref2 == target_index);
        } else {
            if (ref1 < 0 || ref2 < 0) return false;            // test ebx/ebx -> var_28 = 0
            hit = (ref1 == unit_index && ref2 == target_index) ||
                  (ref1 == target_index && ref2 == unit_index);
        }
        if (!hit) continue;

        bool satisfied = false;
        const uint32_t vm_offset = eval_condition(payload, satisfied);   // sub_530F1
        if (!satisfied || vm_offset == 0) break;
        // The other unit turns to face: [other+63h] = (unit facing + 2) % 4
        units_[(size_t)target_index].facing =
            (int8_t)(((int)units_[(size_t)unit_index].facing + 2) & 3);
        ++fired;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: contact trigger table=%d mode=%d unit=%d target=%d\n",
                    table, mode, units_[(size_t)unit_index].id, units_[(size_t)target_index].id);
        VmResult r = run_vm(vm_offset);                        // sub_52E55 (+4FFEF/5001F)
        if (!r.events.empty()) play_vm_events(r.events);
        ran = true;
        break;
    }
    script_forced_target_ = saved_target;
    return ran;
}

// sub_53C12: table 7, the "units of a legion inside an area" trigger. Tested after moves
//   and deaths.
// Record: [skip 2][x1][y1][x2][y2][legion-1][count][compare][u16 length][condition + bytecode]
//   The ASM increments the record's army-1 and compares it against effective [+0x0E] in the
//   CC998 occupancy cells. The port uses the effective-army helper and the base tile of active
//   units, so the army byte matches on the normal path, but group-0/dynamic remap, VM faction
//   and large-unit occupancy are not modelled.
//   compare 0 = equal, 1 = greater or equal, 2 = less or equal. Each record fires once (unk_110254).
// While counting, dword_11024C is chosen as the first unit met, then replaced with 1/3 chance.
bool CombatII::run_region_count_trigger() {
    if (!script_loaded_) return false;
    const uint32_t base = script_table_base(7);
    if (base >= script_buffer_.size()) return false;

    const int saved_target = script_forced_target_;            // ebp
    script_forced_target_ = -1;
    bool ran = false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    for (int rec = 0; c.ok && rec < kTriggerRecordMax; ++rec) {
        c.skip(2);
        const int x1 = c.u8(), y1 = c.u8(), x2 = c.u8(), y2 = c.u8();
        const int army_id = c.u8();                            // Record is effective army-1 (CC968 slot).
        const int want = c.u8();
        const int compare = c.u8();
        const uint16_t size = c.u16();
        if (!c.ok || size == 0) break;
        const uint32_t payload = c.off;
        if (!script_in_range(payload, size)) break;
        c.off = payload + size;
        if (region_trigger_fired_[rec] != 0) continue;

        script_forced_target_ = -1;
        int count = 0;
        for (int y = y1; y <= y2; ++y) {
            for (int x = x1; x <= x2; ++x) {
                for (size_t i = 0; i < units_.size(); ++i) {
                    const CombatUnitPlacement& u = units_[i];
                    if (!u.active || u.x != x || u.y != y ||
                        battle_effective_army(u) != (int)battle_incremented_raw_army(army_id)) continue; // Byte INC wraps; CC998 footprint is still approximated by the base tile.
                    ++count;
                    if (script_forced_target_ < 0 || combat_rand(3) == 0) script_forced_target_ = (int)i;
                }
            }
        }
        bool hit = false;
        if (compare == 0) hit = (count == want);
        else if (compare == 1) hit = (count >= want);
        else if (compare == 2) hit = (count <= want);
        if (!hit) continue;

        bool satisfied = false;
        const uint32_t vm_offset = eval_condition(payload, satisfied);
        if (!satisfied || vm_offset == 0) break;
        region_trigger_fired_[rec]++;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: region trigger rec=%d rect=(%d,%d)-(%d,%d) army=%d count=%d/%d cmp=%d\n",
                    rec, x1, y1, x2, y2, army_id, count, want, compare);
        VmResult r = run_vm(vm_offset);
        if (!r.events.empty()) play_vm_events(r.events);
        ran = true;
        break;
    }
    script_forced_target_ = saved_target;
    return ran;
}

// sub_53E0F: table 8, the morale trigger (low byte of the ally/enemy word in dword_F0BA0).
// Record: [skip 2][target 0 = ally gauge, 1 = enemy gauge][value][compare][u16 length][condition +
// bytecode]
// The original only ever writes fire flag [0] of byte_1104B0 (nothing increments esi), so the
// whole table fires exactly once.
bool CombatII::run_morale_trigger() {
    if (!script_loaded_ || morale_trigger_fired_ != 0) return false;
    const uint32_t base = script_table_base(8);
    if (base >= script_buffer_.size()) return false;

    const int saved_target = script_forced_target_;            // edi
    script_forced_target_ = -1;
    bool ran = false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    for (int rec = 0; c.ok && rec < kTriggerRecordMax; ++rec) {
        c.skip(2);
        const uint8_t which = c.u8();
        const uint8_t value = c.u8();
        const uint8_t compare = c.u8();
        const uint16_t size = c.u16();
        if (!c.ok || size == 0) break;
        const uint32_t payload = c.off;
        if (!script_in_range(payload, size)) break;
        c.off = payload + size;

        if (which > 1) continue;                               // the original compares an uninitialised value; treat it as not fired
        const uint8_t morale = (uint8_t)(which == 0 ? morale_lo_ : morale_hi_);
        bool hit = false;
        if (compare == 0) hit = (morale == value);
        else if (compare == 1) hit = (morale >= value);
        else if (compare == 2) hit = (morale <= value);
        if (!hit) continue;

        bool satisfied = false;
        const uint32_t vm_offset = eval_condition(payload, satisfied);
        if (!satisfied || vm_offset == 0) break;
        morale_trigger_fired_ = 1;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: morale trigger rec=%d which=%u morale=%u cmp=%u value=%u\n",
                    rec, (unsigned)which, (unsigned)morale, (unsigned)compare, (unsigned)value);
        VmResult r = run_vm(vm_offset);
        if (!r.events.empty()) play_vm_events(r.events);
        ran = true;
        break;
    }
    script_forced_target_ = saved_target;
    return ran;
}

// sub_53F94(al = legion-1): table 11, the legion-wipe trigger. sub_1DB1D calls it when a legion
//   list becomes empty.
// Record: [skip 2][legion-1][u16 length][condition + bytecode].
// Unlike the other tables it keeps walking to the end of the table after firing (loc_54002).
// 1-5.SCE's "ally wiped -> two enemy lines -> VICTORY (0x01)" lives in this table.
bool CombatII::run_army_wiped_trigger(int group) {
    if (!script_loaded_) return false;
    const uint32_t base = script_table_base(11);
    if (base >= script_buffer_.size()) return false;

    const int saved_target = script_forced_target_;            // esi
    script_forced_target_ = -1;
    bool ran = false;
    ScriptCursor c{script_buffer_.data(), base, (uint32_t)script_buffer_.size()};
    for (int rec = 0; c.ok && rec < kTriggerRecordMax && !should_exit_; ++rec) {
        c.skip(2);
        const uint8_t record_group = c.u8();
        const uint16_t size = c.u16();
        if (!c.ok || size == 0) break;
        const uint32_t payload = c.off;
        if (!script_in_range(payload, size)) break;
        c.off = payload + size;
        if (record_group != (uint8_t)group) continue;
        bool satisfied = false;
        const uint32_t vm_offset = eval_condition(payload, satisfied);
        if (!satisfied || vm_offset == 0) continue;
        if (battle_trace_) fprintf(stderr, "COMBAT: army wiped trigger group=%d rec=%d\n", group, rec);
        VmResult r = run_vm(vm_offset);
        if (!r.events.empty()) play_vm_events(r.events);
        ran = true;
    }
    script_forced_target_ = saved_target;
    return ran;
}

// Diagnostics: print the slot table, one time-trigger record and one VM trace.
void CombatII::dump_script_engine() {
    if (!script_loaded_) { fprintf(stderr, "COMBAT: script engine not loaded\n"); return; }
    fprintf(stderr, "COMBAT: --- script engine dump (body %zu bytes) ---\n", script_buffer_.size());
    for (int i = 0; i < kScriptSlots; ++i) {
        uint32_t s = script_offsets_[i];
        fprintf(stderr, "COMBAT: slot[%2d] = 0x%X%s\n", i, s,
                s < script_buffer_.size() ? "" : " (out of range)");
    }
    const bool keep_trace = DevUtil::script_trace();
    const uint32_t battle_time_before = battle_time_;
    const int32_t script_seq_before = script_seq_;
    const int16_t morale_lo_before = morale_lo_, morale_hi_before = morale_hi_;
    const uint8_t morale_trigger_fired_before = morale_trigger_fired_;
    const int forced_target_before = script_forced_target_;
    const bool victory_fired_before = victory_fired_;
    const bool occupancy_known_before = battle_wc1_occupancy_state_known_;
    const std::string next_scenario_before = script_next_scenario_;
    const int return_code_before = return_code_;
    const bool should_exit_before = should_exit_;
    const std::vector<int> ready_queue_before = battle_ready_queue_;
    const int battle_player_unit_before = battle_player_unit_;
    const bool battle_attack_select_before = battle_attack_select_;
    const uint16_t battle_state_before = battle_state_;
    const uint16_t battle_overlay_mode_before = battle_overlay_mode_;
    const int16_t ai_gauge_baseline_before = battle_ai_gauge_baseline_;
    std::array<uint8_t, 256> script_flag_word_before;
    memcpy(script_flag_word_before.data(), script_flag_word_, sizeof(script_flag_word_));
    std::vector<uint8_t> unit_battle_gauges_before;
    unit_battle_gauges_before.reserve(units_.size());
    for (const CombatUnitPlacement& unit : units_) unit_battle_gauges_before.push_back(unit.battle_gauge);
    std::vector<uint8_t> unit_one_shot_flags_before;
    unit_one_shot_flags_before.reserve(units_.size());
    for (const CombatUnitPlacement& unit : units_) unit_one_shot_flags_before.push_back(unit.combat_flags_13);
    uint8_t script_flag_bytes_before[sizeof(scenario_flags_)];
    uint8_t script_fired_before[sizeof(script_fired_)];
    uint8_t area_trigger_fired_before[sizeof(area_trigger_fired_)];
    memcpy(script_flag_bytes_before, scenario_flags_, sizeof(scenario_flags_));
    memcpy(script_fired_before, script_fired_, sizeof(script_fired_));
    memcpy(area_trigger_fired_before, area_trigger_fired_, sizeof(area_trigger_fired_));
    script_trace_ = true;
    VmResult r;
    battle_time_ = battle_time_ ? battle_time_ : 1;
    // The observing VM run restores pending parser-side spawns and script/game state, so it changes
    // nothing.
    const size_t units_before = units_.size();
    bool fired = run_time_trigger(battle_time_, r);
    if (units_.size() > units_before) units_.resize(units_before);
    fprintf(stderr, "COMBAT: time trigger(%u) fired=%d lines=%zu ended=%d aborted=%d ops=%d\n",
            battle_time_, fired ? 1 : 0, r.lines.size(), r.ended ? 1 : 0, r.aborted ? 1 : 0, r.instructions);
    if (!fired && script_seq_ >= 0) {   // sub_19D53: `test ebx,ebx; jl`; -1 skips the sequence trigger
        r = VmResult();
        bool f2 = run_seq_trigger((uint32_t)script_seq_, r);
        fprintf(stderr, "COMBAT: seq trigger(%d) fired=%d lines=%zu ended=%d aborted=%d ops=%d\n",
                (int)script_seq_, f2 ? 1 : 0, r.lines.size(), r.ended ? 1 : 0, r.aborted ? 1 : 0, r.instructions);
    }
    // The dump parses events only: discard pending units and restore parser-side state, including
    // the one-way occupancy latch.
    if (units_.size() > units_before) units_.resize(units_before);
    battle_time_ = battle_time_before;
    script_seq_ = script_seq_before;
    morale_lo_ = morale_lo_before;
    morale_hi_ = morale_hi_before;
    morale_trigger_fired_ = morale_trigger_fired_before;
    script_forced_target_ = forced_target_before;
    victory_fired_ = victory_fired_before;
    battle_wc1_occupancy_state_known_ = occupancy_known_before && battle_sce_snapshot_complete_;
    script_next_scenario_ = next_scenario_before;
    return_code_ = return_code_before;
    should_exit_ = should_exit_before;
    battle_ready_queue_ = ready_queue_before;
    battle_player_unit_ = battle_player_unit_before;
    battle_attack_select_ = battle_attack_select_before;
    battle_state_ = (uint8_t)battle_state_before;
    battle_overlay_mode_ = battle_overlay_mode_before;
    battle_ai_gauge_baseline_ = ai_gauge_baseline_before;
    memcpy(scenario_flags_, script_flag_bytes_before, sizeof(scenario_flags_));
    memcpy(script_flag_word_, script_flag_word_before.data(), sizeof(script_flag_word_));
    memcpy(script_fired_, script_fired_before, sizeof(script_fired_));
    memcpy(area_trigger_fired_, area_trigger_fired_before, sizeof(area_trigger_fired_));
    for (size_t i = 0; i < units_.size() && i < unit_battle_gauges_before.size(); ++i)
        units_[i].battle_gauge = unit_battle_gauges_before[i];
    for (size_t i = 0; i < units_.size() && i < unit_one_shot_flags_before.size(); ++i)
        units_[i].combat_flags_13 = unit_one_shot_flags_before[i];
    for (size_t i = 0; i < r.lines.size() && i < 8; ++i)
        fprintf(stderr, "COMBAT: line[%zu] %s: %.90s\n", i, r.lines[i].is_overlay ? "CAPTION" : "DIALOG ", r.lines[i].text.c_str());
    script_trace_ = keep_trace;
}

// ==== sub_44CCA: screen fade-out of the victory path ====
//   1) sub_44D6E(1) / sub_44DEC(0): stop scrolling and overlays
//   2) sub_454B3(camera X, Y, 0xC8): redraw the last frame
//   3) the sub_5FD1C family drives g_palette to 0 (sub_6104A(0,0)/sub_5ECC7(1,0x130) = the whole
//      range, sub_5EDDE(0x10) = step clamp), transmitting on every vertical retrace so the
//      screen darkens.
// The port takes g_palette to 0 in 10 steps and presents after each step.
void CombatII::victory_fade_out() {
    fprintf(stderr, "COMBAT: victory fade-out (sub_44CCA)\n");
    if (auto_advance_) {
        // Auto-advance mode skips the fade frames so verification stays fast.
        memset(g_palette, 0, sizeof(g_palette));
        return;
    }
    for (int step = 1; step <= 10; ++step) {
        for (int i = 0; i < 256; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                int v = g_palette[i][ch];
                v = v - v * step / 10;   // converge to 0 in steps (sub_5FD1C's base+delta*i/100)
                g_palette[i][ch] = (uint8_t)std::clamp(v, 0, 63);
            }
        }
        video_.present();
        SDL_Delay(30);                    // approximation of sub_5E943 (vertical retrace)
    }
    memset(g_palette, 0, sizeof(g_palette));
    video_.present();
}

// ==== sub_52D33 -> sub_55EBC: fade-out of the next-scenario switch ====
// The original 0x12 (VM_SCENARIO) handler sub_52D33 cleans up units/stats and calls sub_55EBC:
//   1) it reads two bytes of `SCENARIO\ast.map` as the brightness level (0..100), normally 100.
//      sub_5AA45 is a no-op while dword_112444 is 0, so the real brightness comes from here.
//   2) after sub_5AA45 it calls `game_timer_set(3, 100)` and loops: `sub_5FA2C(palette, level)`
//      + `sub_499E4` (one tick) + `var -= 5`, darkening from 100 to 5 in steps of 5 over 20 ticks.
//   3) sub_5FA2C writes `value * level / 100` to the DAC (0x3C8/0x3C9).
// The original therefore fades the screen to black over 21 ticks before switching scenario.
// sub_4FD6B then loads the next scenario and sub_61CFF ends the process, so the new process's
// start sequence (sub_55B59: load genesis.pal -> sub_5F9AE(0,0x100) -> the sub_5D5ED/sub_5D15A
// screen reset) becomes the fade-in from black. The port does not restart the process, so it
// splits the effect into this fade-out and the fade-in after load_scenario.
void CombatII::next_scenario_fade_out() {
    if (auto_advance_) {
        // Verification shortcut: go black at once (same auto-advance rule as victory_fade_out).
        memset(g_palette, 0, sizeof(g_palette));
        video_.present();
        return;
    }
    // sub_55EBC, from dasm 94727..94787:
    //  * ast.map is read into var_4 but immediately overwritten by `mov [esp+4+var_4], dx`
    //    (edx = 64h), so the brightness always starts at 100.
    //  * `mov eax,3; call sub_1033B` sets the game-timer period to 3. Without it the previous
    //    dialogue's period (700) survived and every step waited 700 ticks.
    //  * the loop is `cmp var_4,0; jge`: 100, 95, ..., 5, 0 - 21 steps, including 0.
    //  * sub_5FA2C waits for the vertical retrace (sub_5E943), so one step is about 14.3ms
    // (70.086Hz).
    fprintf(stderr, "COMBAT: next-scenario fade-out (sub_55EBC, 100 -> 0 step 5, 21 steps)\n");
    uint8_t saved[256][3];
    memcpy(saved, g_palette, sizeof(saved));
    game_timer_set(3);                                   // sub_1033B(3)
    constexpr double kVretraceUs = 1e6 / 70.086;         // sub_5E943
    for (int brightness = 100; brightness >= 0; brightness -= 5) {
        const double t0 = timer_now_us();
        for (int i = 0; i < 256; ++i)
            for (int ch = 0; ch < 3; ++ch)
                g_palette[i][ch] = (uint8_t)((int)saved[i][ch] * brightness / 100); // sub_5FA2C scale
        video_.present();
        wait_game_tick();                                // sub_499E4 (3 ticks)
        const double remain = kVretraceUs - (timer_now_us() - t0);
        if (remain > 1000.0) SDL_Delay((Uint32)(remain / 1000.0));
    }
    memset(g_palette, 0, sizeof(g_palette));
    video_.present();
}

// ==== sub_52D33: VM 0x12 next-scenario switch (dasm 0x52D33) ====
//   mov edx,10h / mov ebx,eax / mov eax,offset unk_10018C ; copy the 16-byte name
//   mov eax,2  ; call sub_4E4C4     -> open BLACKBOX
//   call sub_4E622                  -> save BLACKBOX (phase/day/stats/storage)
//   call sub_4EC51                  -> restore battle units (write the 0x88 records), free the list
//   call sub_4ECD7                  -> close the BLACKBOX handle
//   call sub_55EBC                  -> fade the palette from 100 to 0
//   xor edx,edx / mov eax,unk_10018C / call sub_4FD6B -> contmap.dat (name, flag 0)
//   mov eax,1  ; call sub_61CFF     -> exit with code 1
// MAIN.EXE's main_ sees a code other than 0FFh, returns to loc_100EA, re-reads contmap.dat and
// starts COMBATII again (main_ loc_100EA -> spawnl(combatII.exe, NULL) -> the argc==1 path).
// Loading, deployment, scenario conditions and cutscenes of the next scenario therefore belong
// to the new run's start sequence, and the side that executed 0x12 ends here. The port keeps that
// boundary and does not load the next .SCE inside this run.
void CombatII::vm_next_scenario_52d33(const std::string& name) {
    fprintf(stderr, "COMBAT: vm 0x12 next scenario '%s' (sub_52D33: save/cleanup/fade/contmap/exit 1)\n",
            name.c_str());
    battle_blackbox_save(2);                 // sub_4E4C4(2) + sub_4E622
    battle_battle_cleanup_4ec51();           // sub_4EC51 + sub_4EAF3
    battle_blackbox_close();                 // sub_4ECD7 (no-op in the port)
    next_scenario_fade_out();                // sub_55EBC
    write_contmap_dat(name, 0);              // sub_4FD6B(unk_10018C, 0)
    script_next_scenario_ = name;
    return_code_ = 1;                        // sub_61CFF(1)
    should_exit_ = true;
}

bool CombatII::wait_for_input(uint32_t min_ms) {
    // Test hook: with `G2_AUTOADVANCE=1` (no video driver, or automated runs), dialogue and
    // caption waits are cut to the minimum.
    static const bool auto_advance = (getenv("G2_AUTOADVANCE") != nullptr);
    if (auto_advance) {
        // Keep the camera scrolling for min_ms/16 frames, like the original battle loop.
        Uint32 frames = std::max<Uint32>(1, min_ms / 16);
        for (Uint32 i = 0; i < frames; ++i) {
            if (camera_scrolling()) camera_update_frame();
            SDL_Delay(4);
        }
        return false;
    }
    Uint32 start = SDL_GetTicks();
    while (true) {
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return true; }
        if (input_.is_key_pressed(SDLK_ESCAPE)) { skip_cutscene_ = true; return true; }
        // The battle loop calls sub_5046B every frame while waiting, so the camera keeps moving.
        if (camera_scrolling()) { camera_update_frame(); present_cutscene_frame(); }
        if (input_.was_left_clicked() || input_.is_key_pressed(SDLK_SPACE) || input_.is_key_pressed(SDLK_RETURN)) {
            if (SDL_GetTicks() - start > min_ms) return false;
        }
        SDL_Delay(16);
    }
}

// unk_7110D = {2,3,4,3} / unk_71111 = {3,4,3,4} (COMBATII.ASM 0x7110D).
// sub_16D3E's prologue (loc_16D78) picks the table from the class in [unit+5Ch], and sub_16A6D
// reloads the pose as `[unit+64h] = table[unk_E5A70]` every 2 sub-frames.
static uint8_t walk_pose_table(uint8_t class_byte, int idx) {
    static const uint8_t table_a[4] = {2, 3, 4, 3};   // unk_7110D - normal classes
    static const uint8_t table_b[4] = {3, 4, 3, 4};   // unk_71111 - classes 0Fh..16h
    const bool special = (class_byte >= 0x0F && class_byte <= 0x16);
    return special ? table_b[idx & 3] : table_a[idx & 3];
}

// Exact port of the 4-direction test order in sub_16D3E loc_16E1A / sub_52848.
//   y first: self y > target y -> 2 (north), self y < target y -> 0 (south)
//   when y is equal, x: self x > target x -> 1 (west), self x < target x -> 3 (east)
//   when both are equal the original leaves [63h] untouched -> -1.
// Same formula as sub_14F52: (x,y) -> (tx,ty) facing is E/W when |dx| > |dy|, else N/S (ties
// go to N/S). sub_14F52(eax,ebx,ecx,edx) matched the disassembly on 589/589 cases. It differs
// from an axis-priority formula on diagonals, but all four call sites are adjacent tiles, where
// the result is identical. A (0,0) tie returns 2 as the original does.
int8_t CombatII::walk_facing_toward(int x, int y, int tx, int ty) {
    const int dx = tx - x, dy = ty - y;
    if (std::abs(dx) > std::abs(dy)) return dx > 0 ? 3 : 1;
    return dy > 0 ? 0 : 2;
}

// ---- sub_508E6(3): mode-1 spawn drop animation ----
//   prologue: word_EDD74 += 8 (walk-repeat correction), and [63h] = unk_E5AD0 (load) per unit.
//   The camera keeps the scroll set by the preceding VM camera command, and each frame draws
//   sub_4559F plus the per-unit drop through sub_50216(unit, 7-cnt):
//     sub_50216 reproduced exactly (dasm 86723~86838):
//     (1) dx=7-cnt <= 1 skips the unit sprite (no sub_5F517) and jumps to (5)
//     (2) draws pose (9-dx) of the unit's NC2 frame table (unk_D66A8 + 572*facing) through
//         sub_5F517, a blitter that ignores pixels and fills the silhouette with colour cl
//         (`mov al,cl; mov ah,cl; ... repe stosd`), so the silhouette brightens to colour 2+cnt
//         (the following `mov ax, [frame+3]` only refreshes the dialogue face cache)
//     (3) reads xo/yo from the 12-byte step table unk_10059C[dx]
//         { dword, word xo@+4, word yo@+6, dword EVENT.SYB@+8 } and accumulates them in di
//         (`add di,[esi+6]; add ax,[esi+4]`)
//     (4) sub_5F3FD(EVENT.SYB sprite, unit tile screen coords + xo, + yo): EVENT.SYB's rec7
//         (2x4 base, yo=+12) through rec0 (pillar top, yo=-16) is drawn for dx=0..7, so the light
//         pillar rises from bottom to top as the counter advances (the unit itself stays put)
//     (5) dx > 7 draws nothing (cnt >= 8: landed, the normal path draws the unit)
//     Counter unk_E5A50 increments on every even frame, emits sub_11F74 dust and refreshes state
//     at 7, and on the next even frame (cnt 8) removes the unit from the dword_CC968 list, so
//     normal drawing resumes.
void CombatII::play_spawn_fall(const VmEvent& ev) {
    if (ev.walk_units.empty()) { present_cutscene_frame(); return; }
    load_event_syb();

    auto unit_at = [&](int i) -> CombatUnitPlacement* {
        return (i >= 0 && (size_t)i < units_.size()) ? &units_[(size_t)i] : nullptr;
    };
    auto bump_input = [&]() -> bool {
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return true; }
        if (input_.is_key_pressed(SDLK_ESCAPE)) { skip_cutscene_ = true; return true; }
        return false;
    };

    // loc_50905: [63h] = load (mode 3). word_EDD74 += 8 is the original's repeat-count correction.
    for (auto& w : ev.walk_units) {
        CombatUnitPlacement* u = unit_at(w.unit_index);
        if (u) u->facing = (int8_t)w.load;
    }
    // sub_508E6 does not reposition the camera. The scroll target set by the preceding VM 0x0F
    // command is left alone, and sub_4559F -> sub_5046B advances it on every drop frame.
    game_timer_set(0x0C);          // `mov eax,0Ch; call sub_1033B`

    // unk_E5A50 starts at -count1 and increments every even frame; 0..7 renders the drop
    // (sub_50216), negative values draw nothing, and 8 is the landing (sub_11F74 dust; the pose is
    // kept and [64h] is not cleared). Total frames = 2*EDD74+1 with EDD74 = max(2*count1+1)+8
    // (the per-group var_14 maximum).
    // Deviation: the K==0 walk-start delay (count1+1 even frames) does not occur in 1-5 and is
    // not ported.
    int max_var14 = 1;
    for (const auto& w : ev.walk_units)
        max_var14 = std::max(max_var14, 2 * w.spawn_delay + 1);
    for (auto& w : ev.walk_units) {
        CombatUnitPlacement* u = unit_at(w.unit_index);
        if (!u) continue;
        u->anim_counter = 0;
        // [64h] is not touched by the drop path (sub_508E6), so pose 3 set at activation (the
        // sub_16D3E prologue, idle without a weapon) is kept. Zero would draw the armed idle frame
        // after landing.
        u->anim_frame = 3;
        u->drop_wait = w.spawn_delay;         // E5A50 < 0 wait window (draws nothing)
        u->drop_step = w.spawn_delay > 0 ? -1 : 7;
        u->drop_sil = w.spawn_delay > 0 ? 0 : 9;
        u->walk_arrived = false;
    }
    const int frames = 2 * (max_var14 + 8) + 1;
    const bool fall_trace = DevUtil::spawn_fall_trace();
    auto land_all = [&]() {
        for (auto& w : ev.walk_units) {
            CombatUnitPlacement* u = unit_at(w.unit_index);
            if (!u) continue;
            u->drop_wait = 0;
            u->drop_step = -1;
            u->drop_sil = 0;
            u->anim_frame = 3;
            u->walk_arrived = true;
        }
    };
    for (int f = 0; f < frames; ++f) {
        for (auto& w : ev.walk_units) {
            CombatUnitPlacement* u = unit_at(w.unit_index);
            if (!u) continue;
            if ((f & 1) == 0) {                    // loc_5096A: test di,1 - every even frame
                if (u->drop_wait > 0) {
                    --u->drop_wait;                // E5A50++ (-count1 -> 0)
                } else if (u->anim_counter < 7) {
                    ++u->anim_counter;
                    u->drop_step = 7 - (int)u->anim_counter;
                    u->drop_sil = 9 - (int)u->anim_counter;   // 5F517 cl = 9 - dx
                } else if (u->anim_counter == 7) {
                    // loc_509B2: sub_11F74 dust and a one-tile landing; [64h] is not written (pose
                    // 3 kept)
                    u->anim_counter = 8;
                    u->anim_frame = 3;
                    u->drop_step = -1;
                    u->drop_sil = 0;
                    u->walk_arrived = true;
                }
            }
            if (fall_trace)
                fprintf(stderr, "COMBAT: f=%d unit=%d cnt=%d wait=%d step=%d sil=%d tile=(%d,%d)\n",
                        f, w.unit_index, (int)u->anim_counter, u->drop_wait,
                        u->drop_step, u->drop_sil, u->x, u->y);
        }
        camera_update_frame();      // sub_4559F
        present_cutscene_frame();   // sub_5E9E8 + sub_499E4
        if (bump_input()) { land_all(); return; }
        wait_game_tick();
    }
    // After landing the unit is gone from dword_CC968 (normal draw path), so clear the step offset.
    for (auto& w : ev.walk_units) {
        CombatUnitPlacement* u = unit_at(w.unit_index);
        if (!u) continue;
        u->drop_step = -1;
        u->drop_sil = 0;
        u->anim_frame = 3;         // keep activation pose 3 (the original leaves [64h] alone after landing too)
        u->walk_arrived = true;
    }
    camera_update_frame();
    present_cutscene_frame();
}

// ==== 0x04 retreat: second half of sub_51449 + sub_17071 ====
// sub_51449 builds a walk path for each record (sup > mode 0) and hands over to sub_17071.
//
// ---- path generation (loc_51557, at most 0Ch points) ----
//   Each step moves the position with sub_49ABC(p1, p2, dir) (jump table: 0 = y+1 south,
//   1 = x-1 west, 2 = y-1 north, 3 = x+1 east). If the new position hits any rule below, that
//   step is not stored and the path ends (the 0xFF terminator takes its place):
//     x < cam_x | x > cam_x+0Bh | y < cam_y | y > cam_y+0Bh   (12x12 tile screen window)
//     x-2 >= map width - 4 | y-4 >= map height - 8            (map border)
//   The unit therefore walks only inside the current camera window, and the moment it would
//   leave the window is when it disappears (1-1A uses dir=0, so it leaves through the bottom).
//   Frame count word_EDD74 = max(stored points - unk_E5A50 + 2), and unk_E5A50 = -record byte 4
//   (when nonzero the original locks state=2 and does nothing).
//
// ---- presentation (sub_17071) ----
//   The prologue matches sub_16D3E (loc_16D78): timer 9, poses 2/3 with tables
//   {3,4,3,4}/{2,3,4,3}, and unk_E5A70=3. Every frame:
//     (a) while state is 0, aim at path[unk_E5A50], update facing [63h] and increment unk_E5A50.
//         A target of 0FFh sets state=1 (arrived).
//     (b) 4 sub-frames of { sub_4559F (map + camera/window) -> sub_5E9E8 (present) ->
//         sub_499E4 (one tick) }: state==1 removes the unit from the battle at that point and
//         sets state=2; state==0 advances the pose through sub_16AEF. The tile advance happens
//         after sub-frame 1 (the +-1 tile of sub_16C93), so exactly one tile is crossed over the
//         four sub-frames.
void CombatII::play_retreat(const VmEvent& ev) {
    if (ev.leave_units.empty()) { present_cutscene_frame(); return; }
    battle_wc1_occupancy_unknown("VM 0x04 retreat/removal", ev.offset); // defensive at the actual mutation path
    const bool retreat_trace = DevUtil::retreat_trace();
    auto unit_at = [&](int i) -> CombatUnitPlacement* {
        return (i >= 0 && (size_t)i < units_.size()) ? &units_[(size_t)i] : nullptr;
    };
    auto bump_input = [&]() -> bool {   // true = exit or skip
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return true; }
        if (input_.is_key_pressed(SDLK_ESCAPE)) { skip_cutscene_ = true; return true; }
        return false;
    };

    // Mirrors the original's var_44 state array plus the dword_F0648 list.
    struct RetreatUnit {
        int ui = -1;                              // dword_F0648[i] (index into units_)
        int dir = 0;                              // record byte 5 (sub_49ABC jump table)
        int step = 0;                             // unk_E5A50[i] = -record byte 4
        int state = 2;                            // var_44[i]: 0 = walking, 1 = arrived, 2 = done
        bool target_valid = false;                // whether this frame has a target (path[step])
        int target_x = 0, target_y = 0;
        std::vector<std::pair<int,int>> path;
    };
    std::vector<RetreatUnit> ents;
    ents.reserve(ev.leave_units.size());
    for (size_t k = 0; k < ev.leave_units.size(); ++k) {
        if (!unit_at(ev.leave_units[k])) continue;
        RetreatUnit e;
        e.ui = ev.leave_units[k];
        e.dir = (k < ev.leave_dirs.size()) ? (ev.leave_dirs[k] & 3) : 0;
        e.step = (k < ev.leave_steps.size()) ? ev.leave_steps[k] : 0;
        ents.push_back(std::move(e));
    }
    if (ents.empty()) { present_cutscene_frame(); return; }

    // loc_5146D: centre the camera on the first record's unit (sub_1124A); path checks use it.
    if (CombatUnitPlacement* u0 = unit_at(ents[0].ui)) camera_center_on(u0->x, u0->y);

    // loc_514FA~loc_51625: build the paths and word_EDD74.
    int edd74 = 0;
    for (auto& e : ents) {
        CombatUnitPlacement* u = unit_at(e.ui);
        if (!u) continue;
        const int sx = u->x, sy = u->y;
        int x = sx, y = sy, k = 0;
        while (k < 12) {
            switch (e.dir) {                        // sub_49ABC jump table
            case 0: y += 1; break;                  // south
            case 1: x -= 1; break;                  // west
            case 2: y -= 1; break;                  // north
            default: x += 1; break;                 // east
            }
            if (x < cam_tile_x_) break;             // cmp ax, word dword_BA83C ; jl
            if (x > cam_tile_x_ + 11) break;        // cmp edx, cam_x+0Bh ; jg
            if (y < cam_tile_y_) break;
            if (y > cam_tile_y_ + 11) break;
            if (x - 2 >= (int)map_w_ - 4) break;    // word dword_BA842+2 = map width
            if (y - 4 >= (int)map_h_ - 8) break;    // word_BA840 = map height
            e.path.push_back({ x, y });
            ++k;
        }
        // Only units with unk_E5A50 == 0 walk (the prologue locks state=2 otherwise).
        e.state = (e.step == 0) ? 0 : 2;
        const int len = (int)e.path.size() - e.step + 2;
        if (len > edd74) edd74 = len;
        if (retreat_trace)
            fprintf(stderr, "COMBAT: unit %d tile=(%d,%d) dir=%d step=%d path=%zu state=%d\n",
                    u->id, sx, sy, e.dir, e.step, e.path.size(), e.state);
    }

    // loc_5168C: mode != 0 leaves sub_17071. Mode 1 calls sub_508E6(4) (light pillar fades),
    // anything else calls sub_50AA7 (removal modes 2/3/4). Both use the EDD74 computed above.
    if (ev.retreat_mode != 0) {
        if (ev.retreat_mode == 1) play_retreat_pillar_508e6(ev, edd74);
        else battle_force_retreat_50aa7(ev);
        return;
    }

    // sub_17071 prologue: `mov eax,9; call sub_1033B` plus pose table/pose 3 and
    // `sub_45559`/`sub_499E4`.
    game_timer_set(9);
    for (auto& e : ents) {
        CombatUnitPlacement* u = unit_at(e.ui);
        if (!u) continue;
        u->anim_counter = 3;
        u->anim_phase = 0;
        u->anim_frame = (u->code_b >= 0x0F && u->code_b <= 0x16) ? 2 : 3;
        u->walk_off_x = u->walk_off_y = 0;
    }
    camera_update_frame();
    present_cutscene_frame();

    size_t remaining = 0;
    for (const auto& e : ents) if (e.state == 0) ++remaining;
    fprintf(stderr,
            "COMBAT: play retreat (mode 0, %zu unit(s), EDD74=%d -> %d frames = %.2fs, %zu walking)\n",
            ents.size(), edd74, edd74 * 4, edd74 * 4.0 * 9.0 / kTimerHz, remaining);

    for (int f = 0; f < edd74; ++f) {
        // (a) target/facing/step advance (loc_17119 ~ loc_171C1)
        for (auto& e : ents) {
            e.target_valid = false;
            if (e.step == 0) e.state = 0;            // cmp unk_E5A50[eax],0; jnz -> state unchanged
            if (e.state != 0) continue;
            CombatUnitPlacement* u = unit_at(e.ui);
            if (!u) { e.state = 2; continue; }
            if (e.step >= 0 && e.step < (int)e.path.size()) {
                e.target_x = e.path[(size_t)e.step].first;
                e.target_y = e.path[(size_t)e.step].second;
                e.target_valid = true;
                const int8_t fd = walk_facing_toward(u->x, u->y, e.target_x, e.target_y);
                if (fd >= 0) u->facing = fd;
            } else {
                e.state = 1;                         // end of path (0FFh) -> arrived -> removed this frame
            }
            e.step++;                                // inc unk_E5A50[eax]
            if (retreat_trace && e.target_valid)
                fprintf(stderr, "COMBAT: f=%d unit %d target=(%d,%d) face=%d\n",
                        f, u->id, e.target_x, e.target_y, (int)u->facing);
        }
        // (b) 4 sub-frames (loc_171D2 loop)
        for (int sub = 0; sub < 4; ++sub) {
            for (auto& e : ents) {
                if (!e.target_valid) continue;
                CombatUnitPlacement* u = unit_at(e.ui);
                if (!u) continue;
                // A quarter tile per sub-frame (6px/4px). Rendering happens before the update,
                // so sub=0 is the starting position (same rule as animate_unit_walk).
                u->walk_off_x = (e.target_x - u->x) * TOWN_TILE_W * sub / 4;
                u->walk_off_y = (e.target_y - u->y) * TOWN_TILE_H * sub / 4;
            }
            camera_update_frame();        // sub_4559F: map + camera/window
            present_cutscene_frame();     // sub_5E9E8
            dump_retreat_frame(f, sub);   // diagnostics (G2_RETREAT_DUMP)
            if (bump_input()) return;
            wait_game_tick();             // sub_499E4
            for (auto& e : ents) {
                CombatUnitPlacement* u = unit_at(e.ui);
                if (!u) continue;
                if (e.state == 1) {
                    // loc_171E8: sub_11EE4(x,y,U) plus removal from the legion list; the unit
                    // leaves the field.
                    u->active = false;
                    u->walk_off_x = u->walk_off_y = 0;
                    e.state = 2;
                    if (remaining) --remaining;
                    if (retreat_trace)
                        fprintf(stderr, "COMBAT: unit %d removed at (%d,%d) (f=%d sub=%d)\n",
                                u->id, u->x, u->y, f, sub);
                } else if (e.state == 0) {
                    // sub_16AEF -> sub_16A6D: advance the pose table one step every 2 sub-frames
                    u->anim_phase = (uint8_t)(u->anim_phase + 1);
                    if (u->anim_phase >= 2) {
                        u->anim_phase = 0;
                        u->anim_counter = (uint8_t)((u->anim_counter + 1) & 3);
                        u->anim_frame = walk_pose_table(u->code_b, u->anim_counter);
                    }
                }
            }
        }
        // (c) end of the iteration: commit the tile towards this frame's target (the +-1 tile of
        // sub_16C93)
        for (auto& e : ents) {
            CombatUnitPlacement* u = unit_at(e.ui);
            if (!u) continue;
            if (e.target_valid) { u->x = e.target_x; u->y = e.target_y; }
            u->walk_off_x = u->walk_off_y = 0;
        }
    }
    // Safety margin: units still alive when the presentation ends are removed from the battle
    // (the original removes them at the end of the path).
    for (auto& e : ents) {
        CombatUnitPlacement* u = unit_at(e.ui);
        if (!u || e.state != 0) continue;
        u->active = false;
        u->walk_off_x = u->walk_off_y = 0;
    }
    camera_update_frame();
    present_cutscene_frame();
}

// sub_508E6(4): light-pillar retreat. The prologue sets unk_E5A50 = 7 - unk_E5A50 (the value
// written by sub_51449 is -record byte 4) and adds 8 to word_EDD74. For 2*(EDD74+8)+1 frames it
// runs sub_4559F, draws each unit through sub_50216 at the current counter, and on every even
// frame decrements the counter; at 6 the unit leaves the list (sub_197AD/113E4/113CC + 11F74).
void CombatII::play_retreat_pillar_508e6(const VmEvent& ev, int edd74) {
    struct PillarUnit { int ui = -1; int counter = 7; bool removed = false; };
    std::vector<PillarUnit> ps;
    ps.reserve(ev.leave_units.size());
    for (size_t k = 0; k < ev.leave_units.size(); ++k) {
        const int ui = ev.leave_units[k];
        if (ui < 0 || (size_t)ui >= units_.size()) continue;
        const int step = (k < ev.leave_steps.size()) ? ev.leave_steps[k] : 0; // unk_E5A50
        ps.push_back({ui, 7 - step, false});          // mov bl,7; sub bl,bh
        units_[(size_t)ui].drop_wait = 1;             // hidden until the counter reaches 7
        units_[(size_t)ui].drop_step = -1;
        units_[(size_t)ui].drop_sil = 0;
    }
    if (ps.empty()) { present_cutscene_frame(); return; }

    game_timer_set(0x0C);                             // mov eax,0Ch; call sub_1033B
    auto refresh = [&]() {
        for (auto& p : ps) {
            if (p.ui < 0 || (size_t)p.ui >= units_.size()) continue;
            auto& u = units_[(size_t)p.ui];
            const int c = p.counter;
            // sub_50216 is called with edx = E5A50 (0..7 after the 7-E5A50 prologue). The
            // renderer's drop path reconstructs dx = 7 - drop_step, so drop_step = 7 - c.
            // 5F517 fills with cl = 9 - dx (see 50216+F3), hence drop_sil = 9 - c.
            if (c >= 0 && c <= 7) {
                u.drop_wait = 0;
                u.drop_step = 7 - c;
                u.drop_sil = 9 - c;
            } else {
                u.drop_wait = 1;                      // dx < 0 or > 7: 50216 draws nothing
                u.drop_step = -1;
                u.drop_sil = 0;
            }
        }
    };
    refresh();
    const int total = 2 * (edd74 + 8);                // di = word_EDD74(+=8) * 2
    for (int f = 0; f <= total && !should_exit_; ++f) {
        camera_update_frame();                        // sub_4559F
        present_cutscene_frame();                     // sub_50216 + sub_5E9E8
        if ((f & 1) == 0) {                           // test di,1 (same parity as the frame index)
            for (auto& p : ps) {
                if (p.removed) continue;
                --p.counter;                          // dec al
                if (p.counter == 6) {                 // cmp al,6; jne (mode 4 path 509EC)
                    // 197AD/113E4/113CC remove the unit from the ready, unit and army lists, but
                    // the draw loop keeps calling 50216 with the dword_F0648 pointer, so the
                    // port must keep drawing it until the counter drops below 0.
                    battle_remove_from_ready_queue(p.ui); // sub_197AD
                    p.removed = true;
                }
            }
        }
        refresh();
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; break; }
        if (input_.is_key_pressed(SDLK_ESCAPE)) skip_cutscene_ = true;
        wait_game_tick();                             // sub_499E4
    }
    // Mode 4 tail: 119B6 image-slot release for every listed unit without [1E]&40h (no-op in
    // the port), then 4559F + 5E9E8.
    for (auto& p : ps) {
        if (p.ui < 0 || (size_t)p.ui >= units_.size()) continue;
        auto& u = units_[(size_t)p.ui];
        u.active = false;                             // 113E4/113CC
        u.drop_wait = 0;
        u.drop_step = -1;
        u.drop_sil = 0;
    }
    camera_update_frame();
    present_cutscene_frame();
    if (DevUtil::retreat_trace())
        fprintf(stderr, "COMBAT: play retreat pillar (mode 1, %zu unit(s), EDD74=%d -> %d frames)\n",
                ps.size(), edd74, total + 1);
}

// sub_50AA7: removal of the units listed in dword_F0648. Units inside the 12x12 viewport run
// their mode's effect first (2 = sub_46710 wipe, 3 = sub_468BC shrink, 4 = sub_46A11 dissolve);
// units outside it are removed without an effect. Every unit then leaves the ready queue
// (sub_197AD), the army list (sub_113E4/113CC) and releases its image slot (sub_119B6).
void CombatII::battle_force_retreat_50aa7(const VmEvent& ev) {
    const int mode = ev.retreat_mode;
    for (int ui : ev.leave_units) {
        if (ui < 0 || (size_t)ui >= units_.size()) continue;
        auto& u = units_[(size_t)ui];
        const int x = (int)(int8_t)u.x, y = (int)(int8_t)u.y;
        const bool in_view = x >= cam_tile_x_ && x < cam_tile_x_ + 12 &&
                             y >= cam_tile_y_ && y < cam_tile_y_ + 12;
        if (in_view && !should_exit_) {
            if (mode == 2) battle_fx_46710(ui);
            else if (mode == 3) battle_fx_468BC(ui);
            else if (mode == 4) {
                u.active = false;                     // sub_11EE4
                battle_fx_46a11(ui);
            }
        }
        if (should_exit_ || (size_t)ui >= units_.size()) continue;
        units_[(size_t)ui].active = false;            // sub_113E4 / sub_113CC
        units_[(size_t)ui].death_dissolve = 0;
        battle_remove_from_ready_queue(ui);           // sub_197AD
    }
    // sub_50AA7's tail is just the epilogue (0x50BAA -> 0x50AA1): no present here.
    if (DevUtil::retreat_trace())
        fprintf(stderr, "COMBAT: play retreat removal mode %d: %zu unit(s)\n",
                mode, ev.leave_units.size());
}

void CombatII::animate_unit_walk(const VmEvent& ev) {
    if (ev.walk_units.empty()) { present_cutscene_frame(); return; }
    const bool walk_trace = DevUtil::walk_trace();
    auto unit_at = [&](int i) -> CombatUnitPlacement* {
        return (i >= 0 && (size_t)i < units_.size()) ? &units_[(size_t)i] : nullptr;
    };
    auto bump_input = [&]() -> bool {   // true = exit or skip
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return true; }
        if (input_.is_key_pressed(SDLK_ESCAPE)) { skip_cutscene_ = true; return true; }
        return false;
    };

    if (ev.walk_group == 0) {
        // VM 0x05 group-0 handler writes each unit's starting tile before sub_16D3E; replay that
        // write here, not during parse.
        if (ev.walk_sets_start_tile) {
            for (const auto& w : ev.walk_units) {
                CombatUnitPlacement* u = unit_at(w.unit_index);
                if (!u || w.path.empty()) continue;
                u->x = w.path.front().first;
                u->y = w.path.front().second;
            }
        }
        const int steps = std::max(1, ev.walk_frames);   // word_EDD74
        game_timer_set(9);                               // `mov eax,9; call sub_1033B`
        // sub_16D3E prologue (loc_16D78): classes 0Fh..16h use pose 2 and table {3,4,3,4};
        // otherwise pose 3 and table {2,3,4,3}. Counter unk_E5A70 starts at 3.
        for (auto& w : ev.walk_units) {
            CombatUnitPlacement* u = unit_at(w.unit_index);
            if (!u) continue;
            u->anim_counter = 3;
            u->anim_phase = 0;
            u->walk_arrived = false;
            u->anim_frame = (u->code_b >= 0x0F && u->code_b <= 0x16) ? 2 : 3;
        }
        for (int iter = 0; iter < steps; ++iter) {
            const size_t next = (size_t)iter + 1;
            // loc_16E1A: update facing towards this iteration's target (unk_E5210[iter] =
            // path[iter+1]).
            // Units whose path ended (0xFF) are left alone here; loc_16F57 in the sub-frame
            // update sets [63h] = load for them.
            for (auto& w : ev.walk_units) {
                CombatUnitPlacement* u = unit_at(w.unit_index);
                if (!u) continue;
                if (next < w.path.size()) {
                    int8_t f = walk_facing_toward(u->x, u->y, w.path[next].first, w.path[next].second);
                    if (f >= 0) u->facing = f;
                }
            }
            for (int sub = 0; sub < 4; ++sub) {
                for (auto& w : ev.walk_units) {
                    CombatUnitPlacement* u = unit_at(w.unit_index);
                    if (!u) continue;
                    if (next < w.path.size()) {
                        const int dx = w.path[next].first  - w.path[iter].first;
                        const int dy = w.path[next].second - w.path[iter].second;
                        u->x = w.path[(size_t)iter].first;
                        u->y = w.path[(size_t)iter].second;
                        // sub_16A6D: 6px horizontally / 4px vertically per sub-frame (tile units /
                        // 4).
                        // Rendering happens before the update, so sub=0 is the starting position.
                        u->walk_off_x = dx * TOWN_TILE_W * sub / 4;
                        u->walk_off_y = dy * TOWN_TILE_H * sub / 4;
                    } else {
                        u->walk_off_x = u->walk_off_y = 0;
                    }
                }
                if (walk_trace)
                    for (const auto& w : ev.walk_units) {
                        const CombatUnitPlacement* u = unit_at(w.unit_index);
                        if (u) fprintf(stderr, "COMBAT: iter=%d sub=%d tile=(%d,%d) off=(%d,%d) face=%d pose=%u\n",
                                       iter, sub, u->x, u->y, u->walk_off_x, u->walk_off_y,
                                       u->facing, (unsigned)u->anim_frame);
                    }
                camera_update_frame();        // sub_4559F
                present_cutscene_frame();     // sub_5E9E8
                if (bump_input()) return;
                wait_game_tick();             // sub_499E4
                // ---- per-unit update after rendering (original order) ----
                for (auto& w : ev.walk_units) {
                    CombatUnitPlacement* u = unit_at(w.unit_index);
                    if (!u) continue;
                    if (next < w.path.size()) {
                        // sub_16AEF -> sub_16A6D: advance the pose table every 2 sub-frames
                        // (`unk_E5A70 = (x+1)&3` when `unk_E5AB0++ == 2`).
                        u->anim_phase = (uint8_t)(u->anim_phase + 1);
                        if (u->anim_phase >= 2) {
                            u->anim_phase = 0;
                            u->anim_counter = (uint8_t)((u->anim_counter + 1) & 3);
                            u->anim_frame = walk_pose_table(u->code_b, u->anim_counter);
                        }
                    } else if (!u->walk_arrived) {
                        // loc_16F57: [63h] = unk_E5AD0 (load), [64h] = (class != 0) ? 2 : 0.
                        u->facing = (int8_t)w.load;
                        u->anim_frame = (u->code_b != 0) ? 2 : 0;
                        u->walk_arrived = true;
                    } else {
                        // loc_16F39: [64h] = 0 (standing pose).
                        u->anim_frame = 0;
                    }
                }
            }
            // End of the iteration: commit the tile position to the next point and clear the
            // offset.
            for (auto& w : ev.walk_units) {
                CombatUnitPlacement* u = unit_at(w.unit_index);
                if (!u) continue;
                if (next < w.path.size()) {
                    u->x = w.path[next].first;
                    u->y = w.path[next].second;
                }
                u->walk_off_x = u->walk_off_y = 0;
            }
        }
    } else {
        // sub_50BAF: the handler does not touch the start position, so it is the tile the unit
        // stands on.
        const int total_frames = 2 * (std::max(1, ev.walk_frames) + 8);
        game_timer_set(0x0C);
        // loc_50BCC: facing becomes load at the start and is kept for the whole move.
        for (auto& w : ev.walk_units) {
            CombatUnitPlacement* u = unit_at(w.unit_index);
            if (u) u->facing = (int8_t)w.load;
        }
        for (int f = 0; f <= total_frames; ++f) {
            const double p = (double)std::min(f, 16) / 16.0;   // 8 steps x 2 frames
            for (auto& w : ev.walk_units) {
                CombatUnitPlacement* u = unit_at(w.unit_index);
                if (!u || w.path.empty()) continue;
                u->walk_off_x = (int)std::lround((w.path[0].first  - u->x) * TOWN_TILE_W * p);
                u->walk_off_y = (int)std::lround((w.path[0].second - u->y) * TOWN_TILE_H * p);
                // [64h] is left alone: sub_50BAF never writes a pose, so the unit holds one
                // posture for the whole move. The sense of motion comes from EVENT.SYB footprints
                // (the sub_50216 step argument -> the +9 sprite of the 12-byte unk_10059C entry)
                // and the unit silhouette (sub_5F517, colour = 9-step). The port has no effect
                // layer, so it interpolates the position instead (p above).
            }
            camera_update_frame();
            present_cutscene_frame();
            if (bump_input()) return;
            wait_game_tick();
        }
    }
    camera_update_frame();          // final `call sub_4559F; call sub_5E9E8` after the loop
    present_cutscene_frame();
    for (auto& w : ev.walk_units) {
        CombatUnitPlacement* u = unit_at(w.unit_index);
        if (!u) continue;
        if (!w.path.empty()) {
            u->x = w.path.back().first;
            u->y = w.path.back().second;
        }
        u->walk_off_x = u->walk_off_y = 0;
        u->facing = (int8_t)w.load;  // loc_16F57
    }
    present_cutscene_frame();
}

// sub_51211 activation: the pending units that run_vm built while scanning the bytecode ahead are
// added to the battle list here and their walk/drop animation plays, so they stay invisible during
// the preceding dialogue/camera frames and appear in the original command order.
void CombatII::battle_spawn_activate(const VmEvent& ev) {
        // Pending units built by run_vm's look-ahead become active in the battle list at this
        // point, so they stay invisible during the preceding dialogue/camera frames and appear in
        // the original command order.
        for (int ui : ev.spawn_units)
            if (ui >= 0 && (size_t)ui < units_.size()) {
                units_[(size_t)ui].active = true;
                units_[(size_t)ui].list_seq = battle_next_list_seq(); // sub_1134F tail insert
            }
        // The gate is evaluated after the reinforcement NC2 loads (loading happens while drawing
        // the
        // drop/walk animation). Right after activation a class-0 descriptor's 44 frames may not be
        // ready yet, so the gate is not closed too eagerly.
        if (ev.walk_units.empty()) {
            present_cutscene_frame();
            battle_wc1_occupancy_preserve_or_close("VM 0x03 spawn/list insertion", ev.offset);
            return;
        }
        if (ev.spawn_mode == 1) {
            // The var_10 != 0 branch at the end of sub_50D9F: sub_508E6(3) drop animation.
            fprintf(stderr, "COMBAT: play spawn fall (%zu unit(s))\n", ev.walk_units.size());
            play_spawn_fall(ev);
            if (should_exit_ || skip_cutscene_) return;
            battle_wc1_occupancy_preserve_or_close("VM 0x03 spawn/list insertion", ev.offset);
            return;
        }
        size_t pts = 0;
        for (const auto& w : ev.walk_units) pts = std::max(pts, w.path.size());
        fprintf(stderr,
                "COMBAT: play spawn walk (%zu unit(s), max %zu points, %d repeats x 4 subframes)\n",
                ev.walk_units.size(), pts, ev.walk_frames);
        animate_unit_walk(ev);
        if (should_exit_ || skip_cutscene_) return;
        battle_wc1_occupancy_preserve_or_close("VM 0x03 spawn/list insertion", ev.offset);
        return;
}

// S17 (sub_50D9F + sub_5069C + sub_51211)
// Original: 0x03 tests each record's destination with sub_1665B (terrain grid choice) and
//   sub_16699 (footprint, occupancy, terrain).
//   Mode 1 (drop) appears regardless of the test result. A record blocked in mode 0 goes into the
//   first free slot of sub_5069C's 11-byte queue (unk_1006BC, 32 slots) as
//   [record pointer][x][y][facing][unit], and is dropped when the queue is full.
//   sub_51211 re-tests slots 0..31 in order with sub_16699 every tick, adds the passing records to
//   the list ([0Eh]++, [60h]=1198D) and walks them all together with one sub_16D3E call. There is
//   no permanent cap.
// Destination = the last point of the walk path (or the record's start tile when there is none);
// facing = the load byte (E5AD0). (ASM 87890~87996)
void CombatII::battle_spawn_dest(const VmEvent& ev, int unit_index, int& x, int& y, int& facing) const {
    x = y = 0; facing = 0;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const CombatUnitPlacement& nu = units_[(size_t)unit_index];
    x = (int)(int8_t)nu.x; y = (int)(int8_t)nu.y; facing = (int)nu.facing & 3;
    for (const VmWalkUnit& w : ev.walk_units) {
        if (w.unit_index != unit_index) continue;
        if (!w.path.empty()) { x = w.path.back().first; y = w.path.back().second; }
        facing = w.load & 3;
        break;
    }
}

VmEvent CombatII::battle_spawn_subevent(const VmEvent& ev, const std::vector<int>& units) const {
    VmEvent out = ev;
    out.spawn_units = units;
    out.walk_units.clear();
    out.walk_frames = 0;
    for (const VmWalkUnit& w : ev.walk_units) {
        if (std::find(units.begin(), units.end(), w.unit_index) == units.end()) continue;
        out.walk_units.push_back(w);
        if (ev.spawn_mode == 0 && !w.path.empty())
            out.walk_frames = std::max(out.walk_frames, (int)w.path.size() + 1);   // word_EDD74
    }
    if (ev.spawn_mode != 0) out.walk_frames = ev.walk_frames;
    return out;
}

// sub_16699: for every footprint cell (in bounds -> CC998 not occupied -> terrain cost > 0).
bool CombatII::battle_16699(int x, int y, int facing, int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    if (map_w_ == 0 || map_h_ == 0) return false;
    const CombatUnitPlacement& nu = units_[(size_t)unit_index];
    int fw = 1, fh = 1;
    battle_d66a8_footprint(nu.sprite_type, facing & 3, &fw, &fh);        // [edx] / [edx+1]
    const int sel = battle_ai_mobility_selector(nu);                      // sub_1665B (EDD6C)
    for (int r = 0; r < fh; ++r) {
        const int yy = y - r;
        for (int c = 0; c < fw; ++c) {
            const int xx = x + c;
            if ((uint16_t)(xx - 2) >= (uint16_t)(map_w_ - 4)) return false;   // cmp cx, W-4; jge
            if ((uint16_t)(yy - 4) >= (uint16_t)(map_h_ - 8)) return false;   // cmp cx, H-8; jge
            for (size_t i = 0; i < units_.size(); ++i) {                      // cmp dword_CC998[i], 0
                if ((int)i == unit_index) continue;
                const auto& u = units_[i];
                if (!u.active || u.hp <= 0) continue;
                int w = 1, h = 1;
                battle_d66a8_footprint(u.sprite_type, (int)u.facing & 3, &w, &h);
                const int ux = (int)(int8_t)u.x, uy = (int)(int8_t)u.y;
                if (xx >= ux && xx < ux + w && yy <= uy && yy > uy - h) return false;
            }
            if (sel >= 0 && sel < kMobilityGridCount) {                       // cmp byte [EDD6C+attr], 0; jle
                const size_t idx = (size_t)yy * map_w_ + (size_t)xx;
                if (idx >= tile_attr_.size()) return false;
                if ((int8_t)kMobilityCostGrids[sel][tile_attr_[idx]] <= 0) return false;
            }
        }
    }
    return true;
}

// sub_5069C: store into the first free slot (original [+0]==0). When all 32 slots are full the
// record is dropped.
void CombatII::battle_spawn_enqueue(const VmEvent& single) {
    if (battle_spawn_queue_.size() < 32) battle_spawn_queue_.resize(32);
    for (PendingSpawn& ps : battle_spawn_queue_) {
        if (ps.used) continue;
        ps.ev = single;
        ps.used = true;
        return;
    }
}

// sub_51211: re-test queue slots 0..31 in order, then walk all passing records with one sub_16D3E.
void CombatII::battle_spawn_queue_step() {
    if (battle_spawn_queue_.empty()) return;
    std::vector<size_t> pass;
    for (size_t s = 0; s < battle_spawn_queue_.size() && s < 32; ++s) {
        PendingSpawn& ps = battle_spawn_queue_[s];
        if (!ps.used || ps.ev.spawn_units.empty()) continue;
        const int ui = ps.ev.spawn_units[0];
        int x = 0, y = 0, f = 0;
        battle_spawn_dest(ps.ev, ui, x, y, f);
        if (battle_16699(x, y, f, ui)) pass.push_back(s);
    }
    if (pass.empty()) return;
    VmEvent merged = battle_spawn_queue_[pass[0]].ev;
    merged.spawn_units.clear();
    merged.walk_units.clear();
    merged.walk_frames = 0;
    for (size_t s : pass) {
        PendingSpawn& ps = battle_spawn_queue_[s];
        for (int ui : ps.ev.spawn_units) merged.spawn_units.push_back(ui);
        for (const VmWalkUnit& w : ps.ev.walk_units) merged.walk_units.push_back(w);
        merged.walk_frames = std::max(merged.walk_frames, ps.ev.walk_frames);
        ps.used = false;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: sub_51211 activated=%zu\n", pass.size());
    battle_spawn_activate(merged);
}

void CombatII::play_vm_events(const std::vector<VmEvent>& events) {
    // VM payloads are decoded ahead of playback, but ordinary unit-list selection is stateful:
    // earlier Spawn/Faction/Leave/HP events must affect later selector opcodes in this same event
    // stream.
    // Selectors 10..7F instead use the captured dword_11024C pointer and validate its current army
    // byte.
    auto resolve_vm_unit_ref = [this](uint8_t sel, uint8_t id, int forced_context) -> int {
        // Selectors 1..3 use modeled effective army; selector 0 and aliased 4..9 retain group/raw
        // fallbacks.
        // Effective remap and CC990 membership are not stored for the paths that still use those
        // fallbacks.
        if (sel <= 9) {
            for (size_t i = 0; i < units_.size(); ++i) {
                const CombatUnitPlacement& unit = units_[i];
                if (!unit.active || unit.id != (int)id) continue;
                const bool army_matches = (sel == 0 && unit.group == 0) ||
                    (sel >= 1 && sel <= 3
                        ? battle_effective_army(unit) == (int)sel + 1
                        : sel != 0 && unit.army == (int)sel);
                if (army_matches) return (int)i;
            }
            return -1;
        }
        // sub_5019C checks effective [+0x0E] on the captured dword_11024C unit; raw+1 below is only
        // a fallback.
        if (sel <= 0x7F && forced_context >= 0 && (size_t)forced_context < units_.size() &&
            battle_effective_army(units_[(size_t)forced_context]) == (int)sel - 9)
            return forced_context;
        return -1; // VM unit-ref selector is outside the ordinary/forced range
    };
    for (const auto& ev : events) {
        if (should_exit_ || skip_cutscene_) return;
        switch (ev.type) {
        case VmEventType::OccupancyStateUnknown:
            // At the parser marker the reinforcement may not be active and its NC2 may not be
            // loaded, so the gate is not closed eagerly. It stays open while the gate still passes
            // and closes when it breaks. The spawn handler re-evaluates preserve-or-close after
            // activation and the animation.
            battle_wc1_occupancy_preserve_or_close(ev.text.c_str(), ev.offset);
            break;
        case VmEventType::Message:
            // As in sub_52B4F, only the window is reserved (no input wait). The remaining display
            // time is consumed by the following 0x0F mode 2 wait loop while it runs sub_5046B.
            message_window_set(ev.text, ev.msg_frames);
            fprintf(stderr, "COMBAT: play message %d frames: %s\n", ev.msg_frames,
                    log_text(ev.text).c_str());
            present_cutscene_frame();
            break;
        case VmEventType::Dialogue: {
            if (ev.text.empty()) { present_cutscene_frame(); break; }
            // sub_507BE -> end of sub_482A5: after drawing the line it waits for a key or 700 ticks
            // (~3.99s) through `mov eax,2BCh; call sub_499EF`. sub_5046B is not called during that
            // wait, so neither the camera nor the message window advances.
            // The original sub_482A5 reads the unit pointers [ebp+0/1], so it uses the live tiles
            // at
            // playback time.
            int stx = ev.speaker_tx, sty = ev.speaker_ty;
            if (ev.speaker_unit >= 0 && ev.speaker_unit < (int)units_.size()) {
                stx = units_[(size_t)ev.speaker_unit].x;
                sty = units_[(size_t)ev.speaker_unit].y;
            }
            set_ui_text(ev.text, false, ev.portrait, ev.speaker_name, stx, sty, ev.speaker_unit);
            present_cutscene_frame();
            // The stored text is Johab, which would print garbled, so log_text converts it to UTF-8
            // with Font::johab_to_utf8. On-screen rendering still uses Johab.
            fprintf(stderr, "COMBAT: play dialogue @0x%X (unit %d face %d tile=(%d,%d) name '%s'): %s\n",
                    ev.offset, ev.speaker_id, ev.portrait, stx, sty, log_text(ev.speaker_name, 40).c_str(),
                    log_text(ev.text).c_str());
            fprintf(stderr, "COMBAT: dialogue wait: key or 700 ticks (%.3fs), timer period=700\n",
                    700.0 / kTimerHz);
            if (wait_key_or_ticks(0x2BC)) return;
            set_ui_text("", false, -1);
            break;
        }
        case VmEventType::CameraScroll:
        case VmEventType::CameraUnit: {
            if (camera_fully_scrolling()) {
                // loc_52A2C: when both the X and Y frames are nonzero the update is queued instead
                // of
                // applied at once (up to 16 entries, until the read index catches up). loc_5050D
                // pops
                // them one at a time.
                cam_waypoints_.push_back({ ev.cam_mode, ev.cam_x, ev.cam_y, ev.cam_speed });
                fprintf(stderr, "COMBAT: play camera queue -> (%d,%d) speed=%d mode=%d (%zu queued) [cur tile=(%d,%d) frames=%d/%d]\n",
                        ev.cam_x, ev.cam_y, ev.cam_speed, ev.cam_mode, cam_waypoints_.size(),
                        cam_tile_x_, cam_tile_y_, (int)cam_scroll_frames_x_, (int)cam_scroll_frames_y_);
            } else {
                camera_setup(ev.cam_mode, ev.cam_x, ev.cam_y, ev.cam_speed);
                fprintf(stderr, "COMBAT: play camera start -> (%d,%d) speed=%d mode=%d%s (frames %d/%d)\n",
                        ev.cam_x, ev.cam_y, ev.cam_speed, ev.cam_mode,
                        ev.type == VmEventType::CameraUnit ? " [unit]" : "",
                        (int)cam_scroll_frames_x_, (int)cam_scroll_frames_y_);
            }
            present_cutscene_frame();
            break;
        }
        case VmEventType::CameraWait: {
            // Exact port of the sub_52940 mode 2 wait loop (loc_52AB6 / loc_52AE2).
            // Before entering, the original runs `mov eax,9; call sub_1033B`, lowering the
            // game-timer
            // period to 9, so each frame is 9/175.47 = 51.3ms, about 19.5fps.
            game_timer_set(9);
            //   B == 0 -> `call sub_454B3; call sub_5046B; test ax,ax; jnz`: loop until a nonzero
            //            return (1 = done, 2 = queue consumed)
            //   B != 0 -> `cmp ax,1; jz` (exit on 1) plus `dec [var_4]` and `jg` each pass, so it
            //            runs B times or exits earlier
            // The original order per frame is `sub_454B3 (map) -> sub_5046B (step) -> test ->
            // sub_5E9E8 (present) -> sub_499E4 (one tick)`, so the step runs before drawing.
            int frames = 0;
            int remaining = ev.cam_count;
            const bool wait_until_done = (remaining == 0);
            bool done = false;
            while (!done) {
                int r = camera_update_frame();
                present_cutscene_frame();
                frames++;
                if (wait_until_done) {
                    if (r != 0) done = true;   // loc_52AB6: test ax,ax; jnz loc_52B18
                } else if (r == 1) {
                    done = true;               // loc_52AE2: cmp ax,1; jz loc_52B18
                } else {
                    remaining--;               // dec var_4
                    if (remaining <= 0) done = true; // loc_52B11: cmp var_4,0; jg
                }
                if (done) break;
                wait_game_tick();              // sub_499E4
                if (should_exit_ || skip_cutscene_) break;
            }
            // loc_52B18: one more frame is drawn with `sub_4559F` + `sub_5E9E8` after the wait.
            if (!should_exit_ && !skip_cutscene_) {
                camera_update_frame();
                present_cutscene_frame();
            }
            fprintf(stderr, "COMBAT: play camera wait (B=%d, %d frames = %.2fs, %d/%d remaining)\n",
                    ev.cam_count, frames, (double)frames * 9.0 / kTimerHz,
                    (int)cam_scroll_frames_x_, (int)cam_scroll_frames_y_);
            break;
        }
        case VmEventType::UnitMove: {
            // With no records the original also skips the presentation (loc_5192F's `test ah,ah;
            // jle`).
            if (ev.walk_units.empty()) { present_cutscene_frame(); break; }
            const char* occupancy_reject_reason = nullptr;
            const bool preserve_occupancy = battle_wc1_group0_move_preserves_occupancy(
                ev, &occupancy_reject_reason);
            if (!preserve_occupancy)
                battle_wc1_occupancy_unknown(occupancy_reject_reason
                    ? occupancy_reject_reason : "VM 0x05 scripted movement", ev.offset);
            else if (battle_trace_ || script_trace_)
                fprintf(stderr, "COMBAT: WC1 occupancy pre-move proof passed for VM 0x05 group-0 batch @0x%X\n",
                        ev.offset);
            if (ev.walk_group == 0) {
                int steps = 1; size_t pts = 0;
                for (const auto& w : ev.walk_units) {
                    steps = std::max(steps, std::max(1, ev.walk_frames));
                    pts = std::max(pts, w.path.size());
                }
                fprintf(stderr,
                        "COMBAT: play unit move (group 0, %zu unit(s), max %zu points, "
                        "%d repeats x 4 subframes x 9 ticks = %d frames = %.2fs)\n",
                        ev.walk_units.size(), pts, steps, steps * 4,
                        steps * 4.0 * 9.0 / kTimerHz);
            } else {
                fprintf(stderr,
                        "COMBAT: play unit move (group %d, %zu unit(s), 2-point interpolation, "
                        "%d+1 frames x 12 ticks = %.2fs)\n",
                        ev.walk_group, ev.walk_units.size(),
                        2 * (std::max(1, ev.walk_frames) + 8),
                        (2 * (std::max(1, ev.walk_frames) + 8) + 1) * 12.0 / kTimerHz);
            }
            animate_unit_walk(ev);
            if (should_exit_ || skip_cutscene_) {
                if (preserve_occupancy)
                    battle_wc1_occupancy_unknown("VM 0x05 scripted movement interrupted", ev.offset);
                return;
            }
            bool moved_to_proven_endpoints = preserve_occupancy;
            if (moved_to_proven_endpoints) {
                for (const VmWalkUnit& walk : ev.walk_units) {
                    if (walk.unit_index < 0 || (size_t)walk.unit_index >= units_.size() || walk.path.empty()) {
                        moved_to_proven_endpoints = false;
                        break;
                    }
                    const CombatUnitPlacement& moved = units_[(size_t)walk.unit_index];
                    if (!moved.active || moved.x != walk.path.back().first || moved.y != walk.path.back().second) {
                        moved_to_proven_endpoints = false;
                        break;
                    }
                }
            }
            if (preserve_occupancy && (!moved_to_proven_endpoints ||
                                       !battle_wc1_static_occupancy_unambiguous()))
                battle_wc1_occupancy_unknown("VM 0x05 post-move occupancy proof failed", ev.offset);
            else if (preserve_occupancy && (battle_trace_ || script_trace_))
                fprintf(stderr, "COMBAT: WC1 occupancy preserved after VM 0x05 group-0 batch @0x%X\n",
                        ev.offset);
            // sub_516D0/sub_16D3E returns to loc_50AA1 without calling sub_5401B. The original
            // area/region trigger is checked only by actual movement call sites (AI/player walks),
            // not by this scripted cutscene displacement. Keep this playback trigger-free.
            for (const VmWalkUnit& w : ev.walk_units) {
                if (w.unit_index < 0 || (size_t)w.unit_index >= units_.size()) continue;
                const CombatUnitPlacement& moved = units_[(size_t)w.unit_index];
                if (!moved.active) continue;
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: scripted move complete unit=%d pos=(%d,%d) (triggers are not checked - VM 0x05 does not call sub_5401B in the disassembly)\n",
                            moved.id, moved.x, moved.y);
            }
            break;
        }
        case VmEventType::AiSettings: {
            const int owner_index = resolve_vm_unit_ref(ev.unit_ref_sel, ev.unit_ref_id,
                                                        ev.forced_target_context);
            if (owner_index >= 0 && (size_t)owner_index < units_.size()) {
                CombatUnitPlacement& unit = units_[(size_t)owner_index];
                unit.ai_decision_state = ev.ai_bytes[0];       // [unit+6Ch]
                unit.ai_field_6d = ev.ai_bytes[1];            // [unit+6Dh]
                unit.ai_field_6e = ev.ai_bytes[2];            // [unit+6Eh]
                if (ev.unit_ref_sel != 0xFF) {
                    unit.ai_field_80 = ev.ai_bytes[3];         // [+80h]: written only when the raw selector byte is not FFh
                    unit.ai_mode = (int8_t)ev.ai_bytes[4];     // [+82h]: the condition comes from the first selector byte, not the payload
                }
                if (script_trace_)
                    fprintf(stderr, "COMBAT: play vm ai unit=%d state=%u fields=(%u,%u) field80=%u mode=%d%s\n",
                            unit.id, unit.ai_decision_state, unit.ai_field_6d, unit.ai_field_6e,
                            unit.ai_field_80, (int)unit.ai_mode,
                            ev.unit_ref_sel == 0xFF ? " (selector=FF: field80/mode unchanged)" : "");
            }
            break;
        }
        case VmEventType::AiTarget: {
            const int owner_index = resolve_vm_unit_ref(ev.unit_ref_sel, ev.unit_ref_id,
                                                        ev.forced_target_context);
            if (owner_index >= 0 && (size_t)owner_index < units_.size()) {
                CombatUnitPlacement& owner = units_[(size_t)owner_index];
                if (ev.target_sub == 0) {
                    owner.ai_home_x = ev.target_a;
                    owner.ai_home_y = ev.target_b;
                } else if (ev.target_sub == 1) {
                    // When sub_52377 runs it passes [a][b] to sub_11DB2 as signed coordinates.
                    owner.ai_target_unit = battle_unit_index_at((int)(int8_t)ev.target_a,
                                                               (int)(int8_t)ev.target_b);
                } else if (ev.target_sub == 2) {
                    // The sub_501BA selector is also resolved when this opcode runs: the normal
                    // list
                    // looks only at active units, while a forced selector checks the current legion
                    // of the captured dword_11024C pointer. The high byte of a VM unit-ref selector
                    // is
                    // not modelled, so the existing +73 value is left alone (separate from AI
                    // CC960).
                    if (ev.target_a <= 0x7F)
                        owner.ai_target_unit = resolve_vm_unit_ref(ev.target_a, ev.target_b,
                                                                   ev.forced_target_context);
                }
                if (script_trace_)
                    fprintf(stderr, "COMBAT: play vm target unit=%d sub=%u (%u,%u) -> home=(%u,%u) watch=(%u,%u) target=%d\n",
                            owner.id, ev.target_sub, ev.target_a, ev.target_b,
                            owner.ai_home_x, owner.ai_home_y, owner.ai_watch_sel,
                            owner.ai_watch_id, owner.ai_target_unit);
            }
            break;
        }
        case VmEventType::Spawn: {
            // S17 (sub_50D9F): mode 1 (drop) appears without a test. In mode 0 every record's
            // destination is tested with sub_16699; the passing records appear together and blocked
            // records enter the sub_5069C queue one by one.
            if (ev.spawn_mode == 1) { battle_spawn_activate(ev); break; }
            std::vector<int> ok;
            for (int ui : ev.spawn_units) {
                int x = 0, y = 0, f = 0;
                battle_spawn_dest(ev, ui, x, y, f);
                if (battle_16699(x, y, f, ui)) { ok.push_back(ui); continue; }
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: spawn blocked (unit %d dest=(%d,%d)) -> sub_5069C queue\n", ui, x, y);
                battle_spawn_enqueue(battle_spawn_subevent(ev, std::vector<int>{ ui }));
            }
            if (!ok.empty()) battle_spawn_activate(ok.size() == ev.spawn_units.size() ? ev
                                                                                      : battle_spawn_subevent(ev, ok));
            break;
        }
        case VmEventType::Faction: {
            battle_wc1_occupancy_unknown("VM 0x0C faction/list mutation", ev.offset); // defensive if a parser marker is absent
            const int unit_index = resolve_vm_unit_ref(ev.unit_ref_sel, ev.unit_ref_id,
                                                       ev.forced_target_context);
            if (unit_index >= 0 && (size_t)unit_index < units_.size()) {
                CombatUnitPlacement& unit = units_[(size_t)unit_index];
                unit.list_seq = battle_next_list_seq(); // 1137A (remove) then 1134F (new list tail)
                // sub_52463 act0 (5252E): 1137A (remove from the old CC968), write the low byte of
                // FDAF6[payload] to [+0Eh]/link, insert into the new CC968 with 1134F, and set
                // [+87h] = payload when [+87h] != 0. No dec/inc.
                // The port models only [+0Eh] (memory_army) and the re-insertion. The [+77h] link
                // record and the [+87h] owner are not stored in units_ (SCE static units are always
                // unlinked), so those two writes have no target. The owner is read only from the
                // BLACKBOX 0x4E26 record while loading and has no consumer during battle, so there
                // is
                // no gameplay effect.
                const int eff = sce_fdaf6_lookup(ev.army);
                if (eff >= 0) {
                    unit.memory_army = eff & 0xFF;
                    unit.battle_list_membership_modeled = unit.memory_army >= 1 && unit.memory_army <= 4;
                } else {
                    unit.army = ev.army;
                    unit.memory_army = -1;
                    unit.battle_list_membership_modeled = false;
                }
                unit.team = (uint8_t)(unit.memory_army >= 0 ? (unit.memory_army <= 2 ? 0 : 1) : (ev.army == 0 ? 0 : 1));
                fprintf(stderr, "COMBAT: play faction: unit %d payload=%d -> eff %d (FDAF6)\n", unit.id, ev.army, unit.memory_army);
            } else {
                fprintf(stderr, "COMBAT: play faction: selector (%u,%u) unresolved at playback\n",
                        ev.unit_ref_sel, ev.unit_ref_id);
            }
            break;
        }
        case VmEventType::Magic: {
            // sub_51E8B effect execution.
            //   mode 0: spell < 0x64 -> F13B0[spell](unit, level) with automatic targeting.
            //     spell >= 0x64 -> sub_2C8A4(spell-0x64), the screen damage effect (meteo/dragon)
            //     from a separate table.
            //   mode 1: sub_2CA39(1) -> F1474[spell](unit, level) (>=0x64 uses
            //     sub_302E6(spell-0x64)) -> sub_2CA39(0) -> [unit+57h]=0.
            const int ui = resolve_vm_unit_ref(ev.unit_ref_sel, ev.unit_ref_id,
                                               ev.forced_target_context);
            const int spell = ev.value & 0xFF;
            const int level = ev.army & 0xFF;
            if (ev.target_sub == 0) {
                if (spell < 0x64) {
                    if (ui >= 0 && (size_t)ui < units_.size()) {
                        const bool auto_saved = battle_spell_auto_;
                        battle_spell_auto_ = true;             // sub_28422(1)
                        // ASM loc_51F2F~loc_51F77: call F13B0[spell] directly with dword_74492=1.
                        // The handler uses that flag to run its automatic target choice
                        // (28982->419F1, 28C8A->41BF5, 29722->41E41). Calling it with (-1,-1) makes
                        // cursor-based spells fail for want of a target, so the target goes through
                        // the shared AI/script path (battle_ai_cast_spell).
                        battle_ai_cast_spell(ui, spell * 3 + level); // F13B0[spell]
                        battle_spell_auto_ = auto_saved;       // sub_28422(0)
                    }
                } else {
                    // sub_2C8A4(spell-0x64): 12x12 full-screen damage plus the dragon/f-meteo2
                    // effect.
                    battle_spell_2c8a4(spell - 0x64);
                }
            } else if (ui >= 0 && (size_t)ui < units_.size()) {
                const int saved_f14d8 = battle_f14d8_;
                battle_f14d8_ = 1;                             // sub_2CA39(1)
                if (spell < 0x64)
                    battle_weapon_f1474(ui, spell, level);     // dword_F1474[spell]
                else
                    battle_weapon_302e6(ui, spell - 0x64);     // sub_302E6
                battle_f14d8_ = saved_f14d8;                   // sub_2CA39(0)
                if ((size_t)ui < units_.size()) units_[(size_t)ui].combat_acc57 = 0; // [57h]=0
            }
            if (should_exit_ || skip_cutscene_) return;
            present_cutscene_frame();
            break;
        }
        case VmEventType::FactionTransform: {
            // sub_52463 action 1..4.
            const int ui = resolve_vm_unit_ref(ev.unit_ref_sel, ev.unit_ref_id,
                                               ev.forced_target_context);
            if (ui < 0 || (size_t)ui >= units_.size()) break;
            // Payload length: action 1/3 = inline 0x88 record, action 2 = 1 byte,
            // action 4 = 2 bytes (loc_5267C `mov edx,1` / loc_5270B `mov edx,2`).
            const size_t need = (ev.value == 1 || ev.value == 3) ? (size_t)kSceUnitRecSize
                              : (ev.value == 4) ? 2u : (ev.value == 2) ? 1u : 0u;
            if (ev.value < 1 || ev.value > 4 || ev.record.size() < need) break;
            CombatUnitPlacement& unit = units_[(size_t)ui];
            const uint8_t* rec = ev.record.data();
            if (ev.value == 1 || ev.value == 3) {
                // Action 1: the linked record ([+77h]) when present, otherwise the unit; action 3:
                // the unit.
                CombatUnitPlacement* dst = &unit;
                const bool linked = (ev.value == 1 && unit.stored_77 != nullptr);
                if (linked) dst = unit.stored_77.get();
                if (ev.value == 1) {
                    // loc_525C3: copy only the name (from 2) and 10/11/2B/40..43/44/46/4C/52/54.
                    dst->name.clear();
                    for (int i = 2; i < 0x10 && rec[i]; ++i) dst->name.push_back((char)rec[i]);
                    dst->face = rec[0x10];
                    dst->combat_class = rec[0x11];
                    dst->combat_level_2b = rec[0x2B];
                    dst->combat_str_40 = rec[0x40];
                    dst->combat_mag41 = rec[0x41];
                    dst->combat_value_42 = (int16_t)(rec[0x42] | (rec[0x43] << 8));
                    dst->combat_value_43 = rec[0x43];
                    dst->combat_value_44 = (int16_t)(rec[0x44] | (rec[0x45] << 8));
                    dst->combat_value_46 = (int16_t)(rec[0x46] | (rec[0x47] << 8));
                    const int32_t maxraw = (int32_t)(rec[0x4C] | (rec[0x4D] << 8) |
                                                     (rec[0x4E] << 16) | ((uint32_t)rec[0x4F] << 24));
                    dst->max_hp = maxraw > 0 ? (int)((maxraw + 99) / 100) : 0;
                    dst->max_hp_deficit = dst->max_hp * 100 - maxraw;
                    if (dst->max_hp_deficit < 0) dst->max_hp_deficit = 0;
                    dst->combat_mp_50 = (uint16_t)(rec[0x50] | (rec[0x51] << 8));
                    dst->combat_mp_max_52 = (uint16_t)(rec[0x52] | (rec[0x53] << 8));
                    dst->combat_value_54 = (int16_t)(rec[0x54] | (rec[0x55] << 8));
                    if (linked) unit.face = dst->face;   // [ecx+10h]=[ebp+10h] + sub_17E6D
                } else {
                    // loc_526C6: name and [10h] only.
                    unit.name.clear();
                    for (int i = 2; i < 0x10 && rec[i]; ++i) unit.name.push_back((char)rec[i]);
                    unit.face = rec[0x10];
                }
            } else if (ev.value == 2 || ev.value == 4) {
                // loc_5267C/loc_5270B: write the payload straight into the unit's [+61h] (sprite
                // type) via `lea esi,[ecx+61h]; call sub_500C9(esi, cursor, edx)`.
                //   action 2: 1 byte -> [+61h] only (the high [62h] is kept)
                //   action 4: 2 bytes -> all of [+61h]/[62h]
                // sub_119B6 then releases the old image slot and sub_1198D([61h]) takes a new one
                // into [60h]. The port keys its NC2 cache by type, so updating slot_hint and
                // sprite_id is enough for rendering to pick the new type.
                if (ev.value == 2) {
                    unit.sprite_type = (unit.sprite_type & 0xFF00) | (int)rec[0];
                } else {
                    unit.sprite_type = (int)rec[0] | ((int)rec[1] << 8);
                }
                unit.slot_hint = unit.sprite_type & 0xFF;     // [60h] = sub_1198D([61h])
                unit.sprite_id = (uint8_t)(unit.sprite_type % 222);
                if (unit.stored_77)                            // loc_526B2: [77h]+60h = dl
                    unit.stored_77->slot_hint = unit.slot_hint;
                if (ev.value == 4) {                          // [48h]=[4Ch] full HP restore
                    unit.hp = unit.max_hp;
                    unit.hp_deficit = unit.max_hp_deficit;
                }
            }
            if (script_trace_)
                fprintf(stderr, "COMBAT: play faction transform unit=%d action=%d name=%s face=%d\n",
                        unit.id, ev.value, unit.name.c_str(), unit.face);
            present_cutscene_frame();
            break;
        }
        case VmEventType::Weather:
            // sub_527FF: only when the value changes, byte_BA84D is updated and sub_44F30 is
            // called.
            // The palette fade (~0.7s) happens right there, so it runs during playback.
            if ((uint8_t)ev.value != weather_) {
                fprintf(stderr, "COMBAT: play weather %u -> %u (palette dimming %s)\n",
                        weather_, (uint8_t)ev.value,
                        ev.value == 0 ? "clear" : "apply");
                weather_ = (uint8_t)ev.value;
                apply_weather_palette(true);
            } else {
                fprintf(stderr, "COMBAT: play weather %u (no change - sub_44F30 skipped)\n", ev.value);
            }
            present_cutscene_frame();
            break;
        case VmEventType::NextScenario:
            fprintf(stderr, "COMBAT: play next scenario '%s'\n", ev.text.c_str());
            // loc_53002 -> sub_52D33 literally: cleanup/save -> fade-out -> contmap.dat -> exit(1).
            // Loading the next scenario belongs to MAIN.EXE restarting (a new COMBATII run).
            vm_next_scenario_52d33(ev.text);
            return;
        case VmEventType::Attack:
            // sub_51C74 loc_51DFE/loc_51E0F: sub_1E61D (attacker) [+ sub_1EF5C]
            play_attack_sequence(ev);
            if (should_exit_ || skip_cutscene_) return;
            break;
        case VmEventType::UnitPose:
            play_unit_pose(ev);                        // loc_51E34
            if (should_exit_ || skip_cutscene_) return;
            break;
        case VmEventType::Item: {
            // sub_5196F: kind 0 stores the item, kinds 1..3 equip it (weapon / shield /
            // accessory). A failed equip goes to the first free backpack slot; when both are
            // occupied the old [5Fh] item is moved to the stash and replaced. The routine then
            // shows the item name plus its suffix through sub_482A5.
            const int ui = ev.item_unit;
            if (ui >= 0 && (size_t)ui < units_.size()) {
                battle_load_item_table();
                std::string name;
                if (ev.item_id >= 1) {
                    const size_t off = ((size_t)ev.item_id - 1) * 66u + 1u;
                    for (size_t k = off; k < battle_lookup_table_.size() && battle_lookup_table_[k] != 0; ++k)
                        name.push_back((char)battle_lookup_table_[k]);
                }
                static const char kGot[] = " \xD2\x42\x97\x62!\n";                       // " 획득!\n"
                static const char kEquipWeapon[] = "\xB5\xA1\x9F\x65\xAD\xA5 \xB8\x77\xC0\x62!"; // 오른손 장착!
                static const char kEquipShield[] = "\xB6\x45\xAD\xA5 \xB8\x77\xC0\x62!";         // 왼손 장착!
                static const char kEquipAccessory[] = "\xA5\xA1\xB9\xA1\x8A\x81\x9D\xA1 \xB8\x77\xC0\x62!"; // 보조구로 장착!
                // VA 0x78000: "배낭에 보관\n있던 ITEM은 창고에 보관" (sub_5196F+266)
                static const char kBagKeep[] = "\xA4\x81\x90\x77\xB5\x41\x20\xA5\xA1\x89\xC5\x0A\xB7\xB6\x94\xE5\x20\x49\x54\x45\x4D\xB7\x65\x20\xC0\x77\x89\xA1\xB5\x41\x20\xA5\xA1\x89\xC5";
                auto& iu = units_[(size_t)ui];
                std::string msg;
                if (ev.item_kind == 0) {
                    battle_stash_add(ev.item_id);                              // sub_4ECE4
                    msg = name + kGot;
                } else if (ev.item_kind >= 1 && ev.item_kind <= 3) {
                    const int sk = ev.item_kind == 1 ? 1 : (ev.item_kind == 2 ? 0 : 2);
                    if (battle_equip_check(ui, ev.item_id, sk)) {              // sub_1832D
                        const int old = sk == 1 ? iu.combat_type_5b
                                      : sk == 0 ? iu.combat_type_5a : iu.combat_type_5d;
                        if (old != 0) {
                            battle_stash_add(old);                             // sub_4ECE4
                            battle_equip_remove(ui, sk);                       // sub_18262
                        }
                        battle_equip_apply(ui, ev.item_id, sk);                // sub_181A6
                        msg = name + (ev.item_kind == 1 ? kEquipWeapon
                                   : ev.item_kind == 2 ? kEquipShield : kEquipAccessory);
                    } else if (iu.inv_5e == 0) {
                        iu.inv_5e = ev.item_id;
                        msg = name + kGot;
                    } else if (iu.inv_5f == 0) {
                        iu.inv_5f = ev.item_id;
                        msg = name + kGot;
                    } else {
                        battle_stash_add(iu.inv_5f);
                        iu.inv_5f = ev.item_id;
                        msg = name + kBagKeep;
                    }
                }
                if (!msg.empty()) battle_unit_message(ui, msg);                 // sub_482A5
            }
            if (should_exit_ || skip_cutscene_) return;
            break;
        }
        case VmEventType::UnitEffect:
            play_unit_effect(ev);                      // sub_51FF7
            if (should_exit_ || skip_cutscene_) return;
            break;
        case VmEventType::Hp:
            play_hp_change(ev);                        // sub_5211D
            if (should_exit_ || skip_cutscene_) return;
            break;
        case VmEventType::UnitLeave: {
            if (!ev.leave_units.empty())
                battle_wc1_occupancy_unknown("VM 0x04 retreat/removal", ev.offset); // defensive if a parser marker is absent
            // Second half of sub_51449 (0x04): in mode 0 sub_17071 walks the unit to the screen
            // edge
            // (border) and then sub_11EE4(x,y,U) plus removal from the legion list take it off the
            // field.
            play_retreat(ev);
            if (should_exit_ || skip_cutscene_) return;
            break;
        }
        case VmEventType::Face: {
            // sub_52848: decide unit0's [63h] from dx = unit0.x - unit1.x, dy = unit0.y - unit1.y.
            //   dx == 0              -> dy > 0 ? 2 (north) : 0 (south)
            //   |dx|*150/100 >= |dy| -> dx > 0 ? 1 (west) : 3 (east)
            //   otherwise            -> dy > 0 ? 2 (north) : 0 (south)
            const int ua_index = resolve_vm_unit_ref((uint8_t)ev.face_refs[0],
                                                     (uint8_t)ev.face_refs[1],
                                                     ev.forced_target_context);
            const int ub_index = resolve_vm_unit_ref((uint8_t)ev.face_refs[2],
                                                     (uint8_t)ev.face_refs[3],
                                                     ev.forced_target_context);
            CombatUnitPlacement* ua = ua_index >= 0 ? &units_[(size_t)ua_index] : nullptr;
            CombatUnitPlacement* ub = ub_index >= 0 ? &units_[(size_t)ub_index] : nullptr;
            if (ua && ub) {
                const int dx = (int)ua->x - (int)ub->x;
                const int dy = (int)ua->y - (int)ub->y;
                const int adx = dx < 0 ? -dx : dx;
                const int ady = dy < 0 ? -dy : dy;
                int facing;
                if (dx == 0) facing = (dy > 0) ? 2 : 0;
                else if (adx * 150 / 100 >= ady) facing = (dx > 0) ? 1 : 3;
                else facing = (dy > 0) ? 2 : 0;
                ua->facing = (int8_t)facing;
                fprintf(stderr, "COMBAT: play face unit %d at (%d,%d) vs (%d,%d) -> facing=%d\n",
                        ua->id, ua->x, ua->y, ub->x, ub->y, (int)ua->facing);
            }
            present_cutscene_frame();
            break;
        }
        case VmEventType::Victory:
            // sub_506F3: sub_53F2C(0) = victory cutscene (already run by the VM) -> when
            // dword_11209E==1, the sub_44CCA(0) fade -> sub_4E4C4(2) open -> sub_4E622 save ->
            // sub_4EC51 cleanup -> sub_4ECD7 close -> sub_4EC9A(1) -> unlink contmap.dat ->
            // `$`=" " -> exit(1).
            // The sub_55518 survivor placement screen runs at the start of the next process
            // (combatii_deploy.cpp). At victory there is no result tally and no wipe check.
            victory_fade_out();
            fprintf(stderr, "COMBAT: stage-clear: save BLACKBOX, cleanup battle units, unlink contmap.dat, write '$'=' ', exit\n");
            battle_blackbox_save(2);               // sub_4E4C4(2)+sub_4E622
            battle_battle_cleanup_4ec51();         // sub_4EC51+sub_4EAF3 (write the 0x88 records)
            battle_blackbox_close();               // sub_4ECD7 (no-op in the port)
            write_blackbox_state(1);               // sub_4EC9A(1)
            {
                std::string cm = game_path("contmap.dat");
                if (!file_exists(cm.c_str())) cm = game_path("CONTMAP.DAT");
                if (file_exists(cm.c_str())) file_delete(cm.c_str());
            }
            // The original writes " " (a space) to '$' through sub_4FDD1 (sub_506F3+~60). MAIN
            // decides its next step from the '$' contents, so the same payload is kept, and the
            // port
            // exits only through should_exit_ (do_transition("EXIT") would write EXIT to '$').
            write_dollar(" ");
            // sub_506F3 ends with `mov eax,1; jmp sub_61CFF`, so the process exit code is 1.
            // MAIN.EXE (main_ loc_10100 / 0x101E2) returns to loc_100EA when the spawnl result is
            // not
            // 0FFh, so this code decides the '$'/contmap.dat handoff order.
            return_code_ = 1;
            should_exit_ = true;
            return;
        default:
            break;
        }
    }
}
