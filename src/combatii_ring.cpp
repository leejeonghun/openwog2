// combatii_ring: player ring interface
//   Main ring: sub_4B0A7 / sub_4B0FB / sub_4AC8A / sub_4AD6F / sub_4B767 /
//              sub_4B842 / sub_4B8EB / sub_4B9D7 / sub_4BA7F
//   Sub rings: sub_4C72A (magic), sub_4CDBD (skill), sub_4CAA3 (special),
//              sub_4D06E (AI two-stage), sub_4CE1F (map target select)
//
// Sprite banks (sub_4B0A7 via sub_4AF61, the standard SPR layout: 0x42 count,
// 0x80 data): MAINRING 56 = 14 logical icons, MAGIC 88 = 22, SPECIAL 104 = 26,
// SKILL 72 = 18, AI_CTRL 56 = 14. The descriptor count is file count / 4 and the
// file index is logical id * 4 + frame, so the frame is the low two bits.
// sub_4B0FB composes y = center_y - y_off, so the final start angle 90 deg puts
// the first icon above the unit on screen (top of the ellipse).
// sub_4AA42: [ring+pos+6] = id -> descriptor table + id*37 -> [desc+4] = frame,
// then draws with the [desc+frame+5/0D] offsets and [desc+frame*4+15h] pixels via
// sub_5EECF; the sub_4AB72 second pass overlays the same descriptor with sub_5F3FD.
// sub_4AC8A: +/-12px box hit test around the icon centre, skips [38h] inactive
// entries, updates [ecx+5] on a switch and resets both old and new [desc+4] to 0.
// Returns the layout index, or -1 on a miss.
// sub_4AD6F: advances the hovered ([ecx+5]) descriptor's [desc+4] by +1&3 every
// 30 ticks of sub_102FF inside the sub_4BA7F input loop; only the hover animates.
// Angle and radius animations (FDAEE flag 0/1): sub_4B767 open 276->36 (-16, 15
// frames, start 5.236 rad = 300 deg), sub_4B8EB show 0->36 (+4, 9 frames, start
// 2pi), sub_4B842 close 36->276 (+16, 15 frames, start 90 deg), sub_4B9D7 hide
// 36->0 (-4, 9 frames, start 90 deg). Every step adds +0.1745 rad (10 deg), so
// 5.236 + 15*0.1745 = 2pi + 9*0.1745 = 1.571 rad (90 deg) at the end. Each frame
// runs sfx 0x1F + game_timer(2) + sub_4AEB4 (advance) + sub_4B0FB + sub_45431 +
// sub_5E9E8 + sub_499E4; the port plays the same steps blocking.
// sub_4AE50: F5D8 = 2pi/N (dbl_77E40 = 6.28318530717959). sub_4AEE5 computes
// x = r*cos(a), y = r*sin(a)*0.83 (dbl_77E48) and wraps at +/-2pi (dbl_77E50 /
// dbl_77E58). Final radius r = 36px (FDAEC = 0x24).
// sub_4BA7F: left click on an icon returns its id (var_14 = 1Bh leaves the loop),
// a left click off the ring returns move (-2, 0xFFFFFFFE), and right click or ESC
// (1Bh / 3) returns cancel (-1, 0xFFFFFFFF).
// 4BC2C / 4A8C3 append ids in order, so position i pairs with menu_ids[i]; id 0
// (sword = attack) is the first slot at the final 90 deg angle and the rest follow
// clockwise. Rendering uses the logical id*4+frame sprite (sub_4AA42 looks the
// descriptor array up by the [ring+pos+6] id).
// sub_4D442 dispatch by id: 0 attack, 1..5 weapon, 6 magic, 7 item, 8 wait/recover,
// 9 defend40, 10 guard, 11 info, 12 defend60, 13 AI config (4D06E + [83h] = 1).
// Returns -4 action end / -3 guard end / -2 weapon-magic-item continue / -1 cancel
// pass / 0 back to ring / 1 defend40 / 2 defend60; battle_ring_dispatch matches.
// sub_4C72A (magic.ico, unk_FDA7C) is a three-stage ring over var_8 = 0..2: each
// stage runs sub_4B0F5 -> sub_4BFCA (stage list) -> sub_4AD69 -> sub_4B8EB (stage 1
// also calls sub_4B744 -> sub_4B18F for info) -> "MP %d" text -> sub_4AC28 icons ->
// sub_4BA7F (stores into var_14[stage]) -> sub_4B9D7. Cancel at stage 1 returns -2,
// at a later stage it steps back (var_8--). sub_4BFCA builds ids 1..7 at stage 0,
// {0,8..14} at stage 1 and {0,15..21} at stage 2; sub_4C72A normalizes them to
// a=1..7, b=(id==0?7:id-8), c=(id==0?7:id-15), reads unk_74FA8, requires
// v==1 (mod 3) and returns R = (v-1) + sub_28429(unit, (v-1)/21), or -1.
// The port uses the same 7/8/8 lists, icon ids and table lookup; battle_cast_magic
// (sub_4C605: Wrong/NoTP/NoMP gates) does the final validation.
// sub_4CDBD (skill.ico, unk_FD93E) is a one-stage ring that picks from the sub_4C19D
// list (dx) via sub_4B0F5 -> sub_4AD69 -> sub_4B8EB(ebx=0) -> sub_4BA7F -> sub_4B9D7;
// cancel returns -1. The port uses battle_item_menu_ids(dx) as the ring options.
// The main ring ids follow sub_4BD26's off_4BD0E: weapon ids 1..5 come from WC1..WC5
// ([38h]/[39h]/[3Ah]/[3Bh] plus the 2Fh/30h class case for 5); WC2 adds nothing.
// sub_4CAA3 (special.ico, unk_FDA12) is a one-stage ring over the sub_4C03E list; an empty
// list buzzes sfx 2 and returns -1. sub_4BA7F yields the sub_4C03E menu id (0 or start+i),
// which sub_4C98A takes as si (dec si / si==0), so the port stores those ids, not the icon
// positions, as the ring options.
// sub_4C4F9 builds the AI first-stage ring: [6Ch]==3 -> 1 else 0; wc==2 skips the
// weapon entry, else [6Dh]==1 -> 4, ==2 -> 5, else 3; [6Eh]==1 -> 7, ==4 -> 8, else
// 6; MP>0 adds [80h]==FF -> 9 else 10; wc<6 && wc!=2 adds [82h]==FF -> 11 else 12;
// [83h]!=2 adds 13.
// sub_4D06E is the AI two-stage ring: show stage 1, sub_4BA7F returns an id, then
// stage 2 offers 0..2, 3..5, 6..8, 9..10 or 11..12 and 13 ends the ring. The second
// sub_4BA7F result goes through the off_4D03A switch: 0,1 -> [6Ch]=id*3; 2 -> sub_4CE1F;
// 3,4,5 -> wc>=6 skip, else wc==5 && id>4 -> [6Ch]=5, else [6Dh]=id-3; 6,7,8 ->
// [6Eh]=0/1/4; 9 -> [80h]=FF; 10 -> sub_4C72A then [80h]=R/3; 11 -> [82h]=FF;
// 12 -> sub_4CAA3 then [82h]=si. Choose 13 at stage 1 sets [83h]=0, calls 196D0 and
// returns 1 (manual control); cancel at stage 2 calls 19718 and returns -1.
// sub_4CE1F selects a map target: initialise [71h]=FF, [73h]=0, [6Fh]=FF, then run
// the cursor loop (arrows 4800/4D00/5000/4B00, mouse, '1'/'2', 1Bh/'3'). Confirm (2)
// stores the hovered unit in [73h] or the tile in [6Fh]/[70h]; cancel (1Bh/3) returns 0,
// confirm returns 1. The port uses click confirm and right-click/Esc cancel on the same
// fields, including the sub_11DB2 guard x in [2,W-3], y in [4,H-5].
//
// Deviations from the original:
//   - Sub rings use the same final layout, hit test and icon rules as the main ring;
//     the per-stage text ("MP %d" etc.) is kept as a bottom overlay.
//   - The debug hotkeys (A/M/I/W/G/D/F/N/V/C/digits) are kept as verification
//     shortcuts; the original only uses the mouse ring.
//   - A map click far from the ring enters move mode; the original ignores it.
//   - sub_4CE1F's arrow-key cursor movement is replaced by clicks. Esc cancel matches
//     the original because sub_4CE1F reads 1Bh via getch_; the facing step sub_16357
//     does not read the keyboard, so the port ignores Esc there (see combatii_moveui).
//   - Open/close animations play blocking. Hide stays 9 frames (36->0) and close keeps
//     the original 15-frame fly-out to radius 276 so no wall-clock delay accumulates
//     on frequent paths; G2_AUTOADVANCE consumes the wait immediately.
#include "combatii.h"
#include "combatii_internal.h"

