// chp_equip.cpp -- port of sub_30AFE (equipment screen, equment.pcx), the lower-right
// button screen of the status window.
//
//   sub_30AFE  status_equipment_screen  screen body (transition + event loop)
//   sub_3061C  equip_draw               background + equipment icons + stats + inventory + portrait
//   sub_31AF2  equip_drag               drag an item to equip, unequip, move or discard it
//   sub_17B71  equip_insert             insert into the middle of the inventory
//   sub_2E8C1  equip_is_kept            items that cannot be discarded
//   sub_1C995  equip_line               line drawing (the X over unusable slots)
//
// The inventory/equipment helpers (sub_32A1A, sub_329BD, sub_339B5, sub_33909) and the
// status variables live in chp_class.cpp. Numeric tables come from chp_equip_data.h;
// this screen needs no extra sprites.
#include "chp.h"
#include "chp_equip_data.h"
#include "chp_class_data.h"
#include "chp_status_data.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

namespace {
constexpr int kEqOff[5] = { 0x5B, 0x5A, 0x5D, 0x5E, 0x5F };      // equipment slots from the left (= drag source ids 1..5)
constexpr int kEqX[5]   = { 0x05, 0x25, 0x44, 0x5F, 0x7F };
constexpr int kTeamSlotBytes = 0x64;                              // max items in one team inventory
inline int s16(const uint8_t* r, int o) { return (int16_t)(r[o] | (r[o + 1] << 8)); }
inline int s32(const uint8_t* r, int o) { return (int32_t)(r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24)); }
}  // namespace

// Thumb length: 123 when count < 6, otherwise 123 / (count / 6) (x87 extended precision,
// stored as float).
float CHP::equip_thumb_h(int count) {
    if (count < 6) return 123.0f;
    return (float)((long double)kChpStatusFlt51D78 / ((long double)count / (long double)kChpStatusFlt51D74));
}

void CHP::equip_draw_bar(int off, float th) {
    draw_bevel_box(0x12A, 0x0F + off, 0x139, (int)th + 0x0F + off);
}

// ==== sub_3061C: draw the equipment screen (no flip) ====
void CHP::equip_draw(int sel, int top) {
    uint8_t* bb = video_.get_backbuffer();
    memcpy(bb, status_snap_.data(), status_snap_.size());           // sub_1DABB(DA370 -> C5EA4)
    if (sel < 0 || sel >= (int)state_.party_count) return;
    const uint8_t* rec = party_rec(sel);

    for (int k = 0; k < 5; k++) {                                   // five equipment icons (+0x5B, +0x5A, +0x5D, +0x5E, +0x5F)
        const int item = rec[kEqOff[k]];
        if (item) status_item_icon(kEqX[k], item);
    }

    char buf[48];
    set_mini_font_color(0x1B); draw_mini_string(0x44, 0x21, "HP:", mini_font_color_);
    set_mini_font_color(0x21); draw_mini_string(0x44, 0x2A, "MP:", mini_font_color_);
    set_mini_font_color(0x11); draw_mini_string(0x41, 0x33, "LV:     EXP:", mini_font_color_);
    set_mini_font_color(0x01);
    snprintf(buf, sizeof(buf), "%d / %d", s32(rec, 0x48) / 100, s32(rec, 0x4C) / 100);
    draw_mini_string(0x5B, 0x21, buf, mini_font_color_);
    snprintf(buf, sizeof(buf), "%d / %d", s16(rec, 0x50), s16(rec, 0x52));
    draw_mini_string(0x5B, 0x2A, buf, mini_font_color_);
    snprintf(buf, sizeof(buf), "%d      %d", (int)rec[0x2B], (int)rec[0x3D]);
    draw_mini_string(0x56, 0x33, buf, mini_font_color_);

    draw_status_text(0x3F, 0x0B, (const char*)rec + 2);
    draw_mini_number(0x1F, 0x43, s16(rec, 0x44), mini_font_color_);
    draw_mini_number(0x4F, 0x43, rec[0x40], mini_font_color_);
    draw_mini_number(0x7D, 0x43, rec[0x41], mini_font_color_);
    draw_mini_number(0x1F, 0x57, s16(rec, 0x46), mini_font_color_);
    draw_mini_number(0x4F, 0x57, rec[0x42], mini_font_color_);
    draw_mini_number(0x7D, 0x57, rec[0x43], mini_font_color_);
    status_draw_gold(0x20, 0x86);                                   // sub_2E9BC

    // Inventory list, 6 rows of 0x18: icon + Johab name + English name (mini font)
    const int team = status_inv_slot();
    for (int row = 0; row < 6; row++) {
        const int idx = top + row;
        const int v = (idx >= 0 && idx < BLACKBOX_SLOT_ITEMS) ? (g_slot_items[team][idx] & 0xFF) : 0;
        const int id0 = v - 1;
        if (id0 < 0 || id0 >= MAX_ITEMS) continue;
        const SpriteEntry* sp = item_box_sprites_.get(id0);
        if (sp && sp->pixel_data)       // adjusted by sub_1FDC4: [+4] = x_off + 0x0C, [+6] = -(word2 + 0x10)
            blit_sprite_rle((int16_t)(sp->x_offset + 0x0C) + 0xA7, -(int16_t)(sp->word2 + 0x10) + 0x2B + 0x18 * row, sp->pixel_data);
        const char* name = (const char*)item_data_[id0] + 1;
        const int len = (int)strlen(name);
        draw_status_text(0xF1 - ((len - (len >> 31)) >> 1) * 8, 6 + 0x18 * row, name);
        const char* en = (const char*)item_data_[id0] + 0x10;
        const int l2 = (int)strlen(en);
        draw_mini_string(0xF1 - 5 * ((l2 - (l2 >> 31)) >> 1), 0x16 + 0x18 * row, en, mini_font_color_);
    }

    load_small_portrait(rec[0x10]);                                 // sub_1FA10 + sub_1D874(5, 9)
    blit_sprite_rle(5, 9, portrait_small_);
}

