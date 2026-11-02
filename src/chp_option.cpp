// chp_option.cpp -- ESC option screen (sub_20BC8), save (sub_16AFA) / load (sub_1716E)
// slot lists, slot timestamp (sub_1603E), yes/no dialog (sub_28E8A) and file copy
// (sub_13C31).
// Each is a direct port of the matching CHP.ASM routine. Strings come from
// chp_option_data.h; sprites are read from CHP.EXE VAs at fixed offsets.
#include "chp.h"
#include "chp_option_data.h"
#include "fileutil.h"
#include "exe_resource.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <SDL.h>

namespace {
enum OptSpr { S_SPEED_L, S_SPEED_R, S_BGM_L, S_BGM_R, S_SFX_L, S_SFX_R,
              S_BTN_OK, S_BTN_MENU, S_BTN_SAVE, S_BTN_LOAD, S_YES, S_NO };
// CHP.EXE VAs in OptSpr order (all sub_1D874 RLE streams)
constexpr uint32_t kOptSprVa[12] = {
    0x7BCBC, 0x7C298, 0x7C874, 0x7CE50,   // speed L/R, music L/R
    0x7D42C, 0x7DA08, 0x7DFE4, 0x7E5C0,   // sound L/R, close/menu buttons
    0x7EB9C, 0x7F178, 0x81E75, 0x822C5,   // save/load buttons, yes/no
};
// Slot row text y and click bands (x 0x50 < x < 0xF0, y1 < y < y2, all strict).
const int kRowTextY[5] = { 0x35, 0x49, 0x5D, 0x71, 0x85 };
const int kRowTop[6]   = { 0x35, 0x49, 0x5D, 0x71, 0x85, 0x99 };
const int kRowBot[6]   = { 0x46, 0x5A, 0x6E, 0x82, 0x96, 0xAA };
}

// sub_2CDD8(4, mode, 0xFF) sets the text mode, then sub_2D2F4(x, y, s) draws.
void CHP::opt_text(int x, int y, const char* s, int mode) {
    uint8_t* d = video_.get_backbuffer();
    if (!d || !s) return;
    font_.set_text_mode(4, (uint16_t)mode, 0xFF);
    font_.draw_string(d, SCREEN_W, SCREEN_H, x, y, s);
}

bool CHP::opt_load_assets() {
    if (opt_assets_loaded_) return true;
    bool ok = true;
    for (int i = 0; i < 12; i++)
        if (!exe_resource_load(ExeId::Chp, kOptSprVa[i], ExeResLength::Rle, opt_spr_[i])) {
            ok = false;
            opt_spr_[i].clear();
        }
    opt_assets_loaded_ = true;
    if (!ok) fprintf(stderr, "CHP: warning: some option sprites could not be read - those sprites will not be drawn\n");
    return ok;
}

bool CHP::esc_down() const {
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    return k && k[SDL_SCANCODE_ESCAPE];
}

// sub_13C31(src, dst): create dst (creat 0x180) and copy all of src into it.
//   A failed src open prints "%s read error!", waits for a key and exits; a failed dst
//   open prints "%s write error!" and exits.
bool CHP::opt_copy_file(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) {
        fprintf(stderr, "CHP: error: cannot read %s\n", src.c_str());
        do_transition("EXIT");
        return false;
    }
    std::vector<uint8_t> buf;
    uint8_t tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), in)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(in);
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) {
        fprintf(stderr, "CHP: error: cannot write %s\n", dst.c_str());
        do_transition("EXIT");
        return false;
    }
    if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), out);
    fclose(out);
    return true;
}

