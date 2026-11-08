#include "pack.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "audio.h"
#include "pcx.h"
#include "palette.h"
#include "talktbl.h"
#include "portrait.h"
#include "game_timer.h"
#include "pack_data.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <queue>
#include <SDL.h>

// Development-only environment hooks (G2_PACK_*). Not present in the original: always
// disabled in normal builds and enabled only with -DPACK_DEBUG_HOOKS.
#ifdef PACK_DEBUG_HOOKS
static const char* dbg_getenv(const char* name) { return getenv(name); }
#else
static const char* dbg_getenv(const char*) { return nullptr; }
#endif
static bool dbg_env(const char* name) { return dbg_getenv(name) != nullptr; }

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

Pack::Pack(Video& video, Input& input) : video_(video), input_(input) {}

Pack::~Pack() {
    free_town_data();
    video_.cursor_reset();   // drop this module's cursors and fall back to the system cursor
}

std::string Pack::game_path(const char* filename) {
    return file_find_case_insensitive(GAME_DATA_DIR, filename);
}

std::string Pack::cstldata_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CSTLDATA", filename);
}

std::string Pack::characte_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CHARACTE", filename);
}

// sub_394D1: PLAY a loaded VOC. The caller passes volume = byte_82732 * 10 and the gate
// on dword_1B61D4 == 1 (driver ready) plus id < 0x50. The port has no driver-ready flag,
// but byte_82732 == 0 (sound off) always skips, matching the callers' explicit volume.
// PACK.EXE only embeds menu.VOC (id 1) and sell.VOC (id 2) via sub_18BC5; EVE op29
// loads its own file into slot 10 through sub_39431/sub_394D1.
void Pack::play_pack_voc_path(const std::string& path) {
    if (game_cfg_[2] == 0) return;
    Audio::play_voc(path, (int)game_cfg_[2] * 10);
}

void Pack::play_pack_sfx(int id) {
    if (id == 1) play_pack_voc_path(game_path("menu.voc"));
    else if (id == 2) play_pack_voc_path(game_path("sell.voc"));
}

// ---- sub_19571: town BGM track (main entry) ----
// Towns 1, 7, 9, 12, 18 (0x12), 19 (0x13) -> 0x0E; towns 10, 11, 29, 30, 32 ->
// 0x0F; all others -> 0x10. byte_15BA23 is the town id.
void Pack::bgm_init_town() {
    const int t = town_id_ & 0xFF;
    if (t == 1 || t == 7 || t == 9 || t == 12 || t == 0x12 || t == 0x13) bgm_town_track_ = 0x0E;
    else if (t == 10 || t == 11 || t == 29 || t == 30 || t == 32) bgm_town_track_ = 0x0F;
    else bgm_town_track_ = 0x10;
}

// ---- sub_39E5E(track, loop): select a CD track ----
// The original returns immediately when dword_1B62F0 == 0 (CD not initialised).
// The port has no CD, so it only records the selection; gating on byte_82731
// (music on) stays with the callers, as in the original.
void Pack::bgm_play_track(int track) {
    if (!bgm_ready_) return;
    bgm_current_track_ = track;
    fprintf(stderr, "PACK: BGM track 0x%X (state-only, CD audio not available)\n", track);
}

// ---- sub_399AC: audio tick ----
// Called every 60 town iterations (sub_19528) and every 60 EVE frames (sub_13B8D).
// With no CD the tick is a no-op; the original restarts the current track on completion.
void Pack::bgm_tick() {
}

// ---- sub_2D3B8: event BGM table (off_2D2E8, 52 cases) ----
// eid - 0x10F indexes the jump table; out-of-range eids use the default 0x0B.
// The table below is decoded from PACK_code.ASM 37115..37203 (label -> track):
// loc_2D3DC=0x0D, loc_2D3EB=0x02, loc_2D3FA=0x14, loc_2D409=0x0B,
// loc_2D418=0x09, loc_2D427=0x09, loc_2D436=0x11, loc_2D442=0x10,
// loc_2D44E=0x11, loc_2D455=0x14, loc_2D45C=0x0E, loc_2D468=0x10,
// loc_2D46F=0x08, loc_2D476=0x02, loc_2D47D=0x0D, loc_2D484=0x03,
// loc_2D48B=0x07, loc_2D492=0x0B (default).
int Pack::bgm_track_for_event(int event_id) const {
    static const int kTable[52] = {
        13, 2, 2, 9, 17, 17, 2, 2,
        17, 13, 17, 17, 16, 2, 2, 14,
        14, 20, 11, 11, 20, 14, 9, 20,
        11, 11, 17, 16, 16, 20, 9, 16,
        9, 9, 9, 8, 2, 13, 9, 2,
        13, 8, 9, 3, 20, 9, 8, 20,
        9, 2, 8, 7,
    };
    const int sw = event_id - 0x10F;
    if (sw < 0 || sw >= 52) return 0x0B;
    return kTable[sw];
}

// ---- off_53460: EVE actor sprite filename table ----
// Flattened form of the 222-entry `dd offset aXXX_nc2` pointer array in dseg02. The last
// word of an op13 record indexes this table, and the NC2 is loaded from
// ".\\characte\\" + filename (loc_12BE2..loc_12D03, sub_1A2D6). Lower-case entries such as
// nullman.nc2 and bottle.nc2 are kept verbatim; file_find_case_insensitive resolves them.
static const char* const kEveSpriteFiles[] = {
    "KNIGHT1.NC2","KNIGHT2.NC2","KNIGHTW.NC2","PARADIN.NC2","IMPEPARA.NC2","PARADINW.NC2",
    "FIGHTER.NC2","FIGHTER2.NC2","FIGHTERW.NC2","ASSASIN.NC2","GRDIATER.NC2","GRADIAW.NC2",
    "THIEF.NC2","THIEFW.NC2","RANGER.NC2","RANGW.NC2","SCOUT.NC2","SCOUTW.NC2",
    "ARCHAEO.NC2","ARCHEOW.NC2","SOLDIER1.NC2","SOLDIER2.NC2","SOLDIERW.NC2",
    "LANCER1.NC2","LANCER2.NC2","LANCERW.NC2","GENERA.NC2","GENERAL2.NC2","GENERAW.NC2",
    "GUARD.NC2","GUARDW.NC2","ROYALG.NC2","ROYALGW.NC2","EARTHQ.NC2","EARTHQW.NC2",
    "ARCHER1.NC2","ARCHER2.NC2","ARCHERW.NC2","HUNTER.NC2","HUNTER2.NC2","HUNTERW.NC2",
    "EAGLE.NC2","EAGLED.NC2","EAGLEW.NC2","SNIPER.NC2","SNIPERW.NC2",
    "MAGE1.NC2","MAGE2.NC2","MAGICIA1.NC2","MAGICIA2.NC2","ALCHEMI1.NC2","ALCHEMI0.NC2",
    "M_FIRE1.NC2","M_FIRE2.NC2","M_THUN1.NC2","M_THUN2.NC2","M_ICE1.NC2","M_ICE2.NC2",
    "WIZARD1.NC2","WIZARD2.NC2","MONK.NC2","SISTER.NC2","CLERIC.NC2","CLERICKW.NC2",
    "PRIEST2.NC2","PRIEST1.NC2","DRUID.NC2","BISHOP.NC2","WARLOCK1.NC2","WARLOCK2.NC2",
    "WITCH1G.NC2","WITCH1F.NC2","WITCH2G.NC2","WITCH2F.NC2","NECROMAN.NC2","LICH.NC2",
    "WARRIOR1.NC2","WARRIOR2.NC2","BERSERKE.NC2","GENOCID.NC2","SORCERER.NC2","SHARMAN.NC2",
    "EXORCIST.NC2","VALTANSG.NC2","nullman.nc2","VALTANAG.NC2","bottle.nc2","VALTANLG.NC2",
    "e_alcia.nc2","E_VAL_AG.NC2","e_boy.nc2","E_VAL_LG.NC2","e_cameo.nc2",
    "MARMANAS.NC2","MERMANL.NC2","MARMANA.NC2","BRIGAND.NC2","e_drau.nc2","PIRATE.NC2",
    "e_dre.nc2","e_duran.nc2","ORIENTAL.NC2","NORMADIC.NC2","e_erich.nc2","e_gs.nc2",
    "e_gsdan.nc2","CAPTAIN.NC2","SAVANT1.NC2","SAVANT2.NC2","e_higen.nc2","e_iyolin.nc2",
    "e_iydan.nc2","e_lacid.nc2","e_larwi.nc2","e_loca.nc2","e_plus.nc2","e_ser3.nc2",
    "e_shuri.nc2","BAT4.NC2","VAMPIRE.NC2","VAMPIREW.NC2","e_coboy.nc2","e_coiy01.nc2",
    "e_coiy02.nc2","e_last.nc2","Z_GHOUL1.NC2","Z_GHOUL2.NC2","Z_GHOUL3.NC2",
    "SLIME1.NC2","SLIME2.NC2","SLIME3.NC2","SLIME4.NC2","SKELETON.NC2",
    "DTOOTH01.NC2","DTOOTHW1.NC2","BP.NC2","S_GS.NC2","L_GS.NC2","B_GS.NC2","M_GS.NC2",
    "STYNER.NC2","NS_GS.NC2","NL_GS.NC2","NB_GS.NC2","NM_GS.NC2","BERADIN.NC2",
    "S_LACIDE.NC2","L_LACIDE.NC2","B_LACIDE.NC2","M_LACIDE.NC2","HLACID.NC2",
    "NS_LACIDE.NC2","NL_LACIDE.NC2","NB_LACIDE.NC2","NM_LACIDE.NC2",
    "S_IYOLIN.NC2","L_IYOLIN.NC2","B_IYOLIN.NC2","M_IYOLIN.NC2",
    "S_ROBIN.NC2","L_ROBIN.NC2","B_ROBIN.NC2","M_ROBIN.NC2",
    "S_KAJA.NC2","L_KAJA.NC2","B_KAJA.NC2","M_KAJA.NC2",
    "S_GISHNE.NC2","L_GISHNE.NC2","B_GISHNE.NC2","M_GISHNE.NC2",
    "S_SHURI.NC2","L_SHURI.NC2","B_SHURI.NC2","M_SHURI.NC2",
    "S_CROW.NC2","L_CROW.NC2","B_CROW.NC2","M_CROW.NC2",
    "S_ARCION.NC2","L_ARCION.NC2","B_ARCION.NC2","M_ARCION.NC2",
    "S_ROVEL.NC2","L_ROVEL.NC2","B_ROVEL.NC2","M_ROVEL.NC2",
    "AKIRA.NC2","AGACYG.NC2","AGACYF.NC2","ALCIA.NC2","ARIANA.NC2","CAMEO.NC2",
    "DBLADE.NC2","DIABLRO.NC2","DICUS.NC2","DURAN.NC2","ERICH.NC2","GRIUM.NC2",
    "HANJO.NC2","ILJIMEA.NC2","IRUS.NC2","KARLS.NC2","KASHIM.NC2","KASHUTAR.NC2",
    "KOON.NC2","LANDAM.NC2","NANGCHUN.NC2","PALTAMIL.NC2","ROCARNO.NC2",
    "SAAD-G.NC2","SAAD_F.NC2","SARA.NC2","SIREN.NC2","VUNSTAIN.NC2","DRAUPNIR.NC2",
    "SGRAY.NC2","LGRAY.NC2","BGRAY.NC2","MGRAY.NC2","VANDYKE.NC2","KIM.NC2",
};
static constexpr int kEveSpriteFileCount = sizeof(kEveSpriteFiles) / sizeof(kEveSpriteFiles[0]);

// PACK direction byte -> NC2 dir (combat facing order: 0=S, 1=W, 2=N, 3=E).
static int pack_dir_byte_to_nc2(int dir_byte) {
    if (dir_byte == 0x50) return 0; // Down/S
    if (dir_byte == 0x4B) return 1; // Left/W
    if (dir_byte == 0x48) return 2; // Up/N
    if (dir_byte == 0x4D) return 3; // Right/E
    return 0;
}

// Raw NC2 blit (w*h, 0xFF = transparent). Uncompressed, unlike the citizen RLE path in
// blit_sprite_rle, so it follows the same convention as the cursor/shadow raw blits.
// x_off/y_off are added by the caller.
static void blit_nc2_frame(uint8_t* backbuf, int stride_w, int stride_h,
                            int x, int y, const NC2Frame& fr) {
    if (!backbuf || fr.w <= 0 || fr.h <= 0 || fr.pixels.empty()) return;
    for (int row = 0; row < fr.h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= stride_h) continue;
        for (int col = 0; col < fr.w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= stride_w) continue;
            uint8_t px = fr.pixels[(size_t)row * fr.w + col];
            if (px == 0xFF) continue;
            backbuf[dy * stride_w + dx] = px;
        }
    }
}

// ---- load_frame_overlays: left/right screen frame sprites (sub_2294D loc_22A01) ----
// Reads the Softmax RLE sprites that were hardcoded in dseg02: unk_54630 (left, 25 px wide
// x 200 px tall) and off_55A99 (right, X offset 295), converting VA to file offset through
// the LE page table. They do not depend on the town, so one load covers every map.
bool Pack::load_frame_overlays() {
    const bool ok_left  = exe_resource_load(ExeId::Pack, 0x54630, ExeResLength::Rle, left_frame_data_);
    const bool ok_right = exe_resource_load(ExeId::Pack, 0x55A99, ExeResLength::Rle, right_frame_data_);
    if (!ok_left)  fprintf(stderr, "PACK: warning: PACK.EXE VA 0x54630 (left frame overlay) load failed\n");
    if (!ok_right) fprintf(stderr, "PACK: warning: PACK.EXE VA 0x55A99 (right frame overlay) load failed\n");
    return ok_left && ok_right;
}

// Cursor hotspots decoded word by word from dword_18A00 / dword_18A10 (see
// render_mouse_cursor). Order: up, up-pressed, down, down-pressed, left, left-pressed,
// right, right-pressed. Software blits draw at (mouse + value); the hardware cursor uses
// -value as the hotspot.
static const int8_t kCursorHotX[8] = { -4, -4, -4, -4,  0,  0, -7, -7 };
static const int8_t kCursorHotY[8] = {  0,  0, -7, -7, -4, -4, -4, -4 };

// ---- load_cursor_sprites: 8-way arrow cursor (off_53D0C) ----
// Reads 800 bytes (0x320) from VA 0x53D0C through the LE object/page table (Object 2,
// reloc base 0x50000). Eight fixed 100-byte entries: width u16, height u16,
// pixels[width*height] (0xFF = transparent); the remaining padding is unused.
bool Pack::load_cursor_sprites() {
    std::vector<uint8_t> raw;
    if (!exe_resource_load(ExeId::Pack, 0x53D0C, ExeResLength::Fixed, raw, 800)) {
        fprintf(stderr, "PACK: warning: PACK.EXE VA 0x53D0C (mouse cursor sprites) load failed - cursor will not be drawn\n");
        return false;
    }

    cursor_sprites_.clear();
    for (size_t off = 0; off + 100 <= raw.size(); off += 100) {
        PackCursorFrame frame;
        frame.width  = raw[off] | (raw[off + 1] << 8);
        frame.height = raw[off + 2] | (raw[off + 3] << 8);
        uint32_t px_count = (uint32_t)frame.width * frame.height;
        if (px_count > 96) px_count = 96; // 100-byte entry minus the 4-byte header
        frame.pixels.assign(raw.begin() + (long)off + 4, raw.begin() + (long)off + 4 + px_count);
        cursor_sprites_.push_back(std::move(frame));
    }
    if (cursor_sprites_.size() != 8) return false;

    // Register the frames with Video, which builds the scaled SDL cursors (video.cpp).
    // Reset first so reloading a town does not register duplicates; Pack is the only
    // module using cursors here. The hotspot is -HOTSPOT so that it matches the software
    // blit position (mx + HOTSPOT).
    video_.cursor_reset();
    cursor_base_ = -1;
    for (size_t i = 0; i < cursor_sprites_.size(); i++) {
        const PackCursorFrame& fr = cursor_sprites_[i];
        CursorFrame cf;
        cf.width = fr.width;
        cf.height = fr.height;
        cf.hot_x = -kCursorHotX[i];
        cf.hot_y = -kCursorHotY[i];
        cf.pixels = fr.pixels;
        int id = video_.cursor_register(std::move(cf));
        if (i == 0) cursor_base_ = id;
    }
    return true;
}

// ---- load_shadow_sprite: foot shadow for the player and NPCs (off_56C00) ----
// Reads 137 bytes (0x89) from VA 0x56C00 via the LE page table (Object 2, reloc base
// 0x50000). Same single-sprite sub_2B2A1 layout as the cursor (width u16, height u16,
// pixels[width*height], 0xFF = transparent) but variable length. The original has exactly
// one shadow: sub_22897 reuses the same off_56C00 pointer for the player and every NPC.
bool Pack::load_shadow_sprite() {
    std::vector<uint8_t> raw;
    if (!exe_resource_load(ExeId::Pack, 0x56C00, ExeResLength::Sprite, raw)) {
        fprintf(stderr, "PACK: warning: PACK.EXE VA 0x56C00 (foot shadow sprite) load failed - shadows will not be drawn\n");
        return false;
    }
    if (raw.size() < 4) return false;

    shadow_sprite_.width  = raw[0] | (raw[1] << 8);
    shadow_sprite_.height = raw[2] | (raw[3] << 8);
    const uint32_t px_count = (uint32_t)shadow_sprite_.width * shadow_sprite_.height;
    if ((size_t)4 + px_count > raw.size()) return false;

    shadow_sprite_.pixels.assign(raw.begin() + 4, raw.begin() + 4 + (long)px_count);
    return true;
}

// ---- load_dialogue_sprites: NPC dialogue window assets ----
// Reads off_58CF4, unk_7C4D2, unk_7C220 and off_5E4F9 through the LE page table. The first
// three are sub_2B240 RLE (uint16 chunk_count, then repeating [uint16 skip, uint8 dword_count,
// uint8 byte_count, (dword_count*4 + byte_count) bytes], self-terminating); speech_tail is
// eight fixed 200-byte entries. Pass each vector.data() to blit_sprite_rle (for off_5E4F9,
// .data() + SPEECH_TAIL_STRIDE*index) to draw them exactly as the original does.
// unk_59ABD is deliberately excluded: nothing in the original uses it as sprite pixels
// (see the pack.h field note).
bool Pack::load_dialogue_sprites() {
    const bool ok1 = exe_resource_load(ExeId::Pack, 0x58CF4, ExeResLength::Rle, dialog_frame_data_);
    const bool ok2 = exe_resource_load(ExeId::Pack, 0x7C4D2, ExeResLength::Rle, ok_button_data_);
    const bool ok3 = exe_resource_load(ExeId::Pack, 0x7C220, ExeResLength::Rle, cancel_button_data_);
    const bool ok4 = exe_resource_load(ExeId::Pack, 0x5E4F9, ExeResLength::Fixed, speech_tail_data_,
                                       (size_t)SPEECH_TAIL_STRIDE * SPEECH_TAIL_COUNT);
    if (ok4 && (long)speech_tail_data_.size() != SPEECH_TAIL_STRIDE * SPEECH_TAIL_COUNT) {
        fprintf(stderr, "PACK: speech_tail size mismatch (expected %d, got %zu)\n",
                SPEECH_TAIL_STRIDE * SPEECH_TAIL_COUNT, speech_tail_data_.size());
    }
    // unk_59ABD dimming mask: u16 250 x u16 76 header, then 19000 bytes of mask.
    // Without it the code falls back to a solid rectangle (render_dialog_dimming).
    if (!exe_resource_load(ExeId::Pack, 0x59AC1, ExeResLength::Fixed, dimming_mask_,
                           (size_t)DIMMING_W * DIMMING_H)) {
        fprintf(stderr, "PACK: warning: PACK.EXE VA 0x59ABD shade mask load failed - solid fallback dimming\n");
        dimming_mask_.clear();
    }
    // AHhhhhhhhhhhhhh (VA 0x60A40): 320x200 mouse-position -> direction table.
    if (!exe_resource_load(ExeId::Pack, 0x60A40, ExeResLength::Fixed, mouse_dir_table_, 320 * 200)) {
        fprintf(stderr, "PACK: warning: PACK.EXE VA 0x60A40 mouse direction table load failed - using an approximate quadrant test\n");
        mouse_dir_table_.clear();
    }
    return ok1 && ok2 && ok3 && ok4;
}

// ---- place_chest_top_tiles: chest top tiles (sub_213A6 loc_216E4, sub_24017 loc_243C8) ----
// sub_24017 walks layer_item_ (dword_15B9BC) and treats any cell with item_id % 1000 >=
// 500 (0x1F4) as a chest, writing the chest top-tile index into layer_top_tiles_
// (dword_15B9C8) one row below it (Y+1, X). sub_213A6 appends two copies of itembox.spr
// frames 0 and 1 to top_tile_sprites_ (word_8B128 = top_tile_count + 2). itembox_sprites_
// loads after load_map_file in run(), so this runs right after that load.
void Pack::place_chest_top_tiles() {
    if (!layer_item_ || !layer_top_tiles_) return;
    if (itembox_sprites_.size() < 2) return;
    if (chest_closed_tile_idx_ >= 0) return; // already placed; do not duplicate on a town reload

    // sub_213A6 loc_216E4..loc_218B8: append two top tiles. The closed chest copies the
    // first 0x1F4 (500) bytes of dword_8E96C (itembox group 3, frame 0) and the open chest
    // the first 0x2BC (700) bytes of dword_8E970 (frame 1). The header is rewritten as
    // width 0x18, height 0x10, x/y offset 0, foot = height + y_off - 10 = 6.
    auto make_chest_tile = [&](const PackSprite& src, size_t bytes) {
        PackSprite t;
        t.width = 0x18;
        t.height = 0x10;
        t.x_offset = 0;
        t.y_offset = 0;
        t.foot_offset_y = (int16_t)(t.height + t.y_offset - 10);
        t.data_size = (uint32_t)bytes;
        t.data.assign(bytes, 0);   // the original only copies into nmalloc'ed memory, so zero-fill the tail
        std::copy_n(src.data.begin(), std::min(bytes, src.data.size()), t.data.begin());
        return t;
    };
    chest_closed_tile_idx_ = (int)top_tile_sprites_.size();
    top_tile_sprites_.push_back(make_chest_tile(itembox_sprites_[0], 0x1F4)); // closed chest (count-2)
    chest_open_tile_idx_ = (int)top_tile_sprites_.size();
    top_tile_sprites_.push_back(make_chest_tile(itembox_sprites_[1], 0x2BC)); // open chest (count-1)

    const uint32_t top_tile_buf_size = map_w_ * (map_h_ + 6);

    // sub_24017 loc_243BC..loc_244BF: x over 0..w-1, y over 0..h-2 (`y < h-1`).
    //   v = item[y][x]; skip when v % 1000 < 500 (500 itself is a chest cell);
    //   q = v / 1000; skip when (q / 2) > g_event_flags[0xC8].
    //   top tile at (y+1, x) = g_event_flags[600 + q] ? open chest : closed chest, and
    //   the passable cell at (y, x) becomes 1.
    for (uint32_t x = 0; x < map_w_; x++) {
        for (uint32_t y = 0; y + 1 < map_h_; y++) {
            const uint16_t v = layer_item_[y * map_w_ + x];
            if (v % 1000 < 500) continue;
            const uint32_t q = v / 1000;
            if ((int)(q / 2) > (int)g_event_flags[0xC8]) continue;
            const uint32_t below_idx = (y + 1) * map_w_ + x;
            if (below_idx < top_tile_buf_size) {
                layer_top_tiles_[below_idx] = (uint16_t)(g_event_flags[600 + q] != 0
                                                          ? chest_open_tile_idx_ : chest_closed_tile_idx_);
            }
            if (layer_passable_) layer_passable_[y * map_w_ + x] = 1;
        }
    }
}

void Pack::free_town_data() {
    delete[] layer_base_tiles_;  layer_base_tiles_ = nullptr;
    delete[] layer_top_tiles_;   layer_top_tiles_ = nullptr;
    delete[] layer_passable_;    layer_passable_ = nullptr;
    delete[] layer_event_;       layer_event_ = nullptr;
    delete[] layer_item_;        layer_item_ = nullptr;
    delete[] layer_building_;    layer_building_ = nullptr;
    delete[] layer_npc_;         layer_npc_ = nullptr;
    delete[] path_cost_grid_;    path_cost_grid_ = nullptr;
    base_tiles_data_.clear();
    top_tile_sprites_.clear();
    // top_tile_sprites_ is cleared, so invalidate the chest indices that pointed into it;
    // place_chest_top_tiles() rebuilds them after the next town load.
    chest_closed_tile_idx_ = -1;
    chest_open_tile_idx_ = -1;
    // Citizen and item sprites are loaded once and reused across towns, so they are kept.
    // Reset the NPC movement state: stale fine offsets and subpixel accumulators from the
    // previous town would make the new town's NPCs jump on their first step.
    memset(npc_fine_offset_, 0, sizeof(npc_fine_offset_));
    memset(npc_subpixel_, 0, sizeof(npc_subpixel_));
}

// ---- sub_1A712: load ITEM.DAT ----
void Pack::load_item_data() {
    if (items_loaded_) return;
    std::string path = game_path("item.dat");
    if (!file_exists(path.c_str())) path = game_path("ITEM.DAT");

    FILE* fp = fopen(path.c_str(), "rb");
    if (fp) {
        for (int i = 0; i < MAX_ITEMS; i++) {
            if (fread(item_data_[i], 1, ITEM_SIZE, fp) < ITEM_SIZE) break;
        }
        fclose(fp);
        items_loaded_ = true;
    }
}

// ---- sub_235A7: load GAME.CFG ----
// On open failure the original prints "game.cfg open error!" and returns without touching
// byte_82730 and the other settings, which stay at their BSS default of 0. On success it
// reads 10 bytes starting at byte_82730; only offset 0 (movement speed) is used.
// update_player_movement() (sub_1D12F) re-reads byte_82730 at the start of every tile step
// to pick SLOW (3,2 / 8 steps) or FAST (6,4 / 4 steps). Missing file or 0 means SLOW.
void Pack::load_game_config() {
    std::string path = game_path("game.cfg");
    if (!file_exists(path.c_str())) path = game_path("GAME.CFG");

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "PACK: warning: cannot open game.cfg\n");
        return; // keep the byte_82730 default (0 = SLOW)
    }

    uint8_t cfg[10] = {0};
    fread(cfg, 1, sizeof(cfg), fp);
    fclose(fp);
    memcpy(game_cfg_, cfg, sizeof(cfg));

    config_move_speed_ = cfg[0]; // byte_82730: 0 = SLOW (normal), 1 = FAST
    move_steps_per_tile_ = (config_move_speed_ == 1) ? MOVE_STEPS_FAST : MOVE_STEPS_SLOW;
}

// ---- sub_23369: load BLACKBOX.DAT ----
// Opens blackbox.dat only (the original has no blackbox.def fallback). On failure it prints
// "blackbox.dat read error!" and the caller exits. The file is unlinked after loading,
// exactly as the original does; sub_23147 recreates it on exit.
bool Pack::load_blackbox() {
    std::string path = game_path("blackbox.dat");
    if (!file_exists(path.c_str())) path = game_path("BLACKBOX.DAT");

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "PACK: error: cannot read blackbox.dat\n");
        return false;
    }
    loaded_blackbox_path_ = path;

    // loc_233B0: skip the leading byte (lseek 1, SEEK_CUR). It is unk_52698, the first
    // field written by sub_23147; nothing reads its value back, so it is skipped.
    fseek(fp, 1, SEEK_CUR);

    // Only two fields precede g_event_flags: quest_counter (2 bytes, unk_1A00C4) and
    // sec_state (1 byte, unk_1A00DB).
    uint16_t quest_counter;
    uint8_t sec_state;
    fread(&quest_counter, 1, 2, fp);
    fread(&sec_state, 1, 1, fp);
    blackbox_day_ = quest_counter;   // unk_1A00C4: day counter, preserved verbatim
    blackbox_sec_ = sec_state;       // unk_1A00DB: secondary state, preserved verbatim
    fread(g_event_flags, 1, 0x4E20, fp);

    uint16_t ch_id;
    fread(&ch_id, 2, 1, fp);
    blackbox_slot_ = ch_id;          // word_1A00CC: team/gold slot, must not be overwritten with town_id

    for (int i = 0; i < MAX_CHARACTERS; i++) {
        fread(&g_characters[i], sizeof(CharacterData), 1, fp);
    }

    // loc_2344D..: 100 slots of {4-byte gold (dword_15FB7C), 2-byte count (word_19763C),
    // count bytes of items (unk_15FD2C + slot*600)}. Every item is incremented by 1 (loc_234C0).
    std::fill(slot_items_.begin(), slot_items_.end(), (uint8_t)0);
    for (int s = 0; s < MAX_BLACKBOX_SLOTS; s++) {
        g_slot_gold[s] = 0;
        g_slot_item_count[s] = 0;
        uint8_t gb[4] = {0, 0, 0, 0};
        uint8_t cb[2] = {0, 0};
        if (fread(gb, 1, 4, fp) != 4) break;
        if (fread(cb, 1, 2, fp) != 2) break;
        g_slot_gold[s] = (uint32_t)gb[0] | ((uint32_t)gb[1] << 8) | ((uint32_t)gb[2] << 16) | ((uint32_t)gb[3] << 24);
        const uint32_t cnt = (uint32_t)cb[0] | ((uint32_t)cb[1] << 8);
        // The original trusts the count and can overflow into the next slot. The port keeps
        // at most kSlotBytes items and seeks past the rest to stay aligned with the file.
        const uint32_t keep = cnt > (uint32_t)kSlotBytes ? (uint32_t)kSlotBytes : cnt;
        if (keep) {
            size_t got = fread(&slot_item(s, 0), 1, keep, fp);
            for (size_t i = 0; i < got; i++) slot_item(s, (int)i) = (uint8_t)(slot_item(s, (int)i) + 1);
            if (got != keep) { g_slot_item_count[s] = (uint16_t)got; break; }
        }
        if (cnt > keep) fseek(fp, (long)(cnt - keep), SEEK_CUR);
        g_slot_item_count[s] = (uint16_t)keep;
    }
    fclose(fp);
    // End of sub_23369: dword_19FF44 = dword_15FB7C[word_1A00CC].
    team_gold_ = g_slot_gold[cur_slot()];
    remove(loaded_blackbox_path_.c_str());   // unlink("blackbox.dat")

    init_player_sprite_base();
    return true;
}

