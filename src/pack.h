#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include "video.h"
#include "font.h"
#include "input.h"
#include "data_structures.h"
#include "talktbl.h"
#include "nc2_parser.h"

constexpr int TOWN_TILE_W     = 24;  // 0x18
constexpr int TOWN_TILE_H     = 16;  // 0x10
constexpr int TOWN_TILE_BYTES = TOWN_TILE_W * TOWN_TILE_H; // 384 (0x180) bytes
constexpr int MAX_TOWN_NPCS   = 170; // 0xAA
constexpr int MAX_TOWN_EVENTS = 40;  // 0x28

#pragma pack(push, 1)
// 40 bytes per NPC (byte_19E144 array in PACK.ASM)
struct PackNPC {
    uint8_t  active;            // +0x00: 1=active, 0=inactive
    uint16_t tile_x;            // +0x01
    uint16_t tile_y;            // +0x03
    char     name[20];          // +0x05: script/role name ("hired1", "inn", etc.)
    // +0x19: talk_id. sub_25F21 reads a dword at +0x17 and keeps its upper word
    // (sar edx, 10h) in both the inn branch (loc_261C4) and the ordinary
    // dialogue branch (loc_26335). Since name[20] ends at +0x18, that upper word
    // is this field, which is then passed to sub_25DD3 (the TALK.TBL parser).
    uint16_t talk_id;           // +0x19: TALK.TBL record index for ordinary dialogue
    uint16_t param1;            // +0x1B
    // +0x1D: citizen sprite number (0, 1, 2, ...), not a sprite group. Both
    // sub_24646 (NPC registration) and sub_1F6E4 (per-frame animation) shift
    // this word left by 4 and add the direction offset (+0/4/8/12), so
    // base_frame = sprite_id * 16. Town NPCs always use group 4
    // (citizen1/2.spr), which is hard-coded at the registration site.
    uint16_t sprite_id;         // +0x1D: citizen sprite number; base_frame = sprite_id * 16, group is always 4
    // +0x1F: only the low byte is read, by
    // "mov al, byte ptr word_19E163[eax]" (eax = npc_idx*40) feeding the
    // portrait_id argument of sub_24BA4. word_19E163 - byte_19E144 = 0x1F.
    // sub_1F6E4 derives animation frames from sprite_id * 16 instead, so this
    // word must never be added as a frame value while rendering.
    uint16_t portrait_word;     // +0x1F: low byte = portrait_id (PORTRAIT.SYF); not used for animation frames
    uint16_t target_x;          // +0x21
    uint16_t target_y;          // +0x23
    uint8_t  direction;         // +0x25: 0x48=Up, 0x50=Down, 0x4B=Left, 0x4D=Right
    uint8_t  move_speed;        // +0x26
    uint8_t  move_type;         // +0x27

    uint8_t portrait_id() const { return (uint8_t)(portrait_word & 0xFF); }
};
static_assert(sizeof(PackNPC) == 40, "PackNPC must be exactly 40 bytes");

// 22 bytes per Event condition (word_19FBD4 array in PACK.ASM)
struct PackEvent {
    uint16_t trigger_x;         // +0x00
    uint16_t trigger_y;         // +0x02
    uint16_t flag_idx1;         // +0x04
    uint16_t flag_idx2;         // +0x06
    uint16_t flag_idx3;         // +0x08
    uint16_t flag_val1;         // +0x0A
    uint16_t flag_val2;         // +0x0C
    uint16_t flag_val3;         // +0x0E
    uint16_t state_check;       // +0x10
    uint16_t event_id;          // +0x12
    uint16_t npc_id_trigger;    // +0x14
};
static_assert(sizeof(PackEvent) == 22, "PackEvent must be exactly 22 bytes");
#pragma pack(pop)

// Softmax Sprite Struct (14 bytes header in ASM)
struct PackSprite {
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t  x_offset = 0;
    int16_t  y_offset = 0;
    int16_t  foot_offset_y = 0;
    uint32_t data_size = 0;
    std::vector<uint8_t> data;
};

// Unified sprite entry for Y-sorted drawing (unk_82934 / word_83D84 in PACK.ASM)
struct DrawSpriteEntry {
    int group = 0;              // 1=custom char, 2=top tile, 3=itembox, 4=citizen, 6=item
    int sprite_idx = 0;
    int screen_x = 0;
    int screen_y = 0;
    int foot_y = 0;
};

// ---- off_53D0C: mouse cursor sprites (sub_2B2A1 raw bitmap format) ----
// 800 bytes (0x320) read at runtime from PACK.EXE object 2 (reloc base 0x50000,
// VA 0x53D0C): 8 entries of 100 bytes, each width u16 + height u16 +
// pixels[width*height] with 0xFF transparent. Order: 0 = Up, 1 = Up pressed,
// 2 = Down, 3 = Down pressed, 4 = Left, 5 = Left pressed, 6 = Right,
// 7 = Right pressed.
struct PackCursorFrame {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> pixels; // width*height bytes, 0xFF = transparent
};

// ---- NPC dialogue window placement (sub_24DA5) ----
// The window, portrait and speech tail are drawn on the half of the screen
// opposite the NPC so they never cover it. Values traced from sub_24DA5, from
// the loc_263CD/loc_264AF branches of sub_25F21 and from the sub_186DC call
// site. The dimming rect (250x76 plus mask, unk_59ABD) is reproduced as-is.
struct DialogPlacement {
    bool npc_upper_half = true;  // NPC screen Y < 0x64 (100)
    bool npc_right_half = false; // tail side: NPC screen centre X >= 0xA0 (160), sub_24DA5
    // The portrait side is a separate test at every sub_24BA4 call site (sub_25F21,
    // sub_2708B, sub_24FD5, sub_26E32): unk_82938 -- the anchor X, not the sprite box
    // centre -- compared with a strict > 0xA0.
    bool portrait_left = false;  // unk_82938 > 0xA0 -> portrait blitted at x = 8
    int frame_x = 0x1E;          // dialogue frame blit X (always 30)
    int frame_y = 0;             // dialogue frame blit Y (105 or 1)
    int dimming_x = 0x24;        // dimming start X (always 36)
    int dimming_y = 0;           // dimming start Y
    int portrait_x = 0;          // portrait blit X (8 or 312 - width)
    int portrait_y = 0;          // portrait blit Y (portrait_y_base - height)
    int speech_tail_index = 0;   // slot in speech_tail_data_ (0..7)
    int tail_x = 0;              // tail position computed by sub_24DA5 (NPC edge)
    int tail_y = 0;