// ==== sub_1C995: line drawing ====
//   Major axis length n = max(|dx|, |dy|); plots n+2 pixels, overshooting the end by one
//   as the original does.
void CHP::equip_line(int x1, int y1, int x2, int y2, uint8_t c) {
    uint8_t* bb = video_.get_backbuffer();
    const int dx = x2 - x1, dy = y2 - y1;
    const int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0), sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    const int adx = std::abs(dx), ady = std::abs(dy);
    const int n = adx > ady ? adx : ady;
    int ax = 0, ay = 0, x = x1, y = y1;
    for (int i = 0; i <= n + 1; i++) {
        if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) bb[y * SCREEN_W + x] = c;   // sub_1D851
        ay += ady;
        ax += adx;
        if (ax > n) { x += sx; ax -= n; }
        if (n < ay) { y += sy; ay -= n; }
    }
}

// sub_2E8C1: true when item+1 is in the 12-entry protected list.
bool CHP::equip_is_kept(int item_plus1) const {
    for (int i = 0; i < 12; i++)
        if ((item_plus1 & 0xFFFF) == kChpEquipKeepItems[i]) return true;
    return false;
}

// sub_17B71: insert item (0-based) at inventory cell idx.
//   If a free cell exists in the 100 cells from idx, shift idx..end back one and store at
//   idx. Otherwise pull items forward into a free cell in 0..idx; if none exists, show a
//   message and return false.
bool CHP::equip_insert(int item0, int idx) {
    const int s = status_inv_slot();
    int b = idx;
    while (b < kTeamSlotBytes && (inv_ref(s, b) & 0xFF)) b++;
    if (b == kTeamSlotBytes) {
        b = 0;
        while (b < idx && (inv_ref(s, b) & 0xFF)) b++;
        if (b == idx) {
            status_message(kChpEquipMsgFull);
            flip_wait(kVsync1366E); flip_wait(kVsync1366E); flip_wait(kVsync1366E);   // sub_1366E x 3 (51 vsync each)
            return false;
        }
        while (b < idx) { inv_ref(s, b) = inv_ref(s, b + 1); b++; }
    } else {
        b = BLACKBOX_SLOT_ITEMS - 1;                                    // the original starts at byte 0x257 (= 599)
        while (b > idx) { b--; inv_ref(s, b + 1) = inv_ref(s, b); }
    }
    inv_ref(s, b) = (uint16_t)((item0 + 1) & 0xFF);
    return true;
}

