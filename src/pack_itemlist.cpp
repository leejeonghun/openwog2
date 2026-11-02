// pack_itemlist.cpp: PACK.EXE item list / discard screen (sub_34261).
//
//   sub_34261  status_inventory_list   6-row list + scrollbar + discard button
//   sub_34038  (draw_rows below)       six rows plus the "Discard" button and label
//   sub_337A2 / sub_33534              shop sell list with the same layout (see pack_shop.cpp)
//   sub_3205E / sub_320A4              scroll offset <-> top row
//   sub_3767B                          remove one inventory slot
//   sub_3197D / sub_319DB              wait until the buttons are released
//
// Callers:
//   * sub_2F789 (class change) + 0xBB0: opened as a "choose an item to discard" window
//     when sub_376D8 (insert at the inventory front) fails.
//   * sub_12857 + 0x88D (EVE op): opened when the inventory is full.
//   Both callers save the current screen to unk_172224 (sub_2B3DC) first, so this screen
//   draws on top of the image restored by sub_2B374(0,0).
//
// Data:
//   * Items: inventory of the current slot (slot_item(cur_slot(), idx), g_slot_item_count).
//     Deviation: the original uses slot dword_1A00C8+2>>16; the port keeps cur_slot() to
//     stay consistent with its inventory model (same policy as pack_status.cpp).
//   * Icons: item.spr (item_sprites_). sub_18B2A builds table dword_99232 with [+2]+=0x10,
//     [+4]+=0x0C and [+6]=-[+2] per frame, so the port adds +0x0C / -(height+0x10) to the
//     original offsets (sub_34038 loc_34115).
//   * Arrow and hand cursors: PACK.EXE VA 0x314B6 / 0x3155A (164 raw bytes), 0x3140A (172).
#include "pack.h"
#include "pack_data.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "game_timer.h"
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

namespace {
// ADsqbfa (VA 0x5212C): "Discard" button label. CHP.EXE holds the same string at
// 0x51E0C, so this reuses the bytes from chp_class_data.h.
constexpr char kDiscardLabel[] = "\xA4\xE1\x9F\xA1\x94\x61";
constexpr int kThumbTrack = 123;   // flt_52137
}  // namespace

