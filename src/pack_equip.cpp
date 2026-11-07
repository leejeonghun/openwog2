// pack_equip.cpp: PACK equipment screen (equment.pcx).
//
//   sub_357C4  status_equipment_screen  screen body (slide transition + event loop)
//   sub_352E2  draw_contents            background, equip icons, stats, inventory list, portrait
//   sub_367B0  equip_drag               drag an item to equip, unequip, move or drop it
//   sub_37DB2  status_equip_apply       equip an item (adds its stat bonuses)
//   sub_37E5E  status_equip_strip       unequip an item (subtracts its stat bonuses)
//   sub_334F9  status_is_kept_item      items that cannot be dropped
//
// Coordinates, conditions and return values were read from PACK_code.ASM 46891..49152.
// CHP has the same UI (src/chp_equip.cpp, sub_30AFE/sub_31AF2); where the numbers differ the
// PACK values win (equip slot x = 5/0x25/0x44/0x5F/0x7F, portrait at (5,9)).
// Equip permission tables unk_70830 (166 classes x 3 bytes) and unk_70A22 (weapon group ->
// item type) are read from PACK.EXE data at runtime.
//
// Deviations:
//   * The original indexes inventory and gold through dword_1A00C8+2 (>>16); the port uses
//     cur_slot() (blackbox_slot_) to stay consistent with its global inventory model. Party
//     records still come from party_rec() (team +0x87 == blackbox_slot_), as in sub_22FED.
//   * sub_18B2A bakes [+2] += 0x10, [+4] += 0x0C and [+6] = -[+2] into every item.spr frame,
//     so the port draws at x = x_offset+0x0C, y = -(h+0x10); PackSprite keeps the file values.
#include "pack.h"
#include "pack_data.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "game_timer.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

namespace {
// Party record offsets, relative to +0x88 (checked in sub_352E2/sub_367B0/sub_37DB2).
constexpr int R_NAME = 0x02, R_FACE = 0x10, R_CLASS = 0x11, R_LEVEL = 0x2B;
constexpr int R_EXPB = 0x3D;
constexpr int R_STAT_B0 = 0x40, R_STAT_B1 = 0x41, R_STAT_B2 = 0x42, R_STAT_B3 = 0x43;
constexpr int R_STAT_W0 = 0x44, R_STAT_W1 = 0x46;
constexpr int R_HP = 0x48, R_HPMAX = 0x4C, R_MP = 0x50, R_MPMAX = 0x52;
constexpr int R_STAT_SUM = 0x59;
constexpr int R_EQUIP_W = 0x5B, R_EQUIP_S = 0x5A, R_EQUIP_A = 0x5D;
constexpr int R_EQUIP_ACC0 = 0x5E, R_EQUIP_ACC1 = 0x5F;
constexpr int kEquipOff[5] = { 0x5B, 0x5A, 0x5D, 0x5E, 0x5F };
constexpr int kEquipIconX[5] = { 0x05, 0x25, 0x44, 0x5F, 0x7F };   // sub_352E2
constexpr int kInvIconX = 0xA7;                                     // inventory icon x base (sub_352E2)
constexpr int kSlotX = 5, kSlotW = 0x1E;                           // X mark / equip slot geometry
constexpr int kListX0 = 0xA9, kListX1 = 0x129, kListY0 = 5, kListY1 = 0x93;
constexpr int kSlotsY0 = 0x67, kSlotsY1 = 0x81;
constexpr int kThumbX0 = 0x12A, kThumbX1 = 0x139;

inline int rd16(const uint8_t* r, int o) { return (int)(int16_t)(r[o] | (r[o + 1] << 8)); }
inline int rd32(const uint8_t* r, int o) {
    return (int)(int32_t)(r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24));
}

// unk_70830 / unk_70A22 live in PACK.EXE data. File-scope cache: there is one Pack instance.
std::vector<uint8_t> g_class_stat3;
std::vector<uint8_t> g_weapon_tab;