// ---- sub_17EB7: compute the party leader's field sprite base ----
// The leader is the character whose character_id equals byte_7D955 (g_event_flags[0xC9]),
// not the first named slot. The frame base comes from the high word of the dword at
// byte_19AC83, i.e. town_sprite_id at struct offset +0x61, shifted left 4 (x16).
// sub_2117F reads the same field as word_19AC85[eax*8], which confirms +0x61.
// town_sprite_id is unrelated to portrait_id; using portrait_id selects a wrong or
// out-of-range citizen.spr frame.
// Guarantee at compile time that town_sprite_id(+0x61, 2 bytes) fits inside padding[].
static_assert(0x61 - 0x31 + 1 < sizeof(CharacterData::padding),
              "town_sprite_id(+0x61, 2bytes) must fit inside CharacterData::padding[]");

void Pack::init_player_sprite_base() {
    player_base_frame_ = 0;

    // byte_7D955 = g_event_flags[0xC9]: character_id of the current party leader
    uint8_t leader_id = g_event_flags[0xC9];

    for (int i = 0; i < MAX_CHARACTERS; i++) {
        const CharacterData& ch = g_characters[i];
        if (ch.character_id != leader_id) continue;

        // town_sprite_id: struct offset +0x61 (padding[48..49]), little-endian uint16_t
        uint16_t town_sprite_id = 0;
        memcpy(&town_sprite_id, &ch.padding[0x61 - 0x31], sizeof(town_sprite_id));
        player_base_frame_ = (uint16_t)(town_sprite_id * 16);
        break;
    }
}

// ---- sub_2117F: camera follow and clamp ----
// The camera sits 6 tiles left of and 7 tiles above the player: loc_21C09 computes
// cam_tile_y_ from (portal_y - 1) - 5, but portal_y - 1 is the value before the +2
// correction and the final player_tile_y_ is portal_y + 1, so relative to the player it
// is -7. 0x21E29 handles cam_fine overflow (fine >= 24 -> tile++, < 0 -> tile--).
// Deviation (C4): the original integrates dword_15BA1A velocity in sub_21E29, so the
// camera eases toward the target. This stateless formula lands on the same steady-state
// position; only the frame-by-frame easing differs. The occupancy half of C4 is fixed
// (0xFF at step start); the camera easing is left as-is because a velocity rewrite
// risks the steady state and needs runtime verification.
void Pack::update_camera() {
    int player_world_x = player_tile_x_ * TOWN_TILE_W + player_fine_x_;
    int player_world_y = player_tile_y_ * TOWN_TILE_H + player_fine_y_;

    // Y is offset by 7 tiles, not 5: sub_2117F computes cam_tile_y_ = player_tile_y_ - 7.
    // The X offset of 6 tiles is unchanged.
    int cam_world_x = player_world_x - 6 * TOWN_TILE_W;
    int cam_world_y = player_world_y - 7 * TOWN_TILE_H;

    // Clamp bounds from loc_212A4..loc_2139A. Each loop decrements the camera tile until
    // its exit condition holds, so the escape constant is one lower than the resulting
    // upper bound:
    //   loc_212A4  cam_tile_y_ >= 1 always.
    //   loc_212CC  cam_tile_x_ >= 0.
    //   loc_212F1/21309  loop exits when cam_tile_y_ + 13 >= map_h_, so the final value is
    //                    map_h_ - 14, not map_h_ - 13.
    //   loc_21348/21364  likewise cam_tile_x_ <= map_w_ - 13. Allowing map_w_ - 12 exposed
    //                    a non-existent tile column and drew a black strip on the right.
    int min_cam_world_y = TOWN_TILE_H; // cam_tile_y_ >= 1
    int max_cam_x = (map_w_ > 13) ? (int)(map_w_ - 13) * TOWN_TILE_W : 0;
    int max_cam_y = (map_h_ > 14) ? (int)(map_h_ - 14) * TOWN_TILE_H : 0;

    if (cam_world_x < 0) cam_world_x = 0;
    if (cam_world_y < min_cam_world_y) cam_world_y = min_cam_world_y;
    if (cam_world_x > max_cam_x) cam_world_x = max_cam_x;
    if (cam_world_y > max_cam_y) cam_world_y = max_cam_y;

    cam_tile_x_ = cam_world_x / TOWN_TILE_W;
    cam_fine_x_ = cam_world_x % TOWN_TILE_W;
    cam_tile_y_ = cam_world_y / TOWN_TILE_H;
    cam_fine_y_ = cam_world_y % TOWN_TILE_H;
}



// ---- sub_23147: save BLACKBOX.DAT ----
void Pack::save_blackbox() {
    std::string path = game_path("blackbox.dat");
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return;

    // The load path skips the leading byte (unk_52698), so the save path must write it to
    // keep the stream aligned. Nothing reads the value back; it only has to exist.
    uint8_t header_byte = 0;
    fwrite(&header_byte, 1, 1, fp);
    g_slot_gold[cur_slot()] = team_gold_;   // sub_23147: dword_15FB7C[slot] = dword_19FF44

    // The original writes back the loaded unk_1A00C4, unk_1A00DB and word_1A00CC unchanged:
    // PACK preserves these header fields instead of regenerating them.
    uint16_t quest_counter = blackbox_day_;
    uint8_t sec_state = blackbox_sec_;
    fwrite(&quest_counter, 1, 2, fp);
    fwrite(&sec_state, 1, 1, fp);
    fwrite(g_event_flags, 1, 0x4E20, fp);

    uint16_t ch_id = blackbox_slot_;
    fwrite(&ch_id, 2, 1, fp);

    for (int i = 0; i < MAX_CHARACTERS; i++) {
        fwrite(&g_characters[i], sizeof(CharacterData), 1, fp);
    }

    // sub_23147: decrement every stored item, write {4-byte gold, 2-byte count, count items}
    // for all 100 slots, then restore the in-memory values. The port subtracts at write time
    // instead of mutating the array.
    for (int s = 0; s < MAX_BLACKBOX_SLOTS; s++) {
        const uint32_t g = g_slot_gold[s];
        const uint8_t gb[4] = {(uint8_t)g, (uint8_t)(g >> 8), (uint8_t)(g >> 16), (uint8_t)(g >> 24)};
        const uint16_t cnt = g_slot_item_count[s] > kSlotBytes ? (uint16_t)kSlotBytes : g_slot_item_count[s];
        const uint8_t cb[2] = {(uint8_t)cnt, (uint8_t)(cnt >> 8)};
        fwrite(gb, 1, 4, fp);
        fwrite(cb, 1, 2, fp);
        for (int it = 0; it < cnt; it++) {
            const uint8_t b = (uint8_t)(slot_item(s, it) - 1);
            fwrite(&b, 1, 1, fp);
        }
    }
    fclose(fp);
}

// ---- slot inventory helpers ----
int Pack::cur_slot() const {
    const int s = (int16_t)blackbox_slot_;
    return (s >= 0 && s < MAX_BLACKBOX_SLOTS) ? s : 0;
}

uint8_t& Pack::slot_item(int slot, int idx) {
    size_t p = (size_t)slot * kSlotBytes + (size_t)idx;
    if (p >= slot_items_.size()) p = slot_items_.size() - 1;   // the original writes out of bounds; the port clamps
    return slot_items_[p];
}

// sub_376D8: insert an item at the front of the current slot (word_1A00CC), storing
// argument low byte + 1. At 0x258 items it shows unk_521CB (sub_34FBE) and returns -1;
// otherwise it returns 1.
int Pack::inv_add_front(int mem_item) {
    const int s = cur_slot();
    const uint16_t cnt = g_slot_item_count[s];
    if (cnt >= kSlotBytes) {
        show_message_box(kPackMsgInvFull);
        return -1;
    }
    for (int b = (int)cnt - 1; b >= 0; b--) slot_item(s, b + 1) = slot_item(s, b);
    slot_item(s, 0) = (uint8_t)(mem_item + 1);
    g_slot_item_count[s] = (uint16_t)(cnt + 1);
    return 1;
}

// sub_3767B: remove item idx by shifting the tail left (while b < count, [b] = [b+1];
// clear the last cell; decrement the count when it is > 0).
void Pack::inv_remove(int idx) {
    const int s = cur_slot();
    const int cnt = g_slot_item_count[s];
    int b = idx;
    while (b < cnt) { slot_item(s, b) = slot_item(s, b + 1); b++; }
    slot_item(s, b) = 0;
    if (cnt > 0) g_slot_item_count[s] = (uint16_t)(cnt - 1);
}

// ---- sub_1A094: CITIZEN1/2.SPR loader ----
bool Pack::load_citizen_sprites(const std::string& filepath) {
    citizen_sprites_.clear();
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) return false;

    // Total frame count at 0x42.
    fseek(fp, 0x42, SEEK_SET);
    uint16_t count = 0;
    if (fread(&count, 2, 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    // Move to 0x80.
    fseek(fp, 0x80, SEEK_SET);
    citizen_sprites_.resize(count);

    for (int i = 0; i < count; i++) {
        int rem = i % 4;
        if (rem == 2) {
            // Reuse frame [i - 2] and skip this entry (0x1A228).
            uint32_t data_size = 0;
            fread(&data_size, 4, 1, fp);
            fseek(fp, data_size + 8, SEEK_CUR); // 8 bytes header + data_size pixels

            if (i >= 2) {
                citizen_sprites_[i] = citizen_sprites_[i - 2];
            }
        } else {
            uint32_t data_size = 0;
            fread(&data_size, 4, 1, fp);
            uint16_t w = 0, h = 0;
            int16_t ox = 0, oy = 0;
            fread(&w, 2, 1, fp);
            fread(&h, 2, 1, fp);
            fread(&ox, 2, 1, fp);
            fread(&oy, 2, 1, fp);

            citizen_sprites_[i].width = w;
            citizen_sprites_[i].height = h;
            citizen_sprites_[i].x_offset = ox;
            citizen_sprites_[i].y_offset = oy;
            citizen_sprites_[i].foot_offset_y = (int16_t)(h + oy);
            citizen_sprites_[i].data_size = data_size;
            citizen_sprites_[i].data.resize(data_size);
            fread(citizen_sprites_[i].data.data(), 1, data_size, fp);
        }
    }

    fclose(fp);
    return true;
}

// ---- sub_19F11: ITEM.SPR and ITEMBOX.SPR loader ----
bool Pack::load_standard_sprites(const std::string& filepath, std::vector<PackSprite>& out_sprites) {
    out_sprites.clear();
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) return false;

    fseek(fp, 0x42, SEEK_SET);
    uint16_t count = 0;
    if (fread(&count, 2, 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    fseek(fp, 0x80, SEEK_SET);
    out_sprites.resize(count);

    for (int i = 0; i < count; i++) {
        uint32_t data_size = 0;
        fread(&data_size, 4, 1, fp);
        uint16_t w = 0, h = 0;
        int16_t ox = 0, oy = 0;
        fread(&w, 2, 1, fp);
        fread(&h, 2, 1, fp);
        fread(&ox, 2, 1, fp);
        fread(&oy, 2, 1, fp);

        out_sprites[i].width = w;
        out_sprites[i].height = h;
        out_sprites[i].x_offset = ox;
        out_sprites[i].y_offset = oy;
        out_sprites[i].foot_offset_y = (int16_t)(h + oy);
        out_sprites[i].data_size = data_size;
        out_sprites[i].data.resize(data_size);
        fread(out_sprites[i].data.data(), 1, data_size, fp);
    }

    fclose(fp);
    return true;
}

// ---- sub_213A6: map graphic and tileset loader (.MAP / .TIL) ----
bool Pack::load_map_file(const std::string& map_filename) {
    std::string path = cstldata_path(map_filename.c_str());
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    // 1. Base tile count (0x215E4)
    uint16_t base_tile_count = 0;
    fread(&base_tile_count, 2, 1, fp);

    // 2. lseek 4 bytes
    fseek(fp, 4, SEEK_CUR);

    // sub_213A6 loc_2160C: more than 1000 base tiles prints "OVER MAX\tBOTTOM TILES!" and exits.
    if (base_tile_count > 1000) {
        fprintf(stderr, "PACK: error: over max bottom tiles\n");
        fclose(fp);
        should_exit_ = true;
        return_code_ = (int)0xFFFFFFFF;
        return false;
    }
    // 3. Read base tiles: base_tile_count * 384 bytes (24x16 raw 8-bit pixels)
    base_tiles_data_.resize(base_tile_count);
    for (int i = 0; i < base_tile_count; i++) {
        base_tiles_data_[i].resize(TOWN_TILE_BYTES);
        fread(base_tiles_data_[i].data(), 1, TOWN_TILE_BYTES, fp);
    }

    // 4. Top tile count (0x21657: word_8B128)
    uint16_t top_tile_count = 0;
    fread(&top_tile_count, 2, 1, fp);

    // 5. Read top tile overlay sprites (0x2167E: dword_8B12A)
    top_tile_sprites_.resize(top_tile_count);
    for (int i = 0; i < top_tile_count; i++) {
        uint32_t data_size = 0;
        uint16_t width = 0, height = 0;
        fread(&data_size, 4, 1, fp);
        fread(&width, 2, 1, fp);
        fread(&height, 2, 1, fp);

        top_tile_sprites_[i].width = width;
        top_tile_sprites_[i].height = height;
        top_tile_sprites_[i].x_offset = 0;

        // ASM 0x21849: Calculate y_offset according to tile height
        int16_t y_off = 0;
        if (height <= 0x10) {
            y_off = 0;
        } else if (height == 0x20) {
            y_off = -16;
        } else if (height == 0x30) {
            y_off = -32;
        } else if (height == 0x40) {
            y_off = -48;
        } else if (height == 0x50) {
            y_off = -64;
        } else if (height == 0x60) {
            y_off = -80;
        } else {
            y_off = -(int16_t)(height - 16);
        }

        top_tile_sprites_[i].y_offset = y_off;
        // ASM 0x218B8: foot_offset_y = height + y_offset - 10
        top_tile_sprites_[i].foot_offset_y = (int16_t)(height + y_off - 10);
        top_tile_sprites_[i].data_size = data_size;
        top_tile_sprites_[i].data.resize(data_size);
        fread(top_tile_sprites_[i].data.data(), 1, data_size, fp);
    }

    // 6. Read map dimensions (0x21919: map_w, map_h)
    uint16_t mw = 0, mh = 0;
    fread(&mw, 2, 1, fp);
    fread(&mh, 2, 1, fp);
    map_w_ = mw;
    map_h_ = mh;
    tile_count_ = map_w_ * map_h_;

    if (tile_count_ == 0 || tile_count_ > 0x10000) {
        fclose(fp);
        return false;
    }

    // 7. Allocate tile layers
    layer_base_tiles_ = new uint16_t[tile_count_]();
    layer_top_tiles_  = new uint16_t[map_w_ * (map_h_ + 6)]();
    std::fill_n(layer_top_tiles_, map_w_ * (map_h_ + 6), 0xFFFF);

    fread(layer_base_tiles_, 2, tile_count_, fp);
    fread(layer_top_tiles_, 2, tile_count_, fp);

    // loc_21A51 always decrements base tile indices with no zero test ("dec word ptr
    // [eax+ecx*2]"), so a file value of 0 underflows to 0xFFFF and means "no tile". The
    // render pass then rejects it through its `b_idx < base_tiles_data_.size()` check.
    for (uint32_t i = 0; i < tile_count_; i++) {
        uint16_t top = layer_top_tiles_[i] & 0x7FFF;
        if (top > 0) {
            layer_top_tiles_[i] = top - 1;
        } else {
            layer_top_tiles_[i] = 0xFFFF;
        }

        layer_base_tiles_[i]--; // unconditional decrement as in the original (0 -> 0xFFFF)
    }

    // 8. Skip layer 3 in map file (lseek tile_count bytes)
    fseek(fp, tile_count_, SEEK_CUR);

    // 9. Read Palette (768 bytes, 6-bit DAC values) from MAP file
    uint8_t pal_raw[768];
    if (fread(pal_raw, 1, 768, fp) == 768) {
        for (int i = 0; i < 256; i++) {
            town_palette_[i][0] = pal_raw[i * 3 + 0];
            town_palette_[i][1] = pal_raw[i * 3 + 1];
            town_palette_[i][2] = pal_raw[i * 3 + 2];
        }
    }

    // sub_213A6 loc_21AB1..loc_21B46: two runs of 20-byte records follow the palette.
    // Group A (one per base tile, unk_F78F8) counts records whose first word is > 0 and is
    // unused elsewhere in the code. Group B (one per top tile, unk_1583D8) keeps the records
    // with R[1] > 0 for sub_21D70.
    tile_anim_records_.clear();
    tile_anim_cur_.clear();
    tile_anim_counter_ = 0;
    uint8_t rec_buf[20];
    for (int i = 0; i < base_tile_count; i++)
        if (fread(rec_buf, 1, 20, fp) != 20) break;
    for (int i = 0; i < top_tile_count; i++) {
        if (fread(rec_buf, 1, 20, fp) != 20) break;
        std::array<int16_t, 10> r;
        for (int k = 0; k < 10; k++) r[k] = (int16_t)(rec_buf[k * 2] | (rec_buf[k * 2 + 1] << 8));
        if (r[1] > 0) tile_anim_records_.push_back(r);
    }
    tile_anim_cur_.assign(tile_anim_records_.size(), 0);

    fclose(fp);
    return true;
}

// sub_21D70: advance every record by one step only while word_58CEF is 0. The counter
// cycles 0..16, a 17-frame period.
void Pack::update_tile_animation() {
    if (tile_anim_counter_ == 0) {
        for (size_t si = 0; si < tile_anim_records_.size(); si++) {
            const auto& r = tile_anim_records_[si];
            uint16_t& cur = tile_anim_cur_[si];
            if (cur + 1 < 10) {
                const int a = r[cur], b = r[cur + 1];
                if (a >= 0 && b >= 0 && a < (int)top_tile_sprites_.size() && b < (int)top_tile_sprites_.size())
                    top_tile_sprites_[a].data.swap(top_tile_sprites_[b].data);   // swap the [+0Ah] data pointers
            }
            cur++;
            if (cur + 1 >= 10 || r[cur + 1] == -1) cur = 0;
        }
    }
    const uint16_t ax = tile_anim_counter_;
    tile_anim_counter_ = (uint16_t)(ax + 1);
    if (ax == 0x10) tile_anim_counter_ = 0;
}

// ---- sub_213A6 and sub_24017: load .TWN and .TEC ----
bool Pack::load_town_and_tec(int town_id) {
    free_town_data();
    town_id_ = town_id;

    char filename[64];
    snprintf(filename, sizeof(filename), "%d.twn", town_id);
    std::string twn_path = cstldata_path(filename);
    if (!file_exists(twn_path.c_str())) {
        snprintf(filename, sizeof(filename), "%d.TWN", town_id);
        twn_path = cstldata_path(filename);
    }

    FILE* fp = fopen(twn_path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "PACK: error: cannot open %s\n", twn_path.c_str());   // sub_213A6: open failure
        return false;
    }

    // 1. Read 13-byte Map filename (e.g. DAKAMA.MAP)
    char map_name[14] = {0};
    fread(map_name, 1, 13, fp);
    map_name[13] = '\0';
    fclose(fp);

    std::string map_str = map_name;
    map_str.erase(std::find(map_str.begin(), map_str.end(), '\0'), map_str.end());

    // 2. Load MAP file (Base tiles, Top tiles, Palette)
    if (!load_map_file(map_str)) {
        return false;
    }

    // 3. sub_24017: Read layers from .TWN
    fp = fopen(twn_path.c_str(), "rb");
    if (!fp) return false;

    fseek(fp, 13, SEEK_SET);

    uint16_t w = 0, h = 0;
    fread(&w, 2, 1, fp); fread(&h, 2, 1, fp);
    // sub_24017: every layer re-reads its own width/height and overwrites the map size, so a
    // .TWN that disagrees with its .MAP wins.
    if (w != map_w_ || h != map_h_) {
        fprintf(stderr, "PACK: TWN size %ux%u overrides MAP size %ux%u\n", w, h, (unsigned)map_w_, (unsigned)map_h_);
        map_w_ = w;
        map_h_ = h;
        tile_count_ = (uint32_t)w * h;
    }
    layer_passable_ = new uint8_t[tile_count_]();
    fread(layer_passable_, 1, tile_count_, fp);
    // sub_24017 loc_24167..loc_24178 normalizes the passable layer: keep exactly 1 and turn
    // every other value (0, 0xFF, other filler) into 0. Skipping this would change
    // walkability wherever the file holds a value other than 0 or 1.
    for (uint32_t i = 0; i < tile_count_; i++) {
        if (layer_passable_[i] != 1) layer_passable_[i] = 0;
    }

    fread(&w, 2, 1, fp); fread(&h, 2, 1, fp);
    layer_event_ = new uint8_t[tile_count_]();
    fread(layer_event_, 1, tile_count_, fp);

    fread(&w, 2, 1, fp); fread(&h, 2, 1, fp);
    layer_item_ = new uint16_t[tile_count_]();
    fread(layer_item_, 2, tile_count_, fp);

    fread(&w, 2, 1, fp); fread(&h, 2, 1, fp);
    layer_building_ = new uint16_t[tile_count_]();
    fread(layer_building_, 2, tile_count_, fp);

    // Read 170 NPCs
    memset(npcs_, 0, sizeof(npcs_));
    fread(npcs_, sizeof(PackNPC), MAX_TOWN_NPCS, fp);
    // sub_24017 loc_242FF: raise move_speed (+0x26, byte_19E16A) to 1 when a record has less.
    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        if (npcs_[i].move_speed < 1) npcs_[i].move_speed = 1;
    }

    // Read 40 Events
    memset(events_, 0, sizeof(events_));
    fread(events_, sizeof(PackEvent), MAX_TOWN_EVENTS, fp);

    fclose(fp);

    // 4. sub_244DF: Load .TEC if present
    char tec_name[64];
    snprintf(tec_name, sizeof(tec_name), "%d.tec", town_id);
    std::string tec_path = cstldata_path(tec_name);
    if (!file_exists(tec_path.c_str())) {
        snprintf(tec_name, sizeof(tec_name), "%d.TEC", town_id);
        tec_path = cstldata_path(tec_name);
    }
    FILE* tec_fp = fopen(tec_path.c_str(), "rb");
    if (tec_fp) {
        fread(events_, sizeof(PackEvent), MAX_TOWN_EVENTS, tec_fp);
        fclose(tec_fp);
    }

    layer_npc_       = new uint8_t[tile_count_]();
    path_cost_grid_  = new uint16_t[tile_count_]();

    // 5. Player start position (layer_event == 0xFF, ASM 0x24223).
    // sub_24017 loc_241EB records the portal cell as (word_15B5B8, word_15B7B4): esi counts
    // completed rows, so a match stores portal_y - 1. sub_213A6 loc_21CBF then sets
    // player_tile_y_ = word_15B7B4 + 2 = portal_y + 1, i.e. the player stands on the row
    // below the portal.
    // If word_56C8D[town_id] / word_56E89[town_id] are not both 0, the town was left before
    // and those saved coordinates are restored: Y = saved_y when re-entering upward
    // (player_dir_ == 0x48), otherwise Y = saved_y + 1 (loc_21C9E). A fresh entry spawns at
    // (portal_x, portal_y + 1).
    bool has_saved_pos = (town_id >= 0 && town_id < MAX_TOWNS) &&
                          (saved_tile_x_[town_id] != 0 || saved_tile_y_[town_id] != 0);
    // The initial camera Y needs its own reference. loc_21C09 uses saved_y - 5 on a return
    // (word_56E89 holds the unadjusted Y), but word_15B7B4 (= portal_y - 1) on a fresh
    // entry, giving portal_y - 6. Because the return path adds a different +0/+1 correction
    // to player_tile_y_ depending on direction, a single player_tile_y_ - 7 formula cannot
    // cover both, so cam_ref_y carries the right baseline.
    int16_t cam_ref_y;

    if (has_saved_pos) {
        player_tile_x_ = saved_tile_x_[town_id];
        if (player_dir_ == 0x48) { // UP
            player_tile_y_ = saved_tile_y_[town_id];
        } else {
            player_tile_y_ = (int16_t)(saved_tile_y_[town_id] + 1);
        }
        cam_ref_y = (int16_t)(saved_tile_y_[town_id] + 2); // minus 7 below gives saved_y - 5
    } else {
        // sub_24017 loc_241EB: with no 0xFF cell the original keeps the BSS defaults
        // word_15B5B8 = 0 / word_15B7B4 = 0, so the spawn is (0, 1) rather than the map
        // centre. Normal towns always have a portal, so this only covers corrupt data.
        player_tile_x_ = 0;
        player_tile_y_ = 1; // 0 (portal_y) + 1
        cam_ref_y = 1;

        for (uint32_t y = 0; y < map_h_; y++) {
            for (uint32_t x = 0; x < map_w_; x++) {
                if (layer_event_[y * map_w_ + x] == 0xFF) {
                    player_tile_x_ = (int16_t)x;
                    player_tile_y_ = (int16_t)(y + 1); // portal_y + 1
                    cam_ref_y = player_tile_y_; // this path satisfies the -7 rule (portal_y - 6)
                    break;
                }
            }
        }
    }

    // Initial camera (loc_21C09): cam_tile_x_ = portal_x - 6 and
    // cam_tile_y_ = (portal_y - 1) - 5 = portal_y - 6, i.e. player_tile_y_ - 7.
    // Initialize tile + fine as sub_2117F and sub_21E29 do.
    player_fine_x_ = 0;
    player_fine_y_ = 0;
    player_move_step_ = 0;
    player_anim_counter_ = 0;
    player_anim_frame_ = 0;
    // loc_1DA1C: the movement mode depends only on byte_82730 from game.cfg
    // (config_move_speed_, loaded once by load_game_config), never on the town. Recompute
    // from it here so moving or warping between towns does not reset the setting.
    move_steps_per_tile_ = (config_move_speed_ == 1) ? MOVE_STEPS_FAST : MOVE_STEPS_SLOW;
    cam_tile_x_ = player_tile_x_ - 6;
    cam_tile_y_ = cam_ref_y - 7; // per-branch baseline, see cam_ref_y above
    cam_fine_x_ = 0;
    cam_fine_y_ = 0;
    // This one-shot clamp at town entry mirrors sub_2117F, so the upper bounds are the same
    // as in update_camera(): map_w_ - 13 and map_h_ - 14.
    if (cam_tile_x_ < 0) cam_tile_x_ = 0;
    if (cam_tile_y_ < 1) cam_tile_y_ = 1;   // same lower bound as update_camera (the original keeps y >= 1)
    if (cam_tile_x_ > (int)map_w_ - 13) cam_tile_x_ = (int)map_w_ - 13;
    if (cam_tile_y_ > (int)map_h_ - 14) cam_tile_y_ = (int)map_h_ - 14;

    // 6. Load GENESIS.PAL and merge with town palette (ASM 0x21B9E: 312 bytes = 104 entries)
    std::string pal_path = game_path("genesis.pal");
    if (!file_exists(pal_path.c_str())) pal_path = game_path("GENESIS.PAL");
    FILE* pal_fp = fopen(pal_path.c_str(), "rb");
    if (pal_fp) {
        uint8_t sys_pal[312];
        if (fread(sys_pal, 1, 312, pal_fp) == 312) {
            for (int i = 0; i < 104; i++) {
                town_palette_[i][0] = sys_pal[i * 3 + 0];
                town_palette_[i][1] = sys_pal[i * 3 + 1];
                town_palette_[i][2] = sys_pal[i * 3 + 2];
            }
        }
        fclose(pal_fp);
    }

    for (int i = 0; i < 256; i++) {
        palette_set_entry(i, town_palette_[i][0], town_palette_[i][1], town_palette_[i][2]);
    }

    init_npcs_and_positions();
    return true;
}

// ---- sub_24646: NPC initialization ----
// NPC name comparison (the original strcmp_): sub_18BE6 and sub_24646 strcmp from
// struct +0x05 (name) up to a NUL. name is 20 bytes and may have no NUL, in which case
// the original reads into the adjacent fields (+0x19..), so compare raw bytes too.
static bool npc_name_equals(const PackNPC& n, const char* s) {
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&n) + 5;
    const int max = (int)sizeof(PackNPC) - 5;
    for (int k = 0; k < max; k++) {
        uint8_t a = raw[k];
        uint8_t b = (uint8_t)s[k];
        if (a != b) return false;
        if (a == 0) return true;
    }
    return false;
}