    // sub_25F21 sets the text X to 0x37 (55) for both the upper and lower
    // branches right after the portrait blit; it never moves opposite the
    // portrait.
    int text_x = 0x37;

    // Y of the bottom (4th) text row: frame_y + 0x3F (63) in both branches
    // (105 + 63 = 168 above, 1 + 63 = 64 below). Row Y values are
    // text_base_y - 48 / -32 / -16 / 0, 16 px apart.
    int text_base_y = 0;
};

class Pack {
public:
    // References the single Video/Input instances owned by gene2.cpp.
    Pack(Video& video, Input& input);
    ~Pack();

    int run(const std::string& mode, const std::string& town_param);

private:
    Video& video_;
    Input& input_;
    Font font_;

    int town_id_ = 1;
    bool should_exit_ = false;
    int return_code_ = 0;

    // ---- blackbox.dat header preservation (sub_23369 / sub_23147) ----
    // The original reads and writes back the leading 1 byte unk_52698 (skipped
    // on load, so it stays 0 as in BSS), 2 bytes day (unk_1A00C4), 1 byte sec
    // (unk_1A00DB), the 0x4E20 flag bytes and 2 bytes team slot
    // (word_1A00CC). PACK never changes the header; keeping the loaded values
    // and writing them back preserves the date and the per-slot gold.
    uint16_t blackbox_day_ = 0;   // unk_1A00C4
    uint8_t blackbox_sec_ = 0;    // unk_1A00DB
    uint16_t blackbox_slot_ = 0;  // word_1A00CC

    // ---- slot inventory (sub_23369 / sub_23147 / sub_376D8 / sub_3767B) ----
    // Each slot is 0x258 (600) bytes at unk_15FD2C + slot*600 with the count in
    // word_19763C[slot]. On disk a slot is 4 bytes gold, 2 bytes count and then
    // count 1-byte items; the in-memory item value is the file byte + 1.
    static constexpr int kSlotBytes = 0x258;
    std::vector<uint8_t> slot_items_ = std::vector<uint8_t>((size_t)MAX_BLACKBOX_SLOTS * 0x258 + 4, 0);
    int cur_slot() const;                  // (int16)word_1A00CC, clamped to 0 when out of range
    uint8_t& slot_item(int slot, int idx); // unk_15FD2C[slot*600+idx]; may overflow into the next slot as in the original
    int  inv_add_front(int mem_item);      // sub_376D8: insert at the front; 1 on success, -1 when all 600 are used (prints a message)
    void inv_remove(int idx);              // sub_3767B: remove idx, shift the rest down, count-1

    // ---- Town map dimensions and layers (sub_213A6 / sub_24017) ----
    uint32_t map_w_ = 0;
    uint32_t map_h_ = 0;
    uint32_t tile_count_ = 0;

    std::vector<std::vector<uint8_t>> base_tiles_data_; // 24x16 uncompressed bitmaps
    std::vector<PackSprite> top_tile_sprites_;          // Overlay sprites (dword_8B12A)

    // ---- sub_2294D loc_22A01: left/right map edge frame overlays ----
    // Softmax RLE sprites stored in PACK.EXE dseg02, read at runtime from unk_54630
    // (VA 0x54630) and off_55A99 (VA 0x55A99). They never change with town_id, so
    // a town switch does not reload them.
    std::vector<uint8_t> left_frame_data_;
    std::vector<uint8_t> right_frame_data_;

    // ---- Dialogue window sprites, read from PACK.EXE dseg02 ----
    // off_58CF4 (main dialogue frame), unk_7C4D2 / unk_7C220 (confirm / cancel
    // buttons) and off_5E4F9 (speech tails) are all passed to sub_2B240, the RLE
    // blitter.
    // unk_59ABD is not a sprite: decoded with the sub_2B240 format it is a
    // horizontal-line noise pattern, so it is used only as the dimming mask below.
    std::vector<uint8_t> dialog_frame_data_;   // off_58CF4 (RLE, self-delimited)
    std::vector<uint8_t> ok_button_data_;      // unk_7C4D2 (RLE, self-delimited)
    std::vector<uint8_t> cancel_button_data_;  // unk_7C220 (RLE, self-delimited)
    // off_5E4F9: 8 entries with a fixed 200-byte stride. Each entry is a
    // self-terminating RLE stream; 200 is the maximum slot size, the remaining
    // bytes are padding. sub_24DA5 indexes it as 200 * (sub_2B81D result 0/1 +
    // direction base 0/2/4/6), giving 0 = Up variant 0, 1 = Up variant 1,
    // 2 = Down variant 0, ... 7 = Right variant 1. The extracted data shows
    // near-identical symmetric pairs for (0,1)/(2,3)/(4,5)/(6,7), matching that
    // mapping. Whether sub_2B81D always returns 0 or 1 is not confirmed, so more
    // variants may exist.
    static constexpr int SPEECH_TAIL_STRIDE = 200;
    static constexpr int SPEECH_TAIL_COUNT  = 8;
    std::vector<uint8_t> speech_tail_data_;    // off_5E4F9, SPEECH_TAIL_STRIDE*SPEECH_TAIL_COUNT bytes
    // Blit entry n with blit_sprite_rle(x, y, speech_tail_data_.data() +
    // SPEECH_TAIL_STRIDE * n).
    // unk_59ABD: dimming rect applied by sub_186DC at (X, Y): 250x76 plus a
    // 19000-byte mask (0xFF = skip, anything else = dim). Read at runtime from
    // PACK.EXE VA 0x59ABD after a 4-byte header.
    static constexpr int DIMMING_W = 250;
    static constexpr int DIMMING_H = 76;
    std::vector<uint8_t> dimming_mask_;        // DIMMING_W*DIMMING_H bytes
    std::vector<uint8_t> mouse_dir_table_;     // aHhhhhhhhhhhhhh: 320x200 direction table (VA 0x60A40)

