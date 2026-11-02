#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "video.h"
#include "spr_parser.h"
#include "input.h"
#include "font.h"
#include "palette.h"
#include "data_structures.h"

class CHP {
public:
    // Video/Input are the single instances owned by gene2.cpp.
    CHP(Video& video, Input& input);
    ~CHP();

    // ---- main_ (0x2353E): entry point mirroring the original argv model ----
    //   argv1 = argv[1][0]
    //     'p'/'P': value MAIN.EXE passes after combat/scenario via spawnl("chp.exe", "p", NULL)
    //              (main_+1D8: cmp cl,70h / cmp cl,50h)
    //     't'/'T': chapter generator tool mode (main_+393: byte_80301 = 3)
    //     's'/'S': argc == 3 path; argv2 builds "\chapdata\<N>.chp" and enters sub_232A4
    //              (main_+23: argc == 3 without 's'/'S' returns to DOS)
    //     digit  : port convenience, treated like 's <number>'
    //   argv2 = argv[2] (number of "\chapdata\<argv2>.chp"); MAIN.EXE passes NULL.
    int run(const char* argv1, const char* argv2);

    // ---- sub_1D202: world map absolute coords -> perspective-projected screen coords ----
    void map_to_screen(int16_t map_x, int16_t map_y, int& screen_x, int& screen_y) const;

private:
    Video& video_;
    Input& input_;
    Font font_;
    GameState state_;
    // Sticky window-close (X) flag: run() unwinds and returns a normal exit code.
    bool dos_exit_ = false;

    // ---- Mouse cursor (sub_288B9 + user handler loc_28AB0, sprites off_80633 / off_806FB) ----
    // The original INT 33h handler drew a software cursor into VRAM on every event (pressed
    // sprite when the button state != 0, no hotspot, top-left at (x, y)). The port reads both
    // frames from CHP.EXE VA 0x80633 and shows them through the Video hardware cursor;
    // scaling/palette/pressed tracking live in video.cpp.
    void init_mouse();
    void shutdown_mouse();   // sub_28C27 (driver reset): restore the system cursor
    int  cursor_normal_ = -1;

    void load_config();
    void load_blackbox();
    bool load_contibak_file(const std::string& filepath);
    void load_chapter_data(int chapter_id);
    void load_script_tables(int mode);   // sub_112E0: SCRIPT1~3.TBL loader

    // ---- Point and path updates (sub_22C11, sub_22562, sub_2213E, sub_2233E, sub_2288C) ----
    void reset_and_update_points(int mode); // sub_22C11
    void update_point_activation();         // sub_22562
    void update_path_activation();          // sub_2213E
    void deactivate_paths();                // sub_2233E
    void deactivate_points();               // sub_2288C
    void sync_point_kind();                 // sub_220B3 (current point kind shadow: +0x3F / +0x40)

    // ---- Display ----
    void init_display();
    void load_fonts();
    void init_audio();
    void init_camera();
    void update_camera();

    // ---- World map rendering ----
    void render_world_map();
    void render_map_and_points();  // sub_21C58: map + points only (no party/UI overlay)
    void render_perspective_map(); // sub_1CFEB
    void render_point_sprites();   // loc_1F0DC
    void render_party_sprite();
    void render_ui_overlay();
    int  check_point_click(int mx, int my); // loc_1F35B (unused: replaced by pick_point_at)

    // sub_1FC5D: chapter name banner shown on chapter entry.
    void show_chapter_title_banner();
    bool load_banner_box_sprite();  // load unk_809A4 (CHP.EXE VA) at runtime
    void load_extracted_sprites();  // dialog frame / shade mask / party sprite (CHP.EXE VA)

    // sub_22C8E (first half): chapter entry screen transition.
    void zoom_out_effect();

    // sub_204A7 (first half): point entry transition + fade out + sound effect.
    void zoom_in_effect();