#include <cmath>
#include <cstdio>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// sub_4B0A7: load the five ICO banks. Returns false if any is missing (text fallback).
bool CombatII::ring_load_icons() {
    if (ring_icons_tried_) return ring_icons_loaded_;
    ring_icons_tried_ = true;
    if (std::getenv("G2_NO_RING") != nullptr) {
        fprintf(stderr, "COMBAT: G2_NO_RING set - text fallback (verification only)\n");
        ring_icons_loaded_ = false;
        return false;
    }
    auto load_one = [&](const char* name, SPRParser& out) -> bool {
        std::string p = game_path(name);
        if (!file_exists(p.c_str())) {
            std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", name);
            if (!alt.empty()) p = alt;
        }
        if (!file_exists(p.c_str())) {
            fprintf(stderr, "COMBAT: warning: %s not found - text fallback\n", name);
            return false;
        }
        if (!out.load(p, false)) {
            fprintf(stderr, "COMBAT: warning: %s parse failed: %s - text fallback\n", name, out.error().c_str());
            return false;
        }
        return true;
    };
    const bool ok_main = load_one("MAINRING.ICO", ring_main_);
    const bool ok_magic = load_one("MAGIC.ICO", ring_magic_);
    const bool ok_special = load_one("SPECIAL.ICO", ring_special_);
    const bool ok_skill = load_one("SKILL.ICO", ring_skill_);
    const bool ok_ai = load_one("AI_CTRL.ICO", ring_aictrl_);
    ring_icons_loaded_ = ok_main && ok_magic && ok_special && ok_skill && ok_ai;
    fprintf(stderr, "COMBAT: icons main=%d magic=%d special=%d skill=%d ai=%d (counts %d/%d/%d/%d/%d)\n",
            ok_main ? 1 : 0, ok_magic ? 1 : 0, ok_special ? 1 : 0, ok_skill ? 1 : 0, ok_ai ? 1 : 0,
            ring_main_.count(), ring_magic_.count(), ring_special_.count(),
            ring_skill_.count(), ring_aictrl_.count());
    return ring_icons_loaded_;
}

// sub_4B0FB layout: centre = unit screen centre (sub_135EB), ellipse with
// ry = r*0.83, start angle `start`, step 2pi/N, in battle_menu_ids_ order.
// The original composes x = center_x + x_off, y = center_y - y_off after sub_4AEE5
// computes x = r*cos, y = r*sin*0.83, so the final 90 deg sits at the top.
// The final layout is r=36 at 90 deg; ring_animate_radius draws the intermediate
// frames through this same function. No logging here: it runs every frame.
void CombatII::ring_compute_layout_at(int unit_index, double radius, double start_angle) {
    ring_layout_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    if (battle_menu_ids_.empty()) return;
    const auto& u = units_[(size_t)unit_index];
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);
    ring_center_x_ = cx;
    ring_center_y_ = cy;
    const size_t n = battle_menu_ids_.size();
    const double kStep = (n == 0) ? 0.0 : (2.0 * M_PI / (double)n); // sub_4AE50 F5D8 = 2pi/N
    for (size_t i = 0; i < n; ++i) {
        // sub_4AEE5: x = r*cos(a), y = r*sin(a)*0.83 (dbl_77E48); sub_4B0FB subtracts y.
        const double ang = start_angle + kStep * (double)i;
        const int x = (int)std::lround((double)cx + radius * std::cos(ang));
        const int y = (int)std::lround((double)cy - radius * 0.83 * std::sin(ang));
        ring_layout_.push_back({battle_menu_ids_[i], x, y});
    }
}

void CombatII::ring_compute_layout(int unit_index) {
    const int keep_hover = ring_hover_pos_;
    ring_compute_layout_at(unit_index, 36.0, M_PI / 2.0); // FDAEC = 0x24, final 90 deg
    // Recomputing the final layout keeps the hover: sub_4AC8A's [ecx+5] never clears it.
    if (keep_hover >= 0 && (size_t)keep_hover < ring_layout_.size())
        ring_hover_pos_ = keep_hover;
    else if (!ring_layout_.empty() && keep_hover >= (int)ring_layout_.size())
        ring_hover_pos_ = -1;
}

// sub_4AC8A: +/-12px box around each icon centre; first hit's layout index, else -1.
int CombatII::ring_hit_test(int mx, int my) const {
    for (size_t i = 0; i < ring_layout_.size(); ++i) {
        const int dx = mx - ring_layout_[i].x;
        const int dy = my - ring_layout_[i].y;
        if (dx > -12 && dx < 12 && dy > -12 && dy < 12) return (int)i;
    }
    return -1;
}

