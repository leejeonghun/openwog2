// combatii_equip.cpp: ally equipment screen (sub_493A3 -> sub_48E90).
//
//   sub_48E90  drag-and-drop equipment swap loop          -> battle_equipment_screen
//   sub_48BC1  draw the screen                            -> battle_equipment_render
//   sub_48863  bottom-right button detail screen          -> battle_equipment_detail
//   sub_1832D  can-equip check (band message on failure)  -> battle_equip_check
//   sub_181A6  write a slot and add its stats             -> battle_equip_apply
//   sub_18262  clear a slot and subtract its stats        -> battle_equip_remove
//   sub_4AC28  draw a SPECIAL.ICO icon                    -> battle_equip_draw_ring_icon
//
// Five slots, left to right: 0 = [5Bh] (weapon, kind 1), 1 = [5Ah] (shield/cloak, kind 0),
//   2 = [5Dh] (accessory, kind 2), 3 = [5Eh], 4 = [5Fh] (backpack, no check).
//   Slot area x 0x94..0x123, y 0x91..0xAB, width 0x1D.
#include "combatii.h"
#include "devutil.h"
#include "combatii_internal.h"
#include "weapon_class_table.h"
#include "combatii_equip_data.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr int kSlotX0 = 0x94, kSlotX1 = 0x123, kSlotY0 = 0x91, kSlotY1 = 0xAB, kSlotW = 0x1D;
// Slot index -> kind used by sub_1832D/181A6/18262 (0 = [5Ah], 1 = [5Bh], 2 = [5Dh], -1 =
// backpack).
constexpr int kSlotKind[5] = {1, 0, 2, -1, -1};

inline bool in_slot_area(int x, int y) {
    return x >= kSlotX0 && x <= kSlotX1 && y >= kSlotY0 && y <= kSlotY1;
}

uint8_t& slot_field(CombatUnitPlacement& u, int idx) {
    switch (idx) {
    case 0: return u.combat_type_5b;
    case 1: return u.combat_type_5a;
    case 2: return u.combat_type_5d;
    case 3: return u.inv_5e;
    default: return u.inv_5f;
    }
}

uint8_t& kind_field(CombatUnitPlacement& u, int kind) {
    return kind == 0 ? u.combat_type_5a : (kind == 1 ? u.combat_type_5b : u.combat_type_5d);
}

// [42h] and [43h] are one word in the original; the port keeps combat_value_42 (word) and
// combat_value_43 (byte), so both are updated together.
inline uint8_t get42(const CombatUnitPlacement& u) { return (uint8_t)(u.combat_value_42 & 0xFF); }
inline void set42_43(CombatUnitPlacement& u, uint8_t b42, uint8_t b43) {
    u.combat_value_43 = b43;
    u.combat_value_42 = (int16_t)(uint16_t)(b42 | (b43 << 8));
}
} // namespace

// Stat add/subtract shared by sub_181A6 / sub_18262, including the final [54h] recompute.
//   ITEM.DAT record (66 bytes): +28h->[40h] +29h->[41h] +2Ah->[42h] +2Bh->[43h]
//   +2Ch (signed)->word[44h] +2Dh (signed)->word[46h] (kind 0 skips) +2Fh->[59h].
//   [54h] = max(0x3C, (int16)([42h] + dword_71B6C[class*8])).
//   item == 0 is the original reading record -1 (the 66 bytes before unk_EDD88, seen as zeros),
//   so the adjustment is 0.
static void equip_adjust(CombatII& self, CombatUnitPlacement& u, int item, int kind, int sign,
                         int (CombatII::*lookup)(int, int) const) {
    auto col = [&](int c) -> int { return item > 0 ? (self.*lookup)(item, c) : 0; };
    u.combat_stat_40 = (uint8_t)(u.combat_stat_40 + sign * col(0x28));
    u.combat_mag41 = (uint8_t)(u.combat_mag41 + sign * col(0x29));
    const uint8_t b42 = (uint8_t)(get42(u) + sign * col(0x2A));
    const uint8_t b43 = (uint8_t)(u.combat_value_43 + sign * col(0x2B));
    set42_43(u, b42, b43);
    u.combat_value_44 = (int16_t)(u.combat_value_44 + sign * (int)(int8_t)col(0x2C));
    if (kind != 0) u.combat_value_46 = (int16_t)(u.combat_value_46 + sign * (int)(int8_t)col(0x2D));
    u.combat_def59 = (uint8_t)(u.combat_def59 + sign * col(0x2F));
    const int v = (int16_t)((int)get42(u) + (int)kEquipTpBase[u.combat_class]);
    u.combat_value_54 = (int16_t)(v >= 0x3C ? v : 0x3C);
}