// ==== sub_31AF2: drag and drop an item ====
//   src: 1..5 = equipment slot (+0x5B, +0x5A, +0x5D, +0x5E, +0x5F), 0x64+n = inventory cell n.
void CHP::equip_drag(int sel, int item0, int src) {
    uint8_t* bb = video_.get_backbuffer();
    uint8_t* rec = party_rec(sel);
    const int team = status_inv_slot();
    int ox = status_ev_.x - 7, oy = status_ev_.y - 8;               // var_10 / var_C: top-left of the dragged icon (cursor - (7, 8))
    if (item0 < 0 || item0 >= MAX_ITEMS) return;
    const uint8_t* it = item_data_[item0];
    const int type = it[0];

    // ---- slots this item may go into (1 = allowed) ----
    uint8_t f[5];
    memcpy(f, kChpEquipFlagInit, 5);
    auto two_hand = [&](int w) { return w >= 0 && w < MAX_ITEMS && item_data_[w][0x25] == 2; };
    const uint8_t al = (uint8_t)(type - 1);
    switch (al) {
        case 0: case 1: case 2: case 3: case 4:                     // weapons: blocked when a two-handed weapon fills the shield slot
            if (rec[0x5A] != 0 && it[0x25] == 2) f[0] = 0;
            f[1] = 0; f[2] = 0;
            break;
        case 5: case 7:                                              // shield/off-hand: blocked while a two-handed weapon is equipped
            if (two_hand(rec[0x5B] - 1)) f[1] = 0;
            f[0] = 0; f[2] = 0;
            break;
        case 6:                                                      // armour
            f[0] = 0; f[1] = 0;
            break;
        default: break;
    }
    if (it[0x25] == 2) draw_mini_string(5, 0x62, kChpEquipStr2Hand, mini_font_color_);
    const int cls = rec[0x11];
    const int grp = cls < 164 ? kChpClassStat3[cls * 3] : 0;
    const int cap = cls < 164 ? kChpClassStat3[cls * 3 + 2] : 0;
    if ((grp < 19 ? kChpEquipWeaponTab[grp] : 0) != it[0]) {
        if (f[1] == 0 && f[2] == 0) draw_mini_string(0x26, 0x62, kChpEquipStrJob, mini_font_color_);
        f[0] = 0;
    }
    if (cap != 1 && it[0] == 6) {
        if (f[0] == 0 && f[2] == 0) draw_mini_string(0x26, 0x62, kChpEquipStrJob, mini_font_color_);
        f[1] = 0;
    }
    if (cap != 2 && it[0] == 8) {
        if (f[0] == 0 && f[2] == 0) draw_mini_string(0x26, 0x62, kChpEquipStrJob, mini_font_color_);
        f[1] = 0;
    }
    // X over each unusable slot (colour 0x16, two pixels thick)
    for (int k = 0; k < 5; k++) {
        if (f[k] == 1) continue;
        const int L = 5 + 0x1E * k, R = 0x1D + 0x1E * k;
        equip_line(L, 0x67, R, 0x81, 0x16);
        equip_line(L - 1, 0x67, R - 1, 0x81, 0x16);
        equip_line(R, 0x67, L, 0x81, 0x16);
        equip_line(R - 1, 0x67, L - 1, 0x81, 0x16);
    }
    status_flip();

    // ---- drag: the icon is drawn to the screen only (the original writes to A0000 ----
    // directly); the back buffer stays clean ----
    const SpriteEntry* sp = item_box_sprites_.get(item0);
    const uint8_t* spr = (sp && sp->pixel_data) ? sp->pixel_data : nullptr;
    std::vector<uint8_t> clean(bb, bb + (size_t)SCREEN_W * SCREEN_H);
    auto show = [&](bool vsync) {
        memcpy(bb, clean.data(), clean.size());
        if (spr) blit_sprite_rle_clipped(ox, oy, spr);
        if (vsync) status_flip_vsync(); else status_flip();
    };
    show(false);
    do {                                                            // loc_31E88
        status_poll();
        if (status_ev_.x - 7 != ox || status_ev_.y - 8 != oy) {
            ox = status_ev_.x - 7;
            oy = status_ev_.y - 8;
            show(true);
        }
    } while (status_ev_.code == 1 && !dos_exit_);
    memcpy(bb, clean.data(), clean.size());

    // ---- drop position ----
    const int mx = status_ev_.x, my = status_ev_.y;
    const int li = discard_top_ + (my - 5) / 0x18;                  // list cell under the cursor (top + row)
    const bool in_list = mx >= 0xA9 && mx <= 0x129 && my >= 5 && my <= 0x93;
    const bool in_slots = mx >= 5 && mx <= 0x98 && my >= 0x67 && my <= 0x81;
    const int k = in_slots ? (mx - 5) / 0x1E : 0;
    uint16_t& cnt = g_slot_item_count[team];

    // Clear equipment slot src (1..5); slots 1,2,3 also undo their stat bonuses (loc_3203F).
    auto clear_src_equip = [&](int sc) {
        if (sc >= 1 && sc <= 3) status_equip_strip(rec, sc);
        else if (sc == 4) rec[0x5E] = 0;
        else if (sc == 5) rec[0x5F] = 0;
    };

    if (src < 6) {
        if (in_list) {
            if (inv_ref(team, li) & 0xFF) {                         // loc_320E6: insert there (the original ignores the result too)
                equip_insert(item0, li);
                cnt++;
                if (src >= 1 && src <= 5) clear_src_equip(src);
            } else {                                                // otherwise use the first free cell
                int i = 0;
                while ((int)cnt > i && (inv_ref(team, i) & 0xFF)) i++;
                inv_ref(team, i) = (uint16_t)((item0 + 1) & 0xFF);
                cnt++;
                if (src >= 1 && src <= 5) clear_src_equip(src);
            }
        } else if (in_slots) {                                      // loc_3214E: equipment slot -> equipment slot
            const int cl = (item0 + 1) & 0xFF;
            switch (k) {
                case 0: case 1: case 2: {                           // weapon/shield/armour slot: only from accessory slot 4 or 5
                    const int dst_off = kEqOff[k];
                    if ((src == 4 || src == 5) && rec[dst_off] == 0 && f[k] == 1) {
                        status_equip_apply(rec, item0, k + 1);
                        rec[kEqOff[src - 1]] = 0;
                    }
                    break;
                }
                case 3: case 4: {                                   // accessory slot: only when empty
                    const int dst_off = kEqOff[k];
                    if (rec[dst_off] == 0) {
                        rec[dst_off] = (uint8_t)cl;
                        if (src >= 1 && src <= 5) clear_src_equip(src);
                    }
                    break;
                }
            }
        }
    } else if (src >= 0x64) {
        const int si = src - 0x64;                                  // source inventory cell
        const int cl = (item0 + 1) & 0xFF;
        if (in_list) {                                              // loc_3240B
            const int bh = inv_ref(team, li) & 0xFF;
            if (bh == 0) {
                inv_ref(team, li) = (uint16_t)cl;
                inv_ref(team, si) = 0;
                status_inv_remove(si);
            } else {                                                // swap
                inv_ref(team, si) = (uint16_t)bh;
                inv_ref(team, li) = (uint16_t)cl;
            }
        } else if (in_slots) {                                      // loc_32496: inventory -> equipment slot
            auto take = [&]() { inv_ref(team, si) = 0; status_inv_remove(si); };
            switch (k) {
                case 0: case 1: case 2: {
                    const int off = kEqOff[k];
                    if (rec[off] == 0 && f[k] == 1) {               // free slot: equip directly
                        status_equip_apply(rec, item0, k + 1);
                        take();
                    } else if (rec[0x5E] == 0 && f[k] == 1) {       // push the equipped item into accessory slot 4, then equip
                        const int old = rec[off];
                        status_equip_strip(rec, k + 1);
                        rec[0x5E] = (uint8_t)old;
                        status_equip_apply(rec, item0, k + 1);
                        take();
                    } else if (rec[0x5F] == 0 && f[k] == 1) {       // the same with accessory slot 5
                        const int old = rec[off];
                        status_equip_strip(rec, k + 1);
                        rec[0x5F] = (uint8_t)old;
                        status_equip_apply(rec, item0, k + 1);
                        take();
                    }
                    break;
                }
                case 3:
                    if (rec[0x5E] == 0 && f[3] == 1) { rec[0x5E] = (uint8_t)cl; take(); }
                    break;
                case 4:
                    if (rec[0x5F] == 0 && f[4] == 1) { rec[0x5F] = (uint8_t)cl; take(); }
                    break;
            }
        }
    }

    // ---- loc_32049: dropping on the trash cell (0x18..0x33, 0xAC..0xBF) discards the ----
    // item, unless it is protected ----
    if (mx >= 0x18 && mx <= 0x33 && my >= 0xAC && my <= 0xBF && !equip_is_kept(item0 + 1)) {
        if (src >= 1 && src <= 5) clear_src_equip(src);
        // loc_32981: discarding from equipment still clears inventory cell (src-0x64) and
        // calls sub_329BD with a negative index. This keeps the original bug: the first
        // inventory item disappears and the count drops by one.
        inv_ref(team, src - 0x64) = 0;
        status_inv_remove(src - 0x64);
    }
}