// sub_4AC8A hover update: resets the old and new descriptor frames to 0 on a switch.
void CombatII::ring_set_hover(int pos) {
    if (pos == ring_hover_pos_) return;
    ring_hover_pos_ = pos;
    ring_hover_frame_ = 0; // [old_desc+4]=0 + [new_desc+4]=0 (79434/79443)
    ring_hover_last_us_ = timer_now_us();
}

// sub_4AD6F: cycles the hovered descriptor frame +1&3 every 30 ticks (171ms).
// The original calls it from the sub_4BA7F input loop only when sub_102FF fires;
// the port gates it on render/input instead.
void CombatII::ring_hover_advance() {
    if (ring_hover_pos_ < 0 || (size_t)ring_hover_pos_ >= ring_layout_.size()) return;
    if (!ring_icons_loaded_) return;
    const double now = timer_now_us();
    if (auto_advance_) {
        // With G2_AUTOADVANCE, one input frame advances one step without wall clock.
        ring_hover_frame_ = (ring_hover_frame_ + 1) & 3;
        ring_hover_last_us_ = now;
        return;
    }
    static constexpr double kStepUs = 30.0 * 1e6 / (1193182.0 / 1700.0 / 4.0); // 30 game ticks
    if (ring_hover_last_us_ == 0.0) { ring_hover_last_us_ = now; return; }
    if (now - ring_hover_last_us_ < kStepUs) return;
    ring_hover_last_us_ = now;
    // The [desc+frame*4+15h] != 0 gate always passes: every icon has 4 frames.
    ring_hover_frame_ = (ring_hover_frame_ + 1) & 3;
    if (ring_hover_frame_ > 3) ring_hover_frame_ = 0;
}

// sub_4AEB4 + sub_4B0FB loop: advance radius and angle, draw every frame, then wait
// one game_timer(2) tick. Starts at r0/a0, adds dr/da until `target` is reached
// (the original compares against FDAEC). The caller has already set sfx and timer.
// Without icons (or with G2_NO_RING) only the final layout is set.
void CombatII::ring_animate_radius(int unit_index, double r0, double a0, double dr, double da, int target) {
    if (!ring_icons_loaded_ || battle_menu_ids_.empty()) {
        ring_compute_layout(unit_index);
        return;
    }
    double r = r0, a = a0;
    // The original advances sub_4AEB4 before drawing, so r0/a0 is never shown.
    for (int guard = 0; guard < 64; ++guard) {
        r += dr; // word_FDAEC += FDAF4
        a += da; // dbl_FD5E8 += FD5F0 (dbl_FD5E0 advances too; same effect here)
        ring_compute_layout_at(unit_index, r, a);
        render_battle_frame();
        // render_battle_frame draws the ring through render_sprites ->
        // render_player_move_overlay -> render_ring; the overlay text is added here
        // as the equivalent of the original sub_45431 + sub_5E9E8 blit.
        render_overlay();
        video_.present();
        wait_game_tick(); // sub_499E4 (period 2)
        if ((dr < 0 && r <= (double)target) || (dr > 0 && r >= (double)target) ||
            (dr == 0 && guard > 0))
            break;
    }
    ring_compute_layout(unit_index); // snap to r=36, 90 deg (the original sets FDAEE=1)
}

// sub_4B767 (first open) and sub_4B8EB (show): sfx 0x1F + FDAEE=1 + radius/angle
// animation. sub_4D674 uses sub_4B767 (276->36, 15 frames, 300 deg + 15*10) for the
// first show of a turn and sub_4B8EB (0->36, 9 frames, 2pi + 9*10) for later ones;
// ring_opened_this_turn_ tells them apart. Both end at a 90 deg start angle.
void CombatII::ring_open_anim(int unit_index) {
    ring_load_icons();
    static constexpr double kDA = 0.17453292519943295; // 10 deg (push 3FC65718h/4AE74489h)
    const bool first = !ring_opened_this_turn_;
    sfx(0x1F); // sub_4B767/4B8EB: mov eax,1Fh; call sub_4F4CD (once per show)
    ring_open_ = true;
    ring_opened_this_turn_ = true;
    ring_hover_frame_ = 0;
    ring_hover_last_us_ = timer_now_us();
    // The original never opens the main ring while a sub ring is visible (it closes
    // via sub_4B9D7 first); close the sub ring defensively.
    if (first) {
        // sub_4B767: push 276, 5.236 -> step -16 (4AE95 pushes FFFFFFF0h)
        game_timer_set(2); // mov eax,2; call sub_1033B
        ring_animate_radius(unit_index, 276.0, 5.235987755982989, -16.0, kDA, 36);
    } else {
        // sub_4B8EB: push 0, 2pi -> step +4. The sub_4B744 -> sub_4B18F info text is kept.
        game_timer_set(2);
        ring_animate_radius(unit_index, 0.0, 2.0 * M_PI, 4.0, kDA, 36);
    }
    ring_compute_layout(unit_index);
    if (battle_trace_ && !ring_layout_.empty()) {
        const auto& u = units_[(size_t)unit_index];
        fprintf(stderr, "COMBAT: open unit=%d center=(%d,%d) n=%zu first=%d ids=",
                u.id, ring_center_x_, ring_center_y_, ring_layout_.size(), first ? 1 : 0);
        for (auto& r : ring_layout_) fprintf(stderr, "%d@(%d,%d) ", r.id, r.x, r.y);
        fprintf(stderr, "\n");
    }
}

// sub_4B842 (close 36->276) and sub_4B9D7 (hide 36->0): animate the radius, then set
// FDAEE=0. The original distinguishes close at turn end (15 frames, +16/+10 deg) from
// hide between selections (9 frames, -4/+10 deg). ring_close_anim is the hide and
// ring_close_anim_full the turn-end fly-out. Both draw from the saved ring_center_ so
// they still work after battle_player_unit_ becomes -1.
void CombatII::ring_close_anim() {
    if (!ring_open_ || ring_layout_.empty()) {
        ring_open_ = false;
        ring_layout_.clear();
        ring_hover_pos_ = -1;
        ring_hover_frame_ = 0;
        return;
    }
    static constexpr double kDA = 0.17453292519943295;
    sfx(0x1F); // sub_4B842/4B9D7: mov eax,1Fh; call sub_4F4CD
    game_timer_set(2);
    // Snapshot ids, centre and N, then advance only radius and angle so camera drift
    // cannot affect the animation.
    std::vector<int> ids;
    ids.reserve(ring_layout_.size());
    for (auto& p : ring_layout_) ids.push_back(p.id);
    const int cx = ring_center_x_, cy = ring_center_y_;
    const size_t n = ids.size();
    double r = 36.0, a = M_PI / 2.0;
    if (ring_icons_loaded_ && n > 0) {
        const double kStep = 2.0 * M_PI / (double)n;
        for (int guard = 0; guard < 16; ++guard) {
            r += -4.0;
            a += kDA;
            if (r < 0.0) r = 0.0;
            ring_layout_.clear();
            for (size_t i = 0; i < n; ++i) {
                const double ang = a + kStep * (double)i;
                const int x = (int)std::lround((double)cx + r * std::cos(ang));
                const int y = (int)std::lround((double)cy - r * 0.83 * std::sin(ang));
                ring_layout_.push_back({ids[i], x, y});
            }
            render_battle_frame();
            render_overlay();
            video_.present();
            wait_game_tick();
            if (r <= 0.0) break;
        }
    }
    ring_open_ = false;
    ring_layout_.clear();
    ring_hover_pos_ = -1;
    ring_hover_frame_ = 0;
}

