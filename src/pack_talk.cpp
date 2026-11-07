// pack_talk.cpp: PACK.EXE dialogue engine, ported from dasm/PACK_code.ASM.
//   sub_25F21 tail (NPC dialogue loop) / sub_2708B (EVE lines): per-character output,
//   page flow, and the "@" line that switches to the choice UI.
//   sub_24FD5 / sub_256CD: four-line choice UI (hover highlight, click to select).
#include "pack.h"
#include "fileutil.h"
#include "game_timer.h"
#include "talktbl.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {
inline bool talktbl_is_lead_byte(uint8_t c) { return (c >> 7) == 1; }   // sub_24D5C: (c & 0xFF) >> 7 == 1
constexpr int kNormal = 0x1000;   // sub_2C1C9(4, 0x1000, 0xFF)
constexpr int kHilite = 0x0100;   // sub_2C1C9(4, 0x0100, 0xFF)
}

bool Pack::talk_placement(int npc_idx, int eve_actor, DialogPlacement& dp) const {
    if (npc_idx >= 0) return compute_dialog_placement(npc_idx, dp);
    return compute_dialog_placement_for_eve(eve_actor, dp);
}

// Four-line choice UI: pulls four "[...]" strings starting at buf[off]. The original always
// reads four entries regardless of how many exist, leaving empty labels when short.
// Rows sit at base-0x30, -0x20, -0x10 and base; the hover band is tx..tx+0xD0, 0x10 tall
// per row. Rows are tested 4,3,2,1, so a boundary y belongs to the lower row. Returns the
// hovered row number while the left button (state 1) is held.
int Pack::run_talk_choice(const DialogPlacement& dp, int portrait_id, const std::vector<uint8_t>& buf, int off) {
    std::string items[4];
    {
        size_t pos = (size_t)off;
        for (int k = 0; k < 4; k++) {
            while (pos < buf.size() && buf[pos] != '[') pos++;
            pos++;                                   // after '['
            std::string s;
            while (pos < buf.size() && buf[pos] != ']') s.push_back((char)buf[pos++]);
            pos++;                                   // after ']'
            items[k] = s;
        }
    }
    render_dialog_dimming(dp.dimming_x, dp.dimming_y);
    render_dialog_frame_and_tail(dp);
    render_dialog_portrait(dp, portrait_id);
    const int base = dp.text_base_y;
    const int tx = dp.text_x;
    int ys[4] = {base - 0x30, base - 0x20, base - 0x10, base};
    for (int k = 0; k < 4; k++) sh_text(tx, ys[k], items[k].c_str(), kNormal);
    video_.present();
    wait_buttons_released();   // sub_3197D

    const uint8_t* bb0 = video_.get_backbuffer();
    int hover_prev = 0;
    while (true) {
        int st, mx, my;
        if (!poll_mouse(st, mx, my)) return -1;
        int hover = 0;
        const int right = tx + 0xD0;
        if (mx >= tx && mx <= right) {
            if (my >= base && my <= base + 0x10) hover = 4;
            else if (my >= base - 0x10 && my <= base) hover = 3;
            else if (my >= base - 0x20 && my <= base - 0x10) hover = 2;
            else if (my >= base - 0x30 && my <= base - 0x20) hover = 1;
        }
        if (hover != hover_prev) {
            for (int k = 1; k <= 4; k++)
                if (k != hover) sh_text(tx, ys[k - 1], items[k - 1].c_str(), kNormal);
            if (hover >= 1) sh_text(tx, ys[hover - 1], items[hover - 1].c_str(), kHilite);
            video_.present();
            hover_prev = hover;
        }
        (void)bb0;
        if (st == 1 && hover >= 1) return hover;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// Dialogue engine. buf/len come from sub_25DD3 (unk_196C14, var_18). The first page shows
// the speaker name plus 3 body lines; later pages show 4 body lines.
int Pack::run_talk_engine(int npc_idx, int eve_actor, int portrait_id, const std::string& speaker,
                          const std::vector<uint8_t>& buf, int len) {
    video_.clear();
    render_town_frame();
    shop_grab_screen();                                  // sub_2B3DC: save unk_172224 on page 1
    bool first_page = true;
    bool choice_mode = false;
    int off = 0;                                         // var_30
    bool latch = true;

    // Action key (dword_1A0168, Left Ctrl level) or left-only mouse (dword_7CF20 == 1).
    // Enter/Space edges are kept as extra aliases (not in the original) for manual testing.
    auto key_down = [&]() {
        return input_.is_key_held(SDLK_LCTRL) ||
               input_.is_key_pressed(SDLK_RETURN) ||
               input_.is_key_pressed(SDLK_SPACE);
    };

    while (true) {
        DialogPlacement dp;
        if (!talk_placement(npc_idx, eve_actor, dp)) return 0;
        render_dialog_dimming(dp.dimming_x, dp.dimming_y);
        render_dialog_frame_and_tail(dp);
        render_dialog_portrait(dp, portrait_id);
        latch = true;                                    // byte_15BA22 = 1
        const int base = dp.text_base_y;
        const int tx = dp.text_x;

        // Draw one 26-byte line character by character. A '@' in the line switches to choice
        // mode without being drawn. Returns false when a quit was requested.
        auto draw_line = [&](int y) -> bool {
            char chunk[28];
            for (int k = 0; k < 26; k++) chunk[k] = (off + k < (int)buf.size()) ? (char)buf[(size_t)off + k] : 0;
            chunk[26] = 0; chunk[27] = 0;
            if (strchr(chunk, '@')) { choice_mode = true; return true; }     // strrchr(chunk, '@') stops at the NUL
            int x = tx;
            for (int idx = 0; idx < 26; idx++) {
                const uint8_t c = (uint8_t)chunk[idx];
                char one[3] = {0, 0, 0};
                if (!talktbl_is_lead_byte(c)) {
                    one[0] = (char)c;
                    sh_text(x, y, one, kNormal);
                    x += 8;
                } else {
                    one[0] = (char)c;
                    one[1] = chunk[idx + 1];
                    sh_text(x, y, one, kNormal);
                    x += 16;
                    idx++;
                }
                video_.present();                         // sub_2B470
                int st, mx, my;
                if (!poll_mouse(st, mx, my)) return false;
                const bool pressed = (st == 1) || key_down();
                // Wait 3 retraces; skipped while pressed and the latch is clear (fast output).
                if (!pressed || latch) GameTimer::delay_with_pump(input_, 43);
                if (!pressed) latch = false;
                if (c == 0) break;
            }
            off += 26;
            return true;
        };

        // Row 0: the speaker name on the first page, body text afterwards.
        if (len > off) {
            if (first_page) {
                sh_text(tx + 8, base - 0x30, speaker.c_str(), kHilite);
                video_.present();
                first_page = false;
            } else if (!draw_line(base - 0x30)) return -1;
        }
        for (int row = 1; row <= 3; row++) {
            if (len > off && !choice_mode) {
                if (!draw_line(base - 0x30 + 0x10 * row)) return -1;
            }
        }
        latch = true;

        // Wait for a fresh click or key press (latch cleared, then pressed again).
        while (true) {
            int st, mx, my;
            if (!poll_mouse(st, mx, my)) return -1;
            const bool pressed = (st == 1) || key_down();
            if (!pressed) latch = false;
            if (pressed && !latch) break;
            GameTimer::delay_with_pump(input_, 16);
        }

        if (choice_mode) {
            shop_restore_screen();                       // sub_2B2A1(0,0): remove the dialogue box
            return run_talk_choice(dp, portrait_id, buf, off);
        }
        if (len <= off) {
            // End: EVE (sub_2708B) restores and presents the screen, NPC (sub_25F21) leaves it.
            if (eve_actor >= 0 && npc_idx < 0) { shop_restore_screen(); video_.present(); }
            click_latch_ = true;
            return 0;
        }
        shop_restore_screen();                           // sub_2B374: restore the background for the next page
    }
}
