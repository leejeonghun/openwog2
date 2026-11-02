#include "ttl.h"
#include "pcx.h"
#include "fli.h"
#include "audio.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "game_timer.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <string>
#include <SDL.h>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

namespace {

// ---- Main menu (sub_137B6 / sub_138A8) ----
// Item number 1..4 = index + 1. Text positions are the sub_15421(x, y, str) call arguments.
struct MenuItem { int x; int y; const char* text; };
const MenuItem kMenuItems[4] = {
    { 0x64, 0x49, "\xC0\xE1\xB7\x71\xA6\x81\xC8\xE1 \xD0\x61\x8B\xA1" },                 // "New Game"
    { 0x64, 0x5D, "  \x9D\xA1\x97\x61 \x89\x41\xB7\xB1" },                              // "Load Game"
    { 0x5A, 0x71, "\xB8\xE5\xCA\x81 \xB7\xA1\xB4\xE1\xAC\xE1 \xD0\x61\x8B\xA1" },       // "Continue Battle"
    { 0x64, 0x85, "   DOS \x9D\xA1" },                                                  // "To DOS"
};
const char kVersionText[] = "Version 1.2";

// ---- Load screen (sub_13038 / sub_1257C) ----
// Row 0..4 = save slot (text y and hit area), row 5 = "[ Back ]".
// Hit area is x 0x50<x<0xF0 and top<y<bot, all strict.
const int kRowTextY[5] = { 0x35, 0x49, 0x5D, 0x71, 0x85 };
const int kRowTop[6]   = { 0x35, 0x49, 0x5D, 0x71, 0x85, 0x99 };
const int kRowBot[6]   = { 0x46, 0x5A, 0x6E, 0x82, 0x96, 0xAA };
const char kSlotNotSaved[] = "NOT SAVED";                                              // x = 0x7D fixed
const char kSlotBack[]     = "[ \xA1\x41\xB7\xA5\xA1\x41\x93\x41\x9D\xA1 ]";           // "[ Back ]"  x = 0x68, y = 0x99
// AI / aI_0 / aPb / aJe (TTL.EXE data 0x2005C..0x2006B), same table as chp_option_data.h.
const char kDateMonth[] = " \xB6\xA9 ";   // month suffix
const char kDateDay[]   = " \xB7\xA9 ";   // day suffix
const char kDateHour[]  = " \xAF\xA1 ";   // hour suffix
const char kDateMin[]   = " \xA6\x85";    // minute suffix

}  // namespace

TTL::TTL(Video& video, Input& input) : video_(video), input_(input) {}
TTL::~TTL() = default;

std::string TTL::game_path(const char* filename) {
    return file_find_case_insensitive(GAME_DATA_DIR, filename);
}

std::string TTL::cstldata_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CSTLDATA", filename);
}

std::string TTL::chapdata_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CHAPDATA", filename);
}

void TTL::load_config() {
    std::string path = game_path("game.cfg");
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "TTL: warning: cannot open game.cfg\n");
        memset(&config_, 0, sizeof(config_));
        return;
    }
    fread(&config_, 1, 0x0A, fp);
    fclose(fp);
}

// ---- sub_13B60: check IPC flags ----
IPCFlag TTL::check_ipc_flags() {
    IPCFlag flag_values[] = {
        IPCFlag::LOAD1, IPCFlag::LOAD2, IPCFlag::LOAD3,
        IPCFlag::LOAD4, IPCFlag::LOAD5
    };

    for (int i = 0; i < 5; i++) {
        char flag_name[16];
        snprintf(flag_name, sizeof(flag_name), "LOAD%d", i + 1);
        std::string path = cstldata_path(flag_name);

        if (file_exists(path.c_str())) {
            remove(path.c_str());
            std::string dollar = game_path("$");
            remove(dollar.c_str());
            return flag_values[i];
        }
    }

    {
        std::string path = cstldata_path("MENU");
        if (file_exists(path.c_str())) {
            remove(path.c_str());
            return IPCFlag::MENU;
        }
    }

    {
        std::string path = cstldata_path("EXIT");
        if (file_exists(path.c_str())) {
            remove(path.c_str());
            std::string dollar = game_path("$");
            remove(dollar.c_str());
            cleanup_and_exit(0);   // sub_12358(0) never returns in the original; callers test should_exit_
        }
    }

    return IPCFlag::NONE;
}