// ---- sub_24646: initial NPC registration (called at the tail of sub_24017) ----
//  1) Only NPCs with active == 1 and +0x27 (move_type) == 0 occupy layer_npc, using code
//     (i + 2). The cell is +0x21/+0x23 (target_x/target_y), not +0x01/+0x03 (tile), and is
//     written only inside the map. Walking NPCs update occupancy in sub_1EAD0/sub_1EC6E.
//  2) Names starting with "hire" are matched against the party and reserve rosters:
//     - "hireN" (name[4] != 'd'): eligible only when record N has team byte (+0x87) ==
//       0x0A, and hidden when that character is already in the party (list A) or the reserve
//       (list B); hidden immediately when the team is not 0x0A.
//     - "hiredN" (N = atoi of name[5..7]): visible only while the character is in list B
//       and not in list A. The sprite id (+0x1D) is overwritten with that record's +0x61
//       (town_sprite_id) and move_type = 0, active = 1. If the tile above (tile_y - 1) is
//       passable, the NPC is then moved down one row and its occupancy follows.
//     Hidden means active = 0 and layer_npc[tile_y*w + tile_x] = 0.
//  list A = characters whose team (+0x87) equals word_1A00CC (blackbox_slot_, sub_22FED).
//  list B = characters whose team is 0x63 (sub_23053). Both keep record order; id is +0x0F.
//  The original iterates 0..179, but entries past 170 overlap the event table, so the port
//  stops at MAX_TOWN_NPCS.
void Pack::init_npcs_and_positions() {
    if (!layer_npc_) return;
    memset(layer_npc_, 0, tile_count_);

    const int w = (int)map_w_;
    const int h = (int)map_h_;
    auto in_map = [&](int x, int y) { return x >= 0 && x < w && y >= 0 && y < h; };

    std::vector<int> listA, listB; // g_characters indices
    for (int c = 0; c < MAX_CHARACTERS; c++) {
        const uint8_t team = chr_team(g_characters[c]);
        if ((uint16_t)team == blackbox_slot_) listA.push_back(c);
        if (team == 0x63) listB.push_back(c);
    }

    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        PackNPC& n = npcs_[i];
        const uint8_t code = (uint8_t)(i + 2);

        // Hide: active = 0 and clear the (tile_x, tile_y) occupancy (the original writes 0
        // without checking the current value).
        auto hide = [&]() {
            n.active = 0;
            const int tx = (int16_t)n.tile_x, ty = (int16_t)n.tile_y;
            if (in_map(tx, ty)) layer_npc_[ty * w + tx] = 0;
        };

        if (n.active == 1 && n.move_type == 0) {
            const int gx = (int16_t)n.target_x, gy = (int16_t)n.target_y;
            if (in_map(gx, gy)) layer_npc_[gy * w + gx] = code;
        }

        if (strncmp(n.name, "hire", 4) != 0) continue;

        if (n.name[4] != 'd') {
            // "hireN"
            char buf[4] = { n.name[4], n.name[5], n.name[6], 0 };
            const int N = (int16_t)atoi(buf);
            if (N < 0 || N >= MAX_CHARACTERS || chr_team(g_characters[N]) != 0x0A) { hide(); continue; }
            bool found = false;
            for (int c : listA) if ((int)g_characters[c].character_id == N) found = true;
            for (int c : listB) if ((int)g_characters[c].character_id == N) found = true;
            if (found) hide();
            continue;
        }

        // "hiredN"
        char buf[4] = { n.name[5], n.name[6], n.name[7], 0 };
        const int N = atoi(buf);
        int jb = -1;
        for (size_t j = 0; j < listB.size(); j++) {
            if ((int)g_characters[listB[j]].character_id == (N & 0xFFFF)) { jb = (int)j; break; }
        }
        if (jb < 0) { hide(); continue; }
        const CharacterData& cb = g_characters[listB[jb]];
        bool inA = false;
        for (int c : listA) if (g_characters[c].character_id == cb.character_id) { inA = true; break; }
        if (inA) { hide(); continue; }

        uint16_t sid = 0;
        memcpy(&sid, reinterpret_cast<const uint8_t*>(&cb) + 0x61, 2);
        n.sprite_id = sid;
        n.move_type = 0;
        n.active = 1;

        const int tx = (int16_t)n.tile_x, ty = (int16_t)n.tile_y;
        if (layer_passable_ && in_map(tx, ty - 1) && layer_passable_[(ty - 1) * w + tx] == 1) {
            const int gx = (int16_t)n.target_x, gy = (int16_t)n.target_y;
            if (in_map(gx, gy)) layer_npc_[gy * w + gx] = 0;
            n.tile_y = (uint16_t)(n.tile_y + 1);
            if (in_map(gx, gy + 1)) layer_npc_[(gy + 1) * w + gx] = code;
        }
    }

    // sub_1EBC2 also marks the player's own tile in layer_npc_ (1 for the player, i+2 for
    // NPCs). The start tile is no exception, so marking it here keeps NPCs and the path
    // search from walking into the player.
    if (player_tile_x_ >= 0 && player_tile_x_ < (int)map_w_ &&
        player_tile_y_ >= 0 && player_tile_y_ < (int)map_h_) {
        layer_npc_[player_tile_y_ * (int)map_w_ + player_tile_x_] = 1;
    }
}

// ---- sub_18BE6: hide NPCs according to story progress (byte_7D954 = g_event_flags[0xC8]) ----
// Called at the top of every sub_19528 main-loop iteration. Matching NPCs only get
// active (+0x00) cleared; their layer_npc occupancy is left untouched, as in the original.
//   progress 9      : hired2/3/1/20/4/8/10/11
//   progress 10     : hired18/7/22/23 plus the Johab name "AC E5 B8 77"
//   progress 7/12/18: none
//   progress 20     : the Johab name (town 7 only) plus hired22 and hired12
//   otherwise       : the Johab name
// The original loops to 0xB3 (179) entries, but everything past 170 overlaps the event
// table, so only 170 are visited.
void Pack::apply_story_npc_visibility() {
    static const char kMxW[] = "\xAC\xE5\xB8\x77";
    const uint8_t st = g_event_flags[0xC8];
    std::vector<const char*> names;
    switch (st) {
        case 7: case 12: case 18:
            return;
        case 9:
            names = { "hired2", "hired3", "hired1", "hired20", "hired4", "hired8", "hired10", "hired11" };
            break;
        case 10:
            names = { "hired18", "hired7", "hired22", "hired23", kMxW };
            break;
        case 20:
            if (town_id_ == 7) names.push_back(kMxW);
            names.push_back("hired22");
            names.push_back("hired12");
            break;
        default:
            names = { kMxW };
            break;
    }
    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        for (const char* nm : names) {
            if (npc_name_equals(npcs_[i], nm)) npcs_[i].active = 0;
        }
    }
}

// ---- sub_1001A..loc_11897: 4-direction path search ----
bool Pack::find_path(int start_x, int start_y, int target_x, int target_y, std::vector<uint8_t>& out_path) {
    out_path.clear();
    if (start_x == target_x && start_y == target_y) return true;
    if (!layer_passable_ || !path_cost_grid_) return false;

    for (uint32_t i = 0; i < tile_count_; i++) {
        path_cost_grid_[i] = 0x7D00;
    }

    struct Node {
        int x, y, cost;
        bool operator>(const Node& o) const { return cost > o.cost; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    int start_idx = start_y * (int)map_w_ + start_x;
    path_cost_grid_[start_idx] = 0;
    pq.push({start_x, start_y, 0});

    const int dx[] = { 0, 0, -1, 1 };
    const int dy[] = { -1, 1, 0, 0 };
    const uint8_t dirs[] = { 0x48, 0x50, 0x4B, 0x4D };

    bool found = false;

    while (!pq.empty()) {
        Node cur = pq.top();
        pq.pop();

        if (cur.x == target_x && cur.y == target_y) {
            found = true;
            break;
        }

        int cur_idx = cur.y * (int)map_w_ + cur.x;
        if (cur.cost > path_cost_grid_[cur_idx]) continue;

        for (int i = 0; i < 4; i++) {
            int nx = cur.x + dx[i];
            int ny = cur.y + dy[i];
            if (nx < 0 || nx >= (int)map_w_ || ny < 0 || ny >= (int)map_h_) continue;

            int n_idx = ny * (int)map_w_ + nx;
            // Wall test from sub_1AA46, the routine the movement engine (sub_1D12F) calls
            // on every step. All four branches (loc_1AA73 UP, loc_1AB2E DOWN, loc_1AABE LEFT,
            // loc_1AAF9 RIGHT) test the same single cell: the destination column, one row
            // above the destination (target_y - 1), which is the head row of a 2-tile-tall
            // character. Neither the destination row's layer_passable_ nor layer_event_
            // (dword_15B9C0) is ever tested here, so neither blocks movement; events only
            // trigger scene changes through sub_1A785, which reads the row above the feet.
            //   UP    target (cur_y-1, cur_x) -> passable[cur_y-2][cur_x]
            //   DOWN  target (cur_y+1, cur_x) -> passable[cur_y][cur_x]
            //   LEFT  target (cur_y, cur_x-1) -> passable[cur_y-1][cur_x-1]
            //   RIGHT target (cur_y, cur_x+1) -> passable[cur_y-1][cur_x+1]
            // layer_npc_ is tested on the destination cell itself.
            int head_ny = ny - 1;
            // Deviation: the original reads [layer_passable_ + (target_y-1)*width + x] with
            // no bounds check, which lands inside the DOS data segment. The port's array has
            // no padding rows, so a head row above the map is treated as a wall instead of
            // reading out of bounds.
            if (head_ny < 0) continue;
            int head_idx = head_ny * (int)map_w_ + nx;
            if (layer_passable_[head_idx] == 1) continue;
            if (layer_npc_[n_idx] != 0) continue;
            int n_cost = cur.cost + 1;
            if (n_cost < path_cost_grid_[n_idx]) {
                path_cost_grid_[n_idx] = (uint16_t)n_cost;
                pq.push({nx, ny, n_cost});
            }
        }
    }

    if (!found && path_cost_grid_[target_y * (int)map_w_ + target_x] == 0x7D00) {
        return false;
    }

    int cx = target_x, cy = target_y;
    while (cx != start_x || cy != start_y) {
        int best_dir = -1;
        int min_cost = path_cost_grid_[cy * (int)map_w_ + cx];

        for (int i = 0; i < 4; i++) {
            int px = cx - dx[i];
            int py = cy - dy[i];
            if (px < 0 || px >= (int)map_w_ || py < 0 || py >= (int)map_h_) continue;
            int p_cost = path_cost_grid_[py * (int)map_w_ + px];
            if (p_cost < min_cost) {
                min_cost = p_cost;
                best_dir = i;
            }
        }

        if (best_dir == -1) break;
        out_path.push_back(dirs[best_dir]);
        cx -= dx[best_dir];
        cy -= dy[best_dir];
    }

    std::reverse(out_path.begin(), out_path.end());
    return !out_path.empty();
}

// ---- is_move_blocked: can (x, y) step one cell toward dir? (sub_1AA46) ----
// Extracts the neighbour rule used by find_path so decide_step_direction and
// try_corner_slide can share it: layer_passable_ is tested one row above the destination,
// rows above the map are walls, layer_npc_ is tested on the destination cell, and
// layer_event_ never affects movement.
bool Pack::is_move_blocked(int x, int y, uint8_t dir) const {
    int nx = x, ny = y;
    if (dir == 0x48) ny -= 1;      // Up
    else if (dir == 0x50) ny += 1; // Down
    else if (dir == 0x4B) nx -= 1; // Left
    else if (dir == 0x4D) nx += 1; // Right
    else return true;

    if (nx < 0 || nx >= (int)map_w_ || ny < 0 || ny >= (int)map_h_) return true;
    int head_ny = ny - 1;
    if (head_ny < 0) return true; // above the map: wall (same as find_path)
    if (layer_passable_ && layer_passable_[head_ny * (int)map_w_ + nx] == 1) return true;
    if (layer_npc_ && layer_npc_[ny * (int)map_w_ + nx] != 0) return true;
    return false;
}

// ---- sub_2E1A4: mouse position relative to the player -> 4-way direction ----
// Per frame: x_idx = clamp(mouse_x - player_screen_x + 0xA0, 0x13, 0x13F) and
// y_idx = clamp(mouse_y - player_screen_y + 0x5A, 1, 0xC7), then look up the 320x200 static
// table aHhhhhhhhhhhhhh[y_idx*320 + x_idx] and use its byte directly as the direction
// (0x48 up, 0x4B left, 0x4D right, 0x50 down; anything else means no change). The anchors
// 0xA0 (160) and 0x5A (90) are constants, so the camera always places the player at that
// fixed screen point and player_screen_x/y is effectively always (160, 90).
uint8_t Pack::compute_mouse_move_direction(int mouse_x, int mouse_y) const {
    if (mouse_dir_table_.size() == 320 * 200) {
        // sub_2E1A4 anchors on the player's real screen position (dword_82943+2, word_82947):
        //   xi = clamp(mouse_x - (player_screen_x - 0xA0), 0x13, 0x13F)
        //   yi = clamp(mouse_y - (player_screen_y - 0x5A), 1, 0xC7)
        const int psx = (player_tile_x_ - cam_tile_x_) * TOWN_TILE_W + player_fine_x_ - cam_fine_x_ + 5;
        const int psy = (player_tile_y_ - cam_tile_y_) * TOWN_TILE_H + player_fine_y_ - cam_fine_y_ - TOWN_TILE_H;
        int xi = mouse_x - (psx - 0xA0);
        if (xi < 0x13) xi = 0x13; else if (xi > 0x13F) xi = 0x13F;
        int yi = mouse_y - (psy - 0x5A);
        if (yi < 0) yi = 1; else if (yi > 0xC7) yi = 0xC7;
        const uint8_t v = mouse_dir_table_[(size_t)yi * 320 + (size_t)xi];
        return (v == 0x48 || v == 0x4B || v == 0x4D || v == 0x50) ? v : 0;
    }
    // Fallback used only when the table is unavailable: quadrant test around (160, 90).
    constexpr int ANCHOR_X = 0xA0; // 160
    constexpr int ANCHOR_Y = 0x5A; // 90

    int dx = mouse_x - ANCHOR_X;
    int dy = mouse_y - ANCHOR_Y;

    // Convert the original clamp ranges (0x13..0x13F and 1..0xC7) to anchor-relative values.
    if (dx < (0x13 - ANCHOR_X)) dx = 0x13 - ANCHOR_X;
    else if (dx > (0x13F - ANCHOR_X)) dx = 0x13F - ANCHOR_X;
    if (dy < (1 - ANCHOR_Y)) dy = 1 - ANCHOR_Y;
    else if (dy > (0xC7 - ANCHOR_Y)) dy = 0xC7 - ANCHOR_Y;

    if (dx == 0 && dy == 0) return 0; // exactly on the anchor: no change

    if (std::abs(dx) >= std::abs(dy)) return (dx < 0) ? 0x4B : 0x4D; // Left/Right
    return (dy < 0) ? 0x48 : 0x50; // Up/Down
}

// ---- is_mouse_over_npc: NPC hover test (sub_1F6E4) ----
// 0x190F7..0x19123: sub_1F6E4 returns an NPC index or -1, and sub_1D12F receives that as
// its edx parameter: edx = 0 (skip the movement test) while an NPC is hovered, else
// edx = 1. get_npc_screen_box reuses the screen formula of render_sprites_and_objects, so
// this check matches the NPC hover test in render_tile_highlight.
bool Pack::is_mouse_over_npc(int mouse_x, int mouse_y, int& out_npc_idx) const {
    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        int nl, nt, nw, nh;
        if (!get_npc_screen_box(i, nl, nt, nw, nh)) continue;
        if (mouse_x >= nl && mouse_x < nl + nw && mouse_y >= nt && mouse_y < nt + nh) {
            out_npc_idx = i;
            return true;
        }
    }
    out_npc_idx = -1;
    return false;
}

// ---- try_corner_slide: corner sliding (sub_1D12F loc_1DA96..loc_1DFC3) ----
// When the chosen direction is blocked, scan along the perpendicular axis (left/right
// blocked -> up/down, up/down blocked -> left/right) inside the camera viewport for the
// nearest cell where the original direction opens again, and step that way instead.
//  - Vertical scans (left/right blocked) stay within 0..13 tiles of cam_tile_y_ (0xD,
//    from dword_56BF6+2 and the sar 10h on dword_56BFA); horizontal scans (up/down blocked)
//    within 0..14 tiles of cam_tile_x_ (0xE, sar 10h on dword_56BFA).
//  - Each side counts the distance to an opening (loc_1DAEA/1DB5B, loc_1DC47/1DCB3,
//    loc_1DD8E/1DDFF, loc_1DEDB/1DF4C), and the cmp/jge/jl branches at loc_1DBB8..loc_1DFC3
//    pick the shorter one (ties included, per direction).
//  - If neither side opens inside the viewport (both 3E7h = 999) the player does not move.
// The eight scan loops each call sub_1AA46 twice:
//   1st: is_move_blocked(fixed axis, scan candidate, blocked_dir). An opening succeeds with
//        count = cells travelled so far; leaving the viewport ends the scan without success.
//   2nd: after advancing the candidate one more cell, is_move_blocked(aux axis offset, ...).
//        If that is blocked too, the whole direction is abandoned (SENTINEL) rather than
//        trying the next cell; otherwise the scan continues.
//   Aux offset: left (0x4B) = +1, right (0x4D) = -1, which makes the second test re-check the
//   same destination one cell further along the scan. Up (0x48) and down (0x50) always use
//   Y+1 regardless of scan or blocked direction.
bool Pack::try_corner_slide(uint8_t blocked_dir, uint8_t& out_dir) const {
    constexpr int SENTINEL = 0x3E7; // 999
    constexpr int VIEW_Y = 0x0D;    // 13
    constexpr int VIEW_X = 0x0E;    // 14

    if (blocked_dir == 0x4B || blocked_dir == 0x4D) {
        // Left/right blocked -> scan up/down. loc_1DAEA/loc_1DB5B (left) and
        // loc_1DC47/loc_1DCB3 (right): a blocked first test retests one cell further along
        // the scan (X left = +1, right = -1); a blocked second test abandons that side
        // (SENTINEL).
        const int xoff = (blocked_dir == 0x4B) ? 1 : -1;
        int up_count = SENTINEL, down_count = SENTINEL;
        for (int k = 1; player_tile_y_ - k >= cam_tile_y_; k++) {
            if (!is_move_blocked(player_tile_x_, player_tile_y_ - k, blocked_dir)) { up_count = k; break; }
            if (is_move_blocked(player_tile_x_ + xoff, player_tile_y_ - k - 1, blocked_dir)) { break; }
        }
        for (int k = 1; player_tile_y_ + k < cam_tile_y_ + VIEW_Y; k++) {
            if (!is_move_blocked(player_tile_x_, player_tile_y_ + k, blocked_dir)) { down_count = k; break; }
            if (is_move_blocked(player_tile_x_ + xoff, player_tile_y_ + k + 1, blocked_dir)) { break; }
        }
        if (up_count == SENTINEL && down_count == SENTINEL) return false;

        if (blocked_dir == 0x4B) {
            // Left blocked: loc_1DBC8 "cmp si,cx; jl loc_1DBD7(Down)", else loc_1DBCD(Up)
            out_dir = (down_count < up_count) ? 0x50 : 0x48;
        } else {
            // Right blocked: loc_1DD20 "cmp si,cx; jge loc_1DBCD(Up)", else loc_1DBD7(Down)
            out_dir = (down_count >= up_count) ? 0x48 : 0x50;
        }
        return true;
    } else if (blocked_dir == 0x48 || blocked_dir == 0x50) {
        // Up/down blocked -> scan left/right. loc_1DD8E/loc_1DDFF (up) and
        // loc_1DEDB/loc_1DF4C (down): same two-stage test, with Y always +1 regardless of the
        // up/down direction.
        int left_count = SENTINEL, right_count = SENTINEL;
        for (int k = 1; player_tile_x_ - k >= cam_tile_x_; k++) {
            if (!is_move_blocked(player_tile_x_ - k, player_tile_y_, blocked_dir)) { left_count = k; break; }
            if (is_move_blocked(player_tile_x_ - k - 1, player_tile_y_ + 1, blocked_dir)) { break; }
        }
        for (int k = 1; player_tile_x_ + k < cam_tile_x_ + VIEW_X; k++) {
            if (!is_move_blocked(player_tile_x_ + k, player_tile_y_, blocked_dir)) { right_count = k; break; }
            if (is_move_blocked(player_tile_x_ + k + 1, player_tile_y_ + 1, blocked_dir)) { break; }
        }
        if (left_count == SENTINEL && right_count == SENTINEL) return false;

        if (blocked_dir == 0x48) {
            // Up blocked: loc_1DE6C "cmp si,cx; jge loc_1DAC0(Left)", else loc_1DC1D(Right)
            out_dir = (right_count >= left_count) ? 0x4B : 0x4D;
        } else {
            // Down blocked: loc_1DFB5 "cmp cx,si; jge loc_1DC1D(Right)", else loc_1DAC0(Left)
            out_dir = (left_count >= right_count) ? 0x4D : 0x4B;
        }
        return true;
    }
    return false;
}

// ---- decide_step_direction: choose a direction at the start of a tile step (loc_1DA1C) ----
// update_player_movement calls this only when player_move_step_ == 0, i.e. on a new tile
// boundary ("test bh,bh (byte_544CE); jbe loc_1DA1C"). The arrow keys are tested even
// with no mouse button held; the mouse arm needs the left button (sub_2E1A4 returns -1
// otherwise). An NPC under the cursor skips the whole test (sub_1F6E4, edx = 0).
void Pack::decide_step_direction() {
    // sub_1D12F loc_1DA1C..loc_1DFC3: at step start (byte_544CE == 0) the direction is
    // chosen by the keyboard-or-mouse OR chain in the order Left, Right, Up, Down:
    //   left:  (dword_1A0220 != 0) || (sub_2E1A4 == 0x4B)
    //   right: (dword_1A0228 != 0) || (sub_2E1A4 == 0x4D)
    //   up:    (dword_1A0214 != 0) || (sub_2E1A4 == 0x48)
    //   down:  (dword_1A0234 != 0) || (sub_2E1A4 == 0x50)
    // The scancode indices are (addr - 0x1A00F4)/4: 0x4B=75, 0x4D=77, 0x48=72, 0x50=80.
    // The NPC-hover case (edx = 0) skips the whole test; a missing left button only
    // disables the mouse arm (sub_2E1A4 returns -1 unless dword_7CF20 == 1).
    // The 0x1D1A5 gate (dword_7CF20 == 1) only guards the tile click interaction; on a
    // miss it jumps to loc_1D23C, which still falls into loc_1DA1C when byte_544CE == 0.
    // There the four arrows are OR'ed with the mouse direction, so arrows move the player
    // with no button held. sub_2E1A4 itself returns -1 unless dword_7CF20 == 1, hence the
    // mouse arm is gated below.
    const bool left_held = input_.is_left_button() && !input_.is_right_button();

    int hover_npc = -1;
    if (is_mouse_over_npc(input_.get_mouse_x(), input_.get_mouse_y(), hover_npc)) {
        // 0x190F7..0x19123: over an NPC, sub_1D12F is called with edx = 0 and skips the
        // movement test. The dialogue trigger that consumes sub_1F6E4's result lives
        // elsewhere; this reproduces only the rule that pressing on an NPC does not walk.
        is_player_moving_ = false;
        return;
    }

    const uint8_t mouse_dir = left_held
        ? compute_mouse_move_direction(input_.get_mouse_x(), input_.get_mouse_y())
        : (uint8_t)0;
    const bool kLeft  = input_.is_key_held(SDLK_LEFT);
    const bool kRight = input_.is_key_held(SDLK_RIGHT);
    const bool kUp    = input_.is_key_held(SDLK_UP);
    const bool kDown  = input_.is_key_held(SDLK_DOWN);

    // Each branch: if the direction is passable, step it; else if the key is held, commit
    // the direction only (loc_1DAC0/loc_1DC1D/...: ebp = dir with no slide, then the
    // loc_1E086.. blocked path turns the player in place); else try the corner slide.
    auto try_dir = [&](uint8_t dir, bool key_held) -> bool {
        if (!is_move_blocked(player_tile_x_, player_tile_y_, dir)) {
            player_dir_ = dir;
            is_player_moving_ = true;
            return true;
        }
        if (key_held) {
            player_dir_ = dir;
            is_player_moving_ = false;
            return true;
        }
        uint8_t slide_dir = 0;
        if (try_corner_slide(dir, slide_dir)) {
            player_dir_ = slide_dir;
            is_player_moving_ = true;
        } else {
            is_player_moving_ = false;
        }
        return true;
    };

    if (kLeft || mouse_dir == 0x4B) { try_dir(0x4B, kLeft); return; }
    if (kRight || mouse_dir == 0x4D) { try_dir(0x4D, kRight); return; }
    if (kUp || mouse_dir == 0x48) { try_dir(0x48, kUp); return; }
    if (kDown || mouse_dir == 0x50) { try_dir(0x50, kDown); return; }

    is_player_moving_ = false;
}

// ---- sub_2B5C0: fast 24x16 base tile copy ----
void Pack::blit_tile_24x16(int x, int y, const uint8_t* src_pixels) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !src_pixels) return;

    for (int row = 0; row < TOWN_TILE_H; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < TOWN_TILE_W; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            dest[dy * SCREEN_W + dx] = src_pixels[row * TOWN_TILE_W + col];
        }
    }
}

// ---- blit_top_tile_transparent: draw one top tile (sub_21E29/sub_2278F/sub_2294D) ----
// Top tiles (the objects loaded into dword_8B12A) use the same Softmax RLE format as
// CITIZEN and ITEM sprites: a 14-byte width/height/x_off/y_off/foot_off header followed by
// RLE pixels. sub_21E29 registers them per frame as group 2 in unk_82935[slot], sub_2278F
// Y-sorts, and sub_2294D draws them through sub_2B240 (the RLE blitter) on the same path as
// groups 4 (citizen), 6 (item) and 3 (itembox). The group index table unk_840A6 places each
// group's sprite pointer array at group * 0x3842, and unk_840A6 + 2*0x3842 is exactly
// dword_8B12A, so the top-tile array is group 2's slot and sub_2294D always calls sub_2B240
// regardless of group. sub_2B2A1 (raw pixels with 0xFF transparent) is only used for fixed
// UI icons such as the cursor and never in the top-tile path.
void Pack::blit_top_tile_transparent(int x, int y, const PackSprite& sp) {
    if (sp.data.empty()) return;
    blit_sprite_rle(x, y, sp.data.data());
}

// ---- sub_2B240: Softmax RLE sprite renderer ----
// The original's EAX < 0 branch (EBX = 0x140 + EAX, EDX -= 1) is algebraically the same as
// computing dest_offset = y*SCREEN_W + x directly (x=-5, y=3 gives 955 either way), so the
// port uses the direct formula. Because the backbuffer is one-dimensional, callers must
// clip off-screen sprites: render_sprites_and_objects applies the original viewport clip,
// otherwise a far out-of-range position would alias onto an unrelated row.
void Pack::blit_sprite_rle(int x, int y, const uint8_t* rle_data) {
    if (!rle_data) return;
    uint8_t* dest_buf = video_.get_backbuffer();
    if (!dest_buf) return;

    // This signature has no length, so instead of comparing an end pointer (which would wrap
    // when passed SIZE_MAX) the stream is guarded by two caps: chunk count and total pixels.
    // Neither fires on valid data, so the output matches the original.
    uint16_t chunk_count = *(const uint16_t*)rle_data;
    // Abort on an insane chunk_count: valid sprites stay below a few hundred. An out-of-range
    // portrait id (107/136/213) can read a file header byte (chunk 29296) here; sub_2B240 has
    // no cap and would keep reading corrupt data.
    if (chunk_count > 4096) {
        fprintf(stderr, "PACK: blit_rle insane chunk_count=%u at (%d,%d), skip\n",
                (unsigned)chunk_count, x, y);
        return;
    }
    const uint8_t* ptr = rle_data + 2;

    int rel_offset = 0;
    int total_drawn = 0; // cap that cannot be reached by a full-screen sprite

    for (uint16_t i = 0; i < chunk_count; i++) {
        uint16_t skip = *(const uint16_t*)ptr;
        ptr += 2;
        uint8_t dword_count = *ptr++;
        uint8_t byte_count  = *ptr++;

        rel_offset += skip;
        int total_bytes = (dword_count * 4) + byte_count;

        for (int b = 0; b < total_bytes; b++) {
            int cur_pixel_rel = rel_offset + b;
            int cur_py = y + (cur_pixel_rel / SCREEN_W);
            int cur_px = x + (cur_pixel_rel % SCREEN_W);

            if (cur_px >= 0 && cur_px < SCREEN_W && cur_py >= 0 && cur_py < SCREEN_H) {
                dest_buf[cur_py * SCREEN_W + cur_px] = *ptr;
                // Well above the largest valid sprite (about 23K pixels for the dialogue
                // window); truncates runaway pixels in a corrupt stream.
                if (++total_drawn > 100000) return;
            }
            ptr++;
        }
        rel_offset += total_bytes;
    }
}

// ---- development screenshot dump (G2_PACK_SHOT) ----
// Writes the backbuffer to /tmp/packshot_NNN.ppm (P6, 320x200) after present(), one file per
// dialogue page, for eyeballing overlaps and portraits. No effect unless the env var is set.
static void pack_maybe_dump_shot(Video& video) {
    static const bool enabled = (dbg_env("G2_PACK_SHOT"));
    if (!enabled) return;
    static int counter = 0;
    uint8_t* backbuf = video.get_backbuffer();
    if (!backbuf) return;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/packshot_%03d.ppm", counter++);
    FILE* fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t r, g, b;
        palette_get_entry(backbuf[i], r, g, b);
        uint8_t rgb[3] = {(uint8_t)(r * 4), (uint8_t)(g * 4), (uint8_t)(b * 4)};
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    fprintf(stderr, "PACK: shot %s\n", path);
}

