// pack_shop.cpp - PACK.EXE shop, inn and item list UI.
//
//   sub_1B344 building-layer buy confirm   -> Pack::handle_building_buy
//   sub_1B75B sell confirm                 -> Pack::sell_item
//   sub_337A2 / sub_33534 item list        -> Pack::run_item_list / Pack::draw_item_list
//   sub_1ABA6 inn                          -> Pack::handle_inn
//   sub_25F21 "shop owner" branch          -> Pack::handle_shop_owner
//   sub_22E57 100-slot insert / sub_35109 gold change / sub_3507E gold box / sub_37A56 stat panel
//   sub_1843D frame / sub_18274, sub_1822D small font
//   sub_2B4D6, sub_2B4A8, sub_2B56C line and fill primitives
//
// Strings and tables come from src/pack_data.h; sprites are read from PACK.EXE VAs at
// runtime.
#include "pack.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "audio.h"
#include "palette.h"
#include "game_timer.h"
#include "pack_data.h"
#include "talktbl.h"
#include "pcx.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <utility>

namespace {
constexpr int kBg = 0x1000;   // sub_2C1C9(4, 0x1000, 0xFF): normal text color
constexpr int kHi = 0x0100;   // sub_2C1C9(4, 0x0100, 0xFF): highlight (hover) text color
}  // namespace

// ==== Assets / screen snapshots ====

void Pack::load_shop_assets() {
    if (shop_assets_loaded_) return;
    const bool ok_gold = exe_resource_load(ExeId::Pack, 0x315FE, ExeResLength::Rle, gold_box_data_);
    const bool ok_up   = exe_resource_load(ExeId::Pack, 0x312C2, ExeResLength::Sprite, scroll_up_data_);
    const bool ok_down = exe_resource_load(ExeId::Pack, 0x31366, ExeResLength::Sprite, scroll_down_data_);

    // Ten option-screen sprites (sub_34B92) plus three confirm-box sprites (sub_2D4A3).
    // Order: speed normal/fast, music off/on, sfx off/on, exit/quit pressed,
    //        exit/quit buttons, confirm frame, yes, no.
    static constexpr uint32_t kOptionVa[13] = {
        0x78788, 0x78D64, 0x79340, 0x7991C, 0x79EF8, 0x7A4D4,
        0x7AAB0, 0x7B08C, 0x7B668, 0x7BC44, 0x5ECC8, 0x60199, 0x605E9,
    };
    option_sprites_.clear();
    bool ok_opt = true;
    for (uint32_t va : kOptionVa) {
        std::vector<uint8_t> spr;
        if (!exe_resource_load(ExeId::Pack, va, ExeResLength::Rle, spr)) {
            ok_opt = false;
            break;
        }
        option_sprites_.push_back(std::move(spr));
    }
    if (!ok_opt || option_sprites_.size() != 13) {
        fprintf(stderr, "PACK: warning: PACK.EXE option sprite load failed\n");
        option_sprites_.clear();
    }
    if (!ok_gold || !ok_up || !ok_down)
        fprintf(stderr, "PACK: warning: PACK.EXE shop sprites (gold_box/scroll_up/scroll_down) load failed\n");
    shop_assets_loaded_ = true;
}

// sub_2B3DC(0, 0, unk_172224, 0x13F, 0xC7): copy a 0x13F x 0xC7 block with a size header
void Pack::shop_grab_screen() {
    uint8_t* bb = video_.get_backbuffer();
    const int w = 0x13F, h = 0xC7;
    shop_bg_.assign(4 + (size_t)w * h, 0);
    shop_bg_[0] = (uint8_t)w; shop_bg_[1] = (uint8_t)(w >> 8);
    shop_bg_[2] = (uint8_t)h; shop_bg_[3] = (uint8_t)(h >> 8);
    for (int y = 0; y < h; y++) memcpy(&shop_bg_[4 + (size_t)y * w], bb + y * SCREEN_W, (size_t)w);
}

// sub_2B374(0,0): opaque copy
void Pack::shop_restore_screen() {
    if (shop_bg_.size() < 4) return;
    uint8_t* bb = video_.get_backbuffer();
    const int w = shop_bg_[0] | (shop_bg_[1] << 8), h = shop_bg_[2] | (shop_bg_[3] << 8);
    for (int y = 0; y < h && y < SCREEN_H; y++)
        memcpy(bb + y * SCREEN_W, &shop_bg_[4 + (size_t)y * w], (size_t)std::min(w, SCREEN_W));
}

void Pack::shop_present() { video_.present(); }

// ==== Drawing primitives ====

// sub_2B4D6: horizontal line at (x,y), length len
void Pack::sh_hline(int x, int y, int len, uint8_t c) {
    if (y < 0 || y >= SCREEN_H) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > SCREEN_W) len = SCREEN_W - x;
    if (len < 1) return;
    memset(video_.get_backbuffer() + y * SCREEN_W + x, c, (size_t)len);
}

// sub_2B4A8: vertical line at (x,y), length len
void Pack::sh_vline(int x, int y, int len, uint8_t c) {
    if (x < 0 || x >= SCREEN_W) return;
    uint8_t* bb = video_.get_backbuffer();
    for (int i = 0; i < len; i++) {
        const int yy = y + i;
        if (yy >= 0 && yy < SCREEN_H) bb[yy * SCREEN_W + x] = c;
    }
}

// sub_2B56C: filled rectangle (x,y) w x h
void Pack::sh_fill(int x, int y, int w, int h, uint8_t c) {
    for (int i = 0; i < h; i++) sh_hline(x, y + i, w, c);
}

// sub_2B2A1: {u16 w, u16 h, pixels} bitmap; 0xFF is transparent
void Pack::sh_raw_blit(int x, int y, const std::vector<uint8_t>& bmp) {
    if (bmp.size() < 4) return;
    const int w = bmp[0] | (bmp[1] << 8), h = bmp[2] | (bmp[3] << 8);
    if (bmp.size() < 4 + (size_t)w * h) return;
    uint8_t* bb = video_.get_backbuffer();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const uint8_t p = bmp[4 + (size_t)j * w + i];
            const int xx = x + i, yy = y + j;
            if (p != 0xFF && xx >= 0 && xx < SCREEN_W && yy >= 0 && yy < SCREEN_H) bb[yy * SCREEN_W + xx] = p;
        }
}

// sub_1843D(x0,y0,x1,y1): top/left edge byte_53D09, bottom/right edge byte_53D0A, fill byte_53D0B
void Pack::draw_frame_box(int x0, int y0, int x1, int y1, int fill) {
    const int w = x1 - x0, h = y1 - y0;
    const uint8_t fill_c = fill < 0 ? kPackFrameColors[2] : (uint8_t)fill;
    sh_hline(x0, y0, w, kPackFrameColors[0]);
    sh_vline(x0, y0, h, kPackFrameColors[0]);
    sh_hline(x0, y1, w, kPackFrameColors[1]);
    sh_vline(x1, y0, h + 1, kPackFrameColors[1]);
    sh_fill(x0 + 1, y0 + 1, w - 1, h - 1, fill_c);
}

