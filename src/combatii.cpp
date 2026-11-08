// Combatii: core module (entry point, main loop, timer/ISR, camera, frame/UI shell, IPC).
#include "combatii.h"
#include "combatii_internal.h"

// Video/Input are the single instances owned by gene2.cpp, so the window survives module switches.
CombatII::CombatII(Video& video, Input& input) : video_(video), input_(input) {}
CombatII::~CombatII() { shutdown_mouse(); free_battle_map(); }

// ==== Mouse cursor (sub_49B16 MAIN.CUR / sub_49C05 / sub_5D41E) ====
// COMBATII uses a software cursor (mode 2, sub_5D388) built from the first four MAIN.CUR
// records instead of the INT 33h hardware cursor. sub_49C05(1) reads the INT 33h position and
// picks frame 0..3 with x > 0x118 (280) -> +1 and y > 0xA4 (164) -> +2. Each frame is a sword
// pointing inward from a screen corner with the hotspot on that outer corner (COMBATII.EXE
// 0x75014/0x75016): 0=(0,0) 1=(14,0) 2=(0,14) 3=(14,14). sub_49C05(0) is initialisation only
// and always uses frame 0. The port registers the four frames as an SDL hardware cursor like
// CHP and PACK do and re-picks the quadrant on every input pump.
static constexpr int kMainCurHotX[4] = {0, 14, 0, 14};
static constexpr int kMainCurHotY[4] = {0, 0, 14, 14};

bool CombatII::init_mouse() {
    if (mouse_initialized_) return true;
    if (main_cursor_frames_.size() < 4) load_main_cur();   // sub_49B16
    if (main_cursor_frames_.size() < 4) {
        fprintf(stderr, "COMBAT: no MAIN.CUR cursor frames, using the system cursor\n");
        return false;
    }

    video_.cursor_reset();                            // drop the previous module's cursor
    for (int i = 0; i < 4; ++i) {
        CursorFrame fr = main_cursor_frames_[(size_t)i];   // keep the parsed frame for re-init
        fr.hot_x = kMainCurHotX[i];
        fr.hot_y = kMainCurHotY[i];
        cursor_ids_[i] = video_.cursor_register(std::move(fr));
    }
    mouse_initialized_ = true;
    cursor_apply(false);                              // sub_49C05(0): initialise to frame 0
    // Reflect pressed/scale/palette changes and quadrant switches from any wait loop.
    input_.set_poll_hook([this] { cursor_apply(true); });
    video_.cursor_show(true);
    fprintf(stderr, "COMBAT: MAIN.CUR mouse cursor loaded (4 frames, hotspots 0/14)\n");
    return true;
}

void CombatII::shutdown_mouse() {
    if (!mouse_initialized_) return;
    input_.set_poll_hook({});
    video_.cursor_reset();
    for (int& id : cursor_ids_) id = -1;
    mouse_initialized_ = false;
}

// sub_49C05: quadrant=false initialises (always frame 0); true picks the current quadrant.
void CombatII::cursor_apply(bool quadrant) {
    if (!mouse_initialized_) return;
    int f = 0;
    if (quadrant) {
        const MouseState& m = input_.get_mouse();
        if (m.x > 0x118) f += 1;
        if (m.y > 0xA4)  f += 2;
    }
    if (cursor_ids_[f] >= 0) video_.cursor_select(cursor_ids_[f]);
}

std::string CombatII::game_path(const char* filename) const {
    return file_find_case_insensitive(GAME_DATA_DIR, filename);
}

std::string CombatII::scenario_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/SCENARIO", filename);
}

std::string CombatII::cstldata_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CSTLDATA", filename);
}

std::string CombatII::characte_path(const char* filename) const {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CHARACTE", filename);
}

// sub_1124A: centre the camera on tile (tx,ty) so the tile's top-left lands on screen centre
// + 6. sub_11003 clamps the target (x<0 -> 0, x>map_w-12 -> map_w-12, same for y), the existing
// scroll machinery (sub_50339 / sub_5046B) builds the path, and loc_1110A runs it to
// completion, drawing through sub_45577 each iteration until si/di reach 0. Unit screen
// coordinates are read only after that, so dialogue ring and head positions always match the
// camera. Both 0x02 dialogue (sub_482A5) and 0x04 retreat (first record of sub_51449) use it.
// word_FDAEA != 0 takes the 111BE immediate-move + 454B3 path instead (game.cfg option).
void CombatII::camera_center_on(int tx, int ty) {
    int cx = tx - 6, cy = ty - 6;
    if (cx < 0) cx = 0;                       // test ax,ax; jge -> clamp to 0 when x<0
    if (cy < 0) cy = 0;
    int maxx = (int)map_w_ - 12; if (maxx < 0) maxx = 0;
    int maxy = (int)map_h_ - 12; if (maxy < 0) maxy = 0;
    if (cx > maxx) cx = maxx;                 // sub eax,0Ch; jle -> clamp to map-12
    if (cy > maxy) cy = maxy;
    if (cx == cam_tile_x_ && cy == cam_tile_y_) return;
    if (battle_instant_camera_) { // equivalent of sub_111BE (FDAEA!=0, no animation)
        cam_tile_x_ = cx; cam_tile_y_ = cy;
        cam_fine_x_ = CAM_FINE_X_REST; cam_fine_y_ = CAM_FINE_Y_REST;
        render_battle_frame(); // equivalent of sub_454B3
        video_.present();
        return;
    }
    camera_setup(0, cx, cy, 4);               // sub_11003+60: set the target via the scroll machinery
    // loc_1110A loop: run synchronously, one fine step per iteration (wrapping into cam +-1) and
    // one sub_45577 draw. It returns only after si/di reach 0, so head coordinates are relative
    // to the final camera.
    while (camera_scrolling()) {
        camera_update_frame();
        render_battle_frame();
        video_.present();
    }
}

// sub_454D5 dim gate (FA8B6). The original sets FA8B6=1 in the 4B767/4B8EB/4D674 loops
// (44D6E(1)) and in 49411(44D6E(1)); 454D5 then dims between the weather layer and the
// markers/HUD with sub_6104A(0), sub_5ECC7(1), sub_5EDDE(0x10,0x130,4,0xC4), sub_5ECC7(0).
// The ring dims only outside the move and target phases so it never overlaps the move shading
// of sub_1547F.
bool CombatII::battle_ring_dim_active() const {
    if (battle_state_ != 1) return false;
    if (battle_move_phase_ != 0) return false;
    if (battle_atk_phase_ != 0) return false;
    if (battle_player_unit_ < 0 || (size_t)battle_player_unit_ >= units_.size()) return false;
    return ring_open_ || ring_sub_open_;
}

// ---- sub_454B3: map/FPM -> units -> weather ----
// 454B3 = sub_10AEB (map) -> [sub_1547F] -> sub_12BEE (units) -> sub_4512E (weather).
// render_battle_map finishes sub_10AEB + sub_12BEE (towers and units sorted by Y), so the
// weather layer is drawn on top afterwards.
void CombatII::battle_fx_world() {
    video_.clear();
    render_battle_map();                                            // sub_10AEB + sub_12BEE
    render_weather_layer();                                         // sub_4512E
}

// sub_10AEB + sub_12D65: the reduced frame the full-screen dark spells use. sub_12D65 has no
// sub_122C4/sub_12567 pass, so it draws no normal/attacking/water units and no weather - only
// the map, its top tiles and the sub_18605 item/field markers.
void CombatII::battle_fx_world_12d65() {
    video_.clear();
    render_battle_map(true);                                        // sub_10AEB + sub_12D65
}

// sub_276B9 step 2 calls sub_454B3 and then sub_12D65 again, so the top tiles and map effects
// are redrawn over the full unit/weather frame.
void CombatII::battle_fx_world_454b3_12d65() {
    battle_fx_world();                                              // sub_454B3
    render_battle_map(true, true);                                  // sub_12D65 over it
}

// One sub_12D65 frame plus the effect transfer/present steps (12E01 -> 454D5 -> 5E9E8).
void CombatII::battle_fx_present_12d65() {
    battle_hud_time_ = battle_time_;                                // present_cutscene_frame
    battle_fx_world_12d65();
    battle_fx_flush();                                              // sub_12E01
    battle_fx_overlay();                                            // sub_454D5
    render_sprites();                                               // 15D0A family (outside 454D5)
    battle_fx_commit();                                             // sub_5E9E8
    magic_fx_draws_.clear();
}

// As in the original sub_454B3: sub_10AEB does not clear the backbuffer, it only overwrites
// 13x13 tiles, units and weather on top of the previous frame. The border strip the map never
// covers (16px left/right, 4px top/bottom) therefore keeps the previous frame's pixels. The
// equip (sub_48BC1) and detail (sub_48863) screens never call sub_454D5 (HUD/ring/dim), so what
// remains is the combat HUD of the previous ring frame: the status box's black frame and MORALE
// bar.
void CombatII::battle_fx_world_keep() {
    battle_keep_backbuffer_ = true;
    render_battle_map();                                            // sub_10AEB + sub_12BEE (no clear)
    battle_keep_backbuffer_ = false;
    render_weather_layer();                                         // sub_4512E
}