// ==== sub_30AFE: equipment screen body ====
//   sel = selected character. Returns 0 for the lower-right close button (closes the whole
//   status window) and 1 for the bottom row (back to the status window).
int CHP::status_equipment_screen(int sel) {
    status_load_assets();
    if (!item_box_sprites_loaded_) item_box_sprites_loaded_ = load_spr(item_box_sprites_, ".", "ITEM.SPR", false);
    uint8_t* bb = video_.get_backbuffer();
    const int team = status_inv_slot();
    auto count = [&]() { return (int)g_slot_item_count[team]; };
    int off = 0;                                                    // var_8: thumb offset (local to this screen)
    float th = equip_thumb_h(count());                              // var_10
    auto in = [&](int x0, int x1, int y0, int y1) {
        return status_ev_.x >= x0 && status_ev_.x <= x1 && status_ev_.y >= y0 && status_ev_.y <= y1;
    };
    auto load_page = [&]() {
        if (!status_load_pcx("equment.pcx", false, status_snap_)) {   // "equment.PCX read error!" -> exit
            fprintf(stderr, "CHP: error: cannot read equment.PCX\n");
            dos_exit_ = true;
            return false;
        }
        return true;
    };
    auto compose = [&]() { equip_draw(sel, discard_top_); equip_draw_bar(off, th); };
    auto redraw = [&]() {                                           // loc_312B8
        compose();
        status_flip();
        status_wait_left_release();
    };

    status_wait_right_release();
    discard_top_ = 0;                                               // dword_1271F0 (top row of the list)
    if (!load_page()) return 0;
    compose();
    memcpy(status_snap_.data(), bb, status_snap_.size());           // sub_1DABB(C5EA4 -> DA370): the finished screen
    status_play_sfx(1);

    // Right half (0xA0..) stays fixed while the old screen (EB4E0) slides left (81 frames).
    {
        int w = 0x140;
        for (int k = 0; k < 0x51 && !dos_exit_; k++) {
            gfx_copy_rect(status_snap_.data() + 0xA0, bb + 0xA0, 0xA0, 0x94);
            gfx_copy_rect(status_prev_.data() + 2 * k, bb, w + w % 4, 0x94);
            w -= 2;
            status_flip_vsync();
        }
    }
    if (!load_page()) return 0;
    memcpy(bb, status_snap_.data(), status_snap_.size());
    compose();
    status_flip();
    status_wait_left_release();

    for (;;) {                                                      // loc_30D3C: handle the current event
        if (dos_exit_) return 0;
        uint8_t* rec = party_rec(sel);
        const int ev = status_ev_.code;
        bool do_redraw = false;

        if (ev == 2) {                                              // ---- right click: item description ----
            if (in(0x99, 0x127, 8, 0x97)) {
                const int idx = discard_top_ + (status_ev_.y - 8) / 0x18;
                status_item_popup(inv_ref(team, idx) & 0xFF);       // the original calls it even for 0
            } else if (in(5, 0x98, 0x67, 0x81)) {
                const int kk = (status_ev_.x - 5) / 0x1E;
                if (kk <= 4 && rec[kEqOff[kk]] != 0) status_item_popup(rec[kEqOff[kk]]);
            }
        } else if (ev == 1) {                                       // ---- left click ----
            if (in(0x60, 0x99, 0x87, 0x91)) {                       // (A) skill/magic grid (only while held)
                status_draw_skill_grid(rec, 9);
                if (!status_btn_a_.empty()) blit_sprite_rle(0x5F, 0x87, status_btn_a_.data());
                status_flip();
                do { status_poll(); } while (status_ev_.code == 1 && !dos_exit_);
                do_redraw = true;
            } else if (in(0x12A, 0x139, 0, 0xC7) &&
                       0x0F + off <= status_ev_.y &&
                       !((long double)status_ev_.y > (long double)(0x0F + off) + (long double)th)) {   // (B) thumb drag
                int grab = status_ev_.y;                            // var_C
                const int lock_x = status_ev_.x;                    // sub_28D1D(2*x): lock horizontal movement while dragging
                const long double room = (long double)kChpStatusFlt51D78 - (long double)th;   // var_18
                auto draw_hand = [&]() {
                    compose();
                    blit_sprite_transparent(status_ev_.x, status_ev_.y, status_hand_.data());
                    status_flip();
                };
                draw_hand();
                while (status_ev_.code == 1 && !dos_exit_) {        // loc_31480
                    status_poll();
                    status_ev_.x = lock_x;
                    if (grab < status_ev_.y) {
                        const int d = status_ev_.y - grab;
                        off += d; grab += d;
                        const long double fv = (long double)off + (long double)th;
                        if (fv > (long double)kChpStatusFlt51D78) {
                            const float fvf = (float)fv;
                            grab = (int)((long double)grab - ((long double)fvf + (long double)kChpStatusFlt51D7C));
                            off = (int)room;
                        }
                    } else if (grab > status_ev_.y) {
                        const int d = grab - status_ev_.y;
                        off -= d; grab -= d;
                        if (off < 0) off = 0;
                    }
                    discard_top_ = status_scroll_to_top(off, (int)th, count());
                    draw_hand();
                }
                do_redraw = true;
            } else if (in(0x12A, 0x139, 5, 0x0F)) {                 // (C) up arrow
                if (discard_top_ > 0) discard_top_--;
                off = status_top_to_scroll(discard_top_, count());
                do_redraw = true;
            } else if (in(0x12A, 0x139, 0x8A, 0x93)) {              // (D) down arrow
                if (count() - 6 > discard_top_) discard_top_++;
                off = status_top_to_scroll(discard_top_, count());
                do_redraw = true;
            } else if (in(0x12A, 0x139, 0x0F, 0x8A)) {              // (E) thumb track
                off = status_ev_.y - 0x0F;
                const long double fv = (long double)off + (long double)th;
                if (fv > (long double)kChpStatusFlt51D78) off = (int)((long double)kChpStatusFlt51D78 - (long double)th);
                discard_top_ = status_scroll_to_top(off, (int)th, count() - 1);   // the original passes count-1 only here
                do_redraw = true;
            } else if (in(0x99, 0x127, 8, 0x97)) {                  // (F) drag an item from the inventory list
                const int idx = discard_top_ + (status_ev_.y - 8) / 0x18;
                const int v = inv_ref(team, idx) & 0xFF;
                if (v != 0) equip_drag(sel, v - 1, 0x64 + idx);
                do_redraw = true;
            } else if (in(5, 0x98, 0x67, 0x81)) {                   // (G) drag an item from an equipment slot
                const int kk = (status_ev_.x - 5) / 0x1E;
                if (kk <= 4 && rec[kEqOff[kk]] != 0) equip_drag(sel, rec[kEqOff[kk]] - 1, kk + 1);
                th = equip_thumb_h(count());                         // only this branch recomputes the thumb length (off is kept)
                do_redraw = true;
            } else if (in(0, 0x13F, 0x94, 0xA3)) {                  // (H) bottom row: back to the status window
                memcpy(status_prev_.data(), bb, status_prev_.size());   // sub_1DABB(C5EA4 -> EB4E0)
                return 1;
            } else {
                do_redraw = true;                                    // anywhere else: just redraw
            }
        }
        if (do_redraw) redraw();

        // loc_3131E: next event; the close button shows its pressed state and returns
        status_poll();
        if (dos_exit_) return 0;
        if (in(0xFC, 0x135, 0xBB, 0xC5) && status_ev_.code == 1) {
            if (!status_btn_b_.empty()) blit_sprite_rle(0xFC, 0xBB, status_btn_b_.data());
            status_flip();
            status_wait_left_release();
            return 0;
        }
    }
}