// ==== sub_28E8A: yes/no dialog ====
//   Draws the unk_809A4 box at (0x34,0x32) with the message centred at y 0x3A and the
//   buttons at (0x48,0x6C) and (0xAE,0x6C).
//   A left click on yes (x 0x48..0x8D, y 0x6C..0x7B) returns true, on no
//   (x 0xAE..0xF3, y 0x6C..0x80) returns false.
bool CHP::confirm_dialog(const char* msg) {
    opt_load_assets();
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return false;
    if (banner_box_sprite_loaded_ && !banner_box_sprite_.empty())
        blit_sprite_rle(0x34, 0x32, banner_box_sprite_.data());
    const int len = (int)strlen(msg);
    draw_status_text(0xA0 - ((len >> 1) << 3), 0x3A, msg);
    if (!opt_spr_[S_YES].empty()) blit_sprite_rle(0x48, 0x6C, opt_spr_[S_YES].data());
    if (!opt_spr_[S_NO].empty())  blit_sprite_rle(0xAE, 0x6C, opt_spr_[S_NO].data());
    status_flip();
    while (!dos_exit_) {
        status_poll();
        const int x = status_ev_.x, y = status_ev_.y;
        if (x >= 0x48 && x <= 0x8D && y >= 0x6C && y <= 0x7B && status_ev_.code == 1) return true;
        if (x >= 0xAE && x <= 0xF3 && y >= 0x6C && y <= 0x80 && status_ev_.code == 1) return false;
    }
    return false;
}

// ==== sub_1603E(k): draw the timestamp of slot k ====
//   Formats the blackbox.svK file time as "<month> month <day> day <hour> hour <min> min",
//   centred on the slot row. dos_findfirst date/time fields: month = (date&0x1E0)>>5,
//   day = date&0x1F, hour = time>>11, minute = (time&0x7E0)>>5.
std::string CHP::slot_info_text(int k) {
    char name[32];
    snprintf(name, sizeof(name), "blackbox.sv%d", k + 1);
    int year = 0, mon = 0, day = 0, hour = 0, min = 0;
    file_get_timestamp(game_path(name).c_str(), year, mon, day, hour, min);   // 0 on failure (the original leaves the struct zeroed)
    std::string s;
    s += std::to_string(mon);  s += kChpDateMonth;
    s += std::to_string(day);  s += kChpDateDay;
    s += std::to_string(hour); s += kChpDateHour;
    s += std::to_string(min);  s += kChpDateMin;
    return s;
}

void CHP::slot_draw_info(int k) {
    if (k < 0 || k > 4) return;
    const std::string s = slot_info_text(k);
    const int x = 0xA0 - (((int)s.size() >> 1) << 3);
    draw_status_text(x, kRowTextY[k], s.c_str());     // the caller sets the text mode
}

// Draw one list row with the current text mode. i = 0..4: timestamp or "NOT SAVED";
//   5: bottom row; anything else: nothing.
//   Bottom row: the save screen uses aBaEbauaab_0 at x 0x78 for both highlight and
//   restore, the load screen uses aBaEbauaab at x 0x68.
void CHP::slot_draw_row(int i, const bool* exists, bool save) {
    if (i >= 0 && i <= 4) {
        if (exists[i]) slot_draw_info(i);
        else draw_status_text(0x7D, kRowTextY[i], kChpSlotNotSaved);
    } else if (i == 5) {
        if (save) draw_status_text(0x78, 0x99, kChpSlotTitleHi);
        else      draw_status_text(0x68, 0x99, kChpSlotTitle);
    }
}

