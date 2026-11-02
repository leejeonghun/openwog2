// chp_class.cpp -- port of sub_128DA (class change screen, cc.pcx), the lower-left
// button screen of the status window, plus the inventory/equipment helpers it shares
// with the equipment screen.
//
//   sub_128DA  status_inventory_screen   screen loop
//   sub_12800  class_can_change          whether the character may change class
//   sub_12586  class_tab_of              character -> tab number
//   sub_12701  class_sprite_row          row holding this town sprite
//   sub_1172D  class_calc_shortfall      requirement shortfalls
//   sub_11AD8  class_req_ok              requirement check
//   sub_11DC7  class_draw_list           right-hand character list
//   sub_11F24  class_draw_req_panel      left-hand requirement icon strip
//   sub_1226E  class_draw_grid           4x4 class grid + selected cell label
//   sub_12753  class_refresh             redraw and flip
//   sub_1285F  class_recalc_stats        recompute stats after a class change
//   sub_32A1A / sub_329BD / sub_339B5 / sub_33909 / sub_32D94 / sub_32FCD  shared inventory/equipment
//
// Numeric tables live in chp_class_data.h; sprites are read from CHP.EXE VAs at fixed
// offsets.
#include "chp.h"
#include "chp_class_data.h"
#include "chp_status_data.h"
#include "audio.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "game_timer.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace {
constexpr int CL_REC = 0x88;
constexpr int CL_PREV = 0x0D, CL_ID = 0x0F, CL_CLASS = 0x11, CL_LEVEL = 0x2B, CL_TOWN = 0x61;
inline int u16at(const uint8_t* r, int o) { return r[o] | (r[o + 1] << 8); }
inline int s32at(const uint8_t* r, int o) { return (int32_t)(r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24)); }
inline void put16(uint8_t* r, int o, int v) { r[o] = (uint8_t)v; r[o + 1] = (uint8_t)(v >> 8); }
inline int halfv(int v) { return (v - (v >> 31)) >> 1; }     // original: (v - sign) >> 1
constexpr int kPartyCountMax = 0x40;
}  // namespace

// ==== Assets ====
bool CHP::class_load_assets() {
    if (class_assets_loaded_) return true;
    exe_resource_load(ExeId::Chp, 0x57710, ExeResLength::Rle, class_req_ok_spr_);
    exe_resource_load(ExeId::Chp, 0x578BE, ExeResLength::Rle, class_req_bad_spr_);
    exe_resource_load(ExeId::Chp, 0x57DC8, ExeResLength::Rle, class_tip_bg_);
    exe_resource_load(ExeId::Chp, 0x57F76, ExeResLength::Rle, class_prev_a_);
    exe_resource_load(ExeId::Chp, 0x58124, ExeResLength::Rle, class_prev_b_);
    exe_resource_load(ExeId::Chp, 0x586EC, ExeResLength::Rle, class_ok_btn_);
    exe_resource_load(ExeId::Chp, 0x3048E, ExeResLength::Sprite, class_arrow_up_);
    exe_resource_load(ExeId::Chp, 0x30532, ExeResLength::Sprite, class_arrow_dn_);
    class_assets_loaded_ = true;
    return true;
}

// ==== Checks and table lookups ====
const uint8_t* CHP::class_rec(int idx) const {
    static const uint8_t zero[CL_REC] = {};
    if (idx < 0 || idx >= (int)state_.party_count || (size_t)(idx + 1) * CL_REC > party_recs_.size()) return zero;
    return party_recs_.data() + (size_t)idx * CL_REC;
}

// sub_12800: class+1 in {3,4,5,6,0xA,0x12,0x21,0x34,0x35,0x47,0x48}, or character id 0x48,
//   blocks a class change.
bool CHP::class_can_change(int idx) const {
    const uint8_t* r = class_rec(idx);
    switch (r[CL_CLASS] + 1) {
        case 3: case 4: case 5: case 6: case 0x0A: case 0x12: case 0x21: case 0x34: case 0x35: case 0x47: case 0x48:
            return false;
        default: break;
    }
    return r[CL_ID] != 0x48;
}

// sub_12586: character id (+0x0F) selects a special tab (3..0xE); others come from the
//   per-class table at 0x5A638. 0xFFFF maps to -1.
int CHP::class_tab_of(int idx) const {
    const uint8_t* r = class_rec(idx);
    const int id = r[CL_ID];
    auto in = [&](std::initializer_list<int> l) { for (int v : l) if (v == id) return true; return false; };
    if (in({0x45, 0x2E})) return 3;
    if (in({0x66, 0x68, 0x6C})) return 4;
    if (in({0x3F, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62})) return 5;
    if (in({0x3E, 0x2C})) return 6;
    if (in({0x14, 0x15, 0x32, 0x3B, 0x3C, 0x3D, 0x6D, 0x6E})) return 7;
    if (in({0x03, 0x04, 0x0C, 0x48, 0x64, 0x65, 0x6A, 0x6B, 0x70, 0x71, 0x73, 0x77})) return 8;
    if (in({0x16, 0x50})) return 9;
    if (in({0x4B})) return 0x0A;
    if (in({0x08, 0x4F, 0x67})) return 0x0B;
    if (in({0x42, 0x41})) return 0x0C;
    if (in({0x43, 0x44})) return 0x0D;
    if (in({0x11, 0x6F})) return 0x0E;
    const int cls = r[CL_CLASS];
    return (int16_t)(cls < kChpClassTabOfCount ? kChpClassTabOf[cls] : 0);
}