    // ---- Event system ----
    int  check_event_triggers(int mode);  // sub_10010: scan 20 triggers, execute matching
    int  execute_event_script(int trigger_id);  // sub_2BDFB: VM interpreter
    void enter_point(int param);          // sub_20912: town/battle/jump entry
    void check_chapter_clear();           // sub_1088D: victory/objective check
    void handle_chapter_reward();         // sub_10975: chapter reward / team cleanup (from sub_232A4)
    bool check_time_limit();              // sub_22A5E: timeout -> TIME OVER + GAMEOVER + EXIT, returns true
    void restore_party_hp_mp();           // sub_20912 loc_20B23 / sub_21476 loc_2184A: restore party copy HP/MP

    // ---- Module transition ----
    void do_transition(const char* command); // sub_204A7: save + write $ + exit
    void world_map_loop();              // sub_21476: main world map event loop
    void chapter_menu();                // sub_232A4: chapter entry point (new game / 's <N>' path)
    void resume_from_save(bool from_load_flag); // main_ loc_237E0: resume / return after combat
    void chapter_menu_tool();
    void transition_exit(const char* cmd);

    // ---- sub_10C6D / sub_10ADF: chapter (quest) selection screen ----
    // Both callers show the same screen:
    //   mode=0: main_+237 (0x23775), after "START" detection (new game) and sub_15DB7;
    //           builds "<number>.chp" from the chosen chapter.
    //   mode=1: sub_22C8E+1B2 (0x22E56), next chapter selection after a chapter clear.
    //           Returns 0x270F when no quest is in progress (quest_state == 1), which is the
    //           entry condition for the ending (sub_33C99).
    // Returns a 1-based chapter number, or 0x270F (no quest left = ending).
    int  show_chapter_select(int mode);                                    // sub_10C6D
    void chapter_select_compose_page(uint8_t* dst, int page);               // sub_10ADF
    void chapter_select_scroll(int dir, int page, std::vector<uint8_t>& pages,
                               const std::vector<uint8_t>& bg);            // loc_10E85~loc_11016

    // ---- sub_22C8E loc_22E1B: chapter end -> next chapter selection / ending ----
    // Called at the top of each world_map_loop frame when byte_523C0
    // (state_.chapter_clear) == 1. Returns true when the next chapter was selected
    // (next_chapter_num_), false when the ending path was entered ("EXIT" queued).
    bool handle_chapter_end();
    int  next_chapter_num_ = 0;      // N of the "\chapdata\<N>.chp" rebuilt by sub_22C8E
    bool next_chapter_pending_ = false;

    // ==== sub_33C99: ending (staff roll) ====
    //   space3.xcp -> unk_DA370, space.xcp/space2.xcp -> dword_C5C5C, STAFF.SPR (24 frames)
    //   into slot 8 and drawn over the two scrolling star fields, then TALK.TBL 0x79A
    //   (narration) and 0x79B..0x7A3 (9 lines) over oddy.xcp, then fade out. The caller
    //   (sub_22C8E) exits with "EXIT".
    void run_ending();
    bool ending_load_assets();                                       // sub_1D26C(8) + sub_2DF77/sub_2DE6F
    void perspective_raster(uint8_t* dest, const std::vector<uint8_t>& buf, long base_off); // sub_1CFEB
    bool load_xcp_into(const char* name, int w, int h, std::vector<uint8_t>& dst, bool with_palette);
    SPRParser ending_sprites_;                   // unk_9C722 slot 8 = STAFF.SPR (24 frames)
    std::vector<uint8_t> ending_buf_;            // dword_C5C5C (0xA8750 bytes) - space/space2
    std::vector<uint8_t> ending_bg3_;            // unk_DA370 (960x150) - space3
    std::vector<uint8_t> ending_runs_;           // dword_5B080 (CHP.EXE VA, 77554 bytes)
    std::vector<uint8_t> ending_heights_;        // word_5AE28 (157 words)
    void save_blackbox();               // sub_15BD9
    void save_contibak();               // sub_1492E
    void write_config();                // sub_1776D