// sub_4B842: turn-end close, 36->276 over 15 frames (+16/+10 deg, sfx 0x1F and
// game_timer(2)), as opposed to the 9-frame hide used right after a command.
void CombatII::ring_close_anim_full() {
    if (!ring_open_ || ring_layout_.empty()) {
        ring_open_ = false;
        ring_layout_.clear();
        ring_hover_pos_ = -1;
        ring_hover_frame_ = 0;
        return;
    }
    static constexpr double kDA = 0.17453292519943295;
    sfx(0x1F);
    game_timer_set(2);
    std::vector<int> ids;
    ids.reserve(ring_layout_.size());
    for (auto& p : ring_layout_) ids.push_back(p.id);
    const int cx = ring_center_x_, cy = ring_center_y_;
    const size_t n = ids.size();
    double r = 36.0, a = M_PI / 2.0;
    if (ring_icons_loaded_ && n > 0) {
        const double kStep = 2.0 * M_PI / (double)n;
        for (int guard = 0; guard < 20; ++guard) {
            r += 16.0;
            a += kDA;
            if (r > 276.0) r = 276.0;
            ring_layout_.clear();
            for (size_t i = 0; i < n; ++i) {
                const double ang = a + kStep * (double)i;
                const int x = (int)std::lround((double)cx + r * std::cos(ang));
                const int y = (int)std::lround((double)cy - r * 0.83 * std::sin(ang));
                ring_layout_.push_back({ids[i], x, y});
            }
            render_battle_frame();
            render_overlay();
            video_.present();
            wait_game_tick();
            if (r >= 276.0) break;
        }
    }
    ring_open_ = false;
    ring_layout_.clear();
    ring_hover_pos_ = -1;
    ring_hover_frame_ = 0;
}

// Draw the ring icons and the hover state; nothing is drawn without the ICO banks
// (the text UI is the fallback). File index = logical id*4 + frame (sub_4AA42 /
// sub_4AB72, [desc+4] = frame); only the hovered ([ecx+5]) descriptor uses
// ring_hover_frame_ (0..3, sub_4AD6F). SPR x/y offsets are negative half sizes, so
// blit at centre + offset. The 4-frame icon cycle is the whole hover effect: sub_4AC8A
// only updates [desc+4]/[ecx+5] and none of sub_4AA42 / sub_4AB72 / sub_4AC28 draws a
// box, so there is no 24x24 border here.
void CombatII::render_ring() {
    if (!ring_open_ || ring_layout_.empty()) return;
    if (!ring_icons_loaded_) return;
    ring_hover_advance(); // hover frame cycle every 30 ticks (sub_4AD6F in the 4BA7F loop)
    if (!video_.get_backbuffer()) return;
    for (size_t i = 0; i < ring_layout_.size(); ++i) {
        const int id = ring_layout_[i].id; // logical id, i.e. the menu id itself
        const int frame = ((int)i == ring_hover_pos_) ? (ring_hover_frame_ & 3) : 0;
        const int file_idx = id * 4 + frame; // inverse of sub_4AF61 quot/rem
        const SpriteEntry* sp = (file_idx >= 0 && file_idx < ring_main_.count())
            ? ring_main_.get(file_idx) : nullptr;
        if (!sp || !sp->pixel_data) continue;
        const int bx = ring_layout_[i].x + (int)sp->x_offset;
        const int by = ring_layout_[i].y + (int)sp->y_offset;
        blit_sprite_rle(bx, by, sp->pixel_data);
    }
}

// sub_4BA7F click: on an icon hit store its id in out_menu_id and return true;
// otherwise return false (the caller takes the move (-2) path).
bool CombatII::ring_handle_click(int unit_index, int mx, int my, int& out_menu_id) {
    (void)unit_index;
    const int pos = ring_hit_test(mx, my);
    if (pos < 0) return false;
    ring_set_hover(pos); // includes the frame reset (sub_4AC8A)
    out_menu_id = ring_layout_[(size_t)pos].id;
    if (battle_trace_) {
        fprintf(stderr, "COMBAT: click (%d,%d) -> pos %d id %d frame %d (equivalent to sub_4BA7F)\n",
                mx, my, pos, out_menu_id, ring_hover_frame_);
    }
    return true;
}

// ---- Shared sub-ring helpers ----

const SPRParser* CombatII::ring_sub_icons() const {
    switch (ring_subkind_) {
        case SubRingKind::MagicG:
        case SubRingKind::MagicD:
        case SubRingKind::MagicK:
        case SubRingKind::AIMagic: return &ring_magic_;
        case SubRingKind::Weapon:
        case SubRingKind::AIWeapon: return &ring_special_;
        case SubRingKind::Item: return &ring_skill_;
        case SubRingKind::AI1:
        case SubRingKind::AI2: return &ring_aictrl_;
        default: return nullptr;
    }
}

SPRParser* CombatII::ring_sub_icons_mut() {
    switch (ring_subkind_) {
        case SubRingKind::MagicG:
        case SubRingKind::MagicD:
        case SubRingKind::MagicK:
        case SubRingKind::AIMagic: return &ring_magic_;
        case SubRingKind::Weapon:
        case SubRingKind::AIWeapon: return &ring_special_;
        case SubRingKind::Item: return &ring_skill_;
        case SubRingKind::AI1:
        case SubRingKind::AI2: return &ring_aictrl_;
        default: return nullptr;
    }
}

