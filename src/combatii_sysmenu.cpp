// Combat system menu (sub_4E0EE): mouse-driven icon menu matching the ASM behavior.
//
// Original structure (COMBATII_code.ASM 83532-84209):
//   sub_4E0EE(eax = no_saveload): enter sub_103A8 (weather tick off) + sub_44D6E(1) (dim) + sub_44DEC(0).
//     di = full redraw flag, cx = hovered item (-1 = none).
//     Full redraw: sub_4DBDE (icon index array word_FD8C2) -> sub_454B3 (battle screen) -> sub_4DC75 (menu art)
//                  -> label (sub_5E048, x = 0xA0 - 4*strlen, y = 0x85) -> present.
//     Events (sub_5D22E / getch_): only Esc (0x1B) matters among keys; other keys reset cx = -1.
//       1 (mouse move) : sub_4DB7E hit test, then redraw only the label panel (MENUSTAT #31) and label.
//       2 (left press) : on a hit (items other than 3/4/6) play the sub_4DD83 pressed effect, then act.
//       3 (right press) / Esc : leave the menu (return -1).
//     After an action, cx 2 or 8 ends the menu; otherwise full redraw (cx is kept so the label stays).
//   MENUSTAT.SYB entries: 0 = background (+0xA0,+0x24), 1.. = icons (normal 2k+1, pressed 2k+2),
//     25..28 = slider arrows, 29 = slider base, 30 = ticks, 31 = label panel (+0xA0,+0x8C).
//   sub_4DE0A: TURN interval slider (byte_74138, 2..30, step 2).
//
// Deviation: uses the SDL hardware cursor, so the software cursor (sub_5D41E/5D570) is not drawn.
// Deviation: the music/SFX toggles (sub_4F369/4F3AE/4F427/4F450) only flip flags; the port has no
// BGM.
// Deviation: cases 7 (restart battle) and 8 (to TITLE) end the process in the original
//   (sub_61CFF/sub_4FE28), so the port returns 7/8 for the caller to handle.
#include "combatii.h"
#include "combatii_internal.h"
#include "combatii_deploy_data.h"

#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
// off_7545C[9] / off_75480 (EXE VA 0x77DC8..0x77E34), Johab encoded.
const char* const kMenuLabels[9] = {
    "\xB7\xB1\xA2\x81\x20\xAC\xE9\xA1\x77",                          // "Mission description"
    "QUICK SAVE",
    "QUICK LOAD",
    "\xB7\x71\xB4\x62\x20\xB9\xA1\xB8\xF7",                          // "Music"
    "\xD2\x61\x89\xC1\xB7\x71\x20\xB9\xA1\xB8\xF7",                  // "Sound effect"
    "TURN " "\x88\x65\x89\x62\x20\xB9\xA1\xB8\xF7",                  // "TURN interval"
    "SCROLL " "\xAD\xA2\x95\xA1\x20\xB9\xA1\xB8\xF7",                // "SCROLL speed"
    "\xB8\xE5\xCA\x81\x20\xC0\xE1\xB7\x71\xA6\x81\xC8\xE1",          // "Restart battle"
    "TITLE" "\x9D\xA1"                                               // "To TITLE"
};
const char kMenuOption[] = "OPTION " "\xB9\xA1\xB8\xF7";             // "OPTION menu"

struct MenuEvent {
    // 0 none, 1 move, 2 left press, 3 right press, 6 move with left held,
    // 7 move with right held, 0x1B/0x4B00/0x4D00/0x4800/0x5000 = key
    int type = 0;
    int x = 0, y = 0;
};
struct MenuPt { int x = 0, y = 0; };
}  // namespace