// sub_18274: small 8x8 font string drawn at y-1, color byte_529FD = 1, no background (byte_826D6 =
// 0)
void Pack::small_text(int x, int y, const char* s) {
    uint8_t* bb = video_.get_backbuffer();
    y -= 1;
    for (; *s; s++) {
        const int idx = (int)(uint8_t)*s - 0x20;
        if (idx < 0 || idx >= 0x60) continue;
        const int gw = kPackFont8Width[idx];
        for (int row = 0; row < 7; row++) {
            const uint8_t bits = kPackFont8Rows[idx * 8 + row];
            for (int col = 0; col < gw; col++) {
                if (!(bits & (0x80 >> col))) continue;
                const int xx = x + col, yy = y + row;
                if (xx >= 0 && xx < SCREEN_W && yy >= 0 && yy < SCREEN_H) bb[yy * SCREEN_W + xx] = 1;
            }
        }
        x += gw;
    }
}

// sub_1822D: draw a signed integer with the small font
void Pack::small_number(int x, int y, int v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    small_text(x, y, buf);
}

// sub_2C1C9(4, color, 0xFF) + sub_2C6AB(x, y, str)
void Pack::sh_text(int x, int y, const char* s, uint16_t color) {
    font_.set_text_mode(4, color, 0xFF);
    font_.draw_string(video_.get_backbuffer(), SCREEN_W, SCREEN_H, x, y, s);
}

// sub_3507E(4,4): gold box (off_315FE RLE) plus team gold, right-aligned at x = 4 + 0x23 -
// digits*5, y = 4 + 5
void Pack::draw_gold_box() {
    load_shop_assets();
    if (!gold_box_data_.empty()) blit_sprite_rle(4, 4, gold_box_data_.data());
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)team_gold_);
    const int len = (int)strlen(buf);
    small_number(4 + 0x23 - len * 5, 4 + 5, (int)team_gold_);
}

// sub_35109(delta): team gold += delta, play sell.VOC (sfx 2, sub_394D1 L46466 with
// volume byte_82732*10), redraw the gold box
void Pack::change_gold(int delta) {
    wait_buttons_released();
    team_gold_ = (uint32_t)((int32_t)team_gold_ + (int16_t)delta);
    play_pack_sfx(2);
    draw_gold_box();
    video_.present();
    GameTimer::delay_with_pump(input_, 714);   // sub_1189F: 50 retrace
}

// ==== Inventory (sub_22E57) ====

// Insert with a 100-slot limit. Returns 1 on success, -1 on failure (message, ~2.1 s wait).
int Pack::inv_insert_at(int item0, int pos) {
    const int s = cur_slot();
    int b = pos;
    while (b < 100 && slot_item(s, b) != 0) b++;
    if (b != 100) {
        // loc_22F16: shift [pos..0x62] one slot back
        for (int i = 0x63; i > pos; ) { i--; slot_item(s, i + 1) = slot_item(s, i); }
        slot_item(s, pos) = (uint8_t)(item0 + 1);
        return 1;
    }
    b = 0;
    while (b < pos && slot_item(s, b) != 0) b++;
    if (b == pos) {
        show_message_box(kPackMsgInvFull100);
        GameTimer::delay_with_pump(input_, 2142);   // sub_1189F x3
        return -1;
    }
    while (b < pos) { b++; slot_item(s, b - 1) = slot_item(s, b); }
    slot_item(s, pos) = (uint8_t)(item0 + 1);
    return 1;
}

// ==== Stat panel (sub_37A56) ====

void Pack::draw_item_stat_panel(int item1) {
    load_item_data();
    const int id0 = item1 - 1;
    int v[7] = {0, 0, 0, 0, 0, 0, 0};
    if (id0 >= 0 && id0 < MAX_ITEMS)
        for (int i = 0; i < 7; i++) v[i] = (int8_t)item_data_[id0][0x28 + i];

    draw_frame_box(0x46, 0x55, 0x5A, 0x62); small_text(0x48, 0x5A, kPackLblStr);
    draw_frame_box(0x78, 0x55, 0x8C, 0x62); small_text(0x7C, 0x5A, kPackLblInt);
    draw_frame_box(0xAA, 0x55, 0xBE, 0x62); small_text(0xAC, 0x5A, kPackLblDex);
    draw_frame_box(0xDC, 0x55, 0xF0, 0x62); small_text(0xDE, 0x5A, kPackLblChr);
    draw_frame_box(0xAA, 0x69, 0xBE, 0x76); small_text(0xAE, 0x6E, kPackLblMp);
    // dword_99256 / dword_99382 = item sprite frames 9 / 84 (no offset applied)
    if (item_sprites_.size() > 9 && !item_sprites_[9].data.empty())
        blit_sprite_rle(0x46, 0x69, item_sprites_[9].data.data());
    if (item_sprites_.size() > 84 && !item_sprites_[84].data.empty())
        blit_sprite_rle(0x78, 0x69, item_sprites_[84].data.data());
    small_number(0x5F, 0x5A, v[0]); small_number(0x91, 0x5A, v[1]);
    small_number(0xC3, 0x5A, v[2]); small_number(0xF5, 0x5A, v[3]);
    small_number(0x5F, 0x6E, v[4]); small_number(0x91, 0x6E, v[5]);
    small_number(0xC3, 0x6E, v[6]);
}

// ==== Input ====

// sub_19924: button state (1=left, 2=right, 3=both) and position; false when quit was requested.
bool Pack::poll_mouse(int& status, int& x, int& y) {
    input_.update();
    if (input_.quit_requested()) { do_transition("EXIT"); return false; }
    status = (input_.is_left_button() ? 1 : 0) | (input_.is_right_button() ? 2 : 0);
    x = input_.get_mouse_x();
    y = input_.get_mouse_y();
    return true;
}