void load_equip_tables() {
    if (!g_class_stat3.empty()) return;
    // 0x70830: 166 classes x 3 bytes (0x70830 + 498 = 0x70A22).
    if (!exe_resource_load(ExeId::Pack, 0x70830, ExeResLength::Fixed, g_class_stat3, 166 * 3))
        fprintf(stderr, "PACK: warning: equipment class table (0x70830) load failed\n");
    // 0x70A22: weapon group (0..18) -> item type; off_70A30 icons follow.
    if (!exe_resource_load(ExeId::Pack, 0x70A22, ExeResLength::Fixed, g_weapon_tab, 19))
        fprintf(stderr, "PACK: warning: equipment weapon table (0x70A22) load failed\n");
}

inline int class_stat3(const uint8_t* rec, int field) {
    const int cls = rec[R_CLASS];
    if (cls < 0 || cls >= (int)(g_class_stat3.size() / 3)) return 0;
    return g_class_stat3[(size_t)cls * 3 + field];
}

// sub_24C7F: load a portsmal.syf portrait as RLE. Skip the entry w/h at +4 and 4 bytes at +8,
// then read 0x2710 bytes.
bool load_small_portrait_rle(const std::string& path, int id, std::vector<uint8_t>& out) {
    out.clear();
    if (id < 0) return false;
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    do {
        if (fseek(fp, 0x80 + (long)id * 4, SEEK_SET) != 0) break;
        uint32_t off = 0;
        if (fread(&off, 4, 1, fp) != 1) break;
        if (fseek(fp, (long)off + 12, SEEK_SET) != 0) break;
        out.resize(0x2710);
        const size_t n = fread(out.data(), 1, out.size(), fp);
        out.resize(n);
        ok = n > 4;
    } while (false);
    fclose(fp);
    return ok;
}

// sub_188DF: Bresenham line (sub_2B21D); writes one palette index per pixel.
void equip_line(uint8_t* bb, int x1, int y1, int x2, int y2, uint8_t color) {
    const int dx = std::abs(x2 - x1), dy = std::abs(y2 - y1);
    const int sx = (x2 > x1) ? 1 : (x2 < x1 ? -1 : 0);
    const int sy = (y2 > y1) ? 1 : (y2 < y1 ? -1 : 0);
    const int n = std::max(dx, dy);
    int x = x1, y = y1, ax = 0, ay = 0;
    for (int i = 0; i <= n; i++) {
        if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) bb[y * SCREEN_W + x] = color;
        ax += dx; ay += dy;
        if (ax > n) { x += sx; ax -= n; }
        if (n < ay) { y += sy; ay -= n; }
    }
}

// sub_37F18: copy a width x height rect from (src + src_x) to (dst + dst_x), stride 0x140.
void rect_copy(const uint8_t* src, int src_x, int dst_x, int width, int height, uint8_t* dst) {
    if (width <= 0 || height <= 0) return;
    if (dst_x < 0) { src_x -= dst_x; width += dst_x; dst_x = 0; }
    if (src_x < 0) { dst_x -= src_x; width += src_x; src_x = 0; }
    if (width <= 0) return;
    if (src_x + width > SCREEN_W) width = SCREEN_W - src_x;
    if (dst_x + width > SCREEN_W) width = SCREEN_W - dst_x;
    if (width <= 0) return;
    height = std::min(height, SCREEN_H);
    for (int y = 0; y < height; y++)
        memcpy(dst + (size_t)y * SCREEN_W + dst_x, src + (size_t)y * SCREEN_W + src_x, (size_t)width);
}

}  // namespace