// sub_12701: row containing this character's town sprite group (+0x61), or 0x1869F if absent.
int CHP::class_sprite_row(int idx) const {
    const int spr = u16at(class_rec(idx), CL_TOWN);
    for (int row = 0; row < kChpClassSprRows; row++)
        for (int c = 0; c < kChpClassSprCols; c++)
            if (kChpClassSprite[row * kChpClassSprCols + c] == spr) return row;
    return 0x1869F;
}

int CHP::class_req_at(int idx) const {
    if (idx < 0 || idx >= kChpClassReqCount * 18) return 0;
    return kChpClassReq[idx];
}

int CHP::class_cell_class(int tab, int cell) const {
    if (tab < 0 || tab >= kChpClassTabCount || cell < 0 || cell >= 16) return -1;
    return (int)kChpClassTab[tab * 16 + cell] - 1;
}

// sub_11AD8: true when every requirement is met.
bool CHP::class_req_ok(const uint8_t* r, int cls) const {
    auto q = [&](int k) { return class_req_at(cls * 18 + k); };
    bool ok = true;
    if (r[0x40] < q(0)) ok = false;
    if (q(1) > r[0x41]) ok = false;
    if (q(2) > r[0x42]) ok = false;
    if (q(3) > r[0x43]) ok = false;
    if (s32at(r, 0x4C) / 100 < q(4)) ok = false;
    if (q(5) > (int16_t)u16at(r, 0x52)) ok = false;
    if (q(6) > r[0x38]) ok = false;
    if (q(7) > r[0x3A]) ok = false;
    if (q(8) > r[0x39]) ok = false;
    if (q(9) > r[0x3B]) ok = false;
    if (q(10) > r[0x2E]) ok = false;
    if (r[0x2F] < q(11)) ok = false;
    if (q(12) > r[0x30]) ok = false;
    if (r[0x31] < q(13)) ok = false;
    if (r[0x32] < q(14)) ok = false;
    if (r[0x34] < q(15)) ok = false;
    { const int rq = q(16); if (r[CL_PREV] + 1 != rq && rq != 0) ok = false; }
    if (r[CL_LEVEL] < q(17)) ok = false;
    return ok;
}

// sub_1172D: same checks as class_req_ok, but records each shortfall in class_short_
//   (entries that pass are left untouched). class_short_[1+k] mirrors byte_5A6B5[k]:
//   k=16 (previous class) stores 1 on mismatch, k=17 (level) stores the missing levels.
bool CHP::class_calc_shortfall(const uint8_t* r, int cls) {
    auto q = [&](int k) { return class_req_at(cls * 18 + k); };
    bool ok = true;
    auto bytecmp = [&](int k, int have) {            // have < req -> record the shortfall
        if (have < q(k)) { class_short_[1 + k] = (uint8_t)(q(k) - have); ok = false; }
    };
    bytecmp(0, r[0x40]);
    bytecmp(1, r[0x41]);
    bytecmp(2, r[0x42]);
    bytecmp(3, r[0x43]);
    { const int hp = s32at(r, 0x4C) / 100; if (hp < q(4)) { class_short_[1 + 4] = (uint8_t)(q(4) - hp); ok = false; } }
    { const int mp = (int16_t)u16at(r, 0x52);
      if (mp < q(5)) { class_short_[1 + 5] = (uint8_t)(q(5) - (uint8_t)mp); ok = false; } }
    bytecmp(6, r[0x38]);
    bytecmp(7, r[0x3A]);
    bytecmp(8, r[0x39]);
    bytecmp(9, r[0x3B]);
    bytecmp(10, r[0x2E]);
    bytecmp(11, r[0x2F]);
    bytecmp(12, r[0x30]);
    bytecmp(13, r[0x31]);
    bytecmp(14, r[0x32]);
    bytecmp(15, r[0x34]);
    { const int rq = q(16); if (r[CL_PREV] + 1 != rq && rq != 0) { class_short_[1 + 16] = 1; ok = false; } }
    { const int lv = r[CL_LEVEL]; if (lv < q(17)) { class_short_[1 + 17] = (uint8_t)(q(17) - lv); ok = false; } }
    return ok;
}

// sub_1285F: recompute derived stats after a class change (equipment bonuses are not
//   applied, as in the original).
void CHP::class_recalc_stats(uint8_t* r) {
    const int cls = r[CL_CLASS];
    int w44 = r[0x40];
    const int b = cls < 164 ? kChpClassStat3[cls * 3] : 0;
    if (b == 0 || b == 6) w44 *= 2;
    put16(r, 0x44, w44);
    const uint8_t* t = cls < 164 ? &kChpClassRec8[cls * 8] : nullptr;
    put16(r, 0x46, t ? u16at(t, 0) : 0);
    int bx = r[0x42] + (t ? u16at(t, 4) : 0);
    int w54 = (uint16_t)bx;
    if ((int16_t)w54 < 0x3C) w54 = 0x3C;
    put16(r, 0x54, w54);
    r[0x59] = t ? t[2] : 0;
}