    // ---- Dialogue system (sub_2B1BD / sub_2AAD3 / sub_2BBA0) ----
    // sub_2B1BD(portrait, name, talk_id, slot): portrait + speaker name + TALK.TBL body.
    // slot indexes the original dword_A250B table (14 bytes per entry); the port always uses 0,
    // whose entry is the party sprite screen position (party_sx_/party_sy_), so the window and
    // portrait are placed above/below or left/right of the party icon.
    // Returns the 0-based choice index when '@' choices were shown, else 0 (the VM ignores it).
    int  run_talk(int portrait_id, const char* name, int talk_id);
    int  run_talk_choice(const std::vector<std::string>& choices, int portrait_id, bool upper); // sub_2AAD3
    void draw_dialog_chrome(int portrait_id, bool upper);  // shade + frame + portrait
    void render_dialog_shade(int x, int y);       // sub_1CAB0 + unk_834E9 mask
    void run_narration(int talk_id);              // sub_2BBA0: vertically scrolling TALK.TBL narration
    void draw_text_char(int x, int y, const char* ch, uint16_t color); // sub_2D2F4
    bool wait_for_input(); // sub_28A06: wait for click/key; false when window close was requested (dos_exit_)
    void flip_wait(int retraces);                 // sub_1D83A x N (approximates the 70Hz retrace wait)

    // ---- Portrait and sprite system ----
    void load_portrait(int id);             // sub_1F930: load from portrait.syf
    void load_small_portrait(int id);       // sub_1FA10: load from portsmal.syf
    void blit_sprite(int x, int y, const uint8_t* data); // RLE wrapper
    void blit_sprite_rle(int x, int y, const uint8_t* data); // sub_1D874: RLE blit
    void blit_sprite_transparent(int x, int y, const uint8_t* data); // sub_1D8D5: raw bitmap blit with 0xFF transparency
    void free_portrait();

    // ---- Mini font and sprite font (sub_17F88, sub_17FB2, sub_17E6A, sub_1802C, sub_18047) ----
    void set_mini_font_color(uint8_t color);                         // sub_1802C
    void draw_mini_char(int x, int y, char c, uint8_t color);        // sub_17E6A
    void draw_mini_string(int x, int y, const char* str, uint8_t color); // sub_17FB2
    void draw_mini_number(int x, int y, int value, uint8_t color);   // sub_17F88
    void draw_sprite_font(int x, int y, const char* str, int font_group); // sub_18047

    uint8_t mini_font_color_ = 0x01; // byte_5AE1D (default color)

    // Portrait state
    uint8_t portrait_large_[14000]; // 0x36B0 bytes RLE data
    uint8_t portrait_small_[10000]; // 0x2710 bytes RLE data
    int portrait_width_ = 0;
    int portrait_height_ = 0;
    bool portrait_loaded_ = false;

    // ---- Equipment system ----
    void load_item_data();                  // sub_2E4C0: load ITEM.DAT
    void load_item_sprites();               // sub_2E533: load CITIZEN2.SPR item sprites
    void show_status_window();              // sub_2F09A: right-click status window (see the section below)
    bool load_spr(SPRParser& parser, const char* subdir, const char* filename, bool is_citizen = false); // generic SPR loader
    void load_all_system_sprites();         // sub_1FDC4: load POINT.SPR, FONT.SPR, TIME_GRA.SYF, ITEM.SPR, CITIZEN2.SPR