    // ---- off_56C00: player/NPC foot shadow sprite (VA 0x56C00) ----
    // 137 bytes (0x89) in the same sub_2B2A1 raw format as the cursor: 19x7 oval
    // dots, every opaque pixel uses palette index 0.
    PackCursorFrame shadow_sprite_;
    // ---- off_53D0C: mouse cursor sprites (VA 0x53D0C, 8 fixed frames) ----
    std::vector<PackCursorFrame> cursor_sprites_;
    // Hardware cursor: the 8 direction frames are registered with Video by
    // load_cursor_sprites() (Video::cursor_register), which also handles scaling
    // and palette changes; only the first registered id is kept here. Falls back
    // to a software blit when registration or the offscreen surface fails.
    int cursor_base_ = -1;
    // dword_8279C+2: mouse direction produced by sub_2E1A4 every frame
    // (0x48/0x4B/0x4D/0x50). The original keeps the previous value when the
    // lookup reports no change, so it is kept apart from movement: the cursor
    // updates every frame, the direction only when a tile step starts.
    uint8_t cursor_dir_ = 0x50; // default Down (the original's initial value is unknown)

    // ---- Map layers ----
    uint16_t* layer_base_tiles_ = nullptr; // dword_15B9B0
    uint16_t* layer_top_tiles_  = nullptr; // dword_15B9C8
    uint8_t*  layer_passable_   = nullptr; // dword_15B9C4
    uint8_t*  layer_event_      = nullptr; // dword_15B9C0
    uint16_t* layer_item_       = nullptr; // dword_15B9BC
    uint16_t* layer_building_   = nullptr; // dword_15B9B8
    uint8_t*  layer_npc_        = nullptr; // dword_15B9B4
    uint8_t   town_palette_[256][3] = {};

    // ---- Pathfinding grid buffers ----
    uint16_t* path_cost_grid_ = nullptr;

    // ---- Player town position, frame and camera ----
    // dword_15BA14+2 = player_tile_x_, word_15BA18 = player_tile_y_,
    // dword_56BFA+2 = cam_tile_x_, dword_56BFA = cam_tile_y_,
    // dword_56BF6+2 = cam_fine_x_, dword_56BF6 = cam_fine_y_ (0..23 / 0..15),
    // byte_544CE = player_move_step_, byte_F30C0 = player_anim_counter_,
    // byte_82949 = player_dir_, word_F30B8/dword_F30BA = speed mode
    // (4/6 fast, 2/3 slow), dword_F30A0 = player_base_frame_.
    int16_t player_tile_x_ = 0;
    int16_t player_tile_y_ = 0;
    int16_t player_fine_x_ = 0; // sub_1D12F: accumulates bh*6, offset inside the tile
    int16_t player_fine_y_ = 0; // sub_1D12F: accumulates bh*4
    uint16_t player_base_frame_ = 0; // dword_F30A0
    uint8_t player_dir_ = 0x50;      // 0x48: Up, 0x50: Down, 0x4B: Left, 0x4D: Right
    uint8_t player_anim_frame_ = 0;
    uint8_t player_anim_counter_ = 0; // byte_F30C0: increments a frame every 2 (fast) or 4 (slow) steps
    uint8_t player_move_step_ = 0;   // byte_544CE
    bool is_player_moving_ = false;
    std::vector<uint8_t> player_path_;

    int32_t cam_tile_x_ = 0;
    int32_t cam_tile_y_ = 0;
    int32_t cam_fine_x_ = 0;
    int32_t cam_fine_y_ = 0;

    // ---- Per-town tile position save/restore (sub_20EB0 / sub_213A6, word_56C8D / word_56E89)
    // ----
    // sub_20EB0 stores the current tile (dword_15BA14+2, word_15BA18) under the
    // town being left (byte_15BA23 = town_id_ before the warp), decrementing Y
    // by 1 when moving DOWN ("cmp byte_82949,50h; dec word_56E89[eax]").
    // sub_213A6 (loc_21B9E..loc_21D00) restores word_56C8D[town]/
    // word_56E89[town] when it is not (0,0): entering while facing UP keeps
    // saved_y, any other direction uses saved_y + 1 (byte_82949 == 0x48 branch).
    // A town never visited falls back to the 0xFF portal spawn
    // (portal_x, portal_y + 1) in load_town_and_tec() (loc_21CBF).
    static constexpr int MAX_TOWNS = 256;
    int16_t saved_tile_x_[MAX_TOWNS] = {}; // word_56C8D
    int16_t saved_tile_y_[MAX_TOWNS] = {}; // word_56E89

    // Fast: 4 steps per tile, 6 px X / 4 px Y. Slow: 8 steps per tile, 3 px / 2 px
    // (word_F30B8 = 2, dword_F30BA = 3). loc_1DA1C compares byte_82730 with 1 and
    // only then takes the fast path; anything else (0, or the default when
    // game.cfg is missing) is slow, which is also the default here until
    // load_game_config() reads the real value.
    static constexpr int MOVE_STEPS_FAST = 4;
    static constexpr int MOVE_STEPS_SLOW = 8;
    int move_steps_per_tile_ = MOVE_STEPS_SLOW;

    // ---- sub_235A7: load game.cfg (byte_82730 = move speed, 0 = slow, 1 = fast) ----
    uint8_t config_move_speed_ = 0;

    // ---- NPCs and events ----
    PackNPC   npcs_[MAX_TOWN_NPCS] = {};
    PackEvent events_[MAX_TOWN_EVENTS] = {};
    uint8_t   npc_anim_frame_[MAX_TOWN_NPCS] = {};

    // ---- Sprites ----
    std::vector<PackSprite> citizen_sprites_;
    std::vector<PackSprite> item_sprites_;
    std::vector<PackSprite> itembox_sprites_;

    // ---- sub_213A6 / sub_24017 loc_216E4/loc_243C8: chest top-tile placement ----
    // Frames 0/1 of itembox.spr (closed/open chest) are appended to
    // top_tile_sprites_, and the index is written to layer_top_tiles_ one row
    // below (y+1) each chest (item_id % 1000 >= 500 in layer_item_) so the chest
    // graphic appears on screen. -1 means not placed yet.
    int chest_closed_tile_idx_ = -1;
    int chest_open_tile_idx_   = -1;

    // ---- Remote object (chest/building) click: walk adjacent, then interact ----
    // When handle_tile_click() paths to an adjacent walkable tile instead of the
    // object tile, this remembers the object tile to interact with on arrival.
    // -1 means no pending interaction.
    int pending_interact_x_ = -1;
    int pending_interact_y_ = -1;

    // ---- Item database (ITEM.DAT) ----
    static constexpr int MAX_ITEMS = 124;
    static constexpr int ITEM_SIZE = 66;
    uint8_t item_data_[MAX_ITEMS][ITEM_SIZE] = {};
    bool items_loaded_ = false;