// sub_1CC90 / sub_1CC00: darken palette entries 0x00..0x60 inside a rectangle to grey 2..7
//   with new = 7 - (R+G+B)/0x25, using the palette word_A3D4C saved when the status
//   window opened.
void CHP::class_dim_rect(int x1, int y1, int x2, int y2) {
    uint8_t* bb = video_.get_backbuffer();
    for (int y = y1; y < y2; y++) {
        if (y < 0 || y >= SCREEN_H) continue;
        for (int x = x1; x < x2; x++) {
            if (x < 0 || x >= SCREEN_W) continue;
            uint8_t& p = bb[y * SCREEN_W + x];
            if (p > 0x60) continue;
            const int sum = status_save_pal_[p][0] + status_save_pal_[p][1] + status_save_pal_[p][2];
            p = (uint8_t)(7 - sum / 0x25);
        }
    }
}

// ==== Drawing ====
// sub_11DC7: right-hand character list (6 rows, 0x18 apart), same layout as the status
//   list, drawn at x+0xA3.
void CHP::class_draw_list(int top, int sel) {
    const int count = state_.party_count;
    for (int row = 0; row < 6; row++) {
        const int idx = top + row;
        const int y0 = 4 + 0x18 * row;
        if (idx == sel) draw_bevel_box(0xA9, y0, 0x129, 0x1C + 0x18 * row);
        if (idx >= count) continue;
        const uint8_t* rec = class_rec(idx);
        const SpriteEntry* sp = item_sprites_.get(u16at(rec, CL_TOWN));
        if (sp && sp->pixel_data) blit_sprite_rle(0xAD, y0, sp->pixel_data);
        draw_status_text(0xC9, 5 + 0x18 * row, (const char*)rec + 2);
        const int cls = rec[CL_CLASS];
        const char* cn = cls < kChpClassNameCount ? kChpClassNames[cls] : "";
        const int half = (int)strlen(cn) >> 1;
        draw_mini_string(0xF1 - 6 * half, 0x16 + 0x18 * row, cn, mini_font_color_);
    }
}

// sub_11F24 (requirement icons): list the selected class's requirements as icons in the
//   left vertical strip (x=4), ordered stats 0..9, skills 10..15, previous class (16),
//   level (17); zero entries are skipped and the rest stack from the top.
void CHP::class_draw_req_panel(int sel) {
    const int cls = class_cell_class(class_tab_, class_cell_);
    if (cls < 0) return;
    const uint8_t* rec = class_rec(sel);
    const bool ok = class_calc_shortfall(rec, cls);               // sub_1172D (record the shortfalls)
    if (class_short_[0] != 0 && !ok) class_short_[0] = 0;
    int row = 0;
    auto put = [&](const uint8_t* spr) {
        if (spr) blit_sprite_rle(4, 9 + row * 0x11, spr);
        class_short_[0] = 0;
        row++;
    };
    for (int i = 0; i < 10; i++)
        if (class_req_at(cls * 18 + i)) put(status_icon(2 * i + (class_short_[1 + i] ? 1 : 0)));
    for (int j = 10; j < 16; j++) {
        const int v = class_req_at(cls * 18 + j);
        if (v) put(status_icon(v * 2 - 2 + (0x14 + 4 * (j - 10)) + (class_short_[1 + j] ? 1 : 0)));
    }
    const int pre = class_req_at(cls * 18 + 16);
    if (pre) {
        const int slot = (pre == 2 ? 0x30 : 0x2C) + class_short_[1 + 16];
        blit_sprite_rle(4, 9 + row * 0x11, status_icon(slot));
        row++;
    }
    if (class_req_at(cls * 18 + 17)) {
        const std::vector<uint8_t>& s = class_short_[1 + 17] == 0 ? class_req_ok_spr_ : class_req_bad_spr_;
        if (!s.empty()) blit_sprite_rle(4, 9 + row * 0x11, s.data());
    }
}

// sub_1226E: 4x4 class grid (town sprite, dimmed when requirements are unmet) plus the
//   label of the selected cell.
void CHP::class_draw_grid(int sel, int tab) {
    const uint8_t* rec = class_rec(sel);
    const SpriteEntry* last = nullptr;                            // esi left over after the loop (used for the label position)
    auto sprite_of = [&](int cls) -> const SpriteEntry* {
        if (class_spr_row_ < 0 || class_spr_row_ >= kChpClassSprRows) return nullptr;
        int fi = class_spr_row_ * kChpClassSprCols + cls;         // cls = -1 reads the last cell of the previous row
        if (fi < 0 || fi >= kChpClassSprRows * kChpClassSprCols) return nullptr;
        return item_sprites_.get(kChpClassSprite[fi]);
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const int cls = class_cell_class(tab, r * 4 + c);
            const SpriteEntry* sp = sprite_of(cls);
            last = sp;
            if (cls < 0 || !sp || !sp->pixel_data) continue;
            const int w = (int16_t)sp->word0, h = (int16_t)sp->word2;
            const int x = (0x2A + 0x20 * c) - halfv(w);
            const int y = (0x11 + 0x1F * r) - halfv(h);
            blit_sprite_rle(x, y, sp->pixel_data);
            if (!class_req_ok(rec, cls)) class_dim_rect(x, y, x + w, y + h);
        }
    }
    const int lw = last ? (int16_t)last->word0 : 0, lh = last ? (int16_t)last->word2 : 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const int cls = class_cell_class(tab, r * 4 + c);
            if (cls < 0 || r * 4 + c != class_cell_) continue;
            const char* name = cls < kChpClassNameCount ? kChpClassNames[cls] : "";
            const int len = (int)strlen(name);
            const int vx = (0x32 + 0x20 * c) - halfv(lw);
            const int vy = (0x11 + 0x1F * r) - halfv(lh);
            int x = vx - 5 * (len >> 1);
            if (x < 0x1A) x = 0x1A;
            draw_bevel_box(x - 2, vy - 2, len * 6 + x, vy + 5, 4);
            if (tab == 0) set_mini_font_color(0x2D);
            else if (tab == 1) set_mini_font_color(0x25);
            else if (tab == 2) set_mini_font_color(0x35);
            class_sel_class_ = cls;
            draw_mini_string(x, vy, name, mini_font_color_);
            set_mini_font_color(1);
        }
    }
}