// sub_181A6(eax = unit, dl = item, bx = kind)
void CombatII::battle_equip_apply(int unit_index, int item, int kind) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (!battle_item_table_tried_) battle_load_item_table();
    auto& u = units_[(size_t)unit_index];
    if (kind >= 0 && kind <= 2) kind_field(u, kind) = (uint8_t)item;
    equip_adjust(*this, u, item, kind, +1, &CombatII::battle_lookup_184a4);
}

// sub_18262(eax = unit, dx = kind): zero the slot's item and subtract its effect.
void CombatII::battle_equip_remove(int unit_index, int kind) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (!battle_item_table_tried_) battle_load_item_table();
    auto& u = units_[(size_t)unit_index];
    if (kind < 0 || kind > 2) return;
    uint8_t& f = kind_field(u, kind);
    const int item = f;
    f = 0;
    equip_adjust(*this, u, item, kind, -1, &CombatII::battle_lookup_184a4);
}

// sub_1832D(eax = unit, dx = item, ebx = slot kind): true if equippable, otherwise show a band
// message (sub_48083) and return false.
bool CombatII::battle_equip_check(int unit_index, int item, int kind) {
    if (item == 0) return true;                                   // test dx,dx; jz -> 1
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return true;
    if (!battle_item_table_tried_) battle_load_item_table();
    const auto& u = units_[(size_t)unit_index];
    auto fail = [&](const char* msg) {
        battle_text_band(std::string(msg), false);               // straight onto the back buffer (equipment screen)
        return false;
    };
    const int cat = (int8_t)battle_lookup_184a4(item, 0);         // [item+0] category
    const int required = (cat <= 5) ? 1 : ((cat == 6 || cat == 8) ? 0 : 2);
    if (kind != required) return fail(kEquipMsgNoSlot);
    if (required == 1) {                                          // weapon slot
        const int wc = weapon_class(u);                           // sub_174CF
        const int need = (wc >= 0 && wc < 16) ? (int)kEquipWeaponReq[wc] : 0;   // unk_74124[wc]
        if (need == -1) return fail(kEquipMsgNoWeapon);
        if ((uint8_t)need != (uint8_t)cat) return fail(kEquipMsgWrongClass);
        if (battle_lookup_184a4(item, 0x25) == 2 && u.combat_type_5a != 0) return fail(kEquipMsgTwoHand);
        return true;
    }
    if (required == 0) {                                          // shield/cloak slot
        const int w = u.combat_type_5b;
        // The original reads byte [5Bh]*66, not ([5Bh]-1)*66 (no dec before the multiply), i.e.
        // one record past the equipped weapon. Ported as-is.
        if (w != 0 && battle_lookup_184a4(w + 1, 0x25) == 2) return fail(kEquipMsgHoldTwoHand);
        const int cls = u.combat_class;
        const int cls_flag = (cls < kWeaponClassCount) ? (int)kWeaponClassTable[cls][2] : 0;  // (off_72794+1)[class*3]
        if (cat == 6) return cls_flag == 1 ? true : fail(kEquipMsgNoShield);
        if (cat == 8) return cls_flag == 2 ? true : fail(kEquipMsgNoCloak);
        return true;
    }
    // Accessory slot: [item+25h] must be 0
    if (battle_lookup_184a4(item, 0x25) != 0) return fail(kEquipMsgNotAccessory);
    return true;
}