    // ---- Loaders and subsystems ----
    bool load_map_file(const std::string& map_filename);
    bool load_town_and_tec(int town_id);
    bool load_citizen_sprites(const std::string& filepath);
    bool load_standard_sprites(const std::string& filepath, std::vector<PackSprite>& out_sprites);
    bool load_frame_overlays(); // left/right edge frame RLE sprites (VA 0x54630 / 0x55A99)
    bool load_cursor_sprites(); // 8 cursor frames from off_53D0C (VA 0x53D0C)
    bool load_shadow_sprite();  // foot shadow sprite off_56C00 (VA 0x56C00)
    // Dialogue sprites: off_58CF4 / unk_7C4D2 / unk_7C220 / off_5E4F9. Each one
    // can fail on its own and simply stays empty; only the dialogue UI reads them.
    bool load_dialogue_sprites();
    void place_chest_top_tiles(); // sub_213A6/sub_24017: dynamic chest top-tile placement
    void free_town_data();
    void load_item_data();
    std::string loaded_blackbox_path_;
    bool load_blackbox();   // sub_23369: true on success; prints an error and returns false when the file is missing
    void save_blackbox();
    void load_game_config(); // sub_235A7: game.cfg (10 bytes: [0] move speed, [1] music on/off byte_82731, [2] volume byte_82732)
    void save_game_config(); // sub_235EC: write the 10-byte game.cfg
    uint8_t game_cfg_[10] = {0};   // byte_82730..byte_82739
    void init_npcs_and_positions();
    void apply_story_npc_visibility(); // sub_18BE6: per frame, disables NPCs according to story progress
    void init_player_sprite_base();
    void update_camera(); // sub_2117F + sub_21E29: camera follow and fine clamp

    // ---- Pathfinding ----
    // SM (town) mode never uses the global A* search: sub_19528 -> sub_190A3 ->
    // sub_1D12F only uses the real-time direction logic below. The search
    // (sub_1001A..loc_11897) is called only by the event script runner sub_12857
    // and TOOL mode, so find_path() is kept for that port.
    bool find_path(int start_x, int start_y, int target_x, int target_y, std::vector<uint8_t>& out_path);

    // ---- sub_2E1A4 / sub_1D12F: real-time mouse-hold movement ----
    // While the left button is held, the quadrant of the player under the mouse
    // picks a direction, and a blocked direction tries a corner slide. Evaluated
    // when a new tile step starts (player_move_step_ == 0, PACK.ASM 0x1D23C:
    // "test bh,bh; jbe loc_1DA1C").
    uint8_t compute_mouse_move_direction(int mouse_x, int mouse_y) const;
    bool is_move_blocked(int x, int y, uint8_t dir) const;
    bool try_corner_slide(uint8_t blocked_dir, uint8_t& out_dir) const;
    // True when the mouse is over an NPC (sub_1F6E4). sub_1D12F is then called
    // with edx = 0 (0x190F7..0x19123) and skips movement, so holding the button
    // over an NPC does not walk.
    bool is_mouse_over_npc(int mouse_x, int mouse_y, int& out_npc_idx) const;
    // Decides whether to start a new tile step and updates
    // player_dir_/is_player_moving_ (called only when player_move_step_ == 0).
    void decide_step_direction();

    // ---- Loop and renderers ----
    void town_loop();
    void render_town_frame();
    void render_base_tiles();
    // sub_22897: draws the ellipse shadow (off_56C00) under the player and the
    // active NPCs, after the base tiles and before characters and top tiles.
    // Reproduces the call order of sub_190A3:
    // sub_21D70 -> sub_21E29 -> sub_22897 -> sub_2294D.
    void render_shadows();
    void render_sprites_and_objects();
    // ---- sub_190A3 loc_191D3..loc_1948C: highlight box on the tile under the mouse ----
    // unk_82935/unk_82936/unk_82938/unk_8293A/unk_840A6/dword_921AE all sit past
    // the physical data of object 2 (reloc base 0x50000, 0x2D000 bytes): e.g.
    // unk_82935 at offset 0x32935. They are runtime buffers filled after the .SPR
    // resources load, i.e. the same role as citizen_sprites_/npcs_/
    // npc_anim_frame_, so get_npc_screen_box() recomputes them the same way
    // render_sprites_and_objects() does.
    void get_mouse_tile(int mouse_x, int mouse_y, int& out_tx, int& out_ty) const;
    // sub_1E4B4 loc_1E75E: screen box (out_left/out_top, out_w/out_h) of NPC
    // npc_idx for the current frame, using the same formulas as
    // render_sprites_and_objects() for group 4 (citizen), 3 (itembox) and
    // 6 (item). Returns false when the NPC is inactive or its frame index is
    // outside the loaded sprite array.
    bool get_npc_screen_box(int npc_idx, int& out_left, int& out_top, int& out_w, int& out_h) const;
    void render_tile_highlight();
    // sub_190A3 loc_1948C: draw the direction cursor
    void render_mouse_cursor();
    void draw_rect_border(int x, int y, int w, int h, uint8_t color);
    void update_player_movement();
    // ---- NPC runtime movement state (sub_1F6E4 / sub_1EC6E) ----
    // unk_5441E and unk_54420 are one array: sub_1F6E4 indexes it with cx
    // (= idx+2) and sub_1EC6E with idx, so IDA produced two symbols
    // (unk_54420 = unk_5441E + 2). It holds the in-tile pixel progress and is
    // reset to 0 on every sub_1EC6E call.
    uint8_t  npc_fine_offset_[MAX_TOWN_NPCS] = {};
    // dword_544CA+2: sub-pixel accumulator (accumulates move_type, 10 units per pixel)
    uint16_t npc_subpixel_[MAX_TOWN_NPCS] = {};

    void set_npc_tile_occupancy(int npc_idx, int tx, int ty, bool occupy); // sub_1EAD0
    void on_npc_tile_completed(int npc_idx); // sub_1EC6E body