// ==== sub_37DB2: equip an item (eax = rec, ebx = slot, edx = item0) ====
// Slot 1 = weapon (+0x5B), 2 = shield (+0x5A), 3 = armor (+0x5D).
// Stats: +0x40..+0x43 (byte), +0x44 (word), +0x46 (word unless slot 2), +0x59 (byte).
void Pack::status_equip_apply(uint8_t* rec, int item0, int slot) {
    if (!rec || item0 < 0 || item0 >= MAX_ITEMS) return;
    const uint8_t* it = item_data_[item0];
    const int plus1 = (item0 + 1) & 0xFF;
    switch (slot) {
        case 1: rec[R_EQUIP_W] = (uint8_t)plus1; break;
        case 2: rec[R_EQUIP_S] = (uint8_t)plus1; break;
        case 3: rec[R_EQUIP_A] = (uint8_t)plus1; break;
        default: break;
    }
    rec[R_STAT_B0] = (uint8_t)(rec[R_STAT_B0] + it[0x28]);
    rec[R_STAT_B1] = (uint8_t)(rec[R_STAT_B1] + it[0x29]);
    rec[R_STAT_B2] = (uint8_t)(rec[R_STAT_B2] + it[0x2A]);
    rec[R_STAT_B3] = (uint8_t)(rec[R_STAT_B3] + it[0x2B]);
    const int w0 = rd16(rec, R_STAT_W0) + it[0x2C];
    rec[R_STAT_W0] = (uint8_t)w0; rec[R_STAT_W0 + 1] = (uint8_t)(w0 >> 8);
    if (slot != 2) {
        const int w1 = rd16(rec, R_STAT_W1) + it[0x2D];
        rec[R_STAT_W1] = (uint8_t)w1; rec[R_STAT_W1 + 1] = (uint8_t)(w1 >> 8);
    }
    rec[R_STAT_SUM] = (uint8_t)(rec[R_STAT_SUM] + it[0x2F]);
}

// ==== sub_37E5E: unequip an item (eax = rec, edx = slot) ====
// Reads the item number from the record, subtracts the bonuses of item0 = number-1, then
// clears the slot.
void Pack::status_equip_strip(uint8_t* rec, int slot) {
    if (!rec) return;
    int item0 = -1;
    switch (slot) {
        case 1: item0 = (int)rec[R_EQUIP_W] - 1; rec[R_EQUIP_W] = 0; break;
        case 2: item0 = (int)rec[R_EQUIP_S] - 1; rec[R_EQUIP_S] = 0; break;
        case 3: item0 = (int)rec[R_EQUIP_A] - 1; rec[R_EQUIP_A] = 0; break;
        default: return;
    }
    if (item0 < 0) return;                       // The original computes number-1, so 0 would read F10xx[-66] (UB).
    const int idx = item0 < MAX_ITEMS ? item0 : 0;
    const uint8_t* it = item_data_[idx];
    rec[R_STAT_B0] = (uint8_t)(rec[R_STAT_B0] - it[0x28]);
    rec[R_STAT_B1] = (uint8_t)(rec[R_STAT_B1] - it[0x29]);
    rec[R_STAT_B2] = (uint8_t)(rec[R_STAT_B2] - it[0x2A]);
    rec[R_STAT_B3] = (uint8_t)(rec[R_STAT_B3] - it[0x2B]);
    const int w0 = rd16(rec, R_STAT_W0) - it[0x2C];
    rec[R_STAT_W0] = (uint8_t)w0; rec[R_STAT_W0 + 1] = (uint8_t)(w0 >> 8);
    if (slot != 2) {
        const int w1 = rd16(rec, R_STAT_W1) - it[0x2D];
        rec[R_STAT_W1] = (uint8_t)w1; rec[R_STAT_W1 + 1] = (uint8_t)(w1 >> 8);
    }
    rec[R_STAT_SUM] = (uint8_t)(rec[R_STAT_SUM] - it[0x2F]);
}

// ==== sub_334F9: items that cannot be dropped (dword_3120A, 12 entries) ====
bool Pack::status_is_kept_item(int item_plus1) const {
    for (int i = 0; i < 12; i++)
        if ((item_plus1 & 0xFFFF) == kPackUnsellable[i]) return true;
    return false;
}