// Scroll thumb (x 0x12A..0x139) and the selected tab button sprite (tabs 0..2 only).
void CHP::class_draw_thumb_and_tab() {
    const int h = (int)status_thumb_h_;
    draw_bevel_box(0x12A, 0x0F + status_thumb_off_, 0x139, h + 0x0F + status_thumb_off_);
    if (class_tab_ >= 0 && class_tab_ < 3) {
        const size_t off = (size_t)kChpStatusExtraOffset + (size_t)class_tab_ * kChpStatusExtraStride;
        if (off < status_icons_.size()) blit_sprite_rle((class_tab_ << 5) + 0x1F, 0x84, status_icons_.data() + off);
    }
}

// sub_11F24 + sub_1226E + bar/tabs: recompose the whole screen over the cc.pcx snapshot
//   (no flip).
void CHP::class_compose(int sel) {
    uint8_t* bb = video_.get_backbuffer();
    memcpy(bb, status_snap_.data(), status_snap_.size());          // sub_1DABB(DA370 -> C5EA4)
    class_draw_list(class_top_, sel);
    class_draw_req_panel(sel);
    class_draw_grid(sel, class_tab_);
    class_draw_thumb_and_tab();
}

// sub_12753
void CHP::class_refresh(int sel) {
    class_compose(sel);
    status_flip();
}

// ==== Shared inventory / equipment helpers ====
int CHP::status_inv_slot() const {
    return state_.team_active < MAX_BLACKBOX_SLOTS ? state_.team_active : 0;
}

// sub_32A1A: insert item (0-based) at the head of the active team inventory; at 100
//   items it shows a message and returns false.
bool CHP::status_inv_add(int item0) {
    const int s = status_inv_slot();
    uint16_t& cnt = g_slot_item_count[s];
    if (cnt >= 0x64) {
        status_message(kChpClassMsgFull);
        return false;
    }
    for (int b = (int)cnt - 1; b >= 0; b--) g_slot_items[s][b + 1] = g_slot_items[s][b];
    g_slot_items[s][0] = (uint16_t)((item0 + 1) & 0xFF);
    cnt++;
    return true;
}

// One team inventory cell. The original indexes the byte array (unk_116508 + 600*team)
// directly, so idx<0 reaches the memory just before it - which is what discarding
// equipment via sub_329BD(src-0x64) does; the port models that area with inv_spill_.
uint16_t& CHP::inv_ref(int slot, int idx) {
    static uint16_t dummy;
    if (idx >= 0 && idx < BLACKBOX_SLOT_ITEMS) return g_slot_items[slot][idx];
    if (idx < 0 && idx >= -128) return inv_spill_[idx + 128];
    dummy = 0;
    return dummy;
}

// sub_329BD: remove item idx and pull the rest forward ([b]=[b+1] while b < count,
//   then [b]=0 and count-1).
void CHP::status_inv_remove(int idx) {
    const int s = status_inv_slot();
    uint16_t& cnt = g_slot_item_count[s];
    int b = idx;
    while (b < (int)cnt) { inv_ref(s, b) = inv_ref(s, b + 1); b++; }
    inv_ref(s, b) = 0;
    if (cnt > 0) cnt--;
}

// sub_339B5: clear an equipment slot (1=+0x5B, 2=+0x5A, 3=+0x5D) and undo that item's
//   stat bonuses.
void CHP::status_equip_strip(uint8_t* r, int slot) {
    int item0 = 0;
    if (slot == 1)      { item0 = r[0x5B] - 1; r[0x5B] = 0; }
    else if (slot == 2) { item0 = r[0x5A] - 1; r[0x5A] = 0; }
    else if (slot == 3) { item0 = r[0x5D] - 1; r[0x5D] = 0; }
    if (item0 < 0 || item0 >= MAX_ITEMS) return;
    const uint8_t* it = item_data_[item0];
    r[0x40] = (uint8_t)(r[0x40] - it[0x28]);
    r[0x41] = (uint8_t)(r[0x41] - it[0x29]);
    r[0x42] = (uint8_t)(r[0x42] - it[0x2A]);
    r[0x43] = (uint8_t)(r[0x43] - it[0x2B]);
    put16(r, 0x44, u16at(r, 0x44) - it[0x2C]);
    if (slot != 2) put16(r, 0x46, u16at(r, 0x46) - it[0x2D]);
    r[0x59] = (uint8_t)(r[0x59] - it[0x2F]);
}