// ---- sub_12E01: flush queued sprites (magic effects included) ----
void CombatII::battle_fx_flush() {
    render_magic_effect_layer();    // draw the queued 5F570/5EE51/5F305 frames over map and units
}

// ---- sub_454D5: dim -> sub_12161 -> sub_44DCC -> sub_13546 -> sub_45431 ----
void CombatII::battle_fx_overlay() {
    // Dim (FA8B6): row-0 LUT over x 16..303, y 4..195 (0x10,0x04,0x120,0xC0). The side bars and
    // the top/bottom HUD fall outside that range and stay bright; markers, HUD and ring are
    // drawn on top of the dim.
    if (battle_dim_row0_ || battle_ring_dim_active())
        render_dialog_dimming(0x10, 0x04, 0x130 - 0x10, 0xC4 - 0x04);
    render_ai_ready_markers();                                      // sub_12161
    render_highlight_units();                                       // sub_44DCC (dword_F9F08)
    // FA8BA != 0 shows every unit's HP, after sub_44DCC and before sub_45431.
    render_all_unit_hp();                                           // sub_13546
    // sub_45431: HUD frame and the black side bars, which must come after map, units and
    // weather so tiles and units poking past the viewport are covered (plus the sub_19FA3/
    // 45BBF status icons).
    render_hud_frame();
}

// ---- sub_5E9E8: present the backbuffer (dialogue and message windows included) ----
void CombatII::battle_fx_commit() {
    render_overlay();
    video_.present();
    dump_frame_if_requested();
}

// sub_454B3 plus the fall-through sub_454D5: one whole frame.
void CombatII::render_battle_frame() {
    battle_fx_world();                                              // sub_454B3 = 10AEB + 12BEE + 4512E
    battle_fx_flush();                                              // sub_12E01
    battle_fx_overlay();                                            // sub_454D5
    render_sprites();          // move/attack queue (15D0A family) - drawn outside 454D5
}

void CombatII::set_ui_text(const std::string& text, bool is_overlay, int portrait,
                           const std::string& speaker_name, int speaker_tx, int speaker_ty,
                           int speaker_unit) {
    ui_text_ = text;
    ui_is_overlay_ = is_overlay;
    ui_portrait_ = portrait;
    ui_speaker_ = speaker_name;
    ui_speaker_tx_ = speaker_tx;
    ui_speaker_ty_ = speaker_ty;
    ui_speaker_unit_ = speaker_unit;
    ui_visible_ = !text.empty();
}

void CombatII::render_overlay() {
    if (ui_visible_) {
        if (ui_is_overlay_) show_overlay_message(ui_text_);
        else show_dialogue_line(ui_text_, ui_portrait_, ui_speaker_, ui_speaker_tx_, ui_speaker_ty_, ui_speaker_unit_);
    }
    // A message window opened by 0x73 is shown separately (the <<Bifrost>> caption in 1-1 etc.).
    // The original redraws it every frame through sub_5E048 from sub_5046B, so it disappears
    // when the window closes.
    if (msg_active_ && !ui_visible_) render_message_window();
}

void CombatII::present_cutscene_frame() {
    // The original cutscene/dialogue/message path also redraws the bottom gauge through
    // sub_45431 -> sub_44EB0 (sub_482A5+11D). The port draws the HUD every frame even while
    // running free, so it only syncs the display time here (see the battle_hud_time_ comment).
    battle_hud_time_ = battle_time_;
    render_battle_frame();
    battle_fx_commit();                                             // sub_5E9E8
}

// ---- sub_50339: set up a camera scroll (non-blocking) ----
// Calling convention: sub_52940 loc_52B24 calls `sub_50339(ecx=D, ebx=C, edx=B, eax=A)`, so
// al = A (mode), dl = B (target X tile), bl = C (target Y tile), cl = D (speed). sub_50339
// stores dl in var_4 and compares it against the low word of dword_BA83C (camera X), and bl
// against its high word (camera Y).
void CombatII::camera_setup(int mode, int x, int y, int speed) {
    if (mode == 1) {
        // sub_50339+A: sub_454B3(B, C) + sub_5E9E8 - move the camera tile and draw at once.
        // The original leaves cam_fine (dword_7001C/BA838) and the frame counters untouched,
        // so a scroll already in flight continues from the new position for its remaining frames.
        cam_tile_x_ = x; cam_tile_y_ = y;
        render_battle_frame();
        video_.present();
        return;
    }
    // loc_5035E..loc_50371: cl (speed) -> subpixels per frame (0->1, 3->8, otherwise x2)
    int cl = speed;
    if (cl == 0) cl = 1;
    else if (cl == 3) cl = 8;
    else cl *= 2;
    // loc_50371..loc_503BB: remaining X frames = |camX - B| * (24 / cl). Zero when B == camX
    // (xor eax,edx); 24/cl is an idiv, so cl > 24 yields 0 and no scroll.
    cam_scroll_frames_x_ = (cam_tile_x_ != x)
        ? (int16_t)(std::abs(cam_tile_x_ - x) * (TOWN_TILE_W / cl)) : 0;
    // loc_503BB..loc_5040A: remaining Y frames = |camY - C| * (16 / cl)
    cam_scroll_frames_y_ = (cam_tile_y_ != y)
        ? (int16_t)(std::abs(cam_tile_y_ - y) * (TOWN_TILE_H / cl)) : 0;
    // loc_5040A redraws the current camera through sub_454B3(camX, camY) with no state change.
    // loc_50440 / loc_5045F: step sign is -cl when the target is greater than the current
    // position, +cl otherwise. World coordinates are cam_tile*24 - cam_fine, so decreasing
    // cam_fine is the forward direction when moving right or down (paired with the wrap in
    // loc_505FE/loc_50670).
    cam_step_x_ = (int16_t)((x > cam_tile_x_) ? -cl : cl);
    cam_step_y_ = (int16_t)((y > cam_tile_y_) ? -cl : cl);
}

bool CombatII::camera_scrolling() const {
    return cam_scroll_frames_x_ != 0 || cam_scroll_frames_y_ != 0;
}

// sub_52940 loc_52A2C: a waypoint is queued only while both the X and Y frame counts are
// non-zero (`cmp word_110584,0; jz loc_52B24` and `cmp word_110582,0; jz loc_52B24`).
bool CombatII::camera_fully_scrolling() const {
    return cam_scroll_frames_x_ != 0 && cam_scroll_frames_y_ != 0;
}

// ---- sub_52B4F: set the message window text ----
// payload = [u16 A][u16 B][B text]. With no window open (dword_110250 == 0) it opens one and
// seeds word_11057C = A as the frame count (loc_52BCF). Otherwise it appends to the next slot
// (unk_FF0BC + 0x10B*idx) and reserves word_10016A[idx] = A frames.
void CombatII::message_window_set(const std::string& text, int frames) {
    MessageWindow w;
    w.text = text;
    w.frames = (frames < 0) ? 0 : frames;
    if (!msg_active_) {
        msg_cur_ = w;
        msg_timer_ = w.frames;
        msg_active_ = true;
        fprintf(stderr, "COMBAT: message window open: %d frames, %zu chars\n",
                msg_timer_, w.text.size());
    } else {
        msg_queue_.push_back(w); // 16 slots in the original (word_10016A); the port uses a deque
        fprintf(stderr, "COMBAT: message window queue[%zu]: %d frames, %zu chars\n",
                msg_queue_.size() - 1, w.frames, w.text.size());
    }
}

// The head of sub_5046B, `sub_5E048(dword_110250, 0x20, 0x24)`: draw the window text at (32,36).
void CombatII::render_message_window() {
    if (!msg_active_ || msg_cur_.text.empty()) return;
    show_overlay_message(msg_cur_.text);
}