// sub_4AC28(table = unk_FDA12 (SPECIAL.ICO), edx = x, ebx = y, cx = logical idx): draw record
// idx*4 (frame 0) at (x+xo, y+yo+1).
//   Nothing is drawn when off screen (x+xo+w < 0, x+xo > 0x12F, y+yo+h < 0, y+yo > 0xC3).
void CombatII::battle_equip_draw_ring_icon(int idx, int x, int y) {
    if (!ring_load_icons()) return;
    ring_draw_icon_4ac28(ring_special_, idx, x, y);
}

// sub_4AC28(eax = icon set, edx = x, ebx = y, ecx = logical idx): descriptor (+5 width, +9 height,
// +0Dh/+11h offsets, +15h pixels) is drawn with sub_5F3FD(x+xo, y+yo+1) only when on screen
// (x <= 0x12F, y <= 0xC3, not negative). One logical idx spans 4 SPR frames (hover frames).
void CombatII::ring_draw_icon_4ac28(const SPRParser& set, int idx, int x, int y) {
    const SpriteEntry* sp = set.get(idx * 4);
    if (!sp || !sp->pixel_data) return;
    const int dx = x + (int)(int8_t)sp->x_offset;
    if (dx + (int)(sp->word0 & 0xFF) < 0 || dx > 0x12F) return;
    const int dy = y + (int)(int8_t)sp->y_offset;
    if (dy + (int)(sp->word2 & 0xFF) < 0 || dy > 0xC3) return;
    blit_sprite_rle(dx, dy + 1, sp->pixel_data);
}

// sub_48BC1(eax = unit)
//   454B3 (map + units + weather; no dim/HUD/ring) -> 48734 panel -> MENUSTAT #37 (0xDC,0x4A)
//   -> #39 (0x100,0xB8) -> 8 stats ("%3d", dialogue font 0x1800) ->
//   #38 (0xDC,0x9E) -> ITEM.SPR icons for the five slots (x 0xA1/0xBE/0xDB/0xF8/0x115, y 0xB6)
void CombatII::battle_equipment_render(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    if (!menustat_tried_) load_menustat();
    if (!item_spr_tried_) load_item_spr();

    // sub_48BC1 calls only sub_454B3 (map + units + weather); dim/HUD/all-unit HP/ring (454D5,
    // 45431,
    // 4B767) are not drawn here, which is why the original equipment screen shows no ring icons.
    // 10AEB does not clear the back buffer, so the border band keeps the previous ring frame's HUD
    // (same as the original).
    battle_fx_world_keep();                       // sub_454B3
    battle_draw_unit_info_panel(unit_index);      // sub_48734

    auto menustat = [&](int n, int x, int y) {
        if (!menustat_loaded_) return;
        const SpriteEntry* sp = menustat_.get(n);
        if (sp && sp->pixel_data) blit_sprite_rle(x + sp->x_offset, y + sp->y_offset, sp->pixel_data);
    };
    menustat(37, 0xDC, 0x4A);
    menustat(39, 0x100, 0xB8);                    // `inc ah` = +0x100

    uint8_t* bb = video_.get_backbuffer();
    if (bb) {
        font_.set_text_mode(4, 0x1800, 0xFF);     // sub_5DB66(4, 0x1800, 0xFF)
        auto num = [&](int x, int y, int v) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%3d", v);
            font_.draw_string(bb, SCREEN_W, SCREEN_H, x, y, buf);
        };
        num(0xB1, 0x2D, u.combat_stat_40);
        num(0x109, 0x2D, get42(u));
        num(0xB1, 0x46, u.combat_mag41);
        num(0x109, 0x46, u.combat_value_43);
        num(0xB1, 0x63, (int)u.combat_value_44);  // [42h] dword >> 16 = word [44h]
        num(0x109, 0x63, (int)u.combat_value_46); // [44h] dword >> 16 = word [46h]
        num(0xB1, 0x7C, u.combat_def59);
        num(0x109, 0x7C, (int)u.combat_value_54); // [52h] dword >> 16 = word [54h]
        font_.set_text_mode(4, 0x1100, 0xFF);     // sub_5DB66(4, 0x1100, 0xFF)
    }
    menustat(38, 0xDC, 0x9E);

    if (item_spr_loaded_) {
        const int items[5] = {u.combat_type_5b, u.combat_type_5a, u.combat_type_5d, u.inv_5e, u.inv_5f};
        static const int kBaseX[5] = {0xA1, 0xBE, 0xDB, 0xF8, 0x115};
        for (int i = 0; i < 5; ++i) {
            if (items[i] == 0) continue;
            const SpriteEntry* sp = item_spr_.get(items[i] - 1);
            if (sp && sp->pixel_data) blit_sprite_480(kBaseX[i] + sp->x_offset, 0xB6 + sp->y_offset, sp->pixel_data);
        }
    }
}