// ---- sub_2B2A1: raw bitmap blitter, used for the mouse cursor ----
// Follows the off_53D0C entry layout: read width*height pixels in order and write each one
// unless it is 0xFF (transparent). Simpler than the RLE blitter sub_2B240 used by top tiles.
void Pack::blit_cursor_raw(int x, int y, const PackCursorFrame& frame) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || frame.pixels.empty()) return;

    for (int row = 0; row < frame.height; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < frame.width; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            uint8_t v = frame.pixels[row * frame.width + col];
            if (v == 0xFF) continue; // transparent
            dest[dy * SCREEN_W + dx] = v;
        }
    }
}

// ---- sub_1D12F loc_1D1A5: click on the town ----
// While the left button is down (state 1) and byte_15BA22 is clear, the tile under the
// mouse is examined: a non-zero building layer (dword_15B9B8) calls sub_1B344(value), then
// a non-zero item layer (dword_15B9BC) calls sub_1B082(x, y). There is no distance limit.
// Tile = ((mx+9)/24 + cam_x, my/16 + cam_y, minus 1 when fine_y > 0).
void Pack::town_click_interaction() {
    if (!input_.is_left_button() || input_.is_right_button()) return;   // dword_7CF20 == 1
    if (click_latch_) return;
    int tx, ty;
    get_mouse_tile(input_.get_mouse_x(), input_.get_mouse_y(), tx, ty);
    if (tx < 0 || tx >= (int)map_w_ || ty < 0 || ty >= (int)map_h_) return;
    const int idx = ty * (int)map_w_ + tx;
    if (layer_building_ && layer_building_[idx] != 0) handle_building_buy(layer_building_[idx]);
    if (layer_item_ && layer_item_[idx] != 0) handle_chest(tx, ty);
}

// ---- sub_1A7B5 (item) and sub_1A8F3 (building): action key on the facing tile ----
// When the action key (dword_1A0168, scancode 0x1D = Left Ctrl level) is held and the
// latch is clear, test the tile the player faces (sub_190A3 loc_19144/loc_19157).
// Base tile = (player screen x / 24 + cam_x, player screen y >> 4 + cam_y),
// where screen y already includes -16. Directions 0x48 up, 0x50 down, 0x4B left and 0x4D
// right offset one tile; any other direction uses the base tile itself.
// Enter/Space edges are kept as extra aliases (not in the original) for manual testing;
// the original is Ctrl level only.
void Pack::town_key_interaction() {
    if (click_latch_) return;
    const bool action = input_.is_key_held(SDLK_LCTRL) ||
                        input_.is_key_pressed(SDLK_RETURN) ||
                        input_.is_key_pressed(SDLK_SPACE);
    if (!action) return;
    const int px = (player_tile_x_ - cam_tile_x_) * TOWN_TILE_W + player_fine_x_ - cam_fine_x_ + 5;
    const int py = (player_tile_y_ - cam_tile_y_) * TOWN_TILE_H + player_fine_y_ - cam_fine_y_ - TOWN_TILE_H;
    int tx = px / TOWN_TILE_W + cam_tile_x_;
    int ty = (py >> 4) + cam_tile_y_;
    if (player_dir_ == 0x48) ty -= 1;
    else if (player_dir_ == 0x50) ty += 1;
    else if (player_dir_ == 0x4B) tx -= 1;
    else if (player_dir_ == 0x4D) tx += 1;
    if (tx < 0 || tx >= (int)map_w_ || ty < 0 || ty >= (int)map_h_) return;
    const int idx = ty * (int)map_w_ + tx;
    if (layer_item_ && layer_item_[idx] != 0) handle_chest(tx, ty);                  // sub_1A7B5
    if (!click_latch_ && layer_building_ && layer_building_[idx] != 0)                // sub_1A8F3
        handle_building_buy(layer_building_[idx]);
}

// ---- sub_3197D: wait until the action input is released ----
// The original loops while dword_7CF20 == 1 (left-only) or dword_1A0168 != 0 (Left Ctrl
// level), so both-pressed (3) counts as released. Ctrl is the left-click substitute.
void Pack::wait_buttons_released() {
    while (true) {
        input_.update();
        if (input_.quit_requested()) return;
        const bool left_only = input_.is_left_button() && !input_.is_right_button();
        if (!left_only && !input_.is_key_held(SDLK_LCTRL)) return;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// ---- sub_18899/sub_187C3: dim rows y0..y1-1 over x 0..0x13E ----
// pixel p -> 7 - (R+G+B)/0x25 using the 6-bit palette unk_15B2B8, the same formula as
// render_dialog_dimming.
void Pack::render_dim_band(int y0, int y1) {
    uint8_t* backbuf = video_.get_backbuffer();
    if (!backbuf) return;
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= SCREEN_H) continue;
        for (int x = 0; x < 0x13F && x < SCREEN_W; x++) {
            uint8_t r, g, b;
            palette_get_entry(backbuf[y * SCREEN_W + x], r, g, b);
            backbuf[y * SCREEN_W + x] = (uint8_t)(7 - ((int)r + (int)g + (int)b) / 0x25);
        }
    }
}

// ---- sub_1B082 loc_1B1BE..loc_1B2C3: item acquired popup ----
// Dim band y 0x55..0x72 (sub_18899(0, 0x73, 0x13F, 0x55)), then the item icon (item.spr
// frame id) at (0x96 - len*6 + x_off + 0x0C, 0x7D - (h + 0x10)) and the "<name> acquired" text at
// (0xB4 - len*6, 0x5F), then wait for a new click or key (len = name length in bytes).
// The icon uses the sub_18B2A-corrected frame: w/h include +0x10 on height, x_off +0x0C,
// y_off = -new_h, foot = 0 (loc_18B41). The popup therefore draws at
// (0x96 - len*6 + x_off + 0x0C, 0x7D - (h + 0x10)).
void Pack::show_item_acquired(int item0, const char* name, bool redraw_scene) {
    if (redraw_scene) {
        video_.clear();
        render_town_frame();
    }
    render_dim_band(0x55, 0x73);

    uint8_t* backbuf = video_.get_backbuffer();
    const int len = (int)strlen(name);
    if (item0 >= 0 && item0 < (int)item_sprites_.size()) {
        const auto& sp = item_sprites_[item0];
        if (!sp.data.empty()) {
            const int icon_x = 0x96 - len * 6 + sp.x_offset + 0x0C;
            const int icon_y = 0x7D - (sp.height + 0x10);
            blit_sprite_rle(icon_x, icon_y, sp.data.data());
        }
    }
    std::string msg = std::string(name) + kPackMsgItemGot;
    font_.set_text_mode(4, 0x1000, 0xFF);
    font_.draw_string(backbuf, SCREEN_W, SCREEN_H, 0xB4 - len * 6, 0x5F, msg.c_str());
    video_.present();

    // loc_1B281: the release-then-press latch (byte_15BA22) with the action input
    // dword_1A0168 (Left Ctrl level) or dword_7CF20 == 1 (left-only). The original does
    // not accept ESC here; Enter/Space edges are kept as extra aliases (not in the
    // original) for manual testing.
    wait_buttons_released();
    while (true) {
        input_.update();
        if (input_.quit_requested()) return;
        const bool left_only = input_.is_left_button() && !input_.is_right_button();
        if (left_only || input_.is_key_held(SDLK_LCTRL) ||
            input_.is_key_pressed(SDLK_RETURN) ||
            input_.is_key_pressed(SDLK_SPACE)) break;
        GameTimer::delay_with_pump(input_, 16);
    }
}

// ---- handle_chest: open a chest or pick up an item (sub_1B082, x = eax, y = edx) ----
// Visible when (v/1000)/2 <= g_event_flags[0xC8] (progress), g_event_flags[600 + v/1000] == 0
// and v != 0. Item id = v%1000 (values above 500 subtract 500 and mark a chest), then -1 for
// the zero-based index. The name starts at byte 1 of the ITEM.DAT record.
// The opened flag is set before adding to the inventory, and only a successful add of a
// chest rewrites the top tile at row y+1 to the open chest. layer_item_ is not cleared; the
// flag prevents reopening.
void Pack::handle_chest(int x, int y) {
    if (!layer_item_ || x < 0 || y < 0 || x >= (int)map_w_ || y >= (int)map_h_) return;
    const int idx = y * (int)map_w_ + x;
    const uint16_t v = layer_item_[idx];

    const int q = v / 1000;
    if (q / 2 > (int)g_event_flags[0xC8]) return;
    if (g_event_flags[600 + q] != 0) return;
    if (v == 0) return;

    int rem = v % 1000;
    bool is_chest = false;
    if (rem > 500) { is_chest = true; rem -= 500; }
    const int item0 = rem - 1;

    g_event_flags[600 + q] = 1;   // set before adding to the inventory

    load_item_data();
    char name[40] = {0};
    if (item0 >= 0 && item0 < MAX_ITEMS) {
        // Record is 66 bytes; the name runs from +1 to the NUL (it ends before +0x25).
        for (int i = 0; i < 0x25 && item_data_[item0][1 + i] != 0; i++) name[i] = (char)item_data_[item0][1 + i];
    }

    show_item_acquired(item0, name);

    if (inv_add_front(item0) != -1 && is_chest) {
        const uint32_t below = (uint32_t)(y + 1) * map_w_ + (uint32_t)x;
        if (layer_top_tiles_ && chest_open_tile_idx_ >= 0 && below < map_w_ * (map_h_ + 6))
            layer_top_tiles_[below] = (uint16_t)chest_open_tile_idx_;   // word_8B128 - 1
    }
    wait_buttons_released();   // sub_3197D
}

// ==== NPC interaction (sub_25F21) ====
// Original call tree: sub_19528 (town main loop) -> sub_190A3 (mouse handling, returns the
// clicked NPC index) -> sub_25F21 (eax = npc_idx, dispatches on the name string).
// Dispatch order, followed instruction by instruction across 0x28850..0x29998:
//   1) name[0..4] == "hired" (exactly 5 bytes) -> dismiss the hired companion; the number
//      after it (name[5..8], atoi) is the target id.
//   2) name[0..3] == "hire" and name[4] != 'd' -> hire a mercenary; name[4..7] is the
//      shop/guild id.
//   3) the whole name == "inn keeper" (cp949 BF A9 B0 FC) -> inn.
//   4) otherwise -> normal dialogue, looking up npc.talk_id in TALK.TBL.
uint16_t Pack::interact_with_npc(int npc_idx) {
    if (npc_idx < 0 || npc_idx >= MAX_TOWN_NPCS) return 0xFFFF;
    PackNPC& npc = npcs_[npc_idx];
    if (npc.active != 1) return 0xFFFF;

    // 1) "hired" + number: dismiss the companion. The original strcmp covers only the first
    // five bytes (it zero-fills var_87 around 0x25EA1 to build a 5-byte string), so whatever
    // follows the name does not affect this test.
    if (strncmp(npc.name, "hired", 5) == 0) {
        handle_npc_dismiss(npc_idx);
        return 0xFFFF;
    }

    // Branch order in sub_25F21: hired -> shop keeper (0x260E9) -> inn keeper (0x261C4) ->
    // hire (0x2628B) -> normal dialogue.
    // 2) Shop keeper: stock list and sales (sub_337A2 + sub_1B75B).
    if (strcmp(npc.name, kPackNameShopOwner) == 0) {
        handle_shop_owner(npc_idx);
        return 0xFFFF;
    }

    // 3) Inn keeper (four Johab characters); the whole name field must match. The argument
    //    is the high word of the dword at +0x17 = talk_id
    //    (sub_1ABA6: eax = dword_19E15B[idx*40] >> 16).
    if (strcmp(npc.name, kPackNameInnOwner) == 0) {
        handle_inn(npc.talk_id);
        wait_buttons_released();
        return 0xFFFF;
    }

    // 4) "hire" + number, excluding "hired" (name[4] != 'd')
    if (strncmp(npc.name, "hire", 4) == 0 && npc.name[4] != 'd') {
        handle_npc_hire(npc_idx);
        return 0xFFFF;
    }

    // 5) Otherwise: normal TALK.TBL dialogue.
    return (uint16_t)run_dialogue_script(npc_idx);
}

// ==== Dialogue window (normal NPC dialogue from TALK.TBL) ====
// Ports sub_25DD3 (TALK.TBL parsing, now talktbl.cpp), sub_24BA4 (portrait loading,
// portrait.cpp), sub_24DA5 (placement), sub_186DC (dimming), sub_2B240 (blit, reusing
// blit_sprite_rle) and sub_2C3A4 / sub_24FD5 (text and choice rendering).