// ---- sub_5046B: one step of "advance message windows + scroll camera" per frame ----
// Returns 0 = still busy (a window is open or frames remain -> loc_50695),
//         1 = nothing to do (loc_50593: no scroll, no queue, no window),
//         2 = popped the next waypoint and started a new scroll.
int CombatII::camera_update_frame() {
    // 1) Message window: the head of sub_5046B, run every frame regardless of scrolling.
    //    `mov dx, word_11057C; sub edx, ebx(=1); mov word_11057C, dx`, then advance to the next
    //    window or close when it reaches 0. bx = 1 on entry when a window was open, so this
    //    frame still reports "busy" (0).
    bool msg_was_active = msg_active_;
    if (msg_active_) {
        if (--msg_timer_ <= 0) {
            if (!msg_queue_.empty()) {
                msg_cur_ = msg_queue_.front();
                msg_queue_.pop_front();
                msg_timer_ = msg_cur_.frames;
                fprintf(stderr, "COMBAT: message window advance: %d frames, %zu chars\n",
                        msg_timer_, msg_cur_.text.size());
            } else {
                msg_active_ = false;
                msg_cur_.text.clear();
                msg_timer_ = 0;
                fprintf(stderr, "COMBAT: message window closed\n");
            }
        }
    }
    // 2) One scroll step while frames remain (loc_505B6).
    if (camera_scrolling()) {
        // loc_505B6: X - add the step to dword_7001C and keep fine in (-12, 12]: fine <= -12 ->
        // +24, camX++; fine > 12 -> -24, camX--.
        if (cam_scroll_frames_x_ > 0) {
            cam_fine_x_ = (int16_t)(cam_fine_x_ + cam_step_x_);
            if (cam_fine_x_ <= -12) { cam_fine_x_ += TOWN_TILE_W; cam_tile_x_ += 1; }
            else if (cam_fine_x_ > 12) { cam_fine_x_ -= TOWN_TILE_W; cam_tile_x_ -= 1; }
            cam_scroll_frames_x_ = (int16_t)(cam_scroll_frames_x_ - 1);
        }
        // loc_50626: Y - keep dword_BA838 in (-16, 0]: fine <= -16 -> +16, camY++;
        // fine > 0 -> -16, camY--.
        if (cam_scroll_frames_y_ > 0) {
            cam_fine_y_ = (int16_t)(cam_fine_y_ + cam_step_y_);
            if (cam_fine_y_ <= -(int16_t)TOWN_TILE_H) { cam_fine_y_ += TOWN_TILE_H; cam_tile_y_ += 1; }
            else if (cam_fine_y_ > 0) { cam_fine_y_ -= TOWN_TILE_H; cam_tile_y_ -= 1; }
            cam_scroll_frames_y_ = (int16_t)(cam_scroll_frames_y_ - 1);
        }
        return 0;
    }
    // 3) loc_5050D: once scrolling ends, consume one waypoint (recalls sub_50339, returns 2).
    if (!cam_waypoints_.empty()) {
        CameraWaypoint wp = cam_waypoints_.front();
        cam_waypoints_.pop_front();
        camera_setup(wp.mode, wp.x, wp.y, wp.speed);
        return 2;
    }
    // 4) loc_50593: `cmp bx,1; jz loc_50695` - return 0 when a window was open, even if it
    // just closed.
    return msg_was_active ? 0 : 1;
}

// ---- Game timer (sub_1033B / sub_10364 / sub_499E4 / sub_499EF) ----
// The sound ISR divides the 701.87Hz PIT by 4, giving a 175.47Hz game tick (kTimerHz).
double CombatII::timer_now_us() {
    return (double)SDL_GetPerformanceCounter() * 1e6 / (double)SDL_GetPerformanceFrequency();
}

void CombatII::game_timer_set(int period) {
    // sub_1033B: `mov word_70004,ax; cmp ax,1; jnb; mov word_70004,1`, then reset the counter
    // and flags. The original also clears the ISR tick counter, so the first wait is period ticks.
    timer_period_ = (period < 1) ? 1 : period;
    timer_deadline_us_ = timer_now_us() + (double)timer_period_ * 1e6 / kTimerHz;
}

bool CombatII::game_timer_poll() {
    // Test hook: auto-advance consumes exactly one tick per call - same frame count, no wall clock.
    if (auto_advance_) { timer_deadline_us_ = 0.0; return true; }
    // The original 175.47Hz game ISR (loc_101F4) runs independently of the game timer
    // (sub_1033B/10364): even while a cutscene waits, the weather phase (sub_450F9) advances
    // every 15 and 30 ticks. Spend the tick through the ISR here for the same effect.
    isr_advance();
    const double period_us = (double)timer_period_ * 1e6 / kTimerHz;
    const double now = timer_now_us();
    if (timer_deadline_us_ <= 0.0) timer_deadline_us_ = now + period_us;
    if (now < timer_deadline_us_) return false;      // loc_10267: counter < word_70004
    // sub_10364: consume one tick and clear the counter. The port advances the deadline by one
    // period so frame cost never accumulates into the next wait, matching the average rate the
    // original got from its hardware ISR counter.
    timer_deadline_us_ += period_us;
    // After a long stall, resync to now instead of consuming the backlog at once.
    if (now - timer_deadline_us_ > 4.0 * period_us) timer_deadline_us_ = now + period_us;
    return true;
}

void CombatII::wait_game_tick() {
    // sub_499E4 = `call sub_10364; test ax,ax; jz sub_499E4`: the original polls until one tick
    // elapses. The port sleeps for the remaining time to keep the same pace.
    if (auto_advance_) {
        // Test hook: consume the tick immediately, but still step the weather phase, which
        // derives from the wall-clock ISR, so snow keeps changing during cutscenes.
        timer_deadline_us_ = 0.0;
        if (weather_tick_enabled_) weather_tick_phase();
        return;
    }
    // Wake 1ms early and poll only the last sub-millisecond: SDL_Delay rounds up to whole
    // milliseconds, so sleeping the exact time accumulates 1-2ms of loss per iteration.
    for (;;) {
        if (game_timer_poll()) return;
        const double remain_us = timer_deadline_us_ - timer_now_us();
        if (remain_us > 1200.0) SDL_Delay((Uint32)((remain_us - 800.0) / 1000.0));
        else SDL_Delay(0);
    }
}

bool CombatII::wait_key_or_ticks(int ticks) {
    // sub_499EF: set the period to `ticks` with `sub_1033B(ticks)`, poll keys through
    // sub_5D22E/sub_5D839 and return as soon as one arrives (consumed by getch_). Otherwise loop
    // until sub_10364 returns 1, i.e. ticks game ticks = ticks/175.47 seconds. Dialogue boxes
    // pass 0x2BC (700), about 3.99s of auto-advance.
    game_timer_set(ticks);
    while (true) {
        input_.update();
        if (input_.quit_requested()) { should_exit_ = true; return true; }
        if (input_.is_key_pressed(SDLK_ESCAPE)) { skip_cutscene_ = true; return true; }
        if (input_.was_left_clicked() || input_.is_key_pressed(SDLK_SPACE)
            || input_.is_key_pressed(SDLK_RETURN)) {
            return false;
        }
        if (game_timer_poll()) return false;
        SDL_Delay(auto_advance_ ? 0 : 1);
    }
}

// ---- INT8 frame ticks (loc_101F4 / sub_1031D / sub_102FF) ----
// Game tick 175.47Hz; word_7899E counts ticks and byte_789A8/byte_789A6 are the frame and
// half-tick flags.
void CombatII::isr_advance() {
    if (auto_advance_) return;
    double now = timer_now_us();
    if (isr_base_us_ == 0.0) { isr_base_us_ = now; return; }
    long long ticks = (long long)((now - isr_base_us_) * kTimerHz / 1e6);
    if (ticks <= 0) return;
    isr_base_us_ += (double)ticks * 1e6 / kTimerHz;
    for (long long i = 0; i < ticks; ++i) {
        isr_counter_ += 1;                                   // inc edx / mov word_7899E, dx
        if (isr_counter_ == 0x0F) {                          // cmp dx,0Fh
            // loc_10219: `cmp byte_70006,0; jz` -> call sub_450F9 when non-zero
            if (weather_tick_enabled_) weather_tick_phase();
            isr_frame_flag_ = true;                          // byte_789A8 = 1
        }
        if (isr_counter_ == 0x1E) {                          // cmp word_7899E,1Eh
            if (weather_tick_enabled_) {
                // Same call order as the 30-tick branch of loc_101F4: sub_10A30 -> sub_12065 ->
                // sub_450F9. It runs in the ISR regardless of battle_state_/battle_tick_due, so
                // lava and idle poses keep animating while paused or waiting for the player.
                battle_tile_anim_advance();                  // sub_10A30 (advance tile frames)
                battle_unit_idle_pose_step();                // sub_12065 (idle pose + E518E/E4FB6 phases)
                weather_tick_phase();                        // sub_450F9
            }
            isr_frame_flag_ = true;                          // byte_789A8 = 1
            isr_half_flag_  = true;                          // byte_789A6 = 1
            isr_counter_ = 0;                                // mov word_7899E, si(=0)
        }
        // loc_10289: decrement word_789A4 when positive (cooldown set by sub_1038B), then
        // `call sub_19F5D`.
        if (battle_pause_cooldown_ > 0) battle_pause_cooldown_--;
        battle_pause_isr_hook();
    }
}

bool CombatII::frame_poll() { // sub_1031D - one viewer frame
    if (auto_advance_) {              // test hook: every call is one frame (15 ticks)
        if (weather_tick_enabled_) {
            // Tile advance (sub_10A30) is a 30-tick step, so it runs every other call.
            auto_tile_div_ = (auto_tile_div_ + 1) & 1;
            if (auto_tile_div_ == 0) battle_tile_anim_advance();
            battle_unit_idle_pose_step();                    // sub_12065 (E518E/E4FB6 phases)
            weather_tick_phase();                            // sub_450F9
        }
        return true;
    }
    isr_advance();
    if (!isr_frame_flag_) return false;
    isr_frame_flag_ = false;
    return true;
}

bool CombatII::half_tick_poll() { // sub_102FF - the 30-tick tick of the turn logic
    if (auto_advance_) return true;
    isr_advance();
    if (!isr_half_flag_) return false;
    isr_half_flag_ = false;
    return true;
}