// sub_33909: equip item (0-based) into a slot and add its stat bonuses.
void CHP::status_equip_apply(uint8_t* r, int item0, int slot) {
    if (slot == 1) r[0x5B] = (uint8_t)(item0 + 1);
    else if (slot == 2) r[0x5A] = (uint8_t)(item0 + 1);
    else if (slot == 3) r[0x5D] = (uint8_t)(item0 + 1);
    if (item0 < 0 || item0 >= MAX_ITEMS) return;
    const uint8_t* it = item_data_[item0];
    r[0x40] = (uint8_t)(r[0x40] + it[0x28]);
    r[0x41] = (uint8_t)(r[0x41] + it[0x29]);
    r[0x42] = (uint8_t)(r[0x42] + it[0x2A]);
    r[0x43] = (uint8_t)(r[0x43] + it[0x2B]);
    put16(r, 0x44, u16at(r, 0x44) + it[0x2C]);
    if (slot != 2) put16(r, 0x46, u16at(r, 0x46) + it[0x2D]);
    r[0x59] = (uint8_t)(r[0x59] + it[0x2F]);
}

// sub_35755: sound effect (1 = menu.VOC, 2 = into.VOC, 3 = flying.VOC); silent when
//   byte_C5CE0 (= config_data[2]) is 0.
void CHP::status_play_sfx(int idx) {
    if (state_.config_data[2] == 0) return;
    static const char* const kName[4] = { nullptr, "menu.voc", "into.voc", "flying.voc" };
    if (idx >= 1 && idx <= 3) Audio::play_voc(game_path(kName[idx]));
}

// sub_32D94: item list of the discard popup (6 rows, 0x18 apart, x 0x34..0xC3) plus the
//   "Discard" button on the right.
void CHP::status_discard_list(int sel_idx, int top) {
    const int s = status_inv_slot();
    for (int i = 0; i < 6; i++) {
        const int y0 = 8 + 0x18 * i;
        const int idx = top + i;
        const int v = idx < BLACKBOX_SLOT_ITEMS ? (g_slot_items[s][idx] & 0xFF) : 0;
        const int id0 = v - 1;
        if (id0 >= 0) {
            if (idx == sel_idx) draw_bevel_box(0x34, y0, 0xC3, y0 + 0x17);
            else { gfx_fill(0x34, y0, 0x8F, 0x17, 0); gfx_hline(0x34, y0 + 0x17, 0x8F, 6); }
            const SpriteEntry* sp = item_box_sprites_.get(id0);
            if (sp && sp->pixel_data)       // adjusted by sub_1FDC4: [+4] = x_off + 0x0C, [+6] = -(word2 + 0x10)
                blit_sprite_rle((int16_t)(sp->x_offset + 0x0C) + 0x34, -(int16_t)(sp->word2 + 0x10) + 0x2E + 0x18 * i, sp->pixel_data);
            if (id0 < MAX_ITEMS) {
                const char* name = (const char*)item_data_[id0] + 1;
                const int len = (int)strlen(name);
                draw_status_text(0x82 - (halfv(len) << 3), 9 + 0x18 * i, name);
                const char* en = (const char*)item_data_[id0] + 0x10;
                const int l2 = (int)strlen(en);
                draw_mini_string(0x82 - 5 * halfv(l2), 0x1A + 0x18 * i, en, mini_font_color_);
            }
        } else {
            gfx_fill(0x34, y0, 0x8F, 0x17, 0);
            gfx_hline(0x34, y0 + 0x17, 0x8F, 6);
        }
    }
    draw_bevel_box(0xDC, 0x64, 0x118, 0x76);
    draw_status_text(0xE6, 0x66, kChpClassDiscardLabel);
}