// Placement formula obtained from loc_263CD and loc_264AF in sub_25F21 plus sub_24DA5.
// See the DialogPlacement note in pack.h for the underlying addresses.
bool Pack::compute_dialog_placement_for_box(int nx, int ny, int nw, int nh,
                                          int tile_y, int anchor_x,
                                          DialogPlacement& out, int extra_raw_y) const {
    // sub_25F21: unk_8293A = (tile_y - cam_tile_y)*16 - cam_fine_y - 16. Below 100 takes the
    // upper branch (loc_263CD), otherwise the lower one (loc_264AF). This is the sub_24DA5
    // core shared by NPCs and EVE actors, both routed through unk_82938/8293A.
    int raw_ny = (tile_y - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ - TOWN_TILE_H + extra_raw_y;
    int center_x = nx + nw / 2;
    // Int center_y = ny + nh / 2; // sub_24DA5 ecx, unused here

    out.npc_upper_half = raw_ny < 0x64;    // sub_25F21: unk_8293A[...] < 100
    out.npc_right_half = center_x >= 0xA0;  // sub_24DA5: esi >= 160 (tail side only)
    // sub_25F21/sub_2708B/sub_24FD5/sub_26E32 test unk_82938 (the anchor X) instead,
    // with a strict >: "cmp word ptr unk_82938[i], 0A0h / jle -> 0x138 - width".
    out.portrait_left = anchor_x > 0xA0;

    int variant = rand() % 2; // sub_2B81D(0, 2) -> 0 or 1

    if (out.npc_upper_half) {
        // loc_263CD: NPC in the upper half, frame drawn below it.
        out.frame_y    = 0x69;             // 105
        out.dimming_y  = out.frame_y + 9;   // 114
        out.portrait_y = 0x73;               // 115 (blit Y = 0x73 - portrait_h)

        if (!out.npc_right_half) {
            // Left + upper: variant+0, tail at (center_x + w/2, top)
            out.speech_tail_index = variant + 0;
            out.tail_x = center_x + nw / 2;  // = nx + nw (right edge)
            out.tail_y = ny;                 // = center_y - h/2 (top edge)
        } else {
            // Right + upper: variant+2, tail at (center_x - w, top)
            out.speech_tail_index = variant + 2;
            out.tail_x = center_x - nw;      // = nx - nw/2 (left edge)
            out.tail_y = ny;
        }
    } else {
        // loc_264AF: NPC in the lower half, frame drawn above it.
        out.frame_y    = 0x50 - 0x4F;        // 1
        out.dimming_y  = 0x50 - 0x46;          // 10
        out.portrait_y = 0xC7;                  // 199 (blit Y = 0xC7 - portrait_h)

        if (!out.npc_right_half) {
            // Left + lower: variant+4, tail at (center_x + w/2, bottom-5)
            out.speech_tail_index = variant + 4;
            out.tail_x = center_x + nw / 2;  // right edge
            out.tail_y = ny + nh - 5;        // bottom edge - 5
        } else {
            // Right + lower: variant+6, tail at (center_x - w, bottom-5)
            out.speech_tail_index = variant + 6;
            out.tail_x = center_x - nw;      // left edge
            out.tail_y = ny + nh - 5;
        }
    }

    out.portrait_x = out.portrait_left ? 8 : 0x138; // 312 - width
    out.text_x = 0x37; // shared by sub_25F21: fixed at 55
    out.text_base_y = out.frame_y + 0x3F; // 63
    return true;
}

bool Pack::compute_dialog_placement(int npc_idx, DialogPlacement& out) const {
    int nx, ny, nw, nh;
    if (!get_npc_screen_box(npc_idx, nx, ny, nw, nh)) return false;
    // nx is unk_82938 for this NPC (the anchor before the sprite x_offset).
    return compute_dialog_placement_for_box(nx, ny, nw, nh, npcs_[npc_idx].tile_y, nx, out);
}

// sub_26E32 (eax = 1): speech bubble anchored on the player. The screen box matches block 2
// (the player) in render_sprites_and_objects.
bool Pack::compute_dialog_placement_for_player(DialogPlacement& out) const {
    const int px = (player_tile_x_ - cam_tile_x_) * TOWN_TILE_W + player_fine_x_ - cam_fine_x_ + 5;
    const int py = (player_tile_y_ - cam_tile_y_) * TOWN_TILE_H + player_fine_y_ - cam_fine_y_ - TOWN_TILE_H;
    int dir_offset = 0;
    if (player_dir_ == 0x48) dir_offset = 12;
    else if (player_dir_ == 0x50) dir_offset = 0;
    else if (player_dir_ == 0x4B) dir_offset = 4;
    else if (player_dir_ == 0x4D) dir_offset = 8;
    if (citizen_sprites_.empty()) return false;
    const int frame = (player_base_frame_ + dir_offset + (player_anim_frame_ % 4)) % (int)citizen_sprites_.size();
    const PackSprite& sp = citizen_sprites_[frame];
    if (sp.data.empty()) return false;
    // unk_82938[2] is px itself (the anchor, before the sprite x_offset).
    return compute_dialog_placement_for_box(px + sp.x_offset, py + sp.y_offset, sp.width, sp.height,
                                            player_tile_y_, px, out, player_fine_y_);
}

bool Pack::compute_dialog_placement_for_eve(int actor, DialogPlacement& out) const {
    int nx, ny, nw, nh;
    if (!get_eve_actor_screen_box(actor, nx, ny, nw, nh)) return false;
    const EveActorState& st = eve_actors_[actor];
    // loc_12BE2: unk_82938 = (tile - cam)*24 + fine, with no x_offset and no +5, unlike
    // the sprite box nx returned above.
    const int anchor_x = (st.tile_x - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ + st.fine_x;
    // EVE computes unk_8293A as (tile - cam)*16 + fine with no -16 (loc_12BE2), and its
    // tile_y is already decremented (p2 - 1). The shared helper subtracts the NPC's -16, so
    // adding 1 makes (p2-cam)*16 - 16 equal the original EVE value.
    return compute_dialog_placement_for_box(nx, ny, nw, nh, st.tile_y + 1, anchor_x, out);
}

// sub_186DC: dim the unk_59ABD mask (250x76) at (x, y). A mask byte of 0xFF is skipped;
// otherwise the pixel becomes 7 - (LUT[p].R + G + B) / 37 (range 0..7). The LUT
// (unk_15B2B8) is the current 256-colour palette, so g_palette is used directly.
void Pack::render_dialog_dimming(int x, int y) {
    uint8_t* backbuf = video_.get_backbuffer();
    if (!backbuf) return;
    const bool use_mask = ((long)dimming_mask_.size() == (long)DIMMING_W * DIMMING_H);

    for (int row = 0; row < DIMMING_H; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < DIMMING_W; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            if (use_mask && dimming_mask_[(size_t)row * DIMMING_W + col] == 0xFF)
                continue; // sub_186DC loc_18799: skip mask bytes of 0xFF

            uint8_t idx = backbuf[dy * SCREEN_W + dx];
            uint8_t r, g, b;
            palette_get_entry(idx, r, g, b);
            // sub_186DC loc_18755..loc_18799: eax = (R+G+B)/37 (idiv, truncated) and the
            // pixel becomes 7 - eax. With 6-bit channels the sum is at most 189, so the
            // quotient is at most 5 and the result never goes negative.
            int v = ((int)r + (int)g + (int)b) / 37;
            int out = 7 - v;
            if (out < 0) out = 0;
            if (out > 255) out = 255;
            backbuf[dy * SCREEN_W + dx] = (uint8_t)out;
        }
    }
}

// sub_24BA4 (portrait loading, now portrait_load in portrait.cpp) + sub_2B240.
// PORTRAIT.SYF pixel data is an RLE stream like every other sprite (the off_58CF4
// dialogue frame included), not raw 8bpp: loc_2656A in sub_25F21 draws it with
// "call sub_2B240", not sub_2B2A1. blit_sprite_rle therefore takes the RLE stream that
// portrait_load returns.
void Pack::render_dialog_portrait(const DialogPlacement& dp, int portrait_id) {
    // sub_24BA4 stores/uses only AL ("mov [esp+8+var_4], al" then "mov al, [var_4]"),
    // so the high byte of the portrait word never reaches the PORTRAIT.SYF table index.
    portrait_id &= 0xFF;
    uint16_t w = 0, h = 0;
    std::vector<uint8_t> rle_data;
    std::string path = game_path("portrait.syf");
    if (!file_exists(path.c_str())) path = game_path("PORTRAIT.SYF");
    if (!portrait_load(path, portrait_id, w, h, rle_data)) {
        if (dbg_env("G2_PACK_SHOT"))
            fprintf(stderr, "PACK: portrait id=%d load FAIL path=%s\n", portrait_id, path.c_str());
        return;
    }
    if (rle_data.empty()) return;

    int x = dp.portrait_left ? 8 : (0x138 - (int)w);
    int y = dp.portrait_y - (int)h;
    if (dbg_env("G2_PACK_SHOT"))
        fprintf(stderr, "PACK: portrait id=%d blit wh=(%u,%u) at=(%d,%d) bytes=%zu right=%d py=%d\n",
                portrait_id, (unsigned)w, (unsigned)h, x, y, rle_data.size(),
                (int)dp.portrait_left, dp.portrait_y);

    blit_sprite_rle(x, y, rle_data.data());
}

// Draws the dialogue frame (off_58CF4) and the speech tail (off_5E4F9, fixed
// SPEECH_TAIL_STRIDE entries). Both are sub_2B240 RLE streams, so blit_sprite_rle is reused.
void Pack::render_dialog_frame_and_tail(const DialogPlacement& dp) {
    // sub_2708B draws the tail (sub_24DA5) first, then sub_186DC dimming, then the frame
    // (sub_2B240 off_58CF4), so the tail sits underneath the frame at the shared edge.
    if (!speech_tail_data_.empty() &&
        dp.speech_tail_index >= 0 && dp.speech_tail_index < SPEECH_TAIL_COUNT) {
        const uint8_t* tail = speech_tail_data_.data() + (size_t)SPEECH_TAIL_STRIDE * dp.speech_tail_index;
        // sub_24DA5 blits the tail straight at the NPC edge coordinate (sub_2B240).
        // tail_x/tail_y come from compute_dialog_placement:
        blit_sprite_rle(dp.tail_x, dp.tail_y, tail);
    }
    if (!dialog_frame_data_.empty()) {
        blit_sprite_rle(dp.frame_x, dp.frame_y, dialog_frame_data_.data());
    }
}

// sub_2C1C9 + sub_2C6AB: draw one line in the requested colour.
void Pack::render_dialog_text_line(int x, int y, const std::string& line, uint16_t color_param) {
    if (line.empty()) return;
    uint8_t* backbuf = video_.get_backbuffer();
    font_.set_text_mode(4, color_param, 0xFF);
    font_.draw_string(backbuf, SCREEN_W, SCREEN_H, x, y, line.c_str());
}

// Normal NPC dialogue from TALK.TBL (the default branch at loc_26335 in sub_25F21).
int Pack::run_dialogue_script(int npc_idx) {
    const PackNPC& npc = npcs_[npc_idx];
    std::string path = game_path("talk.tbl");
    if (!file_exists(path.c_str())) path = game_path("TALK.TBL");
    std::vector<uint8_t> buf;
    int len = 0;
    if (!talktbl_load_buffer(path, npc.talk_id, buf, len)) {
        fprintf(stderr, "PACK: warning: talk.tbl load failed for talk_id=%d (npc=%d)\n", npc.talk_id, npc_idx);
        return 0;
    }
    const int sel = run_talk_engine(npc_idx, -1, npc.portrait_id(), npc.name, buf, len);
    return sel > 0 ? sel : 0;     // var_24: chosen option, 1-based, 0 when none
}

// sub_34FBE: standard message box, drawn over the current screen without rebuilding it.
// With len = strlen(text): frame (0xB0 - 5*len, 0x55)..(0xB8 + 3*len, 0x78), interior fill
// 0x0B (sub_189FA(0xB), restored to 0x0D), text at (0xB4 - 5*len, 0x5F). Waits for a left
// press (state 1), then for release. Keyboard input is ignored.
void Pack::show_message_box(const char* text) {
    if (!text) return;
    wait_buttons_released();                                   // sub_3197D
    const int len = (int)strlen(text);
    draw_frame_box(0xB0 - 5 * len, 0x55, 0xB8 + 3 * len, 0x78, 0x0B);
    sh_text((int16_t)(0xB4 - 5 * len), 0x5F, text, 0x1000);
    video_.present();
    while (true) {
        int st, mx, my;
        if (!poll_mouse(st, mx, my)) return;
        if (st == 1) break;
        GameTimer::delay_with_pump(input_, 16);
    }
    wait_buttons_released();
}

int Pack::show_choice_dialog(const char* msg, const char* opt1, const char* opt2) {
    int selected = 1;
    while (true) {
        video_.clear();
        render_town_frame();

        uint8_t* backbuf = video_.get_backbuffer();
        int bx = 30, by = 60, bw = 260, bh = 70;

        for (int y = by; y < by + bh; y++) {
            for (int x = bx; x < bx + bw; x++) {
                backbuf[y * SCREEN_W + x] = 0x01;
            }
        }
        for (int x = bx; x < bx + bw; x++) {
            backbuf[by * SCREEN_W + x] = 0x0F;
            backbuf[(by + bh - 1) * SCREEN_W + x] = 0x0F;
        }
        for (int y = by; y < by + bh; y++) {
            backbuf[y * SCREEN_W + bx] = 0x0F;
            backbuf[y * SCREEN_W + (bx + bw - 1)] = 0x0F;
        }

        font_.set_text_mode(4, 0x1000, 0xFF);
        font_.draw_string(backbuf, SCREEN_W, SCREEN_H, bx + 12, by + 12, msg);

        font_.set_text_mode(4, selected == 1 ? 0x0100 : 0x1000, 0xFF);
        font_.draw_string(backbuf, SCREEN_W, SCREEN_H, bx + 40, by + 40, opt1);

        font_.set_text_mode(4, selected == 2 ? 0x0100 : 0x1000, 0xFF);
        font_.draw_string(backbuf, SCREEN_W, SCREEN_H, bx + 140, by + 40, opt2);

        video_.present();

        input_.update();
        if (input_.quit_requested()) {
            do_transition("EXIT");
            return 0;
        }
        if (input_.is_key_pressed(SDLK_LEFT)) selected = 1;
        if (input_.is_key_pressed(SDLK_RIGHT)) selected = 2;

        if (input_.was_left_clicked() || input_.is_key_pressed(SDLK_RETURN) || input_.is_key_pressed(SDLK_SPACE)) {
            return selected;
        }
        if (input_.is_key_pressed(SDLK_ESCAPE)) {
            return 0;
        }
        GameTimer::delay_with_pump(input_, 16);
    }
}

// sub_235EC: recreate game.cfg (creat 0x180) and write 10 bytes from byte_82730. On failure
// it prints "game.cfg\twrite error!".
void Pack::save_game_config() {
    std::string path = game_path("game.cfg");
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { fprintf(stderr, "PACK: error: cannot write game.cfg\n"); return; }
    fwrite(game_cfg_, 1, sizeof(game_cfg_), fp);
    fclose(fp);
}

// sub_18FD0 (command string): sub_18F19 (clear the 0..9 duplicate-event flags for towns
// 7/9/12) -> save blackbox (sub_23147) -> save game.cfg (sub_235EC) -> stop audio -> write the
// command to "$" -> blacken the palette (sub_20019, except in TOOL mode) -> exit(1).
void Pack::do_transition(const std::string& cmd) {
    for (int town : {7, 9, 12})
        for (int idx = 0; idx < 10; idx++) fired_flag(town, idx) = 0;
    save_blackbox();
    save_game_config();

    std::string dollar = game_path("$");
    FILE* fp = fopen(dollar.c_str(), "wt");
    if (fp) {
        fprintf(fp, "%s", cmd.c_str());
        fclose(fp);
    }
    for (int pi = 0; pi < 256; pi++) palette_set_entry((uint8_t)pi, 0, 0, 0);
    video_.present();

    should_exit_ = true;
    return_code_ = 1;
}

// ---- sub_21E29: draw the 24x16 base tiles ----
void Pack::render_base_tiles() {
    if (!layer_base_tiles_ || base_tiles_data_.empty()) return;

    // sub_21E29 loc_21EB2..loc_22045: var_1C starts at -16 (0xFFFFFFF0) and adds 16 per row
    // while si < 0x115 (277), so the original scans 19 rows (ty = cam_tile_y_ - 1 ..
    // cam_tile_y_ + 17). The scan starts at cam_tile_x_ (no -1), with screen offsets -7/-16.
    // Deviation: the scan is widened by a couple of tiles on each side (and two extra rows)
    // so no gap appears while scrolling or near map edges.
    int start_tx = cam_tile_x_ - 2;
    int start_ty = cam_tile_y_ - 2;
    int end_tx = cam_tile_x_ + (SCREEN_W / TOWN_TILE_W) + 4; // +17
    int end_ty = cam_tile_y_ + (SCREEN_H / TOWN_TILE_H) + 8; // +20

    for (int ty = start_ty; ty < end_ty; ty++) {
        for (int tx = start_tx; tx < end_tx; tx++) {
            if (tx < 0 || tx >= (int)map_w_ || ty < 0 || ty >= (int)map_h_) continue;

            int idx = ty * (int)map_w_ + tx;
            uint16_t b_idx = layer_base_tiles_[idx];

            // loc_21EDC: the column loop starts at tx = cam_tile_x_ (edi/ecx are initialised
            // from dword_56BFA >> 10h with no -1), and the screen X accumulator var_C starts
            // at -7 ("mov [esp+20h+var_C],0FFFFFFF9h"). var_1C starts at -16 and adds 16 per
            // row, the same -16 used in the ebp backbuffer offset. Player sprites use X=+5,
            // Y=-16, so the 12-pixel X difference puts the player mid-tile.
            int screen_x = (tx - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ - 7;
            int screen_y = (ty - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ - TOWN_TILE_H;

            if (b_idx < base_tiles_data_.size()) {
                blit_tile_24x16(screen_x, screen_y, base_tiles_data_[b_idx].data());
            }
        }
    }
}

// ---- sub_22897: foot shadows for the player and active NPCs ----
// sub_190A3 calls sub_21D70 -> sub_21E29 -> sub_22897 -> sub_2294D, so shadows are drawn
// after the base tiles and just before render_sprites_and_objects. One sprite, off_56C00
// (a 19x7 dot ellipse in the raw sub_2B2A1 format), is reused for the player and every
// active NPC at (feet X - 8, feet Y + 9).
// Player: the same px/py as render_sprites_and_objects (word_82947 + 9, dword_82943+2 - 8),
// with no clipping beyond sub_2B2A1's X > 319 guard.
// NPCs (loc_228C8..loc_22940): only active == 1 (byte_19E144[npc_idx*40]). The original
// walks slots 2..181; the port stops at MAX_TOWN_NPCS like every other NPC loop.
// Clip guards: nx > 10, nx < 305 (0x131), the second 16-bit field of the unk_82938 record
// > -5, and ny < 200. The port keeps the same four bounds.
void Pack::render_shadows() {
    if (shadow_sprite_.pixels.empty()) return;

    // sub_13D04: during an EVE cutscene only EVE actor shadows are drawn (51 slots, active,
    // the same clip and (sx-8, sy+9) offset with off_56C00). The EVE loop sub_13B8D calls it
    // every frame; sub_22897 itself only walks the .TWN NPCs.
    if (in_eve_cutscene_) {
        for (int a = 0; a < MAX_EVE_ACTORS; a++) {
            const EveActorState& st = eve_actors_[a];
            if (!st.active) continue;
            // Same formula as get_eve_actor_screen_box (unk_82938/unk_8293A).
            int sx = (st.tile_x - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ + st.fine_x;
            int sy = (st.tile_y - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ + st.fine_y;
            // sub_13D04 clip: 0x0A < sx < 0x131 and sy < 0xC8.
            if (sx <= 0x0A || sx >= 0x131 || sy >= 0xC8) continue;
            blit_cursor_raw(sx - 8, sy + 9, shadow_sprite_);
        }
        return;
    }

    // 1. Player shadow (dword_82943+2 - 8, word_82947 + 9), same screen formula as px/py in
    // render_sprites_and_objects, including the -16 offset.
    int px = (player_tile_x_ - cam_tile_x_) * TOWN_TILE_W + player_fine_x_ - cam_fine_x_ + 5;
    int py = (player_tile_y_ - cam_tile_y_) * TOWN_TILE_H + player_fine_y_ - cam_fine_y_ - TOWN_TILE_H;
    blit_cursor_raw(px - 8, py + 9, shadow_sprite_);

    // 2. Active NPC shadows (loc_228C8..loc_22940)
    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        if (npcs_[i].active != 1) continue;

        // Same screen formula as block 3 (active NPCs) in render_sprites_and_objects
        // (unk_82938 = nx, unk_8293A = ny, including the -16 offset). loc_1F7E4 and friends
        // add or subtract fine_offset (unk_5441E) per direction, so NPCs carry their
        // sub-tile offset into screen space just like the player.
        int n_fine_x = 0, n_fine_y = 0;
        if (npcs_[i].direction == 0x48)      n_fine_y = -(int)npc_fine_offset_[i];
        else if (npcs_[i].direction == 0x50) n_fine_y =  (int)npc_fine_offset_[i];
        else if (npcs_[i].direction == 0x4B) n_fine_x = -(int)npc_fine_offset_[i];
        else if (npcs_[i].direction == 0x4D) n_fine_x =  (int)npc_fine_offset_[i];

        int nx = (npcs_[i].tile_x - cam_tile_x_) * TOWN_TILE_W + n_fine_x - cam_fine_x_ + 5;
        int ny = (npcs_[i].tile_y - cam_tile_y_) * TOWN_TILE_H + n_fine_y - cam_fine_y_ - TOWN_TILE_H;

        // sub_22897 clip: nx > 10, nx < 305 (0x131), ny > -5 and ny < 200 (0xC8).
        if (nx <= 0x0A || nx >= 0x131 || ny >= 0xC8 || ny <= -5) continue;

        blit_cursor_raw(nx - 8, ny + 9, shadow_sprite_);
    }

    // EVE cutscene actor shadows are handled by the in_eve_cutscene_ branch above
    // (sub_13D04), separately from the TWN NPC loop in sub_22897.
}

// ---- sub_21E29 and sub_2294D: top tiles and Y-sorted sprite rendering ----

// sub_2294D (loc_22961..loc_22A00) viewport clip, applied before calling sub_2B240.
// A sprite whose screen rectangle is completely off-screen is skipped (jump to
// loc_22A00). Without it, far off-screen positions wrap around the 1-D backbuffer and
// overdraw unrelated tiles. The constants 1, 0x13E (318), 0xC5 (197) and the height guard
// 0x64 (100) come straight from the disassembly.
static bool sub_2294D_is_offscreen(int left_x, int top_y, int width, int height) {
    int right_x  = left_x + width;
    int bottom_y = top_y + height;
    if (right_x <= 1)   return true;  // fully left of the screen
    if (left_x >= 0x13E) return true; // fully right of the screen (318)
    if (bottom_y <= 1)   return true; // fully above the screen
    if (top_y >= 0xC5)   return true; // fully below the screen (197)
    if (height >= 0x64)  return true; // the original's bad-height guard (>= 100)
    return false;
}

void Pack::render_sprites_and_objects() {
    std::vector<DrawSpriteEntry> draw_list;

    // 1. Top tiles (registered by sub_21E29).
    // The original scans 19 rows (var_1C from -16 to 272) starting at cam_tile_x_.
    // Deviation: the collection range is widened to X -4 and Y -8..+24 so large top tiles
    // (roofs and trees up to 96 px wide with y_offset down to -128) do not pop in and out at
    // the screen edges. The map buffer is allocated to map_h + 6, so writes stay in range.
    int start_tx = cam_tile_x_ - 4;
    int start_ty = cam_tile_y_ - 8;
    int end_tx = cam_tile_x_ + (SCREEN_W / TOWN_TILE_W) + 4; // +17
    int end_ty = cam_tile_y_ + (SCREEN_H / TOWN_TILE_H) + 12; // +24

    if (layer_top_tiles_) {
        // layer_top_tiles_ is allocated as map_w_ * (map_h_ + 6), matching sub_213A6 and
        // sub_24017. Loaded tiles only cover map_w_ * map_h_, but sub_1B082 and other
        // interaction code write a top-tile index at row + 1, which can land in the padding
        // rows below the map, so the scan runs to map_h_ + 6. Unwritten cells stay 0xFFFF.
        int top_tile_end_ty = (int)(map_h_ + 6);
        for (int ty = start_ty; ty < end_ty; ty++) {
            for (int tx = start_tx; tx < end_tx; tx++) {
                if (tx < 0 || tx >= (int)map_w_ || ty < 0 || ty >= top_tile_end_ty) continue;

                int idx = ty * (int)map_w_ + tx;
                uint16_t top_idx = layer_top_tiles_[idx];

                if (top_idx != 0xFFFF && top_idx < top_tile_sprites_.size()) {
                    // Same -7/-TOWN_TILE_H offsets as render_base_tiles (unk_82938 =
                    // var_C + cam_fine_x with var_C starting at -7 and stepping 24; unk_8293A =
                    // var_1C + cam_fine_y with var_1C starting at -16 and stepping 16).
                    int sx = (tx - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ - 7;
                    int sy = (ty - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ - TOWN_TILE_H;
                    const auto& sp = top_tile_sprites_[top_idx];

                    DrawSpriteEntry entry;
                    entry.group = 2; // top tile
                    entry.sprite_idx = top_idx;
                    entry.screen_x = sx + sp.x_offset;
                    entry.screen_y = sy + sp.y_offset;
                    entry.foot_y = sy + sp.foot_offset_y;
                    draw_list.push_back(entry);
                }
            }
        }
    }

    // 2. Player Sprite (ASM 0x1D12F / 0x2117F + sub_1D12F fine logic)
    // Original: screenX = (playerX - camX)*24 + fineX + 5,
    //           screenY = (playerY - camY)*16 + fineY - 16.
    // loc_1D23C: shl edx,4 / add edx,ebx / sub edx,10h. The -16 is applied
    // unconditionally before the byte_544CE branch and stored to word_82947 at loc_1DA1C.
    // It is a rendering offset only; walkability is decided from player_tile_x_/y_.
    // Without it the sprite sits one tile (16 px) too low.
    int px = (player_tile_x_ - cam_tile_x_) * TOWN_TILE_W + player_fine_x_ - cam_fine_x_ + 5;
    int py = (player_tile_y_ - cam_tile_y_) * TOWN_TILE_H + player_fine_y_ - cam_fine_y_ - TOWN_TILE_H;

    int dir_offset = 0;
    if (player_dir_ == 0x48) dir_offset = 12;      // UP
    else if (player_dir_ == 0x50) dir_offset = 0;  // DOWN
    else if (player_dir_ == 0x4B) dir_offset = 4;  // LEFT
    else if (player_dir_ == 0x4D) dir_offset = 8;  // RIGHT

    int player_frame = player_base_frame_ + dir_offset + (player_anim_frame_ % 4);
    if (!citizen_sprites_.empty()) {
        player_frame %= (int)citizen_sprites_.size();
    }

    DrawSpriteEntry p_entry;
    p_entry.group = 4; // citizen sprite
    p_entry.sprite_idx = player_frame;
    p_entry.screen_x = px;
    p_entry.screen_y = py;
    // sub_2278F sorts on screen_y + y_offset; sorting on foot_offset_y (h + y_offset)
    // gives the same visual order and keeps sprites from hiding behind top tiles.
    if (player_frame >=0 && player_frame < (int)citizen_sprites_.size()) {
        p_entry.foot_y = py + citizen_sprites_[player_frame].foot_offset_y;
    } else {
        // Py already includes the -16 offset, so reuse it without another correction.
        p_entry.foot_y = py + 8;
    }
    draw_list.push_back(p_entry);

    // 3. Active NPCs
    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        if (npcs_[i].active == 1) {
            // loc_1F706: shl ebx,4 / add bx,word ptr dword_56BF6 / sub ebx,10h, so NPC
            // screen Y (unk_8293A) also gets the -16. loc_1F7E4 and friends add or subtract
            // fine_offset (unk_5441E) per direction, so NPCs carry their sub-tile offset.
            int n_fine_x = 0, n_fine_y = 0;
            if (npcs_[i].direction == 0x48)      n_fine_y = -(int)npc_fine_offset_[i];
            else if (npcs_[i].direction == 0x50) n_fine_y =  (int)npc_fine_offset_[i];
            else if (npcs_[i].direction == 0x4B) n_fine_x = -(int)npc_fine_offset_[i];
            else if (npcs_[i].direction == 0x4D) n_fine_x =  (int)npc_fine_offset_[i];

            int nx = (npcs_[i].tile_x - cam_tile_x_) * TOWN_TILE_W + n_fine_x - cam_fine_x_ + 5;
            int ny = (npcs_[i].tile_y - cam_tile_y_) * TOWN_TILE_H + n_fine_y - cam_fine_y_ - TOWN_TILE_H;

            int npc_dir_offset = 0;
            if (npcs_[i].direction == 0x48) npc_dir_offset = 12;
            else if (npcs_[i].direction == 0x50) npc_dir_offset = 0;
            else if (npcs_[i].direction == 0x4B) npc_dir_offset = 4;
            else if (npcs_[i].direction == 0x4D) npc_dir_offset = 8;

            // sub_24646/sub_1F6E4: the animation base frame is sprite_id (+0x1D) * 16 (the
            // separate sprite_base_frame field is only a portrait_id lookup), and town NPCs
            // always use group 4 (citizen) instead of reading a group from the data.
            int npc_frame = (npcs_[i].sprite_id * 16) + npc_dir_offset + (npc_anim_frame_[i] % 4);

            DrawSpriteEntry n_entry;
            n_entry.group = 4; // town NPCs are always citizen group (sub_24646 "mov ecx,4")
            n_entry.sprite_idx = npc_frame;
            n_entry.screen_x = nx;
            n_entry.screen_y = ny;
            if (n_entry.group == 4 && npc_frame >=0 && npc_frame < (int)citizen_sprites_.size()) {
                n_entry.foot_y = ny + citizen_sprites_[npc_frame].foot_offset_y;
            } else {
                // Ny already includes the -16 offset.
                n_entry.foot_y = ny + 8;
            }
            draw_list.push_back(n_entry);
        }
    }

    // 3b. EVE cutscene actors (sub_12857 op13, CHARACTE/*.NC2, group 1), matching the
    // unk_82934 (active) / 82938 / 8293A (screen position) / 840A6 (sprite) tables.
    // loc_12BE2 uses (tile - cam)*24/16 + fine with no +5/-16 adjustment, as in the town.
    for (int a = 0; a < MAX_EVE_ACTORS; a++) {
        const EveActorState& st = eve_actors_[a];
        if (!st.active) continue;
        int sx = (st.tile_x - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ + st.fine_x;
        int sy = (st.tile_y - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ + st.fine_y;
        const NC2Frame* fr = nullptr;
        if (st.sprite_loaded) fr = st.sprite.frame(st.dir, st.frame_idx % 11);
        DrawSpriteEntry e_entry;
        e_entry.group = 1;
        e_entry.sprite_idx = a; // rendering looks up eve_actors_[sprite_idx]
        e_entry.screen_x = sx;
        e_entry.screen_y = sy;
        if (fr && fr->w > 0 && fr->h > 0) {
            e_entry.foot_y = sy + fr->y_off + fr->h;
        } else {
            e_entry.foot_y = sy + 8;
        }
        draw_list.push_back(e_entry);
    }

    // 4. sub_2278F: sort by foot_y. The original is an insertion sort that stops on "jge",
    // so equal foot_y keeps registration order. std::stable_sort preserves that; an unstable
    // sort would swap equal-foot_y tiles between frames and make overlaps flicker.
    std::stable_sort(draw_list.begin(), draw_list.end(), [](const DrawSpriteEntry& a, const DrawSpriteEntry& b) {
        return a.foot_y < b.foot_y;
    });

    // 5. sub_2294D: render every sprite in sorted order.
    // Draw-list log, gated on G2_PACK_TRACE (too noisy during 60 fps roaming).
    if (dbg_env("G2_PACK_TRACE")) {
        int ng[8] = {};
        std::string det;
        for (auto& it : draw_list) {
            if (it.group >= 0 && it.group < 8) ng[it.group]++;
            if (it.group == 1 || it.group == 4) {
                char b[40];
                snprintf(b, sizeof(b), " g%d#%d(%d,%d)", it.group, it.sprite_idx,
                         it.screen_x, it.screen_y);
                det += b;
            }
        }
        fprintf(stderr, "PACK: drawlist n=%zu g1=%d g2=%d g3=%d g4=%d g6=%d%s\n",
                draw_list.size(), ng[1], ng[2], ng[3], ng[4], ng[6], det.c_str());
    }
    for (const auto& item : draw_list) {
        if (item.group == 2) {
            if (item.sprite_idx >= 0 && item.sprite_idx < (int)top_tile_sprites_.size()) {
                const auto& sp = top_tile_sprites_[item.sprite_idx];
                // Item.screen_x/screen_y already include sp.x_offset/y_offset from the
                // registration loop above, so they are left_x/top_y.
                if (!sub_2294D_is_offscreen(item.screen_x, item.screen_y, sp.width, sp.height)) {
                    blit_top_tile_transparent(item.screen_x, item.screen_y, sp);
                }
            }
        } else if (item.group == 4) {
            if (item.sprite_idx >= 0 && item.sprite_idx < (int)citizen_sprites_.size()) {
                const auto& sp = citizen_sprites_[item.sprite_idx];
                if (!sp.data.empty()) {
                    int left_x = item.screen_x + sp.x_offset;
                    int top_y  = item.screen_y + sp.y_offset;
                    if (!sub_2294D_is_offscreen(left_x, top_y, sp.width, sp.height)) {
                        blit_sprite_rle(left_x, top_y, sp.data.data());
                    }
                }
            }
        } else if (item.group == 3) {
            if (item.sprite_idx >= 0 && item.sprite_idx < (int)itembox_sprites_.size()) {
                const auto& sp = itembox_sprites_[item.sprite_idx];
                if (!sp.data.empty()) {
                    int left_x = item.screen_x + sp.x_offset;
                    int top_y  = item.screen_y + sp.y_offset;
                    if (!sub_2294D_is_offscreen(left_x, top_y, sp.width, sp.height)) {
                        blit_sprite_rle(left_x, top_y, sp.data.data());
                    }
                }
            }
        } else if (item.group == 6) {
            if (item.sprite_idx >= 0 && item.sprite_idx < (int)item_sprites_.size()) {
                const auto& sp = item_sprites_[item.sprite_idx];
                if (!sp.data.empty()) {
                    int left_x = item.screen_x + sp.x_offset;
                    int top_y  = item.screen_y + sp.y_offset;
                    if (!sub_2294D_is_offscreen(left_x, top_y, sp.width, sp.height)) {
                        blit_sprite_rle(left_x, top_y, sp.data.data());
                    }
                }
            }
        } else if (item.group == 1) {
            // EVE cutscene actor: raw CHARACTE/*.NC2 blit (0xFF transparent).
            int a = item.sprite_idx;
            if (a >= 0 && a < MAX_EVE_ACTORS && eve_actors_[a].active &&
                eve_actors_[a].sprite_loaded) {
                const EveActorState& st = eve_actors_[a];
                const NC2Frame* fr = st.sprite.frame(st.dir, st.frame_idx % 11);
                if (fr && fr->w > 0 && fr->h > 0 && !fr->pixels.empty()) {
                    int left_x = item.screen_x + fr->x_off;
                    int top_y = item.screen_y + fr->y_off;
                    if (!sub_2294D_is_offscreen(left_x, top_y, fr->w, fr->h)) {
                        uint8_t* backbuf = video_.get_backbuffer();
                        if (backbuf) {
                            blit_nc2_frame(backbuf, SCREEN_W, SCREEN_H,
                                            left_x, top_y, *fr);
                        }
                    }
                }
            }
        }
    }

    // 6. sub_2294D loc_22A01: after the sorted draw list, mask the screen edges with the
    // left frame (0, 0, unk_54630) and the right frame (0x127 = 295, 0, off_55A99). The
    // original calls sub_2B240 twice every frame regardless of town or camera, so no
    // condition is applied here.
    if (!left_frame_data_.empty()) {
        blit_sprite_rle(0, 0, left_frame_data_.data());
    }
    if (!right_frame_data_.empty()) {
        blit_sprite_rle(0x127, 0, right_frame_data_.data());
    }
}

// ---- get_mouse_tile: screen position -> map tile (sub_190A3 loc_191D3..loc_1948C) ----
//   target_tx = (mouse_x + 9) / 24 + cam_tile_x_    (cam_fine_x_ plays no part)
//   target_ty = mouse_y / 16 + cam_tile_y_, minus 1 more when cam_fine_y_ > 0
// sub_190A3 runs this every frame to refresh the hover highlight and cursor, not only on
// click, so it is a shared helper.
void Pack::get_mouse_tile(int mouse_x, int mouse_y, int& out_tx, int& out_ty) const {
    out_tx = (mouse_x + 9) / TOWN_TILE_W + cam_tile_x_;
    out_ty = mouse_y / TOWN_TILE_H + cam_tile_y_;
    if (cam_fine_y_ > 0) out_ty -= 1;
}

// ---- get_npc_screen_box: NPC sprite rectangle (sub_1E4B4 loc_1E75E) ----
// sub_1E4B4 derives the box from unk_82935 (group id), unk_82936 (frame/coords),
// unk_82938, unk_8293A, unk_840A6 (per-group/frame sprite pointers) and dword_921AE, all of
// which sit in the zero-filled area beyond Object 2's physical data (0x2D000 bytes, 45
// pages). They hold no values in the EXE and are filled at runtime while .SPR resources
// load. The port therefore reuses the position and loaded width/height/x_offset/y_offset
// that render_sprites_and_objects already computes for the same frame.
bool Pack::get_npc_screen_box(int npc_idx, int& out_left, int& out_top, int& out_w, int& out_h) const {
    if (npc_idx < 0 || npc_idx >= MAX_TOWN_NPCS) return false;
    const PackNPC& npc = npcs_[npc_idx];
    if (npc.active != 1) return false;

    // Same screen formula as block 3 (active NPCs) in render_sprites_and_objects,
    // including the -16 from sub_1F6E4 loc_1F706, and the per-direction fine_offset
    // applied by loc_1F7E4.
    int n_fine_x = 0, n_fine_y = 0;
    if (npc.direction == 0x48)      n_fine_y = -(int)npc_fine_offset_[npc_idx];
    else if (npc.direction == 0x50) n_fine_y =  (int)npc_fine_offset_[npc_idx];
    else if (npc.direction == 0x4B) n_fine_x = -(int)npc_fine_offset_[npc_idx];
    else if (npc.direction == 0x4D) n_fine_x =  (int)npc_fine_offset_[npc_idx];

    int nx = (npc.tile_x - cam_tile_x_) * TOWN_TILE_W + n_fine_x - cam_fine_x_ + 5;
    int ny = (npc.tile_y - cam_tile_y_) * TOWN_TILE_H + n_fine_y - cam_fine_y_ - TOWN_TILE_H;

    int npc_dir_offset = 0;
    if (npc.direction == 0x48) npc_dir_offset = 12;
    else if (npc.direction == 0x50) npc_dir_offset = 0;
    else if (npc.direction == 0x4B) npc_dir_offset = 4;
    else if (npc.direction == 0x4D) npc_dir_offset = 8;

    // As in render_sprites_and_objects: the frame base is sprite_id * 16 and the group is
    // always 4 (citizen); no group is read from the data.
    int npc_frame = (npc.sprite_id * 16) + npc_dir_offset + (npc_anim_frame_[npc_idx] % 4);
    if (npc_frame < 0 || npc_frame >= (int)citizen_sprites_.size()) return false;
    const PackSprite* sp = &citizen_sprites_[npc_frame];
    if (sp->data.empty()) return false;

    out_left = nx + sp->x_offset;
    out_top  = ny + sp->y_offset;
    out_w = sp->width;
    out_h = sp->height;
    return true;
}

// ---- sub_18303: one-pixel rectangle border ----
// Ports sub_18303(x_start, y_start, x_end = x_start + 24, y_end = y_start + 16, dest, color),
// which calls sub_2B4D6 (horizontal lines at y and y+h over x..x+w-1) and sub_2B4A8
// (vertical lines at x and x+w over y..y+h-1). Those line routines have no clipping in the
// original because callers stay near the screen; the port clamps defensively instead.
void Pack::draw_rect_border(int x, int y, int w, int h, uint8_t color) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;

    auto put_px = [&](int px, int py) {
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
            dest[py * SCREEN_W + px] = color;
        }
    };

    int x2 = x + w; // sub_18303: X_end = X_start + 24 (0x18)
    int y2 = y + h; // sub_18303: Y_end = Y_start + 16 (0x10)

    // sub_2B4D6 twice: horizontal lines at y and y2, length w
    for (int i = 0; i < w; i++) {
        put_px(x + i, y);
        put_px(x + i, y2);
    }
    // sub_2B4A8 twice: vertical lines at x and x2, length h
    for (int i = 0; i < h; i++) {
        put_px(x, y + i);
        put_px(x2, y + i);
    }
}

// ---- render_tile_highlight: hover highlight (sub_190A3 loc_191D3..loc_193BF) ----
// Right after sub_2294D, sub_190A3 checks the tile under the mouse every frame and, when it
// holds a chest/item or a building, draws a 24x16 one-pixel border in colour 1 (loc_19223,
// loc_192FE). Plain floor tiles get no box. NPC hit-testing (dword_544CA) runs at loc_193BF.
void Pack::render_tile_highlight() {
    // Deviation: handle_tile_click walks the player to a distant chest, building or NPC and
    // records the target in pending_interact_x_/y_. The original works from the live mouse
    // position (dword_7CF24/7CF28) while the button is held, but a click-to-path port keeps
    // scrolling the camera, so the tile under the mouse changes and the highlight would
    // vanish on the first step. While pending_interact_ is set, highlight that destination
    // with the same condition, size and colour as loc_19223 / loc_192FE. It is only set for
    // real item/building/NPC targets, never for event-only tiles.
    if (pending_interact_x_ >= 0 && pending_interact_y_ >= 0 &&
        pending_interact_x_ < (int)map_w_ && pending_interact_y_ < (int)map_h_) {
        int pidx = pending_interact_y_ * (int)map_w_ + pending_interact_x_;
        // Same -7/-TOWN_TILE_H correction as render_base_tiles, so the box sits where the
        // tile is actually drawn (sub_190A3 has its own UI constants for its box).
        int pscreen_x = (pending_interact_x_ - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ - 7;
        int pscreen_y = (pending_interact_y_ - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ - TOWN_TILE_H;

        // loc_19223: chest/item destination, using the same g_event_flags[0xC8] progress
        // test as the hover branch.
        if (layer_item_ && layer_item_[pidx] != 0) {
            uint16_t item_category = layer_item_[pidx] / 2000;
            if (item_category <= g_event_flags[0xC8]) {
                draw_rect_border(pscreen_x, pscreen_y, TOWN_TILE_W, TOWN_TILE_H, 1);
                return;
            }
        }

        // loc_192FE: building, shop or inn destination.
        if (layer_building_ && layer_building_[pidx] != 0) {
            draw_rect_border(pscreen_x, pscreen_y, TOWN_TILE_W, TOWN_TILE_H, 1);
            return;
        }

        // NPC destination (pending set through has_npc_here): highlight that NPC itself so
        // the target stays visible while walking, regardless of the hover test below.
        if (layer_npc_ && layer_npc_[pidx] != 0) {
            for (int i = 0; i < MAX_TOWN_NPCS; i++) {
                if (npcs_[i].active == 1 && npcs_[i].tile_x == pending_interact_x_ &&
                    npcs_[i].tile_y == pending_interact_y_) {
                    int nl, nt, nw, nh;
                    if (get_npc_screen_box(i, nl, nt, nw, nh)) {
                        draw_rect_border(nl, nt, nw, nh, 1);
                        return;
                    }
                    break;
                }
            }
        }
    }

    int mx = input_.get_mouse_x();
    int my = input_.get_mouse_y();

    int tx, ty;
    get_mouse_tile(mx, my, tx, ty);

    // sub_190A3 loc_191D3..loc_193BF falls through to the loc_193BF NPC hit test
    // (dword_544CA) even after drawing an item or building box; both can be drawn. The port
    // therefore does not return early and always runs the NPC hover test at the end. The map
    // bounds check protects array access and applies only to the item/building branch; the
    // NPC test compares screen pixels and does not depend on tx/ty.
    bool tile_in_bounds = (tx >= 0 && tx < (int)map_w_ && ty >= 0 && ty < (int)map_h_);
    if (tile_in_bounds) {
        int idx = ty * (int)map_w_ + tx;
        // Same -7/-TOWN_TILE_H correction as render_base_tiles, matching the drawn tile.
        int screen_x = (tx - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ - 7;
        int screen_y = (ty - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ - TOWN_TILE_H;

        // ---- loc_19223: item or chest tile ----
        if (layer_item_ && layer_item_[idx] != 0) {
            uint16_t item_id = layer_item_[idx];
            // byte_7D954 is offset 0xC8 of the g_event_flags buffer (unk_7D88C), one byte
            // before byte_7D955 = g_event_flags[0xC9]. eax = (item_id/1000)/2 =
            // item_id/2000; when eax is larger the item belongs to a later scenario stage, so
            // the highlight is skipped and the building test at loc_192FE runs instead.
            uint16_t item_category = item_id / 2000;
            if (item_category <= g_event_flags[0xC8]) {
                draw_rect_border(screen_x, screen_y, TOWN_TILE_W, TOWN_TILE_H, 1);
            }
        } else if (layer_building_ && layer_building_[idx] != 0) {
            // ---- loc_192FE: building, shop or inn tile ----
            // The original jumps to loc_193BF after drawing an item box and skips the building
            // test, so the else-if form is correct.
            draw_rect_border(screen_x, screen_y, TOWN_TILE_W, TOWN_TILE_H, 1);
        }
    }

    // ---- loc_193BF: NPC highlight (sub_1E4B4 loc_1E75E) ----
    // The mouse must be inside the NPC's on-screen sprite rectangle (get_npc_screen_box) and
    // the left button must not be held (dword_7CF20 != 1, the inverse of is_left_button();
    // was_left_clicked() is a one-shot event and does not match). The original keeps
    // overwriting dword_544CA with the last matching NPC each frame (no break).
    if (!input_.is_left_button()) {
        for (int i = 0; i < MAX_TOWN_NPCS; i++) {
            int nl, nt, nw, nh;
            if (!get_npc_screen_box(i, nl, nt, nw, nh)) continue;
            if (mx >= nl && mx < nl + nw && my >= nt && my < nt + nh) {
                draw_rect_border(nl, nt, nw, nh, 1);
            }
        }
    }

}

// ---- render_mouse_cursor: directional cursor (sub_190A3 loc_1948C) ----
// off_53D0C (Object 2) holds 800 bytes that render as eight directional arrows.
// Hotspots decoded from
//   dword_18A00 dd 2 dup(0FFFCFFFCh),0,0FFF9FFF9h
//   dword_18A10 dd 0,0FFF9FFF9h,2 dup(0FFFCFFFCh)
// give X = [-4,-4,-4,-4,0,0,-7,-7] and Y = [0,0,-7,-7,-4,-4,-4,-4]. These match the
// arrow shapes: index 0 (up) is 9x8 with its tip at column 4 of the top row (X_off = -4,
// Y_off = 0), and index 4 (left) is 9 high with its tip at row 4, column 0 (X_off = 0,
// Y_off = -4).
void Pack::render_mouse_cursor() {
    if (cursor_sprites_.size() != 8) return; // no cursor sprites loaded: draw nothing

    int mx = input_.get_mouse_x();
    int my = input_.get_mouse_y();

    // sub_2E1A4 (dword_8279C+2) keeps the previous value when the lookup is not a direction,
    // so a 0 from compute_mouse_move_direction leaves cursor_dir_ unchanged.
    uint8_t dir = compute_mouse_move_direction(mx, my);
    if (dir != 0) cursor_dir_ = dir;

    int base_index;
    switch (cursor_dir_) {
        case 0x48: base_index = 0; break; // Up
        case 0x50: base_index = 2; break; // Down
        case 0x4B: base_index = 4; break; // Left
        case 0x4D: base_index = 6; break; // Right
        default:   base_index = 2; break; // fallback: down
    }

    // loc_192FE/loc_19223/loc_193BF: over a chest, building or NPC the "+1" action/target
    // cursor variant is used.
    bool action = false;
    int tx, ty;
    get_mouse_tile(mx, my, tx, ty);
    if (tx >= 0 && tx < (int)map_w_ && ty >= 0 && ty < (int)map_h_) {
        int idx = ty * (int)map_w_ + tx;
        if (layer_item_ && layer_item_[idx] != 0) {
            uint16_t item_category = layer_item_[idx] / 2000;
            if (item_category <= g_event_flags[0xC8]) action = true;
        }
        if (!action && layer_building_ && layer_building_[idx] != 0) action = true;
    }
    int hover_npc;
    if (!action && is_mouse_over_npc(mx, my, hover_npc)) action = true;

    int index = base_index + (action ? 1 : 0);
    if (index < 0 || index >= (int)cursor_sprites_.size()) return;

    // Hardware cursor: the OS tracks the position, so only the image changes. Fall back to
    // the software blit when creation failed.
    if (cursor_base_ >= 0 && video_.cursor_select(cursor_base_ + index)) return;

    // Hotspots come from kCursorHotX/Y near the top of the file (see the note there).
    blit_cursor_raw(mx + kCursorHotX[index], my + kCursorHotY[index], cursor_sprites_[index]);
}

// Hardware cursor creation and its scale/palette tracking live in Video::cursor_* (video.cpp).

// ---- render_town_frame: draw the whole town screen (sub_190A3) ----
void Pack::render_town_frame() {
    // sub_190A3 loc_19123 order: sub_21D70 (tile animation, driven separately) -> sub_21E29
    // (base tiles) -> sub_22897 (shadows) -> sub_2294D (characters and top tiles).
    // Shadows must go between the tiles and the sprites so they land on the floor.
    render_base_tiles();
    render_shadows();
    render_sprites_and_objects();
    render_tile_highlight();
    render_mouse_cursor();
}

// ---- update_player_movement: player movement and camera (sub_1D12F) ----
// byte_544CE (player_move_step_): 0..3 fast (4 steps) / 0..7 slow (8 steps), +1 per frame.
// Screen position uses X = bh*6 (fast) or bh*3 (slow), Y = bh*4 or bh*2 (word_82947,
// dword_82943+2). byte_F30C0 (player_anim_counter_) increments per frame and advances the
// walk frame every 2 (fast) or 4 (slow). At the last step sub_1EBC2 updates layer_npc_ and
// the tile coordinates and sub_1A785 checks events; sub_2117F keeps the camera on the player.
void Pack::update_player_movement() {
    // 0x1D23C: "test bh,bh (byte_544CE); jbe loc_1DA1C". At player_move_step_ == 0 the
    // original re-decides the direction from the current mouse state every time
    // (decide_step_direction: sub_2E1A4 + sub_1AA46 + corner sliding), so no precomputed path
    // exists. Mid-tile (player_move_step_ != 0) it keeps the committed player_dir_ and just
    // accumulates pixel deltas, as below.
    if (player_move_step_ == 0) {
        decide_step_direction();
        if (!is_player_moving_) return;

        // loc_1DFE1: at step start the previous occupancy (dword_F30BA/F30BE) is cleared
        // and the destination tile is marked 0xFF, before the first pixel moves. NPCs then
        // see the destination as blocked for the whole step. dword_F3369 (sub_1EBC2) is
        // the separate one-cell reservation; the port's layer check already covers it.
        if (layer_npc_) {
            const int prev_idx = player_tile_y_ * (int)map_w_ + player_tile_x_;
            if (prev_idx >= 0 && prev_idx < (int)tile_count_)
                layer_npc_[prev_idx] = 0;
            int nx = player_tile_x_, ny = player_tile_y_;
            if (player_dir_ == 0x48) ny -= 1;
            else if (player_dir_ == 0x50) ny += 1;
            else if (player_dir_ == 0x4B) nx -= 1;
            else if (player_dir_ == 0x4D) nx += 1;
            if (nx >= 0 && nx < (int)map_w_ && ny >= 0 && ny < (int)map_h_)
                layer_npc_[ny * (int)map_w_ + nx] = 0xFF;
        }

        // loc_1DA1C..loc_1E463: the first frame of a new tile is the commit frame. It only
        // sets byte_544CE (player_move_step_) to 1 and returns with no pixel delta, jumping
        // straight to loc_1E463; movement starts on the next frame. A tile therefore takes
        // 5 frames (FAST) or 9 frames (SLOW).
        player_move_step_ = 1;
        // Every commit point (loc_1E407, loc_1E3F9, ...) pairs byte_544CE = 1 with
        // byte_F30C0 (player_anim_counter_) = 0, restarting the walk cycle on the new tile.
        player_anim_counter_ = 0;
        return;
    }
    if (!is_player_moving_) return;

    uint8_t cur_dir = player_dir_;

    // Animation counter (byte_F30C0): advance the frame every 2 (fast) or 4 (slow) ticks.
    player_anim_counter_++;
    int anim_threshold = (move_steps_per_tile_ == MOVE_STEPS_FAST) ? 2 : 4;
    if (player_anim_counter_ >= anim_threshold) {
        player_anim_counter_ = 0;
        player_anim_frame_ = (player_anim_frame_ + 1) & 0x03;
    }

    // Movement: up/down 4 px and left/right 6 px when fast, 2 px and 3 px when slow.
    int delta_x = 0, delta_y = 0;
    bool is_fast = (move_steps_per_tile_ == MOVE_STEPS_FAST);
    if (cur_dir == 0x48) { // Up
        delta_y = is_fast ? -4 : -2;
    } else if (cur_dir == 0x50) { // Down
        delta_y = is_fast ? 4 : 2;
    } else if (cur_dir == 0x4B) { // Left
        delta_x = is_fast ? -6 : -3;
    } else if (cur_dir == 0x4D) { // Right
        delta_x = is_fast ? 6 : 3;
    }

    player_fine_x_ += delta_x;
    player_fine_y_ += delta_y;

    int prev_tile_x = player_tile_x_;
    int prev_tile_y = player_tile_y_;

    // Tile boundary, handled by sub_1EBC2. In every direction handler (loc_1D2C3, loc_1D487,
    // loc_1D642, loc_1D82E) the tile coordinates word_15BA18 / dword_15BA14+2 change only
    // when the previous step value was exactly 4 (fast) or 8 (slow), i.e. on the single step
    // where the fine offset reaches a full tile. Both directions use that exact compare here.
    if (player_fine_x_ >= TOWN_TILE_W) {
        player_fine_x_ -= TOWN_TILE_W;
        player_tile_x_++;
    } else if (player_fine_x_ <= -TOWN_TILE_W) {
        player_fine_x_ += TOWN_TILE_W;
        player_tile_x_--;
    }
    if (player_fine_y_ >= TOWN_TILE_H) {
        player_fine_y_ -= TOWN_TILE_H;
        player_tile_y_++;
    } else if (player_fine_y_ <= -TOWN_TILE_H) {
        player_fine_y_ += TOWN_TILE_H;
        player_tile_y_--;
    }

    // Occupancy was already updated at step start (loc_1DFE1, 0xFF); the crossing
    // itself only needs the event check below. This keeps the destination blocked for
    // the whole step instead of only from the boundary onward.
    const bool crossed_tile = (prev_tile_x != player_tile_x_ || prev_tile_y != player_tile_y_);

    // sub_1A785 + sub_20EB0: right after sub_1EBC2 every direction handler (loc_1D2C3,
    // loc_1D487, loc_1D642, loc_1D82E, fast and slow) calls sub_1A785 then sub_20EB0.
    // sub_1A785 returns layer_event_[(player_tile_y_ - 1)*width + player_tile_x_], the row
    // where the character's head is. A value of 0 (nothing) or 0xFF (spawn marker, ignored
    // when walking) does nothing; any other value is a town id: value - 1. sub_20EB0 stores
    // the current position in word_56C8D/word_56E89 for the next re-entry, frees the seven
    // layer buffers and calls sub_213A6(town_id) to load the new town in place, so walking
    // through a door or gate switches towns without a click. load_town_and_tec does the same
    // free, reload and spawn/camera setup, so it is called directly.
    if (crossed_tile && layer_event_) {
        int head_row = player_tile_y_ - 1;
        if (head_row >= 0 && head_row < (int)map_h_ &&
            player_tile_x_ >= 0 && player_tile_x_ < (int)map_w_) {
            uint8_t ev = layer_event_[head_row * (int)map_w_ + player_tile_x_];
            // loc_1D7BD calls sub_1A785 twice when byte_544CE completes a tile (4/8 steps).
            // The second call re-reads the same event value and tests it for 0xFE (the town
            // exit marker); if it matches, the result is always the EXIT transition
            // (sub_18FD0), whatever the first, general town-warp attempt did. 0xFE - 1 = 253
            // is never a real town, so the port goes straight to EXIT.
            if (ev == 0xFE) {
                is_player_moving_ = false;
                player_path_.clear();
                player_move_step_ = 0;
                player_fine_x_ = 0;
                player_fine_y_ = 0;
                player_anim_counter_ = 0;
                pending_interact_x_ = pending_interact_y_ = -1;
                // sub_20EB0(0xFD): save the current position for the town being left (Y - 1
                // when moving down), return without loading, then exit through
                // sub_18FD0(unk_5005E = empty string).
                if (town_id_ >= 0 && town_id_ < MAX_TOWNS) {
                    saved_tile_x_[town_id_] = player_tile_x_;
                    saved_tile_y_[town_id_] = player_tile_y_;
                    if (cur_dir == 0x50) saved_tile_y_[town_id_]--;
                }
                do_transition("");
                return;
            }
            if (ev != 0 && ev != 0xFF) {
                int new_town_id = (int)ev - 1;

                // sub_20EB0 stores the current position of the town being left in
                // word_56C8D[town_id_] / word_56E89[town_id_], decreasing the stored Y by 1
                // when moving down ("cmp byte_82949,50h; dec word_56E89[eax]").
                // sub_213A6 restores it when the town is entered again.
                if (town_id_ >= 0 && town_id_ < MAX_TOWNS) {
                    saved_tile_x_[town_id_] = player_tile_x_;
                    saved_tile_y_[town_id_] = player_tile_y_;
                    if (cur_dir == 0x50) { // DOWN
                        saved_tile_y_[town_id_]--;
                    }
                }

                is_player_moving_ = false;
                player_path_.clear();
                player_move_step_ = 0;
                player_fine_x_ = 0;
                player_fine_y_ = 0;
                player_anim_counter_ = 0;
                pending_interact_x_ = pending_interact_y_ = -1;
                if (load_town_and_tec(new_town_id)) {
                    // Same as steps 7b/7c in run(): reload citizen sprites for the town's
                    // parity and replace the chest top tiles. Item, itembox and frame
                    // overlays do not depend on the town and are kept.
                    if (town_id_ % 2 == 0) {
                        load_citizen_sprites(cstldata_path("citizen1.spr"));
                    } else {
                        load_citizen_sprites(cstldata_path("citizen2.spr"));
                    }
                    place_chest_top_tiles();
                }
                return; // layers, player and camera were all reset for the new town
            }
        }
    }

    player_move_step_++;
    update_camera(); // sub_2117F + sub_21E29

    // loc_1D4C4 and the other handlers use "mov al,byte_544CE / inc byte_544CE / cmp al,4
    // (or 8)", so completion is tested on the pre-increment value. player_move_step_ was
    // already incremented above, which makes that "post-increment > move_steps_per_tile_".
    if (player_move_step_ > move_steps_per_tile_) {
        player_move_step_ = 0;
        player_fine_x_ = 0;
        player_fine_y_ = 0;
        player_anim_counter_ = 0;
        update_camera();

        // sub_1D12F never invokes the click handler when a tile completes (byte_544CE
        // reaching 4/8). Chest and building interaction only comes from a mouse click, and
        // the crossed_tile block above already handles door and gate transitions, so no
        // interaction is re-evaluated here.
    }
}

// ---- sub_1EAD0: set or clear an NPC's layer_npc_ occupancy ----
void Pack::set_npc_tile_occupancy(int npc_idx, int tx, int ty, bool occupy) {
    if (!layer_npc_) return;
    if (tx >= 0 && tx < (int)map_w_ && ty >= 0 && ty < (int)map_h_) {
        int idx = ty * (int)map_w_ + tx;
        if (occupy) {
            layer_npc_[idx] = (uint8_t)(npc_idx + 2);
        } else if (layer_npc_[idx] == (uint8_t)(npc_idx + 2)) {
            layer_npc_[idx] = 0;
        }
    }
}

// Per-direction candidate tables, extracted from 0x543CE..0x54419 in Object 2 (reloc base
// 0x50000, page table index 51) through the LE page table. Each table holds five 4-byte
// records and the direction value sits at +2 of each record (up: 00 00 48 00 | 00 00 4B 00
// | ...). The fifth entry is always 0x00 (IDLE): the terminator that stops the NPC when
// every candidate is blocked.
// The restart-from-idle table read at loc_1F56A (unk_543E2 + 20 = 0x543F6, when
// rand(0..9) > 4) is the same memory as kRightCandidates, so the original reuses the right
// table rather than defining another one.
static const uint8_t kUpCandidates[5]    = { 0x48, 0x4B, 0x4D, 0x50, 0x00 }; // unk_543CE
static const uint8_t kLeftCandidates[5]  = { 0x4B, 0x48, 0x50, 0x4D, 0x00 }; // unk_543E2
static const uint8_t kRightCandidates[5] = { 0x4D, 0x48, 0x50, 0x4B, 0x00 }; // unk_543F6 (also reused for restarting from idle)
static const uint8_t kDownCandidates[5]  = { 0x50, 0x4D, 0x4B, 0x48, 0x00 }; // unk_5440A

// ---- on_npc_tile_completed: NPC arrival, wander and patrol AI (sub_1EC6E) ----
// sub_1EC6E..loc_1F6DC runs every time an NPC crosses one tile (16 px vertically, 24 px
// horizontally), not once per move_speed tiles, and resets unk_54420 (= unk_5441E, the same
// array) to 0 on every call. move_speed is only used for the spawn-radius test below, so
// update_npc_movement must never scale the call interval by it.
void Pack::on_npc_tile_completed(int i) {
    PackNPC& npc = npcs_[i];
    if (npc.move_speed == 0) return;

    // 1. Update the tile coordinate from fine_offset (normally exactly one tile, divided
    //    here in case a large per-frame step crosses several).
    if (npc.direction == 0x48) { // UP
        int steps = npc_fine_offset_[i] / TOWN_TILE_H;
        npc.tile_y -= (uint16_t)(steps ? steps : 1);
    } else if (npc.direction == 0x50) { // DOWN
        int steps = npc_fine_offset_[i] / TOWN_TILE_H;
        npc.tile_y += (uint16_t)(steps ? steps : 1);
    } else if (npc.direction == 0x4B) { // LEFT
        int steps = npc_fine_offset_[i] / TOWN_TILE_W;
        npc.tile_x -= (uint16_t)(steps ? steps : 1);
    } else if (npc.direction == 0x4D) { // RIGHT
        int steps = npc_fine_offset_[i] / TOWN_TILE_W;
        npc.tile_x += (uint16_t)(steps ? steps : 1);
    }

    npc_fine_offset_[i] = 0; // unk_54420[idx] = 0 (loc_1F6CD, shared by every branch)

    uint8_t old_dir = npc.direction;
    uint8_t new_dir = 0;

    auto wander_or_stop = [&](int dist_from_target, const uint8_t* candidates) {
        if (dist_from_target >= (int)npc.move_speed) return (uint8_t)0xFF; // turn-around signal
        int r = rand() % ((int)npc.move_speed * 2); // sub_2B81D: rand_range(0, move_speed*2)
        if (r == 1) return (uint8_t)0; // r == 1 means stop (idle)
        int start_k = (r == 0) ? 1 : 0;
        for (int k = start_k; k < 5; k++) {
            // With the terminator candidates[4] == 0, sub_1AA46 tests the current tile rather
            // than a neighbour and therefore always passes, returning 0. is_move_blocked
            // treats dir 0 as blocked, so accept the 0 directly to mirror that branch.
            if (candidates[k] == 0) return (uint8_t)0;
            if (!is_move_blocked(npc.tile_x, npc.tile_y, candidates[k])) return candidates[k];
        }
        return (uint8_t)0;
    };

    if (old_dir == 0x48) { // UP
        int dist = (int)npc.target_y - (int)npc.tile_y;
        uint8_t w = wander_or_stop(dist, kUpCandidates);
        new_dir = (w == 0xFF) ? (!is_move_blocked(npc.tile_x, npc.tile_y, 0x50) ? 0x50 : 0) : w;
    } else if (old_dir == 0x50) { // DOWN
        int dist = (int)npc.tile_y - (int)npc.target_y;
        uint8_t w = wander_or_stop(dist, kDownCandidates);
        new_dir = (w == 0xFF) ? (!is_move_blocked(npc.tile_x, npc.tile_y, 0x48) ? 0x48 : 0) : w;
    } else if (old_dir == 0x4B) { // LEFT
        int dist = (int)npc.target_x - (int)npc.tile_x;
        uint8_t w = wander_or_stop(dist, kLeftCandidates);
        new_dir = (w == 0xFF) ? (!is_move_blocked(npc.tile_x, npc.tile_y, 0x4D) ? 0x4D : 0) : w;
    } else if (old_dir == 0x4D) { // RIGHT
        int dist = (int)npc.tile_x - (int)npc.target_x;
        uint8_t w = wander_or_stop(dist, kRightCandidates);
        new_dir = (w == 0xFF) ? (!is_move_blocked(npc.tile_x, npc.tile_y, 0x4B) ? 0x4B : 0) : w;
    }

    npc.direction = new_dir;

    // 3. Reserve the destination tile for the new direction (sub_1EAD0).
    int nx = npc.tile_x, ny = npc.tile_y;
    if (new_dir == 0x48) ny--;
    else if (new_dir == 0x50) ny++;
    else if (new_dir == 0x4B) nx--;
    else if (new_dir == 0x4D) nx++;
    set_npc_tile_occupancy(i, nx, ny, true);
}

// ---- update_npc_movement: per-frame NPC movement, animation and idle (sub_1F6E4) ----
void Pack::update_npc_movement() {
    for (int i = 0; i < MAX_TOWN_NPCS; i++) {
        PackNPC& npc = npcs_[i];
        if (npc.active != 1) continue;
        if (npc.move_speed == 0) continue; // move_speed 0 means the NPC never moves

        // 1. Sub-pixel accumulator: add move_type and convert every 10 units to a pixel
        //    (dword_544CA).
        npc_subpixel_[i] += npc.move_type;
        int step_delta = npc_subpixel_[i] / 10;
        if (step_delta > 0) {
            npc_fine_offset_[i] = (uint8_t)(npc_fine_offset_[i] + step_delta);
            npc_subpixel_[i] = 0;
        }

        if (npc.direction != 0) {
            // ---- moving ----
            // Arrival is a full tile and unrelated to move_speed. The original's
            // "fine_offset >= move_speed*16/24" test is dead code for move_speed > 1 because
            // fine_offset is reset to 0 each time; the real trigger is the preceding
            // "fine_offset % 16 (or 24) == 0" test.
            bool vertical = (npc.direction == 0x48 || npc.direction == 0x50);
            int tile_size = vertical ? TOWN_TILE_H : TOWN_TILE_W;
            int half_dist = vertical ? 8 : 12;

            if (npc_fine_offset_[i] >= half_dist) {
                set_npc_tile_occupancy(i, npc.tile_x, npc.tile_y, false);
            } else {
                set_npc_tile_occupancy(i, npc.tile_x, npc.tile_y, true);
            }

            int anim_div = vertical ? 4 : 6;
            npc_anim_frame_[i] = (uint8_t)((npc_fine_offset_[i] / anim_div) % 4);

            if (npc_fine_offset_[i] >= tile_size) {
                on_npc_tile_completed(i); // resets npc_fine_offset_[i] to 0
            }
        } else {
            // ---- idle ---- the 16 here is a fixed tile height, unrelated to move_speed. ----
            set_npc_tile_occupancy(i, npc.tile_x, npc.tile_y, true);
            npc_anim_frame_[i] = 0;

            if (npc_fine_offset_[i] >= TOWN_TILE_H) {
                npc_fine_offset_[i] = 0;
                int r = rand() % 10; // sub_2B81D: rand_range(0,10)
                if (r > 4) { // restart is attempted half the time
                    // The original reuses kRightCandidates here (unk_543E2 + 20 == unk_543F6).
                    uint8_t cand_dir = kRightCandidates[r - 5];
                    if (cand_dir != 0 && !is_move_blocked(npc.tile_x, npc.tile_y, cand_dir)) {
                        npc.direction = cand_dir;
                        int nx = npc.tile_x, ny = npc.tile_y;
                        if (cand_dir == 0x48) ny--;
                        else if (cand_dir == 0x50) ny++;
                        else if (cand_dir == 0x4B) nx--;
                        else if (cand_dir == 0x4D) nx++;
                        set_npc_tile_occupancy(i, nx, ny, true);
                    }
                }
            }
        }
    }
}

void Pack::check_town_events() {
    // Entry-point wrapper for the npc_trigger-less call. sub_190A3 only calls sub_2D038 when
    // cam_fine == 0, but the port checks every frame so an entry event can fire immediately.
    check_and_fire_town_events(npc_trigger_, false);
}

// ---- sub_2D038 (PACK_code.ASM 36886..37114) ----
// unk_7FF9C[town*40 + idx] = g_event_flags[10000 + town*40 + idx]: the fired-event table.
// The original indexes the array unconditionally (dl = 5*town, heat at +idx*8), so towns
// whose slot falls past the 0x4E20 save region still fire once and then overwrite the
// adjacent byte. The port keeps that one-shot behaviour with a non-persistent overflow
// array instead of suppressing the firing outright.
uint8_t& Pack::fired_flag(int town, int idx) {
    static uint8_t dummy = 1;
    if (town < 0 || idx < 0 || idx >= 40) { dummy = 1; return dummy; }
    const int p = 10000 + town * 40 + idx;
    if (p >= 20000) {
        static uint8_t overflow[256 * 40] = {};
        const int o = town * 40 + idx;
        if (o >= 0 && o < (int)sizeof(overflow)) return overflow[o];
        dummy = 1;
        return dummy;
    }
    return g_event_flags[p];
}

bool Pack::check_and_fire_town_events(uint32_t npc_trigger, bool is_entry) {
    bool fired_any = false;
    bool trace = (dbg_env("G2_PACK_TRACE"));
    // G2_PACK_FORCE_FLAG=200:1,10880:1 injects flags for isolated tests. The value is
    // parsed, so merely setting the variable does nothing.
    const char* force = dbg_getenv("G2_PACK_FORCE_FLAG");
    if (force && force[0]) {
        // Parse "200:1,10880:1"; malformed tokens are ignored.
        std::string s = force;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t colon = tok.find(':');
            if (colon != std::string::npos) {
                int idx = atoi(tok.substr(0, colon).c_str());
                int val = atoi(tok.substr(colon + 1).c_str());
                if (idx >= 0 && idx < 20000) {
                    g_event_flags[idx] = (uint8_t)val;
                    if (trace) fprintf(stderr, "PACK: force flag[%d]=%d\n", idx, val);
                }
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        // The variable is not unset, so the same values are reapplied on every call, which
        // is harmless but means the log line repeats.
    }

    for (int i = 0; i < MAX_TOWN_EVENTS; i++) {
        const PackEvent& e = events_[i];
        if (e.trigger_x == 0 && e.trigger_y == 0 && e.flag_idx1 == 0 && e.flag_idx2 == 0 &&
            e.flag_idx3 == 0 && e.state_check == 0 && e.event_id == 0 && e.npc_id_trigger == 0)
            continue;

        int need = 0, got = 0;
        bool has_pos = (e.trigger_x != 0 || e.trigger_y != 0);
        bool has_state = (e.state_check != 0);
        bool has_f1 = (e.flag_idx1 != 0 && e.flag_val1 != 0);
        bool has_f2 = (e.flag_idx2 != 0 && e.flag_val2 != 0);
        bool has_f3 = (e.flag_idx3 != 0 && e.flag_val3 != 0);
        bool has_npc = (e.npc_id_trigger != 0);
        if (has_pos) need++;
        if (has_state) need++;
        if (has_f1) need++;
        if (has_f2) need++;
        if (has_f3) need++;
        if (has_npc) need++;

        if (has_pos) {
            // 36959..36974: x == player.x and y + 1 == player.y
            if ((int)e.trigger_x == (int)player_tile_x_ &&
                (int)e.trigger_y + 1 == (int)player_tile_y_)
                got++;
        }
        if (has_state) {
            if (e.state_check == story_hi_) got++;
        }
        if (has_f1) {
            if (e.flag_idx1 < 20000 && (int)g_event_flags[e.flag_idx1] == (int)e.flag_val1) got++;
        }
        if (has_f2) {
            if (e.flag_idx2 < 20000 && (int)g_event_flags[e.flag_idx2] == (int)e.flag_val2) got++;
        }
        if (has_f3) {
            if (e.flag_idx3 < 20000 && (int)g_event_flags[e.flag_idx3] == (int)e.flag_val3) got++;
        }
        if (has_npc) {
            if ((uint32_t)e.npc_id_trigger == npc_trigger) got++;
        }

        if (trace) {
            fprintf(stderr,
                    "PACK: ev%d trig=(%u,%u) pos=(%d,%d) need=%d got=%d "
                    "f1=%u/%u(have=%u) f2=%u/%u(have=%u) f3=%u/%u(have=%u) "
                    "state=%u(hi=%u) npc=%u(trig=%u) eid=%u town=%d done=%d%s\n",
                    i, e.trigger_x, e.trigger_y, player_tile_x_, player_tile_y_,
                    need, got,
                    e.flag_idx1, e.flag_val1, e.flag_idx1 < 20000 ? g_event_flags[e.flag_idx1] : 255,
                    e.flag_idx2, e.flag_val2, e.flag_idx2 < 20000 ? g_event_flags[e.flag_idx2] : 255,
                    e.flag_idx3, e.flag_val3, e.flag_idx3 < 20000 ? g_event_flags[e.flag_idx3] : 255,
                    e.state_check, story_hi_, e.npc_id_trigger, npc_trigger,
                    e.event_id, town_id_,
                    (int)fired_flag(town_id_, i),
                    is_entry ? " [entry]" : "");
        }

        if (need > 0 && got == need) {
            if (fired_flag(town_id_, i)) {
                if (trace) fprintf(stderr, "PACK: ev%d already fired in town %d, skip\n", i, town_id_);
                continue;
            }
            fprintf(stderr,
                    "PACK: FIRING ev%d eid=%u at player=(%d,%d) town=%d flags200=%u%s\n",
                    i, e.event_id, player_tile_x_, player_tile_y_, town_id_,
                    g_event_flags[200],
                    is_entry ? " [entry]" : "");
            fired_flag(town_id_, i) = 1;   // unk_7FF9C[town*40+idx]; saved in blackbox
            // sub_2D038 loc_2D239: fade out before firing (DAC to black, 29 steps, 2 retraces).
            // Entry firings happen before the first present and are invisible, so they skip
            // it. The palette snapshot is restored immediately, matching the sub_20118
            // restore on the first EVE frame; the post-fire loop at 2D28F is net-zero within
            // one town and is omitted.
            uint8_t pre_pal[256][3];
            for (int pi = 0; pi < 256; pi++)
                palette_get_entry((uint8_t)pi, pre_pal[pi][0], pre_pal[pi][1], pre_pal[pi][2]);
            if (!is_entry) {
                fade_out_to_black_rendered(2);
                fprintf(stderr, "PACK: event fade-out done (29 steps)%s\n",
                        dbg_env("G2_PACK_AUTO") ? " [auto]" : "");
            }
            for (int pi = 0; pi < 256; pi++)
                palette_set_entry((uint8_t)pi, pre_pal[pi][0], pre_pal[pi][1], pre_pal[pi][2]);
            // sub_2D3B8(evid): event BGM (52-case table, gated on byte_82731).
            if (game_cfg_[1] != 0) {
                const int track = bgm_track_for_event((int)e.event_id);
                fprintf(stderr, "PACK: sub_2D3B8 eid=%u -> track=0x%X\n", e.event_id, track);
                bgm_play_track(track);
            }
            // sub_13B8D(evid): run the EVE script.
            if (!run_eve_script(e.event_id)) {
                fprintf(stderr, "PACK: warning: run_eve_script(%u) failed or missing, continue\n", e.event_id);
            }
            // loc_2D28F fade tail, BGM restore and byte_54041: the original fades again
            // after the cutscene, restores the town track when byte_82731 == 1 and sets
            // byte_54041 = 1 (palette reapply on the next sub_190A3 tail). The port's EVE
            // loop already restores the palette via sub_20118 on the first frame, so the
            // extra fade is net-zero within one town; the track restore and the flag are
            // kept for parity.
            if (game_cfg_[1] == 1) bgm_play_track(bgm_town_track_);
            palette_reapply_pending_ = true;
            // loc_2D2D1: the loop continues after firing (inc edi), so one call can fire
            // several events.
            fired_any = true;
            if (should_exit_) return true;
        }
    }
    return fired_any;
}

bool Pack::load_eve_file(int event_id, EveFile& out) {
    out = EveFile();
    char name[64];
    snprintf(name, sizeof(name), "%d.eve", event_id);
    std::string path = cstldata_path(name);
    if (!file_exists(path.c_str())) {
        // Upper-case variant; cstldata_path is already case-insensitive.
        snprintf(name, sizeof(name), "%d.EVE", event_id);
        path = cstldata_path(name);
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "PACK: warning: EVE open failed: %s (eid=%d)\n", path.c_str(), event_id);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    if (sz > 0 && fread(buf.data(), 1, sz, fp) != (size_t)sz) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    if (sz < 2) return false;
    size_t off = 0;
    auto rd_u16 = [&](size_t o) -> uint16_t {
        return (uint16_t)(buf[o] | (buf[o + 1] << 8));
    };
    out.total = rd_u16(0); off = 2;
    for (int si = 0; si < 51; si++) {
        if (off + 4 > buf.size()) {
            fprintf(stderr, "PACK: EVE %d truncated at actor %d\n", event_id, si);
            return false;
        }
        uint16_t ln = rd_u16(off), ln2 = rd_u16(off + 2); off += 4;
        out.actors[si].len = ln;
        out.actors[si].words.clear();
        if (ln > 0) {
            if (off + (size_t)ln * 2 > buf.size()) {
                fprintf(stderr, "PACK: EVE %d actor %d body OOB (ln=%u)\n", event_id, si, ln);
                return false;
            }
            out.actors[si].words.reserve(ln);
            for (int k = 0; k < ln; k++) out.actors[si].words.push_back(rd_u16(off + k * 2));
            off += (size_t)ln * 2;
        }
        (void)ln2;
    }
    if (off != buf.size()) {
        fprintf(stderr, "PACK: EVE %d leftover %zu bytes (off=%zu sz=%ld)\n",
                event_id, buf.size() - off, off, sz);
    }
    out.loaded = true;
    fprintf(stderr, "PACK: loaded EVE %d total=%u size=%ld\n", event_id, out.total, sz);
    return true;
}

void Pack::eve_clear_actors() {
    for (int i = 0; i < MAX_EVE_ACTORS; i++) eve_actors_[i] = EveActorState();
    for (int i = 0; i < 320; i++) eve_slots_[i] = EveSlot();
}

// ---- op13: spawn an actor (loc_12BE2, 27 words) ----
// Layout: [fire, 13, spr_idx, p1, p2, p3, name20 (one byte per word), portrait_id].
//   p1 = tile X ([eax+6] -> word_7D022)
//   p2 = tile Y ([eax+8] -> word_7D024, decremented)
//   p3 = initial frame ([eax+0Ah] -> unk_82936; every script 271 uses 0)
//   spr_idx = off_53460 index ([eax+4], via the dword high word, sub_1A2D6/sub_1A51D)
//   portrait_id = last word ([ebp+ecx*2+0Ch] -> unk_7D040 -> sub_2708B -> sub_24BA4)
// Field order was confirmed against cap4.avi and PORTRAIT.SYF renders: treating p0 as the
// portrait leaves 136/213 out of range (85+ portraits are skipped) and maps the sprite slot
// (75/45/76...) to wrong NC2 files. With the order above, sprite ids 16=SCOUT, 136=S_GS,
// 213=SIREN, 29=GUARD, 107=SAVANT1, 84=nullman and portraits 75/45/76/77/8 match the video.
// Every EVE p0 in 0..220 (88 values) fits the 222 off_53460 entries and w26 in 0..84 fits
// the 85 PORTRAIT.SYF entries.
// base is the index of the command inside words; w[base+0] = fire and w[base+1] = 13.
void Pack::eve_op_spawn(int actor, const std::vector<uint16_t>& w, size_t base) {
    if (actor < 0 || actor >= MAX_EVE_ACTORS) return;
    if (base + 27 > w.size()) {
        fprintf(stderr, "PACK: EVE actor%d op13 truncated (base=%zu size=%zu)\n",
                actor, base, w.size());
        return;
    }
    EveActorState& st = eve_actors_[actor];
    // [eax+4] = spr_idx (dword high word -> off_53460), [eax+6] = tile X,
    // [eax+8] = tile Y (decremented), [eax+0Ah] = initial frame, last word = portrait.
    int sprite_idx = w[base + 2] & 0xFFFF;
    int p1 = w[base + 3], p2 = w[base + 4];
    int portrait_id = w[base + 26] & 0xFFFF;
    st.active = true;
    st.tile_x = p1;
    st.tile_y = p2 - 1; // loc_12BE2 "dec ecx": the original stores p2 - 1 in word_7D024
    st.portrait_id = portrait_id;
    // loc_12BE2: entry frame (unk_82936) = word 5 ([+0xA]); direction folds in as
    // (frame / 11) & 3.
    st.frame_idx = (int)(w[base + 5] & 0xFFFF);
    st.dir = (st.frame_idx / 11) & 3;
    st.move_op = 0; // op13 sets unk_8293C = 1 and resets counter/fine (end of loc_12BE2)
    st.move_counter = 0;
    st.move_speed = 1;
    st.fine_x = 0;
    st.fine_y = 0;
    st.name.clear();
    for (int k = 0; k < 20; k++) {
        char c = (char)(w[base + 6 + k] & 0xFF);
        if (c == '\0') break;
        st.name.push_back(c);
    }
    st.sprite_idx = sprite_idx;
    st.sprite_file.clear();
    st.sprite_loaded = false;
    if (sprite_idx >= 0 && sprite_idx < kEveSpriteFileCount) {
        st.sprite_file = kEveSpriteFiles[sprite_idx];
        std::string path = characte_path(st.sprite_file.c_str());
        if (file_exists(path.c_str())) {
            // Every CHARACTE/*.NC2 is class 0 with 44 records (measured), so type_code = 0.
            if (st.sprite.load(path, 0)) {
                st.sprite_loaded = true;
            } else {
                fprintf(stderr, "PACK: warning: EVE actor%d NC2 load failed: %s (%s)\n",
                        actor, path.c_str(), st.sprite.error().c_str());
            }
        } else {
            fprintf(stderr, "PACK: warning: EVE actor%d NC2 not found: %s (idx=%d)\n",
                    actor, st.sprite_file.c_str(), sprite_idx);
        }
    } else {
        fprintf(stderr, "PACK: EVE actor%d op13 bad sprite_idx=%d\n", actor, sprite_idx);
    }
    fprintf(stderr, "PACK: EVE actor%d op13 spawn tile=(%d,%d) portrait=%d name_len=%zu sprite=%s%s\n",
            actor, st.tile_x, st.tile_y, st.portrait_id, st.name.size(),
            st.sprite_file.empty() ? "(none)" : st.sprite_file.c_str(),
            st.sprite_loaded ? "" : " (no gfx)");
}

void Pack::eve_op_hide(int actor) {
    if (actor < 0 || actor >= MAX_EVE_ACTORS) return;
    eve_actors_[actor].active = false;
    fprintf(stderr, "PACK: EVE actor%d op14 hide\n", actor);
}

void Pack::eve_op_set_dir(int actor, int dir_byte) {
    if (actor < 0 || actor >= MAX_EVE_ACTORS) return;
    eve_actors_[actor].dir = pack_dir_byte_to_nc2(dir_byte);
    fprintf(stderr, "PACK: EVE actor%d op10 dir=0x%X -> nc2dir=%d\n",
            actor, dir_byte, eve_actors_[actor].dir);
}

// loc_12DC7: entry frame (unk_82936) = p0. NC2 sprites are laid out as direction (0..3) x
// pose (0..10), so direction folds as (frame / 11) & 3.
void Pack::eve_op_set_frame(int actor, int frame_idx) {
    if (actor < 0 || actor >= MAX_EVE_ACTORS) return;
    if (frame_idx < 0) frame_idx = 0;
    eve_actors_[actor].frame_idx = frame_idx;
    eve_actors_[actor].dir = (frame_idx / 11) & 3;
}

// ---- op1-8: move an actor (loc_12956..12A79 phase 1, loc_1336C..13AB2 phase 2) ----
// op1/5 decrement word_7D024 (up, -Y), op2/6 increment it (down, +Y),
// op3/7 decrement word_7D022 (left, -X), op4/8 increment it (right, +X).
// Phase 1 sets counter = (Y ? 0x10 : 0x18) / speed * steps (24 px per X tile, 16 px per Y
// tile; speed = unk_8293C, initialised to 1 by op13).
// Phase 2 (every EVE frame, eve_animate_moves_once) adds speed to the screen position and
// picks walk frames from (counter >> 2) % 4 for op1-4:
//   op1 [25,24,25,26], op2 [3,2,3,4], op3 [14,13,14,15], op4 [36,35,36,37];
//   op5-8 keep the frame. Tiles update when counter % interval == 0 (non-zero) and at 0.
// Every active actor animates in parallel at 1 px per frame, matching the original.
// G2_PACK_AUTO teleports instead of animating so offscreen tests keep their speed.
void Pack::eve_op_move(int actor, int op, int steps) {
    if (actor < 0 || actor >= MAX_EVE_ACTORS) return;
    EveActorState& st = eve_actors_[actor];
    if (!st.active) {
        fprintf(stderr, "PACK: EVE actor%d op%d move steps=%d ignored (inactive)\n",
                actor, op, steps);
        return;
    }
    if (op < 1 || op > 8) return;
    if (steps < 0) steps = 0;
    int ox = st.tile_x, oy = st.tile_y;
    bool auto_fast = (dbg_env("G2_PACK_AUTO"));
    if (auto_fast) {
        // Test fast path: apply the final tile. Only op1-4 update direction and frame;
        // op5-8 keep both (phase 2 at loc_1390D/13970/139DB/13A54 never writes unk_82936).
        // Dialogue placement only reads the tile, so the result is equivalent.
        int dx = 0, dy = 0;
        switch (op) {
        case 1: case 5: dy = -steps; break;
        case 2: case 6: dy = +steps; break;
        case 3: case 7: dx = -steps; break;
        case 4: case 8: dx = +steps; break;
        default: break;
        }
        st.tile_x += dx;
        st.tile_y += dy;
        if (op >= 1 && op <= 4) {
            int dir = st.dir, frame = st.frame_idx;
            switch (op) {
            case 1: dir = 2; frame = 25; break;
            case 2: dir = 0; frame = 3;  break;
            case 3: dir = 1; frame = 14; break;
            case 4: dir = 3; frame = 36; break;
            default: break;
            }
            st.dir = dir;
            st.frame_idx = frame;
        }
        st.move_op = 0;
        st.move_counter = 0;
        st.fine_x = 0;
        st.fine_y = 0;
        fprintf(stderr, "PACK: EVE actor%d op%d move steps=%d (%d,%d)->(%d,%d) dir=%d frame=%d [auto]\n",
                actor, op, steps, ox, oy, st.tile_x, st.tile_y, st.dir, st.frame_idx);
        return;
    }
    // Phase 1 only sets the counter; the frame loop animates in parallel. No step is taken
    // here so the first pixel moves on the next frame, as in loc_12A2B -> loc_1336C (a
    // one-frame difference), which is imperceptible at 60 fps.
    bool is_y = (op == 1 || op == 2 || op == 5 || op == 6);
    int unit = is_y ? TOWN_TILE_H : TOWN_TILE_W; // 16 or 24
    int speed = st.move_speed > 0 ? st.move_speed : 1;
    if (speed < 1) speed = 1;
    st.move_op = op;
    // loc_12A1C: the counter lives in a byte (unk_7D7EF), so values past 255 wrap to
    // the low 8 bits (e.g. speed 1 X: 11+ tiles, Y: 16+ tiles).
    st.move_counter = ((unit / speed) * steps) & 0xFF;
    // Op1-4 fix the direction; the frame enters its cycle on the first animation step.
    // op5-8 keep both (phase 2 at loc_1390D/13970/139DB/13A54 never writes unk_82936).
    if (op >= 1 && op <= 4) {
        if (op == 1) st.dir = 2;
        else if (op == 2) st.dir = 0;
        else if (op == 3) st.dir = 1;
        else if (op == 4) st.dir = 3;
    }
    fprintf(stderr, "PACK: EVE actor%d op%d move start steps=%d counter=%d (%d,%d) dir=%d\n",
            actor, op, steps, st.move_counter, ox, oy, st.dir);
}

// Phase 2, one step per EVE frame for every moving actor (loc_1336C..13AB2):
// screen += speed -> frame from (counter>>2)%4 for op1-4 -> counter -= 1 -> tile update and
// fine wrap when counter % interval == 0 (non-zero) -> final tile and idle at counter == 0.
bool Pack::eve_animate_moves_once() {
    bool any = false;
    for (int a = 0; a < MAX_EVE_ACTORS; a++) {
        EveActorState& st = eve_actors_[a];
        if (!st.active || st.move_op < 1 || st.move_op > 8 || st.move_counter <= 0) continue;
        any = true;
        int op = st.move_op;
        int speed = st.move_speed > 0 ? st.move_speed : 1;
        if (speed < 1) speed = 1;
        bool is_y = (op == 1 || op == 2 || op == 5 || op == 6);
        int interval = (is_y ? TOWN_TILE_H : TOWN_TILE_W) / speed;
        if (interval < 1) interval = 1;
        int dx = 0, dy = 0;
        switch (op) {
        case 1: case 5: dy = -1; break;
        case 2: case 6: dy = +1; break;
        case 3: case 7: dx = -1; break;
        case 4: case 8: dx = +1; break;
        default: break;
        }
        // Screen movement (fine offset accumulation).
        st.fine_x += dx * speed;
        st.fine_y += dy * speed;
        // Walk frame, op1-4 only, from the counter value before the decrement.
        if (op >= 1 && op <= 4) {
            int idx = ((st.move_counter >> 2) % 4 + 4) % 4;
            static const int kWalk[4][4] = {
                {25, 24, 25, 26}, // op1 up
                {3, 2, 3, 4},     // op2 down
                {14, 13, 14, 15}, // op3 left
                {36, 35, 36, 37}, // op4 right
            };
            int frame = kWalk[op - 1][idx];
            st.frame_idx = frame;
            st.dir = frame / 11;
        }
        // Decrement the counter.
        st.move_counter -= 1;
        if (st.move_counter < 0) st.move_counter = 0;
        // Tile update, intermediate and final.
        if (st.move_counter % interval == 0 && st.move_counter != 0) {
            st.tile_x += dx;
            st.tile_y += dy;
            // Wrap fine so the screen position stays continuous.
            if (dx != 0) st.fine_x += -dx * (is_y ? TOWN_TILE_H : TOWN_TILE_W);
            if (dy != 0) st.fine_y += -dy * (is_y ? TOWN_TILE_H : TOWN_TILE_W);
        }
        if (st.move_counter == 0) {
            st.tile_x += dx;
            st.tile_y += dy;
            if (dx != 0) st.fine_x = 0;
            if (dy != 0) st.fine_y = 0;
            st.move_op = 0;
        }
    }
    return any;
}

// ---- op15: absolute camera (loc_12D97, then sub_126AC) ----
// loc_12D97 sets the camera tiles to (x=p0, y=p1) with no clamping and leaves fine alone;
// the renderer skips tiles outside the map. sub_126AC then walks 50 actors and recomputes
// unk_82938/unk_8293A from (tile - cam)*size + fine. The port recomputes the same value in
// get_eve_actor_screen_box on every render, so moving actors (type 1..8 with a nonzero
// unk_7D7EF counter, which the original adjusts to keep the screen continuous) stay
// continuous through the shared fine offsets. The walk is kept for parity.
void Pack::eve_op_camera_snap(int tx, int ty) {
    cam_tile_x_ = (int16_t)tx;
    cam_tile_y_ = (int16_t)ty;
    for (int a = 0; a < 50; a++) {
        const EveActorState& st = eve_actors_[a];
        if (!st.active) continue;
        (void)st; // screen position is derived on demand; see above
    }
}

// ---- op23: relative camera pan (loc_13063 phase 1, loc_13AC9 phase 2, sub_21E29 tail) ----
// [fire, 23, count, dx, dy] with dx/dy as pixels per frame. Each EVE frame, phase 2 of
// sub_12857 sets 15BA1A = (-dx, dy) (loc_13AC9) and sub_13B8D calls sub_21E29 to integrate it
// into the camera, Y before X. On the last frame (counter 1 -> 0) 15BA1A is cleared first, so
// the camera integrates count - 1 times; wraps are discarded.
// The original's fine sign is the opposite of the port's, so the original convention is
// integrated in shadow variables and converted back through world-space floor division,
// which keeps the rendered world identical.
// The original's Y+ bounds compare against a label named 15BA1A+2 (-dx) at PACK.LST
// 000220B1, but the X guard pattern suggests a mislabelled 15BA1E (map height), so a height
// guard is used; this stays open until a dy != 0 Y+ pan (script 274) verifies it.
void Pack::eve_op_camera_pan(int count, int dx, int dy) {
    int16_t vx = (int16_t)(dx & 0xFFFF), vy = (int16_t)(dy & 0xFFFF);
    int ox = cam_tile_x_, oy = cam_tile_y_;
    bool auto_fast = (dbg_env("G2_PACK_AUTO"));
    if (count <= 0) {
        fprintf(stderr, "PACK: EVE op23 camera pan count=%d d=(%d,%d) (%d,%d)->(%d,%d) noop\n",
                count, vx, vy, ox, oy, ox, oy);
        return;
    }
    // Shadow copy of the original convention: world = tile*size - fine, derived from the
    // port's world on entry.
    int otx = cam_tile_x_, oty = cam_tile_y_;
    int ofx = -cam_fine_x_, ofy = -cam_fine_y_;
    auto floor_div = [](int a, int b) {
        int q = a / b, r = a - q * b;
        if (r != 0 && ((r < 0) != (b < 0))) q--;
        return q;
    };
    // Integrate one frame (sub_21E29 tail, Y then X). Wrapping is ignored.
    auto integrate_once = [&]() {
        // Y (PACK.LST 0002206B~000220E9)
        {
            int new_fy = ofy - vy;
            if (vy > 0) {
                // 000220AB path; the bound is a height guard (see the note above).
                if (oty + 13 < (int)map_h_ - 1) {
                    ofy = new_fy;
                    if (ofy < -TOWN_TILE_H) { oty++; ofy = 0; }
                }
            } else {
                if (oty >= 0) {
                    ofy = new_fy;
                    if (ofy > TOWN_TILE_H) { oty--; ofy = 0; }
                }
            }
        }
        // X (PACK.LST 000220E9~00022128)
        {
            int step = -vx;
            int new_fx = ofx + step;
            if (step > 0) {
                if (otx >= 0) {
                    ofx = new_fx;
                    if (ofx > TOWN_TILE_W) { otx--; ofx = 0; }
                }
            } else {
                if (otx + 12 < (int)map_w_) {
                    ofx = new_fx;
                    if (ofx < -TOWN_TILE_W) { otx++; ofx = 0; }
                }
            }
        }
    };
    auto publish = [&]() {
        int world_x = otx * TOWN_TILE_W - ofx;
        int world_y = oty * TOWN_TILE_H - ofy;
        int tx = floor_div(world_x, TOWN_TILE_W);
        int ty = floor_div(world_y, TOWN_TILE_H);
        int fx = world_x - tx * TOWN_TILE_W;
        int fy = world_y - ty * TOWN_TILE_H;
        cam_tile_x_ = tx;
        cam_tile_y_ = ty;
        cam_fine_x_ = fx;
        cam_fine_y_ = fy;
        if (cam_tile_x_ < 0) { cam_tile_x_ = 0; cam_fine_x_ = 0; }
        if (cam_tile_y_ < 0) { cam_tile_y_ = 0; cam_fine_y_ = 0; }
        if (map_w_ > 13 && cam_tile_x_ > (int)map_w_ - 13) { cam_tile_x_ = (int)map_w_ - 13; cam_fine_x_ = 0; }
        if (map_h_ > 14 && cam_tile_y_ > (int)map_h_ - 14) { cam_tile_y_ = (int)map_h_ - 14; cam_fine_y_ = 0; }
    };
    if (auto_fast) {
        // Integrate to the final world without rendering, keeping the log and timing.
        for (int f = 0; f < count - 1; f++) {
            if (should_exit_) break;
            integrate_once();
        }
        publish();
        // Publish handles the final clamp; fine is included in the log.
        fprintf(stderr, "PACK: EVE op23 camera pan count=%d vel=(%d,%d) (%d,%d)->(%d,%d) fine=(%d,%d) [auto]\n",
                count, vx, vy, ox, oy, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_);
        return;
    }
    // Rendered path: draw count frames but integrate the camera only on the first count - 1
    // (the last frame clears 15BA1A before sub_21E29 runs, loc_13AC9).
    for (int f = 0; f < count; f++) {
        if (should_exit_) break;
        if (f < count - 1) {
            integrate_once();
            publish();
        }
        video_.clear();
        render_town_frame();
        video_.present();
        pack_maybe_dump_shot(video_);
        GameTimer::delay_with_pump(input_, 16);
    }
    fprintf(stderr, "PACK: EVE op23 camera pan count=%d vel=(%d,%d) (%d,%d)->(%d,%d) fine=(%d,%d) smooth\n",
            count, vx, vy, ox, oy, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_);
}

bool Pack::get_eve_actor_screen_box(int actor, int& out_left, int& out_top,
                                     int& out_w, int& out_h) const {
    if (actor < 0 || actor >= MAX_EVE_ACTORS) return false;
    const EveActorState& st = eve_actors_[actor];
    if (!st.active) return false;
    // loc_12BE2: unk_82938 = (tile - cam)*24 + fine and unk_8293A = (tile - cam)*16 + fine,
    // with no +5/-16 adjustment unlike town sprites. fine is the sub-tile offset from phase 2
    // movement.
    int sx = (st.tile_x - cam_tile_x_) * TOWN_TILE_W - cam_fine_x_ + st.fine_x;
    int sy = (st.tile_y - cam_tile_y_) * TOWN_TILE_H - cam_fine_y_ + st.fine_y;
    const NC2Frame* fr = nullptr;
    if (st.sprite_loaded) fr = st.sprite.frame(st.dir, st.frame_idx % 11);
    if (fr && fr->w > 0 && fr->h > 0) {
        out_left = sx + fr->x_off;
        out_top = sy + fr->y_off;
        out_w = fr->w;
        out_h = fr->h;
        return true;
    }
    // Without a loaded sprite, use a 24x32 default box for placement only.
    out_left = sx - 12;
    out_top = sy - 16;
    out_w = 24;
    out_h = 32;
    return true;
}

// ---- op11: EVE dialogue (loc_12B26 -> sub_2708B) ----
// text_id is a TALK.TBL block ([ebp+..] >> 16) and actor is the EVE slot (0..50,
// ecx = slot + 0x140). Portrait, name and placement come from EveActorState filled by op13
// (low byte of unk_7D040 -> sub_24BA4; sub_24DA5 goes through unk_82938/8293A). Pages are
// drawn by the same talktbl_paginate_npc + run_dialog_pages as NPC dialogue, and the result
// is written to story_hi_ as `mov word ptr dword_1A00C8+2,ax`: 0 without choices, the
// 1-based choice index with choices, unchanged on cancel.
void Pack::eve_op_dialogue(int text_id, int actor) {
    // loc_12B26: the speaker name is the string in script words 6.., and portrait and
    // position come from the actor entry. The original's dword_52688 != 0 skip is not ported.
    std::string speaker;
    int portrait_id = 0;
    const bool have_actor = (actor >= 0 && actor < MAX_EVE_ACTORS && eve_actors_[actor].active);
    if (have_actor) {
        speaker = eve_actors_[actor].name;
        portrait_id = eve_actors_[actor].portrait_id;
    }
    std::string path = game_path("talk.tbl");
    if (!file_exists(path.c_str())) path = game_path("TALK.TBL");
    std::vector<uint8_t> buf;
    int len = 0;
    if (!talktbl_load_buffer(path, text_id, buf, len)) {
        fprintf(stderr, "PACK: warning: EVE dialogue text_id=%d actor=%d load failed\n", text_id, actor);
        return;
    }
    // sub_2708B does not skip an empty body, and neither does this.
    const int sel = run_talk_engine(-1, actor, portrait_id, speaker, buf, len);
    if (sel >= 0) story_hi_ = (uint16_t)sel;   // word 1A00CA = sub_2708B result (choice 1..4, else 0)
}

void Pack::fade_out_to_black_rendered(int retraces_per_step) {
    // sub_208FA: cur = src*1000 and step = src*1000/32 in fixed point, applied 29 times
    // (cur -= step). The callers (loc_213CB, loc_2D239, inn at 1AEFC) repeat while al == 1
    // after a retrace wait: warps and events use 2 x sub_2B206, the inn 1 x sub_2B206.
    // The port has no DAC, so it moves g_palette by the same values and renders plus presents
    // every step (Video::blit_backbuffer reads g_palette at present time), which dims the same
    // image. palette_fade_to_black() does not present and hides the effect, so it is unused.
    static constexpr int kFadeSteps = 29; // 7<<2 + 1 (ecx=2)
    static constexpr int kFadeDiv = 32;   // 8<<2
    if (dbg_env("G2_PACK_AUTO")) {
        fprintf(stderr, "PACK: fade-out skipped [auto]\n");
        return;
    }
    long cur[256][3], step[256][3];
    for (int i = 0; i < 256; i++) {
        uint8_t r = 0, g = 0, b = 0;
        palette_get_entry((uint8_t)i, r, g, b);
        const long v[3] = {r, g, b};
        for (int c = 0; c < 3; c++) {
            cur[i][c] = v[c] * 1000L;
            step[i][c] = v[c] * 1000L / kFadeDiv;
        }
    }
    for (int s = 0; s < kFadeSteps; s++) {
        if (should_exit_) break;
        for (int i = 0; i < 256; i++) {
            long o[3];
            for (int c = 0; c < 3; c++) {
                cur[i][c] -= step[i][c];
                o[c] = cur[i][c] / 1000L;
                if (o[c] < 0) o[c] = 0;
                if (o[c] > 63) o[c] = 63;
            }
            palette_set_entry((uint8_t)i, (uint8_t)o[0], (uint8_t)o[1], (uint8_t)o[2]);
        }
        video_.clear();
        render_town_frame();
        video_.present();
        pack_maybe_dump_shot(video_);
        GameTimer::delay_with_pump(input_, retraces_per_step >= 2 ? 33 : 16);
    }
}

void Pack::eve_fade_out_to_black() {
    fade_out_to_black_rendered(2);
    if (!dbg_env("G2_PACK_AUTO"))
        fprintf(stderr, "PACK: EVE warp fade-out done (29 steps)\n");
}

void Pack::eve_op_warp(int new_town_id, int cam_x, int cam_y) {
    fprintf(stderr, "PACK: EVE warp -> town %d cam=(%d,%d) (sub_20EB0)\n",
            new_town_id, cam_x, cam_y);
    if (town_id_ >= 0 && town_id_ < MAX_TOWNS) {
        saved_tile_x_[town_id_] = player_tile_x_;
        saved_tile_y_[town_id_] = player_tile_y_;
        if (player_dir_ == 0x50) saved_tile_y_[town_id_]--;
    }
    is_player_moving_ = false;
    player_path_.clear();
    player_move_step_ = 0;
    player_fine_x_ = player_fine_y_ = 0;
    player_anim_counter_ = 0;
    pending_interact_x_ = pending_interact_y_ = -1;
    // sub_213A6 entry fade (loc_213CB): darken the current screen before reloading, and
    // restore the palette if the load fails (the original exits fatally instead).
    uint8_t saved_pal[256][3];
    for (int i = 0; i < 256; i++)
        palette_get_entry((uint8_t)i, saved_pal[i][0], saved_pal[i][1], saved_pal[i][2]);
    eve_fade_out_to_black();
    if (load_town_and_tec(new_town_id)) {
        if (town_id_ % 2 == 0) load_citizen_sprites(cstldata_path("citizen1.spr"));
        else load_citizen_sprites(cstldata_path("citizen2.spr"));
        place_chest_top_tiles();
        // Op18 [fire, 18, dst, camX, camY]: right after the sub_20EB0 town reload the same
        // command overwrites the camera from +6/+8 (near loc_12E2E, after the sub_20EB0 call:
        // [ebp+6] -> 56BFA+2, [ebp+8] -> 56BFA). Script 271 warps within town 22, where a plain
        // reload puts the camera on the player and pushes the actors off screen; the original
        // restores (3,2) and keeps the centre, matching cap4.avi. The fine offsets set by
        // load_town_and_tec are reset and the tiles are taken as given, unclamped, as in the
        // original.
        cam_tile_x_ = cam_x;
        cam_tile_y_ = cam_y;
        cam_fine_x_ = 0;
        cam_fine_y_ = 0;
        // The done markers survive the reload (unk_7FF9C is global, [town*40+idx]).
        // EVE cutscene actors also survive a warp: sub_20EB0 only frees the map layers
        // (15B9B4/15B9B0/15B9C8/15B9C4/15B9C0/15B9BC/15B9B8) and never touches
        // unk_82934/7D020/840A6, so no actors are cleared here.
    } else {
        for (int i = 0; i < 256; i++)
            palette_set_entry((uint8_t)i, saved_pal[i][0], saved_pal[i][1], saved_pal[i][2]);
        fprintf(stderr, "PACK: warning: EVE warp failed: town %d load failed\n", new_town_id);
    }
}

bool Pack::run_eve_script(int event_id) {
    EveFile eve;
    if (!load_eve_file(event_id, eve)) return false;
    // Clear the actor slots when a cutscene starts. In the original, unk_82934 starts at 0
    // (set by sub_1A4F9/main) and changes only through op13/op14 inside the script.
    eve_clear_actors();
    // sub_13B8D step 1: save the camera (dword_56BFA/56BF6) and the town id, restored on
    // exit (step 4).
    const int saved_cam_x = cam_tile_x_, saved_cam_y = cam_tile_y_;
    const int saved_fine_x = cam_fine_x_, saved_fine_y = cam_fine_y_;
    const int saved_town_id = town_id_;
    // Choose the shadow path: the sub_13B8D loop uses sub_13D04 only, town_loop uses
    // sub_22897 only. Every exit path, including an op17 early return, clears this flag.
    in_eve_cutscene_ = true;
    // Opcode length table (off_127A3, consumed by sub_12857), including 17:2, 18:5 and 23:5.
    static const int kOpLen[30] = {
        0,
        3,3,3,3,3,3,3,3, // 1..8
        4,3,3,4,27,2,4,3, // 9..16
        2,5,34,3,4,4,5,3, // 17..24 (op19 = 0x22 words, op20 = 3, op24 = 3)
        3,3,3,3,16        // 25..29 (loc_12A2B common exit +3; op29 is +0x10)
    };
    fprintf(stderr, "PACK: run_eve %d total=%u begin (actors with len>0:", event_id, eve.total);
    for (int a = 0; a < 51; a++) if (!eve.actors[a].words.empty()) fprintf(stderr, " %d(%zu)", a, eve.actors[a].words.size());
    fprintf(stderr, ")\n");

    // Frame loop: sub_13B8D repeats sub_12857 plus rendering while frame < total. Phase 1
    // (spawn, counters, blocking dialogue) and phase 2 (1 px per frame) run together, giving
    // the three-actor walk of cap4.avi. G2_PACK_AUTO teleports without rendering.
    // sub_12857 keeps a per-actor PC (unk_7CF52 high word / word_7CF54): each actor fetches
    // at most the single command at its PC, only when the command's fire value equals the
    // current frame and the actor is not moving (unk_7D7EF == 0); otherwise it runs the
    // phase-2 move step (loc_1336C) and leaves the PC alone. A command whose fire frame
    // passes while the actor is moving is therefore skipped forever and stalls that actor.
    int eve_pc[51] = {};
    bool eve_auto_fast = (dbg_env("G2_PACK_AUTO"));
    // 60 Hz pacer, mirroring the sub_2B470 vsync wait so movement looks smooth.
    GameTimer::FramePacer eve_pacer(60.0);
    for (int frame = 0; frame < eve.total; frame++) {
        if (should_exit_) break;
        bool saw_visual = false, saw_dialog = false, saw_move_setup = false;
        for (int a = 0; a < 51; a++) {
            const std::vector<uint16_t>& w = eve.actors[a].words;
            size_t i = (size_t)eve_pc[a];
            // loc_1336C gate: a moving actor (counter != 0) fetches nothing this frame.
            {
                const EveActorState& st = eve_actors_[a];
                if (st.active && st.move_op >= 1 && st.move_op <= 8 && st.move_counter > 0) {
                    continue;
                }
            }
            if (i + 1 < w.size()) {
                uint16_t fire = w[i], op = w[i + 1];
                int L = 0;
                if (op >= 1 && op <= 29) L = kOpLen[op];
                if (L <= 0) {
                    // Invalid op (bl-1 > 0x1C at loc_12857+...): the original jumps to
                    // the phase-2 tail without advancing the PC, so the actor stalls.
                    fprintf(stderr, "PACK: EVE %d actor%d bad op=%u at pc=%zu, stall\n",
                            event_id, a, op, i);
                    continue;
                }
                if (i + (size_t)L > w.size()) {
                    continue;
                }
                if ((int)fire == frame) {
                    switch (op) {
                    case 10: {
                        int dir_byte = (L >= 3) ? w[i + 2] : 0x50;
                        eve_op_set_dir(a, dir_byte);
                        saw_visual = true;
                        break;
                    }
                    case 11: {
                        int text_id = (L >= 3) ? w[i + 2] : 0;
                        fprintf(stderr, "PACK: EVE %d actor%d frame %d op11 dialogue text=%d\n",
                                event_id, a, frame, text_id);
                        saw_dialog = true;
                        eve_op_dialogue(text_id, a);
                        break;
                    }
                    case 12: {
                        // loc_12B90: g_event_flags[p0] = (byte)p1, length 4. The TOOL-mode
                        // printf/getch debug output of byte_54040 is not ported.
                        const int idx = (L >= 3) ? (int16_t)w[i + 2] : 0;
                        const int val = (L >= 4) ? (w[i + 3] & 0xFF) : 0;
                        if (idx >= 0 && idx < 20000) g_event_flags[idx] = (uint8_t)val;
                        break;
                    }
                    case 13:
                        eve_op_spawn(a, w, i);
                        saw_visual = true;
                        break;
                    case 14:
                        eve_op_hide(a);
                        saw_visual = true;
                        break;
                    case 1: case 2: case 3: case 4:
                    case 5: case 6: case 7: case 8: {
                        int steps = (L >= 3) ? w[i + 2] : 0;
                        saw_move_setup = true;
                        eve_op_move(a, op, steps);
                        break;
                    }
                    case 15: {
                        int tx = (L >= 3) ? w[i + 2] : cam_tile_x_;
                        int ty = (L >= 4) ? w[i + 3] : cam_tile_y_;
                        eve_op_camera_snap(tx, ty);
                        saw_visual = true;
                        break;
                    }
                    case 16: {
                        int f = (L >= 3) ? w[i + 2] : 0;
                        eve_op_set_frame(a, f);
                        saw_visual = true;
                        break;
                    }
                    case 17: {
                        // loc_12DDF: op17 END. With byte_5268C (eve_end_no_exit_) == 1 it
                        // shows the "town exit" message (sub_34FBE) and continues; otherwise
                        // sub_18FD0(unk_5005E = "") writes "$", blacks the screen
                        // (sub_20019) and exits 1. MAIN then reads an empty "$" and spawns
                        // chp.exe "p". unk_5005E is a one-byte BSS string, so it is always
                        // empty. Every town EVE (271..273 end on actor 50) exits, so chained
                        // firing within one session does not occur in the original.
                        fprintf(stderr, "PACK: EVE %d actor%d frame %d op17 END\n",
                                event_id, a, frame);
                        if (eve_end_no_exit_) {
                            show_message_box("\xA0\x61\xB7\x69 \xB9\xB7\x9E\x61\x21");
                        } else {
                            for (int pi = 0; pi < 256; pi++)
                                palette_set_entry((uint8_t)pi, 0, 0, 0);
                            video_.present();
                            do_transition("");
                            in_eve_cutscene_ = false;
                            return true;
                        }
                        break;
                    }
                    case 18: {
                        int dst = (L >= 5) ? w[i + 2] : town_id_;
                        int cx = (L >= 5) ? w[i + 3] : cam_tile_x_;
                        int cy = (L >= 5) ? w[i + 4] : cam_tile_y_;
                        fprintf(stderr, "PACK: EVE %d actor%d frame %d op18 warp town=%d cam=(%d,%d)\n",
                                event_id, a, frame, dst, cx, cy);
                        eve_op_warp(dst, cx, cy);
                        saw_visual = true;
                        break;
                    }
                    case 9: {
                        // loc_12AA0: set the actor tile directly (no Y-1 unlike op13); the
                        // screen position is recomputed as (tile - cam)*size + fine.
                        if (a < MAX_EVE_ACTORS && L >= 4) {
                            eve_actors_[a].tile_x = w[i + 2];
                            eve_actors_[a].tile_y = w[i + 3];
                            eve_actors_[a].fine_x = eve_actors_[a].fine_y = 0;
                        }
                        saw_visual = true;
                        break;
                    }
                    case 19: {
                        // loc_12EA7: activate NPC[actor] and copy script words 3..0x21 into
                        // the NPC struct (length 0x22): name 20 B at 3..0x16, then tile_x
                        // 0x17, tile_y 0x18, talk_id 0x19, +0x1A/0x1B, sprite_id 0x1B,
                        // portrait_word 0x1C, target_x 0x1D, target_y 0x1E, direction 0x1F,
                        // speed 0x20, move_type 0x21. Finally sub_1A51D registers slot
                        // p0+2 (p0 = w[i+2]) with type 4 and sprite<<4 at (0,0).
                        if (a < MAX_TOWN_NPCS && L >= 34) {
                            PackNPC& n = npcs_[a];
                            n.active = 1;
                            for (int k = 0; k < 20; k++) n.name[k] = (char)(w[i + 3 + k] & 0xFF);
                            n.tile_x = w[i + 0x17];
                            n.tile_y = w[i + 0x18];
                            n.talk_id = w[i + 0x19];
                            n.param1 = w[i + 0x1A];
                            n.sprite_id = w[i + 0x1B];
                            n.portrait_word = w[i + 0x1C];
                            n.target_x = w[i + 0x1D];
                            n.target_y = w[i + 0x1E];
                            n.direction = (uint8_t)(w[i + 0x1F] & 0xFF);
                            n.move_speed = (uint8_t)(w[i + 0x20] & 0xFF);
                            n.move_type = (uint8_t)(w[i + 0x21] & 0xFF);
                            npc_fine_offset_[a] = 0;
                            npc_anim_frame_[a] = 0;
                            const int p0 = (int)(int16_t)w[i + 2];
                            const int slot = p0 + 2;
                            const int sprite = (int)n.sprite_id * 16;
                            if (slot >= 0 && slot < 320) {
                                eve_slots_[slot].active = true;
                                eve_slots_[slot].type = 4;
                                eve_slots_[slot].sprite = sprite;
                                eve_slots_[slot].x = 0;
                                eve_slots_[slot].y = 0;
                            }
                        }
                        saw_visual = true;
                        break;
                    }
                    case 20: {
                        // loc_12E47: remove actor entry p0. NPC (p0-2) is deactivated with its
                        // first name byte cleared, the slot p0 is cleared, and the whole NPC
                        // layer is zeroed.
                        const int entry = (int16_t)w[i + 2];
                        if (entry >= 0x140 && entry - 0x140 < MAX_EVE_ACTORS) eve_actors_[entry - 0x140].active = false;
                        if (entry >= 0 && entry < 320) eve_slots_[entry].active = false;
                        const int n = entry - 2;
                        if (n >= 0 && n < MAX_TOWN_NPCS) { npcs_[n].active = 0; npcs_[n].name[0] = 0; }
                        if (layer_npc_) memset(layer_npc_, 0, (size_t)map_w_ * map_h_);
                        saw_visual = true;
                        break;
                    }
                    case 21: {
                        // loc_1301A: NPC(p0-2).talk_id = p1
                        const int n = (int16_t)w[i + 2] - 2;
                        if (n >= 0 && n < MAX_TOWN_NPCS) npcs_[n].talk_id = w[i + 3];
                        break;
                    }
                    case 22: {
                        // loc_1304D: sub_237F0(p0 = character_id, p1 = team) sets team byte
                        // (+0x87) of roster entry [id - 1].
                        const int idx = (int)w[i + 2] - 1;
                        if (idx >= 0 && idx < MAX_CHARACTERS)
                            reinterpret_cast<uint8_t*>(&g_characters[idx])[0x87] = (uint8_t)w[i + 3];
                        break;
                    }
                    case 24:
                        // loc_13097: sub_17EFF(p0)
                        eve_scroll_text(w[i + 2]);
                        saw_visual = true;
                        break;
                    case 25: {
                        // loc_130A7: with 600 or more items, save the screen (sub_2B3DC), show
                        // the message (sub_34FBE), open the discard list (sub_34261), then
                        // sub_376D8(p0 - 1) and the acquired popup.
                        const int item1 = w[i + 2];
                        const int s = cur_slot();
                        if (g_slot_item_count[s] >= kSlotBytes) {
                            shop_grab_screen();
                            show_message_box(kPackMsgItemFull2);
                            status_inventory_list();
                        }
                        inv_add_front(item1 - 1);
                        load_item_data();
                        char nm[40] = {0};
                        if (item1 - 1 >= 0 && item1 - 1 < MAX_ITEMS)
                            for (int k = 0; k < 0x25 && item_data_[item1 - 1][1 + k] != 0; k++) nm[k] = (char)item_data_[item1 - 1][1 + k];
                        show_item_acquired(item1 - 1, nm, false);
                        saw_visual = true;
                        break;
                    }
                    case 26: {
                        // loc_1326A: remove the first inventory item equal to p0 (sub_3767B).
                        const int s = cur_slot();
                        const int cnt = g_slot_item_count[s];
                        for (int di = 0; di < cnt; di++) {
                            if (slot_item(s, di) == (uint8_t)w[i + 2] && (int)w[i + 2] < 256) {
                                inv_remove(di);
                                break;
                            }
                        }
                        break;
                    }
                    case 27:
                        // loc_132DD: actor movement speed unk_8293C = p0.
                        if (a < MAX_EVE_ACTORS) eve_actors_[a].move_speed = (int)(w[i + 2] & 0xFF);
                        break;
                    case 28:
                        // loc_132EB: with byte_82731 == 1, sub_39E5E(p0, 1) changes the BGM
                        // track (CD audio; state-only here, see bgm_play_track).
                        if (game_cfg_[1] == 1) bgm_play_track(w[i + 2]);
                        break;
                    case 29: {
                        // loc_1330F: load the 13-byte filename (low bytes of words 2..14) with
                        // sub_39431 into sound slot 10 and play it through sub_394D1 (L4137)
                        // with volume byte_82732*10.
                        char nm[14] = {0};
                        for (int k = 0; k < 13; k++) nm[k] = (char)(w[i + 2 + k] & 0xFF);
                        if (nm[0]) {
                            std::string path = game_path(nm);
                            if (!file_exists(path.c_str())) {
                                std::string lower = nm;
                                for (char& c : lower) c = (char)tolower((unsigned char)c);
                                path = game_path(lower.c_str());
                            }
                            play_pack_voc_path(path);
                        }
                        break;
                    }
                    case 23: {
                        int cnt = (L >= 3) ? w[i + 2] : 0;
                        int dx = (L >= 4) ? w[i + 3] : 0;
                        int dy = (L >= 5) ? w[i + 4] : 0;
                        eve_op_camera_pan(cnt, dx, dy);
                        saw_visual = true;
                        break;
                    }
                    default:
                        fprintf(stderr, "PACK: EVE %d actor%d frame %d op%u (len %d stub, p0=%u)\n",
                                event_id, a, frame, op, L, L >= 3 ? w[i + 2] : 0);
                        break;
                    }
                    if (should_exit_) return true;
                    eve_pc[a] = (int)(i + (size_t)L);
                }
            }
        }
        if (eve_auto_fast) continue; // skip rendering and animation after the teleport
        if (should_exit_) break;
        // Phase 2: advance every moving actor by 1 px, including actors set up this frame.
        bool moving = eve_animate_moves_once();
        // The sub_13B8D frame loop runs sub_12857 (VM), sub_21D70 (tile animation),
        // sub_21E29, sub_13D04 and sub_2294D every frame; only frames that drew their own
        // dialogue are skipped.
        update_tile_animation();
        bool need_render = !saw_dialog || moving;
        (void)saw_visual;
        if (need_render) {
            video_.clear();
            render_town_frame();
            video_.present();
            pack_maybe_dump_shot(video_);
            eve_pacer.pace(input_);
        }
        // Outside G2_PACK_AUTO, saw_move_setup has already become a counter; nothing to do.
        (void)saw_move_setup;
        // sub_13B8D loc_13C80: every 60 EVE frames call sub_399AC. The first frame also
        // applies the saved palette (sub_20118) and sets word_7D78C = 1; the port renders
        // every frame, so only the tick is kept.
        if (frame > 0 && frame % 60 == 0) bgm_tick();
    }
    // Tail safety: if a counter survives past total (only on a malformed EVE), advance up to
    // 512 frames to settle the positions. Script 271 never needs this.
    if (!eve_auto_fast && !should_exit_) {
        for (int extra = 0; extra < 512; extra++) {
            bool moving = eve_animate_moves_once();
            if (!moving) break;
            video_.clear();
            render_town_frame();
            video_.present();
            pack_maybe_dump_shot(video_);
            eve_pacer.pace(input_);
            if (should_exit_) break;
        }
    }
    // ---- sub_13B8D teardown: sub_11F2D ----
    // On script exit the original deactivates all 51 actor slots (unk_82934 = 0, PC, opcode
    // and movement counters reset, sprites released by sub_1A5CA). Unlike op14, this applies
    // to every slot regardless of respawn.
    {
        int n = 0;
        for (int a = 0; a < MAX_EVE_ACTORS; a++) if (eve_actors_[a].active) n++;
        eve_clear_actors();
        fprintf(stderr, "PACK: run_eve %d done (total %u frames, %d actors cleared)\n",
                event_id, eve.total, n);
    }
    in_eve_cutscene_ = false;
    // sub_13B8D step 4: restore the camera, and if the town id changed (op18 warp during the
    // cutscene), warp back with sub_20EB0 to the original id.
    cam_tile_x_ = saved_cam_x; cam_tile_y_ = saved_cam_y;
    cam_fine_x_ = saved_fine_x; cam_fine_y_ = saved_fine_y;
    if (!should_exit_ && town_id_ != saved_town_id) warp_to_town(saved_town_id);
    return true;
}

// ---- sub_19528: town main interaction loop ----
void Pack::town_loop() {
    GameTimer::FramePacer pacer(60.0);
    while (!should_exit_) {
        input_.update();
        if (input_.quit_requested()) {
            do_transition("EXIT");
            break;
        }

        // sub_18BE6 at the top of the sub_19528 loop: hide NPCs by story progress.
        apply_story_npc_visibility();

        if (input_.is_key_pressed(SDLK_ESCAPE)) {
            handle_options_menu();
        }

        // sub_190A3 loc_191A5: dword_7CF20 == 2 (right-only) or dword_1A01D4 != 0 (Left Alt
        // level, scancode 0x38, the right-click substitute) opens sub_32315 (status window).
        // The original polls every frame; the port opens on the right-click edge or the Alt
        // key edge (is_key_pressed), since status_window blocks until exit.
        if (input_.get_mouse().right_clicked || input_.is_key_pressed(SDLK_LALT)) {
            status_window();
            input_.update();
            if (should_exit_) break;
            continue;
        }

        // ---- sub_19528 -> sub_190A3 -> sub_25F21: interact with a clicked NPC ----
        // Every frame, regardless of the player's tile step, the town loop gets the NPC
        // clicked this frame from sub_190A3 and runs the sub_25F21 dispatcher. Here
        // is_mouse_over_npc (the sub_1F6E4 hit test) finds the hovered NPC and interaction
        // fires only on a left-click event. decide_step_direction already blocks movement
        // over an NPC, so only the click needs checking. This runs before the tile-based
        // interaction, matching the original's NPC-first order.
        {
            int hover_npc = -1;
            if (input_.was_left_clicked() &&
                is_mouse_over_npc(input_.get_mouse_x(), input_.get_mouse_y(), hover_npc)) {
                // sub_19528: dword_8271C = npc_idx + 1 (the click trigger for sub_2D038).
                npc_trigger_ = (uint32_t)(hover_npc + 1);
                // sub_19528 C5: word dword_82943 = word dword_F309C after sub_25F21.
                // sub_1E4B4 saved dword_82943 into dword_F309C on hover, so the frame
                // after the dialogue equals the frame before it. The port derives the
                // frame from dir + anim counter, so saving and restoring them here gives
                // the same visible result even if a dialogue path ever touches them.
                const uint8_t saved_dir = player_dir_;
                const uint8_t saved_anim = player_anim_frame_;
                // sub_19528: "mov word ptr dword_1A00C8+2, ax" (sub_25F21 result), then
                // sub_3197D waits for the button release.
                story_hi_ = interact_with_npc(hover_npc);
                player_dir_ = saved_dir;
                player_anim_frame_ = saved_anim;
                wait_buttons_released();
                click_latch_ = true;   // sub_1F6E4 returns the NPC and sets byte_15BA22 = 1
            } else {
                // No click: the trigger stays 0. The original refreshes it every iteration
                // from sub_190A3; the port sets it on the click frame and clears it on the
                // next one, after check_and_fire.
            }
            // Without a click the trigger stays 0; it is cleared after the event check below.
        }

        // Movement is not click-to-path: at the start of every tile step the direction is
        // re-decided from the arrow keys or, while the left button is held, from the mouse
        // quadrant (sub_1D12F loc_1DA1C; see decide_step_direction). A click only triggers an immediate
        // interaction with the current or adjacent tile; the check below covers clicking a
        // chest, building or NPC while standing still.
        // End of sub_1F6E4/sub_27BF2: release the latch (byte_15BA22) once the action
        // input (dword_1A0168 Left Ctrl level) and both mouse buttons are up. The
        // original keeps the latch while dword_7CF20 == 1 or == 2 (left-only or
        // right-only) or Ctrl is held. Enter/Space edges are kept as extra aliases.
        {
            const bool action = input_.is_key_held(SDLK_LCTRL) ||
                                input_.is_key_pressed(SDLK_RETURN) ||
                                input_.is_key_pressed(SDLK_SPACE);
            const bool left_only = input_.is_left_button() && !input_.is_right_button();
            const bool right_only = input_.is_right_button() && !input_.is_left_button();
            if (!action && !left_only && !right_only)
                click_latch_ = false;
        }
        town_click_interaction();   // sub_1D12F: building/item under the mouse
        town_key_interaction();     // sub_1A7B5/sub_1A8F3: the facing tile

        update_player_movement();
        update_npc_movement();
        // Gate at the top of sub_190A3 (PACK_code.ASM line 11418): sub_2D038 runs only when
        // cam_fine_x == 0 and cam_fine_y == 0, which avoids firing mid-scroll. The one-shot
        // entry call (main_+166) ignores the gate, so run() keeps check_and_fire(0, true).
        if (cam_fine_x_ == 0 && cam_fine_y_ == 0) {
            check_and_fire_town_events(npc_trigger_, false);
        } else if (dbg_env("G2_PACK_TRACE")) {
            fprintf(stderr, "PACK: gate skip fine=(%d,%d) player=(%d,%d) trig=%u\n",
                    cam_fine_x_, cam_fine_y_, player_tile_x_, player_tile_y_, npc_trigger_);
        }
        npc_trigger_ = 0; // clear for the next frame; only the click frame carries it

        update_tile_animation();   // sub_190A3 loc_19123: sub_21D70 -> sub_21E29 -> sub_22897 -> sub_2294D
        video_.clear();
        render_town_frame();
        video_.present();

        // sub_190A3 tail: byte_54041 reapply (sub_20118) instead of the vsync wait.
        // The port renders every frame, so the flag only records the state; the next
        // town frame already uses the restored palette.
        if (palette_reapply_pending_) palette_reapply_pending_ = false;

        // sub_19528 loc_1952C..loc_19561: every 60 iterations call sub_399AC.
        if (++bgm_tick_count_ >= 60) {
            bgm_tick_count_ = 0;
            bgm_tick();
        }

        pacer.pace(input_);
    }
}

// ---- main_ (0x195D0 in PACK.ASM) ----
int Pack::run(const std::string& mode, const std::string& town_param) {
    fprintf(stderr, "PACK: === Town engine started (mode=%s, param=%s) ===\n",
            mode.c_str(), town_param.c_str());

    // Main_ validates argv[1]: anything other than exactly "SM" or "TOOL" (case sensitive)
    // exits with 0xFF.
    if (!(mode == "SM" || mode == "TOOL")) {
        fprintf(stderr, "PACK: unknown mode '%s' (need SM/TOOL), exit 0xFF\n", mode.c_str());
        return 0xFF;
    }

    should_exit_ = false;
    return_code_ = 0;
    npc_trigger_ = 0;
    story_hi_ = 0;

    // 1. Reset the screen (Video/Input are injected by the constructor and initialised once
    //    in gene2.cpp).
    video_.set_mode_13h();

    // 2. Load Fonts
    font_.load_hangul(game_path("newhan.fnt").c_str());
    font_.load_ascii(game_path("newascii.fnt").c_str());
    font_.set_text_mode(4, 0x1000, 0xFF);

    // 3. Load blackbox and config.
    if (!load_blackbox()) { should_exit_ = true; return_code_ = 1; return -1; }   // exit without saving
    // sub_235A7: load game.cfg (movement speed in byte_82730). The original calls it once at
    // startup and sub_1D12F re-reads byte_82730 every step, so one load before the town is
    // enough.
    load_game_config();

    // 4. Parse Town ID
    // The original atoi's argv[2] into a byte (byte_15BA23) and exits on 0xFF; a non-numeric
    // prefix is not skipped.
    const int tid = (uint8_t)atoi(town_param.c_str());
    if (tid == 0xFF) {
        fprintf(stderr, "PACK: warning: invalid town id '%s' (0xFF)\n", town_param.c_str());
        return -1;
    }
    town_id_ = tid;
    // main_+145 sub_19571: town BGM track (before the town load).
    bgm_init_town();

    // 5. sub_1A094: Display NOWLOAD.PCX
    std::string nowload_path = game_path("nowload.pcx");
    if (!file_exists(nowload_path.c_str())) nowload_path = game_path("NOWLOAD.PCX");
    pcx_load_to_buffer(nowload_path, video_.get_backbuffer(), SCREEN_W, SCREEN_H);
    video_.present();

    // 6. Load item database
    load_item_data();

    // 7. Load Town Map, Tiles, Palette & .TEC (this calls free_town_data)
    if (!load_town_and_tec(town_id_)) {
        fprintf(stderr, "PACK: warning: Failed to load town map data for town %d\n", town_id_);
        return -1;
    }
    // main_+161/+181: with byte_82731 == 1, initialise CD (sub_397DD) and play the town
    // track (sub_39E5E). State-only here (no CD data ships); SFX (sub_18BC5) is covered
    // by play_pack_sfx.
    if (game_cfg_[1] == 1) {
        bgm_ready_ = true;
        bgm_play_track(bgm_town_track_);
    }

    // 7b. Load citizen animation sprites AFTER town load (ASM 0x18B2A)
    // free_town_data() inside load_town_and_tec clears sprites, so must load after
    if (town_id_ % 2 == 0) {
        load_citizen_sprites(cstldata_path("citizen1.spr"));
    } else {
        load_citizen_sprites(cstldata_path("citizen2.spr"));
    }
    load_standard_sprites(game_path("item.spr"), item_sprites_);
    load_standard_sprites(cstldata_path("itembox.spr"), itembox_sprites_);
    place_chest_top_tiles();

    // 7c. sub_2294D loc_22A01: load the left/right screen frame overlays from PACK.EXE VAs
    // 0x54630 and 0x55A99.
    load_frame_overlays();

    // 7d. sub_190A3 loc_1948C: load the cursor sprites (PACK.EXE VA 0x53D0C). The data does
    // not depend on the town, so one load is enough; on failure only the cursor is missing.
    load_cursor_sprites();

    // 7d-2. sub_22897: load the foot shadow sprite (PACK.EXE VA 0x56C00). Town-independent,
    // so one load is enough; on failure only the shadows are missing.
    load_shadow_sprite();

    // 7e. Load the NPC dialogue assets (off_58CF4, unk_7C4D2, unk_7C220, off_5E4F9). They do
    // not depend on the town, so one load is enough.
    load_dialogue_sprites();

    // 7f. main_+166: one sub_2D038 entry check right after the town load. main calls it
    // unconditionally, without the cam_fine gate.
    fprintf(stderr, "PACK: entry check town=%d player=(%d,%d) flags200=%u\n",
            town_id_, player_tile_x_, player_tile_y_, g_event_flags[200]);
    check_and_fire_town_events(0, true);
    if (should_exit_) return return_code_;

    // 8. Run Town Loop
    town_loop();

    return return_code_;
}