    // ==== Right-click status window (sub_2F09A) and helpers ====
    // Character records (0x88 bytes) are read straight from g_characters[] with the offsets
    // CHP uses: name +2, id +0x0F, small portrait +0x10, class +0x11, level +0x2B,
    // skills +0x2E.., equipment +0x5A.., team +0x87.
    struct StatusEv { int code = 0, x = 0, y = 0; };   // dword_8FF20 (1=left, 2=right button) / 8FF24 / 8FF28
    bool status_load_assets();                          // load status window sprites from CHP.EXE VA
    bool status_load_pcx(const char* name, bool with_palette, std::vector<uint8_t>& dst); // sub_2DD68 / sub_2DC95
    void status_poll();                                 // sub_28A06
    void status_wait_left_release();                    // sub_2E6CF
    void status_wait_right_release();                   // sub_2E720
    void status_flip();                                 // sub_28C8B + sub_1DA67 + sub_28C49
    void status_flip_vsync();                           // sub_1DA88
    void status_draw_main(int sel, int top);            // sub_2EA3D
    void status_draw_party_list(int top, int sel);      // sub_30169
    void status_draw_gold(int x, int y);                // sub_2E9BC
    void status_draw_thumb(bool with_offset);           // scrollbar thumb (called from sub_1C6F6)
    void status_draw_skill_grid(const uint8_t* rec, int x0); // loc_2F387..loc_2F859 (shared with sub_30AFE)
    void status_item_popup(int item);                   // sub_2EE3F
    void status_item_stats(int item);                   // sub_32ABA
    void status_message(const char* johab_msg);         // sub_2E8FC
    void status_slide_back(int sel, bool from_inventory); // return transition from a sub-screen
    const uint8_t* status_icon(int slot) const;         // off_529CC + slot*0x1AE
    void status_item_icon(int x_base, int item);        // item.spr icon (offsets adjusted by sub_1FDC4)
    static int status_scroll_to_top(int off, int h, int count);  // sub_2EDE2
    static int status_top_to_scroll(int top, int count);         // sub_2EE28
    void draw_bevel_box(int x1, int y1, int x2, int y2, uint8_t fill = 0x0D); // sub_1C6F6 (fill = byte_7B470)
    void draw_outline_rect(int x1, int y1, int x2, int y2, uint8_t color); // sub_1C5BC
    void gfx_hline(int x, int y, int len, uint8_t c);            // sub_1DB05
    void gfx_vline(int x, int y, int len, uint8_t c);            // sub_1DAD7
    void gfx_fill(int x, int y, int w, int h, uint8_t c);        // sub_1DB9B
    void gfx_copy_rect(const uint8_t* src, uint8_t* dst, int width, int height); // sub_33A6C
    void draw_status_text(int x, int y, const char* s);          // sub_2D2F4
    void party_rebuild(int team = -1);                           // sub_15A7F + sub_15AE5 (team<0 means state_.team_active)
    void party_writeback();                                      // sub_15B40
    uint8_t* party_rec(int i) { return party_recs_.data() + (size_t)i * 0x88; }
    // Two sub-screens: chp_class.cpp (bottom-left button) / chp_equip.cpp (bottom-right button)
    int status_inventory_screen(int& sel, int& top);             // sub_128DA
    int status_equipment_screen(int sel);                        // sub_30AFE

    // ---- sub_128DA: class change screen (chp_class.cpp, chp_class_data.h / CHP.EXE sprites) ----
    bool class_load_assets();
    const uint8_t* class_rec(int idx) const;                     // party record (zero-filled record when out of range)
    bool class_can_change(int idx) const;                        // sub_12800
    int  class_tab_of(int idx) const;                            // sub_12586
    int  class_sprite_row(int idx) const;                        // sub_12701 (100000 = 0x1869F when not found)
    bool class_req_ok(const uint8_t* rec, int cls) const;        // sub_11AD8
    bool class_calc_shortfall(const uint8_t* rec, int cls);      // sub_1172D (shortfall -> class_short_)
    void class_recalc_stats(uint8_t* rec);                       // sub_1285F
    void class_dim_rect(int x1, int y1, int x2, int y2);         // sub_1CC90 / sub_1CC00
    void class_draw_list(int top, int sel);                      // sub_11DC7
    void class_draw_req_panel(int sel);                          // condition icon list of sub_11F24
    void class_draw_grid(int sel, int tab);                      // sub_1226E
    void class_compose(int sel);                                 // sub_11F24 + sub_1226E + bar + tabs (no flip)
    void class_refresh(int sel);                                 // sub_12753
    void class_draw_thumb_and_tab();                             // thumb (0x12A..0x139) + selected tab sprite
    int  class_cell_class(int tab, int cell) const;              // unk_58CB8[tab*16+cell]-1 (-1 when absent)
    int  class_req_at(int idx) const;                            // byte_5884A[idx] (0 when out of range)
    // Shared inventory/equipment helpers (sub_32A1A / sub_329BD / sub_339B5 / sub_33909 / sub_32D94
    // / sub_32FCD)
    int  status_inv_slot() const;                                // dword_1271E4 >> 16 (current team)
    bool status_inv_add(int item0);                              // sub_32A1A: insert at front (message + false when full)
    void status_inv_remove(int idx);                             // sub_329BD
    void status_equip_strip(uint8_t* rec, int slot);             // sub_339B5: unequip (reverts stats)
    void status_equip_apply(uint8_t* rec, int item0, int slot);  // sub_33909: equip (adds stats)
    void status_discard_list(int sel_idx, int top);              // sub_32D94: discard window list
    void status_discard_popup();                                 // sub_32FCD: pick an item to discard when the inventory is full
    void status_play_sfx(int idx);                               // sub_35755 (1 menu / 2 into / 3 flying)
    bool class_assets_loaded_ = false;
    std::vector<uint8_t> class_req_ok_spr_, class_req_bad_spr_, class_tip_bg_, class_prev_a_, class_prev_b_, class_ok_btn_;
    std::vector<uint8_t> class_arrow_up_, class_arrow_dn_;
    int  class_tab_ = 0;          // dword_8FF38
    int  class_cell_ = 0;         // dword_8FF3C
    int  class_spr_row_ = 0;      // dword_8FF34
    int  class_top_ = 0;          // dword_8FF40
    int  class_sel_class_ = 0;    // dword_8FF44
    uint8_t class_short_[0x20] = {};   // byte_5A6B4.. ([0]=5A6B4, [1+k]=shortfall of condition k, [17]=5A6C5, [18]=5A6C6)
    int  discard_top_ = 0;        // dword_1271F0