// ==== sub_367B0: drag and drop an item ====
// src: 1..5 = equip slot (kEquipOff), 0x64+n = inventory entry n.
// top = dword_1A4F38, the first visible list row. It is a different variable from the status
// window's dword_1A4F14.
void Pack::equip_drag(int sel, int item0, int src, int top) {
    uint8_t* bb = video_.get_backbuffer();
    if (!bb || item0 < 0 || item0 >= MAX_ITEMS) return;
    load_equip_tables();
    uint8_t* rec = party_rec(sel);
    if (!rec) return;
    const uint8_t* it = item_data_[item0];
    const int type = it[0];
    int ox = input_.get_mouse_x() - 7, oy = input_.get_mouse_y() - 8;   // var_10/var_C

    // ---- Usable equip slots (1 = allowed), from the off_36704 jump table ----
    uint8_t f[5] = { 1, 1, 1, 1, 1 };
    auto two_hand = [&](int w) {
        return w >= 0 && w < MAX_ITEMS && item_data_[w][0x25] == 2;
    };
    if (type >= 1 && type <= 5) {                 // weapon
        if (rec[R_EQUIP_S] != 0 && it[0x25] == 2) f[0] = 0;
        f[1] = 0; f[2] = 0;
    } else if (type == 6) {                       // shield / off-hand
        if (two_hand((int)rec[R_EQUIP_W] - 1)) f[1] = 0;
        f[0] = 0; f[2] = 0;
    } else if (type == 7) {                       // armor
        f[0] = 0; f[1] = 0;
    } else if (type == 8) {                       // accessory
        if (two_hand((int)rec[R_EQUIP_W] - 1)) f[1] = 0;
        f[0] = 0; f[2] = 0;
    }

    if (it[0x25] == 2) small_text(5, 0x62, "2HAND");   // a2hand

    // Class restriction: unk_70A22[unk_70830[class*3]] must equal the item type.
    const int grp = class_stat3(rec, 0);
    const int allowed = (grp >= 0 && grp < (int)g_weapon_tab.size()) ? g_weapon_tab[grp] : 0;
    if (allowed != type) {
        if (f[1] == 0 && f[2] == 0) small_text(0x26, 0x62, "JOB");
        f[0] = 0;
    }
    const int cap = class_stat3(rec, 2);
    if (cap != 1 && type == 6) {
        if (f[0] == 0 && f[2] == 0) small_text(0x26, 0x62, "JOB");
        f[1] = 0;
    }
    if (cap != 2 && type == 8) {
        if (f[0] == 0 && f[2] == 0) small_text(0x26, 0x62, "JOB");
        f[1] = 0;
    }

    // Draw an X (color 0x16, four strokes) over each forbidden slot.
    for (int k = 0; k < 5; k++) {
        if (f[k] == 1) continue;
        const int L = kSlotX + kSlotW * k, R = 0x1D + kSlotW * k;
        equip_line(bb, L, 0x67, R, 0x81, 0x16);
        equip_line(bb, L - 1, 0x67, R - 1, 0x81, 0x16);
        equip_line(bb, R, 0x67, L, 0x81, 0x16);
        equip_line(bb, R - 1, 0x67, L - 1, 0x81, 0x16);
    }
    video_.present();

    // ---- Drag: draw only the item sprite and keep the backbuffer pristine ----
    const PackSprite* sp = (item0 < (int)item_sprites_.size()) ? &item_sprites_[item0] : nullptr;
    const uint8_t* spr = (sp && !sp->data.empty()) ? sp->data.data() : nullptr;
    std::vector<uint8_t> clean(bb, bb + (size_t)SCREEN_W * SCREEN_H);
    auto show = [&]() {
        memcpy(bb, clean.data(), clean.size());
        if (spr) blit_sprite_rle(ox, oy, spr);
        video_.present();
    };
    show();
    int st = 0, mx = 0, my = 0;
    do {
        if (!poll_mouse(st, mx, my)) return;
        if (mx - 7 != ox || my - 8 != oy) {
            ox = mx - 7; oy = my - 8;
            show();
            GameTimer::delay_with_pump(input_, 16);
        }
    } while (st == 1);
    memcpy(bb, clean.data(), clean.size());

    // ---- Drop target ----
    const int team = cur_slot();
    auto inv = [&](int i) -> uint8_t& { return slot_item(team, i); };
    uint16_t& cnt = g_slot_item_count[team];
    const int li = top + (my - 5) / 0x18;                            // list row under the cursor
    const bool in_list = mx >= kListX0 && mx <= kListX1 && my >= kListY0 && my <= kListY1;
    const bool in_slots = mx >= 5 && mx <= 0x98 && my >= kSlotsY0 && my <= kSlotsY1;
    const int k = in_slots ? (mx - 5) / kSlotW : 0;

    auto clear_src_equip = [&](int sc) {
        if (sc >= 1 && sc <= 3) status_equip_strip(rec, sc);
        else if (sc == 4) rec[R_EQUIP_ACC0] = 0;
        else if (sc == 5) rec[R_EQUIP_ACC1] = 0;
    };

    if (src < 6) {                                    // dragged from an equip slot
        if (in_list) {                                // loc_36DA4: insert into the list row
            if (inv(li) & 0xFF) {
                inv_insert_at(item0, li);             // sub_22E57
                cnt++;
                if (src >= 1 && src <= 5) clear_src_equip(src);
            } else {                                  // loc_36C51: put it in the first free entry
                int i = 0;
                while ((int)cnt > i && (inv(i) & 0xFF)) i++;
                inv(i) = (uint8_t)((item0 + 1) & 0xFF);
                cnt++;
                if (src >= 1 && src <= 5) clear_src_equip(src);
            }
        } else if (in_slots) {                        // equip slot -> equip slot
            const int cl = (item0 + 1) & 0xFF;
            if (k <= 2) {                             // weapon/shield/armor: only from accessory 4 or 5
                if ((src == 4 || src == 5) && rec[kEquipOff[k]] == 0 && f[k] == 1) {
                    status_equip_apply(rec, item0, k + 1);
                    // loc_36E8A/loc_36F3F/loc_36FED: the source slot is rewritten with
                    // cl = item0+1 instead of being cleared, so the accessory keeps its item.
                    rec[kEquipOff[src - 1]] = (uint8_t)cl;
                }
            } else {                                  // accessory slot: only when empty
                if (rec[kEquipOff[k]] == 0) {
                    rec[kEquipOff[k]] = (uint8_t)cl;
                    if (src >= 1 && src <= 5) clear_src_equip(src);
                }
            }
        }
    } else if (src >= 0x64) {                         // dragged from the inventory
        const int si = src - 0x64;
        const int cl = (item0 + 1) & 0xFF;
        if (in_list) {                                // loc_370C9: move into an empty row, otherwise swap
            const int bh = inv(li) & 0xFF;
            if (bh == 0) {
                inv(li) = (uint8_t)cl;
                inv(si) = 0;
                inv_remove(si);                       // sub_3767B
            } else {
                inv(si) = (uint8_t)bh;
                inv(li) = (uint8_t)cl;
            }
        } else if (in_slots) {                        // loc_37154: inventory -> equip slot
            auto take = [&]() { inv(si) = 0; inv_remove(si); };
            if (k <= 2) {
                const int off = kEquipOff[k];
                if (rec[off] == 0 && f[k] == 1) {
                    status_equip_apply(rec, item0, k + 1);
                    take();
                } else if (rec[R_EQUIP_ACC0] == 0 && f[k] == 1) {   // move the equipped item to accessory 4
                    const int old = rec[off];
                    status_equip_strip(rec, k + 1);
                    rec[R_EQUIP_ACC0] = (uint8_t)old;
                    status_equip_apply(rec, item0, k + 1);
                    take();
                } else if (rec[R_EQUIP_ACC1] == 0 && f[k] == 1) {   // move the equipped item to accessory 5
                    const int old = rec[off];
                    status_equip_strip(rec, k + 1);
                    rec[R_EQUIP_ACC1] = (uint8_t)old;
                    status_equip_apply(rec, item0, k + 1);
                    take();
                }
            } else if (k == 3) {
                if (rec[R_EQUIP_ACC0] == 0 && f[3] == 1) { rec[R_EQUIP_ACC0] = (uint8_t)cl; take(); }
            } else if (k == 4) {
                if (rec[R_EQUIP_ACC1] == 0 && f[4] == 1) { rec[R_EQUIP_ACC1] = (uint8_t)cl; take(); }
            }
        }
    }

    // ---- Trash can (0x18..0x33, 0xAC..0xBF): drop the item unless it is protected ----
    if (mx >= 0x18 && mx <= 0x33 && my >= 0xAC && my <= 0xBF &&
        !status_is_kept_item(item0 + 1)) {
        if (src >= 1 && src <= 5) clear_src_equip(src);
        // loc_3763F: the original also touches unk_15FD2C[src-0x64] and sub_3767B with a
        // negative index when the item came from an equip slot (that reads the previous slot).
        // The port stores each slot in its own vector, where a negative index would clamp and
        // clear an unrelated entry, so this runs only for a real inventory source.
        if (src >= 0x64) {
            inv(src - 0x64) = 0;
            inv_remove(src - 0x64);
        }
    }
}