// sub_48863(eax = unit, ebx = 1): detail screen. 454B3 -> MENUSTAT #40 (0xA0,0x64) -> [3Fh] ->
// five rows of weapon-type icons -> [3Eh] -> seven magic groups (MENUSTAT 0x29+...) ->
// 5E9E8 -> wait in 499EF(0BB8h).
void CombatII::battle_equipment_detail(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    const auto& u = units_[(size_t)unit_index];
    if (!small_font_tried_) load_small_font();
    if (!menustat_tried_) load_menustat();

    battle_fx_world_keep();                       // sub_454B3 (no dim/HUD/ring; stale border band)
    if (menustat_loaded_) {
        const SpriteEntry* sp = menustat_.get(40);
        if (sp && sp->pixel_data) blit_sprite_rle(0xA0 + sp->x_offset, 0x64 + sp->y_offset, sp->pixel_data);
    }
    char buf[16];
    // sub_48863: `mov al,[esi+3Fh]` reads the growth counter. code_a is filled only on the SCE load
    // path and is a duplicate that stays 0 after 4E81F / quick load, so read the real [3Fh],
    // growth_3f.
    snprintf(buf, sizeof(buf), "%3d", (int)u.growth_3f);              // [3Fh]
    draw_small_text(0xE9, 0x0B, buf, 7, 1);
    draw_small_text(0xE8, 0x0A, buf, 0x18, 1);

    // Weapon-type count icons: rows y 0x21/0x39/0x51/0x69/0x81, x from 0x6D step 0x15,
    // first icon index 1/8/0xC/0x10/0x14.
    const int counts[5] = {u.weap_cnt_38, u.weap_cnt_39, u.weap_cnt_3a, u.weap_cnt_3b, u.weap_cnt_3c};
    static const int kStartIdx[5] = {1, 8, 0xC, 0x10, 0x14};
    for (int row = 0; row < 5; ++row) {
        int x = 0x6D;
        for (int i = 0; i < counts[row]; ++i, x += 0x15)
            battle_equip_draw_ring_icon(kStartIdx[row] + i, x, 0x21 + 0x18 * row);
    }

    snprintf(buf, sizeof(buf), "%3d", (int)u.combat_stat_3e);         // [3Eh]
    draw_small_text(0xE1, 0x94, buf, 7, 1);
    draw_small_text(0xE0, 0x93, buf, 0x18, 1);

    // Seven magic groups: when charge byte [2Eh+g] != 0, draw MENUSTAT #((charge-1)*7 + 0x29 + g)
    // at (0x4E + 0x1A*g, 0xAA); if sub_28429(g)+1 (learned + charge, max 3) is >= 1, draw "%d"
    // at (x-3, 0xBA) with small-font colour 0x18.
    for (int g = 0; g < 7; ++g) {
        const int ex = 0x4E + 0x1A * g;
        const int ch = u.spell_charge_2e[g];
        if (ch > 0 && menustat_loaded_) {
            const SpriteEntry* sp = menustat_.get((ch - 1) * 7 + 0x29 + g);
            if (sp && sp->pixel_data) blit_sprite_rle(ex + sp->x_offset, 0xAA + sp->y_offset, sp->pixel_data);
        }
        const int n = battle_spell_kind(u, g) + 1;
        if (n > 0) {
            snprintf(buf, sizeof(buf), "%d", n);
            draw_small_text(ex - 3, 0xBA, buf, 0x18, 1);
        }
    }
    video_.present();                             // sub_5E9E8
    dump_frame_if_requested();
    wait_info_dismiss(0xBB8);                     // sub_499EF(0BB8h)
}