    void update_npc_movement();
    void check_town_events();
    // ---- sub_2D038: check and fire town event triggers (SM22 entry scripts) ----
    // Position is y+1 (the player standing directly below the event). The event
    // fires when the number of satisfied state/flag/NPC conditions equals the
    // number of active ones and at least one is active. Duplicate suppression
    // uses unk_7FF9C[town_id*40+idx]; firing runs sub_2D3B8 then sub_13B8D.
    // npc_trigger is dword_8271C from sub_19528 (clicked NPC + 1, else 0);
    // story_hi is dword_1A00C8 >> 16 (not modelled, so state != 0 never fires).
    // Returns true when an event fired, so the caller skips further processing.
    bool check_and_fire_town_events(uint32_t npc_trigger, bool is_entry);
    // sub_11ACE: .EVE file loader
    struct EveActor { uint16_t len = 0; std::vector<uint16_t> words; };
    struct EveFile { uint16_t total = 0; EveActor actors[51]; bool loaded = false; };
    bool load_eve_file(int event_id, EveFile& out);
    // ---- sub_13B8D / sub_12857 VM (SM22 271/272/273 dialogue, warp and END) ----
    // op11 = dialogue via sub_2708B (TALK + portrait + placement, through
    // EveActorState below), op12 = write flag, op13 = spawn actor
    // (CHARACTE/*.NC2 via sub_1A2D6/sub_1A51D), op14 = hide, op10 = direction,
    // op16 = frame, op18 = town warp (sub_20EB0). op15/op23 are handled by the
    // camera helpers; move ops 1-8 by eve_op_move(). Other ops are logged and
    // skipped. Frames 0..total-1 run in order and only commands whose fire value
    // matches the frame execute (as in sub_12857).
    bool run_eve_script(int event_id);
    // ---- EVE cutscene actor state (sub_12857 unk_82934/82935/82936/82938/8293A/840A6) ----
    // The original registers custom actors in a 0x140 (320) slot table; the port
    // keeps only the 51 slots used by EVE scripts, separate from the 170 town
    // NPCs. op13 fills tile/portrait/name/sprite, op14 deactivates, op10/op16
    // update dir/frame.
    struct EveActorState {
        bool active = false;
        int tile_x = 0, tile_y = 0;   // op13 p1/p2 (loc_12BE2 [eax+6]/[eax+8]-1)
        int portrait_id = 0;          // op13 last word ([ebp+ecx*2+0Ch] -> unk_7D040 -> sub_24BA4)
        std::string name;             // op13 +0Ch, 20 words, low bytes (NUL terminated)
        int sprite_idx = -1;          // op13 p0 ([eax+4] -> high word -> off_53460 index)
        std::string sprite_file;      // file name from the off_53460 table (e.g. SCOUT.NC2)
        NC2Parser sprite;             // CHARACTE/*.NC2 (class 0, 44 records)
        bool sprite_loaded = false;
        int dir = 0;                  // 0 = S (0x50), 1 = W (0x4B), 2 = N (0x48), 3 = E (0x4D)
        int frame_idx = 0;            // op16 absolute frame (0..43), split into dir*11 + pose
        // ---- secondary move state (loc_1336C..13AB2, unk_7D7EF/82938/8293A/82936/8293C) ----
        // The primary op sets counter = (16 or 24) / speed * steps. Each EVE
        // frame then moves 1 px, cycles the walk frame with (counter >> 2) % 4,
        // and commits the tile when counter % interval == 0 (!= 0) or counter
        // reaches 0. fine is the in-tile pixel offset (-23..23 / -15..15,
        // starts at 0): screen = (tile - cam) * size - cam_fine + fine.
        int move_op = 0;              // 0 = idle, 1..8 moving (unk_7D7BC)
        int move_counter = 0;         // remaining pixels (unk_7D7EF)
        int move_speed = 1;           // unk_8293C (op13 sets 1, op27 changes it)
        int fine_x = 0, fine_y = 0;   // in-tile pixel offset
    };
    static constexpr int MAX_EVE_ACTORS = 51;
    EveActorState eve_actors_[MAX_EVE_ACTORS];
    // sub_1A51D slot table unk_82934 (320 slots, 13 bytes each). op19 registers slot
    // p0+2 with type 4 and sprite<<4 at (0,0); op20 clears it. The port renders those
    // NPCs through the town NPC path, so the slots are bookkeeping only (no extra draw).
    struct EveSlot { bool active = false; int type = 0; int sprite = 0; int x = 0, y = 0; };
    EveSlot eve_slots_[320];
    void eve_clear_actors();
    void eve_op_spawn(int actor, const std::vector<uint16_t>& w, size_t base);
    void eve_op_hide(int actor);
    void eve_op_set_dir(int actor, int dir_byte);
    void eve_op_set_frame(int actor, int frame_idx);
    // ---- op1..8 movement (primary loc_12956/1299F/129D8/12A40/12A79, secondary loc_1336C..13AB2)
    // ----
    // Tile delta: op1/5 = up (-Y), op2/6 = down (+Y), op3/7 = left (-X),
    // op4/8 = right (+X).
    // Primary: counter = (Y ? 16 : 24) / speed * steps. Direction and frame are
    // set by op1-4 only; op5-8 keep them (loc_1390D/13970/139DB/13A54 write no
    // frame).
    // Secondary (every frame): screen += speed; op1-4 cycle the walk frame with
    // (counter >> 2) % 4 using op1 [25,24,25,26], op2 [3,2,3,4],
    // op3 [14,13,14,15], op4 [36,35,36,37]; op5-8 keep their frame. The tile
    // commits when counter % interval == 0 (!= 0) or counter == 0.
    void eve_op_move(int actor, int op, int steps);
    // Advance every actor with counter > 0 by 1 px per EVE frame (in parallel),
    // for smooth walking. Returns true while any movement is in progress.
    bool eve_animate_moves_once();
    // Op15 absolute camera (loc_12D97): set cam_tile = (x, y) directly
    void eve_op_camera_snap(int tx, int ty);
    // ---- op23 relative camera (loc_13063 primary + loc_13AC9 secondary + sub_21E29 tail) ----
    // [fire,23,count,dx,dy]: dx and dy are speeds in px per frame. Evidence: the
    // shake cases (283: +-8 over 4 frames, 306: +-14 over 2 frames) and the snap
    // (-12 over 2 frames = -24 px = -1 tile) cannot be read as tile distances
    // but fit a speed exactly. Every EVE frame the secondary path sets
    // 15BA1A = (-dx, dy) and sub_21E29 integrates it into the camera (Y then X).
    // On the last frame counter goes 1 -> 0, 15BA1A is cleared first and 21E29
    // still runs, so the camera integrates count-1 times (271 has count 30 ->
    // 29, world -54 px).
    // The wrap drops the remainder instead of carrying it (X: fine > 24 ->
    // tile--, fine = 0; fine < -24 -> tile++, fine = 0; Y uses 16 the same
    // way). The original's fine sign is opposite to the port
    // (world = tile*size - fine vs world = tile*size + fine), so a shadow
    // variable integrates the original convention and converts back through
    // world with floor.
    void eve_op_camera_pan(int count, int dx, int dy);
    bool get_eve_actor_screen_box(int actor, int& out_left, int& out_top,
                                  int& out_w, int& out_h) const;
    void eve_op_dialogue(int text_id, int actor);
    // ---- shared DAC fade-out engine (sub_208FA, ebx = 0xFF, ecx = 2) ----
    // cur = src*1000 and step = src*1000/32 in fixed point, then 29 iterations
    // of cur -= step. The caller repeats while al == 1 after waiting for a
    // retrace. retraces_per_step = 2 gives 33 ms per step (warp and events,
    // about 0.96 s), 1 gives 16 ms (inn, about 0.5 s). SDL has no DAC, so
    // g_palette is faded by the same amount with a render and present per step.
    // G2_PACK_AUTO skips rendering.
    void fade_out_to_black_rendered(int retraces_per_step);
    // ---- warp fade-out (sub_213A6 loc_213CB + sub_208FA, ebx = 0xFF, ecx = 2) ----
    // op18 -> sub_20EB0 -> sub_213A6 fades the current screen to black
    // (29 steps, 2 retraces each, about 0.96 s). The reload (sub_20118) restores
    // the palette, so there is no fade-in and the picture changes at once.
    // G2_PACK_AUTO skips rendering.
    void eve_fade_out_to_black();
    void eve_op_warp(int town_id, int cam_x = 0, int cam_y = 0);
    // Duplicate suppression marker unk_7FF9C [town 0..255][event 0..39]
    uint8_t& fired_flag(int town, int idx);   // g_event_flags[10000 + town*40 + idx]
    // ---- BGM state (CD audio, state-only port) ----
    // The original BGM is CD-ROM audio (sub_397DD/sub_39E5E/sub_399AC, ASCII
    // commands via sub_3A995). No CD data ships in GENESIS2/, so the port keeps
    // only the track numbers and gating; playback itself is silent.
    int bgm_town_track_ = 0x10;     // dword_82718 (sub_19571)
    int bgm_current_track_ = 0;     // last sub_39E5E argument
    bool bgm_ready_ = false;        // dword_1B62F0 (CD init done)
    int bgm_tick_count_ = 0;        // sub_399AC every 60 iterations
    bool palette_reapply_pending_ = false; // byte_54041 (set after warp/event, consumed in sub_190A3 tail)
    void bgm_init_town();           // sub_19571
    void bgm_play_track(int track); // sub_39E5E(track, 1)
    void bgm_tick();                // sub_399AC
    int bgm_track_for_event(int event_id) const; // sub_2D3B8 (52 cases, 0x10F..0x142)