    // ---- sub_30AFE: equipment screen (chp_equip.cpp, chp_equip_data.h) ----
    uint16_t& inv_ref(int slot, int idx);                         // team inventory slot (idx<0 reads inv_spill_, the memory the original touches before the array)
    uint16_t inv_spill_[128] = {};
    void equip_draw(int sel, int top);                            // sub_3061C
    void equip_draw_bar(int off, float th);                       // scrollbar (0x12A..0x139)
    void equip_line(int x1, int y1, int x2, int y2, uint8_t c);   // sub_1C995 (straight line for the X mark)
    bool equip_insert(int item0, int idx);                        // sub_17B71: insert into inventory slot idx
    bool equip_is_kept(int item_plus1) const;                     // sub_2E8C1: item cannot be discarded
    void equip_drag(int sel, int item0, int src);                 // sub_31AF2: drag and drop an item
    static float equip_thumb_h(int count);                        // thumb length (123 when count < 6)

    // ---- sub_20BC8 options / sub_16AFA save / sub_1716E load / sub_28E8A confirm (chp_option.cpp) ----
    static constexpr int kVsync1366E = 51;                        // sub_1366E = sub_1D83A x 51
    static constexpr int kVsync1376D = 33;                        // sub_1376D = sub_1D83A x 33
    void option_screen();                                         // sub_20BC8
    bool confirm_dialog(const char* msg);                         // sub_28E8A (true = yes)
    void slot_screen(bool save);                                  // sub_16AFA (save) / sub_1716E (load)
    void slot_draw_info(int k);                                   // sub_1603E: save date/time of slot k
    std::string slot_info_text(int k);                            // sub_1603E string part ("<M> month <D> day <H> hour <M> min")
    void slot_draw_row(int i, const bool* exists, bool save);     // list row i (0..4 slots, 5 = bottom row, 6 = none)
    void opt_text(int x, int y, const char* s, int mode);         // sub_2CDD8(4, mode, 0xFF) + sub_2D2F4
    bool opt_load_assets();                                       // 12 option sprites (CHP.EXE VA, sub_20BC8)
    bool opt_copy_file(const std::string& src, const std::string& dst);  // sub_13C31
    bool esc_down() const;                                        // dword_9C51C (ESC key is held)
    std::vector<uint8_t> opt_spr_[12];
    bool opt_assets_loaded_ = false;
    StatusEv status_ev_;
    int   status_top_ = 0;                // dword_1271B8 (top list row)
    int   status_thumb_off_ = 0;          // dword_1271D0 (scrollbar y offset, kept when the window closes)
    float status_thumb_h_ = 123.0f;       // dword_1271CC
    std::vector<uint8_t> party_recs_;     // unk_A48B8 (party record copy)
    std::vector<uint8_t> reserve_recs_;   // unk_940B4 (reserve roster copy)
    bool status_assets_loaded_ = false;
    std::vector<uint8_t> status_icons_, status_slot_, status_btn_a_, status_btn_b_, status_gold_, status_hand_;
    std::vector<uint8_t> status_snap_, status_prev_;             // unk_DA370 / unk_EB4E0
    std::function<bool(StatusEv&)> status_poll_hook_;            // test hook: returning true consumes the event
    uint8_t status_save_pal_[256][3] = {};                       // word_A3D4C