// sub_4B0FB layout for sub rings: same radius, angle and hit test, only the options
// differ. Final layout is r=36 at 90 deg (top); ring_sub_animate draws the frames.
void CombatII::ring_sub_compute_layout_at(int unit_index, double radius, double start_angle) {
    ring_sub_layout_.clear();
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    std::vector<int> opts;
    switch (ring_subkind_) {
        case SubRingKind::MagicG:
            // sub_4BFCA stage 0: ids 1..7 (all groups; unlearned picks end in Wrong Spell).
            for (int a = 1; a <= 7; ++a) opts.push_back(a);
            break;
        case SubRingKind::MagicD:
            // sub_4BFCA stage 1: id 0 then 8..14 (b = 0->7, 8..14->0..6).
            opts = {0, 8, 9, 10, 11, 12, 13, 14};
            break;
        case SubRingKind::MagicK:
            // sub_4BFCA stage 2: id 0 then 15..21 (c = 0->7, 15..21->0..6).
            opts = {0, 15, 16, 17, 18, 19, 20, 21};
            break;
        case SubRingKind::Item:
            opts = battle_item_options_;
            break;
        case SubRingKind::Weapon:
            // sub_4CAA3 returns the sub_4BA7F menu id (start+i / 0), not the icon position;
            // sub_4C98A indexes its tables with si-1 (or si==0 for the V-weapon slot).
            opts = battle_weapon_options_;
            break;
        case SubRingKind::AI1:
            opts = battle_ai_first_ids(unit_index);
            break;
        case SubRingKind::AI2:
            opts = battle_ai_second_ids(unit_index, ring_sub_ai_first_);
            break;
        case SubRingKind::AIMagic: {
            if (ring_sub_magic_g_ < 0) {
                for (int a = 1; a <= 7; ++a) opts.push_back(a);
            } else if (ring_sub_magic_d_ == -2) {
                opts = {0, 8, 9, 10, 11, 12, 13, 14};
            } else {
                opts = {0, 15, 16, 17, 18, 19, 20, 21};
            }
            break;
        }
        case SubRingKind::AIWeapon:
            opts = battle_weapon_options_;
            break;
        default:
            return;
    }
    if (opts.empty()) return;
    const auto& u = units_[(size_t)unit_index];
    int cx = 0, cy = 0;
    unit_screen_center(u, cx, cy);
    ring_sub_center_x_ = cx;
    ring_sub_center_y_ = cy;
    const size_t n = opts.size();
    const double kStep = (n == 0) ? 0.0 : (2.0 * M_PI / (double)n);
    for (size_t i = 0; i < n; ++i) {
        const double ang = start_angle + kStep * (double)i;
        const int x = (int)std::lround((double)cx + radius * std::cos(ang));
        const int y = (int)std::lround((double)cy - radius * 0.83 * std::sin(ang));
        ring_sub_layout_.push_back({opts[i], x, y});
    }
}

void CombatII::ring_sub_compute_layout(int unit_index) {
    const int keep = ring_sub_hover_;
    // Recomputing the final layout keeps the hover, as on the main ring.
    SubRingKind k = ring_subkind_;
    // ring_sub_compute_layout_at clears the layout, so restore the hover afterwards.
    ring_sub_compute_layout_at(unit_index, 36.0, M_PI / 2.0);
    (void)k;
    if (keep >= 0 && (size_t)keep < ring_sub_layout_.size())
        ring_sub_hover_ = keep;
    else if (!ring_sub_layout_.empty() && keep >= (int)ring_sub_layout_.size())
        ring_sub_hover_ = -1;
}

int CombatII::ring_sub_hit_test(int mx, int my) const {
    for (size_t i = 0; i < ring_sub_layout_.size(); ++i) {
        const int dx = mx - ring_sub_layout_[i].x;
        const int dy = my - ring_sub_layout_[i].y;
        if (dx > -12 && dx < 12 && dy > -12 && dy < 12) return (int)i;
    }
    return -1;
}

void CombatII::ring_sub_set_hover(int pos) {
    if (pos == ring_sub_hover_) return;
    ring_sub_hover_ = pos;
    ring_sub_hover_frame_ = 0;
    ring_sub_hover_last_us_ = timer_now_us();
}

void CombatII::ring_sub_hover_advance() {
    if (ring_sub_hover_ < 0 || (size_t)ring_sub_hover_ >= ring_sub_layout_.size()) return;
    if (!ring_icons_loaded_) return;
    const double now = timer_now_us();
    if (auto_advance_) {
        ring_sub_hover_frame_ = (ring_sub_hover_frame_ + 1) & 3;
        ring_sub_hover_last_us_ = now;
        return;
    }
    static constexpr double kStepUs = 30.0 * 1e6 / (1193182.0 / 1700.0 / 4.0); // 30 game ticks
    if (ring_sub_hover_last_us_ == 0.0) { ring_sub_hover_last_us_ = now; return; }
    if (now - ring_sub_hover_last_us_ < kStepUs) return;
    ring_sub_hover_last_us_ = now;
    ring_sub_hover_frame_ = (ring_sub_hover_frame_ + 1) & 3;
}

// sub_4B8EB loop for sub rings: 0->36 over 9 frames, start 2pi, step +4/+10 deg.
void CombatII::ring_sub_animate(int unit_index) {
    if (!ring_icons_loaded_ || ring_sub_layout_.empty()) {
        ring_sub_compute_layout(unit_index);
        return;
    }
    static constexpr double kDA = 0.17453292519943295;
    double r = 0.0, a = 2.0 * M_PI;
    game_timer_set(2);
    for (int guard = 0; guard < 16; ++guard) {
        r += 4.0;
        a += kDA;
        ring_sub_compute_layout_at(unit_index, r, a);
        render_battle_frame();
        render_overlay();
        video_.present();
        wait_game_tick();
        if (r >= 36.0) break;
    }
    ring_sub_compute_layout(unit_index);
}

// sub_4B8EB for sub rings: sfx 0x1F plus the 0->36 animation. The original runs
// sub_4B8EB -> sub_4BA7F -> sub_4B9D7 for every stage of sub_4C72A / sub_4CDBD /
// sub_4CAA3 / sub_4D06E. Entry hides the main ring (36->0) and then shows the sub
// ring (0->36), the same two-stage animation as the original.
void CombatII::ring_sub_open(SubRingKind kind, int unit_index) {
    ring_load_icons();
    const bool was = (ring_subkind_ == kind && ring_sub_open_);
    // Entering a sub ring hides the main ring (sub_4B9D7, 36->0 over 9 frames).
    if (ring_open_ && !ring_layout_.empty())
        ring_close_anim(); // sfx+hide, FDAEE=0
    else {
        ring_open_ = false;
        ring_layout_.clear();
        ring_hover_pos_ = -1;
    }
    ring_subkind_ = kind;
    ring_sub_open_ = true;
    ring_sub_hover_frame_ = 0;
    ring_sub_hover_last_us_ = timer_now_us();
    if (!was) sfx(0x1F);
    ring_sub_compute_layout(unit_index);
    // Set the final layout first, then animate over it (an empty list takes the buzz path).
    if (!ring_sub_layout_.empty())
        ring_sub_animate(unit_index);
    if (battle_trace_) {
        const auto& u = units_[(size_t)unit_index];
        fprintf(stderr, "COMBAT: open kind=%d unit=%d center=(%d,%d) n=%zu opts=",
                (int)kind, u.id, ring_sub_center_x_, ring_sub_center_y_, ring_sub_layout_.size());
        for (auto& r : ring_sub_layout_) fprintf(stderr, "%d@(%d,%d) ", r.id, r.x, r.y);
        fprintf(stderr, "\n");
    }
}