// ==== sub_357C4: equipment screen body ====
// Returns 0 for the close button in the bottom right (close the whole status window) and 1
// for the bottom row (return to the status window).
int Pack::status_equipment_screen(int sel) {
    load_item_data();
    status_load_assets();                     // equip icon table (off_70A30), hand cursor, buttons
    load_equip_tables();
    if (item_sprites_.empty()) load_standard_sprites(game_path("item.spr"), item_sprites_);
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return 0;
    const int team = cur_slot();
    auto count = [&]() { return (int)g_slot_item_count[team]; };
    auto inv = [&](int i) -> uint8_t& { return slot_item(team, i); };

    // The original keeps this screen's scroll state in its own globals (dword_1A4F38 = first
    // row, dword_1A4F34 = thumb position), separate from the status window (dword_1A4F10/1A4F1C),
    // so the port keeps them in locals.
    int sec_top = 0;
    float sec_off = 0.0f;
    float sec_h = status_thumb_height(count());
    wait_right_released();
    if (!status_load_pcx("equment.pcx", false)) {          // sub_2E392 (no palette)
        fprintf(stderr, "PACK: error: cannot read equment.PCX\n");
        do_transition("EXIT");
        should_exit_ = true;
        return 0;
    }

    // ---- sub_352E2: draw the screen ----
    auto draw_contents = [&](int sel_, int top_) {
        memcpy(bb, status_bg_.data(), status_bg_.size());
        const uint8_t* rec = party_rec(sel_);
        if (rec) {
            for (int k = 0; k < 5; k++) {
                const int item = rec[kEquipOff[k]];
                if (item) status_equip_icon(kEquipIconX[k], item);
            }
            char buf[48];
            status_small_text(0x44, 0x21, "HP:", 0x1B);
            status_small_text(0x44, 0x2A, "MP:", 0x21);
            status_small_text(0x41, 0x33, "LV:\tEXP:", 0x11);
            // Original format strings aDD_2/aDD_3: "%d /\t%d" and "%d\t   %d" ('\t' has width 0).
            snprintf(buf, sizeof(buf), "%d /\t%d", rd32(rec, R_HP) / 100, rd32(rec, R_HPMAX) / 100);
            status_small_text(0x5B, 0x21, buf, 1);
            snprintf(buf, sizeof(buf), "%d /\t%d", rd16(rec, R_MP), rd16(rec, R_MPMAX));
            status_small_text(0x5B, 0x2A, buf, 1);
            snprintf(buf, sizeof(buf), "%d\t   %d", (int)rec[R_LEVEL], (int)rec[R_EXPB]);
            status_small_text(0x56, 0x33, buf, 1);

            sh_text(0x3F, 0x0B, (const char*)rec + R_NAME, 0x1000);
            status_small_number(0x1F, 0x43, rd16(rec, R_STAT_W0), 1);
            status_small_number(0x4F, 0x43, rec[R_STAT_B0], 1);
            status_small_number(0x7D, 0x43, rec[R_STAT_B1], 1);
            status_small_number(0x1F, 0x57, rd16(rec, R_STAT_W1), 1);
            status_small_number(0x4F, 0x57, rec[R_STAT_B2], 1);
            status_small_number(0x7D, 0x57, rec[R_STAT_B3], 1);
            status_gold_box_at(0x20, 0x86);

            std::vector<uint8_t> rle;
            std::string ppath = file_find_case_insensitive(GAME_DATA_DIR, "portsmal.syf");
            if (ppath.empty() || !file_exists(ppath.c_str())) ppath = game_path("portsmal.syf");
            if (load_small_portrait_rle(ppath, rec[R_FACE], rle))
                blit_sprite_rle(5, 9, rle.data());
        }

        // Six inventory rows: icon, Korean name, English name.
        for (int row = 0; row < 6; row++) {
            const int idx = top_ + row;
            const int v = (idx >= 0 && idx < kSlotBytes) ? (inv(idx) & 0xFF) : 0;
            const int id0 = v - 1;
            if (id0 < 0 || id0 >= MAX_ITEMS) continue;
            if (id0 < (int)item_sprites_.size() && !item_sprites_[id0].data.empty()) {
                const PackSprite& sp = item_sprites_[id0];
                blit_sprite_rle(kInvIconX + sp.x_offset + 0x0C,
                                0x2B + 0x18 * row - (sp.height + 0x10), sp.data.data());
            }
            const char* name = (const char*)item_data_[id0] + 1;
            const int len = (int)strlen(name);
            sh_text(0xF1 - (len / 2) * 8, 6 + 0x18 * row, name, 0x1000);
            const char* en = (const char*)item_data_[id0] + 0x10;
            const int l2 = (int)strlen(en);
            small_text(0xF1 - 5 * (l2 / 2), 0x16 + 0x18 * row, en);
        }
    };
    auto draw_thumb = [&]() {
        const int h = (int)sec_h;
        const int off = (int)sec_off;
        draw_frame_box(kThumbX0, 0x0F + off, kThumbX1, h + 0x0F + off);
    };
    auto redraw = [&]() {
        draw_contents(sel, sec_top);
        draw_thumb();
        video_.present();
    };
    auto in = [&](int x0, int x1, int y0, int y1, int mx, int my) {
        return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
    };
    auto wait_left_release = [&]() {
        int st, mx, my;
        do { if (!poll_mouse(st, mx, my)) return; } while (st == 1);
    };

    // ---- Slide transition (loc_35905): the right half of the new screen stays fixed while ----
    // the previous screen is pushed out to the left ----
    draw_contents(sel, sec_top);
    draw_thumb();
    memcpy(status_snap_.data(), bb, status_snap_.size());
    // sub_357C4 L46982: menu SFX id 1 with volume byte_82732*10, before the slide loop.
    play_pack_sfx(1);
    {
        int w = 0x140;
        for (int k = 0; k < 0x51 && !should_exit_; k++) {
            // sub_37F18(unk_1722C4, 0xA0, 0x94, backbuffer+0xA0): keep the new right half
            rect_copy(status_snap_.data(), 0xA0, 0xA0, 0xA0, 0x94, bb);
            // sub_37F18(unk_183394 + 2k, w + w%4, 0x94, backbuffer): previous screen slides left
            rect_copy(status_prev_.data(), 2 * k, 0, w + (w & 3), 0x94, bb);
            w -= 2;
            video_.present();
            GameTimer::delay_with_pump(input_, 16);
        }
    }
    if (!status_load_pcx("equment.pcx", false)) { do_transition("EXIT"); should_exit_ = true; return 0; }
    memcpy(bb, status_bg_.data(), status_bg_.size());
    draw_contents(sel, sec_top);
    draw_thumb();
    video_.present();
    wait_left_release();

    for (;;) {
        int st = 0, mx = 0, my = 0;
        if (!poll_mouse(st, mx, my)) return 0;

        // loc_35FF8: left click on the close button (0xFC..0x135, 0xBB..0xC5).
        if (st == 1 && in(0xFC, 0x135, 0xBB, 0xC5, mx, my)) {
            if (!status_btn_b_.empty()) blit_sprite_rle(0xFC, 0xBB, status_btn_b_.data());
            video_.present();
            wait_left_release();
            return 0;
        }

        if (st == 2) {                                    // ---- right click: item description ----
            if (in(0x99, 0x127, 8, 0x97, mx, my)) {
                const int idx = sec_top + (my - 8) / 0x18;
                const int item = (idx >= 0 && idx < kSlotBytes) ? (inv(idx) & 0xFF) : 0;
                if (item) status_item_popup(item);
            } else if (in(5, 0x98, kSlotsY0, kSlotsY1, mx, my)) {
                const int kk = (mx - 5) / kSlotW;
                const uint8_t* rec = party_rec(sel);
                if (kk <= 4 && rec && rec[kEquipOff[kk]] != 0)
                    status_item_popup(rec[kEquipOff[kk]]);
            }
            continue;                                     // the popup restores the screen itself
        }
        if (st != 1) continue;

        // ---- left click ----
        if (in(0x60, 0x99, 0x87, 0x91, mx, my)) {         // (A) skill/magic grid, held down
            const uint8_t* rec = party_rec(sel);
            if (rec) status_draw_skill_grid(rec, 9);
            if (!status_btn_a_.empty()) blit_sprite_rle(0x5F, 0x87, status_btn_a_.data());
            video_.present();
            wait_left_release();
            redraw();
            wait_left_release();
            continue;
        }
        if (mx >= kThumbX0 && mx <= kThumbX1) {           // (B) drag the scroll thumb
            const int yy = 0x0F + (int)sec_off;
            if (yy <= my && !((long double)my > (long double)yy + (long double)sec_h)) {
                int grab = my;
                for (;;) {
                    draw_contents(sel, sec_top);
                    draw_thumb();
                    status_hand_cursor();
                    video_.present();
                    int s2 = 0, x2 = 0, y2 = 0;
                    if (!poll_mouse(s2, x2, y2)) return 0;
                    if (s2 != 1) break;
                    if (grab < y2) {
                        const int d = y2 - grab;
                        sec_off += (float)d;
                        grab += d;
                        const long double fv = (long double)sec_off + (long double)sec_h;
                        if (fv > 123.0L) {
                            grab = (int)((long double)grab - ((long double)(float)fv - 123.0L));
                            sec_off = (float)(123.0L - (long double)sec_h);
                        }
                        sec_top = status_scroll_to_top((int)sec_off, (int)sec_h, count());
                    } else if (grab > y2) {
                        const int d = grab - y2;
                        sec_off -= (float)d;
                        grab -= d;
                        if (sec_off < 0.0f) sec_off = 0.0f;
                        sec_top = status_scroll_to_top((int)sec_off, (int)sec_h, count());
                    }
                }
                redraw();
                wait_left_release();
                continue;
            }
        }
        if (in(kThumbX0, kThumbX1, 5, 0x0F, mx, my)) {    // (C) scroll up
            if (sec_top > 0) sec_top--;
            sec_off = (float)status_top_to_scroll(sec_top, count());
            redraw();
            wait_left_release();
            continue;
        }
        if (in(kThumbX0, kThumbX1, 0x8A, 0x93, mx, my)) { // (D) scroll down
            if (count() - 6 > sec_top) sec_top++;
            sec_off = (float)status_top_to_scroll(sec_top, count());
            redraw();
            wait_left_release();
            continue;
        }
        if (in(kThumbX0, kThumbX1, 0x0F, 0x8A, mx, my)) { // (E) scroll track
            sec_off = (float)(my - 0x0F);
            const long double fv = (long double)sec_off + (long double)sec_h;
            if (fv > 123.0L) sec_off = (float)(123.0L - (long double)sec_h);
            sec_top = status_scroll_to_top((int)sec_off, (int)sec_h, count() - 1);
            redraw();
            wait_left_release();
            continue;
        }
        if (in(0x99, 0x127, 8, 0x97, mx, my)) {           // (F) drag an item from the list
            const int idx = sec_top + (my - 8) / 0x18;
            const int v = (idx >= 0 && idx < kSlotBytes) ? (inv(idx) & 0xFF) : 0;
            if (v != 0) equip_drag(sel, v - 1, 0x64 + idx, sec_top);
            redraw();
            wait_left_release();
            continue;
        }
        if (in(5, 0x98, kSlotsY0, kSlotsY1, mx, my)) {    // (G) drag an item from an equip slot
            const int kk = (mx - 5) / kSlotW;
            uint8_t* rec = party_rec(sel);
            if (kk <= 4 && rec && rec[kEquipOff[kk]] != 0)
                equip_drag(sel, rec[kEquipOff[kk]] - 1, kk + 1, sec_top);
            sec_h = status_thumb_height(count());   // only this path recomputes the thumb height
            redraw();
            wait_left_release();
            continue;
        }
        if (in(0, 0x13F, 0x94, 0xA3, mx, my)) {           // (H) bottom row: back to the status window
            // loc_366B1: sub_2B433 saves the current screen to unk_183394 as the "previous"
            // frame of the slide-back transition, then returns 1.
            memcpy(status_prev_.data(), bb, status_prev_.size());
            return 1;
        }
        redraw();                                          // no hit: redraw
        wait_left_release();
    }
}