bool TTL::copy_file(const char* src, const char* dst) {
    return file_copy(src, dst);
}

// ---- sub_123E3: copy a file; on failure prints a message and exits with 0xFF ----
bool TTL::copy_or_die(const std::string& src, const std::string& dst) {
    if (!file_exists(src.c_str())) {
        fprintf(stderr, "TTL: error: cannot read %s\n", src.c_str());
        cleanup_and_exit(0xFF);
        return false;
    }
    if (!copy_file(src.c_str(), dst.c_str())) {
        fprintf(stderr, "TTL: error: cannot write %s\n", dst.c_str());
        cleanup_and_exit(0xFF);
        return false;
    }
    return true;
}

// ---- sub_1948B: play FLI intro with VOC audio ----
bool TTL::play_fli_with_audio(const std::string& fli_name, const std::string& voc_name) {
    std::string fli_path = game_path(fli_name.c_str());
    std::string voc_path = game_path(voc_name.c_str());

    if (!file_exists(fli_path.c_str())) {
        fprintf(stderr, "TTL: warning: Intro %s not found, skipping.\n", fli_name.c_str());
        return false;
    }

    fprintf(stderr, "TTL: Playing intro: %s with audio: %s\n", fli_path.c_str(), voc_path.c_str());
    palette_set_all_black();
    video_.clear();
    video_.present();

    return FLIPlayer::play(fli_path, voc_path, video_, input_);
}

// ---- main_ (loc_13E39 / loc_13F01): intro ----
// Flag 0 (first run): HICOM + SOFTMAX. Flag 3 (back to MENU): SOFTMAX only,
// HICOM skipped via loc_13F01.
void TTL::play_intro(bool with_hicom) {
    fprintf(stderr, "TTL: === Starting intro sequences ===\n");

    if (with_hicom) {
        // 1. HiCom Intro (HICOM.FLI + HICOM.VOC)
        play_fli_with_audio("HICOM.FLI", "HICOM.VOC");
    }

    // 2. Softmax Intro (SOFTMAX.FLI + SOFTMAX.VOC)
    play_fli_with_audio("SOFTMAX.FLI", "SOFTMAX.VOC");

    palette_fade_to_black(16);
    video_.clear();
    video_.present();
}

// ==== Mouse cursor (sub_141B4..sub_14600) ====
// The original draws a software cursor into VRAM from an INT 33h user handler (loc_143AA):
//   - button state 0 uses unk_2096B (14x14), otherwise unk_20A33 (14x12)
//   - no hotspot: the sprite's top-left corner sits at (x, y)
//   - hidden initially (word_20969 = 0), shown by sub_14543 after the menu is drawn,
//     hidden by sub_14585
// The port uses Video's hardware cursor (scaling and palette handling live in video.cpp).
// The original driver's coordinate clamp (x 8..307, y 2..194) is not applied to the OS cursor.
// Two variable-length sprites at VA 0x2096B..0x20ADF of TTL.EXE Object 2 are read directly:
// [width u16][height u16][pixels width*height] (0xFF = transparent).
// Frame 0 = released (unk_2096B, 14x14), frame 1 = pressed (unk_20A33, 14x12).
bool TTL::load_cursor_frames(std::vector<CursorFrame>& frames) {
    std::vector<uint8_t> raw;
    if (!exe_resource_load(ExeId::Ttl, 0x2096B, ExeResLength::Fixed, raw, 372)) {
        fprintf(stderr, "TTL: warning: TTL.EXE VA 0x2096B (mouse cursor sprites) load failed - using the system cursor\n");
        return false;
    }

    if (!Video::cursor_parse_bin(raw.data(), raw.size(), frames) || frames.size() < 2) {
        fprintf(stderr, "TTL: warning: TTL.EXE VA 0x2096B: cursor sprites not found (expected 2, got %zu)\n", frames.size());
        return false;
    }
    return true;
}