    // ---- Item data ----
    static constexpr int MAX_ITEMS = 124;   // 0x7C items in ITEM.DAT
    static constexpr int ITEM_SIZE = 66;    // 0x42 bytes per item
    uint8_t item_data_[MAX_ITEMS][ITEM_SIZE] = {};
    bool items_loaded_ = false;

    // ---- Item sprites (CITIZEN2.SPR via SPRParser) ----
    SPRParser item_sprites_;                // unified SPR parser
    bool item_sprites_loaded_ = false;

    // ---- System sprite caches (POINT.SPR, ITEM.SPR, TIME_GRA.SYF, FONT.SPR from sub_1FDC4) ----
    SPRParser point_sprites_;               // POINT.SPR (point icons - group 2)
    SPRParser font_sprites_;                // FONT.SPR (group 3)
    SPRParser time_sprites_;                // TIME_GRA.SYF (time display - group 5)
    SPRParser item_box_sprites_;            // ITEM.SPR (item box icons - group 10)
    bool point_sprites_loaded_ = false;

    // unk_809A4: chapter title banner box frame RLE sprite (CHP.EXE VA 0x809A4)
    std::vector<uint8_t> banner_box_sprite_;
    bool banner_box_sprite_loaded_ = false;

    // Resources embedded in CHP.EXE (VA -> file offset via the LE page table at runtime)
    std::vector<uint8_t> dialog_frame_rle_;      // off_82720
    std::vector<uint8_t> dialog_shade_mask_;     // unk_834E9 (4-byte header + w*h)
    std::vector<uint8_t> party_sprite_data_[3];  // team 0x17 / <= 0x18 / other
    bool font_sprites_loaded_ = false;
    bool time_sprites_loaded_ = false;
    bool item_box_sprites_loaded_ = false;

    // ---- Time display ----
    void render_time_display();             // sub_22C8E (loc_230E2): date and UI overlay rendering

    // ---- VM helpers ----
    void vm_team_rebuild();             // sub_15A7F + sub_15AE5 + sub_15B40
    void vm_set_character_team(int char_no, int team); // sub_17E45

    // ---- World map movement (sub_21476 / sub_1FEDA) ----
    // The original does not walk along paths: click -> camera pan (sub_1FEDA, blocking, time
    // advances) -> arrival handling on the next frame (sub_10010(4),(1) -> point entry).
    void world_map_step();                                   // sub_21476 (once per frame)
    void camera_pan(int x0, int y0, int x1, int y1);         // sub_1FEDA
    void advance_time_phase();                               // sub_1FEDA loc_20303~loc_2035E
    void render_party_sprite_at(int world_x, int world_y);   // sub_22C8E loc_22EF9~loc_23093
    void blit_sprite_rle_clipped(int x, int y, const uint8_t* data); // clipped RLE blit
    bool is_point_pair_active(int from_pt, int to_pt) const; // sub_206E1
    int  pick_point_at(int mx, int my) const;                // sub_21476 loc_21957 (no activation check)
    int  party_sx_ = 0, party_sy_ = 0;     // dword_A250B (+0 x, +2 y)
    bool moving_ = false;                  // byte_A2511
    bool click_latch_ = false;             // byte_804A5
    bool first_frame_ = true;              // byte_80320
    bool pan_active_ = false;              // byte_7BCA4 == 0