// sub_48E90(eax = unit)
void CombatII::battle_equipment_screen(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (!menustat_tried_) load_menustat();
    if (!item_spr_tried_) load_item_spr();
    if (!battle_item_table_tried_) battle_load_item_table();
    if (!small_font_tried_) load_small_font();
    const int saved_hover = battle_free_hover_;
    battle_free_hover_ = -1;                      // the 454B3 frame has no hover box

    auto& U = units_[(size_t)unit_index];
    uint8_t slot[5] = {U.combat_type_5b, U.combat_type_5a, U.combat_type_5d, U.inv_5e, U.inv_5f};   // var_20..var_1C
    bool redraw = true;                           // var_10

    // Clear or fill one slot (sub_18262/sub_181A6 for checked slots, the backpack byte directly).
    auto unequip = [&](int idx) {
        if (kSlotKind[idx] >= 0) battle_equip_remove(unit_index, kSlotKind[idx]);
        else slot_field(U, idx) = 0;
    };
    auto equip = [&](int idx, int item) {
        if (kSlotKind[idx] >= 0) battle_equip_apply(unit_index, item, kSlotKind[idx]);
        else slot_field(U, idx) = (uint8_t)item;
    };
    auto fill_rect = [&](int x1, int y1, int x2, int y2, uint8_t color) {   // sub_5EB4A(colour) + sub_5EDDE, mode 0 = solid
        uint8_t* bb = video_.get_backbuffer();
        if (!bb) return;
        for (int y = y1; y < y2; ++y) {
            if (y < 0 || y >= SCREEN_H) continue;
            for (int x = x1; x < x2; ++x)
                if (x >= 0 && x < SCREEN_W) bb[(size_t)y * SCREEN_W + (size_t)x] = color;
        }
    };

    // Press and release on the same slot: item name tag (English name = [item+10h], small font).
    auto show_tooltip = [&](int src) {
        battle_equipment_render(unit_index);                          // sub_48BC1
        const int item = slot[src];
        std::string name;
        if (item > 0) {
            const size_t off = (size_t)(item - 1) * 66u + 0x10u;
            for (size_t i = off; i < battle_lookup_table_.size() && battle_lookup_table_[i]; ++i)
                name.push_back((char)battle_lookup_table_[i]);
        }
        const int w = (int)name.size() * 6 + 4;                       // imul edx,ecx,6 ; add edx,4
        const int x0 = src * kSlotW + 0xB4 - w;
        const int x1 = x0 + w;
        fill_rect(x0, 0xAA, x1 - 1, 0xB2, 3);                         // colour 3
        fill_rect(x0, 0xAB, x1, 0xB3, 5);                             // colour 5 (shadow)
        fill_rect(x0 + 1, 0xAB, x1 - 1, 0xB2, 2);                     // colour 2 (fill)
        draw_small_text(x0 + 2, 0xAC, name.c_str(), 5, 1);            // sub_60944(x0+2, 0xAC, 1, 5, "%s", name)
    };

    // Drop handling (loc_48F82 onward): src slot -> dest slot
    auto drop = [&](int src, int dest) {
        const int item = slot[src];
        // (1) Does the dragged item fit the destination slot? Slots 0/1/2 are checked; 3/4 pass.
        bool ok = true;
        if (kSlotKind[dest] >= 0) ok = battle_equip_check(unit_index, item, kSlotKind[dest]);
        // (2) Does the destination item fit the source slot?
        if (ok && kSlotKind[src] >= 0) ok = battle_equip_check(unit_index, slot[dest], kSlotKind[src]);
        if (dest == src) {                                            // same slot: name tag
            if (slot[src] != 0) show_tooltip(src);
            return;
        }
        if (!ok) return;
        // loc_491EA: swap - remove both effects (source then destination), then put each item in
        // the other slot.
        unequip(src);
        if (slot[dest] != 0) unequip(dest);
        equip(src, slot[dest]);
        equip(dest, item);
        slot[src] = slot[dest];
        slot[dest] = (uint8_t)item;
        redraw = true;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: equip swap unit=%d slot %d<->%d item=%d (sub_48E90)\n", U.id, src, dest, item);
    };

    bool done = false;
    while (!done) {
        if (redraw) battle_equipment_render(unit_index);              // cmp var_10,1 -> sub_48BC1
        redraw = false;
        video_.present();                                             // sub_5D41E + sub_5E9E8
        dump_frame_if_requested();

        // Wait for input: keys (getch_) take priority over mouse events.
        bool got = false;
        while (!got && !done) {
            input_.update();
            if (input_.quit_requested()) { should_exit_ = true; done = true; break; }
            const MouseState& m = input_.get_mouse();
            if (input_.any_getch_key_pressed()) {
                got = true;
                const bool esc_key = input_.is_key_pressed(SDLK_ESCAPE);              // cmp di,1Bh
                input_.consume_event();        // getch_ equivalent: the key wins over mouse events and leaves the queue
                if (esc_key) done = true;
            } else if (m.right_clicked) {
                got = true; done = true;                                              // event 3
                input_.consume_event();        // sub_5D22E pop: keep this right click from leaking into a ring cancel
            } else if (m.left_clicked) {
                got = true;                                                           // event 2
                const int px = m.x, py = m.y;
                input_.consume_event();        // sub_5D22E pop (the drag reads the left_button level)
                if (in_slot_area(px, py)) {
                    const int src = (px - kSlotX0) / kSlotW;
                    if (src < 0 || src > 4 || slot[src] == 0) continue;
                    // ---- drag: the item icon follows the cursor (mouse clamped to x 0x1C..0x124,
                    // y 0x10..0xB8) ----
                    uint8_t* bb = video_.get_backbuffer();
                    if (!bb) continue;
                    std::vector<uint8_t> snap(bb, bb + (size_t)SCREEN_W * SCREEN_H);
                    const SpriteEntry* sp = item_spr_loaded_ ? item_spr_.get(slot[src] - 1) : nullptr;
                    int mx = std::clamp(px, 0x1C, 0x124), my = std::clamp(py, 0x10, 0xB8);
                    while (true) {
                        std::memcpy(bb, snap.data(), snap.size());            // sub_5D570: restore the background under the cursor
                        if (sp && sp->pixel_data) {                           // sub_5D41E: draw at (x - w/2, y - h/2)
                            const int hx = (int)(int8_t)((sp->word0 >> 1) & 0xFF);
                            const int hy = (int)(int8_t)((sp->word2 >> 1) & 0xFF);
                            blit_sprite_480(mx - hx, my - hy, sp->pixel_data);
                        }
                        video_.present();
                        input_.update();
                        if (input_.quit_requested()) { should_exit_ = true; done = true; break; }
                        const MouseState& dm = input_.get_mouse();
                        mx = std::clamp(dm.x, 0x1C, 0x124);
                        my = std::clamp(dm.y, 0x10, 0xB8);
                        if (!dm.left_button) break;                           // event 8: left release
                        SDL_Delay(1);
                    }
                    std::memcpy(bb, snap.data(), snap.size());
                    if (done) break;
                    if (in_slot_area(mx, my)) drop(src, (mx - kSlotX0) / kSlotW);
                } else if (px >= 0xD8 && px <= 0x128 && py >= 0xAF && py <= 0xC0) {
                    battle_equipment_detail(unit_index);                       // sub_48863
                    redraw = true;
                }
            }
            if (!got) SDL_Delay(1);
        }
    }
    battle_free_hover_ = saved_hover;
    if (battle_trace_) fprintf(stderr, "COMBAT: equipment screen closed unit=%d\n", U.id);
}