// loc_53026 / loc_53030: the wrap-up loop the VM runs at the end of a body (00 byte).
//   loc_53026: mov eax,9; call sub_1033B     -> game timer period 9
//   loc_53030: call sub_5046B; cmp ax,1; jz loc_5305D
//              sub_5E9E8 (present) + sub_499E4 (one tick) + sub_454B3 (redraw map) -> repeat
// It drains any camera scroll and message window still pending when the script ends, which is
// also where blocks such as 1-1 arrive after their last camera command.
void CombatII::drain_camera_scroll() {
    if (!camera_scrolling() && cam_waypoints_.empty() && !msg_active_) return;
    game_timer_set(9);
    int frames = 0;
    for (;;) {
        int r = camera_update_frame();
        present_cutscene_frame();
        frames++;
        if (r == 1) break;                 // cmp ax,1; jz loc_5305D
        if (should_exit_ || skip_cutscene_) break;
        if (frames > 4096) break;          // safety valve
        wait_game_tick();
    }
    fprintf(stderr, "COMBAT: play camera drain (%d frames, %d/%d remaining)\n",
            frames, (int)cam_scroll_frames_x_, (int)cam_scroll_frames_y_);
}

// Equivalent of sub_49411: show the scenario condition (byte_FDBC0).
// An empty first byte returns 0 without drawing. Otherwise 44D6E(1) + 454B3, then draw the
// header "[ scenario condition ]" at (0x30,0x22) and the body at (0x40,0x37) through 5E048,
// present with 5E9E8, wait 499EF(0xBB8 ticks, about 17.1s, or until a key), then return 1.
// The caller (main_) clears 44D6E(0) when it sees 1. 44D6E(1)=FA8B6 enables the sub_454D5 dim
// (row 0, 0x10,4-0x130,0xC4): the original dims the whole battle background behind this
// display, and the port uses battle_dim_row0_ to dim at the same place (after weather, before
// markers/HUD).
bool CombatII::show_scenario_condition() {
    if (scenario_condition_.empty() || scenario_condition_[0] == '\0') return false;
    battle_dim_row0_ = true; // sub_44D6E(1)
    render_battle_frame();
    battle_dim_row0_ = false; // main_ clears sub_44D6E(0) after the display; the dim is in the frame
    // Header at EXE 0x77D63, "[ scenario condition ] \n" (Johab; 5E048 treats 0x0A as a newline).
    static const std::string kHeader(
        "\x5b\x20\xaf\xa1\x90\x61\x9f\xa1\xb5\xa1\x20\xb9\xa1\x88\xe5\x20\x5d\x20\x0a",
        19);
    draw_text_wrapped(0x30, 0x22, kHeader); // push 22h,30h
    draw_text_wrapped(0x40, 0x37, scenario_condition_); // push 37h,40h
    render_overlay();
    video_.present();
    dump_frame_if_requested(); // G2_DUMP_FRAME capture (the condition screen is dumped too)
    fprintf(stderr, "COMBAT: scenario condition shown (%zu bytes, sub_49411)\n",
            scenario_condition_.size());
    wait_key_or_ticks(0xBB8); // mov eax,0BB8h; call sub_499EF
    if (should_exit_) return true;
    return true;
}

void CombatII::execute_cutscene(){
    // ---- Script VM path (preferred) ----
    // At battle start the original sets dword_F0418(=1) and runs the sub_53523 time trigger for
    // the opening sequence; on 0x12 (sub_52D33) it loads the next .SCE and continues. The port
    // follows the same order but records the commands as an event timeline and replays it. The
    // weather layer is drawn every frame by sub_4512E inside render_battle_frame.
    for (int chain = 0; chain < 4 && !should_exit_; ++chain) {
        std::vector<CombatDialogueEntry> fallback_lines;
        std::string next_scenario;
        bool played_events = false;
        if (script_loaded_) {
            VmResult r;
            // The first sub_19D53 of the original: `inc dword_F0418` (0 -> 1) -> [sequence trigger
            // when dword_74134 >= 0] -> sub_51211 -> sub_53523(1). sub_19652 leaves dword_74134 at
            // -1, so the sequence trigger does not run on this tick. battle_update() continues
            // the clock from 2.
            battle_time_ += 1;
            const uint32_t t = battle_time_;
            bool fired = run_time_trigger(t, r);
            fprintf(stderr, "COMBAT: time trigger(%u) fired=%d lines=%zu events=%zu (ops=%d, ended=%d, aborted=%d) seq=%d\n",
                    t, fired ? 1 : 0, r.lines.size(), r.events.size(),
                    r.instructions, r.ended ? 1 : 0, r.aborted ? 1 : 0, (int)script_seq_);
            if (!r.next_scenario.empty()) {
                script_next_scenario_ = r.next_scenario;
                next_scenario = r.next_scenario;
                fprintf(stderr, "COMBAT: script requests next scenario '%s'\n", next_scenario.c_str());
            }
            if (!r.events.empty()) {
                // Replay the events in order: dialogue and captions wait for input, camera and
                // movement animate.
                play_vm_events(r.events);
                played_events = true;
                // loc_53026: when a block ends on 00 the original VM drains all camera motion and
                // windows before returning, so leftover camera state cannot disturb the next block.
                if (r.end_opcode == 0x00 && !should_exit_ && !skip_cutscene_)
                    drain_camera_scroll();
            } else {
                // With no trigger script, or one that stopped at an unimplemented opcode
                // (0x07/0x08 etc.), scan the body's display blocks to fill in the cutscene.
                std::vector<CombatDialogueEntry> scan;
                int nb = scan_script_blocks(scan);
                fprintf(stderr, "COMBAT: block scan found %d display blocks, %zu lines\n", nb, scan.size());
                if (!scan.empty()) fallback_lines = std::move(scan);
            }
        }
        if (!played_events && !should_exit_) {
            // Nothing came out of the VM: fall back to the scan result, then to the text heuristic.
            std::vector<CombatDialogueEntry>& lines =
                fallback_lines.empty() ? cutscene_dialogues_ : fallback_lines;
            for (size_t i = 0; i < lines.size() && !should_exit_; i++) {
                auto& dlg = lines[i];
                render_battle_frame();
                if (dlg.is_overlay) {
                    show_overlay_message(dlg.text);
                } else {
                    int pid = dlg.portrait_id;
                    if (pid < 0 && !units_.empty())
                        pid = units_[(i * 2) % units_.size()].face; // dialogue portrait from [+10h]
                    show_dialogue_line(dlg.text, pid, dlg.speaker_name, dlg.speaker_x, dlg.speaker_y,
                                       dlg.speaker_unit);
                }
                video_.present();
                if (wait_for_input(200)) return;
            }
        }
        if (should_exit_) return;
        // 0x12 (sub_52D33) is handled inside play_vm_events exactly as in the original:
        // sub_4E4C4(2)/4E622 -> sub_4EC51 -> sub_4ECD7 -> sub_55EBC fade -> sub_4FD6B
        // (contmap.dat) -> sub_61CFF(1). That path sets should_exit_, so the function has already
        // returned by now. Loading, deployment, the scenario condition and the cutscene of the
        // next scenario belong to the fresh COMBATII process MAIN.EXE starts (main_ loc_100EA ->
        // argc==1 contmap.dat path).
        if (!next_scenario.empty() && script_trace_)
            fprintf(stderr, "COMBAT: module ends for next scenario '%s' (sub_61CFF(1))\n",
                    next_scenario.c_str());
        break;
    }
}