// sub_32FCD: "choose an item to discard" popup shown when the inventory is full.
//   Picking an item and pressing the upper-right button (0xDC..0x118, 0x64..0x76)
//   discards it and closes; the lower button (0x78..0x8A) just closes.
//   The original copies the background from unk_DA370 with sub_1D9A8; the port uses the
//   whole current screen snapshot status_snap_.
void CHP::status_discard_popup() {
    uint8_t* bb = video_.get_backbuffer();
    const int s = status_inv_slot();
    int sel = 0;                                      // ebp
    int off = 0;                                      // var_8 (thumb offset)
    int hover = 0, last_hover = 0;                    // var_1C / var_20
    const int count0 = g_slot_item_count[s];
    float th = count0 < 6 ? 123.0f : (float)((long double)kChpStatusFlt51D78 / ((long double)count0 / (long double)kChpStatusFlt51D74));  // var_10
    const float fl_room = (float)((long double)kChpStatusFlt51D78 - (long double)th);   // var_18

    auto count = [&]() { return (int)g_slot_item_count[s]; };
    auto redraw = [&]() {                              // drawing part of loc_33794
        memcpy(bb, status_snap_.data(), status_snap_.size());
        status_discard_list(sel, discard_top_);
        if (class_arrow_up_.size() > 4) blit_sprite_transparent(0xC4, 8, class_arrow_up_.data());
        if (class_arrow_dn_.size() > 4) blit_sprite_transparent(0xC4, 0x8E, class_arrow_dn_.data());
        gfx_fill(0xC4, 0x12, 0x0F, 0x7C, 3);
        draw_bevel_box(0xC4, 0x12 + off, 0xD3, (int)th + 0x12 + off);
    };
    status_wait_left_release();
    status_wait_right_release();
    discard_top_ = 0;
    redraw();
    status_flip();

    for (;;) {                                          // loc_33149
        if (dos_exit_) return;
        status_poll();
        const int item = (sel >= 0 && sel < BLACKBOX_SLOT_ITEMS) ? (g_slot_items[s][sel] & 0xFF) : 0;
        const int x = status_ev_.x, y = status_ev_.y;
        bool clickable_state = true;
        if (item != 0 && x >= 0xDC && x <= 0x118 && y >= 0x64 && y <= 0x76) {
            hover = 1;
            if (status_ev_.code == 1) {                 // discard
                g_slot_items[s][sel] = 0;
                status_inv_remove(sel);
                return;
            }
        } else if (x >= 0xDC && x <= 0x118 && y >= 0x78 && y <= 0x8A) {
            hover = 2;
            if (status_ev_.code == 1) { status_wait_left_release(); return; }
        } else {
            hover = 0;
        }
        if (status_ev_.code == 1 && clickable_state) {   // loc_3323E
            if (x >= 0x34 && x <= 0xC3 && y >= 8 && y <= 0x97) sel = discard_top_ + (y - 8) / 0x18;
            if (x >= 0xC4 && x <= 0xD2 && y >= 0x12 + off && !((long double)y > (long double)(0x12 + off) + (long double)th)) {
                // Thumb drag
                int grab = y;
                const int lock_x = x;
                status_flip();
                for (;;) {                              // loc_332FE
                    blit_sprite_transparent(status_ev_.x, status_ev_.y, status_hand_.data());
                    status_flip();
                    if (status_ev_.code != 1 || dos_exit_) break;
                    status_poll();
                    status_ev_.x = lock_x;               // sub_28D1D: lock horizontal movement
                    if (grab < status_ev_.y) {
                        const int d = status_ev_.y - grab;
                        off += d; grab += d;
                        const long double fv = (long double)off + (long double)th;
                        if (fv > (long double)kChpStatusFlt51D78) {
                            const float fvf = (float)fv;
                            grab = (int)((long double)grab - ((long double)fvf + (long double)kChpStatusFlt51D7C));
                            off = (int)(long double)fl_room;
                        }
                        discard_top_ = status_scroll_to_top(off, (int)th, count());
                    } else if (grab > status_ev_.y) {
                        const int d = grab - status_ev_.y;
                        off -= d; grab -= d;
                        if (off < 0) off = 0;
                        discard_top_ = status_scroll_to_top(off, (int)th, count());
                    } else continue;
                    redraw();
                }
                goto L_after_click;
            }
            if (x >= 0xC4 && x <= 0xD3 && y >= 8 && y <= 0x11) {                   // up arrow
                if (discard_top_ > 0) discard_top_--;
                off = status_top_to_scroll(discard_top_, count());
                goto L_after_click;
            }
            if (x >= 0xC4 && x <= 0xD3 && y >= 0x8E && y <= 0x97) {                 // down arrow
                if (count() - 6 > discard_top_) discard_top_++;
                off = status_top_to_scroll(discard_top_, count());
                goto L_after_click;
            }
            if (x >= 0xC4 && x <= 0xD3 && y >= 0x12 && y <= 0x8D) {                 // thumb track
                off = y - 0x12;
                const long double fv = (long double)off + (long double)th;
                if (fv > (long double)kChpStatusFlt51D78) off = (int)(long double)fl_room;
                discard_top_ = status_scroll_to_top(off, (int)th, count() - 1);     // the original passes count-1 only here
            }
        L_after_click:
            redraw();                                    // loc_33794
            status_flip();
            status_wait_left_release();
        }
        // loc_33860: recolour the label when the hovered button changes
        if (last_hover != hover) {
            if (hover == 1) { font_.set_text_mode(4, 0x100, 0xFF); font_.draw_string(bb, SCREEN_W, SCREEN_H, 0xE6, 0x66, kChpClassDiscardLabel); }
            font_.set_text_mode(4, 0x1000, 0xFF);
            if (last_hover == 1) font_.draw_string(bb, SCREEN_W, SCREEN_H, 0xE6, 0x66, kChpClassDiscardLabel);
            status_flip();
            last_hover = hover;
        }
    }
}