void CombatII::ring_sub_close() {
    if (ring_sub_open_ && !ring_sub_layout_.empty() && ring_icons_loaded_) {
        // sub_4B9D7 hide, 36->0 over 9 frames, collapsing around the saved centre so it
        // still draws after battle_player_unit_ becomes -1 (no unit lookup).
        static constexpr double kDA = 0.17453292519943295;
        sfx(0x1F);
        game_timer_set(2);
        std::vector<int> ids;
        ids.reserve(ring_sub_layout_.size());
        for (auto& p : ring_sub_layout_) ids.push_back(p.id);
        const int cx = ring_sub_center_x_, cy = ring_sub_center_y_;
        const size_t n = ids.size();
        double r = 36.0, a = M_PI / 2.0;
        if (n > 0) {
            const double kStep = 2.0 * M_PI / (double)n;
            for (int guard = 0; guard < 16; ++guard) {
                r += -4.0;
                a += kDA;
                if (r < 0.0) r = 0.0;
                ring_sub_layout_.clear();
                for (size_t i = 0; i < n; ++i) {
                    const double ang = a + kStep * (double)i;
                    const int x = (int)std::lround((double)cx + r * std::cos(ang));
                    const int y = (int)std::lround((double)cy - r * 0.83 * std::sin(ang));
                    ring_sub_layout_.push_back({ids[i], x, y});
                }
                render_battle_frame();
                render_overlay();
                video_.present();
                wait_game_tick();
                if (r <= 0.0) break;
            }
        }
    }
    ring_sub_open_ = false;
    ring_sub_layout_.clear();
    ring_sub_hover_ = -1;
    ring_sub_hover_frame_ = 0;
    ring_subkind_ = SubRingKind::None;
}

void CombatII::render_sub_ring() {
    if (!ring_sub_open_ || ring_sub_layout_.empty()) return;
    if (!ring_icons_loaded_) return;
    const SPRParser* icons = ring_sub_icons();
    if (!icons) return;
    ring_sub_hover_advance();
    if (!video_.get_backbuffer()) return;
    // Sub-ring icon mapping: sub_4AA42 looks the descriptor up by the logical
    // [ring+pos+6] id, and the file index is logical*4 + frame as on the main ring.
    //   Magic stages use sub_4BFCA's ids directly (1..7, {0,8..14}, {0,15..21});
    //   Item: logical dx; Weapon/AIWeapon: logical entry or si;
    //   AI1/AI2: logical id.
    // Out-of-range entries are skipped, like the off-screen skip in sub_4AA42.
    for (size_t i = 0; i < ring_sub_layout_.size(); ++i) {
        const int opt = ring_sub_layout_[i].id;
        // Magic options already are the original icon ids (sub_4BFCA).
        const int logical = opt;
        // Weapon/AIWeapon options are already sub_4C03E menu ids (0 or start+i), so the icon
        // descriptor id is the option itself (sub_4AA42 looks it up from [ring+pos+6]).
        const int frame = ((int)i == ring_sub_hover_) ? (ring_sub_hover_frame_ & 3) : 0;
        const int file_idx = logical * 4 + frame;
        const SpriteEntry* sp = (file_idx >= 0 && file_idx < icons->count()) ? icons->get(file_idx) : nullptr;
        if (!sp || !sp->pixel_data) continue;
        const int bx = ring_sub_layout_[i].x + (int)sp->x_offset;
        const int by = ring_sub_layout_[i].y + (int)sp->y_offset;
        blit_sprite_rle(bx, by, sp->pixel_data);
    }
    // The sub ring's hover is also just the 4-frame icon cycle (same 4AA42/4AB72 path).
}

// sub_4BA7F click on a sub ring: on a hit store the option value and return true.
bool CombatII::ring_sub_handle_click(int unit_index, int mx, int my, int& out_opt) {
    (void)unit_index;
    const int pos = ring_sub_hit_test(mx, my);
    if (pos < 0) return false;
    ring_sub_set_hover(pos);
    out_opt = ring_sub_layout_[(size_t)pos].id;
    if (battle_trace_) {
        fprintf(stderr, "COMBAT: click (%d,%d) -> pos %d opt %d kind=%d frame %d (equivalent to sub_4BA7F)\n",
                mx, my, pos, out_opt, (int)ring_subkind_, ring_sub_hover_frame_);
    }
    return true;
}

// Learned magic groups (sub_28429 kind >= 0), used to gate the magic menu/AI entry.
// The ring itself follows sub_4BFCA stage 0 (ids 1..7), including unlearned groups.
std::vector<int> CombatII::battle_magic_group_options(int unit_index) const {
    std::vector<int> out;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return out;
    const auto& u = units_[(size_t)unit_index];
    for (int g = 0; g < 7; ++g)
        if (battle_spell_kind(u, g) >= 0) out.push_back(g);
    return out;
}

// sub_4C4F9: AI first-stage ring ids.
std::vector<int> CombatII::battle_ai_first_ids(int unit_index) const {
    std::vector<int> out;
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return out;
    const auto& u = units_[(size_t)unit_index];
    // Cmp [6Ch],3 -> 1, else 0 (sub_4C4F9+1C).
    out.push_back(u.ai_decision_state == 3 ? 1 : 0);
    // Wc==2 skips the weapon entry (sub_4C4F9+45).
    const int wc = weapon_class(u);
    if (wc != 2) {
        // [6Dh]==1 -> 4, ==2 -> 5, else 3.
        if (u.ai_field_6d == 1) out.push_back(4);
        else if (u.ai_field_6d == 2) out.push_back(5);
        else out.push_back(3);
    }
    // [6Eh]==1 -> 7, ==4 -> 8, else 6.
    if (u.ai_field_6e == 1) out.push_back(7);
    else if (u.ai_field_6e == 4) out.push_back(8);
    else out.push_back(6);
    // With MP>0: [80h]==FF -> 9, else 10.
    if (u.combat_mp_50 > 0) out.push_back(u.ai_field_80 == 0xFF ? 9 : 10);
    // With wc<6 && wc!=2: [82h]==FF -> 11, else 12.
    if (wc < 6 && wc != 2) out.push_back(u.ai_mode == (int8_t)0xFF ? 11 : 12);
    // [83h]!=2 adds 13.
    if (u.auto_battle != 2) out.push_back(13);
    return out;
}

// sub_4D06E stage 1 -> stage 2: the stage-2 options offered for each stage-1 id.
std::vector<int> CombatII::battle_ai_second_ids(int unit_index, int first_id) const {
    (void)unit_index;
    if (first_id >= 0 && first_id <= 2) return {0, 1, 2};
    if (first_id >= 3 && first_id <= 5) return {3, 4, 5};
    if (first_id >= 6 && first_id <= 8) return {6, 7, 8};
    if (first_id == 9 || first_id == 10) return {9, 10};
    if (first_id == 11 || first_id == 12) return {11, 12};
    return {};
}