// ==== sub_34261: item list / discard window ====
// No return value. Pressing "Discard" clears the selected slot and closes the window;
// pressing the lower right area (0xDC..0x118, 0x78..0x8A) just closes it.
void Pack::status_inventory_list() {
    load_item_data();
    if (item_sprites_.empty()) load_standard_sprites(game_path("item.spr"), item_sprites_);

    // ---- Sprites (the original copies them to the stack on entry; the port loads once) ----
    static std::vector<uint8_t> arrow_up, arrow_dn, hand;
    if (arrow_up.empty())
        exe_resource_load(ExeId::Pack, 0x314B6, ExeResLength::Fixed, arrow_up, 164);
    if (arrow_dn.empty())
        exe_resource_load(ExeId::Pack, 0x3155A, ExeResLength::Fixed, arrow_dn, 164);
    if (hand.empty())
        exe_resource_load(ExeId::Pack, 0x3140A, ExeResLength::Fixed, hand, 172);

    // The original requires the caller to have saved the screen with sub_2B3DC. If no
    // snapshot exists, grab the current screen so the background does not disappear.
    if (shop_bg_.size() < 4) shop_grab_screen();

    const int s = cur_slot();
    auto count = [&]() { return (int)g_slot_item_count[s]; };

    int sel = 0;                                   // ebp
    status_top_ = 0;                               // dword_1A4F14 = 0 on entry
    int off = 0;                                   // var_8: thumb offset
    int hover = 0, last_hover = 0;                 // var_1C / var_20
    const float thumb_f = status_thumb_height(count());    // var_10
    const float room = (float)kThumbTrack - thumb_f;       // var_18
    const int thumb = (int)thumb_f;

    // sub_34038: the six list rows plus the discard button frame and label.
    auto draw_rows = [&]() {
        for (int i = 0; i < 6; i++) {
            const int y0 = 8 + 0x18 * i;
            const int idx = status_top_ + i;
            const int v = (idx < 0) ? 0 : slot_item(s, idx);
            const int id0 = v - 1;
            if (id0 >= 0) {
                if (idx == sel) draw_frame_box(0x34, y0, 0xC3, y0 + 0x17);
                else {
                    sh_fill(0x34, y0, 0x8F, 0x17, 0);           // sub_2B56C(w=0x8F, h=0x17, color 0)
                    sh_hline(0x34, y0 + 0x17, 0x8F, 6);         // sub_2B4D6(color 6)
                }
                if (id0 < (int)item_sprites_.size() && !item_sprites_[id0].data.empty()) {
                    const PackSprite& sp = item_sprites_[id0];
                    // loc_34115: x = 0x34 + (x_off+0x0C), y = 0x2E+0x18i - (height+0x10)
                    blit_sprite_rle(0x34 + sp.x_offset + 0x0C,
                                    0x2E + 0x18 * i - (sp.height + 0x10),
                                    sp.data.data());
                }
                if (id0 < MAX_ITEMS) {
                    const char* name = (const char*)item_data_[id0] + 1;
                    const int len = (int)strlen(name);
                    sh_text(0x82 - (len / 2) * 8, 9 + 0x18 * i, name, 0x1000);
                    const char* en = (const char*)item_data_[id0] + 0x10;
                    const int l2 = (int)strlen(en);
                    small_text(0x82 - (l2 / 2) * 5, 0x1A + 0x18 * i, en);
                }
            } else {
                sh_fill(0x34, y0, 0x8F, 0x17, 0);
                sh_hline(0x34, y0 + 0x17, 0x8F, 6);
            }
        }
        // End of sub_34038: discard button (0xDC,0x64)-(0x118,0x76), label at (0xE6,0x66).
        draw_frame_box(0xDC, 0x64, 0x118, 0x76);
        sh_text(0xE6, 0x66, kDiscardLabel, 0x1000);
    };

    // loc_343DD/redraw: restore the saved screen (sub_2B374(0,0)), then list, arrows, track, thumb.
    auto draw_screen = [&]() {
        shop_restore_screen();
        draw_rows();
        if (!arrow_up.empty()) sh_raw_blit(0xC4, 8, arrow_up);
        if (!arrow_dn.empty()) sh_raw_blit(0xC4, 0x8E, arrow_dn);
        sh_fill(0xC4, 0x12, 0x0F, 0x7C, 3);                        // sub_2B56C(0xC4, 0x12, color 3, w 0x0F, h 0x7C)
        draw_frame_box(0xC4, 0x12 + off, 0xD3, 0x12 + off + thumb); // sub_1843D
    };

    // Original entry: sub_3197D (left released) -> sub_319DB (right released) -> first draw.
    wait_buttons_released();
    wait_right_released();
    draw_screen();
    video_.present();

    for (;;) {
        int st, mx, my;
        if (!poll_mouse(st, mx, my)) return;

        // ---- Discard button area (loc_343DD) ----
        const int cur_item = slot_item(s, sel);                     // dl = unk_15FD2C[...]
        hover = 0;
        if (cur_item != 0 && mx >= 0xDC && mx <= 0x118 && my >= 0x64 && my <= 0x76) {
            hover = 1;                                              // var_1C = 1
            if (st == 1) {                                          // discard
                slot_item(s, sel) = 0;                              // mov byte ptr unk_15FD2C[eax],0
                inv_remove(sel);                                    // sub_3767B(ebp)
                return;
            }
        } else if (mx >= 0xDC && mx <= 0x118 && my >= 0x78 && my <= 0x8A) {
            hover = 2;                                              // var_1C = 2
            if (st == 1) {
                wait_buttons_released();                            // sub_3197D
                return;
            }
        }

        if (st == 1) {                                              // loc_344D2
            // List row selection.
            if (mx >= 0x34 && mx <= 0xC3 && my >= 8 && my <= 0x97)
                sel = status_top_ + (my - 8) / 0x18;

            bool dragged = false;
            if (mx >= 0xC4 && mx <= 0xD2 && my >= 0x12 + off &&
                !((long double)my > (long double)(0x12 + off) + (long double)thumb_f)) {
                // ---- Thumb drag (loc_34592..loc_3475D) ----
                dragged = true;
                int grab = my;                                      // var_C
                for (;;) {
                    if (!hand.empty()) sh_raw_blit(input_.get_mouse_x(), input_.get_mouse_y(), hand);
                    video_.present();                               // sub_2B44F
                    int st2, x2, y2;
                    if (!poll_mouse(st2, x2, y2)) return;
                    if (st2 != 1) break;                            // loc_345B8
                    if (grab < y2) {                                // down
                        const int d = y2 - grab;
                        off += d;
                        grab += d;
                        if ((float)off + thumb_f > 123.0f) {        // flt_52137
                            grab = (int)((float)grab - ((float)off + thumb_f - 123.0f));
                            off = (int)room;                        // flt_5213B = -123
                        }
                        status_top_ = status_scroll_to_top(off, thumb, count());
                    } else if (grab > y2) {                         // up
                        const int d = grab - y2;
                        off -= d;
                        grab -= d;
                        if (off < 0) off = 0;
                        status_top_ = status_scroll_to_top(off, thumb, count());
                    } else {
                        continue;                                    // back to loc_345B8
                    }
                    draw_screen();
                }
                // loc_34875: restore the mouse range (sub_19C3B) -> loc_34A28.
            }
            if (!dragged && mx >= 0xC4 && mx <= 0xD3) {
                if (my >= 8 && my <= 0x11) {                        // up arrow
                    if (status_top_ > 0) status_top_--;
                    off = status_top_to_scroll(status_top_, count());
                } else if (my >= 0x8E && my <= 0x97) {               // down arrow
                    if (count() - 6 > status_top_) status_top_++;
                    off = status_top_to_scroll(status_top_, count());
                } else if (my >= 0x12 && my <= 0x8D) {               // track click
                    off = my - 0x12;
                    if ((float)off + thumb_f > 123.0f) off = (int)room;
                    // The original passes count-1 only here (loc_349E5 dec ecx).
                    status_top_ = status_scroll_to_top(off, thumb, count() - 1);
                }
            }
            // loc_34A28: redraw, then wait until the buttons are released.
            draw_screen();
            video_.present();
            wait_buttons_released();
        }

        // ---- loc_34AF4: a hover change recolors the button label ----
        if (last_hover != hover) {
            if (hover == 1) sh_text(0xE6, 0x66, kDiscardLabel, 0x0100);       // sub_2C1C9(4,0x100,0xFF)
            if (last_hover == 1) sh_text(0xE6, 0x66, kDiscardLabel, 0x1000);  // sub_2C1C9(4,0x1000,0xFF)
            video_.present();
            last_hover = hover;
        }
        // The original sub_19924 blocks on the mouse driver event. The SDL port sleeps
        // briefly only on frames without a left-button press, avoiding a 100% spin.
        if (st != 1) GameTimer::delay_with_pump(input_, 16);
    }
}