    // Tile animation (sub_21D70): collects the 20-byte records stored after the
    // MAP palette (group B, one per top tile) whose R[1] > 0. R[0..9] are the
    // rotating top tile numbers, terminated by -1. Every 17 frames the data
    // pointers of R[cur] and R[cur+1] are swapped.
    std::vector<std::array<int16_t, 10>> tile_anim_records_;   // unk_1583D8 (count = word_15BA12)
    std::vector<uint16_t> tile_anim_cur_;                       // unk_570CF
    uint16_t tile_anim_counter_ = 0;                            // word_58CEF
    void update_tile_animation();
    uint16_t story_hi_ = 0; // dword_1A00C8 >> 16, not modelled
    bool click_latch_ = false;   // byte_15BA22: click/action latch, blocks new interactions until the button is released
    uint32_t npc_trigger_ = 0; // dword_8271C, refreshed every frame in town_loop
    // byte_5268C: op17 exit gate. The original sets it to 1 only while sub_16C33
    // runs; 1 prints the "town finished" message and continues the script, 0
    // exits PACK through sub_18FD0. sub_16C33 is not ported, so this stays 0 and
    // every town EVE exits PACK when it ends.
    bool eve_end_no_exit_ = false;
    // Set while run_eve_script runs, to pick the shadow path: the original EVE
    // loop (sub_13B8D) draws only the EVE actor shadows (sub_13D04) and not the
    // player/town NPC shadows (sub_22897) that town_loop draws.
    bool in_eve_cutscene_ = false;

    // ---- Handlers ----
    void town_click_interaction();      // sub_1D12F loc_1D1A5: building/item on the mouse tile (sub_1B344 / sub_1B082)
    void town_key_interaction();        // sub_1A7B5 + sub_1A8F3: action key on the item/building in front
    void handle_chest(int x, int y);
    void show_item_acquired(int item0, const char* name, bool redraw_scene = true); // sub_1B082 loc_1B1BE..loc_1B2C3: dim band + icon + "<name> acquired"
    void render_dim_band(int y0, int y1);                  // sub_18899/sub_187C3: dims x 0..0x13E, y0..y1-1
    void wait_buttons_released();                          // sub_3197D
    void handle_options_menu();

    // ---- NPC interaction (PACK.ASM sub_25F21) ----
    // sub_190A3 (mouse click handling) returns the clicked NPC index and
    // sub_25F21 dispatches on the NPC name. town_loop() calls it on a left click
    // while the mouse is over an NPC (is_mouse_over_npc).
    //   1) name[0..4] == "hired": only the first five bytes are compared, since
    //      the original copies them into a NUL-terminated buffer. Dismisses a
    //      hired companion (handle_npc_dismiss); atoi(name[5..8]) is the member
    //      number.
    //   2) name[0..3] == "hire" and name[4] != 'd': atoi(name[4..]) is the shop
    //      or guild id for mercenary hiring (handle_npc_hire).
    //   3) name is the full 8-byte Johab string for "innkeeper" (loc_261C4):
    //      the inn (handle_inn).
    //   4) otherwise: ordinary dialogue looked up from npc.talk_id in TALK.TBL
    //      (run_dialogue_script).
    uint16_t interact_with_npc(int npc_idx);   // sub_25F21: returns AX (0xFFFF for shop/inn/hire/join, the choice number for dialogue)
    void handle_npc_dismiss(int npc_idx); // "hired" (+ digits): dismiss a hired companion
    void handle_npc_hire(int npc_idx);    // "hireN" (N = shop/guild id): mercenary hiring
    int  run_dialogue_script(int npc_idx); // otherwise: TALK.TBL dialogue (returns choice 1..4, 0 when none)