// ==== sub_16AFA (save) / sub_1716E (load): five slots plus the bottom row ====
//   Both routines share the same body. Slot k "exists" when both blackbox.svK and
//   chapdata\contibak.svK open.
//   Save: sub_15BD9 (write blackbox.dat) + sub_1492E (write contibak.tmp), copy both to
//   svK, reload AN02.PCX, redraw only that slot's timestamp, wait sub_1366E and close.
//   Load: write "k\n" to cstldata\LOAD(k+1) and exit; MAIN performs the actual restore.
//   The bottom row closes. The hovered row is redrawn with text mode 0x100 to highlight it.
void CHP::slot_screen(bool save) {
    opt_load_assets();
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;

    bool exists[5] = { true, true, true, true, true };           // dword_13939 = 01 01 01 01 01
    for (int k = 0; k < 5; k++) {
        char n1[32], n2[32];
        snprintf(n1, sizeof(n1), "blackbox.sv%d", k + 1);
        snprintf(n2, sizeof(n2), "contibak.sv%d", k + 1);
        FILE* f1 = fopen(game_path(n1).c_str(), "rb");
        FILE* f2 = fopen(chapdata_path(n2).c_str(), "rb");
        if (!f1 || !f2) exists[k] = false;
        if (f1) fclose(f1);
        if (f2) fclose(f2);
    }

    std::vector<uint8_t> bg;
    auto load_bg = [&]() {                                       // sub_2DD68("AN02.PCX") (with palette)
        if (status_load_pcx("AN02.PCX", true, bg)) memcpy(bb, bg.data(), (size_t)SCREEN_W * SCREEN_H);
    };
    load_bg();
    font_.set_text_mode(4, 0x1000, 0xFF);
    for (int k = 0; k < 5; k++) slot_draw_row(k, exists, save);
    draw_status_text(0x68, 0x99, kChpSlotTitle);                 // both screens start with aBaEbauaab at x 0x68
    status_flip();
    status_wait_left_release();

    int hl = 6;                                                  // var_4: highlighted row (6 = none)
    while (!dos_exit_) {
        status_poll();
        const int x = status_ev_.x, y = status_ev_.y;
        const bool left = status_ev_.code == 1;
        int hit = 6;
        if (x > 0x50 && x < 0xF0) {
            for (int r = 0; r < 6; r++) {
                if (y > kRowTop[r] && y < kRowBot[r]) { hit = r; break; }
            }
        }
        if (hit <= 4 && left) {
            if (save) {
                // sub_15BD9 / sub_1492E / sub_13C31 x2
                save_blackbox();
                save_contibak();
                char n1[32], n2[32];
                snprintf(n1, sizeof(n1), "blackbox.sv%d", hit + 1);
                snprintf(n2, sizeof(n2), "contibak.sv%d", hit + 1);
                if (!opt_copy_file(game_path("blackbox.dat"), game_path(n1))) return;
                if (!opt_copy_file(chapdata_path("contibak.tmp"), chapdata_path(n2))) return;
                load_bg();
                font_.set_text_mode(4, 0x1000, 0xFF);
                slot_draw_info(hit);
                status_flip();
                flip_wait(kVsync1366E);
                break;
            } else if (exists[hit]) {
                char n[16];
                snprintf(n, sizeof(n), "LOAD%d", hit + 1);
                FILE* f = fopen(cstldata_path(n).c_str(), "wt");
                if (f) { fprintf(f, "%d\n", hit); fclose(f); }
                do_transition("EXIT");
                return;
            }
        } else if (hit == 5 && left) {
            break;                                               // loc_17119 / loc_17133
        }
        if (hit == hl) continue;
        // Highlight the new row (0x100) and restore the previous one (0x1000).
        font_.set_text_mode(4, 0x100, 0xFF);
        slot_draw_row(hit, exists, save);
        font_.set_text_mode(4, 0x1000, 0xFF);
        slot_draw_row(hl, exists, save);
        hl = hit;
        status_flip();
    }
    memset(bb, 0, (size_t)SCREEN_W * SCREEN_H);                  // sub_1DB9B (clear the whole screen)
    video_.present();
}