// ==== sub_128DA: screen body ====
//   Returns -1 when this character may not change class (the caller shows a message),
//   0 for the lower-right close button, 1 for the bottom row (back to the status window).
int CHP::status_inventory_screen(int& sel_io, int& top_io) {
    class_load_assets();
    status_load_assets();
    uint8_t* bb = video_.get_backbuffer();
    int ebp = sel_io;                                   // selected character
    int var4 = 1;                                       // 1 = clean state, no tooltip or highlight
    if (!class_can_change(ebp)) return -1;
    class_spr_row_ = class_sprite_row(ebp);
    class_top_ = top_io;                                // dword_8FF40
    auto count = [&]() { return (int)state_.party_count; };
    auto load_page = [&]() {
        if (!status_load_pcx("cc.pcx", false, status_snap_)) {   // "cc.PCX read error!" -> exit
            fprintf(stderr, "CHP: error: cannot read cc.PCX\n");
            dos_exit_ = true;
            return false;
        }
        return true;
    };
    auto calc_thumb = [&]() {                           // dword_1271CC
        if (count() < 6) status_thumb_h_ = 123.0f;
        else status_thumb_h_ = (float)((long double)kChpStatusFlt51D78 / ((long double)count() / (long double)kChpStatusFlt51D74));
    };
    auto in = [&](int x0, int x1, int y0, int y1) {
        return status_ev_.x >= x0 && status_ev_.x <= x1 && status_ev_.y >= y0 && status_ev_.y <= y1;
    };

    if (!load_page()) return 0;
    calc_thumb();
    class_tab_ = class_tab_of(ebp);
    class_compose(ebp);
    memcpy(status_snap_.data(), bb, status_snap_.size());          // sub_1DABB(C5EA4 -> DA370): the finished screen
    status_play_sfx(1);

    // loc_12A89: the old screen (EB4E0) slides right while the new one appears from the left.
    {
        int s = 0, e = 0x13E;
        do {
            gfx_copy_rect(status_snap_.data(), bb, s + s % 4, 0x94);
            gfx_copy_rect(status_prev_.data(), bb + s, e + e % 4, 0x94);
            e -= 2; s += 2;
            status_flip_vsync();
        } while (s <= 0xA0 && !dos_exit_);
    }

L_restart:                                              // loc_12AF4: rebuild around the selected character
    class_spr_row_ = class_sprite_row(ebp);
    if (!load_page()) return 0;
    memcpy(bb, status_snap_.data(), status_snap_.size());
    calc_thumb();
    class_tab_ = class_tab_of(ebp);
    class_compose(ebp);
    status_flip();
    status_wait_left_release();

L_top:                                                  // loc_12C7C
    if (dos_exit_) return 0;
    // ---- class cell click ----
    for (int r = 0; r < 4; r++) {
        bool hit = false;
        for (int c = 0; c < 4; c++) {
            if (in(0x1D + 0x1E * c, 0x3A + 0x1E * c, r * 0x1F + 2, r * 0x1F + 0x20) && status_ev_.code == 1) {
                class_cell_ = r * 4 + c;
                memset(class_short_ + 1, 0, 0x14);
                class_refresh(ebp);
                var4 = 0;
                hit = true;
                break;
            }
        }
        if (hit) break;
    }
    // ---- show the shortfall while the mouse is over the left requirement strip ----
    if (in(4, 0x18, 9, 0x8F)) {
        const int row = (status_ev_.y - 9) / 0x11;
        const int cls = class_cell_class(class_tab_, class_cell_);
        int edi = 0, edx = 0;
        do {
            const int b = class_req_at(cls * 18 + edi);
            edi++;
            if (b) edx++;
        } while (edx <= row && edi < 0x14);
        const int bh = class_short_[edi];               // byte_5A6B4[edi] = shortfall[edi-1]
        edi--;
        if (bh > 0 && edi < 0x14) {
            bool shown = true;
            if (bh == 1 && edi == 0x10) {
                const int dl = class_req_at(cls * 18 + 0x10);
                if (dl < bh) shown = false;
                else {
                    const std::vector<uint8_t>* sp = dl == 1 ? &class_prev_a_ : (dl == 2 ? &class_prev_b_ : nullptr);
                    if (sp && !sp->empty()) blit_sprite_rle(4, row * 0x11 + 9, sp->data());
                    else if (!sp) shown = false;
                }
            } else {
                if (!class_tip_bg_.empty()) blit_sprite_rle(4, row * 0x11 + 9, class_tip_bg_.data());
                set_mini_font_color(0x14);
                draw_mini_number(8, row * 0x11 + 0x0F, class_short_[edi + 1], mini_font_color_);
                set_mini_font_color(1);
            }
            if (shown) {
                status_flip();
                do {                                    // until the mouse leaves that row
                    status_poll();
                } while (!dos_exit_ && in(4, 0x18, row * 0x11 + 9, row * 0x11 + 0x1A));
                class_refresh(ebp);
            }
        }
    }
    // loc_12EB6
    if (var4 != 1) { class_refresh(ebp); var4 = 1; }
    if (status_ev_.code != 1) goto L_poll;              // no button press -> poll again

    // ==== left button handling ====
    // ---- right-hand character list ----
    if (in(0xAB, 0x12B, 8, 0x97)) {
        const int idx = class_top_ + (status_ev_.y - 8) / 0x18;
        if (class_can_change(idx)) {
            ebp = idx < count() ? idx : count() - 1;
            memset(class_short_ + 1, 0, 0x14);
            goto L_restart;
        }
        status_message(kChpClassMsgBad);
        class_refresh(ebp);
    }
    // ---- scroll bar (loc_12F61) ----
    {
        const bool in_bar_x = status_ev_.x >= 0x12A && status_ev_.x <= 0x139;
        const int top_y = 0x0F + status_thumb_off_;
        if (in_bar_x && top_y <= status_ev_.y &&
            !((long double)status_ev_.y > (long double)top_y + (long double)status_thumb_h_)) {
            int grab = status_ev_.y;                    // var_1C
            const int lock_x = status_ev_.x;            // sub_28D1D(2*x): lock horizontal movement while dragging
            status_flip();
            while (status_ev_.code == 1 && !dos_exit_) {   // loc_12FE2
                status_poll();
                status_ev_.x = lock_x;
                if (grab < status_ev_.y) {                 // down
                    const int d = status_ev_.y - grab;
                    status_thumb_off_ += d;
                    grab += d;
                    const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
                    if (fv > (long double)kChpStatusFlt51D78) {
                        const float fvf = (float)fv;
                        grab = (int)((long double)grab - ((long double)fvf + (long double)kChpStatusFlt51D7C));
                        status_thumb_off_ = (int)((long double)kChpStatusFlt51D78 - (long double)status_thumb_h_);
                    }
                    class_top_ = status_scroll_to_top(status_thumb_off_, (int)status_thumb_h_, count());
                    if (count() == 9 && class_top_ == 4) class_top_ = 3;
                } else if (grab > status_ev_.y) {          // up
                    const int d = grab - status_ev_.y;
                    status_thumb_off_ -= d;
                    grab -= d;
                    if (status_thumb_off_ < 0) status_thumb_off_ = 0;
                    class_top_ = status_scroll_to_top(status_thumb_off_, (int)status_thumb_h_, count());
                } else continue;
                class_refresh(ebp);
            }
            goto L_tabs;
        }
    }
    if (in(0x12A, 0x139, 5, 0x0F)) {                     // up arrow
        if (class_top_ > 0) class_top_--;
        status_thumb_off_ = status_top_to_scroll(class_top_, count());
        class_refresh(ebp);
        goto L_tabs;
    }
    if (in(0x12A, 0x139, 0x8A, 0x93)) {                  // down arrow
        if (count() - 6 > class_top_) class_top_++;
        status_thumb_off_ = status_top_to_scroll(class_top_, count());
        class_refresh(ebp);
        goto L_tabs;
    }
    if (in(0x12A, 0x139, 0x0F, 0x8A)) {                  // thumb track
        status_thumb_off_ = status_ev_.y - 0x0F;
        const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
        if (fv > (long double)kChpStatusFlt51D78)
            status_thumb_off_ = (int)((long double)kChpStatusFlt51D78 - (long double)status_thumb_h_);
        class_top_ = status_scroll_to_top(status_thumb_off_, (int)status_thumb_h_, count());
        class_refresh(ebp);
    }

L_tabs:                                                 // loc_132B2 (only coordinates are tested, not the button state)
    {
        int new_tab = -1;
        if (in(0x1F, 0x34, 0x84, 0x90) && class_tab_ < 3) new_tab = 0;
        else if (in(0x3F, 0x54, 0x84, 0x90) && class_tab_ < 3) new_tab = 1;
        else if (in(0x5F, 0x74, 0x84, 0x90) && class_tab_ < 3) new_tab = 2;
        if (new_tab >= 0) {
            class_tab_ = new_tab;
            memset(class_short_ + 1, 0, 0x14);
            class_refresh(ebp);
            goto L_wait_release;
        }
    }
    if (in(0x7F, 0x94, 0x84, 0x90)) {                    // confirm button
        blit_sprite_rle(0x7F, 0x84, class_ok_btn_.data());
        status_flip();
        flip_wait(kVsync1376D);                          // sub_1376D (33 vsync)
        uint8_t* rec = party_rec(ebp);
        if (class_req_ok(rec, class_sel_class_)) {
            status_play_sfx(2);
            rec[CL_CLASS] = (uint8_t)class_sel_class_;
            if (class_spr_row_ < kChpClassSprRows && class_sel_class_ >= 0 && class_sel_class_ < kChpClassSprCols)
                put16(rec, CL_TOWN, kChpClassSprite[class_spr_row_ * kChpClassSprCols + class_sel_class_]);
            auto unequip = [&](int off, int slot) {      // return equipped items to the inventory
                if (rec[off] == 0) return;
                if (!status_inv_add(rec[off] - 1)) status_discard_popup();
                if (slot) status_equip_strip(rec, slot);
                else rec[off] = 0;
            };
            unequip(0x5B, 1);
            unequip(0x5A, 2);
            unequip(0x5D, 3);
            unequip(0x5E, 0);
            unequip(0x5F, 0);
        }
        class_recalc_stats(rec);                          // always runs, even when requirements are unmet (as in the original)
        class_refresh(ebp);
        goto L_wait_release;
    }
    if (in(0, 0x13F, 0x94, 0xA3)) {                      // bottom row: back to the status window
        memcpy(bb, status_prev_.data(), status_prev_.size());
        top_io = class_top_;
        sel_io = ebp;
        return 1;
    }
L_wait_release:                                         // loc_135CC
    status_wait_left_release();
L_poll:                                                 // loc_135D1
    status_poll();
    if (dos_exit_) return 0;
    if (in(0xFC, 0x135, 0xBB, 0xC5) && status_ev_.code == 1) {
        if (!status_btn_b_.empty()) blit_sprite_rle(0xFC, 0xBB, status_btn_b_.data());
        status_flip();
        status_wait_left_release();
        return 0;
    }
    goto L_top;
}