// ---- Scripted input injection (G2_INPUT_SCRIPT test harness) ----
// File lines: "AT <tick> KEY <name>" | "AT <tick> CLICK_TILE <x> <y>"
//           | "AT <tick> CLICK_RING <menu_id>" | "AT <tick> CLICK_SCREEN <x> <y>"
//           | "AT <tick> CLICK_SUB <opt>".
// name: a c w m i g d f n v escape space return 1..7.
// KEY is injected through Input::test_inject_key. The a c w m i g d f n v 1..7 keys and the
// ring's return are test-only ring commands that the original never had (ring 4BA7F accepts only
// Esc (1Bh) and the mouse), so they are read only via Input::is_test_key_pressed and a real
// keyboard can never trigger them. escape/space/return are keys the original does read (4BA7F,
// 4A815, 499EF, 5580F) and therefore take the same path as real input. CLICK_RING clicks the
// centre of the given menu_id in the current ring layout (for verifying sub_4BA7F); with a
// sub-ring open it searches the sub-ring layout first (4C72A/4CDBD/4CAA3/4D06E). CLICK_SUB is the
// sub-ring-only alias. A missing layout is logged and the row consumed without a retry. Rows
// whose tick has passed are injected in order, one row per frame.
void CombatII::battle_script_poll() {
    if (!test_script_loaded_) {
        test_script_loaded_ = true;
        if (const char* path = getenv("G2_INPUT_SCRIPT")) {
            FILE* fp = fopen(path, "r");
            if (fp) {
                char line[128];
                while (fgets(line, sizeof(line), fp)) {
                    std::string s(line);
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                    if (!s.empty() && s[0] != '#') test_script_lines_.push_back(s);
                }
                fclose(fp);
                fprintf(stderr, "COMBAT: input script %s (%zu lines)\n", path, test_script_lines_.size());
            }
        }
    }
    if (test_script_pos_ >= test_script_lines_.size()) return;
    const std::string& line = test_script_lines_[test_script_pos_];
    unsigned tick = 0;
    char op[16] = {}, arg[32] = {};
    int cx = 0, cy = 0;
    if (sscanf(line.c_str(), "AT %u %15s %31s", &tick, op, arg) < 2) { test_script_pos_++; return; }
    if (battle_time_ < tick) return;
    // Harness determinism: consume one row per player-turn frame (state 1) only. Without this gate
    // every row is consumed during turn 1 or dropped during AI time, a race that makes the result
    // depend on frame/tick timing. G2_INPUT_SCRIPT only.
    if (battle_state_ != 1) return;
    test_script_pos_++;
    std::string ops(op), ag(arg);
    if (ops == "KEY") {
        static const std::pair<const char*, int> keys[] = {
            {"a", SDLK_a}, {"c", SDLK_c}, {"w", SDLK_w}, {"m", SDLK_m}, {"i", SDLK_i}, {"g", SDLK_g},
            {"d", SDLK_d}, {"f", SDLK_f}, {"n", SDLK_n}, {"v", SDLK_v},
            {"escape", SDLK_ESCAPE}, {"space", SDLK_SPACE}, {"return", SDLK_RETURN},
            {"1", SDLK_1}, {"2", SDLK_2}, {"3", SDLK_3}, {"4", SDLK_4},
            {"5", SDLK_5}, {"6", SDLK_6}, {"7", SDLK_7},
        };
        for (auto& kv : keys)
            if (ag == kv.first) {
                input_.test_inject_key((SDL_Keycode)kv.second);
                fprintf(stderr, "COMBAT: script KEY %s @tick %u\n", ag.c_str(), battle_time_);
                return;
            }
        fprintf(stderr, "COMBAT: script bad KEY %s\n", ag.c_str());
    } else if (ops == "CLICK_TILE") {
        if (sscanf(arg, "%d,%d", &cx, &cy) == 2 ||
            sscanf(line.c_str(), "AT %u CLICK_TILE %d %d", &tick, &cx, &cy) >= 3) {
            const int mx = (cx - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X + TOWN_TILE_W / 2;
            const int my = (cy - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + TOWN_TILE_H / 2;
            input_.test_inject_click(mx, my);
            fprintf(stderr, "COMBAT: script CLICK_TILE (%d,%d)->(%d,%d) @tick %u\n", cx, cy, mx, my, battle_time_);
        } else {
            fprintf(stderr, "COMBAT: script bad CLICK_TILE %s\n", line.c_str());
        }
    } else if (ops == "CLICK_RING" || ops == "CLICK_SUB") {
        int want_id = -1;
        if (sscanf(arg, "%d", &want_id) == 1 ||
            sscanf(line.c_str(), "AT %u CLICK_RING %d", &tick, &want_id) >= 2 ||
            sscanf(line.c_str(), "AT %u CLICK_SUB %d", &tick, &want_id) >= 2) {
            // With a sub-ring open, look there first (4C72A/4CDBD/4CAA3/4D06E).
            int mx = -1, my = -1;
            if (ring_subkind_ != SubRingKind::None && ring_sub_open_) {
                ring_sub_compute_layout(battle_player_unit_);
                for (auto& r : ring_sub_layout_)
                    if (r.id == want_id) { mx = r.x; my = r.y; break; }
                if (mx >= 0) {
                    input_.test_inject_click(mx, my);
                    fprintf(stderr, "COMBAT: script %s id=%d->(%d,%d) subkind=%d @tick %u\n",
                            ops.c_str(), want_id, mx, my, (int)ring_subkind_, battle_time_);
                    return;
                }
            }
            // Otherwise find the icon centre in the layout for the current camera.
            ring_compute_layout(battle_player_unit_);
            for (auto& r : ring_layout_)
                if (r.id == want_id) { mx = r.x; my = r.y; break; }
            if (mx < 0) {
                fprintf(stderr, "COMBAT: script CLICK_RING id=%d not in ring (n=%zu sub=%zu) @tick %u\n",
                        want_id, ring_layout_.size(), ring_sub_layout_.size(), battle_time_);
            } else {
                input_.test_inject_click(mx, my);
                fprintf(stderr, "COMBAT: script CLICK_RING id=%d->(%d,%d) @tick %u\n", want_id, mx, my, battle_time_);
            }
        } else {
            fprintf(stderr, "COMBAT: script bad CLICK_RING %s\n", line.c_str());
        }
    } else if (ops == "CLICK_SCREEN") {
        if (sscanf(arg, "%d,%d", &cx, &cy) == 2 ||
            sscanf(line.c_str(), "AT %u CLICK_SCREEN %d %d", &tick, &cx, &cy) >= 3) {
            input_.test_inject_click(cx, cy);
            fprintf(stderr, "COMBAT: script CLICK_SCREEN (%d,%d) @tick %u\n", cx, cy, battle_time_);
        } else {
            fprintf(stderr, "COMBAT: script bad CLICK_SCREEN %s\n", line.c_str());
        }
    } else {
        fprintf(stderr, "COMBAT: script bad op %s\n", line.c_str());
    }
}

// ---- One battle viewer frame (map -> weather -> dim -> markers -> HUD -> sprites) ----
// Shared by the game-frame path (11.7Hz) and the manual-scroll 60Hz path. The caller presents
// and dumps.
void CombatII::render_battle_viewer() {
    video_.clear();
    render_battle_map();
    render_weather_layer();   // sub_454B3: sub_10AEB -> sub_12BEE -> sub_4512E
    // sub_454D5 dim (FA8B6, row 0, 0x10,4-0x130,0xC4): applied while the ring is shown, after
    // weather and before markers/HUD. Same gate as render_battle_frame(); this inlined path
    // applies it directly.
    if (battle_dim_row0_ || battle_ring_dim_active())
        render_dialog_dimming(0x10, 0x04, 0x130 - 0x10, 0xC4 - 0x04);
    render_ai_ready_markers(); // sub_454D5: sub_12161 (ETC.ICO #0..#7)
    render_highlight_units();  // sub_454D5: sub_44DCC (dword_F9F08 list, after 12161, before 13546)
    render_all_unit_hp();      // sub_454D5: sub_13546 for every unit when FA8BA != 0
    render_hud_frame();        // sub_454D5 -> sub_45431 (HUD + black side bars) + status icons
    render_sprites();
}

bool CombatII::handle_scroll_input(double now_us, const std::function<void()>& draw_frame){
    // Stepping of sub_4A32C (mouse edge) and sub_4A3FF (arrow keys): repeat `sub_1033B(3)`, one
    // direction step (0x10/0x18 into sub_10DA7/10E2F/10ED1/10F5C) and `sub_499E4`. The 10DA7
    // family stops once its accumulator reaches ecx (0x10 = 16px for Y, 0x18 = 24px for X), so one
    // iteration always moves exactly one tile: X 24px per 3 game ticks, Y 16px per 3 ticks
    // (17.1ms, about 1403.8px/s and 935.8px/s). The outer band (x<40/x>280/y<20/y>180; 5580F
    // 94415-94438, 1F19C 22188-22210) triggers the first tile and holding is gated by the inner
    // band (x<=22/x>=298/y<=8/y>=192; 4A32C `cmp [y],8 / [y],0C0h / [x],16h / [x],12Ah` with
    // jle/jge -> keep going, else forced stop). The arrow keys (4A3FF) repeat while held and have
    // no inner band. The port takes discrete per-axis tile steps at the same 3/kTimerHz period.
    // Manual scroll exists only in free wait (5580F), move target (4A483), attack target
    // (1F19C/1F466) and AI target (4CE1F); ring input (4BA7F) and facing (16357) never call it.
    bool scroll_allowed = false;
    if (battle_state_ != 1) {
        scroll_allowed = true; // outside the player's turn (pre-battle, cutscene, AI turn)
    } else if (battle_free_wait_) {
        scroll_allowed = true; // free wait
    } else if (battle_move_phase_ == 1) {
        scroll_allowed = true; // choosing a destination
    } else if (battle_atk_phase_ != 0) {
        scroll_allowed = true; // choosing an attack target
    } else if (ring_subkind_ == SubRingKind::AITarget && ring_sub_open_) {
        scroll_allowed = true; // choosing an AI target
    } else {
        scroll_allowed = false; // ring input, facing, sub-ring
    }
    // Outside those states reset the hold and return false (the original never enters its hold
    // loop there).
    if (!scroll_allowed) {
        scroll_hold_x_ = 0; scroll_hold_y_ = 0;
        scroll_next_x_us_ = 0.0; scroll_next_y_us_ = 0.0;
        scroll_resid_x_ = 0.0; scroll_resid_y_ = 0.0; scroll_last_us_ = now_us;
        return false;
    }
    const uint8_t* keys=SDL_GetKeyboardState(nullptr);
    int key_x = 0, key_y = 0;
    if(keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) key_x+=1;
    if(keys[SDL_SCANCODE_RIGHT]|| keys[SDL_SCANCODE_D]) key_x-=1;
    if(keys[SDL_SCANCODE_UP]   || keys[SDL_SCANCODE_W]) key_y+=1;
    if(keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) key_y-=1;
    if(key_x > 1) key_x = 1; if(key_x < -1) key_x = -1;
    if(key_y > 1) key_y = 1; if(key_y < -1) key_y = -1;
    int mx=input_.get_mouse_x(), my=input_.get_mouse_y();
    int m_outer_x = 0, m_outer_y = 0, m_inner_x = 0, m_inner_y = 0;
    if(mx>=0){
        // Outer band of the original (1F19C 22188-22210, 5580F 94415-94438):
        // x<0x28 / x>0x118 / y<0x14 / y>0xB4.
        if(mx < 0x28) m_outer_x+=1;
        if(mx > 0x118) m_outer_x-=1;
        if(my < 0x14) m_outer_y+=1;
        if(my > 0xB4) m_outer_y-=1;
        // Inner band of the original (4A32C: [y],8 / [y],0C0h / [x],16h / [x],12Ah).
        if(mx <= 0x16) m_inner_x+=1;
        if(mx >= 0x12A) m_inner_x-=1;
        if(my <= 0x08) m_inner_y+=1;
        if(my >= 0xC0) m_inner_y-=1;
    }
    // Per-axis outer trigger: keys first, then the mouse band. The original calls 4A3FF and 4A32C
    // separately; with both active on one axis it would move two tiles, so the port moves one and
    // prefers the key.
    const int outer_x = (key_x != 0) ? key_x : m_outer_x;
    const int outer_y = (key_y != 0) ? key_y : m_outer_y;
    if(outer_x == 0 && outer_y == 0 && scroll_hold_x_ == 0 && scroll_hold_y_ == 0) {
        scroll_last_us_ = now_us;
        return false;
    }
    // One sub_4A32C iteration = `sub_1033B(3)` -> move primitive -> `sub_499E4`, i.e. one tile
    // (24px X / 16px Y) every 3 game ticks (17.1ms) regardless of the speed setting. word_70020
    // (4/8) only sizes the primitive's sub-steps (4px -> 6, 8px -> 3).
    const double kStepUs = 3.0 * 1e6 / kTimerHz;
    const int sub_px = (battle_scroll_speed_ >= 8) ? 8 : 4;
    if(!(now_us >= 0.0)) now_us = 0.0;
    bool drew = false;
    // sub_45577: compose and present. Without a caller-supplied draw, render the whole viewer.
    auto draw_sub_step = [&]() {
        if (draw_frame) draw_frame();
        else { render_battle_viewer(); video_.present(); }
        drew = true;
    };
    // One sub_10DA7/10E2F/10ED1/10F5C step moves sub_px. Crossing a boundary restores that
    // sub-step and returns false (the original's ebx=0: top for 10DA7, left for 10ED1, right and
    // bottom for 10E2F/10F5C). World x = cam_tile*tile_px - cam_fine, so cam_fine grows towards
    // the left and top (same sign convention as sub_50339); the wrap range is
    // fine in (rest - tile_px, rest].
    auto sub_step = [&](int axis, int dir) -> bool {
        const int tile_px = (axis == 0) ? TOWN_TILE_W : TOWN_TILE_H;
        const int rest = (axis == 0) ? CAM_FINE_X_REST : CAM_FINE_Y_REST;
        const int max_tile = std::max(0, (int)((axis == 0) ? map_w_ : map_h_) - 12);
        int& fine = (axis == 0) ? cam_fine_x_ : cam_fine_y_;
        int& tile = (axis == 0) ? cam_tile_x_ : cam_tile_y_;
        const int fine0 = fine, tile0 = tile;
        if(dir > 0) {          // left (4B00h -> 10ED1) / up (4800h -> 10DA7): fine += sub_px with wrap
            fine += sub_px;
            while(fine > rest) { fine -= tile_px; tile -= 1; }
        } else {               // right (4D00h -> 10F5C) / down (5000h -> 10E2F): fine -= sub_px
            fine -= sub_px;
            while(fine <= rest - tile_px) { fine += tile_px; tile += 1; }
        }
        if(tile < 0 || tile > max_tile) { fine = fine0; tile = tile0; return false; }
        return true;
    };
    // One tile = tile_px/sub_px sub-steps, each drawn through sub_45577.
    auto step_tile = [&](int axis, int dir) -> bool {
        const int tile_px = (axis == 0) ? TOWN_TILE_W : TOWN_TILE_H;
        bool moved = false;
        for(int done = 0; done < tile_px; done += sub_px) {
            if(!sub_step(axis, dir)) break;
            draw_sub_step();
            moved = true;
        }
        return moved;
    };
    // ---- X axis ----
    if(scroll_hold_x_ != 0) {
        // Keep holding while the key stays down (4A3FF) or the mouse stays in the inner band
        // (4A32C).
        const bool key_holds = (key_x == scroll_hold_x_);
        const bool mouse_holds = (m_inner_x == scroll_hold_x_);
        if(!(key_holds || mouse_holds)) {
            scroll_hold_x_ = 0; // end the hold (original ebx=0 forced stop); no step this frame
        } else if(now_us >= scroll_next_x_us_) {
            const bool moved = step_tile(0, scroll_hold_x_);
            if(!moved) scroll_hold_x_ = 0; // boundary refused (ebx=0) -> stop; outer cooldown retries
            scroll_next_x_us_ = now_us + kStepUs;
        }
    }
    if(scroll_hold_x_ == 0 && outer_x != 0) {
        if(now_us >= scroll_next_x_us_) {
            const bool moved = step_tile(0, outer_x);
            // Enter hold: keep it through the key when a key started it, otherwise only through
            // the inner band.
            const bool hold_ok = (key_x == outer_x) || (m_inner_x == outer_x);
            if(moved && hold_ok) scroll_hold_x_ = outer_x;
            else scroll_hold_x_ = 0;
            scroll_next_x_us_ = now_us + kStepUs;
        }
    }
    // ---- Y axis ----
    if(scroll_hold_y_ != 0) {
        const bool key_holds = (key_y == scroll_hold_y_);
        const bool mouse_holds = (m_inner_y == scroll_hold_y_);
        if(!(key_holds || mouse_holds)) {
            scroll_hold_y_ = 0;
        } else if(now_us >= scroll_next_y_us_) {
            const bool moved = step_tile(1, scroll_hold_y_);
            if(!moved) scroll_hold_y_ = 0;
            scroll_next_y_us_ = now_us + kStepUs;
        }
    }
    if(scroll_hold_y_ == 0 && outer_y != 0) {
        if(now_us >= scroll_next_y_us_) {
            const bool moved = step_tile(1, outer_y);
            const bool hold_ok = (key_y == outer_y) || (m_inner_y == outer_y);
            if(moved && hold_ok) scroll_hold_y_ = outer_y;
            else scroll_hold_y_ = 0;
            scroll_next_y_us_ = now_us + kStepUs;
        }
    }
    scroll_last_us_ = now_us;
    scroll_resid_x_ = 0.0; scroll_resid_y_ = 0.0;
    return drew; // true only when the camera moved and at least one frame was drawn
}

// sub_4FDD1: `creat_("$", 0x180)` -> `open_("$", 0x101)` -> write -> close. The length written
// is strlen found by `repne scasb` plus 1, NUL included. MAIN.EXE's sub_10014 uses
// `fopen("$", "rt")` + `fscanf(fp, "%s", unk_2052C)` and zeroes the 13-byte buffer first, so the
// NUL does not change the result, but the port writes it to reproduce the original file bytes
// exactly (even a single ' ' is two bytes).
void CombatII::write_dollar(const std::string& cmd){
    std::string dollar=game_path("$");
    FILE* fp=fopen(dollar.c_str(),"wb");
    if(fp){
        fwrite(cmd.c_str(), 1, cmd.size() + 1, fp);   // strlen + NUL
        fclose(fp);
    }
}

// sub_4FD6B: `creat_("contmap.dat", 0x180)` -> `open_(..., 0x201)`, then write the 15-byte name
// and a 1-byte flag. There are three callers: main_ loc_56032 (argv or contmap name, flag 0),
// sub_52D33 (next scenario after 0x12, flag 0) and sub_4E0EE loc_4E3D3 (battle result unk_FDF68,
// flag 0). This file is how COMBATII tells MAIN.EXE which scenario to run next.
void CombatII::write_contmap_dat(const std::string& name, uint8_t flag){
    std::string path=std::string(GAME_DATA_DIR)+"/contmap.dat";
    FILE* fp=fopen(path.c_str(),"wb");
    if(!fp) fp=fopen(game_path("contmap.dat").c_str(),"wb");
    if(!fp){ fprintf(stderr,"COMBAT: warning: contmap.dat write failed\n"); return; }
    char buf15[15]={0};
    size_t len=std::min<size_t>(name.size(),15);
    if(len) memcpy(buf15,name.c_str(),len);
    fwrite(buf15,1,15,fp);
    fwrite(&flag,1,1,fp);
    fclose(fp);
}

// sub_4EC9A: overwrite only byte 0 of BLACKBOX.DAT. The original does
// `open_("BLACKBOX.DAT", 0x201)` (O_WRONLY|O_CREAT, offset 0) then `write_(h,&al,1)`, so it writes
// one byte without truncating and the 37KB save data survives. It is written from two places:
// main_ loc_56032 (`xor eax,eax; call sub_4EC9A` -> 0, battle in progress) and sub_506F3 on a 0x01
// stage clear (`mov eax,1; call sub_4EC9A` -> 1). CHP reads this byte as word_C5D99 (sub_15DB7
// reads a single byte) and treats 1 as "stage cleared" to update chapter progress and the
// completion marker (sub_1088D / loc_21D9E), so without this write chapter progress does not
// advance after 1-1A ends.
void CombatII::write_blackbox_state(uint8_t value){
    std::string path=game_path("BLACKBOX.DAT");
    if(!file_exists(path.c_str())) path=game_path("blackbox.dat");
    bool created=false;
    if(!file_exists(path.c_str())){
        path=std::string(GAME_DATA_DIR)+"/blackbox.dat";   // O_CREAT
        created=true;
    }
    FILE* fp=fopen(path.c_str(), created?"wb":"r+b");
    if(!fp){ fprintf(stderr,"COMBAT: warning: blackbox state write failed (%s)\n",path.c_str()); return; }
    fseek(fp,0,SEEK_SET);
    fwrite(&value,1,1,fp);
    fclose(fp);
    fprintf(stderr,"COMBAT: blackbox state = %u (sub_4EC9A, '%s'%s)\n",
            (unsigned)value, path.c_str(), created?" (created)":"");
}

// Port-only transition (no counterpart in the original): write the command to '$' and end the
// module. The original's normal exit is `mov eax,0FFh; call sub_61CFF`, so the code is 0xFF;
// MAIN.EXE reads '$' on 0FFh and otherwise loops back to loc_100EA.
void CombatII::do_transition(const std::string& cmd){
    pending_command_=cmd;
    write_dollar(cmd);
    return_code_ = 0xFF;
    should_exit_=true;
}

int CombatII::run(const std::string& scenario_param){
    fprintf(stderr,"COMBAT: === Module start (param='%s') ===\n", scenario_param.c_str());
    // Seed for the sub_499BD LCG (combat_rand). Set G2_RANDOM_RNG (presence only, value ignored)
    // for a different sequence each run, like the original's time_->srand_ in 55B59. Without it
    // the seed is the fixed 0x12345678, so runs stay reproducible for verification.
    if (std::getenv("G2_RANDOM_RNG") != nullptr) {
        combat_rng_ = (uint32_t)std::time(nullptr);
        fprintf(stderr, "COMBAT: RNG seed=%u (G2_RANDOM_RNG wall-clock)\n",
                (unsigned)combat_rng_);
    }
    // ---- main_ argc branch, reproduced from COMBATII_code.ASM main_ +0B..loc_56032 ----
    // argc==1 (scenario_param empty): open "contmap.dat" with mode 0x200. On failure write " " to
    // "$" and exit 0xFF at once, before video init, so nothing is drawn. On success read the
    // 15-byte scenario name and the 1-byte flag into unk_1120EC/var_4, unlink the file and recreate
    // it with flag 0 through sub_4FD6B at loc_56032.
    // argc==2 (scenario_param non-empty): copy argv[1] into unk_1120EC two bytes at a time with
    // var_4=0, then recreate contmap.dat the same way at loc_56032.
    // Any other argc exits 0xFF, which cannot happen here: the port has only these two cases.
    std::string effective_param;
    uint8_t var4_flag = 0;
    bool is_argc1 = scenario_param.empty();
    if (is_argc1) {
        std::string contmap_path = game_path("contmap.dat");
        if (!file_exists(contmap_path.c_str())) contmap_path = game_path("CONTMAP.DAT");
        // Case-insensitive fallback search
        if (!file_exists(contmap_path.c_str())) {
            std::string alt = file_find_case_insensitive(GAME_DATA_DIR, "contmap.dat");
            if (!alt.empty() && file_exists(alt.c_str())) contmap_path = alt;
        }
        FILE* fp = file_exists(contmap_path.c_str()) ? fopen(contmap_path.c_str(), "rb") : nullptr;
        if (!fp) {
            // Original: push " "; call sub_4FDD1; mov eax,0FFh; call sub_61CFF
            fprintf(stderr, "COMBAT: warning: contmap.dat not found (argc==1), exit 0xFF without video\n");
            write_dollar(" ");
            return 255;
        }
        char buf15[15] = {0};
        uint8_t flag1 = 0;
        size_t r15 = fread(buf15, 1, 15, fp);
        size_t r1  = fread(&flag1, 1, 1, fp);
        fclose(fp);
        if (r15 != 15 || r1 != 1) {
            fprintf(stderr, "COMBAT: contmap.dat truncated, exit 0xFF\n");
            file_delete(contmap_path.c_str());
            write_dollar(" ");
            return 255;
        }
        // The original unlinks the file right after reading it.
        file_delete(contmap_path.c_str());
        // loc_56032: sub_4FD6B(unk_1120EC, 0) recreates it with flag 0; var_4 stays on the stack.
        write_contmap_dat(std::string(buf15, 15), 0);
        // unk_1120EC -> effective_param (up to NUL; the original copies words, which keeps Johab
        // text intact)
        std::string s;
        s.reserve(15);
        for (int i = 0; i < 15; i++) {
            if (buf15[i] == '\0') break;
            s.push_back(buf15[i]);
            // The original copies words, so a zero first byte ends the name; byte-wise is
            // equivalent.
        }
        // Trim whitespace in case the file was polluted by a text editor. Scenario names never
        // contain spaces, so this matches the original.
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c); }), s.end());
        size_t nul = s.find('\0');
        if (nul != std::string::npos) s.resize(nul);
        if (s.empty()) {
            fprintf(stderr, "COMBAT: contmap.dat empty scenario, exit 0xFF\n");
            write_dollar(" ");
            return 255;
        }
        effective_param = s;
        var4_flag = flag1;
    } else {
        // Argc==2: copy two bytes at a time (CP949-safe). Reproducing the loc_56014 loop is
        // equivalent to byte-wise copying with a doubled NUL check.
        effective_param = scenario_param;
        var4_flag = 0;
        // loc_56032: sub_4FD6B(unk_1120EC, 0) recreates contmap.dat from argv[1]
        write_contmap_dat(effective_param, 0);
    }
    // sub_4EC9A(0) at main_ loc_56032: just before loading the scenario, set BLACKBOX.DAT byte 0
    // to 0 ("battle in progress"). A 0x01 stage clear changes it to 1 and CHP uses that value for
    // chapter progress and completion (sub_1088D / loc_21D9E). A missing file is created like
    // O_CREAT.
    write_blackbox_state(0);
    // Video/Input are the injected single instances (initialised once in gene2.cpp).
    video_.set_mode_13h();
    std::string han = game_path("NEWHAN.FNT"); if(!file_exists(han.c_str())) han=game_path("newhan.fnt");
    std::string asc = game_path("NEWASCII.FNT"); if(!file_exists(asc.c_str())) asc=game_path("newascii.fnt");
    font_.load_hangul(han.c_str());
    font_.load_ascii(asc.c_str());
    font_.set_text_mode(4, 0x1000, 0xFF);

    // ---- Game timer init (sub_102B5 hook in sub_55B59 + PIT 0x6A4 / 4 in sound driver sub_5C665)
    // ----
    // word_70004 starts at 1 (dasm: `word_70004 dw 1`).
    auto_advance_ = (getenv("G2_AUTOADVANCE") != nullptr);
    frame_trace_ = (getenv("G2_FRAME_TRACE") != nullptr);
    battle_trace_ = (getenv("G2_BATTLE_TRACE") != nullptr);
    if (const char* bt = getenv("G2_BATTLE_TICK")) {
        if (!strcmp(bt, "free")) battle_tick_period_ = 0;
        else if (!strcmp(bt, "frame")) battle_tick_period_ = kFrameTickPeriod;
        else if (!strcmp(bt, "half")) battle_tick_period_ = kHalfTickPeriod;
    }
    game_timer_set(1);
    fprintf(stderr, "COMBAT: game timer %0.2fHz (PIT 701.87Hz / sound ISR divide-by-4), period=1, autoadvance=%d\n",
            kTimerHz, auto_advance_ ? 1 : 0);

    std::string param = effective_param;
    // var_4 drives the loc_56066 branch: 1 takes sub_4FD25 (test battle), anything else takes
    // sub_4FCD9 (normal save verification). Neither blocks entry into battle, so the port only
    // logs.
    if (var4_flag == 1) {
        fprintf(stderr, "COMBAT: var_4==1 -> sub_4FD25 path (special/test battle)\n");
    } else {
        // sub_4FCD9: genesis.qsv verification. The original calls sub_4F4EF when the file is
        // missing
        // or its contents differ from unk_FDF68; the port only logs.
        std::string qsv = game_path("genesis.qsv");
        if (!file_exists(qsv.c_str())) qsv = game_path("GENESIS.QSV");
        if (file_exists(qsv.c_str())) {
            // Verification skipped, log only
            fprintf(stderr, "COMBAT: var_4==0 -> sub_4FCD9 genesis.qsv exists, skip\n");
        } else {
            fprintf(stderr, "COMBAT: warning: var_4==0 -> genesis.qsv not found, would call sub_4F4EF (deferred)\n");
        }
    }
    std::string sce_name=param;
    bool has_sce = (sce_name.find(".SCE")!=std::string::npos || sce_name.find(".sce")!=std::string::npos);
    bool has_map = (sce_name.find(".MAP")!=std::string::npos || sce_name.find(".map")!=std::string::npos);
    if(!has_sce && !has_map) sce_name+=".SCE";

    fprintf(stderr,"COMBAT: Resolved scenario '%s' -> '%s'\n", param.c_str(), sce_name.c_str());

    scenario_condition_.clear(); // a MAP-only run has no condition; parse_sce_header fills it for SCE
    battle_deploy_enabled_ = false; sce_zone_grid_.clear();   // a MAP-only run has no deploy zones
    bool loaded=false;
    if(has_map){
        if(load_battle_map(sce_name)) loaded=true;
        else if(load_scenario(sce_name)) loaded=true;
    } else {
        if(load_scenario(sce_name)) loaded=true;
        else {
            std::string map_try=param;
            if(map_try.find(".MAP")==std::string::npos && map_try.find(".map")==std::string::npos) map_try+=".MAP";
            if(load_battle_map(map_try)){ loaded=true; map_info_.map_filename=map_try; }
        }
    }
    if(!loaded){
        fprintf(stderr,"COMBAT: warning: Failed to load scenario %s\n", sce_name.c_str());
        video_.clear();
        write_dollar(" ");
        return 255;
    }

    // Ensure fallback sprites loaded even for MAP-only runs
    if (!battle_sprites_loaded_) load_battle_sprites();

    // sub_55B59: sub_49B16 (MAIN.CUR cursor) -> sub_5D388 (software cursor mode), then the
    // deploy/battle loop. The port registers the four MAIN.CUR frames as an SDL hardware cursor.
    init_mouse();

    // Main_ loc_5606B: after loading (4FD25/4FCD9) and before the battle loop (5580F), show the
    // scenario condition (byte_FDBC0) through sub_49411, ahead of the cutscene (time t=1 trigger).
    // main_ order: sub_55B59 (load, 4ED18 included) -> sub_55518 (deploy) -> sub_137D1 ->
    // 4FD25/4FCD9 -> 49411 -> 5580F.
    battle_deploy_55518();
    if (should_exit_) { video_.clear(); return 0; }
    show_scenario_condition();
    if (should_exit_) { video_.clear(); return 0; }

    execute_cutscene();
    if(should_exit_){ video_.clear(); return 0; }

    bool running=true;
    fprintf(stderr, "COMBAT: cutscene done -> battle viewer loop (tick period %d game ticks%s)\n",
            battle_tick_period_, battle_tick_period_ == 0 ? " = free-run" : "");
    skip_cutscene_ = false;   // clearing the ESC cutscene skip keeps in-battle events playing
    battle_loop_active_ = true;
    battle_tick_next_us_ = 0.0;
    while(running && !should_exit_){
        // Test hook: auto-advance closes the viewer right after the cutscene instead of running the
        // battle loop, which keeps scripted verification fast.
        if (auto_advance_) { do_transition("EXIT"); break; }
        // ---- One sub_5580F iteration (loc_55874..loc_55B46) ----
        //   1) read keys (sub_5D22E/sub_5D839 -> getch_)
        //   2) `call sub_1031D`: draw only when byte_789A8 (frame tick) is set (loc_5589F..loc_558E2)
        //   3) `call sub_19D53`: one battle tick, handling return codes 2/3 (loc_558E2)
        //   4) key handling (loc_5592D..): Space resumes (loc_55B19), ESC opens the system menu
        //      (sub_4E0EE)
        input_.update();
        if(input_.quit_requested()){ do_transition("EXIT"); break; }
        // During the player turn (state 1) Esc is consumed first by ring/move/target cancel
        // (sub_4BA7F/4D96F equivalents); in free wait and other states it opens the system menu.
        // Script injection runs after input polling but before consumption.
        battle_script_poll();
        const bool esc_done = battle_player_input();
        if(!esc_done && input_.is_key_pressed(SDLK_ESCAPE)){
            const int mr = battle_system_menu_4e0ee();
            if (mr == 8) { do_transition("EXIT"); break; }   // back to TITLE (loc_4E3F0)
            if (mr == 7) {                                   // restart the battle (loc_4E3D3)
                std::string restart = current_scenario_name_;
                if (restart.empty()) restart = map_info_.map_filename;
                if (restart.empty()) restart = effective_param;
                write_contmap_dat(restart, 0);               // sub_4FD6B(unk_FDF68, 0)
                return_code_ = 1;                            // sub_61CFF(1): MAIN restarts combatII
                should_exit_ = true;
                break;
            }
            // 2 = quick load done (state already swapped), -1 = cancelled; both continue the loop.
        }
        // ---- loc_5589F: `call sub_1031D; test ax,ax; jz loc_558E2` ----
        // Draw only on frames where byte_789A8 is set (15 game ticks = 85.5ms = 11.7Hz):
        // sub_5D570 (viewport) -> sub_4A210 (sprites) -> sub_454B3 (camera, which runs sub_5046B:
        // message window advance + waypoint queue) -> sub_4A106 -> ... -> sub_5E9E8 (present).
        if (frame_poll()) {
            if (frame_trace_) {
                static int ft_n = 0;
                static double ft_t0 = 0.0;
                double now = timer_now_us();
                if (ft_t0 == 0.0) ft_t0 = now;
                fprintf(stderr, "COMBAT: [frame %d] t=%.1fms battle_time=%u cam=(%d,%d,%d,%d)\n",
                        ft_n++, (now - ft_t0) / 1000.0, battle_time_,
                        cam_tile_x_, cam_fine_x_, cam_tile_y_, cam_fine_y_);
            }
            bool manual_scroll = handle_scroll_input(timer_now_us());
            if (!manual_scroll) camera_update_frame();
            render_battle_viewer();
            video_.present();
            // Verification: dump only player-wait (state 1) frames so move-range shading and lava
            // animation can be compared in one scene. With no input the camera centres on the first
            // frame and then drifts because the offscreen mouse (0,0) triggers edge scrolling, so
            // derive cam/fine when analysing.
            if (battle_state_ == 1) dump_frame_if_requested();
        } else {
            // Manual scroll (sub_4A32C): one tile every 3 game ticks in 4/8px sub-steps, each drawn
            // and presented as one sub_45577 frame inside handle_scroll_input. Nothing is drawn
            // when
            // no step happened (waiting for the period or a refused boundary).
            const bool scrolled = handle_scroll_input(timer_now_us());
            if (frame_trace_ && scrolled) {
                static double st_t0 = 0.0;
                double now = timer_now_us();
                if (st_t0 == 0.0) st_t0 = now;
                fprintf(stderr, "COMBAT: [scroll] t=%.1fms battle_time=%u cam=(%d,%d,%d,%d)\n",
                        (now - st_t0) / 1000.0, battle_time_,
                        cam_tile_x_, cam_fine_x_, cam_tile_y_, cam_fine_y_);
            }
            if (!scrolled && battle_tick_period_ != 0) {
                // The original spins as fast as possible (repeating sub_19D53 without drawing), but
                // the port yields the CPU. SDL_Delay(1) can sleep up to 15.6ms on Windows depending
                // on timer resolution, which would break the 3-tick (17.1ms) scroll period, so
                // sleep
                // only until the next step is due.
                double wait_us = 4000.0;
                if (scroll_next_x_us_ > 0.0 || scroll_next_y_us_ > 0.0) {
                    const double n = timer_now_us();
                    wait_us = 1e18;
                    if (scroll_next_x_us_ > 0.0) wait_us = std::max(0.0, scroll_next_x_us_ - n);
                    if (scroll_next_y_us_ > 0.0) wait_us = std::max(0.0, std::min(wait_us, scroll_next_y_us_ - n));
                }
                if (wait_us < 2000.0) SDL_Delay(0);
                else SDL_Delay((Uint32)std::min(wait_us / 1000.0 - 1.0, 4.0));
            }
        }
        // ---- loc_558E2: `call sub_19D53` ----
        // With F042E != 0 (paused or waiting for input) the original also returns 1 immediately, so
        // it is called without a gate. Otherwise the port's pacing (battle_tick_due) sets the rate.
        int bret = 1;
        if (battle_state_ != 0) {
            battle_tick_next_us_ = 0.0;                    // do not accumulate missed ticks after resuming
            bret = battle_update();
        } else if (battle_tick_due()) {
            bret = battle_update();
        }
        if (should_exit_) break;
        if (bret == 2 || bret == 3) {
            // loc_558E2: 2 -> `xor ecx,ecx` (reset the unit round-robin index), 3 -> re-read the
            // mouse position (sub_5CEE3...).
        }
        // loc_55B19: Space resumes when F042E != 0 and the cooldown has expired. The original only
        // handles it in the free loop (5580F); the blocking ring loop (4BA7F) never reads Space.
        // The
        // port ignores it during ring, move and attack targeting and resumes only in free wait or
        // while paused with the ring closed.
        if (input_.is_key_pressed(SDLK_SPACE) &&
            !ring_open_ && !ring_sub_open_ && battle_move_phase_ == 0 && battle_atk_phase_ == 0)
            battle_resume_key();
    }
    battle_loop_active_ = false;
    video_.clear();
    if(!pending_command_.empty()) fprintf(stderr,"COMBAT: Transition -> %s\n", pending_command_.c_str());
    else write_dollar(" ");
    fprintf(stderr,"COMBAT: === Module end ===\n");
    return 0;
}