// ==== sub_20BC8: ESC option screen (option.pcx) ====
//   Row 1 (y 0x31..0x4E): byte_C5CDE, 0 = wait for vsync (left, x 0x3E..0x80),
//                          1 = no wait (right, x 0xBF..0x101).
//   Row 2 (y 0x51..0x6C): byte_C5CDF, background music off/on.
//   Row 3 (y 0x71..0x8E): byte_C5CE0, sound effects off/on.
//   Buttons: close (0x21..0x47, 0x97..0xA5) / to main menu (0xF8..0x11E, 0x97..0xA5;
//   after confirmation writes cstldata\MENU and exits) / save (0x5F..0x83, 0xA2..0xBE) /
//   load (0xBC..0xE0, 0xA2..0xBE).
//   byte_C5CDE..C5CE0 = state_.config_data[0..2], the first three bytes of CHP.CFG.
void CHP::option_screen() {
    opt_load_assets();
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    fprintf(stderr, "CHP: option_screen (sub_20BC8)\n");

    palette_get_range(0, 255, status_save_pal_);                 // sub_1DDC4 -> word_A3D4C
    memset(bb, 0, (size_t)SCREEN_W * SCREEN_H);
    video_.present();                                            // the original lays down the map with sub_286DC and immediately clears it

    std::vector<uint8_t> pcx;
    auto redraw = [&]() {                                        // loc_20C01
        if (status_load_pcx("option.pcx", true, pcx)) memcpy(bb, pcx.data(), (size_t)SCREEN_W * SCREEN_H);
        auto put = [&](int idx, int x, int y) { if (!opt_spr_[idx].empty()) blit_sprite_rle(x, y, opt_spr_[idx].data()); };
        const uint8_t speed = state_.config_data[0];
        if (speed == 0) put(S_SPEED_L, 0x3E, 0x31); else if (speed == 1) put(S_SPEED_R, 0xBF, 0x31);
        if (state_.config_data[1] == 1) put(S_BGM_R, 0xBF, 0x51); else put(S_BGM_L, 0x3E, 0x51);
        if (state_.config_data[2] == 1) put(S_SFX_R, 0xBF, 0x71); else put(S_SFX_L, 0x3E, 0x71);
        status_flip();
        status_wait_left_release();                              // sub_2E6CF
    };
    auto put_btn = [&](int idx, int x, int y) {
        if (!opt_spr_[idx].empty()) blit_sprite_rle(x, y, opt_spr_[idx].data());
        status_flip();
    };
    auto close_screen = [&]() {                                  // loc_21090
        status_wait_left_release();
        memset(bb, 0, (size_t)SCREEN_W * SCREEN_H);
        video_.present();
        palette_set_range(0, 255, status_save_pal_);             // sub_1DD0D
    };

    redraw();
    while (!dos_exit_) {
        status_poll();
        if (status_ev_.code != 1) continue;
        const int x = status_ev_.x, y = status_ev_.y;
        auto in = [&](int x0, int x1, int y0, int y1) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; };

        if (in(0x3E, 0x80, 0x31, 0x4E)) { state_.config_data[0] = 0; redraw(); continue; }   // C5C58 = 0x895440
        if (in(0xBF, 0x101, 0x31, 0x4E)) { state_.config_data[0] = 1; redraw(); continue; }  // C5C58 = 0
        if (in(0x3E, 0x80, 0x51, 0x6C)) {
            state_.config_data[1] = 0;                           // sub_3615B: stop music (the port has no music engine)
            redraw(); continue;
        }
        if (in(0xBF, 0x101, 0x51, 0x6C)) {
            state_.config_data[1] = 1;                           // sub_35A61 + sub_360E2(11): start music (the port has no music engine)
            redraw(); continue;
        }
        if (in(0x3E, 0x80, 0x71, 0x8E)) { state_.config_data[2] = 0; redraw(); continue; }
        if (in(0xBF, 0x101, 0x71, 0x8E)) { state_.config_data[2] = 1; redraw(); continue; }

        if (in(0x21, 0x47, 0x97, 0xA5)) {                        // close
            put_btn(S_BTN_OK, 0x21, 0x97);
            close_screen();
            return;
        }
        if (in(0xF8, 0x11E, 0x97, 0xA5)) {                       // to main menu
            put_btn(S_BTN_MENU, 0xF8, 0x97);
            flip_wait(kVsync1376D);
            status_wait_left_release();
            if (confirm_dialog(kChpOptMsgMenu)) {
                file_create_handle_flag(cstldata_path("MENU").c_str());
                do_transition("EXIT");
                return;
            }
            redraw();
            continue;
        }
        if (in(0x5F, 0x83, 0xA2, 0xBE)) {                        // save
            put_btn(S_BTN_SAVE, 0x5F, 0xA2);
            flip_wait(kVsync1376D);
            slot_screen(true);
            if (state_.transition_pending || dos_exit_) return;
            close_screen();
            return;
        }
        if (in(0xBC, 0xE0, 0xA2, 0xBE)) {                        // load
            put_btn(S_BTN_LOAD, 0xBC, 0xA2);
            flip_wait(kVsync1376D);
            slot_screen(false);
            if (state_.transition_pending || dos_exit_) return;
            close_screen();
            return;
        }
    }
}