int CombatII::battle_system_menu_4e0ee(int no_saveload) {
    if (!menustat_tried_) load_menustat();

    // ---- MENUSTAT helpers (12-byte record: +4 xoff, +6 yoff, +8 pixel pointer) ----
    auto ms_get = [&](int idx) -> const SpriteEntry* {
        return menustat_loaded_ ? menustat_.get(idx) : nullptr;
    };
    auto ms_xo = [&](int idx) { const SpriteEntry* s = ms_get(idx); return s ? (int)s->x_offset : 0; };
    auto ms_yo = [&](int idx) { const SpriteEntry* s = ms_get(idx); return s ? (int)s->y_offset : 0; };
    // sub_5F3FD(x, y, record pointer): the caller adds the offsets before passing them.
    auto ms_draw = [&](int idx, int x, int y) {
        const SpriteEntry* s = ms_get(idx);
        if (s && s->pixel_data) { blit_sprite_rle(x, y, s->pixel_data); return; }
        // Fallback when MENUSTAT.SYB is missing: 24x24 box; positions still use offset 0.
        uint8_t* bb = video_.get_backbuffer();
        if (!bb) return;
        const uint8_t col = (idx & 1) ? 0x08 : 0x10;
        for (int yy = std::max(0, y); yy < std::min(SCREEN_H, y + 24); ++yy)
            for (int xx = std::max(0, x); xx < std::min(SCREEN_W, x + 24); ++xx)
                bb[(size_t)yy * SCREEN_W + (size_t)xx] = col;
    };

    MenuPt pos[11];          // dword_FD88C[0..10]: items 0..8 plus slider arrows 9 and 10
    int icon[9] = {};        // word_FD8C2[0..8]

    // sub_4DBDE: icon index array
    auto build_icons = [&]() {
        icon[0] = 0; icon[1] = 1; icon[2] = 2;
        icon[3] = battle_music_on_ ? 5 : 3;
        icon[4] = battle_sfx_on_ ? 6 : 4;
        icon[5] = 7;
        icon[6] = 8 + (battle_instant_camera_ ? 1 : 0);
        icon[7] = 10;
        icon[8] = 11;
    };
    // sub_4DB7E: hit test over a 24x24 area, bounds inclusive
    auto hit_test = [&](int x, int y, int count) -> int {
        for (int i = 0; i < count; ++i) {
            if (x >= pos[i].x && x <= pos[i].x + 0x18 && y >= pos[i].y && y <= pos[i].y + 0x18) return i;
        }
        return -1;
    };
    // sub_4DC75: menu art
    auto draw_menu = [&]() {
        ms_draw(0, ms_xo(0) + 0xA0, ms_yo(0) + 0x24);
        pos[0].x = ms_xo(1) + 0x60;
        pos[0].y = ms_yo(1) + 0x57;
        ms_draw(1, pos[0].x, pos[0].y);
        int esi = 0x80, edi = 0x4C;
        for (int i = 1; i < 9; ++i) {
            const int e = icon[i] * 2 + 1;
            pos[i].x = ms_xo(e) + esi;
            pos[i].y = ms_yo(e) + edi;
            ms_draw(e, pos[i].x, pos[i].y);
            esi += 0x1E;
            if (i == 4) { esi = 0x80; edi += 0x20; }
        }
        ms_draw(31, ms_xo(31) + 0xA0, ms_yo(31) + 0x8C);
    };
    auto draw_label = [&](int cx) {
        const char* s = (cx >= 0 && cx < 9) ? kMenuLabels[cx] : kMenuOption;
        const int x = 0xA0 - 4 * (int)strlen(s);
        draw_text_wrapped(x, 0x85, s);
    };
    auto present = [&]() {
        video_.present();                       // sub_5D41E + sub_5E9E8
        dump_frame_if_requested();
    };

    // ---- event reading (sub_5D22E / sub_61E7E + getch_) ----
    // Keys take priority over mouse events; a move event is produced only when the coordinates
    // changed.
    int last_mx = input_.get_mouse_x(), last_my = input_.get_mouse_y();
    auto poll_event = [&](MenuEvent& ev) -> bool {
        const MouseState& m = input_.get_mouse();
        ev = MenuEvent();
        if (input_.any_getch_key_pressed()) {
            if (input_.is_key_pressed(SDLK_ESCAPE)) ev.type = 0x1B;
            else if (input_.is_key_pressed(SDLK_LEFT)) ev.type = 0x4B00;
            else if (input_.is_key_pressed(SDLK_RIGHT)) ev.type = 0x4D00;
            else if (input_.is_key_pressed(SDLK_UP)) ev.type = 0x4800;
            else if (input_.is_key_pressed(SDLK_DOWN)) ev.type = 0x5000;
            else ev.type = 0x7FFF;              // key without meaning (ignored, cx = -1)
        } else if (m.right_clicked) {
            ev.type = 3; ev.x = m.x; ev.y = m.y;
        } else if (m.left_clicked) {
            ev.type = 2; ev.x = m.x; ev.y = m.y;
        } else if (m.x != last_mx || m.y != last_my) {
            ev.type = m.left_button ? 6 : (m.right_button ? 7 : 1);
            ev.x = m.x; ev.y = m.y;
        }
        last_mx = m.x; last_my = m.y;
        return ev.type != 0;
    };
    // Wait for an event; returns false when the window close was requested.
    auto wait_event = [&](MenuEvent& ev) -> bool {
        while (!should_exit_) {
            input_.update();
            if (input_.quit_requested()) { should_exit_ = true; return false; }
            if (poll_event(ev)) return true;
            SDL_Delay(1);
        }
        return false;
    };

    // ---- sub_4DE0A: TURN interval slider ----
    auto turn_slider = [&]() -> bool {
        pos[9].x = ms_xo(25) + 0x67;  pos[9].y = ms_yo(25) + 0xB2;    // dword_FD8B0
        pos[10].x = ms_xo(27) + 0xD9; pos[10].y = ms_yo(27) + 0xB2;   // dword_FD8B4
        const int edi = 0xB2;
        auto get_value = [&]() { return std::clamp((int)hud_time_divisor_, 0, 255); };    // sub_1A11C
        auto set_value = [&](int v) { hud_time_divisor_ = v; battle_turn_interval_ = v; }; // sub_1A116
        auto draw_panel = [&]() { ms_draw(29, ms_xo(29) + 0xA0, ms_yo(29) + edi); };
        auto draw_ticks = [&](int value) {
            const int limit = value * 3 + 0x73;
            for (int cx = 0x73; cx < limit; cx += 6) ms_draw(30, cx, ms_yo(30) + edi);
        };
        MenuEvent ev;
        for (;;) {
            // loc_4DE61
            draw_panel();
            ms_draw(25, pos[9].x, pos[9].y);
            ms_draw(27, pos[10].x, pos[10].y);
            draw_ticks(get_value());
            present();
            if (!wait_event(ev)) return false;
            if (ev.type == 0x4B00) {                              // left arrow
                if (get_value() > 2) set_value(get_value() - 2);
            } else if (ev.type == 0x4D00) {                       // right arrow
                if (get_value() < 0x1E) set_value(get_value() + 2);
            } else if (ev.type == 2) {                            // left press: repeat every 9 ticks while held
                game_timer_set(9);
                for (;;) {
                    if (game_timer_poll()) {                      // loc_4DF93: only act when the timer expired
                        const MouseState& m = input_.get_mouse();
                        const int hit = hit_test(m.x, m.y, 11);
                        if (hit == 9 || hit == 10) {
                            ms_draw(hit * 2 + 8, pos[hit].x, pos[hit].y);   // pressed arrow
                            present();
                        }
                        if (hit == 9 && get_value() > 2) set_value(get_value() - 2);
                        else if (hit == 10 && get_value() < 0x1E) set_value(get_value() + 2);
                        draw_panel();
                        draw_ticks(get_value());
                        present();
                    }
                    // loc_4E090: read events; 6 (move while held) and idle continue, 8 (release)
                    // ends
                    input_.update();
                    if (input_.quit_requested()) { should_exit_ = true; return false; }
                    if (!input_.is_left_button()) { ev = MenuEvent(); ev.type = 8; break; }
                    SDL_Delay(1);
                }
                last_mx = input_.get_mouse_x(); last_my = input_.get_mouse_y();
            }
            // loc_4E0AE: Esc or a right press ends; otherwise redraw.
            if (ev.type == 0x1B || ev.type == 3) return true;
        }
    };

    // ---- entry (sub_103A8, sub_44D6E(1), sub_44DEC(0)) ----
    weather_tick_enabled_ = false;
    battle_dim_row0_ = true;
    battle_overlay_mode_ = 0;

    bool redraw = true;      // di
    int cx = -1;             // hovered item, also the return value
    MenuEvent ev;
    while (!should_exit_) {
        if (redraw) {        // loc_4E11F: di == 1
            build_icons();
            battle_dim_row0_ = true;                    // show_scenario_condition may have cleared it
            render_battle_frame();                      // sub_454B3
            draw_menu();
            draw_label(cx);
        }
        present();                                      // loc_4E197
        if (!wait_event(ev)) { cx = -1; break; }
        redraw = false;                                 // xor edi, edi

        bool leave = false;
        if (ev.type == 1) {
            // Mouse move: hit test, then label panel and label
            cx = hit_test(ev.x, ev.y, 9);
            ms_draw(31, ms_xo(31) + 0xA0, ms_yo(31) + 0x8C);
            draw_label(cx);
            present();
        } else if (ev.type == 2) {
            cx = hit_test(ev.x, ev.y, 9);
            if (cx >= 0) {
                if (cx != 3 && cx != 4 && cx != 6) {    // sub_4DD83: pressed effect
                    game_timer_set(9);
                    ms_draw(icon[cx] * 2 + 2, pos[cx].x, pos[cx].y);
                    present();
                    wait_game_tick();
                    ms_draw(icon[cx] * 2 + 1, pos[cx].x, pos[cx].y);
                    present();
                }
                switch (cx) {
                    case 0: {                           // sub_49411 mission briefing
                        const bool old_skip = skip_cutscene_;
                        show_scenario_condition();
                        skip_cutscene_ = old_skip;      // keep the flag the port's wait loop may set on Esc from leaking out
                        break;
                    }
                    case 1:                             // QUICK SAVE
                        if (no_saveload == 0) {
                            battle_text_band(deploydata::kStrSaving, false);   // sub_48083("Saving...")
                            wait_info_dismiss(0x5A);                           // sub_499EF(0x5A) inside sub_48083
                            battle_quick_save_4f4ef();                         // sub_4F4EF
                        }
                        break;
                    case 2:                             // QUICK LOAD
                        if (no_saveload == 0) {
                            battle_text_band(deploydata::kStrLoading, false);  // sub_48083("Loading...")
                            wait_info_dismiss(0x5A);                           // sub_499EF(0x5A) inside sub_48083
                            battle_quick_load_4fd25();                         // sub_4FD25 + sub_4A321
                        }
                        break;
                    case 3:                             // music (sub_4F369 / sub_4F3AE)
                        battle_music_on_ = !battle_music_on_;
                        fprintf(stderr, "COMBAT: music %s (word_FDAE8, sub_4E0EE)\n", battle_music_on_ ? "on" : "off");
                        break;
                    case 4:                             // sfx (sub_4F427 / sub_4F450)
                        battle_sfx_on_ = !battle_sfx_on_;
                        fprintf(stderr, "COMBAT: sfx %s (word_FDAE6, sub_4E0EE)\n", battle_sfx_on_ ? "on" : "off");
                        break;
                    case 5:                             // sub_4DE0A TURN interval
                        if (!turn_slider()) { leave = true; cx = -1; }
                        break;
                    case 6:                             // SCROLL speed (sub_10D9A(8/4))
                        battle_instant_camera_ = !battle_instant_camera_;
                        battle_scroll_speed_ = battle_instant_camera_ ? 8 : 4;
                        fprintf(stderr, "COMBAT: scroll speed %d (word_FDAEA, sub_4E0EE)\n", battle_scroll_speed_);
                        break;
                    case 7:                             // restart battle (sub_55EBC, sub_4FD6B, sub_61CFF(1))
                    case 8:                             // to TITLE (sub_4FDD1("EXIT"), sub_4FE28)
                    default:
                        break;
                }
                if (leave) break;
                // loc_4E3FF: cx 2 or 8 ends the menu (the port also returns 7), otherwise full
                // redraw
                if (cx == 2 || cx == 7 || cx == 8) break;
                redraw = true;
            }
        } else {
            cx = -1;                                    // loc_4E412: other events are ignored
        }
        // loc_4E417: Esc or a right press ends the menu (returns -1)
        if (ev.type == 0x1B || ev.type == 3) { cx = -1; break; }
    }

    // ---- exit (sub_44DEC(1), sub_44D6E(0), sub_103A0) ----
    battle_overlay_mode_ = 1;
    battle_dim_row0_ = false;
    weather_tick_enabled_ = true;
    return cx;
}