    // ---- Dialogue window UI (PACK.ASM sub_24DA5/sub_186DC/sub_2B240/sub_2C3A4/sub_24FD5) ----
    // Draws the frame, dimming, portrait, speech tail, text and choices at the
    // placement computed by compute_dialog_placement(). TALK.TBL pagination and
    // PORTRAIT.SYF loading live in talktbl.h/portrait.h; Pack only draws, using
    // its Video/Font and the dialogue sprites loaded above.
    bool compute_dialog_placement(int npc_idx, DialogPlacement& out) const;
    // sub_24DA5 core: placement from the screen box (nx, ny, nw, nh), the raw
    // tile Y and the anchor X (unk_82938, used only for the portrait side test).
    // Shared by the NPC, player and EVE actor wrappers.
    bool compute_dialog_placement_for_box(int nx, int ny, int nw, int nh,
                                          int tile_y, int anchor_x,
                                          DialogPlacement& out, int extra_raw_y = 0) const;
    bool compute_dialog_placement_for_eve(int actor, DialogPlacement& out) const;
    bool compute_dialog_placement_for_player(DialogPlacement& out) const;   // sub_26E32(eax=1): player-relative
    // sub_186DC: dims the background behind the dialogue window. The mask is
    // applied at (X = 0x24, Y = frame_y + 9) with unk_59ABD: 250x76 plus a
    // 19000-byte mask (0xFF = skip). Each pixel becomes index
    // 7 - (R+G+B)/37 from the original integer formula (LUT 15B2B8 is the
    // current 256-colour palette, i.e. g_palette).
    void render_dialog_dimming(int x, int y);
    // sub_24BA4 + sub_2B240: load portrait_id and blit it at
    // (placement.portrait_x, placement.portrait_y - height). PORTRAIT.SYF pixels
    // are RLE like every other sprite, so blit_sprite_rle() is reused.
    void render_dialog_portrait(const DialogPlacement& dp, int portrait_id);
    // Draw the dialogue frame (off_58CF4) and the speech tail (off_5E4F9).
    void render_dialog_frame_and_tail(const DialogPlacement& dp);
    // sub_2C1C9 + sub_2C6AB: draw one line (max 26 bytes) in the given colour.
    void render_dialog_text_line(int x, int y, const std::string& line, uint16_t color_param);
    // Dialogue main loop: page turns on click/space/enter; on the last page
    // non-empty choices go to run_talk_choice().
    // sub_25F21 draws the NPC name in colour 0x0100 on row 1 of page 1 (only
    // while var_8 == 1, and it does not advance the text cursor) and fills rows
    // 2-4 (3 lines); later pages have no name and fill all four rows. pages
    // therefore takes the TalkPage list from talktbl_paginate_npc(), and
    // npc_name is drawn on page 1 only.
    // Returns the selected choice index (0-based), or -1 when there is no choice
    // or the player cancelled. EVE op11 stores it in story_hi_ (dword_1A00C8+2).
    // Shared engine for sub_25F21 (NPC) and sub_2708B (EVE): character-by-
    // character output, and a line starting with "@" opens the choice menu
    // (sub_24FD5/sub_256CD). actor >= 0 places for an EVE actor, npc_idx >= 0
    // for an NPC. Returns the choice number (1..4), 0 when there is none, -1 to
    // quit.
    int run_talk_engine(int npc_idx, int eve_actor, int portrait_id, const std::string& speaker,
                        const std::vector<uint8_t>& buf, int len);
    int run_talk_choice(const DialogPlacement& dp, int portrait_id, const std::vector<uint8_t>& buf, int off);
    bool talk_placement(int npc_idx, int eve_actor, DialogPlacement& dp) const;
    // sub_24FD5 (approximation): lists up to four choices vertically and returns
    // the 0-based index picked by click or arrow keys + confirm. Row Y is
    // dp.text_base_y - 48/-32/-16/0 (the same four-row grid as the body text);
    // the selected row uses colour 0x0100 and the rest 0x1000 (loc_25D50).

    // ---- UI ----
    void show_message_box(const char* text);   // sub_34FBE (approximate appearance)
    int  show_choice_dialog(const char* msg, const char* opt1, const char* opt2);

    // ---- Shop/inn/list UI (pack_shop.cpp) ----
    // team_gold_ is dword_19FF44: copied from g_slot_gold[slot] when loading
    // finishes and written back when saving starts.
    uint32_t team_gold_ = 0;
    // unk_172224: screen snapshot {u16 width 0x13F, u16 height 0xC7, pixels}; saved
    // by sub_2B3DC, restored by sub_2B374/sub_2B2A1.
    std::vector<uint8_t> shop_bg_;
    std::vector<uint8_t> gold_box_data_;     // off_315FE (sub_2B240 RLE)
    std::vector<uint8_t> scroll_up_data_;    // off_312C2 (16x10 raw)
    std::vector<uint8_t> scroll_down_data_;  // off_31366 (16x10 raw)
    std::vector<std::vector<uint8_t>> option_sprites_;   // 13 RLE sprites for the option screen and confirm boxes
    int  confirm_box(const char* msg);        // sub_2D4A3: confirm box (1 yes / -1 no / 0 quit requested)
    bool shop_assets_loaded_ = false;
    int  shop_scroll_ = 0;                   // dword_1A4F14: index of the first visible row