// sub_319DB: wait until the right action input is released
// The original loops while dword_7CF20 == 2 (right-only) or dword_1A01D4 != 0 (Left Alt
// level, the right-click substitute), so both-pressed counts as released.
void Pack::wait_right_released() {
    while (true) {
        input_.update();
        if (input_.quit_requested()) return;
        const bool right_only = input_.is_right_button() && !input_.is_left_button();
        if (!right_only && !input_.is_key_held(SDLK_LALT)) return;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// Yes/no loop shared by sub_1B344 loc_1B4FF, sub_1B75B and sub_1ABA6.
// Yes button x 0xA1..0xD2 (open interval), no button x 0xD8..0x10B, y 0x86..0x97.
// cx state: 1 = over yes, 0 = over no, 0x3E7 = neither. Returns 1 yes / 0 no / -1 quit.
int Pack::run_yes_no(const std::vector<uint8_t>& base) {
    int cx = 0x3E7;
    auto redraw = [&](int state) {
        memcpy(video_.get_backbuffer(), base.data(), base.size());
        sh_text(0xB2, 0x88, kPackStrYes, state == 1 ? kHi : kBg);
        sh_text(0xDA, 0x88, kPackStrNo, state == 0 ? kHi : kBg);
        video_.present();
    };
    while (true) {
        int st, mx, my;
        if (!poll_mouse(st, mx, my)) return -1;
        const bool in_y = my > 0x86 && my < 0x97;
        if (mx > 0xA1 && mx < 0xD2 && in_y) {
            if (cx == 0 || cx == 0x3E7) { redraw(1); cx = 1; }
            if (st == 1) return cx;
        } else if (mx > 0xD8 && mx < 0x10B && in_y) {
            if (cx == 1 || cx == 0x3E7) { redraw(0); cx = 0; }
            if (st == 1) return cx;
        } else if (cx == 1 || cx == 0) {
            redraw(2);
            cx = 0x3E7;
        }
        GameTimer::delay_with_pump(input_, 16);
    }
}

// ==== Buy/sell confirm screen ====

// Shared by sub_1B344 / sub_1B75B: dim band, name, price, stat panel, button frames, prompt, gold
// box.
// Returns a screen snapshot used as the base for redrawing the button labels.
std::vector<uint8_t> Pack::draw_confirm_screen(int item1, const char* name, int price, const char* prompt) {
    video_.clear();
    render_town_frame();
    load_shop_assets();

    char pstr[24];
    snprintf(pstr, sizeof(pstr), "%d", price);
    strncat(pstr, kPackStrEld, sizeof(pstr) - strlen(pstr) - 1);

    render_dim_band(0x22, 0xA0);                         // sub_18899(0, 0xA0, 0x13F, 0x22)
    const int len = (int)strlen(name);
    sh_text(0x8C - len * 8, 0x28, name, kBg);            // name
    sh_text(0xDC - len * 4, 0x28, pstr, kHi);            // price (positioned from the name length, highlight color)
    draw_item_stat_panel(item1);                         // sub_37A56
    draw_frame_box(0xA1, 0x86, 0xD2, 0x97);
    draw_frame_box(0xD8, 0x86, 0x10B, 0x97);
    sh_text(0x32, 0x88, prompt, kBg);
    draw_gold_box();                                     // sub_3507E(4,4)
    video_.present();
    wait_buttons_released();
    const uint8_t* bb = video_.get_backbuffer();
    return std::vector<uint8_t>(bb, bb + (size_t)SCREEN_W * SCREEN_H);
}

// sub_1B344(eax = building layer value): item = value - 1, price = ITEM.DAT +0x26 (word)
void Pack::handle_building_buy(int building_value) {
    const int id0 = (uint16_t)(building_value - 1);
    load_item_data();
    char name[40] = {0};
    int price = 0;
    if (id0 < MAX_ITEMS) {
        for (int i = 0; i < 0x25 && item_data_[id0][1 + i] != 0; i++) name[i] = (char)item_data_[id0][1 + i];
        price = (int16_t)(item_data_[id0][0x26] | (item_data_[id0][0x27] << 8));
    }
    std::vector<uint8_t> base = draw_confirm_screen(id0 + 1, name, price, kPackPromptBuy);
    const int cx = run_yes_no(base);
    if (cx < 0) return;
    video_.present();
    if (cx == 1) {
        if ((int32_t)team_gold_ - price < 0) {
            show_message_box(kPackMsgNoGold);
            return;                                      // loc_1B73F: returns without waiting
        }
        inv_insert_at(id0, 0);                           // count/gold handling still runs if this fails (-1)
        const int s = cur_slot();
        g_slot_item_count[s] = (uint16_t)(g_slot_item_count[s] + cx);   // add ebx, ecx (ecx = 1)
        if (strlen(name) != 0) change_gold(-price);                       // test cx,cx (name length)
    }
    wait_buttons_released();   // sub_3197D
    wait_right_released();     // sub_319DB
}

// sub_334F9: check the 12-entry unsellable list
static bool is_unsellable(int item1) {
    for (int i = 0; i < 12; i++) if (item1 == kPackUnsellable[i]) return true;
    return false;
}

// sub_1B75B(eax = item byte chosen in the list). -1: nothing selected, 0: unsellable, 1: done
int Pack::sell_item(int item1) {
    if ((uint16_t)item1 == 0) return -1;
    if (is_unsellable(item1)) {
        show_message_box(kPackMsgCannotSell);
        wait_buttons_released();    // sub_1A6E8: latch
        return 0;
    }
    load_item_data();
    const int id0 = item1 - 1;
    char name[40] = {0};
    int price = 0;
    if (id0 >= 0 && id0 < MAX_ITEMS) {
        for (int i = 0; i < 0x25 && item_data_[id0][1 + i] != 0; i++) name[i] = (char)item_data_[id0][1 + i];
        price = (int)((uint16_t)(item_data_[id0][0x26] | (item_data_[id0][0x27] << 8)) >> 1);   // shr cx,1
    }
    std::vector<uint8_t> base = draw_confirm_screen(item1, name, price, kPackPromptSell);
    const int cx = run_yes_no(base);
    if (cx < 0) return -1;
    video_.present();
    if (cx == 1) {
        const int s = cur_slot();
        int idx = 0;
        const int count = g_slot_item_count[s];
        while (idx < count && slot_item(s, idx) != (uint8_t)item1) idx++;
        slot_item(s, idx) = 0;
        inv_remove(idx);
        change_gold(price);
    }
    wait_buttons_released();   // sub_3197D
    return 1;
}

// ==== Item list (sub_337A2 / sub_33534) ====

// sub_33534: six-row list plus the "Sell"/"Cancel" buttons on the right.
// selected = chosen inventory index
void Pack::draw_item_list(int selected) {
    const int s = cur_slot();
    load_item_data();
    for (int i = 0; i < 6; i++) {
        const int top = 8 + 24 * i, bot = 0x1F + 24 * i;
        const int idx = shop_scroll_ + i;
        const int b = slot_item(s, idx);
        if (b == 0) {
            sh_fill(0x34, top, 0x8F, 0x17, 0);
            sh_hline(0x34, bot, 0x8F, 6);
            continue;
        }
        const int id0 = b - 1;
        if (idx == selected) draw_frame_box(0x34, top, 0xC3, bot);
        else { sh_fill(0x34, top, 0x8F, 0x17, 0); sh_hline(0x34, bot, 0x8F, 6); }
        if (id0 < (int)item_sprites_.size() && !item_sprites_[id0].data.empty()) {
            const auto& sp = item_sprites_[id0];
            blit_sprite_rle(0x34 + sp.x_offset, 0x2E + 24 * i + sp.y_offset, sp.data.data());
        }
        char nm[40] = {0}, en[40] = {0};
        if (id0 < MAX_ITEMS) {
            for (int k = 0; k < 0x0F && item_data_[id0][1 + k] != 0; k++) nm[k] = (char)item_data_[id0][1 + k];
            for (int k = 0; k < 0x16 && item_data_[id0][0x10 + k] != 0; k++) en[k] = (char)item_data_[id0][0x10 + k];
        }
        const int l1 = (int)strlen(nm), l2 = (int)strlen(en);
        sh_text(0x82 - (l1 / 2) * 8, 9 + 24 * i, nm, kBg);
        small_text(0x82 - (l2 / 2) * 5, 0x1A + 24 * i, en);
    }
    draw_frame_box(0xDC, 0x64, 0x118, 0x76);
    sh_text(0xE6, 0x66, kPackBtnSell, kBg);
    draw_frame_box(0xDC, 0x78, 0x118, 0x8A);
    sh_text(0xE6, 0x7A, kPackBtnCancel, kBg);
}

// sub_337A2: inventory list of the current slot. Returns the chosen item byte, 0 on cancel, -1 on
// quit.
int Pack::run_item_list() {
    load_shop_assets();
    const int s = cur_slot();
    const int count = g_slot_item_count[s];
    shop_scroll_ = 0;
    int selected = 0;                   // ebp
    int pos = 0;                        // var_8: thumb position in px
    int prev_hover = 0;                 // var_20
    // var_10: thumb length; 123.0 when count < 6, else 123 / (count / 6)
    const float thumb_f = count < 6 ? 123.0f : 123.0f / ((float)count / 6.0f);
    const float thumb_max = 123.0f - thumb_f;     // var_18
    const int thumb = (int)thumb_f;               // var_4

    auto redraw_all = [&]() {
        shop_restore_screen();
        draw_item_list(selected);
        sh_raw_blit(0xC4, 8, scroll_up_data_);
        sh_raw_blit(0xC4, 0x8E, scroll_down_data_);
        sh_fill(0xC4, 0x12, 0x0F, 0x7C, 3);                      // bar track
        draw_frame_box(0xC4, pos + 0x12, 0xD3, pos + thumb + 0x12);
        video_.present();
    };
    auto pos_from_scroll = [&]() { return (int)(123.0f * (float)shop_scroll_ / (float)count); };   // sub_320A4
    auto scroll_from_pos = [&](int c, int p) {                                                       // sub_3205E
        if (thumb == 0x7B) return 0;                              // cmp ebx, 7Bh
        return (int)(((float)(c - 5) * (float)p) / (123.0f - (float)thumb));   // (C-5) / ((A-B)/D)
    };

    redraw_all();
    wait_buttons_released();

    int last_status = 0;
    while (true) {
        if (last_status == 2) { wait_right_released(); return 0; }
        int st, mx, my;
        if (!poll_mouse(st, mx, my)) return -1;
        last_status = st;
        const int dl = slot_item(s, selected);
        int hover = 0;
        if (dl != 0 && mx >= 0xDC && mx <= 0x118 && my >= 0x64 && my <= 0x76) {
            hover = 1;
            if (st == 1) return dl;
        } else if (mx >= 0xDC && mx <= 0x118 && my >= 0x78 && my <= 0x8A) {
            hover = 2;
            if (st == 1) { wait_buttons_released(); return 0; }
        }

        if (st != 1) {
            if (prev_hover != hover) {
                // loc_33F64: draw the hovered label highlighted and restore the previous one
                if (hover == 1) sh_text(0xE6, 0x66, kPackBtnSell, kHi);
                else if (hover == 2) sh_text(0xE6, 0x7A, kPackBtnCancel, kHi);
                if (prev_hover == 1) sh_text(0xE6, 0x66, kPackBtnSell, kBg);
                else if (prev_hover == 2) sh_text(0xE6, 0x7A, kPackBtnCancel, kBg);
                video_.present();
                prev_hover = hover;
            }
            GameTimer::delay_with_pump(input_, 16);
            continue;
        }

        // Left button held
        if (mx >= 0x34 && mx <= 0xC3 && my >= 8 && my <= 0x97) selected = shop_scroll_ + (my - 8) / 0x18;

        bool dragging = false;
        if (mx >= 0xC4 && mx <= 0xD2) {
            const float top = (float)(pos + 0x12);
            if (!((float)my < top) && !((float)my > top + thumb_f)) {
                // Dragging the thumb (loc_33A58..loc_33CE3)
                dragging = true;
                int grab_y = my;
                while (true) {
                    int st2, x2, y2;
                    if (!poll_mouse(st2, x2, y2)) return -1;
                    if (st2 != 1) break;
                    if (y2 > grab_y) {
                        const int d = y2 - grab_y;
                        pos += d;
                        grab_y = y2;
                        if ((float)pos + thumb_f > 123.0f) {
                            grab_y = (int)((float)grab_y - ((float)pos + thumb_f - 123.0f));
                            pos = (int)thumb_max;
                        }
                    } else if (y2 < grab_y) {
                        const int d = grab_y - y2;
                        pos -= d;
                        grab_y -= d;
                        if (pos < 0) pos = 0;
                    } else { GameTimer::delay_with_pump(input_, 16); continue; }
                    shop_scroll_ = scroll_from_pos(count, pos);
                    redraw_all();
                    GameTimer::delay_with_pump(input_, 16);
                }
            }
        }
        if (!dragging && mx >= 0xC4 && mx <= 0xD3) {
            if (my >= 8 && my <= 0x11) {                          // up arrow
                if (shop_scroll_ > 0) shop_scroll_--;
                pos = pos_from_scroll();
            } else if (my >= 0x8E && my <= 0x97) {                // down arrow
                if (count - 6 > shop_scroll_) shop_scroll_++;
                pos = pos_from_scroll();
            } else if (my >= 0x12 && my <= 0x8D) {                // track click
                pos = my - 0x12;
                if ((float)pos + thumb_f > 123.0f) pos = (int)thumb_max;
                shop_scroll_ = scroll_from_pos(count - 1, pos);
            }
        }
        redraw_all();
        wait_buttons_released();   // end of loc_33E97: sub_3197D
    }
}

// ==== Inn (sub_1ABA6) ====

bool Pack::warp_to_town(int new_town_id) {
    if (town_id_ >= 0 && town_id_ < MAX_TOWNS) {
        saved_tile_x_[town_id_] = player_tile_x_;
        saved_tile_y_[town_id_] = player_tile_y_;
        if (player_dir_ == 0x50) saved_tile_y_[town_id_]--;     // cmp byte_82949, 50h
    }
    is_player_moving_ = false;
    player_path_.clear();
    player_move_step_ = 0;
    player_fine_x_ = 0;
    player_fine_y_ = 0;
    player_anim_counter_ = 0;
    pending_interact_x_ = pending_interact_y_ = -1;
    if (!load_town_and_tec(new_town_id)) return false;
    load_citizen_sprites(cstldata_path(town_id_ % 2 == 0 ? "citizen1.spr" : "citizen2.spr"));
    place_chest_top_tiles();
    return true;
}

void Pack::handle_inn(int talk_id) {
    video_.clear();
    render_town_frame();
    shop_grab_screen();                                   // sub_2B3DC: unk_172224
    load_shop_assets();

    render_dim_band(0x22, 0xA0);
    draw_frame_box(0xA1, 0x86, 0xD2, 0x97);
    draw_frame_box(0xD8, 0x86, 0x10B, 0x97);
    sh_text(0x50, 0x50, kPackInnFee, kBg);
    sh_text(0x84, 0x50, kPackInn200, kHi);                // "200" (highlight)
    sh_text(0xA0, 0x50, kPackInnEld, kBg);
    sh_text(0x50, 0x64, kPackInnAsk, kBg);
    sh_text(0x32, 0x88, kPackInnPrompt, kBg);
    draw_gold_box();
    video_.present();
    wait_buttons_released();
    const uint8_t* bb0 = video_.get_backbuffer();
    std::vector<uint8_t> base(bb0, bb0 + (size_t)SCREEN_W * SCREEN_H);

    const int cx = run_yes_no(base);
    if (cx < 0) return;
    if (cx == 0) { wait_buttons_released(); return; }       // loc_1AEB5: no
    if ((int32_t)team_gold_ - 0xC8 < 0) { show_message_box(kPackMsgNoGold); return; }
    change_gold(-0xC8);                                     // sub_35109(-200)

    // sub_201CF/sub_208FA: fade out; byte_54041 = 1 makes the main loop fade back in.
    uint8_t pal[256][3];
    for (int i = 0; i < 256; i++) palette_get_entry((uint8_t)i, pal[i][0], pal[i][1], pal[i][2]);
    fade_out_to_black_rendered(1);

    // loc_1AF1D: find the cell holding (talk_id+1) in the event layer, scanning y 0..h-2, x 0..w-2
    const int w = (int)map_w_, h = (int)map_h_;
    const int want = (uint16_t)talk_id + 1;
    int fy = 0, fx = 0;
    if (layer_event_) {
        for (fy = 0; fy < h - 1; fy++) {
            fx = 0;
            while (fx < w - 1 && (int)layer_event_[fy * w + fx] != want) fx++;
            if ((int)layer_event_[fy * w + fx] == want) break;
        }
        if (fy != h - 1 && fx != w - 1) {
            cam_tile_y_ = fy;
            cam_tile_x_ = fx;
            cam_fine_x_ = cam_fine_y_ = 0;
            warp_to_town(talk_id & 0xFF);                    // sub_20EB0(talk_id)
        }
    }

    // loc_1B042: heal every party member (team byte == word_1A00CC, the unk_19AC24 party array)
    for (int i = 0; i < MAX_CHARACTERS; i++) {
        if ((int)chr_team(g_characters[i]) == (int)blackbox_slot_) {
            // dword_19AC6C[i*0x88] = dword_19AC70 (HP +0x48 <- max +0x4C),
            // word_19AC74 = word_19AC76 (MP +0x50 <- +0x52). Raw offsets are used because the
            // CharacterData field offsets are unverified; same as R_HP/R_MP in chp.cpp.
            uint8_t* rec = reinterpret_cast<uint8_t*>(&g_characters[i]);
            memcpy(rec + 0x48, rec + 0x4C, 4);
            memcpy(rec + 0x50, rec + 0x52, 2);
        }
    }
    for (int i = 0; i < 256; i++) palette_set_entry((uint8_t)i, pal[i][0], pal[i][1], pal[i][2]);
}

// ==== NPC bubble / shop owner (sub_26E32, sub_25F21) ====

// sub_26E32: save a screen snapshot and draw the bubble (name + 3 lines); it does not
// wait for a click
void Pack::shop_bubble(int npc_idx, int portrait_id, const char* name,
                       const char* l1, const char* l2, const char* l3) {
    video_.clear();
    render_town_frame();
    shop_grab_screen();
    DialogPlacement dp;
    const bool ok = npc_idx < 0 ? compute_dialog_placement_for_player(dp) : compute_dialog_placement(npc_idx, dp);
    if (!ok) { video_.present(); return; }
    render_dialog_dimming(dp.dimming_x, dp.dimming_y);
    render_dialog_frame_and_tail(dp);
    render_dialog_portrait(dp, portrait_id);
    render_dialog_text_line(dp.text_x + 8, dp.text_base_y - 48, name, 0x0100);
    render_dialog_text_line(dp.text_x, dp.text_base_y - 32, l1, 0x1000);
    render_dialog_text_line(dp.text_x, dp.text_base_y - 16, l2, 0x1000);
    render_dialog_text_line(dp.text_x, dp.text_base_y, l3, 0x1000);
    video_.present();
}

// sub_25F21 "shop owner" branch: bubble -> 50 retrace -> repeat (list -> sell)
void Pack::handle_shop_owner(int npc_idx) {
    shop_bubble(npc_idx, 0x54, kPackNameShopOwner, "", kPackMsgShowGoods, "");
    GameTimer::delay_with_pump(input_, 714);                 // sub_1189F
    while (true) {
        const int sel = run_item_list();
        if (sel < 0) return;                                 // quit requested
        shop_restore_screen();
        const int r = sell_item(sel);
        if (r == -1) break;
        shop_restore_screen();
    }
    wait_buttons_released();
}

// ==== Mercenary hire (sub_1BC43) ====

static_assert(sizeof(CharacterData) == 136, "CharacterData must match the original record size (0x88)");

namespace {
// Record offsets (same as chp.cpp R_NAME/R_ID/R_FACE/R_CLASS/R_LEVEL, rechecked in PACK sub_1BC43)
constexpr int R_NAME = 0x02, R_SEX = 0x0D, R_ID = 0x0F, R_FACE = 0x10, R_CLASS = 0x11, R_LEVEL = 0x2B, R_LV2 = 0x43, R_TEAM = 0x87;
uint8_t* chr_raw(int idx) { return reinterpret_cast<uint8_t*>(&g_characters[idx]); }
const char* class_name(int cls) {
    return cls >= 0 && cls < 165 ? reinterpret_cast<const char*>(&kPackClassNames[cls * 16]) : "";
}
}  // namespace

// sub_31933: wait for a fresh action press after the previous input is released
// Phase 1 waits for both released (dword_7CF20 == 1 / dword_1A0168 == 0, via
// wait_buttons_released); phase 2 waits for left-only or Left Ctrl level. The original
// never accepts ESC here; Enter/Space edges are kept as extra aliases.
void Pack::wait_click() {
    wait_buttons_released();
    while (true) {
        input_.update();
        if (input_.quit_requested()) return;
        const bool left_only = input_.is_left_button() && !input_.is_right_button();
        if (left_only || input_.is_key_held(SDLK_LCTRL) ||
            input_.is_key_pressed(SDLK_RETURN) ||
            input_.is_key_pressed(SDLK_SPACE)) return;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// sub_237F0(character_id, team): move roster entry [id-1] into the current slot by writing its team
// byte (+0x87).
void Pack::join_team(int char_id) {
    const int idx = (char_id & 0xFFFF) - 1;
    if (idx >= 0 && idx < MAX_CHARACTERS) chr_raw(idx)[R_TEAM] = (uint8_t)blackbox_slot_;
}

int Pack::hire_merc(int npc_idx, int id) {
    if (id < 1 || id > MAX_CHARACTERS) return 0;
    uint8_t merc[0x88];
    memcpy(merc, chr_raw(id - 1), sizeof(merc));              // unk_F7870 <- unk_15BA24[(id-1)*0x88]
    const char* merc_name = reinterpret_cast<const char*>(merc + R_NAME);   // unk_F7872

    // Party = roster entries whose team byte equals word_1A00CC (the unk_19AC24 array built
    // by sub_22FED, count dword_1A00C8)
    std::vector<int> party;
    for (int i = 0; i < MAX_CHARACTERS; i++)
        if ((int)chr_raw(i)[R_TEAM] == (int)blackbox_slot_) party.push_back(i);
    // loc_1BC65: index of the first party member whose character number is byte_7D955 (var_8)
    int pidx = 0;
    while (pidx < (int)party.size() && chr_raw(party[pidx])[R_ID] != g_event_flags[0xC9]) pidx++;
    static const uint8_t zero_rec[0x88] = {0};
    const uint8_t* lead = pidx < (int)party.size() ? chr_raw(party[pidx]) : zero_rec;
    const char* lead_name = reinterpret_cast<const char*>(lead + R_NAME);

    // Step 1: name/class and the "hire this mercenary?" prompt
    video_.clear();
    render_town_frame();
    shop_grab_screen();
    load_shop_assets();
    render_dim_band(0x4A, 0xA0);
    draw_frame_box(0xA1, 0x86, 0xD2, 0x97);
    draw_frame_box(0xD8, 0x86, 0x10B, 0x97);
    sh_text((int16_t)(0x32 - pidx * 8), 0x50, merc_name, kHi);
    sh_text(0xC8, 0x50, class_name(merc[R_CLASS]), kHi);
    sh_text(0x50, 0x6E, kPackHireAsk, kBg);
    sh_text(0x32, 0x88, kPackInnPrompt, kBg);
    draw_gold_box();
    video_.present();
    wait_buttons_released();
    {
        const uint8_t* bb = video_.get_backbuffer();
        std::vector<uint8_t> base(bb, bb + (size_t)SCREEN_W * SCREEN_H);
        const int cx = run_yes_no(base);
        if (cx < 0) return 0;
        if (cx == 0) { wait_buttons_released(); return 0; }
    }
    if (party.size() >= 0x5A) {                                 // cmp word ptr dword_1A00C8, 5Ah
        show_message_box(kPackHireFull);
        wait_buttons_released();
        return 0;
    }

    // The player character's offer
    shop_restore_screen();
    shop_bubble(-1, lead[R_FACE], lead_name, "", kPackHireSay, "");
    wait_click();

    // Level check: the mercenary refuses when byte_F78B3 > the leader's unk_19AC67[...]
    if (merc[R_LV2] > lead[R_LV2]) {
        shop_restore_screen();
        shop_bubble(npc_idx, merc[R_FACE], merc_name, kPackHireRefuse1, kPackHireRefuse2, "");
        wait_click();
        return 0;
    }

    // Mercenaries missing from the dword_1A6A4 table join for free
    bool paid = false;
    for (int i = 0; i < 18; i++) if (merc[R_ID] == kPackHireTable[i]) { paid = true; break; }
    uint8_t pal[256][3];
    for (int i = 0; i < 256; i++) palette_get_entry((uint8_t)i, pal[i][0], pal[i][1], pal[i][2]);
    auto finish_join = [&]() {
        fade_out_to_black_rendered(1);                          // sub_201CF / sub_208FA
        shop_restore_screen();
        video_.present();
        join_team(merc[R_ID]);                                  // sub_237F0(byte_F787F, team, 1)
        GameTimer::delay_with_pump(input_, 471);                // sub_1199E: 33 retrace
        for (int i = 0; i < 256; i++) palette_set_entry((uint8_t)i, pal[i][0], pal[i][1], pal[i][2]);   // byte_54041 = 1
    };
    if (!paid) {
        shop_restore_screen();
        shop_bubble(npc_idx, merc[R_FACE], merc_name, kPackHireFree1, kPackHireFree2, kPackHireFree3);
        wait_click();
        wait_buttons_released();   // byte_15BA22 = 1 (latch)
        finish_join();
        return 1;
    }

    // Fee line, then the contract screen
    shop_restore_screen();
    shop_bubble(npc_idx, merc[R_FACE], merc_name, "", kPackHirePrice1, "");
    wait_click();
    shop_restore_screen();
    video_.present();

    const int price = merc[R_LEVEL] * 10;
    char pstr[24];
    snprintf(pstr, sizeof(pstr), "%d%s", price, kPackStrEld);
    char lvl[8];
    snprintf(lvl, sizeof(lvl), "%d", (int)merc[R_LEVEL]);
    const int len = (int)strlen(merc_name);

    render_dim_band(0x28, 0xA0);
    sh_text(0x64, 0x2D, kPackHireLblClass, kBg);
    sh_text(0xDC, 0x2D, kPackHireLblSex, kBg);
    sh_text(0x64, 0x3F, kPackHireLblLevel, kBg);
    sh_text(0xAA, 0x3F, kPackHireLblFee, kBg);
    sh_text((int16_t)(0x50 - len * 8), 0x2D, merc_name, kHi);
    sh_text(0x8C, 0x2D, class_name(merc[R_CLASS]), kHi);
    sh_text(0x104, 0x2D, merc[R_SEX] == 1 ? kPackHireFemale : kPackHireMale, kHi);
    sh_text(0x8C, 0x3F, lvl, kHi);
    sh_text(0xE6, 0x3F, pstr, kHi);
    draw_frame_box(0xA1, 0x86, 0xD2, 0x97);
    draw_frame_box(0xD8, 0x86, 0x10B, 0x97);
    sh_text(0x50, 0x64, kPackHireAsk, kBg);
    sh_text(0x22, 0x88, kPackHireAsk2, kBg);
    draw_gold_box();
    video_.present();
    wait_buttons_released();
    const uint8_t* bb = video_.get_backbuffer();
    std::vector<uint8_t> base(bb, bb + (size_t)SCREEN_W * SCREEN_H);
    const int cx = run_yes_no(base);
    if (cx != 1) return 0;                                       // loc_1C6A1 (returns without waiting)
    if ((int32_t)team_gold_ - atoi(pstr) < 0) { show_message_box(kPackMsgNoGold); return 0; }
    change_gold(-atoi(pstr));
    finish_join();
    return 1;
}

// sub_25F21 hire branch: when sub_1BC43 returns 1, deactivate the NPC and clear the whole
// NPC layer plus the passability tile underneath.
void Pack::handle_npc_hire(int npc_idx) {
    PackNPC& npc = npcs_[npc_idx];
    const int id = atoi(npc.name + 4);
    if (hire_merc(npc_idx, id) == 1) {
        npc.active = 0;
        if (layer_npc_) memset(layer_npc_, 0, (size_t)map_w_ * map_h_);
        if (layer_passable_) {
            const int idx = npc.tile_y * (int)map_w_ + npc.tile_x;
            if (idx >= 0 && idx < (int)tile_count_) layer_passable_[idx] = 0;
        }
    }
    wait_buttons_released();   // byte_15BA22 = 1
}

// ==== Waiting companion rejoins (sub_1C6AB, "hiredNNN") ====

// sub_1BBEA(char_id): 1 if in dword_1A67C, 0x12 if in dword_1A684, else 0
static int rejoin_category(int char_id) {
    for (int i = 0; i < 2; i++) if (char_id == kPackSetGs[i * 4]) return 1;
    for (int i = 0; i < 8; i++) if (char_id == kPackSetLoyal[i * 4]) return 0x12;
    return 0;
}

// Widx = index in the waiting list (team byte 0x63, unk_197704). Returns 1 when the member joins.
int Pack::rejoin_member(int npc_idx, int member) {
    uint8_t rec[0x88];
    memcpy(rec, chr_raw(member), sizeof(rec));
    const char* name = reinterpret_cast<const char*>(rec + R_NAME);

    std::vector<int> party;
    for (int i = 0; i < MAX_CHARACTERS; i++)
        if ((int)chr_raw(i)[R_TEAM] == (int)blackbox_slot_) party.push_back(i);
    int pidx = 0;
    while (pidx < (int)party.size() && chr_raw(party[pidx])[R_ID] != g_event_flags[0xC9]) pidx++;
    static const uint8_t zero_rec[0x88] = {0};
    const uint8_t* lead = pidx < (int)party.size() ? chr_raw(party[pidx]) : zero_rec;

    video_.clear();
    render_town_frame();
    shop_grab_screen();
    load_shop_assets();
    if (party.size() >= 0x5A) {
        show_message_box(kPackHireFull);
        wait_buttons_released();
        return 0;
    }
    render_dim_band(0x4A, 0xA0);
    draw_frame_box(0xA1, 0x86, 0xD2, 0x97);
    draw_frame_box(0xD8, 0x86, 0x10B, 0x97);
    sh_text(0x50, 0x64, kPackRejoinAsk, kBg);
    sh_text(0x32, 0x88, kPackInnPrompt, kBg);
    sh_text(0x50, 0x50, name, kHi);
    draw_gold_box();
    video_.present();
    wait_buttons_released();
    {
        const uint8_t* bb = video_.get_backbuffer();
        std::vector<uint8_t> base(bb, bb + (size_t)SCREEN_W * SCREEN_H);
        const int cx = run_yes_no(base);
        if (cx < 0) return 0;
        if (cx == 0) { wait_buttons_released(); return 0; }
    }

    // Refuses when the leader's level is below the companion's
    if (lead[R_LV2] < rec[R_LV2]) {
        shop_restore_screen();
        shop_bubble(npc_idx, rec[R_FACE], name, kPackRejoinSorry1, kPackRejoinSorry2, kPackRejoinSorry3);
        wait_click();
        wait_buttons_released();   // byte_15BA22 = 1
        return 0;
    }
    const int cat = rejoin_category(rec[R_ID]);
    if (cat == 1 && lead[R_ID] == 1) {
        shop_restore_screen();
        shop_bubble(npc_idx, rec[R_FACE], name, kPackRejoinGs1, kPackRejoinGs2, kPackRejoinGs3);
        wait_click();
        return 0;
    }
    if (cat == 0x12 && lead[R_ID] == 0x12) {
        shop_restore_screen();
        shop_bubble(npc_idx, rec[R_FACE], name, kPackRejoinLoyal1, kPackRejoinLoyal2, "");
        wait_click();
        return 0;
    }

    uint8_t pal[256][3];
    for (int i = 0; i < 256; i++) palette_get_entry((uint8_t)i, pal[i][0], pal[i][1], pal[i][2]);
    auto finish_join = [&]() {
        fade_out_to_black_rendered(1);
        shop_restore_screen();
        video_.present();
        join_team(rec[R_ID]);                                   // sub_22A53(character_id, 1)
        GameTimer::delay_with_pump(input_, 471);                // sub_1199E
        for (int i = 0; i < 256; i++) palette_set_entry((uint8_t)i, pal[i][0], pal[i][1], pal[i][2]);
    };
    bool paid = false;
    for (int i = 0; i < 18; i++) if (rec[R_ID] == kPackRejoinTable[i]) { paid = true; break; }
    if (!paid) { finish_join(); return 1; }

    // Pay line, then the contract screen (pay = level * 2 ELD)
    shop_restore_screen();
    shop_bubble(npc_idx, rec[R_FACE], name, kPackRejoinFee1, kPackRejoinFee2, "");
    wait_click();
    const int len = (int)strlen(name);
    const int price = rec[R_LEVEL] * 2;
    char pstr[24];
    snprintf(pstr, sizeof(pstr), "%d%s", price, kPackStrEld);
    char lvl[8];
    snprintf(lvl, sizeof(lvl), "%d", (int)rec[R_LEVEL]);
    shop_restore_screen();
    render_dim_band(0x28, 0xA0);
    sh_text(0x64, 0x2D, kPackHireLblClass, kBg);
    sh_text(0xDC, 0x2D, kPackHireLblSex, kBg);
    sh_text(0x64, 0x3F, kPackHireLblLevel, kBg);
    sh_text(0xB4, 0x3F, kPackRejoinLblPay, kBg);
    sh_text((int16_t)(0x50 - len * 8), 0x2D, name, kHi);
    sh_text(0x8C, 0x2D, class_name(rec[R_CLASS]), kHi);
    sh_text(0x104, 0x2D, rec[R_SEX] == 1 ? kPackHireFemale : kPackHireMale, kHi);
    sh_text(0x8C, 0x3F, lvl, kHi);
    sh_text(0xDC, 0x3F, pstr, kHi);
    draw_frame_box(0xA1, 0x86, 0xD2, 0x97);
    draw_frame_box(0xD8, 0x86, 0x10B, 0x97);
    sh_text(0x50, 0x64, kPackHireAsk, kBg);
    sh_text(0x22, 0x88, kPackHireAsk2, kBg);
    draw_gold_box();
    video_.present();
    wait_buttons_released();
    const uint8_t* bb = video_.get_backbuffer();
    std::vector<uint8_t> base(bb, bb + (size_t)SCREEN_W * SCREEN_H);
    const int cx = run_yes_no(base);
    if (cx != 1) return 0;
    if ((int32_t)team_gold_ - price < 0) { show_message_box(kPackMsgNoGold); return 0; }
    change_gold(-price);
    finish_join();
    return 1;
}

// sub_25F21 "hired"+number: find the waiting companion (team byte 0x63) with that character
// id, hand it to sub_1C6AB, and on success deactivate the NPC and clear only its own layer tile.
void Pack::handle_npc_dismiss(int npc_idx) {
    PackNPC& npc = npcs_[npc_idx];
    const int n = atoi(npc.name + 5);
    int member = -1;
    for (int i = 0; i < MAX_CHARACTERS; i++)
        if (chr_raw(i)[R_TEAM] == 0x63 && chr_raw(i)[R_ID] == n) { member = i; break; }
    if (member < 0) return;   // the original would use a garbage record; normal data always matches

    if (rejoin_member(npc_idx, member) == 1) {
        npc.active = 0;
        if (layer_npc_) {
            const int idx = npc.tile_y * (int)map_w_ + npc.tile_x;
            if (idx >= 0 && idx < (int)tile_count_) layer_npc_[idx] = 0;
        }
    }
    wait_buttons_released();   // byte_15BA22 = 1
}

// ==== EVE op24: vertically scrolling text (sub_17EFF) ====

// Snapshots the screen (sub_2B433) and scrolls the TALK record (sub_25DD3, 26-column lines) upward.
//   - stream = 0x82 (130) leading zero bytes + converted text (len) + trailing zeros
//   - six lines per frame at x = 0x32, y = 0x0A/0x1E/0x32/0x46/0x5A/0x6E + dy (line pitch 0x14)
//   - per frame: restore the snapshot, draw the lines, restore the top 20 rows (0x1900 bytes)
//     from the snapshot, clear rows 130..149 (unk_17C4A4) to black, wait 3 retraces, present.
//     dy starts at 0 and decrements by 1; on the frame where it was 0 it resets to 20 and
//     advances one line (26 bytes).
//   - ends when the line offset ebx >= len + 0x8C
void Pack::eve_scroll_text(int talk_id) {
    std::string path = game_path("talk.tbl");
    if (!file_exists(path.c_str())) path = game_path("TALK.TBL");
    std::string text;
    if (!talktbl_load_raw(path, talk_id, text)) text.clear();
    const int len = (int)text.size();
    std::vector<uint8_t> stream(130 + (size_t)len + 26 * 8, 0);
    memcpy(&stream[130], text.data(), (size_t)len);

    uint8_t* bb = video_.get_backbuffer();
    std::vector<uint8_t> snap(bb, bb + (size_t)SCREEN_W * SCREEN_H);

    int ebx = 0, dy = 0;
    while (ebx < len + 0x8C) {
        memcpy(bb, snap.data(), snap.size());
        static const int kLineY[6] = {0x0A, 0x1E, 0x32, 0x46, 0x5A, 0x6E};
        for (int line = 0; line < 6; line++) {
            char buf[27];
            const size_t off = (size_t)ebx + 26 * (size_t)line;
            for (int k = 0; k < 26; k++) buf[k] = off + k < stream.size() ? (char)stream[off + k] : 0;
            buf[26] = 0;
            sh_text(0x32, (int16_t)(kLineY[line] + dy), buf, kBg);
        }
        memcpy(bb, snap.data(), 0x1900);                                   // restore the top 20 rows
        memset(bb + 0xA280, 0, 0x1900);                                    // rows 130..149 = unk_17C4A4 (zero)
        GameTimer::delay_with_pump(input_, 43);                            // sub_2B206 x3
        video_.present();
        const int cur = dy;
        dy = cur - 1;
        ebx += 26 * 6;
        if (cur != 0) ebx -= 0x9C;
        else { dy = 0x14; ebx -= 0x82; }
        input_.update();
        if (input_.quit_requested()) { do_transition("EXIT"); return; }
    }
}

// ==== Option screen / confirm box (sub_34B92, sub_2D4A3) ====

// sub_2D4A3(msg): frame at (0x34,0x32), text at (0xA0 - (len/2)*8, 0x3A), yes (0x48,0x6C)
// and no (0xAE,0x6C) sprites. A left click on yes (x 0x48..0x8D, y 0x6C..0x7B) returns 1;
// on no (x 0xAE..0xF3, y 0x6C..0x80) returns -1.
int Pack::confirm_box(const char* msg) {
    load_shop_assets();
    if (option_sprites_.size() < 13) return -1;
    blit_sprite_rle(0x34, 0x32, option_sprites_[10].data());
    const int len = (int)strlen(msg);
    sh_text((int16_t)(0xA0 - (len / 2) * 8), 0x3A, msg, kBg);
    blit_sprite_rle(0x48, 0x6C, option_sprites_[11].data());
    blit_sprite_rle(0xAE, 0x6C, option_sprites_[12].data());
    video_.present();
    while (true) {
        int st, mx, my;
        if (!poll_mouse(st, mx, my)) return 0;
        if (mx >= 0x48 && mx <= 0x8D && my >= 0x6C && my <= 0x7B && st == 1) return 1;
        if (mx >= 0xAE && mx <= 0xF3 && my >= 0x6C && my <= 0x80 && st == 1) return -1;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// sub_34B92: option.pcx screen. Speed (byte_82730), music (byte_82731) and sfx (byte_82732)
// toggle by clicking the left (0) or right (1) area; the bottom-left button returns to the
// game and the bottom-right one asks "quit?", writes cstldata\MENU and exits with "EXIT".
void Pack::handle_options_menu() {
    load_shop_assets();
    if (option_sprites_.size() < 13) return;
    uint8_t saved_pal[256][3];
    for (int i = 0; i < 256; i++) palette_get_entry((uint8_t)i, saved_pal[i][0], saved_pal[i][1], saved_pal[i][2]);

    auto fill_black = [&]() { memset(video_.get_backbuffer(), 0, (size_t)SCREEN_W * SCREEN_H); };
    fill_black();
    video_.present();

    std::string pcx = game_path("option.pcx");
    if (!file_exists(pcx.c_str())) pcx = game_path("OPTION.PCX");

    while (true) {
        pcx_load_to_buffer(pcx, video_.get_backbuffer(), SCREEN_W, SCREEN_H);     // sub_2E46B: image + palette
        // Speed: left sprite at 0x3E for 0, right at 0xBF for 1, nothing otherwise
        if (game_cfg_[0] == 0) blit_sprite_rle(0x3E, 0x31, option_sprites_[0].data());
        else if (game_cfg_[0] == 1) blit_sprite_rle(0xBF, 0x31, option_sprites_[1].data());
        if (game_cfg_[1] == 1) blit_sprite_rle(0xBF, 0x51, option_sprites_[3].data());
        else blit_sprite_rle(0x3E, 0x51, option_sprites_[2].data());
        if (game_cfg_[2] == 1) blit_sprite_rle(0xBF, 0x71, option_sprites_[5].data());
        else blit_sprite_rle(0x3E, 0x71, option_sprites_[4].data());
        blit_sprite_rle(0x5F, 0xA2, option_sprites_[8].data());
        blit_sprite_rle(0xBC, 0xA2, option_sprites_[9].data());
        video_.present();
        wait_buttons_released();

        bool redraw = false;
        while (!redraw) {
            int st, mx, my;
            if (!poll_mouse(st, mx, my)) return;
            if (st != 1) { GameTimer::delay_with_pump(input_, 16); continue; }
            auto in = [&](int x0, int x1, int y0, int y1) { return mx >= x0 && mx <= x1 && my >= y0 && my <= y1; };
            if (in(0x3E, 0x80, 0x31, 0x4E)) { game_cfg_[0] = 0; redraw = true; }
            else if (in(0xBF, 0x101, 0x31, 0x4E)) { game_cfg_[0] = 1; redraw = true; }
            else if (in(0x3E, 0x80, 0x51, 0x6C)) { game_cfg_[1] = 0; fprintf(stderr, "PACK: BGM off (sub_39ED7, state-only)\n"); redraw = true; }       // sub_39ED7 (music off)
            else if (in(0xBF, 0x101, 0x51, 0x6C)) { game_cfg_[1] = 1; bgm_ready_ = true; bgm_play_track(bgm_town_track_); redraw = true; }      // sub_397DD + sub_39E5E(dword_82718)
            else if (in(0x3E, 0x80, 0x71, 0x8E)) { game_cfg_[2] = 0; redraw = true; }
            else if (in(0xBF, 0x101, 0x71, 0x8E)) { game_cfg_[2] = 1; redraw = true; }
            else if (in(0x21, 0x47, 0x97, 0xA5)) {
                blit_sprite_rle(0x21, 0x97, option_sprites_[6].data());                    // pressed "exit"
                video_.present();
                wait_buttons_released();
                fill_black();
                for (int i = 0; i < 256; i++) palette_set_entry((uint8_t)i, saved_pal[i][0], saved_pal[i][1], saved_pal[i][2]);   // sub_20118
                config_move_speed_ = game_cfg_[0];
                move_steps_per_tile_ = (config_move_speed_ == 1) ? MOVE_STEPS_FAST : MOVE_STEPS_SLOW;
                click_latch_ = true;
                return;
            } else if (in(0xF8, 0x11E, 0x97, 0xA5)) {
                blit_sprite_rle(0xF8, 0x97, option_sprites_[7].data());                    // pressed "quit"
                video_.present();
                GameTimer::delay_with_pump(input_, 471);                                    // sub_1199E
                wait_buttons_released();
                fill_black();
                const int ans = confirm_box(kPackQuitAsk);
                if (ans == 0) return;
                if (ans == 1) {
                    FILE* f = fopen(cstldata_path("MENU").c_str(), "wb");   // creat + open + write(4-byte handle) + close
                    if (f) { const uint32_t h = 5; fwrite(&h, 1, 4, f); fclose(f); }
                    do_transition("EXIT");
                    return;
                }
                redraw = true;
            }
        }
    }
}