// sub_4D06E off_4D03A: apply a stage-2 id to [6Ch]/[6Dh]/[6Eh]/[80h]/[82h].
// 2 (sub_4CE1F), 10 (sub_4C72A) and 12 (sub_4CAA3) open their own sub state in the
// caller and are not handled here.
void CombatII::battle_ai_apply_second(int unit_index, int second_id) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    const int wc = weapon_class(u);
    if (battle_trace_)
        fprintf(stderr, "COMBAT: apply unit=%d second=%d (equivalent to off_4D03A)\n", u.id, second_id);
    switch (second_id) {
        case 0:
        case 1: {
            // loc_4D2C0: keep [6Ch]==5, else [6Ch]=second*3.
            if (u.ai_decision_state == 5) break;
            u.ai_decision_state = (uint8_t)(second_id * 3);
            if (battle_trace_) fprintf(stderr, "COMBAT: [6Ch]=%u\n", (unsigned)u.ai_decision_state);
            break;
        }
        case 3:
        case 4:
        case 5: {
            // loc_4D2E6: wc>=6 skips, wc==5 && second>4 sets [6Ch]=5, else [6Dh]=second-3.
            if (wc >= 6) break;
            if (wc == 5 && second_id > 4) {
                u.ai_decision_state = 5;
                if (battle_trace_) fprintf(stderr, "COMBAT: wc5 second>4 [6Ch]=5\n");
                break;
            }
            u.ai_field_6d = (uint8_t)(second_id - 3);
            if (battle_trace_) fprintf(stderr, "COMBAT: [6Dh]=%u\n", (unsigned)u.ai_field_6d);
            break;
        }
        case 6: u.ai_field_6e = 0; break; // loc_4D310
        case 7: u.ai_field_6e = 1; break; // loc_4D319
        case 8: u.ai_field_6e = 4; break; // loc_4D322
        case 9: u.ai_field_80 = 0xFF; break; // loc_4D32B
        case 11: u.ai_mode = (int8_t)0xFF; break; // loc_4D36F
        default: break;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: now [6Ch]=%u [6Dh]=%u [6Eh]=%u [80h]=%02X [82h]=%02X\n",
                (unsigned)u.ai_decision_state, (unsigned)u.ai_field_6d, (unsigned)u.ai_field_6e,
                (unsigned)u.ai_field_80, (unsigned)(uint8_t)u.ai_mode);
}

// sub_4CE1F entry: [71h]=FF, [73h]=0, [6Fh]=FF.
void CombatII::battle_ai_enter_target(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    u.ai_watch_sel = 0xFF; // [71h]
    u.ai_watch_id = 0;     // [72h]; the original leaves it alone, cleared here explicitly
    u.ai_target_unit = -1; // [73h]=0 (NULL)
    u.ai_home_x = 0xFF;    // [6Fh]=[71h]
    // [70h] is only written on confirm; the original also leaves it untouched on entry.
    ring_subkind_ = SubRingKind::AITarget;
    ring_sub_open_ = true;
    ring_sub_layout_.clear();
    ring_close_anim();
    if (battle_trace_) fprintf(stderr, "COMBAT: 4CE1F enter unit=%d ([71h]=FF [73h]=0 [6Fh]=FF)\n", u.id);
}

// sub_4CE1F cursor loop: click confirms, right click or Esc cancels; true means the
// input was consumed. Confirm stores the hovered unit (inside the 11DB2 guard) in
// [73h], or the tile in [6Fh]/[70h]. The caller returns to AI1 either way.
bool CombatII::battle_ai_target_input(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    auto& u = units_[(size_t)unit_index];
    const bool esc = input_.is_key_pressed(SDLK_ESCAPE);
    const MouseState& m = input_.get_mouse();
    if (m.right_clicked || esc) {
        // loc_4CFF2 -> loc_4D005: cancel, eax=0.
        if (battle_trace_) fprintf(stderr, "COMBAT: 4CE1F cancel unit=%d (eax=0)\n", u.id);
        ring_subkind_ = SubRingKind::AI1;
        ring_sub_open_ = true;
        ring_sub_compute_layout(unit_index);
        // Cancel returns to stage 1, not stage 2 (original loc_4D0D0 loop); together
        // with the caller's AI2 cancel path, the port also lands on stage 1.
        return true;
    }
    if (!input_.was_left_clicked()) return false;
    const int mx = input_.get_mouse_x(), my = input_.get_mouse_y();
    // Screen to tile (sub_109A3 / sub_109CE).
    int tx = cam_tile_x_ + (mx - cam_fine_x_ - TILE_ANCHOR_X) / TOWN_TILE_W;
    int ty = cam_tile_y_ + (my - cam_fine_y_ - TILE_ANCHOR_Y) / TOWN_TILE_H;
    if (tx < 0 || ty < 0 || tx >= (int)map_w_ || ty >= (int)map_h_) return true;
    // sub_11DB2 guard: outside x in [2,W-3] and y in [4,H-5] yields 0. The original's
    // occupancy test includes the unit itself.
    const bool in_guard = (tx >= 2 && ty >= 4 && tx < (int)map_w_ - 2 && ty < (int)map_h_ - 4);
    int occ = -1;
    if (in_guard) occ = battle_unit_index_at(tx, ty);
    if (occ >= 0 && (size_t)occ < units_.size() && units_[(size_t)occ].active) {
        // loc_4D016 (edi!=0): [73h]=edi, eax=1.
        u.ai_target_unit = occ;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 4CE1F confirm unit=%d target unit=%d id=%d ([73h], eax=1)\n",
                    u.id, occ, units_[(size_t)occ].id);
    } else {
        // loc_4D01F (edi==0): [6Fh]=x, [70h]=y, eax=1.
        u.ai_home_x = (uint8_t)(tx & 0xFF);
        u.ai_home_y = (uint8_t)(ty & 0xFF);
        u.ai_target_unit = -1;
        if (battle_trace_)
            fprintf(stderr, "COMBAT: 4CE1F confirm unit=%d tile=(%d,%d) ([6Fh]/[70h], eax=1)\n",
                    u.id, tx, ty);
    }
    // After confirm, return to stage 1 (original jmp loc_4D0D0).
    ring_subkind_ = SubRingKind::AI1;
    ring_sub_open_ = true;
    ring_sub_compute_layout(unit_index);
    return true;
}