    void load_shop_assets();
    // sub_394D1: play a SoundBlaster VOC with volume byte_82732*10; skipped when
    // byte_82732 == 0 (and when the driver is not ready). id 1 = menu.VOC, id 2 = sell.VOC
    // (sub_18BC5); other ids are playable via the explicit path overload.
    void play_pack_sfx(int id);
    void play_pack_voc_path(const std::string& path);
    void shop_grab_screen();                              // sub_2B3DC(0,0,unk_172224,0x13F,0xC7)
    void shop_restore_screen();                           // sub_2B374(0,0)
    void shop_present();                                  // sub_2B44F / sub_2B470 (back buffer -> screen)
    bool poll_mouse(int& status, int& x, int& y);         // sub_19924 (status 1 = left, 2 = right)
    void wait_right_released();                           // sub_319DB
    int  run_yes_no(const std::vector<uint8_t>& base);    // yes/no hover and click loop
    std::vector<uint8_t> draw_confirm_screen(int item1, const char* name, int price, const char* prompt);
    void sh_hline(int x, int y, int len, uint8_t c);      // sub_2B4D6
    void sh_vline(int x, int y, int len, uint8_t c);      // sub_2B4A8
    void sh_fill(int x, int y, int w, int h, uint8_t c);  // sub_2B56C
    void sh_raw_blit(int x, int y, const std::vector<uint8_t>& bmp); // sub_2B2A1 (0xFF = transparent)
    void draw_frame_box(int x0, int y0, int x1, int y1, int fill = -1);  // sub_1843D (fill = byte_53D0B, changed by sub_189FA)
    void small_text(int x, int y, const char* s);         // sub_18274 (small 8x8 font, colour 1)
    void small_number(int x, int y, int v);               // sub_1822D
    void sh_text(int x, int y, const char* s, uint16_t color); // sub_2C1C9(4,color,0xFF) + sub_2C6AB
    void draw_gold_box();                                 // sub_3507E(4,4)
    void change_gold(int delta);                          // sub_35109
    void draw_item_stat_panel(int item1);                 // sub_37A56 (item1 = 1-based item number)
    int  inv_insert_at(int item0, int pos);               // sub_22E57 (insert into the 100-slot limit)
    void handle_building_buy(int building_value);         // sub_1B344
    int  sell_item(int item1);                            // sub_1B75B (-1 cancel, 0 cannot sell, 1 sold)
    int  run_item_list();                                 // sub_337A2 (selected item byte, 0 on cancel)
    void draw_item_list(int selected);                    // sub_33534
    void handle_inn(int talk_id);                         // sub_1ABA6
    void handle_shop_owner(int npc_idx);                  // sub_25F21 shop-owner branch
    void shop_bubble(int npc_idx, int portrait_id, const char* name,
                     const char* l1, const char* l2, const char* l3); // sub_26E32
    bool warp_to_town(int new_town_id);                   // sub_20EB0 + sub_213A6
    void wait_click();                                    // sub_31933: wait for a new click or key
    int  hire_merc(int npc_idx, int id);                  // sub_1BC43 (1 = hired)
    void eve_scroll_text(int talk_id);                    // sub_17EFF (EVE op24): vertical scroll of TALK text
    int  rejoin_member(int npc_idx, int member);          // sub_1C6AB (1 = a waiting companion rejoins the party)
    void join_team(int char_id);                          // sub_237F0 (add to the party)

    // ==== Status and equipment menus ====
    // PACK_code.ASM sub_32315 / sub_31CC8 / sub_320BB / sub_357C4 / sub_367B0 /
    // sub_2F789 / sub_34261.
    // Party records use the 0x88-byte records in blackbox.dat (g_characters)
    // directly. sub_22FED copies only records whose team byte (+0x87) equals
    // word_1A00CC into 0x19AC24 on load and sub_230AE copies them back on exit;
    // the port edits the original records, so no copy-back is needed.
    static constexpr int kPartyRecSize = 0x88;
    int party_count() const;
    uint8_t* party_rec(int i);
    const uint8_t* party_rec(int i) const;

    void status_window();                                  // sub_32315
    void status_draw_main(int sel, int top);               // sub_31CC8
    void status_draw_party_list(int top, int sel);         // sub_333C7
    void status_draw_thumb();                              // sub_1843D(0x87..0x96)
    void status_draw_skill_grid(const uint8_t* rec, int x0);   // loc_32603~loc_32AAD
    void status_item_popup(int item1);                     // sub_320BB
    void status_item_stats_panel(int item1);               // sub_3777C
    void status_equip_icon(int x, int item1);              // unk_9922E icon placement
    void status_gold_box_at(int x, int y);                 // sub_3507E(x,y)
    void status_small_text(int x, int y, const char* s, uint8_t color);   // sub_18136+sub_18274
    void status_small_number(int x, int y, int v, uint8_t color);         // sub_1822D
    void status_load_assets();
    bool status_load_pcx(const char* name, bool with_palette);   // sub_2E46B/sub_2E392
    void status_slide_back(int sel, bool from_item_list);  // sub_37F18 transition
    float status_thumb_height(int count);                  // dword_1A4F18
    int  status_scroll_to_top(int off, int h, int count);  // sub_3205E
    int  status_top_to_scroll(int top, int count);         // sub_320A4
    void status_hand_cursor();                             // 0x3115E (14x12, sub_2B2A1)

    // Sub-screens (pack_equip.cpp / pack_class.cpp / pack_itemlist.cpp)
    int  status_equipment_screen(int sel);   // sub_357C4 (0 = close the status window, 1 = back)
    void equip_drag(int sel, int item0, int src, int top);   // sub_367B0
    int  status_class_screen(int sel);       // sub_2F789 (0 = close, -1 = not allowed, 1 = back)
    void status_inventory_list();            // sub_34261
    // Item equip/unequip (sub_37E5E). Slot 1 = weapon, 2 = shield, 3 = armour.
    void status_equip_apply(uint8_t* rec, int item0, int slot);
    void status_equip_strip(uint8_t* rec, int slot);
    bool status_is_kept_item(int item_plus1) const;   // sub_334F9

    // Status window state (original dword_1A4F10/1A4F14/1A4F18/1A4F1C)
    int   status_sel_ = 0;
    int   status_top_ = 0;
    float status_thumb_h_ = 123.0f;
    float status_thumb_off_ = 0.0f;
    std::vector<uint8_t> status_bg_;        // status.pcx (loaded with its palette)
    std::vector<uint8_t> status_prev_;      // previous screen for the transition
    std::vector<uint8_t> status_snap_;      // new screen for the transition
    std::vector<uint8_t> status_icons_;     // off_70A30, 53 x 0x1AE
    std::vector<uint8_t> status_slot_;      // unk_7C784 (skill slot frame)
    std::vector<uint8_t> status_hand_;      // 0x3115E, 14x12 raw (0xFF transparent)
    std::vector<uint8_t> status_btn_a_;     // unk_7C220
    std::vector<uint8_t> status_btn_b_;     // unk_7C4D2
    bool status_assets_loaded_ = false;

    // ---- Blitters (sub_2B240 and sub_2B2A1) ----
    void do_transition(const std::string& cmd);
    std::string game_path(const char* filename);
    std::string cstldata_path(const char* filename);
    std::string characte_path(const char* filename);
    void blit_tile_24x16(int x, int y, const uint8_t* src_pixels);
    void blit_top_tile_transparent(int x, int y, const PackSprite& sp);
    void blit_sprite_rle(int x, int y, const uint8_t* rle_data);
    // sub_2B2A1: raw bitmap blitter (0xFF = transparent)
    void blit_cursor_raw(int x, int y, const PackCursorFrame& frame);
};