    // ---- Travel effects (word_C5DD9+1 = 0 none / 1 bigong1 / 2 bigong2 / 3 seal) ----
    // sub_21476 (click) -> sub_21234 (takeoff: ship rises + fade out) -> sub_1FEDA (sky and
    // ground perspective flight pan, direction sprite, sound) -> descent + fade out ->
    // sub_21374 (landing).
    bool travel_begin(int from_pt);                 // load sprites + save palette + takeoff effect
    void travel_end(int to_pt);                     // kind==3 clears the screen, otherwise restore palette + land
    bool load_sky_xcp(int chapter);                 // sub_2DF77: .\chapdata\sky<N>.xcp (960x151) -> DA370
    void build_fog_lut();                           // sub_3907C x6: horizon fog shade LUT
    void render_flying_view();                      // sub_286DC + sub_1D0E8 + fog bands + border
    bool travel_fade_step();                        // sub_1E4CD(pal,0,255,2): true while still fading
    void travel_flip();                             // sub_1DA88 + sub_1D83A (present + vertical sync)
    void play_travel_sfx();                         // sub_35755(C5CE0*10, 3 = flying.VOC, 1)
    int  travel_frame_ms() const;                   // C5CDE: 0 = wait for retrace (70Hz), 1 = no wait
    bool travel_fx_ = false;                        // travel effect in use for this move
    SPRParser travel_sprites_;                      // unk_9C720 slot 8 (bigong1/2.spr, seal.spr)
    std::vector<uint8_t> travel_sky_;               // unk_DA370 (sky xcp, 960x151)
    std::vector<uint8_t> travel_snapshot_;          // unk_DA370 (also reused as the screen snapshot)
    int  travel_sky_off_ = 0xF0;                    // sub_1FEDA var_C
    uint8_t travel_pal_[256][3] = {};               // word_A3D4C: saved palette (6-bit)
    uint8_t fog_lut_[6][256] = {};                  // the 6 shade rows of dword_13952C
    int  fade_counter_ = 0;                         // dword_7BC98
    int  fade_cur_[256][3] = {};                    // word_A314C
    int  fade_delta_[256][3] = {};                  // word_A2B4C

    // CONTIBAK global state block (0xCD bytes @ C5DD2), preserved as-is; [8] = word_C5DD9+1
    uint8_t contibak_block_[0xCD] = {};

    // ---- Path helpers ----
    std::string game_path(const char* filename);
    std::string cstldata_path(const char* filename);
    std::string chapdata_path(const char* filename);

    int chapter_num_ = 1;
    bool in_tool_mode_ = false;

    // ---- Contibak data (loaded from file) ----
    // 20 chapters x 2374 bytes (points + per-chapter state)
    uint8_t all_chapter_points_[MAX_CHAPTERS][CONTIBAK_CHAPTER_STRIDE] = {};
    // 20 chapters x 15 paths x 200 bytes
    uint8_t all_chapter_paths_[MAX_CHAPTERS][MAX_PATHS_PER_CHAPTER][CONTIBAK_PATH_STRIDE] = {};

    // ---- Event system data ----
    EventTriggerCondition g_event_triggers_[MAX_EVENT_TRIGGERS] = {};
    // VM script bytecode: 20 records x 4000 byte stride (2000 bytes used)
    uint8_t g_script_data_[MAX_EVENT_TRIGGERS][VM_SCRIPT_MEM_STRIDE] = {};
    // 10 quest objective names (C5CE8 byte form)
    QuestObjective g_quest_objectives_[QUEST_COUNT] = {};
    // 10 script running markers (C5D6A, word array) - nonzero = script running for this slot
    uint16_t g_script_markers_[QUEST_COUNT] = {};

    // ---- blackbox.dat slot (sub_15DB7/sub_15BD9, 2 bytes at file +0x4E24) ----
    // The original reads and writes dword_1271E4+2 (team/gold slot, from dword_C5E9B).
    // The chapter id (byte_C5E9F) comes from contibak.tmp and is unrelated to blackbox, so
    // the loaded value is kept here and written back unchanged.
    uint16_t blackbox_slot_ = 0;  // dword_1271E4+2
};