// sub_4D06E entry: centre the camera with 4D582 + 1124A, then show the sub_4C4F9
// stage-1 ring. sub_4D06E itself never writes [83h]; the dispatcher's case 13
// (loc_4D559) sets it to 1 before calling, and the 4D674 prologue path calls with the
// value already >= 1. 4C4F9 omits id 13 only when [83h] == 2, so the caller's value must
// reach the ring as-is.
void CombatII::ring_handle_ai_config(int unit_index) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return;
    auto& u = units_[(size_t)unit_index];
    battle_4d582_center(unit_index); // sub_4D582 + 1124A 1x1 in the sub_4D06E prologue
    if (should_exit_) return;
    ring_sub_magic_g_ = -1;
    ring_sub_magic_d_ = -1;
    ring_sub_ai_first_ = -1;
    ring_sub_open(SubRingKind::AI1, unit_index);
    battle_magic_select_ = false;
    battle_item_select_ = false;
    battle_weapon_select_ = false;
    if (battle_trace_)
        fprintf(stderr, "COMBAT: ai-config unit=%d id=%d [83h]=%u direct=%d (sub_4D06E entry)\n",
                unit_index, u.id, (unsigned)u.auto_battle, battle_ai_config_direct_ ? 1 : 0);
}

// sub_4D442 dispatch. Returns true when this frame's input was consumed; hotkeys and
// ring clicks both call it, and the -4..2 result is handled inside.
bool CombatII::battle_ring_dispatch(int unit_index, int menu_id) {
    if (unit_index < 0 || (size_t)unit_index >= units_.size()) return false;
    auto& u = units_[(size_t)unit_index];
    const int ui = unit_index;
    auto back_to_ring = [&]() {
        battle_attack_cancel();
        battle_move_cancel();
        battle_magic_select_ = false;
        battle_item_select_ = false;
        battle_weapon_select_ = false;
        ring_sub_close();
        if (battle_tp() < 10) {
            if (battle_trace_) fprintf(stderr, "COMBAT: ring TP=%d <10, turn ends\n", battle_tp());
            ring_close_anim();
            battle_player_end_turn(false);
            return;
        }
        battle_player_begin_overlay(ui);
        battle_player_show_menu(ui);
    };
    if (battle_trace_)
        fprintf(stderr, "COMBAT: dispatch unit=%d id=%d (equivalent to sub_4D442)\n", u.id, menu_id);
    switch (menu_id) {
        case 0: // loc_4D45C: sub_1F738; 0 cancel, 2 vanish (TP=0, -4), else 45B4E(40)
            battle_attack_begin(ui);
            return true;
        case 1: case 2: case 3: case 4: case 5: { // loc_4D490: 4CAA3 -> 4C98A, -2 continue
            battle_weapon_options_.clear();
            battle_weapon_list_4c03e(u, battle_weapon_options_);
            if (battle_weapon_options_.empty()) {
                sfx(0x02); // the sub_4CAA3 empty-list buzz
                if (battle_trace_) fprintf(stderr, "COMBAT: weapon submenu empty unit=%d (sub_4CAA3 buzz)\n", u.id);
                back_to_ring();
                return true;
            }
            // Enter the sub_4CAA3 graphic ring (special.ico); waits for a si menu id.
            battle_weapon_select_ = true;
            battle_attack_cancel();
            battle_magic_select_ = false;
            battle_item_select_ = false;
            ring_sub_open(SubRingKind::Weapon, ui);
            return true;
        }
        case 6: { // loc_4D4B1: sub_4C72A -> sub_4C605
            auto groups = battle_magic_group_options(ui);
            if (groups.empty()) return true;
            // Enter the sub_4C72A three-stage ring at the group stage.
            battle_magic_select_ = true;
            battle_magic_pending_r_ = -1;
            battle_attack_cancel();
            battle_item_select_ = false;
            battle_weapon_select_ = false;
            ring_sub_magic_g_ = -1;
            ring_sub_magic_d_ = -1;
            ring_sub_open(SubRingKind::MagicG, ui);
            battle_magic_show_page(ui);
            return true;
        }
        case 7: { // loc_4D4CF: sub_4CDBD -> sub_4CB5E
            battle_item_full_options_ = battle_item_menu_ids(ui);
            if (battle_item_full_options_.empty()) return true;
            battle_item_options_ = battle_item_full_options_;
            battle_item_page_ = 0;
            battle_item_select_ = true;
            battle_item_pending_dx_ = -1;
            battle_attack_cancel();
            battle_magic_select_ = false;
            battle_weapon_select_ = false;
            // Enter the sub_4CDBD graphic ring (skill.ico) with the full list, as the
            // original does; the 7-entry paging stays only for the hotkey fallback.
            ring_sub_open(SubRingKind::Item, ui);
            return true;
        }
        case 8: { // loc_4D4EB: 1A0E9 -> 4D3BA -> 45B4E (all TP)
            battle_wait_heal(ui);
            battle_consume_tp(ui, battle_tp());
            if (battle_trace_) fprintf(stderr, "COMBAT: wait unit=%d\n", u.id);
            back_to_ring();
            return true;
        }
        case 9: { // loc_4D504: 45B4E(40), returns 1
            battle_consume_tp(ui, 0x28);
            battle_used_bits_ = (uint16_t)(battle_used_bits_ | 0x01);
            if (battle_tp_changed()) u.combat_acc57 = 0;
            if (battle_trace_) fprintf(stderr, "COMBAT: defend40 unit=%d\n", u.id);
            back_to_ring();
            return true;
        }
        case 10: { // loc_4D517: [57h] += 20 (cap 500), TP=0, ends with -3
            if (u.combat_acc57 < 500) {
                u.combat_acc57 = (uint16_t)(u.combat_acc57 + 20);
                if (u.combat_acc57 > 500) u.combat_acc57 = 500;
            }
            battle_tp_set(0);
            if (battle_trace_) fprintf(stderr, "COMBAT: guard unit=%d acc57=%u (si=-3)\n", u.id, (unsigned)u.combat_acc57);
            ring_close_anim();
            ring_sub_close();
            battle_player_end_turn(false);
            return true;
        }
        case 11: // loc_4D53B: 493A3, returns 0 -> 4D93E reopens the ring when TP >= var18
            battle_show_info(ui);
            if (should_exit_) return true;
            back_to_ring();
            return true;
        case 12: { // loc_4D546: 45B4E(60), returns 2
            battle_consume_tp(ui, 0x3C);
            battle_used_bits_ = (uint16_t)(battle_used_bits_ | 0x02);
            if (battle_tp_changed()) u.combat_acc57 = 0;
            if (battle_trace_) fprintf(stderr, "COMBAT: defend60 unit=%d\n", u.id);
            back_to_ring();
            return true;
        }
        case 13: // loc_4D559: [83h]=1 -> 4D06E
            u.auto_battle = 1; // mov byte ptr [eax+83h],1
            battle_ai_config_direct_ = false; // dispatcher path: -1 settles through 4DAAF
            ring_handle_ai_config(ui);
            return true;
        default:
            return false;
    }
}