void TTL::init_mouse() {
    std::vector<CursorFrame> frames;
    if (load_cursor_frames(frames)) {
        cursor_normal_ = video_.cursor_register(std::move(frames[0]));
        const int pressed = video_.cursor_register(std::move(frames[1]));
        video_.cursor_set_pressed_variant(cursor_normal_, pressed);   // button state != 0 -> pressed sprite (loc_143AA)
        video_.cursor_select(cursor_normal_);
        // Reflect pressed state, scaling and palette changes from the event pump,
        // wherever the wait loop runs
        input_.set_poll_hook([this] { video_.cursor_update(); });
    }
    // If the load failed the id stays -1 and the system cursor stays visible.
    video_.cursor_show(false);   // shown when the first menu is drawn
    fprintf(stderr, "TTL: Mouse initialized\n");
}

bool TTL::left_only() const {
    return input_.is_left_button() && !input_.is_right_button();
}

// sub_1232D: wait while only the left button is held, so a click already down when a screen
// is drawn does not leak into the next screen.
void TTL::wait_left_release() {
    while (!should_exit_) {
        input_.update();
        if (input_.quit_requested()) {
            cleanup_and_exit(0);
            return;
        }
        if (!left_only()) return;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// ---- sub_13759: New Game ----
void TTL::new_game() {
    fprintf(stderr, "TTL: New game selected (sub_13759)\n");
    if (!copy_or_die(game_path("BLACKBOX.DEF"), game_path("blackbox.dat"))) return;
    file_create_empty(game_path("$").c_str());
    file_create_empty(game_path("START").c_str());
    file_delete(game_path("continue.sve").c_str());
    file_delete(game_path("contmap.dat").c_str());

    // sub_13759: mov eax, 1; call sub_12358 -> exit with code 1
    cleanup_and_exit(1);
}

void TTL::draw_text(int x, int y, const char* text) {
    uint8_t* backbuf = video_.get_backbuffer();
    font_.draw_string(backbuf, SCREEN_W, SCREEN_H, x, y, text);
}

// ---- Draw one menu item in its current state (highlight: text mode 0x100, normal: 0x1000) ----
void TTL::draw_menu_item(int item, bool highlight) {
    if (item < 1 || item > 4) return;
    const MenuItem& m = kMenuItems[item - 1];
    font_.set_text_mode(4, highlight ? 0x0100 : 0x1000, 0xFF);
    draw_text(m.x, m.y, m.text);
}

// ---- sub_137B6: draw the main menu (PCX + items + version), then show the cursor (sub_14543) ----
// The "Continue Battle" item is drawn only when blackbox.dat exists.
void TTL::render_menu() {
    uint8_t* backbuf = video_.get_backbuffer();
    pcx_load_to_buffer(game_path("AN02.PCX"), backbuf, SCREEN_W, SCREEN_H);

    for (int item = 1; item <= 4; item++) {
        if (item == 3 && !has_continue_data_) continue;
        draw_menu_item(item, false);
    }

    // Version string     -> X=4, Y=182 (0xB6)
    font_.set_text_mode(4, 0x1000, 0xFF);
    draw_text(4, 0xB6, kVersionText);

    video_.present();
    video_.cursor_show(true);
}

// ---- sub_138A8 hit test: 0x64<x<0xCC, y strictly inside (top, bot); 0 = none ----
// Item 3 (Continue Battle) only hits when can_continue is set: blackbox.dat and
// chapdata\contibak.tmp both exist.
int TTL::menu_hit(int mx, int my, bool can_continue) const {
    if (!(mx > 0x64 && mx < 0xCC)) return 0;
    if (my > 0x49 && my < 0x5A) return 1;
    if (my > 0x5D && my < 0x6E) return 2;
    if (can_continue && my > 0x71 && my < 0x82) return 3;
    if (my > 0x85 && my < 0x96) return 4;
    return 0;
}

// ---- sub_138A8: main menu ----
// The original leaves sub_138A8 when returning from the load screen and main_ re-enters it,
// so the outer loop here restarts from the top (file checks + full redraw).
void TTL::handle_menu() {
    while (!should_exit_) {
        has_continue_data_ = file_exists(game_path("blackbox.dat").c_str());
        const bool can_continue = has_continue_data_ &&
                                  file_exists(chapdata_path("contibak.tmp").c_str());
        render_menu();

        int prev = 0;   // dword_20951: item highlighted last
        bool reopen = false;

        while (!should_exit_ && !reopen) {
            input_.update();
            if (input_.quit_requested()) {
                cleanup_and_exit(0);
                break;
            }
    
            const int cur = menu_hit(input_.get_mouse_x(), input_.get_mouse_y(), can_continue);

            // Run the item as soon as it is pressed (left only); the original tests button state ==
            // 1
            if (cur != 0 && left_only()) {
                switch (cur) {
                    case 1: // New Game
                        video_.cursor_show(false);   // sub_14585
                        new_game();
                        break;
                    case 2: // Load Game
                        video_.cursor_show(false);
                        handle_save_load();
                        reopen = true;               // returning via "[ Back ]" reopens the menu
                        break;
                    case 3: // Continue Battle
                        video_.cursor_show(false);
                        file_create_empty(cstldata_path("PASS").c_str());
                        file_create_empty(game_path("$").c_str());
                        cleanup_and_exit(1);
                        break;
                    case 4: // To DOS
                        file_delete(game_path("$").c_str());
                        cleanup_and_exit(0);
                        break;
                }
                if (should_exit_ || reopen) break;
            }

            if (cur != prev) {
                // Redraw only the text: new item highlighted (0x100), previous restored (0x1000).
                // The PCX is not repainted.
                draw_menu_item(cur, true);
                draw_menu_item(prev, false);
                video_.present();
                prev = cur;
            }

            GameTimer::delay_with_pump(input_, 16);
        }
    }
}

// ==== Load screen (sub_13038 / sub_1257C) ====

// sub_1257C: blackbox.svN timestamp as "M month D day H hour M minute"
// (itoa output, no zero padding)
std::string TTL::slot_info_text(int slot) {
    char name[32];
    snprintf(name, sizeof(name), "blackbox.sv%d", slot);
    int year = 0, mon = 0, day = 0, hour = 0, min = 0;
    file_get_timestamp(game_path(name).c_str(), year, mon, day, hour, min);   // 0 on failure

    std::string s;
    s += std::to_string(mon);  s += kDateMonth;
    s += std::to_string(day);  s += kDateDay;
    s += std::to_string(hour); s += kDateHour;
    s += std::to_string(min);  s += kDateMin;
    return s;
}

// Draw one row in its current state (0x100 when highlighted, else 0x1000).
//   0..4: date centred at x = 0xA0 - (strlen>>1)*8, empty slot prints "NOT SAVED" (x = 0x7D)
//   5   : "[ Back ]" (x = 0x68, y = 0x99)
void TTL::draw_slot_row(int row, const bool* exists, bool highlight) {
    if (row < 0 || row > 5) return;
    font_.set_text_mode(4, highlight ? 0x0100 : 0x1000, 0xFF);
    if (row == 5) {
        draw_text(0x68, 0x99, kSlotBack);
    } else if (exists[row]) {
        const std::string s = slot_info_text(row + 1);
        draw_text(0xA0 - (((int)s.size() >> 1) << 3), kRowTextY[row], s.c_str());
    } else {
        draw_text(0x7D, kRowTextY[row], kSlotNotSaved);
    }
}

// Start a save slot (sub_13038 click handling / main_ loc_14035..loc_14103):
//   copy blackbox.svN -> blackbox.dat and chapdata\contibak.svN -> contibak.tmp,
//   leave LOADED and "$", delete contmap.dat, then exit(1)
void TTL::start_loaded_game(int slot, bool from_ipc_flag) {
    char src_bb[64], src_ci[64], flag_file[64];
    snprintf(src_bb, sizeof(src_bb), "blackbox.sv%d", slot);
    snprintf(src_ci, sizeof(src_ci), "contibak.sv%d", slot);
    snprintf(flag_file, sizeof(flag_file), "LOAD%d", slot);

    if (!copy_or_die(game_path(src_bb), game_path("blackbox.dat"))) return;
    if (!copy_or_die(chapdata_path(src_ci), chapdata_path("contibak.tmp"))) return;

    if (from_ipc_flag) {
        // Main_: delete LOADn and contmap.dat first, then create LOADED and "$"
        file_delete(cstldata_path(flag_file).c_str());
        file_delete(game_path("contmap.dat").c_str());
        file_create_empty(cstldata_path("LOADED").c_str());
        file_create_empty(game_path("$").c_str());
    } else {
        // sub_13038: create LOADED and "$", then delete contmap.dat
        file_create_empty(cstldata_path("LOADED").c_str());
        file_create_empty(game_path("$").c_str());
        file_delete(game_path("contmap.dat").c_str());
    }

    cleanup_and_exit(1);
}

void TTL::handle_save_load() {
    fprintf(stderr, "TTL: Save/Load screen (sub_13038)\n");

    // A slot exists when both blackbox.svN and chapdata\contibak.svN can be opened
    bool exists[5];
    for (int k = 0; k < 5; k++) {
        char n1[32], n2[32];
        snprintf(n1, sizeof(n1), "blackbox.sv%d", k + 1);
        snprintf(n2, sizeof(n2), "contibak.sv%d", k + 1);
        exists[k] = file_exists(game_path(n1).c_str()) && file_exists(chapdata_path(n2).c_str());
    }

    uint8_t* backbuf = video_.get_backbuffer();
    pcx_load_to_buffer(game_path("AN02.PCX"), backbuf, SCREEN_W, SCREEN_H);
    for (int row = 0; row < 6; row++) draw_slot_row(row, exists, false);
    video_.present();
    video_.cursor_show(true);   // sub_14543
    wait_left_release();        // sub_1232D

    int prev = 6;               // var_4: highlighted row (6 = none)
    while (!should_exit_) {
        input_.update();
        if (input_.quit_requested()) {
            cleanup_and_exit(0);
            break;
        }

        const int mx = input_.get_mouse_x();
        const int my = input_.get_mouse_y();

        int cur = 6;
        if (mx > 0x50 && mx < 0xF0) {
            for (int r = 0; r < 6; r++) {
                if (my > kRowTop[r] && my < kRowBot[r]) { cur = r; break; }
            }
        }

        if (cur <= 4 && left_only() && exists[cur]) {
            start_loaded_game(cur + 1, false);
            break;
        }
        if (cur == 5 && left_only()) {
            break;   // loc_1374C: back to the previous menu
        }

        if (cur != prev) {
            draw_slot_row(cur, exists, true);
            draw_slot_row(prev, exists, false);
            video_.present();
            prev = cur;
        }

        GameTimer::delay_with_pump(input_, 16);
    }
}

// ---- sub_12358: cleanup and exit ----
// Video/Audio are owned by gene2.cpp and stay open; only the screen is reset
// and the exit code stored.
void TTL::cleanup_and_exit(int code) {
    fprintf(stderr, "TTL: Cleanup and exit (code=%d)\n", code);

    input_.set_poll_hook({});
    video_.cursor_reset();   // sub_14521: reset the mouse driver -> back to the system cursor

    if (code != 0) {
        palette_fade_to_black(16);
    } else {
        video_.set_text_mode();
    }

    video_.clear();
    should_exit_ = true;
    return_code_ = code;
}

int TTL::run() {
    fprintf(stderr, "TTL: Starting...\n");

    should_exit_ = false;
    return_code_ = 0;

    // Video/Input are the single instances injected by the constructor
    // (initialised once in gene2.cpp).
    video_.set_mode_13h();
    video_.cursor_show(false);   // no cursor during the intro (the original initialises the driver later)

    font_.load_hangul(game_path("NEWHAN.FNT").c_str());
    font_.load_ascii(game_path("NEWASCII.FNT").c_str());
    font_.set_text_mode(4, 0x1000, 0xFF);

    load_config();
    ipc_flag_ = check_ipc_flags();
    if (should_exit_) return return_code_;   // EXIT flag

    has_continue_data_ = file_exists(game_path("blackbox.dat").c_str());

    // Main_: flag 0 -> HICOM + SOFTMAX then MENU, 3 (MENU) -> SOFTMAX only,
    // 4..8 (LOAD) -> no intro
    if (ipc_flag_ == IPCFlag::NONE) {
        play_intro(true);
        ipc_flag_ = IPCFlag::MENU;
    } else if (ipc_flag_ == IPCFlag::MENU) {
        play_intro(false);
    }

    init_mouse();

    switch (ipc_flag_) {
        case IPCFlag::LOAD1:
        case IPCFlag::LOAD2:
        case IPCFlag::LOAD3:
        case IPCFlag::LOAD4:
        case IPCFlag::LOAD5:
            start_loaded_game((int)ipc_flag_ - 3, true);
            break;
        case IPCFlag::MENU:
        default:
            handle_menu();
            break;
    }

    return return_code_;
}
