// combatii_map: resource loading (MAP/FPM/SCE/NC2/weather/HUD/effects), rendering and dialogue UI.
// Split TU of combatii.cpp; code moved unchanged.
#include "combatii.h"
#include "devutil.h"
#include "combatii_internal.h"

// Names in the original tables (off_70024 and friends) with a base name of 9+ characters were cut
// to 8 characters by the DOS/ISO9660 8.3 rule when the release was mastered. Measured: table
// `NS_LACIDE.NC2` (9-char base) vs `NS_LACID.NC2` on disk; all four LACIDE entries are like this,
// and their art differs from S_LACIDE.NC2. sub_11575 would simply fail on the original executable,
// so the port retries with the truncated name to recover the artwork that is actually shipped.
static std::string dos83_fallback_name(const char* fname) {
    if (!fname || !*fname) return {};
    const std::string s = fname;
    const size_t dot = s.rfind('.');
    const std::string base = (dot == std::string::npos) ? s : s.substr(0, dot);
    const std::string ext = (dot == std::string::npos) ? "" : s.substr(dot);
    if (base.size() <= 8) return {};  // nothing to truncate
    return base.substr(0, 8) + ext;
}

// ---- sub_11575 / sub_1198D: open a character image file by unit type code ----
//   sub_1198D(type) -> sub_114EF finds the slot; if it is missing, sub_11575(type, slot)
//   loads `CHARACTE\<off_70024[type]>`. The port loads each type code only once.
const NC2Frame* CombatII::nc2_frame_for(int type_code, int dir, int pose) const {
    if (type_code < 0) return nullptr;
    if (DevUtil::nc2_off()) return nullptr;  // A/B check: force the CITIZEN fallback without NC2
    auto it = nc2_chars_.find(type_code);
    if (it == nc2_chars_.end()) {
        const char* fname = char_file_name_for_type(type_code);
        auto parser = std::make_unique<NC2Parser>();
        bool ok = false;
        if (fname && *fname) {
            const std::string path = characte_path(fname);
            if (file_exists(path.c_str())) ok = parser->load(path, type_code);
        }
        std::string short_name;
        if (!ok && fname && *fname && (short_name = dos83_fallback_name(fname)).size()) {
            const std::string path = characte_path(short_name.c_str());
            if (file_exists(path.c_str())) {
                ok = parser->load(path, type_code);
                if (ok) {
                    fprintf(stderr, "COMBAT: NC2 type=%d %s not on disk -> 8.3 fallback %s\n",
                            type_code, fname, short_name.c_str());
                    fname = nullptr;  // make the loaded log below print the fallback name
                }
            }
        }
        if (!ok) {
            fprintf(stderr, "COMBAT: warning: NC2 type=%d (%s) load failed: %s\n", type_code,
                    fname ? fname : "(no name)", parser->error().c_str());
            nc2_chars_[type_code] = nullptr;  // cache the failure so it is not retried every frame
            return nullptr;
        }
        fprintf(stderr, "COMBAT: NC2 type=%d %s loaded (%d frames, class %d)\n",
                type_code, fname ? fname : short_name.c_str(), parser->count(),
                type_code < 0x12C ? 0 : (type_code < 0x1F4 ? 1 : 2));
        // G2_NC2_TRACE: log each record (w, h, offsets, pixel sum).
        if (DevUtil::nc2_trace()) {
            for (int d = 0; d < NC2Parser::DIR_COUNT; d++)
                for (int p = 0; p < NC2Parser::POSE_COUNT; p++) {
                    const NC2Frame* fr = parser->frame(d, p);
                    if (!fr) continue;
                    uint64_t sum = 0;
                    for (uint8_t px : fr->pixels) sum += px;
                    fprintf(stderr, "NC2: type=%d dir=%d pose=%d %dx%d off=(%d,%d) sil=(%d,%d) sum=%llu\n",
                            type_code, d, p, fr->w, fr->h, fr->x_off, fr->y_off, fr->sil_x, fr->sil_y,
                            (unsigned long long)sum);
                }
        }
        it = nc2_chars_.emplace(type_code, std::move(parser)).first;
    }
    if (!it->second) return nullptr;
    return it->second->frame(dir, pose);
}

// ---- D66A8 raw table (equivalent to the sub_11575 loader) ----
// entry[0]/[1] = NC2Frame tile_w/h, [2]/[3] = w/h, [4]/[5] = x/y offsets,
// [6] = shadow, [7]/[8] = sil; [9..12] are sprite pointers and stay zero.
bool CombatII::battle_d66a8_raw(int type_code, int dir, int pose, uint8_t out13[13]) const {
    const NC2Frame* fr = nc2_frame_for(type_code, dir & 3, pose);
    if (!fr || fr->w <= 0 || fr->h <= 0) return false;
    for (int i = 0; i < 13; i++) out13[i] = 0;
    out13[0] = (uint8_t)(fr->tile_w & 0xFF);
    out13[1] = (uint8_t)(fr->tile_h & 0xFF);
    out13[2] = (uint8_t)(fr->w & 0xFF);
    out13[3] = (uint8_t)(fr->h & 0xFF);
    out13[4] = (uint8_t)((int8_t)fr->x_off);
    out13[5] = (uint8_t)((int8_t)fr->y_off);
    out13[6] = (uint8_t)(fr->shadow_idx & 0xFF);
    out13[7] = (uint8_t)((int8_t)fr->sil_x);
    out13[8] = (uint8_t)((int8_t)fr->sil_y);
    return true;
}

bool CombatII::battle_d66a8_footprint(int type_code, int dir, int* w, int* h) const {
    uint8_t raw[13];
    if (battle_d66a8_raw(type_code, dir & 3, 0, raw)) {
        if (w) *w = raw[0] ? raw[0] : 1;
        if (h) *h = raw[1] ? raw[1] : 1;
        return true;
    }
    if (w) *w = 1;
    if (h) *h = 1;
    return false;
}

// Blit a strip (w*h, 0xFF = transparent), matching the staging-buffer layout made by sub_10083.
void CombatII::blit_nc2_frame(int x, int y, const NC2Frame& fr) {
    if (fr.pixels.empty() || fr.w <= 0 || fr.h <= 0) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    for (int row = 0; row < fr.h; row++) {
        const int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        const uint8_t* src = fr.pixels.data() + (size_t)row * fr.w;
        uint8_t* d = dest + (size_t)dy * SCREEN_W;
        for (int col = 0; col < fr.w; col++) {
            const uint8_t px = src[col];
            if (px == 0xFF) continue;  // transparent
            const int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            d[dx] = px;
        }
    }
}

// sub_5F4AF (NC2 strip version): draw only the top scan_limit scanlines.
// The unit frame is already decoded, so a row-limit test gives the same result as leaving the
// original RLE decode loop.
void CombatII::blit_nc2_frame_clipped(int x, int y, const NC2Frame& fr, int scan_limit) {
    if (fr.pixels.empty() || fr.w <= 0 || fr.h <= 0 || scan_limit <= 0) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    for (int row = 0; row < fr.h && row < scan_limit; row++) {
        const int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        const uint8_t* src = fr.pixels.data() + (size_t)row * fr.w;
        uint8_t* d = dest + (size_t)dy * SCREEN_W;
        for (int col = 0; col < fr.w; col++) {
            const uint8_t px = src[col];
            if (px == 0xFF) continue;  // transparent
            const int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            d[dx] = px;
        }
    }
}

// sub_5F517 (dasm 107394): fill every opaque pixel with a solid color without reading the palette.
// The drop effect (sub_50216) paints unit silhouettes with color 2 + counter.
void CombatII::blit_nc2_frame_solid(int x, int y, const NC2Frame& fr, uint8_t color) {
    if (fr.pixels.empty() || fr.w <= 0 || fr.h <= 0) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    for (int row = 0; row < fr.h; row++) {
        const int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        const uint8_t* src = fr.pixels.data() + (size_t)row * fr.w;
        uint8_t* d = dest + (size_t)dy * SCREEN_W;
        for (int col = 0; col < fr.w; col++) {
            const uint8_t px = src[col];
            if (px == 0xFF) continue;  // transparent
            const int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            d[dx] = color;
        }
    }
}

void CombatII::blit_tile_24x16(int x, int y, const uint8_t* src_pixels) {
    if (!src_pixels) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    for (int ty = 0; ty < TOWN_TILE_H; ty++) {
        int dy = y + ty;
        if (dy < 0 || dy >= SCREEN_H) continue;
        const uint8_t* s = src_pixels + ty * TOWN_TILE_W;
        uint8_t* d = dest + dy * SCREEN_W + x;
        for (int tx = 0; tx < TOWN_TILE_W; tx++) {
            int dx = x + tx;
            if (dx < 0 || dx >= SCREEN_W) continue;
            d[tx] = s[tx];
        }
    }
}

void CombatII::put_pixel(int x, int y, uint8_t col) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    dest[y * SCREEN_W + x] = col;
}

// Exact copy of Pack::blit_sprite_rle (sub_2B240).
void CombatII::blit_sprite_rle(int x, int y, const uint8_t* rle_data) {
    if (!rle_data) return;
    uint8_t* dest_buf = video_.get_backbuffer();
    if (!dest_buf) return;
    uint16_t chunk_count = rle_data[0] | (rle_data[1] << 8);
    if (chunk_count == 0 || chunk_count > 800) return;
    const uint8_t* ptr = rle_data + 2;
    int rel_offset = 0;
    for (uint16_t i = 0; i < chunk_count; i++) {
        if (ptr + 4 > rle_data + 10000000) break;
        uint16_t skip = ptr[0] | (ptr[1] << 8); ptr += 2;
        uint8_t dword_count = *ptr++;
        uint8_t byte_count = *ptr++;
        rel_offset += skip;
        int total_bytes = dword_count * 4 + byte_count;
        for (int b = 0; b < total_bytes; b++) {
            int cur_pixel_rel = rel_offset + b;
            int cur_py = y + (cur_pixel_rel / SCREEN_W);
            int cur_px = x + (cur_pixel_rel % SCREEN_W);
            if (cur_px >= 0 && cur_px < SCREEN_W && cur_py >= 0 && cur_py < SCREEN_H) {
                dest_buf[cur_py * SCREEN_W + cur_px] = *ptr;
            }
            ptr++;
        }
        rel_offset += total_bytes;
    }
}

// sub_5EE51: flat w/h + pixels (0xFF = transparent), used only for TIME_GRA.SYB.
// The original resolves the screen offset with sub_5EB00, reads w = [ebx] and h = [ebx+2],
// then copies the flat pixels from +4 while skipping 0xFF. TIME_GRA.SYB is not RLE
// (w*h == size-4), so blit_sprite_rle would display it incorrectly.
void CombatII::blit_flat_transparent(int x, int y, const uint8_t* flat_data) {
    if (!flat_data) return;
    uint8_t* dest_buf = video_.get_backbuffer();
    if (!dest_buf) return;
    const uint16_t w = (uint16_t)(flat_data[0] | (flat_data[1] << 8));
    const uint16_t h = (uint16_t)(flat_data[2] | (flat_data[3] << 8));
    if (w == 0 || h == 0 || w > 320 || h > 200) return;
    const uint8_t* src = flat_data + 4;
    for (int row = 0; row < (int)h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int col = 0; col < (int)w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= SCREEN_W) continue;
            const uint8_t px = src[(size_t)row * w + (size_t)col];
            if (px != 0xFF) dest_buf[(size_t)dy * SCREEN_W + (size_t)dx] = px;
        }
    }
}

// sub_5EFB9: same [w][h]+flat layout, but clipped at 13Fh/C7h. The original computes the
// left/top source offsets from a negative x/y and the right/bottom trims from x+w-13Fh /
// y+h-C7h, so column 319 and row 199 are never written.
void CombatII::blit_flat_clip319(int x, int y, const uint8_t* flat_data) {
    if (!flat_data) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    const int src_w = flat_data[0] | (flat_data[1] << 8);
    const int src_h = flat_data[2] | (flat_data[3] << 8);
    if (src_w <= 0 || src_h <= 0 || src_w > SCREEN_W || src_h > SCREEN_H) return;
    int w = src_w, h = src_h;
    if (x + w > 0x13F) w -= x + w - 0x13F;
    if (y + h > 0xC7) h -= y + h - 0xC7;
    int left = 0, top = 0;
    if (x < 0) { left = -x; x = 0; w -= left; }
    if (y < 0) { top = -y; y = 0; h -= top; }
    if (w <= 0 || h <= 0) return;
    const uint8_t* src = flat_data + 4;
    for (int row = 0; row < h; ++row) {
        const uint8_t* srow = src + (size_t)(top + row) * (size_t)src_w + (size_t)left;
        uint8_t* drow = dest + (size_t)(y + row) * SCREEN_W + (size_t)x;
        for (int col = 0; col < w; ++col)
            if (srow[col] != 0xFF) drow[col] = srow[col];
    }
}

// sub_5F4AF: stop decoding the RLE stream as soon as the row passes scan_limit.
// The original decrements edx (the clip height) on each scanline and leaves the decode loop at 0.
// Used for foot sinking: scanlines below a unit's feet are never drawn.
void CombatII::blit_sprite_rle_clipped(int x, int y, const uint8_t* rle_data, int scan_limit) {
    if (!rle_data || scan_limit <= 0) return;
    uint8_t* dest_buf = video_.get_backbuffer();
    if (!dest_buf) return;
    uint16_t chunk_count = rle_data[0] | (rle_data[1] << 8);
    if (chunk_count == 0 || chunk_count > 800) return;
    const uint8_t* ptr = rle_data + 2;
    int rel_offset = 0;
    bool done = false;
    for (uint16_t i = 0; i < chunk_count && !done; i++) {
        if (ptr + 4 > rle_data + 10000000) break;
        uint16_t skip = ptr[0] | (ptr[1] << 8); ptr += 2;
        uint8_t dword_count = *ptr++;
        uint8_t byte_count = *ptr++;
        rel_offset += skip;
        int total_bytes = dword_count * 4 + byte_count;
        for (int b = 0; b < total_bytes; b++) {
            int cur_pixel_rel = rel_offset + b;
            int cur_row = cur_pixel_rel / SCREEN_W;      // scanline from the sprite top
            if (cur_row >= scan_limit) { done = true; break; } // reproduces the 0x13130 exit
            int cur_py = y + cur_row;
            int cur_px = x + (cur_pixel_rel % SCREEN_W);
            if (cur_px >= 0 && cur_px < SCREEN_W && cur_py >= 0 && cur_py < SCREEN_H) {
                dest_buf[cur_py * SCREEN_W + cur_px] = *ptr;
            }
            ptr++;
        }
        rel_offset += total_bytes;
    }
}

// sub_5F3FD: draw RLE data against the 480px scratch-canvas stride (WEA_EFFC/BATTLE projectiles
// and trails). Unlike the 320-stride blit_sprite_rle, rows are derived by dividing by 480, as
// sub_11BB8 + sub_10031 assume.
void CombatII::blit_sprite_480(int x, int y, const uint8_t* rle_data) {
    if (!rle_data) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    const uint16_t chunks = (uint16_t)(rle_data[0] | (rle_data[1] << 8));
    if (chunks == 0 || chunks > 4096) return;
    const uint8_t* ptr = rle_data + 2;
    int64_t rel = 0;
    for (uint16_t i = 0; i < chunks; ++i) {
        if (ptr + 4 > rle_data + 10000000) break;
        const uint16_t skip = (uint16_t)(ptr[0] | (ptr[1] << 8));
        const size_t payload = (size_t)ptr[2] * 4u + ptr[3];
        ptr += 4;
        rel += skip;
        if (ptr + payload > rle_data + 10000000) break;
        for (size_t b = 0; b < payload; ++b) {
            const int64_t canvas_raw = (int64_t)x + rel + (int64_t)b;
            int64_t row = canvas_raw / 480;
            int64_t cx = canvas_raw % 480;
            if (cx < 0) { cx += 480; --row; }
            const int64_t py = (int64_t)y + row;
            if (cx < SCREEN_W && py >= 0 && py < SCREEN_H)
                dest[(size_t)py * SCREEN_W + (size_t)cx] = ptr[b];
        }
        rel += (int64_t)payload;
        ptr += payload;
    }
}

// ---- sub_10420: load MAP_EFFC.SYB ----
// Standard Softmax "New Fast&Compressed Sprite Ver3.0" SPR file (measured: count = 30 at 0x42,
// 12-byte headers {u32 size, u16 w, u16 h, s16 xo, s16 yo} + RLE data from 0x80, exactly 30
// sprites = 15 terrain types x 2 frames, ending precisely at EOF).
// Slot mapping is not fixed: sub_105AD loc_1089B reads the 11 selector bytes from the map trailer
// one at a time and calls sub_10420(eax = slot 0..10, edx = selector), which skips `selector`
// sprites from 0x80 and stores the **next two** into unk_789AC[slot*2 + 0/1]
// (`cmp dx,1Ch; jle`; selectors above 28 become 0). Rendering uses (terrain-0x65)&0x0F as the
// slot index (sub_12F2A loc_13123). The RLE stream follows the blit_sprite_rle convention
// (u16 chunk count + {u16 skip, u8 dword, u8 byte}) on a 320-stride linear buffer.
// If the file is missing, the table stays zero and overlays are skipped, as in the original.
bool CombatII::load_terrain_overlays(const std::string& map_filename) {
    (void)map_filename;
    for (int i = 0; i < 32; i++) terrain_overlay_[i] = TerrainOverlayFrame{};
    terrain_anim_frame_ = 0;
    static const char* CANDIDATES[] = {
        "MAP_EFFC.SYB", "MAPEFFC.SYB", "MAPEFFC.SY_", "MAP_EFFC.SY_",
    };
    std::string path;
    for (const char* name : CANDIDATES) {
        std::string p = game_path(name);
        if (!file_exists(p.c_str())) p = scenario_path(name);
        if (!file_exists(p.c_str())) {
            std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR)+"/", name);
            if (!alt.empty()) p = alt;
        }
        if (file_exists(p.c_str())) { path = p; break; }
    }
    if (path.empty()) {
        fprintf(stderr, "COMBAT: warning: MAP_EFFC.SYB not found - terrain foot overlays disabled\n");
        return false;
    }
    SPRParser spr;
    if (!spr.load(path, false)) {
        fprintf(stderr, "COMBAT: warning: MAP_EFFC.SYB parse failed: %s\n", spr.error().c_str());
        return false;
    }
    // sub_10420 x 11: selector sel of slot s -> sprites [sel], [sel+1]
    int loaded = 0;
    for (int s = 0; s < 11; s++) {
        int sel = map_trailer_.valid ? (int)map_trailer_.mapeffc[s] : 0;   // without a trailer the original reads stale buffer bytes; pin to 0
        if (sel > 0x1C) sel = 0;                       // xor esi,edx: invalid selector -> 0
        for (int f = 0; f < 2; f++) {
            const SpriteEntry* se = spr.get(sel + f);
            if (!se || !se->pixel_data || se->data_size <= 0) continue;
            TerrainOverlayFrame& fr = terrain_overlay_[s * 2 + f];
            fr.w = se->word0;         // measured: every entry is 24x8 (the blitter ignores header xo/yo)
            fr.h = se->word2;
            fr.x = se->x_offset;
            fr.y = se->y_offset;
            fr.rle.assign(se->pixel_data, se->pixel_data + se->data_size);
            loaded++;
        }
    }
    fprintf(stderr, "COMBAT: terrain foot overlays loaded=%d/22 (MAP_EFFC.SYB %d sprites, selectors="
            "[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d])\n", loaded, (int)spr.count(),
            map_trailer_.mapeffc[0], map_trailer_.mapeffc[1], map_trailer_.mapeffc[2],
            map_trailer_.mapeffc[3], map_trailer_.mapeffc[4], map_trailer_.mapeffc[5],
            map_trailer_.mapeffc[6], map_trailer_.mapeffc[7], map_trailer_.mapeffc[8],
            map_trailer_.mapeffc[9], map_trailer_.mapeffc[10]);

    // ---- sub_11BB8 (bulk battle resource load): 7 shadow.syb frames -> dword_D6658 ----
    // Unit foot shadows; the generic sub_12F2A path selects one with (attr & 0Fh) & 3.
    for (int i = 0; i < 7; i++) shadow_sprites_[i] = TerrainOverlayFrame{};
    {
        std::string sp = game_path("SHADOW.SYB");
        if (!file_exists(sp.c_str())) sp = scenario_path("SHADOW.SYB");
        if (file_exists(sp.c_str())) {
            SPRParser sh;
            if (sh.load(sp, false) && sh.count() >= 4) {
                for (int i = 0; i < 7 && i < sh.count(); i++) {
                    const SpriteEntry* se = sh.get(i);
                    if (!se || !se->pixel_data || se->data_size <= 0) continue;
                    shadow_sprites_[i].w = se->word0;
                    shadow_sprites_[i].h = se->word2;
                    shadow_sprites_[i].x = se->x_offset;
                    shadow_sprites_[i].y = se->y_offset;
                    shadow_sprites_[i].rle.assign(se->pixel_data, se->pixel_data + se->data_size);
                }
                fprintf(stderr, "COMBAT: unit shadows loaded=%d (SHADOW.SYB)\n",
                        std::min<int>(7, sh.count()));
            }
        } else {
            fprintf(stderr, "COMBAT: warning: SHADOW.SYB not found - unit shadows disabled\n");
        }
    }
    return loaded > 0;
}

// ==== Dialogue dimming primitives ====
//    sub_6104A(0)  : dword_128550 = dword_128554 + 0*256  -> selects **row 0** of the shade table
//    sub_5ECC7(1)  : dword_755E2 = sub_5EE2B               -> "fill" = shade fill
//    sub_5EE2B     : each of ebx horizontal pixels becomes `pix = table[pix]`, one step darker (cumulative)
//    sub_5EDDE     : for y1..y2-1 call sub_5EE2B(x1, y, x2-x1) - box dimming
//    sub_6075A     : midpoint ellipse fill; calls sub_5EE2B on every row

// One row of sub_6106C(eax = row, dx = tint, bl = pct) + sub_6098B + sub_609F9(mode 0).
//   dword_128558 = 100 - bl            (percent of the original color kept)
//   sub_6098B  : tint component = pal[tint] * bl / 100               (byte_128563/66/65)
//   sub_609F9  : target = pal[i] * (100-bl) / 100 + tint component + 9 (per channel, 6-bit VGA)
//                -> the palette index whose |dR|+|dG|+|dB| is **first** minimal (cmp ax,cx; jge = strict <)
// The dialogue box builds its table in sub_55B59 with `sub_6106C(0, tint=0, bl=0x3C)`:
// 40% original + 60% black + 9. sub_341C7 also builds a row between steps 7 and 8 with
// `sub_6106C(3, 11h, 46h)`, so this is shared code.
void build_shade_row_original(const uint8_t pal[256][3], int tint, int bl, uint8_t out[256]) {
    const int keep = 100 - bl;
    const int tr = pal[tint][0] * bl / 100;
    const int tg = pal[tint][1] * bl / 100;
    const int tb = pal[tint][2] * bl / 100;
    for (int i = 0; i < 256; i++) {
        const int r = pal[i][0] * keep / 100 + tr + 9;
        const int g = pal[i][1] * keep / 100 + tg + 9;
        const int b = pal[i][2] * keep / 100 + tb + 9;
        int best = 0, best_d = 0x7FFF;
        for (int j = 0; j < 256; j++) {
            const int d = std::abs(r - pal[j][0]) + std::abs(g - pal[j][1]) + std::abs(b - pal[j][2]);
            if (d < best_d) { best_d = d; best = j; }
        }
        out[i] = (uint8_t)best;
    }
}

namespace {

// Shade-table reproduction options; both were verified by running COMBATII.EXE directly.
//  - kDlgExactShadeTable: true uses the sub_6106C(row 0) algorithm, false the legacy
//    approximation (2/3 brightness).
//      * Table generator dword_12854C starts as zeroed BSS, but sub_5F874 at sub_55B59+44 calls
//        `sub_61011(0)` to set sub_609F9(mode 0), and only then does sub_55B59+10A.. build rows 0/1.
//      * Row 0 = sub_6106C(eax=0, edx=tint 0, ebx=0x3C): the `mov ebx,3Ch` survives the
//        sub_6102C -> _nmalloc_ call because _nmalloc_ pushes and pops ebx. All 256 bytes matched
//        the original code for six different palettes.
//  - kDlgEmulateCanvasWrap: the original sub_5EB00 uses a linear (y*pitch + x) address and
//    sub_5EE2B does **no clipping**. Battle drawing targets the offscreen canvas chosen by
//    sub_5F7F5 -> sub_5F804 (0x2EE00 bytes = 480x400, pitch dword_755DA = 0x1E0, origin
//    dword_755D6 = 0xBB80, i.e. canvas column 0, row 100), and sub_482A5 leaves it unchanged.
//    Rows of a wide ellipse therefore run past the right-hand 160px margin into the next or
//    previous row. True reproduces that too, matching the 320x200 frame pixel for pixel when
//    emulating the original sub_482A5.
constexpr bool kDlgExactShadeTable   = true;
constexpr bool kDlgEmulateCanvasWrap = true;
constexpr int  kDlgCanvasPitch       = 0x1E0;

// Legacy approximation (2/3 brightness, nearest in Euclidean RGB); used only when
// kDlgExactShadeTable is false.
void build_shade_row_legacy(const uint8_t pal[256][3], uint8_t out[256]) {
    for (int i = 0; i < 256; i++) {
        const int r = pal[i][0] * 2 / 3, g = pal[i][1] * 2 / 3, b = pal[i][2] * 2 / 3;
        int best = i, best_d = 0x7FFFFFFF;
        for (int j = 0; j < 256; j++) {
            const int dr = pal[j][0] - r, dg = pal[j][1] - g, db = pal[j][2] - b;
            const int d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = j; }
        }
        out[i] = (uint8_t)best;
    }
}

} // namespace

// dword_128550: the original builds the table **once** at startup (sub_55B59), after sub_4ED18
// (map palette load + sub_44F30 for the SCE header weather), in the order `sub_6102C(4)`,
// `sub_6106C(0, 0, 0x3C)`, ... . sub_44F30 copies unk_FDC68 (base) to unk_12824C and then
// multiplies the weather scale (0x50/0x64 etc.) into unk_12824C itself, so the table is based on
// the **weather-adjusted** palette, not battle_palette_. (Chapter 1-1: with snow, whites are
// already scaled by 0.8 when the table is built, so the doubled-dimming color at the box center
// differs from a table built on the unmodified palette. Measured: index 224 -> 4 (one pass) ->
// 5 (two passes), versus 12 from the base palette.)
void CombatII::snapshot_dialog_shade_palette() {
    bool any = false;
    for (int i = 0; i < 256 && !any; i++)
        for (int ch = 0; ch < 3; ch++) if (weather_cur_palette_[i][ch]) { any = true; break; }
    memcpy(dlg_shade_pal_, any ? weather_cur_palette_ : battle_palette_, sizeof(dlg_shade_pal_));
    dlg_shade_pal_set_ = true;
    dlg_shade_valid_   = false;                        // rebuild on next use
    move_shade_valid_  = false;                        // the move-range LUT (row 1) uses the same base, invalidate too
}

const uint8_t* CombatII::dialog_shade_lut() {
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();   // fallback when the load path was skipped
    if (!dlg_shade_valid_) {
        if (kDlgExactShadeTable) build_shade_row_original(dlg_shade_pal_, 0, 0x3C, dlg_shade_lut_);
        else                     build_shade_row_legacy(dlg_shade_pal_, dlg_shade_lut_);
        dlg_shade_valid_ = true;
    }
    return dlg_shade_lut_;
}

// sub_5EE2B: run w horizontal pixels starting at (x, y) through the shade table once
// (`cmp bx,0; jle` ignores width <= 0).
void CombatII::shade_span(int x, int y, int w) {
    if (w <= 0) return;
    uint8_t* buf = video_.get_backbuffer();
    if (!buf) return;
    const uint8_t* lut = dialog_shade_lut();

    if (!kDlgEmulateCanvasWrap) {                      // plain clipping
        if (y < 0 || y >= SCREEN_H) return;
        const int x0 = std::max(x, 0), x1 = std::min(x + w, SCREEN_W);
        for (int px = x0; px < x1; px++) { uint8_t& p = buf[y * SCREEN_W + px]; p = lut[p]; }
        return;
    }

    // Linear address = y*0x1E0 + x. Only (column < 320, row 0..199) is visible; the rest (the
    // right-hand 160px margin and 100 rows above/below) is invisible canvas memory, so writing
    // there cannot affect the screen.
    long lin = (long)y * kDlgCanvasPitch + x;
    while (w > 0) {
        long row = (lin >= 0) ? lin / kDlgCanvasPitch : -((-lin + kDlgCanvasPitch - 1) / kDlgCanvasPitch);
        const int col = (int)(lin - row * kDlgCanvasPitch);
        const int run = std::min(w, kDlgCanvasPitch - col);
        if (row >= 0 && row < SCREEN_H && col < SCREEN_W) {
            const int x1 = std::min(col + run, SCREEN_W);
            uint8_t* line = buf + row * SCREEN_W;
            for (int px = col; px < x1; px++) line[px] = lut[line[px]];
        }
        lin += run;
        w   -= run;
    }
}

// sub_5EDDE(mode 1): dim x..x+w-1, y..y+h-1 row by row (end points exclusive).
void CombatII::render_dialog_dimming(int x, int y, int w, int h) {
    for (int row = 0; row < h; row++) shade_span(x, y + row, w);
}

// sub_6075A (dasm 109277): a **direct** port of the midpoint ellipse fill.
//   Args: eax = cx, edx = cy, ebx = **horizontal** radius (a), ecx = **vertical** radius (b),
//   [stack] = color (irrelevant in shade mode).
//   Region 1 (slope < 1): x (ebp) grows and y (edi) decreases only while the decision variable
//   esi is positive; the top and bottom rows are drawn at that point.
//   Region 2 (slope >= 1): y decreases every row, drawing the top and bottom rows, and x grows
//   only while esi < 0.
//   A row spans (cx-ebp .. cx+ebp-1) = width 2*ebp (`sub eax,ecx` then cwde); sub_5EE2B ignores
//   width 0. Each row is drawn once (no ebx duplication), so no row is dimmed twice.
void CombatII::ellipse_shade_fill(int cx, int cy, int rx, int ry) {
    const int a = (int16_t)rx, b = (int16_t)ry;
    const int a2 = a * a, b2 = b * b;          // var_20, var_24
    const int t14 = 2 * a2, t18 = 2 * b2;      // var_14, var_18
    int edi  = b;                              // current y offset
    int esi  = b2 - a2 * edi + (a2 >> 2);      // decision variable (sar 2: a2 >= 0)
    int var4 = t14 * edi;                      // 2a^2*y
    int var8 = 0;                              // 2b^2*x
    int ebp  = 0;                              // current x offset

    while (var8 < var4) {                                      // loc_607C1
        if (esi > 0) {
            const int x0 = cx - ebp, w = (int16_t)(2 * ebp);
            shade_span(x0, cy - edi, w);
            shade_span(x0, cy + edi, w);
            var4 -= t14; --edi; esi -= var4;
        }
        var8 += t18; ++ebp; esi += b2 + var8;                  // loc_6081F
    }
    // loc_60838: region-switch correction. Not imul/idiv: a /2 that truncates toward zero
    // ((v - sign) >> 1).
    esi += (3 * (a2 - b2)) / 2 - (var8 + var4) / 2;

    while (edi >= 0) {                                         // loc_60867
        const int x0 = cx - ebp, w = (int16_t)(2 * ebp);
        shade_span(x0, cy - edi, w);
        if (cy + edi != cy - edi) shade_span(x0, cy + edi, w); // edi == 0 draws the middle row once
        if (esi < 0) { var8 += t18; ++ebp; esi += var8; }      // loc_608B8
        var4 -= t14; --edi; esi += a2 - var4;                  // loc_608CD
    }
}

// sub_5EC44: fill w horizontal pixels at (x,y) with a solid color (ignores w <= 0, clips to
// screen).
void CombatII::ellipse_solid_span(int x, int y, int w, uint8_t color) {
    if (w <= 0) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    if (y < 0 || y >= SCREEN_H) return;
    const int x0 = std::max(x, 0), x1 = std::min(x + w, SCREEN_W);
    for (int px = x0; px < x1; px++) dest[y * SCREEN_W + px] = color;
}

// sub_6075A in solid mode (sub_5ECC7(0) + sub_5EC44): the same midpoint ellipse, filling rows with
// one color. The sub_45686 gauge disks and rings are all rx == ry circles, but the general ellipse
// path shares this loop anyway.
void CombatII::ellipse_solid_fill(int cx, int cy, int rx, int ry, uint8_t color) {
    const int a = (int16_t)rx, b = (int16_t)ry;
    if (a <= 0 || b <= 0) return;
    const int a2 = a * a, b2 = b * b;
    const int t14 = 2 * a2, t18 = 2 * b2;
    int edi = b;
    int esi = b2 - a2 * edi + (a2 >> 2);
    int var4 = t14 * edi;
    int var8 = 0;
    int ebp = 0;

    while (var8 < var4) {                                      // loc_607C1
        if (esi > 0) {
            const int x0 = cx - ebp, w = (int16_t)(2 * ebp);
            ellipse_solid_span(x0, cy - edi, w, color);
            ellipse_solid_span(x0, cy + edi, w, color);
            var4 -= t14; --edi; esi -= var4;
        }
        var8 += t18; ++ebp; esi += b2 + var8;
    }
    esi += (3 * (a2 - b2)) / 2 - (var8 + var4) / 2;            // loc_60838
    while (edi >= 0) {                                         // loc_60867
        const int x0 = cx - ebp, w = (int16_t)(2 * ebp);
        ellipse_solid_span(x0, cy - edi, w, color);
        if (cy + edi != cy - edi) ellipse_solid_span(x0, cy + edi, w, color);
        if (esi < 0) { var8 += t18; ++ebp; esi += var8; }
        var4 -= t14; --edi; esi += a2 - var4;
    }
}

void CombatII::render_dialog_frame(int x, int y, int w, int h) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    for (int px = x; px < x + w; px++) { if (px>=0&&px<SCREEN_W) { if(y>=0&&y<SCREEN_H) dest[y*SCREEN_W+px]=0x0F; if(y+h-1>=0&&y+h-1<SCREEN_H) dest[(y+h-1)*SCREEN_W+px]=0x0F; } }
    for (int py = y; py < y + h; py++) { if (py>=0&&py<SCREEN_H) { if(x>=0&&x<SCREEN_W) dest[py*SCREEN_W+x]=0x0F; if(x+w-1>=0&&x+w-1<SCREEN_W) dest[py*SCREEN_W+x+w-1]=0x0F; } }
}

void CombatII::free_battle_map() {
    delete[] layer_base_tiles_; layer_base_tiles_ = nullptr;
    delete[] layer_top_tiles_; layer_top_tiles_ = nullptr;
    delete[] layer_passable_; layer_passable_ = nullptr;
    base_tiles_data_.clear();
    top_tile_sprites_.clear();
    map_base_anim_.clear();
    map_top_anim_.clear();
    map_base_anim_frame_.clear();
    map_top_anim_frame_.clear();
    map_w_ = map_h_ = tile_count_ = 0;
}

bool CombatII::load_battle_map(const std::string& map_filename, bool use_sce_unit_records) {
    free_battle_map();
    std::string cand_paths[] = {
        scenario_path(map_filename.c_str()),
        cstldata_path(map_filename.c_str()),
        game_path(map_filename.c_str())
    };
    std::string path;
    for (auto& c : cand_paths) if (file_exists(c.c_str())) { path = c; break; }
    if (path.empty()) {
        std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR)+"/SCENARIO", map_filename);
        if (!alt.empty() && file_exists(alt.c_str())) path = alt;
        else {
            fprintf(stderr, "COMBAT: warning: MAP not found: %s\n", map_filename.c_str());
            return false;
        }
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    uint16_t base_tile_count = 0;
    if (fread(&base_tile_count, 2, 1, fp) != 1) { fclose(fp); return false; }
    fseek(fp, 4, SEEK_CUR);
    base_tiles_data_.resize(base_tile_count);
    for (int i=0;i<base_tile_count;i++){
        base_tiles_data_[i].resize(TOWN_TILE_BYTES);
        if (fread(base_tiles_data_[i].data(),1,TOWN_TILE_BYTES,fp)!=TOWN_TILE_BYTES){ fclose(fp); return false; }
    }
    uint16_t top_tile_count=0;
    if (fread(&top_tile_count,2,1,fp)!=1){ fclose(fp); return false; }
    top_tile_sprites_.resize(top_tile_count);
    for (int i=0;i<top_tile_count;i++){
        uint32_t dsize=0; uint16_t w=0,h=0;
        if (fread(&dsize,4,1,fp)!=1) break;
        fread(&w,2,1,fp); fread(&h,2,1,fp);
        int16_t yoff=0;
        if (h <= 0x10) yoff=0;
        else if (h==0x20) yoff=-16;
        else if (h==0x30) yoff=-32;
        else if (h==0x40) yoff=-48;
        else if (h==0x50) yoff=-64;
        else if (h==0x60) yoff=-80;
        else yoff= -(int16_t)(h-16);
        top_tile_sprites_[i].w=w; top_tile_sprites_[i].h=h; top_tile_sprites_[i].yoff=yoff;
        top_tile_sprites_[i].foot=(uint16_t)(h + yoff -10);
        top_tile_sprites_[i].dsize=dsize;
        top_tile_sprites_[i].data.resize(dsize);
        if (dsize) fread(top_tile_sprites_[i].data.data(),1,dsize,fp);
    }
    uint16_t mw=0,mh=0;
    if (fread(&mw,2,1,fp)!=1){ fclose(fp); return false; }
    if (fread(&mh,2,1,fp)!=1){ fclose(fp); return false; }
    map_w_=mw; map_h_=mh; tile_count_= (uint32_t)map_w_*map_h_;
    if (tile_count_==0 || tile_count_> 300000){ fclose(fp); return false; }
    layer_base_tiles_ = new uint16_t[tile_count_]();
    layer_top_tiles_ = new uint16_t[map_w_ * (map_h_ + 6)]();
    std::fill_n(layer_top_tiles_, map_w_*(map_h_+6), 0xFFFF);
    if (fread(layer_base_tiles_,2,tile_count_,fp)!=tile_count_){ fclose(fp); return false; }
    if (fread(layer_top_tiles_,2,tile_count_,fp)!=tile_count_){ fclose(fp); return false; }
    for (uint32_t i=0;i<tile_count_;i++){
        uint16_t top = layer_top_tiles_[i] & 0x7FFF;
        layer_top_tiles_[i] = (top>0? top-1: 0xFFFF);
        layer_base_tiles_[i]--;
    }
    // Third layer = unk_88BD4 terrain attributes (1 byte/tile), the buffer behind the sub_12F2A
    // foot-sinking test (`cmp al,64h; jbe`).
    // 0..100 = normal terrain, 101+ = sunken terrain ((attr-0x65)&0xF indexes unk_70A58).
    tile_attr_.assign(tile_count_, 0);
    if (fread(tile_attr_.data(), 1, tile_count_, fp) != tile_count_) { fclose(fp); return false; }
    uint8_t pal_raw[768]={0};
    if (fread(pal_raw,1,768,fp)==768){
        for(int i=0;i<256;i++){ battle_palette_[i][0]=pal_raw[i*3]; battle_palette_[i][1]=pal_raw[i*3+1]; battle_palette_[i][2]=pal_raw[i*3+2]; }
    }
    // ---- Trailer (sub_105AD continues past the palette) ----
    // [base_tile_count * 20B stat table][top_tile_count * 20B stat table]
    // [9B weather slots][11B MAP_EFFC][16B embedded FPM name][2B FPM bias]
    // Verified byte-exact against IM05.MAP.
    // The stat tables (unk_7EBAC/unk_7D824, 10 words per tile) are tile animation chains:
    // sub_109EE/sub_10A0F read the word at the current frame counter to pick the tile to draw and
    // sub_10A30 (30-tick ISR) advances the counter. sub_105AD loc_10783/loc_107D6 copies only the
    // first 9 words of each 20B entry and zeroes the counters (unk_839CC/unk_BA6DA); the port keeps
    // all 10 words (the tenth is an out-of-chain flag) and also starts the counters at 0.
    map_trailer_ = MapTrailer{};
    map_base_anim_.clear();
    map_top_anim_.clear();
    map_base_anim_frame_.clear();
    map_top_anim_frame_.clear();
    {
        const long stat_start = ftell(fp);
        bool stat_ok = true;
        if (base_tile_count > 0) {
            map_base_anim_.resize((size_t)base_tile_count * 10);
            for (int i = 0; i < base_tile_count; ++i) {
                uint8_t raw[20];
                if (fread(raw, 1, 20, fp) != 20) { stat_ok = false; break; }
                for (int k = 0; k < 10; ++k)
                    map_base_anim_[(size_t)i * 10 + (size_t)k] =
                        (int16_t)(raw[2 * k] | (raw[2 * k + 1] << 8));
            }
        }
        if (stat_ok && top_tile_count > 0) {
            map_top_anim_.resize((size_t)top_tile_count * 10);
            for (int i = 0; i < top_tile_count; ++i) {
                uint8_t raw[20];
                if (fread(raw, 1, 20, fp) != 20) { stat_ok = false; break; }
                for (int k = 0; k < 10; ++k)
                    map_top_anim_[(size_t)i * 10 + (size_t)k] =
                        (int16_t)(raw[2 * k] | (raw[2 * k + 1] << 8));
            }
        }
        if (stat_ok) {
            map_base_anim_frame_.assign(base_tile_count, 0);
            map_top_anim_frame_.assign(top_tile_count, 0);
        } else {
            // Short file: jump to the end of the stat tables and still try to parse the trailer.
            // If the file is too short the trailer read fails and it stays invalid, as before; the
            // frame tables are cleared so rendering falls back to the raw original indices.
            map_base_anim_.clear();
            map_top_anim_.clear();
            if (stat_start >= 0)
                fseek(fp, stat_start + (long)base_tile_count * 20 + (long)top_tile_count * 20, SEEK_SET);
        }
    }
    {
        uint8_t w9[9] = {};
        if (fread(w9, 1, 9, fp) == 9) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    map_trailer_.weather_slots[i][j] = w9[i*3+j];
            if (fread(map_trailer_.mapeffc, 1, 11, fp) == 11) {
                // sub_105AD loc_1089B: `call eof_; cmp eax,1; jz loc_10922`. The 11 selector
                // bytes are mandatory; the FPM name (16B) and bias (2B) are optional and skipped at
                // EOF (IM01.MAP ends right after the selectors).
                map_trailer_.valid = true;
                char namebuf[16] = {};
                if (fread(namebuf, 1, 16, fp) == 16) {
                    std::string nm(namebuf, strnlen(namebuf, 16));
                    auto dot = nm.find('.');
                    map_trailer_.fpm_base_name = (dot != std::string::npos) ? nm.substr(0, dot) : nm;
                    int16_t bias = -1;
                    if (fread(&bias, 2, 1, fp) == 1) map_trailer_.fpm_bias = bias;
                }
            }
        }
    }
    if (map_trailer_.valid) {
        fprintf(stderr, "COMBAT: trailer weather=[%d,%d,%d][%d,%d,%d][%d,%d,%d] fpm_name=%s fpm_bias=%d\n",
            map_trailer_.weather_slots[0][0], map_trailer_.weather_slots[0][1], map_trailer_.weather_slots[0][2],
            map_trailer_.weather_slots[1][0], map_trailer_.weather_slots[1][1], map_trailer_.weather_slots[1][2],
            map_trailer_.weather_slots[2][0], map_trailer_.weather_slots[2][1], map_trailer_.weather_slots[2][2],
            map_trailer_.fpm_base_name.c_str(), map_trailer_.fpm_bias);
    } else {
        fprintf(stderr, "COMBAT: trailer not parsed for %s (short file or unexpected layout)\n", map_filename.c_str());
    }
    fclose(fp);
    for(int i=0;i<256;i++) palette_set_entry(i,battle_palette_[i][0],battle_palette_[i][1],battle_palette_[i][2]);
    // Overlay genesis.pal first 104 entries as PACK does
    std::string gen = game_path("genesis.pal");
    if (!file_exists(gen.c_str())) gen = game_path("GENESIS.PAL");
    if (file_exists(gen.c_str())){
        FILE* pf=fopen(gen.c_str(),"rb");
        if(pf){ uint8_t sys[312]; if(fread(sys,1,312,pf)==312){ for(int i=0;i<104;i++){ battle_palette_[i][0]=sys[i*3]; battle_palette_[i][1]=sys[i*3+1]; battle_palette_[i][2]=sys[i*3+2]; palette_set_entry(i, sys[i*3], sys[i*3+1], sys[i*3+2]); } } fclose(pf); }
    }
    loaded_map_filename_ = map_filename;
    cam_tile_x_ = (int)map_w_/2 - 6;
    cam_tile_y_ = (int)map_h_/2 - 7;
    if(cam_tile_x_<0) cam_tile_x_=0;
    if(cam_tile_y_<0) cam_tile_y_=0;
    cam_fine_x_=CAM_FINE_X_REST; cam_fine_y_=CAM_FINE_Y_REST;   // resting values (12, 0)
    // Only the static SCE loader can establish a complete occupancy snapshot.
    init_dummy_units(use_sce_unit_records);
    // Weather init after map geometry is known
    load_weather_effects(map_filename);
    // sub_11BB8: battle.syb / wea_effc.syb / chr_effc.syb are loaded by battle init (sub_55B59).
    // The port loads lazily, so preload here: otherwise chr_effc stays empty until the first effect
    // and the sub_123C1 status overlay is never drawn.
    load_effect_sprites();
    // Recentre on the protagonist (first unit of group 0); the original does the same.
    if (!units_.empty()) {
        int lead_x = units_[0].x;
        int lead_y = units_[0].y;
        cam_tile_x_ = lead_x - 6;
        cam_tile_y_ = lead_y - 7;
        if(cam_tile_x_<0) cam_tile_x_=0;
        if(cam_tile_y_<0) cam_tile_y_=0;
        // Original bounds are W-12 / H-12 (sub_111BE, sub_10F5C/sub_10E2F); -13/-14 stop one or two
        // tiles early.
        int max_x = (int)map_w_ -12; if(max_x<0) max_x=0;
        int max_y = (int)map_h_ -12; if(max_y<0) max_y=0;
        if(cam_tile_x_ > max_x) cam_tile_x_=max_x;
        if(cam_tile_y_ > max_y) cam_tile_y_=max_y;
    }
    fprintf(stderr, "COMBAT: Loaded MAP %s %ux%u base=%u top=%zu\n", map_filename.c_str(), map_w_, map_h_, base_tile_count, top_tile_sprites_.size());
    return true;
}

bool CombatII::load_battle_sprites() {
    // NC2 files do not match SPRParser's standard/citizen layout, so fall back to CITIZEN sprites,
    // which are known-good SPR, for the battle sprite Y-sort and blit path.
    if (battle_sprites_loaded_) return true;
    std::string cand = cstldata_path("CITIZEN1.SPR");
    if (!file_exists(cand.c_str())) cand = cstldata_path("CITIZEN2.SPR");
    if (!file_exists(cand.c_str())) cand = game_path("CITIZEN1.SPR");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "CITIZEN1.SPR");
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: CITIZEN fallback not found, battle sprites unavailable\n");
        return false;
    }
    // Load all 16 frames per character (4 directions x 4 poses). CHP's sub_2E533 keeps only the
    // first frame and drops 15, which would make [unit+63h] direction / [unit+64h] pose unusable.
    if (!battle_sprites_.load_citizen_full(cand)) {
        fprintf(stderr, "COMBAT: warning: Failed to load fallback sprites %s: %s\n", cand.c_str(), battle_sprites_.error().c_str());
        return false;
    }
    battle_sprites_loaded_ = true;
    fprintf(stderr, "COMBAT: Battle sprites loaded (fallback CITIZEN full, %d frames = %d chars x 16)\n",
            battle_sprites_.count(), battle_sprites_.count() / 16);
    return true;
}

void CombatII::init_dummy_units(bool parse_sce_snapshot) {
    // Extract real unit placements from the SCE file (sub_4ED18's 4-group 0x88 loop).
    // SCE layout: 0x15-byte preamble, 0x64/0x64, 200B, 168B, 2B, 11B, then var10/varC (W/H) and a
    // W*H*2 zero-filled tile buffer, then 4 groups of (u16 count + count * 0x88 BattleUnit).
    // x/y sit at offset 0 of the 0x88 record for most scenarios and match the MAP W/H; when
    // parsing fails the port falls back to a cross at the map center.
    units_.clear();
    battle_roster_cc990_.clear();                     // dword_CC990: a new battle process starts with an empty roster
    battle_spawn_queue_.clear();                      // sub_5069C queue: cleared when entering a new battlefield
    battle_sce_snapshot_complete_ = false;
    battle_wc1_occupancy_state_known_ = false;
    if (parse_sce_snapshot && !sce_raw_.empty() && sce_raw_.size() > 0x19A) {
        // Var10/varC are at file offsets 0x196/0x198 (mirrors MAP W/H)
        if (sce_raw_.size() >= 0x19A) {
            uint16_t sce_w = sce_raw_[0x196] | (sce_raw_[0x197] << 8);
            uint16_t sce_h = sce_raw_[0x198] | (sce_raw_[0x199] << 8);
            if (sce_w == map_w_ && sce_h == map_h_ && sce_w > 0 && sce_h > 0) {
                size_t tile_size = (size_t)sce_w * sce_h * 2;
                size_t pos = 0x19A + tile_size;
                bool parsed_any = false;
                std::vector<CombatUnitPlacement> parsed;
                bool all_groups_parsed = true;
                bool all_placements_direct = true;
                int groups_parsed = 0;
                for (int grp = 0; grp < 4; ++grp) {
                    if (pos + 2 > sce_raw_.size()) { all_groups_parsed = false; break; }
                    uint16_t cnt = sce_raw_[pos] | (sce_raw_[pos+1] << 8);
                    pos += 2;
                    // Sanity: cnt should be small (0..64). If absurd, abort parsing.
                    if (cnt > 128) { all_groups_parsed = false; break; }
                    bool group_records_parsed = true;
                    for (int i = 0; i < cnt; ++i) {
                        if (pos + 0x88 > sce_raw_.size()) {
                            all_groups_parsed = false;
                            group_records_parsed = false;
                            break;
                        }
                        const uint8_t* u = &sce_raw_[pos];
                        const bool record_linked = sce_unit_record_linked(u);
                        // sub_4ED18 loc_4EFCB: [+0x77] != 0 means one extra 0x88 linked record
                        // follows.
                        // A linked record is an attachment to the preceding unit, not a unit of its
                        // own,
                        // so it is skipped instead of being added to units_
                        // (e.g. 1-4.SCE group 2's last unit: type 316 + linked type 4).
                        const size_t rec_span = sce_unit_record_span(u, sce_raw_.size() - pos);
                        if (rec_span == 0) {
                            all_groups_parsed = false;
                            group_records_parsed = false;
                            break;
                        }
                        if (rec_span > kSceUnitRecSize)
                            fprintf(stderr, "COMBAT: unit record @0x%zX (group %d #%d, id=%d) has linked 0x88 record (+0x77 != 0) - skipped\n",
                                    pos, grp, i, (int)u[0x0F]);
                        int x = u[0];
                        int y = u[1];
                        const bool direct_signed_position = (int8_t)u[0] >= 0 && (int8_t)u[1] >= 0 &&
                            x < (int)map_w_ && y < (int)map_h_ && !record_linked;
                        if (!direct_signed_position) all_placements_direct = false;
                        // A skipped invalid primary record or a linked descriptor changes the
                        // original lists/occupancy beyond units_.
                        if (rec_span != kSceUnitRecSize ||
                            x >= (int)map_w_ || y >= (int)map_h_)
                            all_placements_direct = false;
                        // The name occupies the first bytes as a Johab string, so offset 0 can hold
                        // a
                        // Johab lead byte (0x80+) instead of x/y; the range check filters those. In
                        // 1-1.SCE and similar the bytes at offset 0 are real in-range coordinates
                        // (29,24, ...). When they fall outside the map, offsets 0x40/0x41 are
                        // tried.
                        if (x < 0 || x >= (int)map_w_ || y < 0 || y >= (int)map_h_) {
                            int alt_x = u[0x40];
                            int alt_y = u[0x41];
                            if (alt_x >= 0 && alt_x < (int)map_w_ && alt_y >= 0 && alt_y < (int)map_h_) {
                                x = alt_x; y = alt_y;
                            } else {
                                all_placements_direct = false;
                                pos += rec_span;
                                continue; // skip invalid placement
                            }
                        }
                        CombatUnitPlacement p;
                        p.x = x; p.y = y;
                        if (rec_span > kSceUnitRecSize) p.linked_class = (int)u[kSceUnitRecSize + 0x11];   // sub_174CF: [[+77h]+11h]
                        // ---- Fields read directly from the original unit record (sub_4ED18 /
                        // sub_500ED / loc_4F0D3) ----
                        p.id       = u[0x0F];             // unit ID referenced by scripts
                        p.owner_87 = u[0x87];             // [+87h] owner checked by sub_501BA
                        // [+0Eh] is the raw army byte, kept as an approximation of the effective CC968
                        // list slot: outer groups 1..3 increment it and append at
                        // CC968[(effective value)-1], while group 0 remaps through
                        // sub_4E81F [+87h]/word_FDAF6 with conditional CC990 copies.
                        // Static SCE rejects remapped negative bytes before insertion; remapped 0
                        // becomes memory army 1 / CC968 slot 0, and the helper reuses a CC990
                        // army-1/ID
                        // candidate or adds a [+0Eh]=1 copy only when none exists. The
                        // dynamic-spawn
                        // caller skips that sign check, so remapped FFh can wrap on increment to
                        // CC960.
                        // Effective remap and CC990 membership are not represented in units_.
                        // Measured: 1-1A references army 1 (ids 8, 9) with sel=1 and army 2 (ids 5,
                        // 8)
                        // with sel=2, so the same id 8 appears in two different lists.
                        p.army     = (int)u[0x0E];
                        p.group    = grp;
                        // sub_4ED18: outer groups 1..3 increment [+0x0E] before CC968 insertion.
                        // Group 0 goes through sub_4E81F: BLACKBOX owner / [+87h] remapped by
                        // word_FDAF6.
                        // owner = BLACKBOX [+87h] (not the SCE value); 0/0x63 means the low byte of
                        // FDF85; eff = low byte of FDAF6[owner], and the caller increment makes the
                        // effective value. FDAF6 == 0 means dec 0xFF, i.e. freed and skipped.
                        if (grp >= 1 && grp <= 3) {
                            p.memory_army = (int)battle_incremented_raw_army(p.army);
                        } else if (grp == 0) {
                            int owner = blackbox_owner_for_id(p.id);
                            if (owner < 0) owner = (int)u[0x87];
                            if (owner == 0 || owner == 0x63) owner = (int)fdf85_lo_;
                            const int eff = sce_fdaf6_lookup(owner);
                            if (eff < 0) {
                                p.memory_army = -1;
                            } else if (eff == 0) {
                                // sub_4E81F dec 0xFF -> loc_4F02F jl, free (as at 1134A: no
                                // placement)
                                all_placements_direct = false;
                                pos += rec_span;
                                continue;
                            } else {
                                p.memory_army = eff & 0xFF;
                            }
                        } else {
                            p.memory_army = -1;
                        }
                        p.battle_list_membership_modeled = grp >= 1 && grp <= 3 &&
                            p.memory_army >= 1 && p.memory_army <= 4 && !record_linked &&
                            rec_span == kSceUnitRecSize;
                        // The character type code is the u16 at +0x61: loc_4F02F reads it with
                        // `mov eax,[ebp+5Fh]; sar eax,10h` and passes it to sub_1198D(type), and
                        // sub_12567 tests the same word with `mov bx,[esi+61h]; cmp bx,12Ch`.
                        // Measured: 1-1 G.Scavenger=136 (S_GS), Strider=16 (SCOUT), Sarah=213
                        // (SIREN),
                        //       37-1 Hanjo=199 (HANJO), 1-2 Ranger=14 (RANGER) - the names match.
                        p.sprite_type = (int)u[0x61] | ((int)u[0x62] << 8);
                        p.slot_hint   = u[0x60];          // initial slot recorded in the SCE
                        // The dialogue portrait is [+10h]: sub_482A5 reads `mov bl,[ebp+10h]` and
                        // passes it to sub_48132("portrait.syf", index=bx). PORTRAIT.SYF holds 85
                        // entries (u16 at 0x42) and the 1-1 values 0x2d/0x4b/0x4c are all below
                        // that.
                        p.face     = u[0x10];
                        // The name is a NUL-terminated string at [+02h] (sub_482A5 copies it with
                        // `lea edi,[ebp+2]` and draws it through `sub_5E048(... "%s", name)`).
                        for (int ci = 2; ci < 0x10 && u[ci]; ++ci) p.name.push_back((char)u[ci]);
                        p.team     = (uint8_t)(grp & 1);  // coarse side hint from outer-group parity; not army/list membership
                        // Index into the CITIZEN bank fallback, used only when NC2 cannot be
                        // opened.
                        p.sprite_id = (uint8_t)(p.sprite_type % 222);
                        p.code_a = u[0x3F];
                        p.combat_class = u[0x11];
                        p.combat_flags_14 = u[0x14];
                        p.combat_flags_15 = u[0x15];
                        p.combat_flags_1e = u[0x1E];
                        p.auto_battle = u[0x83];
                        p.ai_decision_state = u[0x6C];
                        p.ai_field_6d = u[0x6D]; p.ai_field_6e = u[0x6E];
                        // sub_4ED18 loc_4F069 zeroes [56h]/[81h]/[84h]/[7Fh] right after loading
                        // (`mov byte ptr [ebp+56h],0` ... `mov al,[ebp+81h]; mov [ebp+7Fh],al`).
                        // The SCE record's own [7Fh]/[81h] values are never used, so they are not
                        // read.
                        p.battle_gauge = 0;                                  // [56h]=0
                        p.ai_field_81 = 0;                                   // [81h]=0
                        p.flags_84 = 0;                                      // [84h]=0
                        p.ai_field_7f = 0;                                   // [7Fh]=[81h]=0
                        p.ai_random_29 = u[0x29]; p.ai_random_2a = u[0x2A];
                        p.ai_home_x = u[0x6F]; p.ai_home_y = u[0x70];
                        if (DevUtil::ai_trace())
                            fprintf(stderr, "COMBAT: id=%u group=%d fields state=%u d=%u e=%u mode7f=%u field80=%u field81=%u mode82=%d patrol=(%u,%u) home=(%u,%u) watch=(%u,%u) target_marker=%u\n",
                                    u[0x0F], grp, u[0x6C], u[0x6D], u[0x6E], u[0x7F], u[0x80], u[0x81],
                                    (int8_t)u[0x82], u[0x29], u[0x2A], u[0x6F], u[0x70], u[0x71], u[0x72],
                                    (unsigned)((u[0x73] | u[0x74] | u[0x75] | u[0x76]) != 0));
                        p.ai_home_x = u[0x6F]; p.ai_home_y = u[0x70];
                        p.ai_watch_sel = u[0x71]; p.ai_watch_id = u[0x72];
                        p.ai_field_80 = u[0x80];
                        p.ai_mode = (int8_t)u[0x82];
                        // A nonzero [+73h] in a static record becomes a tile target in sub_4ED18's
                        // post-pass.
                        p.ai_target_marker = (u[0x73] | u[0x74] | u[0x75] | u[0x76]) != 0;
                        p.code_b = u[0x5C];
                        // sub_4ED18 loc_4F0FF, right after CC990/CC968 insertion:
                        //   `mov bh,[ebp+5Ch]; cmp 5/6/20h -> or byte [ebp+14h],1`.
                        // Transformation and mount forms therefore carry the flying/mounted bit
                        // from
                        // load time, the same bit sub_17E6D sets.
                        if (p.code_b == 5 || p.code_b == 6 || p.code_b == 0x20) {
                            p.combat_flags_14 = (uint8_t)(p.combat_flags_14 | 1);
                            p.flying = true;
                        }
                        apply_record_hp(p, u);           // [+0x48]/[+0x4C] = HP x100 (displayed by sub_13498)
                        // sub_4ED18 loc_4F103: weapon_class is derived from the class/code_b/link
                        // fields read so far.
                        if (weapon_class(p) == 5 && p.ai_mode == 0) p.ai_mode = -1;
                        // ---- Initial facing/pose: an SCE unit record is a 0x88-byte BattleUnit
                        // snapshot ----
                        // Evidence: 1-1.SCE group0 = {id18 type136 +0x63=02, id118 type16 +0x63=03,
                        // id119 type213 +0x63=00}, and G.Scavenger (id18) appears facing up (north,
                        // back) on screen. sub_40295 (unit activation) also stores the record byte
                        // in
                        // the unit with `mov [esi+63h], al` (near loc_407BF), the same offset that
                        // loc_16F57 restores [63h] to when a walk ends.
                        // [0x64] is the initial pose (deploy records are all 0 = standing).
                        p.facing     = (int8_t)(u[0x63] & 3);
                        // [64h] initial pose: the original ignores the SCE +0x64 byte. Unit activation
                        // and the cutscene driver (sub_1620A loc_16253, sub_16D3E loc_16D78,
                        // sub_17071
                        // loc_17096) force pose 2 (+ table unk_71111) for units whose code_b
                        // ([5Ch]) is
                        // in 0Fh..16h and pose 3 for the rest. Pose 3 is the swordless idle frame
                        // of the
                        // NC2 direction block (e.g. S_GS dir2 rec25); using +0x64 (=0) would show
                        // pose 0
                        // (the sword frame, rec22) and differ from the original.
                        p.anim_frame = (u[0x5C] >= 0x0F && u[0x5C] <= 0x16) ? 2 : 3;
                        parsed.push_back(p);
                        pos += rec_span;
                    }
                    if (!group_records_parsed) break;
                    ++groups_parsed;
                }
                // The SCE continues with trigger tables and bytecode after these four unit groups;
                // completeness is about consuming every declared placement, not reaching EOF.
                const bool sce_snapshot_complete = all_groups_parsed && groups_parsed == 4 &&
                    all_placements_direct;
                if (!parsed.empty()) {
                    units_ = std::move(parsed);
                    battle_sce_snapshot_complete_ = sce_snapshot_complete;
                    battle_wc1_occupancy_state_known_ = sce_snapshot_complete;
                    // sub_4ED18 loc_4F163~4F1AD walks CC968 slots 0..3 (effective memory armies
                    // 1..4),
                    // resolves nonzero [+73h] markers via [+6F/+70], then sets [+6F]=FFh.
                    // loc_4F190 initializes every unit in slot 0 (effective [+0Eh]==1). The port
                    // loop
                    // approximates CC968 membership by stored effective army for ordinary outer
                    // groups 1..3;
                    // group-0 remap/copy membership is not represented by units_ and falls back to
                    // raw+1.
                    // loc_4F103's weapon_class==5 && [+82]==0 -> FFh is handled during record
                    // parsing.
                    for (int army = 0; army < 4; ++army) {
                        for (size_t i = 0; i < units_.size(); ++i) {
                            CombatUnitPlacement& unit = units_[i];
                            if (battle_effective_army(unit) != army + 1) continue; // CC968 slot army maps to effective [+0x0E] = army+1; remap fallback remains approximate.
                            if (unit.ai_target_marker) {
                                // sub_4ED18 compares ecx=0 against the [+73h] dword; when nonzero
                                // it
                                // stores the unit occupying the [+6F/+70] coordinates without
                                // excluding
                                // itself. The port looks up active units' base tiles only, so it is
                                // more
                                // limited than the original occupancy grid.
                                unit.ai_target_unit = battle_unit_index_at(unit.ai_home_x, unit.ai_home_y);
                                unit.ai_home_x = 0xFF;
                                unit.ai_target_marker = false;
                            }
                            if (army == 0) {
                                // loc_4F190: applies to every unit in effective CC968 slot 0; the
                                // units_ scan only approximates list membership.
                                unit.ai_field_80 = 0xFF;
                                unit.ai_mode = -1;
                                unit.auto_battle = 0;
                            }
                        }
                    }
                    fprintf(stderr, "COMBAT: Parsed %zu units from SCE (SCE W/H %u/%u)\n", units_.size(), sce_w, sce_h);
                    for (size_t i = 0; i < units_.size() && i < 8; ++i)
                        fprintf(stderr, "COMBAT: unit[%zu] id=%d army=%d group=%d pos=(%d,%d) type=%d(%s) slot=%d face=%d class=%u weapon_class=%d\n",
                                i, units_[i].id, units_[i].army, units_[i].group,
                                units_[i].x, units_[i].y, units_[i].sprite_type,
                                char_file_name_for_type(units_[i].sprite_type), units_[i].slot_hint,
                                units_[i].face, (unsigned)units_[i].combat_class, weapon_class(units_[i]));
                    parsed_any = true;
                }
                if (parsed_any) return;
            }
        }
    }
    if (map_w_==0 || map_h_==0) return;
    int cx = (int)map_w_/2;
    int cy = (int)map_h_/2;
    // Fallback: small cross around center
    const struct {int dx,dy; uint8_t tm;} layout[] = { {0,0,0},{-2,0,0},{2,0,1},{0,-2,0},{0,2,1},{-2,-2,0},{2,2,1} };
    for (auto &e: layout) {
        int x = cx + e.dx, y = cy + e.dy;
        if (x<0||x>=(int)map_w_||y<0||y>=(int)map_h_) continue;
        CombatUnitPlacement u; u.x=x; u.y=y; u.team=e.tm; u.sprite_id=(e.tm==0? 4: 8);
        units_.push_back(u);
    }
}

bool CombatII::decode_fpm_sub10500(const std::vector<uint8_t>& file_data, std::vector<uint8_t>& out_canvas) {
    // Exact port of sub_10500 (dasm/COMBATII_code.ASM 571..648) verified against
    // GENESIS2/COMBATII.EXE bytes 0x16980: 81 FE 00 EE 02 00 (limit 0x2EE00=192000).
    // File layout: header 0x3C bytes ignored, payload = file[0x3C .. file_len-0x300), because
    // sub_10500 does lseek 0x3C + read filelen-0x33C (so last 0x300 bytes not read).
    // Decoding: ebx=src offset, esi=decoded counter, ecx=dst offset (ebx/esi are counters, not
    // pointers).
    //   ah = src[ebx]; edi=ebx+1;
    //   if ah > 0xC0: al=ah-0xC0; ebx=edi; esi+=al; // control: repeat next byte al times (edx=ebx+ebp copies same [edx] al times without increment)
    //   else: al=1; esi++; if ah==0xC0: ebx=edi; // C0 escape: copy next byte once
    //           else: // ah<0xC0 literal: copy ah itself (ebx stays, edx points to ah)
    //   copy loop: edx=ebx+ebp; while al>0: [ecx++] = [edx]; al-- (edx not incremented => same byte repeated for >C0 case, single literal otherwise)
    //   then inc ebx; cmp esi, 0x2EE00 (192000) jbe loop.
    // The result is the full 192,000-byte / 480x400 virtual scrolling canvas (dword_755CC;
    // sub_5F7F5 allocates it with nmalloc(0x2EE00)). sub_5EB00/sub_5EAE3 crop it every frame
    // according to the camera, so the whole canvas has to be kept.
    if (file_data.size() <= 0x33C) return false;
    size_t read_len = file_data.size() - 0x33C;
    if (read_len <= 0x3C) return false;
    // Payload starts at 0x3C
    const uint8_t* src = file_data.data() + 0x3C;
    size_t src_len = read_len - 0x3C; // superseded by the value below
    // The original seeks to 0x3C and reads filelen-0x33C bytes (= read_len) starting there.
    src_len = read_len;
    // Guard: ensure src_len doesn't exceed file_data size-0x3C
    if (src_len > file_data.size() - 0x3C) src_len = file_data.size() - 0x3C;
    constexpr int kLimit = 0x2EE00; // 192000, from CMP ESI,0x0002EE00 at 0x16980
    std::vector<uint8_t> full;
    full.reserve(kLimit + 16);
    size_t ebx = 0;
    int esi = 0;
    while (ebx < src_len && esi <= kLimit) {
        uint8_t ah = src[ebx];
        size_t edi = ebx + 1;
        uint8_t al;
        if (ah > 0xC0) {
            al = (uint8_t)(ah - 0xC0);
            ebx = edi;
            esi += al;
            if (ebx >= src_len) break;
            uint8_t val = src[ebx];
            for (int i=0;i<al;i++) full.push_back(val);
            ebx += 1; // inc ebx after copy
            if (esi > kLimit) break;
        } else {
            al = 1;
            esi += 1;
            if (ah != 0xC0) {
                // Literal: copy ah itself
                full.push_back(ah);
                ebx = edi;
            } else {
                // Escape C0: copy next byte
                ebx = edi;
                if (ebx >= src_len) break;
                full.push_back(src[ebx]);
                ebx += 1;
            }
            if (esi > kLimit) break;
        }
        // Loop continues; original checks esi <= 0x2EE00
    }
    constexpr size_t kCanvasBytes = (size_t)FPM_CANVAS_W * FPM_CANVAS_H; // 480*400 = 192000
    if (full.size() < kCanvasBytes) {
        // Some FPMs may decode short; pad with the last valid row repeated so
        // render_fpm_as_background() still has a full canvas to crop from,
        // rather than silently failing and showing nothing at all.
        if (full.empty()) return false;
        full.resize(kCanvasBytes, full.back());
    }
    out_canvas.assign(full.begin(), full.begin() + kCanvasBytes);
    return true;
}

bool CombatII::load_fpm(const std::string& fpm_filename) {
    std::string path = scenario_path(fpm_filename.c_str());
    if (!file_exists(path.c_str())) path = game_path(fpm_filename.c_str());
    if (!file_exists(path.c_str())) return false;
    FILE* fp=fopen(path.c_str(),"rb");
    if(!fp) return false;
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    fpm_data_.resize(sz);
    fread(fpm_data_.data(),1,sz,fp); fclose(fp);
    // Try exact sub_10500 decode; if succeeds, keep both raw and decoded for render_fpm priority.
    // Some FPMs are small (<64K) and may be raw; decoder requires filelen>0x33C and decodes to
    // >=64K.
    std::vector<uint8_t> decoded;
    if (decode_fpm_sub10500(fpm_data_, decoded)) {
        fpm_decoded_ = std::move(decoded);
        fprintf(stderr, "COMBAT: Loaded FPM %s %ld (decoded %zu via sub_10500, raw %ld kept)\n", fpm_filename.c_str(), sz, fpm_decoded_.size(), sz);
    } else {
        fpm_decoded_.clear();
        fprintf(stderr, "COMBAT: Loaded FPM %s %ld (raw, no decode)\n", fpm_filename.c_str(), sz);
    }
    return true;
}

// ---- sub_4ED18: SCE header (fixed offsets read in order) ----
//   read 2B -> word_FDF80 -> low word of dword_BA83C  = initial camera X tile
//   read 2B -> word_FDF7E -> high word of dword_BA83C = initial camera Y tile
//   read 1B -> dword_BA84A+3 (byte_BA84D) = weather
//   read 1B -> [esp+2Ch+var_4] = time-of-day phase (see below)
//   read 1B -> byte_BA84E = map kind
//   read 1B -> dword_11209E+2
//   read 13B -> map file name inside the SCENARIO folder (e.g. "IM05.MAP")
//   close_ -> sub_105AD(map name) loads map/palette -> unk_FDC68 <- unk_12824C -> sub_44F30
bool CombatII::parse_sce_header(const uint8_t* data, size_t size) {
    if (!data || size < 0x20) return false;
    sce_cam_x_ = (uint16_t)(data[0] | (data[1] << 8));
    sce_cam_y_ = (uint16_t)(data[2] | (data[3] << 8));
    // sub_4ED18: lseek(fd, 0x15); read 2B -> word ptr dword_F0BA0 (ally morale),
    //            read 2B -> word ptr dword_F0BA0+2 (enemy morale). These feed the top HUD gauges.
    if (size >= 0x19) {
        morale_lo_ = (int16_t)(data[0x15] | (data[0x16] << 8));
        morale_hi_ = (int16_t)(data[0x17] | (data[0x18] << 8));
    }
    weather_ = data[4];
    // Third read of sub_4ED18 (`lea edx,[esp+2Ch+var_4]`): sub_4ED18+274 reads it back with
    // `mov ah,[esp+2Ch+var_4]; test ah,ah; jl skip; mov byte_FDF84, ah`.
    // A negative value (0x80..0xFF) keeps the BLACKBOX.DAT time of day; otherwise this is the
    // time-of-day phase (0..15).
    sce_time_byte_ = (int8_t)data[5];
    sce_map_kind_ = data[6];
    sce_deploy_facing_ = data[7];                    // read 1B -> dword_11209E+2
    battle_deploy_zone_parse(data, size);            // read W*H*2 -> E8F46+2 (-1), dword_11209E low
    std::string mapname((const char*)data + 8, 13);
    mapname = mapname.c_str();                       // truncate at NUL
    while(!mapname.empty() && (uint8_t)mapname[0] < 0x20) mapname.erase(0,1);
    // Fallback: when the header field holds no '.', scan the first bytes for a ".MAP" name.
    if (mapname.find('.')==std::string::npos || mapname.size() > 13) {
        std::string s((const char*)data, std::min<size_t>(size, 128));
        size_t pos = std::string::npos;
        for (size_t i=0;i+4<=s.size();i++){
            if (s[i]=='.' && (s[i+1]=='M'||s[i+1]=='m') && (s[i+2]=='A'||s[i+2]=='a') && (s[i+3]=='P'||s[i+3]=='p')){ pos=i; break; }
        }
        if (pos==std::string::npos) return false;
        int start = (int)pos;
        while(start>0){
            char c=s[start-1];
            if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-') start--;
            else break;
        }
        mapname = s.substr(start, pos-start+4);
        mapname.erase(std::find(mapname.begin(), mapname.end(), '\0'), mapname.end());
        while(!mapname.empty() && (uint8_t)mapname[0] < 0x20) mapname.erase(0,1);
        if (mapname.find('.')==std::string::npos) return false;
    }
    map_info_.map_filename = mapname;
    sce_header_parsed_ = true;
    fprintf(stderr, "COMBAT: SCE header cam=(%u,%u) weather=%u kind=%u map='%s'\n",
            sce_cam_x_, sce_cam_y_, weather_, sce_map_kind_, mapname.c_str());
    // sub_4ED18 after lseek 0x15 reads F0BA0(2) + F0BA0+2(2) + FDAF8(0xC8) + FDBC0(0xA8).
    // FDBC0 (file offset 0xE1, 0xA8) is the NUL-terminated Johab scenario condition shown by
    // sub_49411.
    scenario_condition_.clear();
    {
        constexpr size_t kCondOff = 0xE1, kCondLen = 0xA8;
        if (size >= kCondOff + 1) {
            const size_t avail = std::min(kCondLen, size - kCondOff);
            size_t end = 0;
            while (end < avail && data[kCondOff + end] != 0) ++end;
            if (end > 0 && data[kCondOff] != 0)
                scenario_condition_.assign((const char*)data + kCondOff, end);
        }
    }
    if (!scenario_condition_.empty())
        fprintf(stderr, "COMBAT: scenario condition %zu bytes (sub_49411 byte_FDBC0)\n",
                scenario_condition_.size());
    // sub_4ED18 copies FDAF8 (SCE 0x19, 0xC8 = 64 words) into word_FDAF6. FDAF6[0] is BSS zero and
    // FDAF6[i] = SCE word[i-1] for i >= 1; only the low byte is used (byte ptr).
    sce_fdaf6_.assign(0x65, 0);
    if (size >= 0x19 + 0xC8) {
        for (int i = 1; i <= 0x64; ++i) {
            const size_t off = 0x19 + (size_t)(i - 1) * 2;
            sce_fdaf6_[(size_t)i] = (int)(data[off] | (data[off + 1] << 8));
        }
    }
    // Low byte of word_FDF85 (BLACKBOX 0x49D4+0x1F4 = 0x4BC8; used when [+87h] is 0/0x63).
    fdf85_lo_ = 0;
    fdf85_word_ = 0;
    {
        std::string bp = game_path("BLACKBOX.DAT");
        if (!file_exists(bp.c_str())) bp = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "BLACKBOX.DAT");
        FILE *bf = bp.empty() ? nullptr : fopen(bp.c_str(), "rb");
        if (bf) {
            if (fseek(bf, 0x4BC8, SEEK_SET) == 0) {
                uint8_t b[2] = {0, 0};
                if (fread(b, 1, 2, bf) == 2) { fdf85_lo_ = b[0]; fdf85_word_ = (uint16_t)(b[0] | (b[1] << 8)); }
            }
            fclose(bf);
        }
    }
    return true;
}

int CombatII::sce_fdaf6_lookup(int idx) const {
    if (idx < 0 || (size_t)idx >= sce_fdaf6_.size()) return -1;
    return sce_fdaf6_[(size_t)idx] & 0xFF;
}

int CombatII::blackbox_owner_for_id(int id) {
    if (id <= 0 || id > 200) return -1;
    std::string bp = game_path("BLACKBOX.DAT");
    if (!file_exists(bp.c_str())) bp = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "BLACKBOX.DAT");
    if (bp.empty() || !file_exists(bp.c_str())) return -1;
    FILE *bf = fopen(bp.c_str(), "rb");
    if (!bf) return -1;
    const long off = 0x4E26L + (long)(id - 1) * 0x88L + 0x87L;
    if (fseek(bf, off, SEEK_SET) != 0) { fclose(bf); return -1; }
    int c = fgetc(bf);
    fclose(bf);
    return c < 0 ? -1 : (c & 0xFF);
}

// sub_4EA22: for every party slot with word_FDAF6[slot] == 1, read the first BLACKBOX record at
//   0x4E26 + j*0x88 whose [+87h] owner equals that slot and restore it as a battle unit.
//   Forced fields: [0]=FFh, [56h]=FFh, [0Eh]=1. The port replaces [0] with a valid coordinate (the
//   original places the unit again on the deploy screen) and adds it to units_; an ID that is
//   already part of the static SCE placement is skipped.
void CombatII::battle_restore_blackbox_party() {
    std::string bp = game_path("BLACKBOX.DAT");
    if (!file_exists(bp.c_str())) {
        std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/", "BLACKBOX.DAT");
        if (!alt.empty()) bp = alt;
    }
    if (!file_exists(bp.c_str())) return;
    FILE* bf = fopen(bp.c_str(), "rb");
    if (!bf) return;
    int restored = 0;
    for (int slot = 1; slot <= 0x64; ++slot) {
        if (sce_fdaf6_lookup(slot) != 1) continue;            // cmp word_FDAF6[eax*2],1
        for (int j = 0; j < 0x7B; ++j) {                      // cmp si,7Bh
            const long base = 0x4E26L + (long)j * 0x88L;
            if (fseek(bf, base + 0x87L, SEEK_SET) != 0) break;
            const int owner = fgetc(bf);
            if (owner != slot) continue;
            uint8_t rec[0x88];
            if (fseek(bf, base, SEEK_SET) != 0 || fread(rec, 1, sizeof(rec), bf) != sizeof(rec)) break;
            const int id = rec[0x0F];
            bool duplicate = false;
            for (const auto& e : units_) {
                if (e.active && e.id == id && battle_effective_army(e) == 1) { duplicate = true; break; }
            }
            if (duplicate) {
                if (battle_trace_) fprintf(stderr, "COMBAT: BLACKBOX party slot=%d id=%d already present - skip\n", slot, id);
                break;
            }
            CombatUnitPlacement p;
            p.x = rec[0] < map_w_ ? (int)rec[0] : (int)map_w_ / 2;
            p.y = rec[1] < map_h_ ? (int)rec[1] : (int)map_h_ / 2;
            if (battle_deploy_enabled_) { p.x = 0xFF; p.y = rec[1]; }   // mov byte [0],0FFh (placed on the deploy screen)
            p.id = id;
            p.army = 1;
            p.memory_army = 1;                                // mov byte [0Eh],1
            p.group = 1;
            p.battle_list_membership_modeled = true;
            p.sprite_type = (int)(uint16_t)(rec[0x61] | (rec[0x62] << 8));
            p.slot_hint = rec[0x60];
            p.face = rec[0x10];
            p.combat_class = rec[0x11];
            p.code_b = rec[0x5C];
            p.combat_flags_14 = rec[0x14];
            p.combat_flags_15 = rec[0x15];
            p.combat_flags_1e = rec[0x1E];
            p.combat_flags_13 = rec[0x13];
            p.ai_decision_state = rec[0x6C];
            p.ai_field_6d = rec[0x6D]; p.ai_field_6e = rec[0x6E];
            p.ai_field_7f = rec[0x7F]; p.ai_field_81 = rec[0x81];
            p.ai_random_29 = rec[0x29]; p.ai_random_2a = rec[0x2A];
            p.ai_home_x = rec[0x6F]; p.ai_home_y = rec[0x70];
            p.ai_watch_sel = rec[0x71]; p.ai_watch_id = rec[0x72];
            p.ai_field_80 = rec[0x80];
            p.ai_mode = (int8_t)rec[0x82];
            p.facing = (int8_t)(rec[0x63] & 3);
            p.anim_frame = rec[0x64];
            apply_record_hp(p, rec);
            for (int ci = 2; ci < 0x10 && rec[ci]; ++ci) p.name.push_back((char)rec[ci]);
            p.battle_gauge = 0xFF;                            // mov byte [56h],0FFh
            p.active = true;
            p.list_seq = battle_next_list_seq();
            if (battle_deploy_enabled_) {
                // Scenario with deploy zones: the original adds the unit to CC990 only (end of the
                // sub_1134F list) and the deploy phase places it on the map.
                p.active = false;
                p.deploy_roster = true;
                p.battle_list_membership_modeled = false;
                battle_roster_cc990_.push_back(p);
                ++restored;
                if (battle_trace_)
                    fprintf(stderr, "COMBAT: BLACKBOX party slot=%d record=%d id=%d -> CC990 roster only (deploy)\n", slot, j, p.id);
                break;
            }
            units_.push_back(p);
            ++restored;
            if (battle_trace_)
                fprintf(stderr, "COMBAT: BLACKBOX party slot=%d record=%d id=%d hp=%d/%d level=%u class=%u (sub_4EA22)\n",
                        slot, j, p.id, p.hp, p.max_hp, (unsigned)p.combat_level_2b, (unsigned)p.combat_class);
            break;
        }
    }
    fclose(bf);
    if (restored)
        fprintf(stderr, "COMBAT: BLACKBOX party restored %d unit(s) (sub_4EA22)\n", restored);
}

bool CombatII::load_scenario(const std::string& sce_filename) {
    last_dialogue_unit_ = -1;                   // dword_1102B8: reset the previous speaker on a scenario change
    battle_deploy_enabled_ = false; sce_zone_grid_.clear();   // parse_sce_header refills the grid
    battle_deploy_active_ = false;
    std::string fname = sce_filename;
    if (fname.find('.')==std::string::npos) fname += ".SCE";
    current_scenario_name_ = fname;             // scenario identifier used by quick save/load
    std::string path = scenario_path(fname.c_str());
    if (!file_exists(path.c_str())) path = game_path(fname.c_str());
    if (!file_exists(path.c_str())) {
        std::string alt = file_find_case_insensitive(std::string(GAME_DATA_DIR)+"/SCENARIO", fname);
        if (!alt.empty()) path = alt;
    }
    if (!file_exists(path.c_str())){ fprintf(stderr,"COMBAT: warning: SCE not found %s\n", fname.c_str()); return false; }
    FILE* fp=fopen(path.c_str(),"rb");
    if(!fp) return false;
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    sce_raw_.resize(sz);
    fread(sce_raw_.data(),1,sz,fp); fclose(fp);
    fprintf(stderr,"COMBAT: Loaded SCE %s %ld\n", path.c_str(), sz);
    sce_header_parsed_ = false;                 // a failed header read must not inherit the previous camera/weather
    if (!parse_sce_header(sce_raw_.data(), sce_raw_.size())){
        fprintf(stderr,"COMBAT: warning: Failed to parse SCE header for %s\n", path.c_str());
    }
    if (!map_info_.map_filename.empty()){
        if (!load_battle_map(map_info_.map_filename, sce_header_parsed_)){
            fprintf(stderr,"COMBAT: warning: Failed to load battle map %s\n", map_info_.map_filename.c_str());
            std::string base=fname; auto dot=base.find('.'); if(dot!=std::string::npos) base=base.substr(0,dot);
            if (!load_battle_map(base+".MAP", sce_header_parsed_)) return false;
            map_info_.map_filename=base+".MAP";
        }
    } else {
        std::string base=fname; auto dot=base.find('.'); if(dot!=std::string::npos) base=base.substr(0,dot);
        std::string cand=base+".MAP";
        if (!load_battle_map(cand, sce_header_parsed_)){ fprintf(stderr,"COMBAT: warning: Fallback MAP %s not found\n", cand.c_str()); return false; }
        map_info_.map_filename=cand;
    }
    // ---- sub_4ED18 palette/camera post-processing (after sub_105AD) ----
    //  After the map loader fills the palette (unk_12824C), the original snapshots the base palette
    //  (unk_FDC68) and calls sub_44F30 with `dword_BA84A >> 18h` (SCE +0x04 weather). The load fade
    //  shows no change on its first frame, so the port applies the palette immediately.
    reset_weather_palette_base();
    apply_weather_palette(false);
    // sub_6106C(row 0) in sub_55B59 builds the dialogue dimming table from the weather-adjusted
    // unk_12824C produced by sub_44F30 above.
    snapshot_dialog_shade_palette();
    // ---- sub_10420: load MAP_EFFC.SYB (terrain foot overlays, unk_789AC[32]) ----
    load_terrain_overlays(map_info_.map_filename);
    //  SCE +0x00/+0x02 hold the initial camera tile (low/high word of dword_BA83C) and override the
    //  leader-centred value load_battle_map computed.
    //  1-1 uses (23,4); the cutscene shows the mountain range and then moves down to (23,18).
    if (sce_header_parsed_) {
        cam_tile_x_ = sce_cam_x_; cam_tile_y_ = sce_cam_y_;
        // sub_4ED18 leaves the sub-tile fine offset alone, so the resting values (dword_7001C=0Ch,
        // BA838=0) stand.
        cam_fine_x_ = CAM_FINE_X_REST; cam_fine_y_ = CAM_FINE_Y_REST;
        if (cam_tile_x_ < 0) cam_tile_x_ = 0;
        if (cam_tile_y_ < 0) cam_tile_y_ = 0;
        fprintf(stderr, "COMBAT: initial camera from SCE header -> tile (%d,%d)\n", cam_tile_x_, cam_tile_y_);
    }
    cutscene_dialogues_.clear();
    // ---- SCE text extraction ----
    // The original uses a run of 0x0A (0x0A 0x0A) as the separator, mixing "<< ... >>"
    // chapter/region
    // captions (screen-centre overlay) and character lines (bottom box + portrait) in one stream.
    // For example 1-1.SCE 0x1CD8 holds "<< Bifrost Gigantes Mountains >>", a region caption shown
    // as
    // an overlay, and the lines after it use the bottom dialogue box below the HP bar.
    // A line starting with "<<" and ending with ">>" becomes is_overlay=true; every other line is a
    // normal line routed to the bottom box.
    for(size_t i=0;i+4 < sce_raw_.size();){
        if(sce_raw_[i]==0x0A){
            size_t j=i;
            while(j < sce_raw_.size() && sce_raw_[j]==0x0A) j++; // skip consecutive 0x0A
            size_t k=j;
            while(k < sce_raw_.size() && sce_raw_[k]!=0x0A && sce_raw_[k]!=0x00 && k-j<500) k++;
            if(k-j>10){
                std::string txt((char*)&sce_raw_[j], k-j);
                bool has_high=false;
                for(unsigned char c: txt) if(c>=0x80) has_high=true;
                if(has_high && txt.size()>6){
                    while(!txt.empty() && (uint8_t)txt.back()<0x20) txt.pop_back();
                    // Drop leading spaces/control bytes too (captions start with "<<" right after
                    // 0x0A, but a few SCE files pad them).
                    size_t lead=0;
                    while(lead<txt.size() && (uint8_t)txt[lead]<0x20) lead++;
                    while(lead<txt.size() && txt[lead]==' ') lead++;
                    if(lead) txt.erase(0, lead);
                    while(!txt.empty() && txt.back()==' ') txt.pop_back();
                    if(txt.size()>=4) {
                        bool is_overlay = (txt.size()>=4 && txt.rfind("<<",0)==0 && txt.size()>=2 && txt.compare(txt.size()-2,2,">>")==0);
                        // Trailing spaces outside "<< ... >>" carry no meaning.
                        CombatDialogueEntry e; e.text=txt; e.portrait_id=-1; e.is_overlay=is_overlay;
                        cutscene_dialogues_.push_back(e);
                    }
                }
            }
            i=k;
            if(i < sce_raw_.size() && sce_raw_[i]!=0x0A) i++;
        } else i++;
    }
    // Classification log
    {
        size_t ovl_cnt=0;
        for(auto& e: cutscene_dialogues_) if(e.is_overlay) ovl_cnt++;
        fprintf(stderr,"COMBAT: Extracted %zu entries (%zu overlay + %zu dialogue)\n", cutscene_dialogues_.size(), ovl_cnt, cutscene_dialogues_.size()-ovl_cnt);
        for(size_t idx=0; idx<cutscene_dialogues_.size() && idx<6; idx++){
            auto& e=cutscene_dialogues_[idx];
            fprintf(stderr,"COMBAT: [%zu] %s: %s\n", idx, e.is_overlay?"OVERLAY":"DIALOG ", log_text(e.text, 80).c_str());
        }
        if(cutscene_dialogues_.size()>6) fprintf(stderr,"COMBAT: ... +%zu more\n", cutscene_dialogues_.size()-6);
    }
    if(cutscene_dialogues_.empty()){
        CombatDialogueEntry e; e.text="Battle Start!"; e.portrait_id=-1; e.is_overlay=false;
        cutscene_dialogues_.push_back(e);
    }
    // Only character lines get a portrait; overlays have none.
    for (size_t i=0;i<cutscene_dialogues_.size();i++) {
        if(cutscene_dialogues_[i].is_overlay) cutscene_dialogues_[i].portrait_id = -1;
        else cutscene_dialogues_[i].portrait_id = (int)(i % 8);
    }
    // sub_105AD derives the FPM filename from a name embedded in the .MAP's own
    // trailer (e.g. "IM05.PCX" -> "IM05.fpm"), not from the SCE/MAP filename.
    // Try that first (matches the original exactly), then fall back to the old
    // filename-guessing heuristics for maps whose trailer didn't parse.
    bool fpm_ok = false;
    if (map_trailer_.valid && !map_trailer_.fpm_base_name.empty()) {
        fpm_ok = load_fpm(map_trailer_.fpm_base_name + ".FPM");
    }
    if (!fpm_ok) {
        std::string base=fname; auto dot=base.find('.'); if(dot!=std::string::npos) base=base.substr(0,dot);
        std::string fpm_cand=base+".FPM";
        std::string map_base=map_info_.map_filename; auto mdot=map_base.find('.'); if(mdot!=std::string::npos) map_base=map_base.substr(0,mdot);
        if(!load_fpm(fpm_cand)) load_fpm(map_base+".FPM");
    }
    // ---- Script engine load (sub_5306A) ----
    // Loads the 12 slots and the bytecode that follow the SCE body (four unit groups + n*12B
    // table).
    // sub_4E4DD loads FE04F early in sub_4ED18 and consumes tile events at the end, so it has to
    // run
    // before load_script_engine()->battle_tile_load_events().
    battle_load_fe04f();
    battle_restore_blackbox_party();   // sub_4EA22 (restore party slots with word_FDAF6[slot] == 1)
    if (load_script_engine()) {
        dump_script_engine();
    }
    // sub_4FD25 calls sub_19652 (battle state reset) before loading the scenario (sub_4F85C), so no
    // parser-only state is left behind afterwards.
    battle_reset();
    battle_load_time_of_day();
    // Ensure sprites available for cutscene overlay
    load_battle_sprites();
    return true;
}

// Crop a 320x200 window out of the 480x400 virtual FPM canvas at the current camera position and
// blit it to the backbuffer. Port of sub_10AEB's parallax block:
//
//   - dword_BA834's HIGH word (map_trailer_.fpm_bias, loaded 2 bytes at a
//     time straight after the embedded FPM name in the per-map trailer, see
//     load_battle_map()) is compared against 0xFFFF at sub_10AEB+3D; when
//     equal, the function jumps straight to loc_10BE1 (the tile loop) and
//     never calls sub_5EAE3 at all - i.e. the FPM layer is not drawn for
//     that map, full stop. We match that instead of guessing a crop.
//
//   - dword_BA83C is written at sub_10AEB's own entry as
//     {low word = camX arg, high word = camY arg} and every caller (e.g.
//     loc_20597) feeds it right back in - so the eax/dx arguments are just
//     the camera's TILE position, i.e. cam_tile_x_/cam_tile_y_ here.
//
//   - X (loc_10B6D): ebx = camX_tile*2, +1 if dword_7001C (fine/sub-tile X
//     scroll, matches cam_fine_x_) is <= 0. So the FPM scrolls horizontally
//     at HALF the tile grid's rate - a deliberate parallax factor, not an
//     approximation.
//
//   - Y (loc_10BB1): two branches on camY vs fpm_bias:
//       camY <  bias: Y = 150 - 4*(bias - camY) - fine_y/4   (loc_10B6D..10BCA)
//       camY >= bias: Y = camY + 150 - bias                   (loc_10BB1)
//     fine_y is dword_BA838's low word (read via the "dword_BA834+2, sar 16"
//     idiom at sub_10AEB+53); dword_BA838 and dword_7001C are written
//     together as a pair everywhere else in the disassembly (e.g. dasm
//     25449-25450, 26955-26956, 32892-32893), confirming dword_BA838 is the
//     fine/sub-tile Y scroll counterpart to dword_7001C's fine X - i.e.
//     cam_fine_y_ here.
//     Integer division truncates toward zero, matching the sar/sbb sequence.
//
//   - Final source address = unk_8B3E8 (the decoded FPM canvas) + crop_col +
//     Y*480 (sub_10AEB+C4..CE: Y<<4, -Y, <<5 == Y*480). sub_5EAE3 then does a
//     FLAT 96,000-byte copy from there to dword_755CC + dword_755D6 (= the page
//     buffer at row 100, **column 0**; sub_5F804 sets dword_755D6 = 0BB80h = 480*100,
//     dword_755DA = 480, dword_755D0 = dword_755CC). Tiles (sub_5EB00) and the
//     screen blit (sub_5E9E8: esi = dword_755CC + 0BB80h, 200 rows x 320 bytes,
//     stride 480) use exactly the same origin, so screen column x shows FPM canvas
//     column crop_col + x. There is no 80-column margin: the visible window starts
//     at canvas column crop_col (sub_5E9E8), not at the centre of the page.
//     Because the copy is flat (not per row), any canvas column >= 480 wraps into
//     the next canvas row; the port reproduces that with flat indexing.
void CombatII::render_fpm_as_background() {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;

    if (!fpm_decoded_.empty() && fpm_decoded_.size() >= (size_t)FPM_CANVAS_W*FPM_CANVAS_H) {
        if (map_trailer_.valid && map_trailer_.fpm_bias != -1) {
            const int bias = map_trailer_.fpm_bias;

            int crop_col = cam_tile_x_ * 2;
            if (cam_fine_x_ <= 0) crop_col += 1;

            int crop_row;
            if (cam_tile_y_ < bias) {
                crop_row = 150 - 4 * (bias - cam_tile_y_) - (cam_fine_y_ / 4);
            } else {
                crop_row = cam_tile_y_ + 150 - bias;
            }

            // Only crop_row is clamped defensively (the original does not clamp; a valid camera
            // range
            // always lands in 0..200). crop_col is left alone: sub_5EAE3 copies the canvas as one
            // contiguous block, so a column past 480 wraps into the next row.
            crop_row = std::clamp(crop_row, 0, FPM_CANVAS_H - SCREEN_H);

            // Screen (x, y) = canvas flat index crop_row*480 + crop_col + y*480 + x, i.e. the
            // contiguous 96,000-byte copy of sub_5EAE3 followed by the 320-byte col-0 output of
            // sub_5E9E8.
            const size_t canvas_size = (size_t)FPM_CANVAS_W * FPM_CANVAS_H;
            const uint8_t* canvas = fpm_decoded_.data();
            const size_t base = (size_t)crop_row * FPM_CANVAS_W + (size_t)crop_col;
            for (int y = 0; y < SCREEN_H; y++) {
                const size_t row0 = base + (size_t)y * FPM_CANVAS_W;
                uint8_t* d = dest + (size_t)y * SCREEN_W;
                if (row0 + SCREEN_W <= canvas_size) {
                    memcpy(d, canvas + row0, SCREEN_W);
                } else {
                    // Past the end of the canvas the original reads unrelated data after unk_8B3E8.
                    for (int x = 0; x < SCREEN_W; x++) {
                        const size_t idx = row0 + (size_t)x;
                        d[x] = (idx < canvas_size) ? canvas[idx] : 0;
                    }
                }
            }
        }
        // fpm_bias == -1 (or no trailer parsed at all): sub_10AEB skips the
        // FPM layer entirely for this map (jumps to loc_10BE1) - draw
        // nothing here and let the tile loop paint over whatever's already
        // in the backbuffer, same as the original.
        return;
    }

    // Fallback for raw/undecoded FPM data (decode failed): best-effort static blit.
    if (fpm_data_.empty()) return;
    if (fpm_data_.size() >= 64000) {
        size_t off = 0;
        while (off + 64000 <= fpm_data_.size() && fpm_data_[off]==0) off++;
        if (off + 64000 <= fpm_data_.size()) memcpy(dest, fpm_data_.data()+off, 64000);
        else memcpy(dest, fpm_data_.data(), std::min<size_t>(64000, fpm_data_.size()));
    }
}

void CombatII::render_fpm() {
    // Same crop logic as render_fpm_as_background(); kept as a separate entry
    // point for the "no tile layer at all" fallback path in render_battle_map().
    render_fpm_as_background();
}

// G2_DUMP_FRAME support: screen position of the last NC2 sprite drawn.
static struct { int x = -1, y = -1, w = 0, h = 0; const NC2Frame* fr = nullptr; } g_last_nc2_draw;

// ==== Tile animation frames (sub_109EE / sub_10A0F / sub_10A30) ====
//   sub_109EE: after `dec eax` (1-based file id -> 0-based), read the stat word indexed by the
//   frame counter (high byte of unk_839C9). The value is a 0-based pixel index
//   (YOU01 tile 72 chain [71,75,79,75,71,-1,...] = file ids [72,76,80,76,72,-1,...]).
//   sub_10A30: per tile, read [frame] and [frame+1] as a dword; when the high word (next slot) is
//   -1 or the counter is 8 or more, wrap to 0, otherwise advance by one. At the end it toggles
//   BA84A+2 (foot effect phase). An empty table (short file) returns the raw index unchanged.
int CombatII::battle_base_tile_frame(int file_id) const {
    if (file_id <= 0) return -1;
    const size_t i = (size_t)(file_id - 1);
    if (i >= base_tiles_data_.size()) return -1;
    if (map_base_anim_.size() == base_tiles_data_.size() * 10 &&
        map_base_anim_frame_.size() == base_tiles_data_.size()) {
        const int f = (int)map_base_anim_frame_[i];
        if (f >= 0 && f < 10) {
            const int v = (int)map_base_anim_[i * 10 + (size_t)f];
            if (v >= 0 && (size_t)v < base_tiles_data_.size()) return v;
        }
    }
    return (int)i;
}

int CombatII::battle_top_tile_frame(int file_id) const {
    if (file_id <= 0) return -1;
    const size_t i = (size_t)(file_id - 1);
    if (i >= top_tile_sprites_.size()) return -1;
    if (map_top_anim_.size() == top_tile_sprites_.size() * 10 &&
        map_top_anim_frame_.size() == top_tile_sprites_.size()) {
        const int f = (int)map_top_anim_frame_[i];
        if (f >= 0 && f < 10) {
            const int v = (int)map_top_anim_[i * 10 + (size_t)f];
            if (v >= 0 && (size_t)v < top_tile_sprites_.size()) return v;
        }
    }
    return (int)i;
}

void CombatII::battle_tile_anim_advance() {
    // sub_10A30 first half: advance the base tile counters (dx = 0..base_cnt-1).
    const size_t bn = base_tiles_data_.size();
    if (map_base_anim_.size() == bn * 10 && map_base_anim_frame_.size() == bn) {
        for (size_t i = 0; i < bn; ++i) {
            int f = (int)map_base_anim_frame_[i];
            if (f < 0 || f > 8) { map_base_anim_frame_[i] = 0; continue; }
            // High word of the dword load = next slot: e[f+1] (f <= 8, so f+1 <= 9 < 10).
            const int16_t nxt = map_base_anim_[i * 10 + (size_t)f + 1];
            map_base_anim_frame_[i] = (nxt == -1 || f >= 8) ? 0 : (uint8_t)(f + 1);
        }
    }
    // sub_10A30 second half: advance the top tile counters (dx = 0..top_cnt-1, same shape).
    const size_t tn = top_tile_sprites_.size();
    if (map_top_anim_.size() == tn * 10 && map_top_anim_frame_.size() == tn) {
        for (size_t i = 0; i < tn; ++i) {
            int f = (int)map_top_anim_frame_[i];
            if (f < 0 || f > 8) { map_top_anim_frame_[i] = 0; continue; }
            const int16_t nxt = map_top_anim_[i * 10 + (size_t)f + 1];
            map_top_anim_frame_[i] = (nxt == -1 || f >= 8) ? 0 : (uint8_t)(f + 1);
        }
    }
    // sub_10A30 tail: `test ah,ah; setz al` toggles BA84A+2 (foot effect phase).
    // It advances only in the 30-tick ISR, so render_battle_map must not toggle it per frame.
    terrain_anim_frame_ = (terrain_anim_frame_ + 1) & 1;
}

// Shade table row 1 (sub_6104A(1)), used by the move-range shading (sub_1547F -> sub_10130).
// sub_55B59 builds it with `sub_6106C(row=1, tint=1, bl=0x1E)`: 70% original + 30% tint + 9.
// Its base palette is the same snapshot as the dialogue dimming table (weather-adjusted
// unk_12824C).
const uint8_t* CombatII::battle_move_shade_lut() {
    if (!dlg_shade_pal_set_) snapshot_dialog_shade_palette();
    if (!move_shade_valid_) {
        build_shade_row_original(dlg_shade_pal_, 1, 0x1E, move_shade_lut_);
        move_shade_valid_ = true;
    }
    return move_shade_lut_;
}

void CombatII::render_battle_map(){
    highlight_draws_.clear();               // the sub_44DCC replay list is rebuilt on every sub_454B3 pass
    // Always draw FPM behind tiles when available (sub_10AEB order: sub_5EAE3 FPM first, then tiles
    // via sub_10010).
    // The FPM layer is drawn first so gaps between tiles show the background, as in sub_10AEB.
    bool has_fpm = !fpm_decoded_.empty() || !fpm_data_.empty();
    if(!layer_base_tiles_ || base_tiles_data_.empty()){
        if(has_fpm){ render_fpm(); return; }
        video_.clear(); return;
    }
    // Fill background first
    if (has_fpm) render_fpm_as_background();
    else if (!battle_keep_backbuffer_) video_.clear();  // sub_10AEB never clears (battle_fx_world_keep)
    // sub_10AEB (loc_10BE7/loc_10BEE): outer loop dx = 0..0Ch, inner loop si = 0..0Ch, so exactly
    // 13 columns x 13 rows with cam_tile as the first column/row; columns/rows before cam_tile-1
    // are not drawn (at rest with fine=(12,0) the leftmost 16px and top 4px show FPM/background).
    // Tile ids are resolved to the current animation frame by sub_109EE (base) / sub_10A0F (top).
    int start_tx = cam_tile_x_;
    int start_ty = cam_tile_y_;
    int end_tx = cam_tile_x_ + 13;
    int end_ty = cam_tile_y_ + 13;
    for(int ty=start_ty; ty<end_ty; ty++){
        for(int tx=start_tx; tx<end_tx; tx++){
            if(tx<0||tx>=(int)map_w_||ty<0||ty>=(int)map_h_) continue;
            int idx=ty*(int)map_w_+tx;
            uint16_t b_raw=layer_base_tiles_[idx];
            if(b_raw==0xFFFF) continue;             // file id 0 = empty cell (sub_10AEB: test ax,ax; jle)
            // sub_10AEB computes canvas_x = fineX + 4 + (tx-camX)*24, so the sub-tile offset is
            // added (camera world position = cam_tile*24 - cam_fine). The first column/row
            // (cam_x, cam_y) is drawn by sub_5EB00(fineX+4, fineY+4).
            int sx=(tx - cam_tile_x_)*TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
            int sy=(ty - cam_tile_y_)*TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;
            const int b_idx=battle_base_tile_frame((int)b_raw + 1);
            if(b_idx >= 0 && (size_t)b_idx < base_tiles_data_.size()){
                blit_tile_24x16(sx,sy,base_tiles_data_[(size_t)b_idx].data());
            }
        }
    }
    // sub_454B3: sub_10AEB (base) -> [sub_1547F when word_FA8BC != 0] -> sub_12BEE (tops + units).
    // The move-range shading must sit between the base tiles and the tops/units so unit sprites
    // are never dimmed.
    render_player_move_range();
    // Build one merged Y-sorted draw list (top tiles + units); the original uses a stable foot_y
    // sort so units and roofs occlude each other correctly.
    std::vector<DrawEntry> draw_list;
    // sub_46A11 dissolve strip copies. draw_list is blitted at the end of this function, so these
    // frames must live at function scope; inside the unit block they would be destroyed first.
    std::vector<NC2Frame> dissolve_frames;
    dissolve_frames.reserve(units_.size() + 1);
    int top_start_tx = cam_tile_x_ -4;
    int top_start_ty = cam_tile_y_ -6;
    int top_end_tx = cam_tile_x_ + (SCREEN_W/TOWN_TILE_W)+6;
    int top_end_ty = cam_tile_y_ + (SCREEN_H/TOWN_TILE_H)+10;
    for(int ty=top_start_ty; ty<top_end_ty; ty++){
        for(int tx=top_start_tx; tx<top_end_tx; tx++){
            if(tx<0||tx>=(int)map_w_||ty<0||ty>=(int)map_h_) continue;
            uint32_t idx = ty * map_w_ + tx;
            if(idx >= map_w_*(map_h_+6)) continue;
            uint16_t t_idx = layer_top_tiles_[idx];
            if(t_idx==0xFFFF) continue;
            // sub_10A0F: top tiles are resolved to the current animation frame too.
            const int rt_idx = battle_top_tile_frame((int)t_idx + 1);
            if(rt_idx < 0 || (size_t)rt_idx >= top_tile_sprites_.size()) continue;
            auto &sp = top_tile_sprites_[(size_t)rt_idx];
            int sx=(tx - cam_tile_x_)*TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;
            int sy=(ty - cam_tile_y_)*TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y + sp.yoff;
            int right = sx + sp.w;
            int bottom = sy + sp.h;
            if(right <=1 || sx >= 318 || bottom <=1 || sy >= 197 || sp.h >=100) continue;
            int foot_y = ty*TOWN_TILE_H + sp.foot;
            draw_list.push_back({sx,sy,foot_y,0,rt_idx});
        }
    }
    // Append units to same list; foot_y is tile bottom + sprite offset for stable interleaving
    if (!units_.empty()) {
        // The original selects the sprite from three record bytes:
        //   [unit+0x60] type code, [unit+0x63] facing, [unit+0x64] pose
        //   entry = unk_D66A8 + 572*slot + 13*(11*facing + pose), entry[9..12] = sprite pointer
        //   The type code selects `CHARACTE\<name>.NC2` through off_70024/703A8/705A4 and sub_11575
        //   loads 44 records (4 facings x 11 poses); the port uses the NC2 directly. (sub_11E04
        //   reads the occupied tile size from the same (facing, pose) pair, cross-checking it.)
        // CITIZEN sprites are only a fallback for a missing NC2 file:
        //   frame = char*16 + dir_block*4 + pose. The dir block order matches PACK
        //   (sub_1A094/sub_24812), i.e. (down, left, right, up), so original facings
        //   0=south, 1=west, 2=north, 3=east map to {0,1,3,2}.
        //   CITIZEN has only 4 poses per direction (0=neutral, 1/3=walk, 2=duplicate of 0), so the
        //   original pose is shifted down by one: stand 0 -> 0, walk 2/3/4 -> 1/2/3. The table
        //   {2,3,4,3} then cycles 1,2,3,2 = step, neutral, step, neutral, matching CITIZEN's walk.
        static const int kCitizenDirBlock[4] = {0, 1, 3, 2};
        // G2_UNIT_FRAME_TRACE: log one line only when facing/pose change (1-1 walk verification).
        const bool frame_trace = DevUtil::unit_frame_trace();
        static int trace_face[64], trace_pose[64], trace_char[64];
        static bool trace_init = false;
        if (frame_trace && !trace_init) {
            trace_init = true;
            for (int k = 0; k < 64; ++k) { trace_face[k] = -99; trace_pose[k] = -99; trace_char[k] = -99; }
        }
        for (size_t i=0;i<units_.size();i++) {
            auto &u = units_[i];
            // sub_11EE4(x,y,U) removes the unit from the tile occupancy grid (unk_E5210..) in the
            // original. The render queue (sub_12567) is built by walking that grid, so a removed
            // unit
            // does not enter the queue at all from the next frame on (after death sub_463FB or
            // retreat sub_51449). The port walks units_ directly, so active carries the same
            // meaning.
            // During a sub_46A11 dissolve the unit is drawn even though it is no longer active.
            if (!u.active && u.death_dissolve == 0) continue;
            // sub_50216 gate: draw only while the drop counter (E5A50) is in 0..7; while it is < 0
            // (waiting) nothing is drawn, and drop_wait > 0 also hides the normal sprite.
            if (u.drop_wait > 0) continue;
            // Add the [unit+0x65]/[unit+0x66] walk sub-pixel offsets directly (sub_16A6D).
            // sub_12F2A: var_18 = sub_10971+0Ch (+16), var_1C = sub_10988-2 (+2).
            // The x_off/y_off of entry[4]/[5] are added later, in the draw step.
            int sx = (u.x - cam_tile_x_)*TOWN_TILE_W + cam_fine_x_ + UNIT_ANCHOR_X + u.walk_off_x;
            int sy = (u.y - cam_tile_y_)*TOWN_TILE_H + cam_fine_y_ + UNIT_ANCHOR_Y + u.walk_off_y;
            int foot_y = u.y * TOWN_TILE_H + 6 + u.walk_off_y;
            // Units registered with sub_44D89 (dword_F9F08) are drawn a second time by sub_44DCC
            // during 454D5 (above magic effects/dim, below the HUD). Copying the entry here lets
            // the
            // highlight pass replay the same pixels at the same coordinates.
            const bool hl = std::find(battle_highlight_units_.begin(), battle_highlight_units_.end(),
                                      (int)i) != battle_highlight_units_.end();
            auto unit_push = [&](DrawEntry e) {
                if (hl) highlight_draws_.push_back(e);
                draw_list.push_back(e);
            };
            // ---- mode-1 spawn drop (sub_508E6 -> sub_50216) ----
            //   dx = 7 - unk_E5A50 (counter). sub_50216 in full:
            //   (1) dx <= 1: skip the unit sprite (sub_50216+7D: cmp dx,1; jle loc_50319)
            //   (2) fill the NC2 frame (facing, pose 9-dx) with solid color 2+cnt (sub_5F517)
            //   (3) draw the EVENT.SYB sprite of stage table unk_10059C[dx] at the unit tile's
            //       screen position plus the record xo/yo, as sub_5F3FD does: rec7 (floor, yo=+12)
            //       through rec0 (pillar top, yo=-16), so the light pillar rises as dx advances
            //   (4) dx > 7 (counter 8, landed): both are skipped and the normal path draws
            //   Draw order matches the original: unit silhouette first, pillar on top.
            if (u.drop_step >= 0) {
                load_event_syb();
                const int dx = 7 - u.drop_step;          // dx = drop counter
                if (dx <= 1) continue;                    // sub_50216: jl loc_50330 (not drawn)
                static const std::vector<NC2Frame> kEmptySyb;
                const std::vector<NC2Frame>& syb = event_syb_ ? *event_syb_ : kEmptySyb;
                if (u.drop_sil > 0) {
                    const NC2Frame* sf = nc2_frame_for(u.sprite_type, u.facing & 3, 9 - dx);
                    if (sf) unit_push({sx, sy, foot_y, 3, u.drop_sil, sf});
                }
                if ((size_t)u.drop_step < syb.size() && !syb[u.drop_step].pixels.empty()) {
                    const NC2Frame& ds = syb[u.drop_step];
                    unit_push({sx, sy, foot_y, 2, 0, &ds});   // xo/yo are applied by the type-2 draw
                }
                unit_status_overlay_123c1(u, draw_list, sx - u.walk_off_x, sy - u.walk_off_y, foot_y);
                continue;                                 // skip the normal NC2/CITIZEN path
            }
            // ---- sub_12567 (dasm 3751..3796): map_kind 6 flips facing from the unit tile
            // attribute ----
            //   attribute % 100 < 16 -> 0->1, 2->3   (towards the inner diagonal)
            //   otherwise            -> 1->0, 3->2   (loc_126E7)
            //   The original writes [63h] back into the unit, so the port updates the unit too; both
            //   branches are fixed points after one application. The original only updates units
            //   inside its 12x12 scan window, so the port applies this only near the screen.
            if (sce_map_kind_ == 6 && !tile_attr_.empty() &&
                u.x >= 0 && u.y >= 0 && u.x < (int)map_w_ && u.y < (int)map_h_) {
                const int vx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_;
                const int vy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_;
                if (vx > -48 && vx < SCREEN_W + 48 && vy > -48 && vy < SCREEN_H + 48) {
                    const uint8_t attr = tile_attr_[(size_t)u.y * map_w_ + (size_t)u.x];
                    if ((int)attr % 100 < 0x10) {
                        if (u.facing == 0) u.facing = 1;
                        else if (u.facing == 2) u.facing = 3;
                    } else {
                        if (u.facing == 1) u.facing = 0;
                        else if (u.facing == 3) u.facing = 2;
                    }
                }
            }
            // ---- NC2 first (as in the original): entry = 11*facing + pose ----
            const int dir = u.facing & 3;
            const int pose = (int)u.anim_frame;
            // sub_46A11: take the sub_10083 strip of a unit that already left the occupancy grid
            // (the (facing, pose 0) frame pixels), blank the pixels marked in death_mask to 0xFF
            // and
            // draw it like sub_5EE51. entry is facing*143 + pose 0, and 5EE51 applies no
            // shadow/sink/clip.
            if (u.death_dissolve > 0) {
                const NC2Frame* df = nc2_frame_for(u.sprite_type, dir, 0);
                if (df && !df->pixels.empty() && df->w == u.death_w && df->h == u.death_h &&
                    u.death_mask.size() == (size_t)(u.death_w * u.death_h)) {
                    NC2Frame copy = *df;
                    for (size_t k = 0; k < copy.pixels.size(); ++k)
                        if (u.death_mask[k]) copy.pixels[k] = 0xFF;
                    dissolve_frames.push_back(std::move(copy));
                    // sub_5EE51 draws at sub_10971+0Ch+entry[4] / sub_10988-2+entry[5]; the walk
                    // sub-pixel offsets ([65h]/[66h]) are not added here.
                    draw_list.push_back({sx - u.walk_off_x, sy - u.walk_off_y, foot_y, 2, 0,
                                         &dissolve_frames.back(), 0, 0, nullptr});
                }
                continue;
            }
            const NC2Frame* ncf = nc2_frame_for(u.sprite_type, dir, pose);
            if (ncf) {
                if (frame_trace && i < 64 &&
                    (trace_face[i] != u.facing || trace_pose[i] != (int)u.anim_frame ||
                     trace_char[i] != u.sprite_type)) {
                    trace_face[i] = u.facing; trace_pose[i] = (int)u.anim_frame;
                    trace_char[i] = u.sprite_type;
                    fprintf(stderr, "COMBAT: unit[%zu] type=%d face=%d([63h]) pose=%u([64h]) -> entry=%d %dx%d off=(%d,%d) at=(%d,%d) tile=(%d,%d) walkoff=(%d,%d)\n",
                            i, u.sprite_type, u.facing, (unsigned)u.anim_frame, dir*11 + pose,
                            ncf->w, ncf->h, ncf->x_off, ncf->y_off,
                            sx + ncf->x_off, sy + ncf->y_off, u.x, u.y, u.walk_off_x, u.walk_off_y);
                }
                // ---- sub_12F2A recreation ----
                // Unit render y anchor: sy = (ty-cam)*16 + cam_fine_y + 4 (var_1C = sub_10988-2).
                // u65/u66 are the walk sub-pixel offsets [65h]/[66h]. Entry fields:
                // [4] x_off, [5] y_off, [6] shadow, [7] sil_x, [8] sil_y; the sink depth is
                // unk_70A58[(attr-65h)&0Fh].
                //   (1) sprite (common): (sx + x_off + u65, sy + y_off + u66 + depth)
                //   (2) branch (loc_12FDE): type < 12Ch -> loc_12FE4, otherwise loc_13016 when
                //       attr >= 100h. Both paths fall into the shared sink block (loc_1302F), so
                //       class 0 also gets sink/clip/decal on tiles with attr > 100. Entering
                //       loc_1302F sets edx/ebx per class:
                //         class 0 (loc_12FE4): shadow var_C = sx + sil_x,
                //           var_10 = sy + sil_y + 12, clip var_28 = sil_y - y_off + 22 - depth
                //         class 1/2 (loc_13016): shadow var_C = sx - 12,
                //           var_10 = sy + 4, clip var_28 = 14 - y_off - depth
                //       (while sinking var_10/var_C += depth+2, which gives the decal position)
                //   (3) shadow = dword_D6658[entry[6]] (SHADOW.SYB; class 0 hardcodes entry[6]=0,
                //       class 1 reads record +0x0E). Skipped when the attribute high word is -1
                //       (wide) or (attr & 0Fh) == 8 (water). The sink path has no shadow.
                //   (4) decal (loc_13123, sinking only): MAP_EFFC[(terr&0Fh)*2 + BA84A+2], repeated
                //       entry[0] times at 24px steps from (var_C, var_10) =
                //       (sx + sil_x, sy + sil_y + 14 + depth); class 0 uses entry[0] = 1.
                //   Flying ([esi+14h]&1) and map kind >= 4 skip both shadow and sink (jge loc_130DC).
                int shadow_spr = -1;
                bool special_terrain = false;
                int depth = 0;
                const int tile_idx = (u.x >= 0 && u.y >= 0 && u.x < map_w_ && u.y < map_h_)
                                     ? (int)((size_t)u.y * map_w_ + u.x) : -1;
                if (sce_map_kind_ < 4 && !u.flying && tile_idx >= 0 && !tile_attr_.empty()) {
                    const int attr = tile_attr_[(size_t)tile_idx];
                    const bool wide = ((attr >> 4) == 0xFF);   // sar 10h -> 0FFFFFFFh
                    if (!wide && (attr & 0xF) != 8)
                        shadow_spr = ncf->shadow_idx;          // dword_D6658[entry6*3]
                    if (attr > 0x64) {
                        special_terrain = true;
                        depth = TERRAIN_DEPTH[(attr - 0x65) & 0xF];
                    }
                }
                if (!special_terrain) {
                    // Normal path: the sprite position is common (sx/sy already include u65/u66)
                    // and
                    // only the shadow position is class-specific (loc_12FE4). The type-2 push uses
                    // base coordinates; the draw switch adds the offsets.
                    const bool big = (u.sprite_type >= 0x12C);
                    const int sh_x = big ? sx - 12 : sx + ncf->sil_x;
                    const int sh_y = big ? sy + 4 : sy + ncf->sil_y + 12;
                    if (u.death_dissolve == 0 && shadow_spr >= 0 && shadow_spr < 7 &&
                        !shadow_sprites_[shadow_spr].rle.empty())
                        unit_push({sh_x, sh_y, foot_y, 6, shadow_spr,
                                   nullptr, 0, 0,
                                   &shadow_sprites_[shadow_spr]});
                    if (DevUtil::terrain_trace() && i < 64)
                        fprintf(stderr, "COMBAT: unit[%zu] type=%d normal shadow=%d\n",
                                i, u.sprite_type, shadow_spr);
                    unit_push({sx, sy, foot_y, 2, 0, ncf, 0, 0, nullptr});
                    // sub_123C1: the status overlay follows the unit frame immediately (same
                    // foot_y);
                    // the original calls it right after sub_12567 pushes the unit entry.
                    unit_status_overlay_123c1(u, draw_list, sx - u.walk_off_x, sy - u.walk_off_y, foot_y);
                } else {
                    // Sink path: the clip height is class-specific - class 0: sil_y-y_off+22-depth,
                    // class 1/2: 14-y_off-depth (loc_1302F).
                    const bool big = (u.sprite_type >= 0x12C);
                    const int draw_x = sx + ncf->x_off;
                    const int spr_y = sy + ncf->y_off + depth;
                    const int clip_h = big ? 14 - (int)ncf->y_off - depth
                                           : (int)ncf->sil_y - (int)ncf->y_off + 22 - depth;
                    if (DevUtil::terrain_trace() && i < 64)
                        fprintf(stderr, "COMBAT: unit[%zu] type=%d sink tile=(%d,%d) attr=%d depth=%d clip_h=%d "
                                "slot=%d sel=%d tile_w=%d\n",
                                i, u.sprite_type, u.x, u.y,
                                tile_attr_[(size_t)tile_idx], depth, clip_h,
                                (tile_attr_[(size_t)tile_idx] - 0x65) & 0xF,
                                (int)map_trailer_.mapeffc[(tile_attr_[(size_t)tile_idx] - 0x65) & 0xF],
                                ncf->tile_w);

                    if (clip_h > 0)
                        unit_push({draw_x, spr_y, foot_y, 4, 0, ncf, clip_h, 0, nullptr});
                    else
                        unit_push({draw_x, spr_y, foot_y, 2, 0, ncf, 0, 0, nullptr});
                    // Foot decal (loc_13123): x = sx + sil_x, y = sy + sil_y + 14 + depth
                    // (shadow y + depth + 2), repeated entry[0] times at 24px steps.
                    const int terr_idx = (tile_attr_[(size_t)tile_idx] - 0x65) & 0xF;
                    const TerrainOverlayFrame& ov =
                        terrain_overlay_[(size_t)(terr_idx * 2 + terrain_anim_frame_)];
                    if (clip_h > 0 && !ov.rle.empty()) {
                        const int reps = std::max(1, ncf->tile_w);
                        for (int t = 0; t < reps; t++)
                            unit_push({sx + ncf->sil_x + t*0x18,
                                       sy + ncf->sil_y + 14 + depth,
                                       foot_y, 5, 0, nullptr, 0, 0, &ov});
                    }
                    unit_status_overlay_123c1(u, draw_list, sx - u.walk_off_x, sy - u.walk_off_y, foot_y);
                }
                continue;
            }
            if (!battle_sprites_loaded_ || battle_sprites_.count() <= 0) continue;
            const int dir_block = kCitizenDirBlock[u.facing & 3];
            const int cpose = (u.anim_frame > 0) ? ((int)u.anim_frame - 1) : 0;
            int frame = (int)u.sprite_id * 16 + dir_block * 4 + (cpose & 3);
            if (frame >= battle_sprites_.count()) frame %= battle_sprites_.count();
            if (frame_trace && i < 64 &&
                (trace_face[i] != u.facing || trace_pose[i] != (int)u.anim_frame ||
                 trace_char[i] != u.sprite_type)) {
                trace_face[i] = u.facing; trace_pose[i] = u.anim_frame;
                trace_char[i] = u.sprite_type;
                fprintf(stderr, "COMBAT: unit[%zu] char=%u face=%d([63h]) pose=%u([64h]) -> CITIZEN block=%d frame=%d\n",
                        i, (unsigned)u.sprite_id, u.facing, (unsigned)u.anim_frame, dir_block, frame);
            }
            // Use idx=frame; type=1 distinguishes from top tiles during draw. Store frame in idx.
            unit_push({sx,sy,foot_y,1,frame});
            unit_status_overlay_123c1(u, draw_list, sx - u.walk_off_x, sy - u.walk_off_y, foot_y);
        }
    }
    // Single Y-sort (stable), matching PACK's stable_sort on foot_y
    std::stable_sort(draw_list.begin(), draw_list.end(), [](auto& a, auto& b){return a.foot < b.foot;});
    for(auto &e: draw_list) blit_draw_entry(e);
}

// Draw one entry (the original's sub_5F3FD / sub_5F4AF / sub_5F517 call).
// Shared by the Y-sorted pass of render_battle_map and the sub_44DCC highlight replay.
void CombatII::blit_draw_entry(const DrawEntry& e) {
    if(e.type==0){
        auto &sp = top_tile_sprites_[e.idx];
        blit_sprite_rle(e.x, e.y, sp.data.data());
    } else if (e.type==2) {
        // NC2: entry[4]/[5] are the sprite top-left offsets, the same convention as the
        // x_offset/y_offset in a CITIZEN SPR header (measured: CITIZEN 24x24 is (-12,-11) and
        // KNIGHT1 20x23 is (-11,-10), both "horizontally centred, feet at -13").
        const NC2Frame* fr = e.nc2;
        if (!fr) return;
        int draw_x = e.x + fr->x_off;
        int draw_y = e.y + fr->y_off;
        if (draw_x < -64 || draw_x >= SCREEN_W+64 || draw_y < -64 || draw_y >= SCREEN_H+64) return;
        blit_nc2_frame(draw_x, draw_y, *fr);
        g_last_nc2_draw = {draw_x, draw_y, fr->w, fr->h, fr};
    } else if (e.type==3) {
        // sub_5F517: solid silhouette (spawn drop unit); idx carries the color (2 + counter).
        const NC2Frame* fr = e.nc2;
        if (!fr) return;
        int draw_x = e.x + fr->x_off;
        int draw_y = e.y + fr->y_off;
        if (draw_x < -64 || draw_x >= SCREEN_W+64 || draw_y < -64 || draw_y >= SCREEN_H+64) return;
        blit_nc2_frame_solid(draw_x, draw_y, *fr, (uint8_t)e.idx);
    } else if (e.type==4) {
        // sub_12F2A loc_13102: clip height > 0 uses sub_5F4AF (scanlines below the feet omitted).
        const NC2Frame* fr = e.nc2;
        if (!fr) return;
        blit_nc2_frame_clipped(e.x, e.y, *fr, e.clip_h);
    } else if (e.type==5) {
        // sub_12F2A loc_13123: overlay the foot sprite (MAP_EFFC.SYB) on the unit.
        if (e.ov && !e.ov->rle.empty()) {
            blit_sprite_rle(e.x, e.y, e.ov->rle.data());
        }
    } else if (e.type==6) {
        // sub_12F2A normal path: unit foot shadow (SHADOW.SYB, dword_D6658).
        if (e.ov && !e.ov->rle.empty())
            blit_sprite_rle(e.x, e.y, e.ov->rle.data());
    } else {
        const auto* spr = battle_sprites_.get(e.idx);
        if (!spr || !spr->pixel_data) return;
        int draw_x = e.x + spr->x_offset;
        int draw_y = e.y + spr->y_offset;
        if (draw_x < -64 || draw_x >= SCREEN_W+64 || draw_y < -64 || draw_y >= SCREEN_H+64) return;
        blit_sprite_rle(draw_x, draw_y, spr->pixel_data);
    }
}

// ---- sub_44DCC: redraw the units in the dword_F9F08 highlight list ----
//   Called inside 454D5 after sub_12161 and before sub_13546 (above dim/magic effects, below the
//   HUD). The original draws each unit pointer through sub_12F2A again; the port replays the draw
//   entries render_battle_map built for the same units.
void CombatII::render_highlight_units() {
    for (const DrawEntry& e : highlight_draws_) blit_draw_entry(e);
    highlight_draws_.clear();
}

// ---- sub_44D89: add a unit to the highlight list (max 59h entries) ----
void CombatII::battle_highlight_add(int unit_index) {
    if (battle_highlight_units_.size() >= 0x59) return;      // cmp bx,59h; jl
    battle_highlight_units_.push_back(unit_index);
}

// ---- sub_44D81: remove the last entry ----
void CombatII::battle_highlight_pop() {
    if (!battle_highlight_units_.empty()) battle_highlight_units_.pop_back();
}

// ---- sub_44D75: clear the list (word ptr dword_FA8B6+2 = 0) ----
void CombatII::battle_highlight_clear() {
    battle_highlight_units_.clear();
}

// ---- sub_44AB6: load weather.syb into the 72-frame table (dword_F8730) ----
//  The original always loads it during battle init (sub_55B59), independent of map and scenario;
//  whether anything is drawn is decided later by byte_BA84D (weather) in sub_4512E. The 9-byte
//  map-trailer slots are MAP_EFFC decal selectors for sub_5FFB5/sub_601E5 and unrelated to this
// layer.
bool CombatII::load_weather_effects(const std::string& map_filename) {
    (void)map_filename;
    weather_loaded_ = false;
    std::string cand = game_path("WEATHER.SYB");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "WEATHER.SYB");
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: WEATHER.SYB not found, weather layer unavailable\n");
        return false;
    }
    if (!weather_sprites_.load(cand)) {
        fprintf(stderr, "COMBAT: warning: Failed to load WEATHER.SYB: %s\n", weather_sprites_.error().c_str());
        return false;
    }
    weather_loaded_ = true;
    fprintf(stderr, "COMBAT: WEATHER.SYB loaded %d frames (sub_44AB6, 144x96 mask/span stream)\n",
            weather_sprites_.count());
    return true;
}

// ---- sub_4F25F: load frame.syb (HUD frames 14..17) ----
//  The original loads frame.syb twice: 0..13 into unk_F8138 and 14..17 into unk_F82E0 with
//  sub_4F25F(ebx = count to skip, ecx = count to read). A record is 12 bytes {w,h,xo,yo,ptr}, so
//  dword_F82E8/F82F4/F8300/F830C are the data pointers of frames 14/15/16/17. Blitter sub_5F3FD
//  ignores xo/yo, so the coordinates below are plain screen coordinates.
static constexpr int HUD_SPR_TOP = 14, HUD_SPR_BOTTOM = 15, HUD_SPR_DOT_L = 16, HUD_SPR_DOT_R = 17;

bool CombatII::load_hud_frames() {
    hud_frames_tried_ = true;
    hud_frames_loaded_ = false;
    std::string cand = game_path("FRAME.SYB");
    if (!file_exists(cand.c_str())) cand = file_find_case_insensitive(GAME_DATA_DIR, "FRAME.SYB");
    if (cand.empty() || !file_exists(cand.c_str())) {
        fprintf(stderr, "COMBAT: warning: FRAME.SYB not found, HUD frames unavailable (black bars only)\n");
        return false;
    }
    if (!hud_frames_.load(cand) || hud_frames_.count() <= HUD_SPR_DOT_R) {
        fprintf(stderr, "COMBAT: warning: FRAME.SYB load failed or too few sprites (%d)\n", hud_frames_.count());
        return false;
    }
    hud_frames_loaded_ = true;
    fprintf(stderr, "COMBAT: FRAME.SYB loaded %d sprites (HUD = #14..#17)\n", hud_frames_.count());
    return true;
}

// ---- sub_45431 (called by sub_454D5 after sub_4512E) ----
//   call sub_44DF3   top frame + morale gauges
//   call sub_44EB0   bottom frame + time dots
//   sub_5EB4A(0); sub_5EDDE(x 0..16,    y 0..200)   left black bar
//   sub_5EB4A(0); sub_5EDDE(x 304..320, y 0..200)   right black bar
// (sub_5EDDE takes eax=x1, ebx=x2, edx=y1, ecx=y2 and fills the half-open span with sub_5EC44.)
void CombatII::render_hud_frame() {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    if (!hud_frames_tried_) load_hud_frames();

    auto hline = [&](int x, int y, int len, uint8_t col) {          // sub_5EC44 (ignored when len <= 0)
        if (len <= 0 || y < 0 || y >= SCREEN_H) return;
        const int x0 = std::max(x, 0), x1 = std::min(x + len, SCREEN_W);
        for (int px = x0; px < x1; px++) dest[y * SCREEN_W + px] = col;
    };
    auto sprite = [&](int idx, int x, int y) {                       // sub_5F3FD
        if (!hud_frames_loaded_) return;
        const SpriteEntry* s = hud_frames_.get(idx);
        if (s && s->pixel_data) blit_sprite_rle(x, y, s->pixel_data);
    };

    // ---- sub_44DF3: top frame (1,0) + morale gauges ----
    sprite(HUD_SPR_TOP, 1, 0);
    {
        // dword_44A16 = 2C 29 2D | 15 13 17: palette colors of the three rows (y = 0,1,2) per
        // gauge.
        static const uint8_t kGaugeColors[2][3] = { {0x2C, 0x29, 0x2D}, {0x15, 0x13, 0x17} };
        const int morale[2] = { (int)morale_lo_, (int)morale_hi_ };  // word ptr dword_F0BA0 / +2
        for (int i = 0; i < 2; i++) {
            const int len = (int16_t)(morale[i] - 0x28);
            // I=0: x = (0x6E - (v-0x28)) + 0x1E = 180 - v  (right edge fixed at x=140, grows left)
            // i=1: x = 0xAF = 175                          (grows right)
            const int x = (i == 0) ? (int16_t)(0x6E - (morale[i] - 0x28) + 0x1E) : 0xAF;
            for (int k = 0; k < 3; k++) hline(x, k, len, kGaugeColors[i][k]);
        }
    }

    // ---- sub_44EB0: bottom frame (1,196) + time dots ----
    sprite(HUD_SPR_BOTTOM, 1, 0xC4);
    {
        // The original redraws 44EB0 every battle tick, so the 0/1 alternation is invisible. At the
        // port's slower tick (5.85Hz by default) it would blink, so the display value is latched.
        const uint32_t div = hud_time_divisor_ > 0 ? (uint32_t)hud_time_divisor_ : 1u;  // byte_74138
        const int n = (int)(((battle_hud_time_ % div) + 1) >> 1);    // (rem + 1) >> 1
        int xl = 0x9B, xr = 0xAD;                                    // 155 / 173
        for (int k = 0; k < n; k++, xl -= 8, xr += 8) {
            sprite(HUD_SPR_DOT_L, xl, 0xC4);
            sprite(HUD_SPR_DOT_R, xr, 0xC4);
        }
    }

    // ---- Left/right black bars: color 0, full height ----
    for (int y = 0; y < 0xC8 && y < SCREEN_H; y++) {
        hline(0x00, y, 0x10, 0);     // [0, 16)
        hline(0x130, y, 0x10, 0);    // [304, 320)
    }

    // ---- sub_454D5: status icons after sub_45431 (pause etc.) ----
    render_battle_state_overlay();
}// ---- sub_455BD: move the HUD panel to another slot when the mouse covers it ----
//   bx = word ptr dword_75008+2 (panel X), cx = word_7500C (panel Y)
//   bx -= 0x26, cx -= 0x1D -> rectangle (bx, cx) of size (0x4C, 0x3E)
//   (ax, dx) = mouse x/y; bounds are inclusive:
//     bx > ax -> miss / bx+0x4C < ax -> miss / cx > dx -> miss / cx+0x3E < dx -> miss
//   On a hit, pick a slot:
//     dx < 0x5A -> 2: dword_75008+2 = 0xFA, word_7500C = 0x9B (bottom)
//     ax > 0xA0 -> 0: 0x4B, 0x26 (top left)
//     otherwise -> 1: 0xFA, 0x26 (top right, the initial value)
//   The coordinates are rewritten either way and 1 is returned (loc_4562C/4564A/45664 -> 4567C).
//   Called from the four original sites: sub_4A483 destination cursor loop, sub_4D674 entry,
//   ring display loop, and the target cursor loop.
bool CombatII::battle_hud_panel_relocate(int mx, int my) {
    const int left   = battle_gauge_x_ - 0x26;
    const int top    = battle_gauge_y_ - 0x1D;
    if (left > mx) return false;                 // cmp bx, ax ; jg loc_455F9
    if (left + 0x4C < mx) return false;          // add ebx,4Ch ; cmp/jl
    if (top > my) return false;                  // cmp cx, dx ; jg loc_455F9
    if (top + 0x3E < my) return false;           // add ebx,3Eh ; cmp/jge loc_45600
    int slot = 1;
    if (my < 0x5A) slot = 2;                     // cmp dx,5Ah ; jge -> next test
    else if (mx > 0xA0) slot = 0;                // cmp ax,0A0h ; jle -> slot 1
    switch (slot) {
        case 0: battle_gauge_x_ = 0x4B; battle_gauge_y_ = 0x26; break;
        case 2: battle_gauge_x_ = 0xFA; battle_gauge_y_ = 0x9B; break;
        default: battle_gauge_x_ = 0xFA; battle_gauge_y_ = 0x26; break;
    }
    if (battle_trace_)
        fprintf(stderr, "COMBAT: 455BD hud panel hit (%d,%d) slot=%d -> (%d,%d)\n",
                mx, my, slot, battle_gauge_x_, battle_gauge_y_);
    return true;
}

// ---- sub_4ED18: base palette snapshot right after the map/palette load (sub_105AD) ----
//   `unk_FDC68 <- unk_12824C` (300h bytes = 256 colors x 3). It is the reference for the shading,
//   so it must be re-taken on every scenario load or the previous weather would accumulate.
void CombatII::reset_weather_palette_base() {
    memcpy(weather_base_palette_, g_palette, sizeof(weather_base_palette_));
    memcpy(weather_cur_palette_, g_palette, sizeof(weather_cur_palette_));
}

// ---- sub_44F30: weather palette transform + fade ----
//   1) unk_12824C <- unk_FDC68 (working palette = base palette)
//   2) branch on dx (weather): loc_44F71 / loc_44FDE / loc_45046 / loc_450AC
//        0    : no transform
//        1, 2 : c = c * 0x50 / 0x64          (idiv 100, i.e. 80%)
//        3    : c = c * 0x50 / 0x64 + 0x0C
//        4    : c = c * 0x32 / 0x64 + 0x15   (50% + 21)
//        5+   : no transform
//   3) loc_450AC: sub_49A3E(0, 0x100, unk_F9188, unk_12824C, push 2)
//        - sub_5FC21 builds unk_FABE0 = target - current (delta)
//        - for each step i = 0,2,4,...,100, sub_5FD1C writes `base[j] + delta[j]*i/100` for
//          j in [i,256), waiting for vertical retrace per step (sub_5E943), i.e. a ~0.7s fade;
//          the factor is clamped to 0x64.
//      Then unk_F9188 <- unk_12824C and sub_5F9AE(0,0x100,unk_12824C) commits the DAC.
void CombatII::apply_weather_palette(bool animate) {
    uint8_t target[256][3];
    memcpy(target, weather_base_palette_, sizeof(target));
    if (weather_ >= 1 && weather_ <= 4) {
        // loc_44F71: weather 1/2 use x0x50/0x64 only (no add)
        // loc_44FDE: weather 3 uses x0x50/0x64 + 0x0C
        // loc_45046: weather 4 uses x0x32/0x64 + 0x15
        const int num = (weather_ == 4) ? 0x32 : 0x50;
        const int add = (weather_ <= 2) ? 0x00 : (weather_ == 4 ? 0x15 : 0x0C);
        for (int i = 0; i < 256; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                int v = ((int)target[i][ch] * num) / 100 + add; // idiv 64h (truncates toward 0)
                target[i][ch] = (uint8_t)std::clamp(v, 0, 63);
            }
        }
    }
    // Diagnostic log: how many entries change, plus a representative sample (index 1 = snow color)
    {
        int n_changed = 0;
        for (int i = 0; i < 256; ++i)
            if (memcmp(target[i], weather_base_palette_[i], 3) != 0) ++n_changed;
        fprintf(stderr, "COMBAT: weather palette: weather=%u -> %d/256 entries converted "
                        "(sample [1] %u,%u,%u -> %u,%u,%u)\n",
                weather_, n_changed,
                weather_base_palette_[1][0], weather_base_palette_[1][1], weather_base_palette_[1][2],
                target[1][0], target[1][1], target[1][2]);
    }
    int delta[256][3];
    for (int i = 0; i < 256; ++i)
        for (int ch = 0; ch < 3; ++ch)
            delta[i][ch] = (int)target[i][ch] - (int)weather_cur_palette_[i][ch];
    const bool changed = memcmp(target, weather_cur_palette_, sizeof(target)) != 0;
    if (animate && changed && !auto_advance_) {
        // 51 steps x one vertical retrace each = about 0.7s. As in the original, low indices reach
        // the target later, which produces the palette ramp.
        for (int f = 0; f <= 100; f += 2) {
            for (int i = 0; i < 256; ++i) {
                uint8_t v[3];
                for (int ch = 0; ch < 3; ++ch) {
                    int factor = (i < f) ? f : i;            // result of the [i,256) range write
                    if (factor > 100) factor = 100;          // 64h clamp inside sub_5FD1C
                    int base = weather_cur_palette_[i][ch];
                    v[ch] = (uint8_t)std::clamp(base + delta[i][ch] * factor / 100, 0, 63);
                }
                palette_set_entry((uint8_t)i, v[0], v[1], v[2]);
            }
            video_.present();
            SDL_Delay(14);                                   // sub_5E943 (vertical retrace)
        }
    }
    // sub_5F9AE(0, 0x100, unk_12824C): commit the final palette to the DAC.
    memcpy(weather_cur_palette_, target, sizeof(target));
    for (int i = 0; i < 256; ++i)
        palette_set_entry((uint8_t)i, target[i][0], target[i][1], target[i][2]);
}

// ---- sub_450F9: overlay phase counter ----
//   `mov edx, dword ptr unk_FA8AE; sar edx,10h` reads word ptr dword_FA8B0, adds 1, takes the
//   remainder modulo 9 and stores it back into word ptr dword_FA8B0.
//   The ISR (loc_101F4) calls it twice every 15 ticks (0x0F and 0x1E), i.e. +2 steps per 30 ticks.
void CombatII::weather_tick_phase() {
    weather_phase_ = (uint16_t)((weather_phase_ + 1) % 9);
}

// ---- sub_4512E: weather overlay ----
//   A weather.syb frame measures 196 chunks and 135 payload bytes (chunk = {skip:u16, dc:u8,
//   bc:u8} + dc*4+bc bytes, all values 01h), and the last chunk lands at position 30544
//   (= 95.45 rows of 320), so the mask is a sparse dot pattern of 320 columns x 96 rows (the
//   144x96 header only sets the 144px column spacing). The original overlays it as 3 columns
//   (x, +144, +288) x 2 rows (y, y+96) and advances the pointer without clipping, so dots past
//   the screen wrap at the end of the buffer.
//
//   The two row positions (loc_451E6..loc_45251, verified): with edi = 16*(camY%12) - fineY,
//     0 < edi < 96  -> yA = 4-edi,   yB = 100-edi   (loc_4523A path)
//     edi > 96      -> yA = 100-edi, yB = 196-edi   (loc_45228 path)
//     edi == 0 / 96 -> yA = 4,       yB = 100       (equivalent loc_453A6 path)
//   In all three cases yB = yA + 96 and yA = 4 - (edi % 96).
//
//   Row counts: sub_5F42D takes ecx as the row count (edi / edi-96) while sub_5F3FD has none,
//   but once the count is exhausted control falls into loc_5F491 (unlimited) and keeps drawing
//   the same stream, so both passes produce identical pixels. The port therefore draws the whole
//   mask in both passes.
void CombatII::render_weather_layer() {
    // sub_4512E head: `mov al,[BA84A+3]; dec al; cmp al,3; ja loc_45427` - only weather 1..4 draws.
    if (!weather_layer_active() || weather_sprites_.count() <= 0) return;
    if (weather_ < 1 || weather_ > 4) return;
    const int nframes = weather_sprites_.count();
    // var_1C = phase*2 + (weather-1)*18  (off_4511E table: +0, +0x12, +0x24, +0x36)
    int A = (int)weather_phase_ * 2 + 18 * ((int)weather_ - 1);
    int frame1 = A;       // var_1C - columns 1 and 3
    int frame2 = A;       // var_C  - column 2
    // Column x (loc_45166/loc_451D8): X = 24*(camX%12) - fineX + 12.
    //   X >= 0x90: X = 0x90 - X (outer columns = A+1, middle = A);
    //   otherwise: X = -X (outer columns = A, middle = A+1). Then loc_451E6 does `add ebx,10h`.
    int x0 = 24 * (cam_tile_x_ % 12) - cam_fine_x_ + 12;
    int xstart;
    if (x0 >= 144) { xstart = 144 - x0; frame1 = A + 1; }
    else           { xstart = -x0;      frame2 = A + 1; }
    xstart += 16;
    // Two rows (loc_451E6..loc_45251): yA = 4 - (edi % 96), yB = yA + 96.
    const int edi = 16 * (cam_tile_y_ % 12) - cam_fine_y_;
    const int ystart = 4 - (edi % 96);
    const SpriteEntry* f1 = weather_sprites_.get(frame1 % nframes);
    const SpriteEntry* f2 = weather_sprites_.get(frame2 % nframes);
    if (!f1 || !f1->pixel_data) return;
    if (!f2 || !f2->pixel_data) f2 = f1;
    // Dev hook: log position/frames only when the phase changes (G2_WEATHER_TRACE=1).
    if (DevUtil::weather_trace()) {
        static int last = -1, last_y0 = 9999;
        if (last != (int)weather_phase_ || last_y0 != ystart) {
            last = (int)weather_phase_;
            last_y0 = ystart;
            fprintf(stderr, "COMBAT: weather layer phase=%u A=%d frames=%d/%d x=%d..%d y=(%d,%d) cam=(%d,%d) fine=(%d,%d) edi=%d\n",
                    weather_phase_, A, frame1 % nframes, frame2 % nframes,
                    xstart, xstart + 288, ystart, ystart + 96, cam_tile_x_, cam_tile_y_,
                    cam_fine_x_, cam_fine_y_, edi);
        }
    }
    const int xs[3] = { xstart, xstart + 144, xstart + 288 };
    // Original order: the three yA columns first (sub_5F42D), then the three yB columns
    // (sub_5F3FD).
    for (int col = 0; col < 3; ++col) {
        const SpriteEntry* s = (col == 1 ? f2 : f1);
        blit_weather_mask(xs[col], ystart, s->pixel_data, s->data_size);
    }
    for (int col = 0; col < 3; ++col) {
        const SpriteEntry* s = (col == 1 ? f2 : f1);
        blit_weather_mask(xs[col], ystart + 96, s->pixel_data, s->data_size);
    }
}

// sub_5F3FD / sub_5F42D / sub_5F4AF: span-stream blit.
//   `lodsd` reads (skip:u16, dword count:u8, byte count:u8), advances the destination pointer by
//   skip and copies dword count*4 + byte count bytes verbatim.
//   The destination is sub_5EB00 = base + (y * pitch with an unsigned mul) + x + dword_755D6,
//   where base is a segment address (the battle buffer is the dword_755CC heap, cutscenes/scroll
//   use 0xA0000 -> segment 0xA000, a 64K window). Offsets therefore wrap at 16 bits, and dots
//   above the screen (y < 0) are not clipped: they wrap around the segment end and land below.
//   The port reproduces the 16-bit rule: (pos + b) & 0xFFFF, written only when it falls inside
//   the 320x200 = 64,000-byte buffer; 64,000..65,535 is segment tail and is dropped.
void CombatII::blit_weather_mask(int x, int y, const uint8_t* mask, int data_size) {
    if (!mask) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    const uint16_t chunks = (uint16_t)(mask[0] | (mask[1] << 8));
    const uint8_t* p = mask + 2;
    const long total = (long)SCREEN_W * SCREEN_H;
    long pos = (long)y * SCREEN_W + x;
    for (uint16_t i = 0; i < chunks; ++i) {
        const uint16_t skip = (uint16_t)(p[0] | (p[1] << 8));
        const uint8_t dc = p[2], bc = p[3];
        p += 4;
        pos += skip;
        const int n = dc * 4 + bc;
        if (data_size > 0 && (int)(p - mask) + n > data_size) break; // guard not present in the original
        for (int b = 0; b < n; ++b) {
            const long d = (long)((uint32_t)(pos + b) & 0xFFFFu);
            if (d < total) dest[d] = p[b];
        }
        p += n;
        pos += n;
    }
}

void CombatII::render_sprites(){
    // draw_battle_map already drew the units; add only the current movement/attack cue.
    // (The weather layer is handled earlier by render_battle_frame/render_weather_layer.)
    render_player_move_overlay();
    if (units_.empty()) {
        uint8_t* dest=video_.get_backbuffer();
        if(!dest) return;
        int cx=SCREEN_W/2 -6;
        int cy=SCREEN_H/2 -6;
        for(int y=0;y<12;y++) for(int x=0;x<12;x++){
            int dx=cx+x, dy=cy+y;
            if(dx>=0&&dx<SCREEN_W&&dy>=0&&dy<SCREEN_H){
                dest[dy*SCREEN_W+dx]= (x==0||y==0||x==11||y==11)? 0x0F : 0x2A;
            }
        }
    }
}

// ==== A4: status overlays (sub_12065 idle pose, sub_123C1 chr_effc overlay) ====

// sub_12065 (dasm 3186..3258): toggle the idle pose of battle units and advance global phases.
// Walks the four army lists (CC968) and, when a condition below holds, sets [64h] = ([64h]+1) % 2
// to alternate 0/1 (standing/breathing, 2 frames).
//   - map_kind 5/6 and pose < 2                                    (loc_12096)
//   - flying unit ([14h]&1) and pose < 2                           (loc_1209C)
//   - otherwise: [+56h]==FFh (ready to act) and
//       ((type < 12Ch and [5Ch]==0) or [5Ch] in 27h..32h) and pose < 2   (loc_120A8..)
//     Type >= 12Ch jumps to loc_120BC and still requires class 27h..32h (measured: type=12Ch,
//     class=5 does not toggle).
// The tail (loc_120E8) advances byte_E518E (+1 & 3) and dword_E4FB6+2 ((+1)%3); the latter is the
// chr_effc.syb 3-frame phase used by sub_123C1.
void CombatII::battle_unit_idle_pose_step() {
    for (auto& u : units_) {
        if (!u.active) continue;                                        // not in the CC968 lists
        bool bob = false;
        if ((sce_map_kind_ == 6 || sce_map_kind_ == 5) && u.anim_frame < 2) bob = true;
        if (!bob && u.flying && u.anim_frame < 2) bob = true;
        if (!bob && u.battle_gauge == 0xFF) {
            const bool class_ok = (u.sprite_type < 0x12C && u.code_b == 0) ||
                                  (u.code_b >= 0x27 && u.code_b <= 0x32);
            if (class_ok && u.anim_frame < 2) bob = true;
        }
        if (bob) u.anim_frame = (uint8_t)((u.anim_frame + 1) % 2);      // idiv 2 -> dl
    }
    ai_marker_phase_ = (uint8_t)((ai_marker_phase_ + 1) & 3);           // inc byte_E518E; and 3
    effc_phase_ = (uint8_t)((effc_phase_ + 1) % 3);                     // dword_E4FB6+2 = (phase+1)%3
}

// sub_123C1 (dasm 3483..3655): unit status overlay (chr_effc.syb, 0x24 frames).
// Called right after sub_12567 queues the unit frame. Depending on the status byte [+1Eh] it
// selects up to six 12-byte records {w, h, xo(word), yo(word), ptr} and draws them at
// (unit position + record offset + walk sub-pixel [65h]/[66h]).
//   group 0: [+1Eh]&7   -> idx = phase + 3*((v&7)>>1)   (status 1..7, 3-frame sets)
//   group 1: [+1Eh]&8   -> phase + 9
//   group 2: [+1Eh]&10h -> phase + 0Ch
//   group 3: [+1Eh]&20h -> phase + 0Fh
//   group 4: [+1Eh]&80h -> phase + 12h
//   group 5: [+15h]&80h -> phase + 1Eh
// When [+1Eh]==0 only group 5 is tested, otherwise all of 0..5 (loc_123E5).
// Flying units add -0x18 to y (var_8, loc_123DF).
// The original's arg_4 clip (entry[4] = arg_4 - y) is 0 for every known caller, so it is omitted.
void CombatII::unit_status_overlay_123c1(const CombatUnitPlacement& u, std::vector<DrawEntry>& out,
                                        int base_x, int base_y, int foot) {
    if (chr_effc_.empty()) return;
    const uint8_t status = u.combat_flags_1e;
    static constexpr uint8_t kGroupBit[6] = {0x07, 0x08, 0x10, 0x20, 0x80, 0x00};
    static constexpr int     kGroupOff[6] = {0, 9, 0x0C, 0x0F, 0x12, 0x1E};
    const int y_adj = u.flying ? -0x18 : 0;                             // test [14h],1
    int start = 0;
    if (status == 0) {
        if ((u.combat_flags_15 & 0x80) == 0) return;                    // nothing to draw
        start = 5;
    }
    for (int g = start; g < 6; ++g) {
        int idx;
        if (g == 0) {
            const int v = status & 7;
            if (v == 0) continue;                                       // loc_12420 test al,7
            idx = (int)effc_phase_ + 3 * (v >> 1);                       // sar 1 -> lea ebp,[eax+eax*2]
        } else if (g == 5) {
            if ((u.combat_flags_15 & 0x80) == 0) continue;              // loc_12492 test [15h],80h
            idx = (int)effc_phase_ + kGroupOff[5];
        } else {
            if ((status & kGroupBit[g]) == 0) continue;
            idx = (int)effc_phase_ + kGroupOff[g];
        }
        if (idx < 0 || (size_t)idx >= chr_effc_.size()) continue;
        const TerrainOverlayFrame& fr = chr_effc_[(size_t)idx];
        if (fr.rle.empty()) continue;
        DrawEntry e;
        e.x = base_x + fr.x + u.walk_off_x;                              // + [65h]
        e.y = base_y + fr.y + u.walk_off_y + y_adj;                      // + [66h] + var_8
        e.foot = foot;
        e.type = 5;                                                      // RLE overlay blit
        e.ov = &fr;
        out.push_back(e);
    }
}

void CombatII::draw_text_wrapped(int x, int y, const std::string& text) {
    // Port of sub_5DD5B(ebx = string, eax = x, dx = y). The global dword_127646 holds x in its low
    // word and y in its high word, and var_18 keeps the initial x (the left margin).
    //   '\n' (0Ah):        x = var_18 (initial x), y += word_755C6 (= 0x10)   (loc_5DF52)
    //   2-byte (Johab >7Fh): when x > 130h and word_755C8, x = 0 and y += 10h, then draw the glyph
    //                        and advance x by word_755C4 (= 16)                 (loc_5DDE1)
    //   1-byte (<= 7Fh):     when x > 138h and word_755C8, x = 0 and y += 10h, then draw the glyph
    //                        and advance x by unk_755C2>>1 (= 8)                 (loc_5DF7A)
    // A line therefore starts at the x where it began, not at the x where it wrapped.
    // Text length ends at NUL as in the original (`repne scasb` measures it, then loops that far).
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    font_.set_text_mode(4, 0x1000, 0xFF);
    const int margin = x;
    int cx = x, cy = y;
    int line_x = x;   // x at which the current line started
    std::string line;
    auto flush = [&]() {
        if (!line.empty()) {
            if (DevUtil::text_trace())
                fprintf(stderr, "COMBAT: line at (%d,%d): %.40s\n", line_x, cy, line.c_str());
            font_.draw_string(dest, SCREEN_W, SCREEN_H, line_x, cy, line.c_str());
        }
        line.clear();
    };
    for (size_t i = 0; i < text.size();) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0) break;
        if (c == '\n') { flush(); cx = margin; line_x = margin; cy += 0x10; ++i; continue; }
        if (c == '\r') { ++i; continue; }
        bool han = (c >= 0x80);
        int step = (han && i + 1 < text.size()) ? 2 : 1;
        if (cx > (han ? 0x130 : 0x138)) { // loc_5DDE1(2B) / loc_5DF7A(1B)
            flush();
            cx = 0; line_x = 0;           // an automatic wrap resets x to 0
            cy += 0x10;
        }
        line.append(text, i, (size_t)step);
        cx += han ? 16 : 8;
        i += (size_t)step;
    }
    flush();
}

// sub_5046B -> `sub_5E048(dword_110250, x=0x20, y=0x24)`: draw the window text only, at (32,36).
// This window has no box or dimming in the original; it just consumes queued windows one per frame.
void CombatII::show_overlay_message(const std::string& text) {
    draw_text_wrapped(0x20, 0x24, text);
}

// ==== Speaker selection frame (sub_49B16 / sub_49FE1 / sub_4A106 / sub_49CB1 / sub_13498) ====
//
//  Original flow (sub_482A5 -> sub_1124A):
//    sub_4A321   dword_FB1BC (frame target unit) = 0
//    sub_49FE1   store the unit on the speaker tile in dword_FB1BC, compute outer size/offset,
//                clear the visible flag
//    sub_454B3   draw the world (+HUD)
//    sub_4A106   convert to screen coordinates -> sub_49CB1 (corner ornament) -> sub_13498 (HP)
//  sub_482A5 draws this combination once at (a) entry (end of sub_1124A), (b) the first frame of
//  the circle effect, (c) every iteration of the circle loop, and (d) the final layer (loc_4841C,
//  before the dimming window). The border color cycles 9 -> 10 -> 11 on each draw, so the frame
//  blinks during the effect. The port redraws the world every frame, so the original's background
//  save/restore (sub_5EECF / sub_4A210) is unnecessary.

namespace {
// sub_5F3FD chunk stream: u16 chunk count + [u16 skip][u8 dword_count][u8 byte_count] + payload.
// The first four MAIN.CUR entries (mouse cursors) use linear offsets on a 320-wide canvas (skip is
// a multiple of 320). The original patches the 320->480 stride with sub_10031 and draws into a
// 480-wide buffer; the port's backbuffer is 320 wide, so the file coordinates are used unchanged.
bool decode_main_cur_cursor(const uint8_t* data, size_t size, int w, int h, CursorFrame& out) {
    if (!data || size < 2 || w <= 0 || h <= 0 || w > SCREEN_W || h > SCREEN_H) return false;
    const uint16_t chunks = (uint16_t)(data[0] | (data[1] << 8));
    out = CursorFrame{};
    out.width = w;
    out.height = h;
    out.pixels.assign((size_t)w * (size_t)h, 0xFF);   // 0xFF = transparent
    size_t p = 2;
    size_t off = 0;
    for (uint16_t c = 0; c < chunks; ++c) {
        if (p + 4 > size) return false;
        const size_t skip = (size_t)(data[p] | (data[p + 1] << 8));
        const size_t len = (size_t)data[p + 2] * 4 + data[p + 3];
        p += 4;
        if (p + len > size) return false;
        off += skip;
        for (size_t k = 0; k < len; ++k, ++off) {
            const size_t x = off % SCREEN_W;
            const size_t y = off / SCREEN_W;
            if (x < (size_t)w && y < (size_t)h)
                out.pixels[y * (size_t)w + x] = data[p + k];
        }
        p += len;
    }
    return true;
}
}  // namespace

// sub_49B16: read the 8 records of MAIN.CUR starting at 0x80. Record = {u32 size,
// 8 bytes (w,h,xo,yo - skipped), size bytes of RLE}. The first 4 are mouse cursors (dword_FB080)
// and the last 4 are the corner ornaments of the selection frame (dword_FB070).
bool CombatII::load_main_cur() {
    sel_corner_tried_ = true;
    sel_corner_loaded_ = false;
    for (auto& c : sel_corner_) c.clear();
    main_cursor_frames_.clear();
    std::string path = game_path("MAIN.CUR");
    if (!file_exists(path.c_str())) path = file_find_case_insensitive(GAME_DATA_DIR, "MAIN.CUR");
    if (path.empty() || !file_exists(path.c_str())) {
        fprintf(stderr, "COMBAT: warning: MAIN.CUR not found, speaker frame corners unavailable\n");
        return false;
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "COMBAT: warning: cannot open %s\n", path.c_str());
        return false;
    }
    bool ok = (fseek(fp, 0x80, SEEK_SET) == 0);
    for (int i = 0; ok && i < 8; i++) {
        uint32_t size = 0;
        if (fread(&size, 4, 1, fp) != 1 || size == 0 || size > 0x100000) { ok = false; break; }
        uint8_t hdr[8];
        if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) { ok = false; break; } // lseek(h, 8, SEEK_CUR)
        std::vector<uint8_t> buf(size);
        if (fread(buf.data(), 1, size, fp) != size) { ok = false; break; }
        if (i >= 4) {
            sel_corner_[i - 4] = std::move(buf);                            // dword_FB070[i-4]
        } else {
            CursorFrame cf;
            const int cw = hdr[0] | (hdr[1] << 8);
            const int ch = hdr[2] | (hdr[3] << 8);
            if (decode_main_cur_cursor(buf.data(), buf.size(), cw, ch, cf))
                main_cursor_frames_.push_back(std::move(cf));               // dword_FB080[i]
        }
    }
    fclose(fp);
    if (!ok) {
        fprintf(stderr, "COMBAT: warning: MAIN.CUR parse failed (%s)\n", path.c_str());
        for (auto& c : sel_corner_) c.clear();
        main_cursor_frames_.clear();
        return false;
    }
    sel_corner_loaded_ = true;
    fprintf(stderr, "COMBAT: MAIN.CUR loaded (cursor frames: %zu, frame corners: %zu/%zu/%zu/%zu bytes)\n",
            main_cursor_frames_.size(),
            sel_corner_[0].size(), sel_corner_[1].size(), sel_corner_[2].size(), sel_corner_[3].size());
    return true;
}

// sub_5F874: read 100 glyphs of TEXT.CHR from 0x80. Each glyph skips 8 bytes with
// `lseek(h, 8, SEEK_CUR)` and reads 0x1D (29) bytes into dword_127D4C[i].
// Buffer = {u16 w, u16 h, w*h pixels} (sub_5F093: 0xFF is transparent, everything else is filled
// with the caller's color), so 4 + w*h <= 29.
bool CombatII::load_small_font() {
    small_font_tried_ = true;
    small_font_loaded_ = false;
    for (auto& g : small_font_) g = SmallGlyph{};
    std::string path = game_path("TEXT.CHR");
    if (!file_exists(path.c_str())) path = file_find_case_insensitive(GAME_DATA_DIR, "TEXT.CHR");
    if (path.empty() || !file_exists(path.c_str())) {
        fprintf(stderr, "COMBAT: warning: TEXT.CHR not found, small font unavailable\n");
        return false;
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = (fseek(fp, 0x80, SEEK_SET) == 0);
    for (int i = 0; ok && i < 100; i++) {
        uint8_t b[0x1D] = {};
        if (fseek(fp, 8, SEEK_CUR) != 0) { ok = false; break; }
        if (fread(b, 1, sizeof(b), fp) != sizeof(b)) { ok = false; break; }
        const int w = b[0] | (b[1] << 8), h = b[2] | (b[3] << 8);
        if (w <= 0 || h <= 0 || 4 + w * h > 0x1D) continue;              // empty/invalid glyph: not drawn
        small_font_[i].w = w; small_font_[i].h = h;
        small_font_[i].px.assign(b + 4, b + 4 + w * h);
    }
    fclose(fp);
    if (!ok) { fprintf(stderr, "COMBAT: warning: TEXT.CHR parse failed (%s)\n", path.c_str()); return false; }
    // Digits '0'..'9' are at index 0x2F..0x38 (= character code - 1); any invalid one means the
    // layout assumption is wrong.
    for (int d = 0; d < 10; d++)
        if (small_font_[0x2F + d].w <= 0) {
            fprintf(stderr, "COMBAT: warning: TEXT.CHR digit glyph %d invalid -> small font disabled\n", d);
            return false;
        }
    small_font_loaded_ = true;
    fprintf(stderr, "COMBAT: TEXT.CHR loaded (digit glyph %dx%d)\n", small_font_[0x30].w, small_font_[0x30].h);
    return true;
}

// sub_608E6(eax = x, edx = y, ebx = string, ecx = letter spacing, [stack] = color): per character
//   lowercase -> uppercase (0x61..0x7A: -0x20), 0x7B and up: -0x1A (the comparison is signed, so
//   0x80 and above are unchanged); after `dec al`, values <= 0 (signed) are skipped, otherwise
//   dword_127D4C[value] is drawn at (x, y). x advances by 5 + spacing (a fixed 5, not the glyph
//   width).
void CombatII::draw_small_text(int x, int y, const char* s, uint8_t color, int spacing) {
    uint8_t* dest = video_.get_backbuffer();
    if (!dest || !s) return;
    for (; *s; ++s, x += 5 + spacing) {
        int8_t al = (int8_t)*s;
        if (al >= 0x61) al = (al <= 0x7A) ? (int8_t)(al - 0x20) : (int8_t)(al - 0x1A);
        al = (int8_t)(al - 1);
        if (al <= 0) continue;
        if (al >= 100) continue;
        const SmallGlyph& g = small_font_[al];
        for (int row = 0; row < g.h; row++)
            for (int col = 0; col < g.w; col++) {
                if (g.px[(size_t)row * g.w + col] == 0xFF) continue;      // transparent
                const int px = x + col, py = y + row;
                if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) dest[py * SCREEN_W + px] = color;
            }
    }
}

// unk_70A62[[unit+0Eh]]: per-side HP text color. [unit+0Eh] becomes (army + 1) at load time
// (loc_4F02F: `inc dh; mov [ebp+0Eh], dh`). Values are dumped from COMBATII.EXE Object 2
// (base 0x70000) at 0x70A62: 04 10 31 51 20 | 01 00 00 ...
// The table is 5 bytes (0x70A62..0x70A66); unk_70A67 (another table) follows. The original reads
// without a range check, so indices 5..7 use the real bytes (01 00 00).
static constexpr uint8_t kSpeakerHpColor[8] = {0x04, 0x10, 0x31, 0x51, 0x20, 0x01, 0x00, 0x00};

void CombatII::draw_speaker_frame(int unit_index) {
    // sub_4A106: does nothing when dword_FB1BC == 0.
    if (unit_index < 0 || unit_index >= (int)units_.size()) return;
    const CombatUnitPlacement& u = units_[(size_t)unit_index];
    if (!u.active) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    if (!sel_corner_tried_) load_main_cur();

    // ---- sub_49FE1: entry = unk_D66A8 + 572*slot + 143*[unit+63h] (facing, pose 0) ----
    //   entry[2] = width, entry[3] = height, entry[4] = x offset, entry[5] = y offset.
    //   Equivalent to the raw D66A8 table (battle_d66a8_raw); only a missing NC2 falls back to CITIZEN.
    int ew = 24, eh = 24, exo = 0, eyo = 0;
    {
        uint8_t raw[13];
        if (battle_d66a8_raw(u.sprite_type, u.facing & 3, 0, raw)) {
            ew = raw[2]; eh = raw[3]; exo = (int)(int8_t)raw[4]; eyo = (int)(int8_t)raw[5];
        } else if (const NC2Frame* fr = nc2_frame_for(u.sprite_type, u.facing & 3, 0)) {
            ew = fr->w; eh = fr->h; exo = fr->x_off; eyo = fr->y_off;
        } else if (battle_sprites_loaded_ && battle_sprites_.count() > 0) {
        // Units without an NC2 use the same CITIZEN fallback frame size/offset as the renderer
        // (direction block {0,1,3,2}, pose 0).
        static const int kCitizenDirBlock[4] = {0, 1, 3, 2};
        int frame = (int)u.sprite_id * 16 + kCitizenDirBlock[u.facing & 3] * 4;
        if (frame >= battle_sprites_.count()) frame %= battle_sprites_.count();
        if (const SpriteEntry* sp = battle_sprites_.get(frame)) {
            if (sp->word0 > 0 && sp->word2 > 0) { ew = sp->word0; eh = sp->word2; }
            exo = sp->x_offset; eyo = sp->y_offset;
        }
        }
    }
    // Horizontal: below 24 widen to 24 and shift left by (24-w)/2 (centred), otherwise x_off+12.
    // Vertical: below 24 widen to 24 and shift up by (24-h)/2, otherwise y_off-2.
    int w = ew, h = eh, offx, offy;
    if (w < 24) { offx = exo + 12 - (24 - w) / 2; w = 24; } else offx = exo + 12;
    if (h < 24) { offy = eyo - 2 - (24 - h) / 2; h = 24; } else offy = eyo - 2;

    // ---- sub_4A106: tile -> screen (sub_10971 / sub_10988) + offset, viewport clipping ----
    const int sx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;   // sub_10971
    const int sy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;   // sub_10988
    const int L = sx + offx, T = sy + offy;
    int x2 = L + w, y2 = T + h;
    if (x2 > 0x130) x2 = 0x130;            // cmp word FB1D0,130h; jle
    if (y2 > 0xC3)  y2 = 0xC3;             // cmp word FB1C4,0C3h; jle
    int x1 = L - 1, y1 = T - 1;            // dec edx / dec eax
    if (x1 < 0x10) x1 = 0x10;              // cmp dx,10h; jge
    if (y1 < 4)    y1 = 4;                 // cmp word FB1CA,4; jge

    // ---- sub_49CB1: border (sub_5ECF2 = hollow rectangle, end points inclusive) ----
    // Color = the top byte of dword_75021; after drawing it becomes ((color-8) mod 3)+9
    // (idiv: remainder truncated toward 0).
    const int8_t col = (int8_t)sel_frame_color_;
    sel_frame_color_ = (uint8_t)((((int)col - 8) % 3) + 9);
    {
        int rx1 = std::min(x1, x2), rx2 = std::max(x1, x2);
        int ry1 = std::min(y1, y2), ry2 = std::max(y1, y2);
        auto px = [&](int x, int y) {
            if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) dest[y * SCREEN_W + x] = (uint8_t)col;
        };
        for (int x = rx1; x <= rx2; x++) { px(x, ry1); px(x, ry2); }
        for (int y = ry1; y <= ry2; y++) { px(rx1, y); px(rx2, y); }
    }
    // Four corners (sub_5F3FD): dword_FB070/74/78/7C at (x1-1,y1-1), (x2-6,y1-1), (x1-1,y2-5),
    // (x2-6,y2-5).
    if (sel_corner_loaded_) {
        blit_sprite_rle(x1 - 1, y1 - 1, sel_corner_[0].data());
        blit_sprite_rle(x2 - 6, y1 - 1, sel_corner_[1].data());
        blit_sprite_rle(x1 - 1, y2 - 5, sel_corner_[2].data());
        blit_sprite_rle(x2 - 6, y2 - 5, sel_corner_[3].data());
    }

    // ---- sub_13498: HP number ----
    // Displayed value = ceil(HP/100); the port's hp is already in display units. Position =
    // (sub_10971+2, sub_10988+6), 24px higher for flying units ([unit+14h] bit 0). Letter spacing
    // is 5+1 = 6. A black shadow is drawn first at (x+1, y), then the side color at (x, y).
    char digits[16];
    snprintf(digits, sizeof(digits), "%d", u.hp);
    const int hx = sx + 2;
    int hy = sy + 6;
    if (u.flying) hy -= 0x18;
    const int fidx = std::min(std::max(battle_effective_army(u), 0), 7); // Remapped factions still use the helper's documented raw+1 fallback.
    const uint8_t fcol = kSpeakerHpColor[fidx];
    if (!small_font_tried_) load_small_font();
    if (small_font_loaded_) {
        // sub_608E6(ecx=1): pass 1 draws the black shadow at (x+1, y), pass 2 the side color at (x,
        // y).
        draw_small_text(hx + 1, hy, digits, 0, 1);
        draw_small_text(hx, hy, digits, fcol, 1);
        return;
    }
    // Fall back to the dialogue font when TEXT.CHR could not be read.
    auto put_digits = [&](int x, int y, uint16_t mode_color) {
        font_.set_text_mode(4, mode_color, 0xFF);
        char one[2] = {0, 0};
        for (int k = 0; digits[k]; k++) {
            one[0] = digits[k];
            font_.draw_string(dest, SCREEN_W, SCREEN_H, x + k * 6, y, one);
        }
    };
    put_digits(hx + 1, hy, 0x0000);
    put_digits(hx, hy, (uint16_t)(fcol << 8));
    font_.set_text_mode(4, 0x1000, 0xFF);                 // restore the default mode for later text
}

// sub_13498 HP number only (no box), extracted from the tail of draw_speaker_frame.
// sub_13546 draws it for every visible unit with the same position, color and two passes; it skips
// the box, corners and color cycling.
void CombatII::draw_unit_hp_number(const CombatUnitPlacement& u) {
    if (!u.active) return;
    uint8_t* dest = video_.get_backbuffer();
    if (!dest) return;
    const int sx = (u.x - cam_tile_x_) * TOWN_TILE_W + cam_fine_x_ + TILE_ANCHOR_X;   // sub_10971
    const int sy = (u.y - cam_tile_y_) * TOWN_TILE_H + cam_fine_y_ + TILE_ANCHOR_Y;   // sub_10988
    char digits[16];
    snprintf(digits, sizeof(digits), "%d", u.hp); // the port's hp is already the displayed value
    const int hx = sx + 2; // sub_13498: edx+2
    int hy = sy + 6;       // edi+6, minus 0x18 when flying
    if (u.flying) hy -= 0x18;
    const int fidx = std::min(std::max(battle_effective_army(u), 0), 7);
    const uint8_t fcol = kSpeakerHpColor[fidx]; // unk_70A62[[0Eh]]
    if (!small_font_tried_) load_small_font();
    if (small_font_loaded_) {
        draw_small_text(hx + 1, hy, digits, 0, 1);    // pass 1: shadow at x+1
        draw_small_text(hx, hy, digits, fcol, 1);     // pass 2: glyph body
        return;
    }
    auto put_digits = [&](int x, int y, uint16_t mode_color) {
        font_.set_text_mode(4, mode_color, 0xFF);
        char one[2] = {0, 0};
        for (int k = 0; digits[k]; k++) {
            one[0] = digits[k];
            font_.draw_string(dest, SCREEN_W, SCREEN_H, x + k * 6, y, one);
        }
    };
    put_digits(hx + 1, hy, 0x0000);
    put_digits(hx, hy, (uint16_t)(fcol << 8));
    font_.set_text_mode(4, 0x1000, 0xFF);
}

// sub_13546 (dasm 4957..5022): scan the visible window over CC998, re-check coordinates, then
// call sub_13498.
// Original window: X in [camX, camX+12] (13 columns, var_4 = camX+0x0D exclusive),
//                  Y in [camY+1, camY+11] (11 rows, var_C = camY+1+0x0B exclusive).
// The port walks units_ and culls by the window, which is equivalent (the CC998 stale guard is a
// coordinate match).
// Call order (sub_454D5): after dim, after 12161/44DCC and before 45431 (HUD), so the HUD bars
// cover the overdraw. The FA8BA gate is battle_show_all_hp() (ring_open_ || ring_sub_open_).
void CombatII::render_all_unit_hp() {
    if (!battle_show_all_hp()) return; // skipped when word_FA8BA == 0 (sub_454D5+50)
    const int x0 = cam_tile_x_;
    const int x1 = cam_tile_x_ + 12;   // inclusive
    const int y0 = cam_tile_y_ + 1;
    const int y1 = cam_tile_y_ + 11;   // inclusive
    int n = 0;
    for (const CombatUnitPlacement& u : units_) {
        if (!u.active) continue;
        if (u.x < x0 || u.x > x1 || u.y < y0 || u.y > y1) continue;
        draw_unit_hp_number(u);
        n++;
    }
    if (battle_trace_) {
        static int last_n = -1;
        static int last_cx = -9999, last_cy = -9999;
        if (n != last_n || cam_tile_x_ != last_cx || cam_tile_y_ != last_cy) {
            last_n = n; last_cx = cam_tile_x_; last_cy = cam_tile_y_;
            fprintf(stderr, "COMBAT: sub_13546 %d units at cam=(%d,%d) window x[%d,%d] y[%d,%d]\n",
                    n, cam_tile_x_, cam_tile_y_, x0, x1, y0, y1);
        }
    }
}

// sub_482A5: actual layout of the dialogue window (0x02) - dasm 75663..75750.
//   `call sub_6104A(0)` (select the palette shade table) + `sub_5ECC7(al=1)` (mode 1 = shade fill),
//   then
//     sub_5EDDE(x=16, y=120, x2=304, y2=184)   dim the whole box once
//     sub_5EDDE(x=16, y=125, x2=304, y2=184)   dim y >= 125 again (title row + two-line body)
//   Portrait: `sub_48132(eax=0x48, bx=unit[0x10])` with bx=0 (loc_4824E -> loc_48277) draws at
//   X = 0x48 - width/2 (centred on x=72) and Y = 0x78 - height (y=120 is the bottom anchor).
//   See show_portrait().
//   Name:  `sub_5E048(x = 0x48 - 4*byte_length, y = 0x5D, "%s")`
//   Body:  `sub_5E048(text, x = 0x20, y = 0x82)` -> (32,130)
//   Then `sub_5E9E8` presents and `mov eax,2BCh; call sub_499EF` waits for a key or 700 ticks
//   (~3.99s).
void CombatII::show_dialogue_line(const std::string& text, int portrait_id,
                                  const std::string& speaker_name, int speaker_tx, int speaker_ty,
                                  int speaker_unit){
    // sub_507BE -> sub_482A5 passes the speaker unit pointer, so the unit tile is read again at
    // display time instead of using a stored tile or the (sel, idx) of the dialogue prefix.
    if (speaker_unit >= 0 && speaker_unit < (int)units_.size()) {
        speaker_tx = units_[(size_t)speaker_unit].x;
        speaker_ty = units_[(size_t)speaker_unit].y;
    }
    // ==== The steps of sub_482A5 (dasm 75580..75750) ====
    //
    // (1) Entry (sub_1124A): align the speaker tile with the camera centre (speaker-6).
    //     Clamps (sub_11003 loc_11018/loc_11034/loc_1103D/loc_11059): x<0 -> 0,
    //     x>map width-12 -> map width-12 (same for y). The original leaves the fine offsets
    //     (dword_7001C/BA838) alone, so the resting values (12, 0) take part in the coordinate
    //     math. When no unit stands on the speaker tile bx=0 and the animation is skipped
    //     (sub_482A5+5A: jz loc_4841C).
    // (2) Speaker head in screen space (sub_482A5+33): sub_10971(x)+12, sub_10988(y)+4, where
    //     sub_10971 = (tile-camX)*24 + dword_7001C(lo) + 4 and sub_10988 =
    //     (tile-camY)*16 + dword_BA838 + 4. `add edi,4` puts the head 4px below the tile top.
    //     Skipped when offscreen (sub_6104A=0).
    // (3) Dialogue background expansion (loc_482D9 .. loc_48368 loop). This is an ellipse, not a
    //     circle:
    //     First frame (before the loop): one ellipse (rx=10, ry=8) over the previous world frame,
    //     present, then sub_1033B(0x1E) + sub_499E4 hold it for one tick (30/175.47 = 171ms).
    //     Loop state (stack): rx = [R+4] starts at 10, ry = [R+0xC] starts at 8, delta = [R+8]
    //     starts at 2 (`mov ecx,2` survives sub_1033B/sub_499E4).
    //     Each iteration: redraw the world (sub_454B3) -> dim mode 1 -> ellipse (sub_6075A,
    //     ebx=rx, ecx=ry) -> mode 0 -> sub_45431 (redraw HUD and black bars so the ellipse never
    //     dims the HUD) -> present.
    //     Then rx += delta; ry += 1; delta += 2 (horizontal accelerates by 2,4,6.., vertical by 1);
    //     edi += (160-edi)/10 and the loop ends once edi > 150 (cmp di,96h; jg loc_4841C);
    //     esi += (300-esi)/10.
    //     The result: a small ellipse lifts off the head, slides towards (300,160) and spreads
    //     horizontally into a wide band around y = 116..184, which becomes the dialogue window.
    //     There is no tick wait inside the loop: in the original the render speed was the timing.
    //     Deviation: the port waits until each frame is actually presented (Video::wait_presented),
    //     which the original never calls. On low-end hardware rendering was slow enough to show the
    //     ellipse growing; on a modern PC the loop finishes in a few ms, so the wait restores the
    //     visible expansion (a vsync-less renderer fills ~15ms per frame).
    //     There is no window dimming inside the loop; the window (sub_5EDDE twice) is only drawn
    //     in the final layer of step (4).
    // (4) Final layer (loc_4841C): one world frame -> dim mode 1 -> the dialogue window twice ->
    //     switch to mode 0 -> sub_5EDDE(16,120,304,184) -> portrait -> name -> body -> wait 2BC
    //     ticks.

    // ---- (1) Centre the camera on the speaker tile (sub_1124A + sub_11003) ----
    if (speaker_tx >= 0 && speaker_ty >= 0) {
        camera_center_on(speaker_tx, speaker_ty);
        if (script_trace_)
            fprintf(stderr, "COMBAT: dialogue camera: speaker tile=(%d,%d) -> cam=(%d,%d) fine=(%d,%d)\n",
                    speaker_tx, speaker_ty, cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_);
    }

    // ---- (2) Determine the speaker unit (sub_49FE1: the unit standing on the speaker tile) ----
    // sub_507BE passes the speaker unit pointer (ebp) directly, so the unit index wins; the tile
    // lookup is only a fallback (covers units whose tile changed through spawn/walk).
    int su = -1;
    if (speaker_unit >= 0 && speaker_unit < (int)units_.size())
        su = speaker_unit;
    else if (speaker_tx >= 0 && speaker_ty >= 0)
        su = find_unit_index((uint8_t)speaker_tx, (uint8_t)speaker_ty);

    // ---- (3) Whether to animate: port of the bx decision in sub_507BE loc_508B4 ----
    //   ecx (speaker unit) == dword_1102B8 (previous speaker) -> bx=0: skip the ellipse
    //   a different speaker                                 -> bx=1: play it
    //   no speaker found (loc_508DF)                        -> bx=2, turned into 0 at loc_482D7
    // dword_1102B8 is always set to the current speaker right after the sub_482A5 call
    // (`mov dword_1102B8, ecx`), regardless of bx.
    bool animate = true;
    if (su < 0)
        animate = false;                          // no speaker -> bx=2 -> 0
    else if (last_dialogue_unit_ >= 0 && su == last_dialogue_unit_)
        animate = false;                          // same speaker again -> bx=0
    last_dialogue_unit_ = su;

    // ---- Tail of sub_1124A (+0x68..+0x98): sub_454B3 -> sub_4A106 -> sub_5E9E8 ----
    // Once the camera is aligned, draw the world, overlay the speaker frame (corners + HP) and
    // present once. (The bx=2 path has no frame target.)
    if (su >= 0) {
        render_battle_frame();
        draw_speaker_frame(su);
        video_.present();
    }

    // ---- (3) Speaker head in screen space (sub_482A5+33), using the current fine offset ----
    int sx = 0, sy = 300;
    if (su >= 0) {
        const auto& us = units_[(size_t)su];
        // sub_482A5: esi = sub_10971+0Ch, edi = sub_10988+4 (head).
        sx = (us.x - cam_tile_x_) * 24 + cam_fine_x_ + TILE_ANCHOR_X + CENTER_ADD_X;
        sy = (us.y - cam_tile_y_) * 16 + cam_fine_y_ + TILE_ANCHOR_Y + HEAD_ADD_Y;
    }

    if (animate) {
        // ---- (4) Ellipse expansion (loc_482D9 first frame + loc_48368 loop) ----
        const bool dtrace = DevUtil::dialogue_trace();

        int rx = 10, ry = 8, delta = 2;            // [R+4], [R+0xC], [R+8]
        int frames = 0;
        if (dtrace)
            fprintf(stderr, "COMBAT: unit=%d tile=(%d,%d) cam=(%d,%d) head=(%d,%d)\n",
                    su, units_[(size_t)su].x, units_[(size_t)su].y,
                    cam_tile_x_, cam_tile_y_, sx, sy);

        // First frame: one small ellipse over the world sub_1124A drew last (sub_454B3). This frame
        // does not call sub_45431 again; it is presented as is (dasm 75630..75640).
        render_battle_frame();
        draw_speaker_frame(su);                    // loc_482D9 -> `call sub_4A106` (before dim/ellipse)
        ellipse_shade_fill(sx, sy, rx, ry);        // esi = head X+12, edi = head Y+4, rx=10, ry=8
        video_.present();                          // sub_5E9E8
        game_timer_set(0x1E);                      // sub_1033B(1Eh): tick period becomes 30 (171ms)
        wait_game_tick();                          // sub_499E4: hold the first frame for one tick

        // loc_48368 loop; it runs at least once because the exit test follows the update.
        for (;;) {
            render_battle_frame();                 // sub_454B3: redraw the world
            draw_speaker_frame(su);                // sub_4A106 (sub_4A210 clears the visibility flag of the last iteration)
            ellipse_shade_fill(sx, sy, rx, ry);    // sub_6075A: ebx=rx (horizontal), ecx=ry (vertical)
            render_hud_frame();                    // sub_45431: redraw HUD and black bars above the ellipse
            video_.present();                      // sub_5E9E8
            // Deviation: wait until this frame is actually presented. The original loop has no tick
            // or
            // retrace wait - a 386 rendered each frame slowly enough to show the growth - while on
            // a
            // modern PC the loop ends in a few milliseconds. At 60Hz each presented frame shows the
            // ellipse spreading; on a vsync-less renderer the wait fills ~15ms.
            video_.wait_presented();
            frames++;

            rx += delta;                           // add edx,eax  (horizontal: accelerating)
            ry += 1;                               // inc ebx      (vertical: by 1)
            delta += 2;                            // add ecx,2
            sy += (160 - sy) / 10;                 // edi += (160-edi)/10  (idiv: truncates toward 0)
            if (sy > 150) break;                   // cmp di,96h; jg loc_4841C  (esi not updated)
            sx += (300 - sx) / 10;                 // esi += (300-esi)/10
        }
        if (dtrace)
            fprintf(stderr, "COMBAT: ellipse done frames=%d end=(%d,%d) rx=%d ry=%d\n", frames, sx, sy, rx, ry);
    }

    // ---- (5) Static final layer (loc_4841C..) ----
    // The original redraws the world with sub_454B3 on entry too, so no ellipse frame is left
    // behind (otherwise the ellipse stays as an afterimage behind the dialogue).
    render_battle_frame();
    // loc_4841C: `call sub_4A106` right after sub_454B3, so corners + HP are drawn before the
    // dimming window (sub_5EDDE).
    draw_speaker_frame(su);
    // sub_5EDDE excludes the end points (x2/y2). Register values verified at dasm 75703..75713:
    //   pass 1: x1=0x10 x2=0x130 y1=0x78 y2=0xBD (ecx; `mov ecx,0BDh` survives sub_454B3/sub_4A106/
    //           sub_6104A/sub_5ECC7) -> x 16..303, y 120..188
    //   pass 2: x1=0x10 x2=0x130 y1=0x7D y2=0xB8 -> x 16..303, y 125..183
    // The top/bottom five rows (120..124, 184..188) are dimmed once; the middle (125..183) twice.
    render_dialog_dimming(0x10, 0x78, 0x130 - 0x10, 0xBD - 0x78);
    render_dialog_dimming(0x10, 0x7D, 0x130 - 0x10, 0xB8 - 0x7D);
    // Portrait: sub_48132(eax=0x48, bx=unit[0x10]) aligns x=0x48 as the horizontal centre and
    // y=0x78 as the bottom edge (see show_portrait).
    if (portrait_id >= 0) show_portrait(portrait_id, 0x48, 0x78);
    // Name: `mov eax,48h; shl ecx,2; sub eax,ecx` -> x = 0x48 - 4 * (byte length), y = 0x5D
    if (!speaker_name.empty()) {
        int nx = 0x48 - 4 * (int)speaker_name.size();
        if (nx < 0) nx = 0;
        draw_text_wrapped(nx, 0x5D, speaker_name);
    }
    // Body: `push edx; push 82h; push 20h` -> (x=0x20, y=0x82)
    draw_text_wrapped(0x20, 0x82, text);
}

void CombatII::show_portrait(int portrait_id, int x, int y){
    // Exact port of the coordinate math in sub_48132 (dasm 75455). The dialogue path (sub_482A5)
    // passes ecx (camera word pair) = 0, bypassing the sub_10031 scroll correction, and calls with
    // bx=0, so it goes loc_4824E -> loc_48277:
    //   loc_4824E: X = anchor_x - width/2
    //     `mov eax,[esp+0Ah]` reads the low dword (= h<<16 | w) of the adjacent var_14(h)/var_10(w)
    //     stack slots and `sar 11h` gives width/2 (this path when ebx=0).
    //   loc_48277: Y = anchor_y - height
    //     `mov eax,[esp+1Ch+var_18+2]` reads the high word of var_18(h); `sar 11h` gives height.
    // So x is the horizontal centre and y is the bottom anchor (verified at dasm 87167..87197).
    uint16_t w=0,h=0;
    std::vector<uint8_t> rle;
    std::string path = game_path("PORTRAIT.SYF");
    if (!file_exists(path.c_str())) path = game_path("portrait.syf");
    if (!file_exists(path.c_str())) path = file_find_case_insensitive(GAME_DATA_DIR, "PORTRAIT.SYF");
    if (portrait_load(path, portrait_id, w, h, rle) && !rle.empty()) {
        int bx = x - (int)w / 2;   // loc_4824E: anchor_x - width/2
        int by = y - (int)h;       // loc_48277: anchor_y - height
        blit_sprite_rle(bx, by, rle.data());
        return;
    }
    uint8_t* dest=video_.get_backbuffer();
    if(!dest) return;
    // Placeholder 48x48 box on failure
    for(int py=0;py<48;py++) for(int px=0;px<48;px++){
        int dx=x+px, dy=y+py;
        if(dx>=0&&dx<SCREEN_W&&dy>=0&&dy<SCREEN_H){
            dest[dy*SCREEN_W+dx]= (px==0||py==0||px==47||py==47)? 0x0E : 0x09;
        }
    }
}

// G2_RETREAT_DUMP=<prefix>: dump every subframe of the 0x04 retreat effect (sub_17071) as PGM.
void CombatII::dump_retreat_frame(int frame, int sub) {
    const char* path = getenv("G2_RETREAT_DUMP");
    if (!path || !*path) return;
    const uint8_t* buf = video_.get_backbuffer();
    if (!buf) return;
    char name[512];
    snprintf(name, sizeof(name), "%s.%02d_%d.pgm", path, frame, sub);
    if (!DevUtil::dump_pgm_gray(name, buf, SCREEN_W, SCREEN_H)) return;
    fprintf(stderr, "COMBAT: dumped %s (cam=(%d,%d) fine=(%d,%d))\n", name,
            cam_tile_x_, cam_tile_y_, cam_fine_x_, cam_fine_y_);
}

// G2_DUMP_FRAME=<path>: dump the 320x200 backbuffer once as an 8-bit PGM.
void CombatII::dump_frame_if_requested() {
    const char* path = getenv("G2_DUMP_FRAME");
    if (!path || !*path) return;
    static int dumped = 0;
    static int skipped = 0;
    const int skip = getenv("G2_DUMP_FRAME_SKIP") ? atoi(getenv("G2_DUMP_FRAME_SKIP")) : 0;
    if (skipped < skip) { skipped++; return; }
    const int limit = getenv("G2_DUMP_FRAME_N") ? atoi(getenv("G2_DUMP_FRAME_N")) : 1;
    if (dumped >= limit) return;
    const uint8_t* buf = video_.get_backbuffer();
    if (!buf) return;
    char name[512];
    snprintf(name, sizeof(name), "%s.%d.pgm", path, dumped);
    if (!DevUtil::dump_pgm_gray(name, buf, SCREEN_W, SCREEN_H)) return;
    dumped++;
    fprintf(stderr, "COMBAT: dumped frame -> %s (last NC2 unit at (%d,%d) %dx%d)\n", name,
            g_last_nc2_draw.x, g_last_nc2_draw.y, g_last_nc2_draw.w, g_last_nc2_draw.h);
}

// sub_516D0(0x05) movement effect. The first point is the current position; the rest is the path.
//
// ---- group 0: sub_16D3E (path walking) ----
//   sub_1033B(9)                    -> subframe length 9/175.47 = 51.3ms
//   loc_1702A: var_4 = 0 .. word_EDD74-1 (word_EDD74 = max(path point count + 1))
//     loc_16DFA (each iteration): index = counter. If unk_E5210[counter] == 0xFF the unit has
//                          arrived (var_4C=1), otherwise [unit+63h] = direction of that point
//                          (loc_16E1A). [64h] is not touched here; the pose is advanced only by
//                          sub_16A6D in the subframe loop.
//     loc_16EBC: 4 subframes x { sub_4559F (camera/window) + sub_5E9E8 (present) + sub_499E4
//                                  (tick) -> then the per-unit update }
//     Per-unit update (sub_16AEF -> sub_16A6D): advance the pose counters (unk_E5AB0/unk_E5A70)
//                and [64h], and add [65h] += 6 / [66h] += 4. An arrived unit walks var_4C
//                1 -> 2 -> 3, sets [63h] = load and [64h] = 2 (class != 0) -> 0, then stops.
//     loc_16FE6 (after subframe 1): sub_16C93 moves the tile by +-1 and rewinds the offset by
//                -+(12/8), so one 24x16 tile is crossed in exactly 4 subframes (6*4=24, 4*4=16).
//   In 1-1 (26 path points = 25 steps) that is (26+1) x 4 = 108 frames = 5.54s for 25 tiles.
//
//   Where things are drawn: because of that order, render pass k sees the state after update
//   k-1. Measured from the start tile the offsets are 0, +-1/4, +-2/4, +-3/4 of a tile, and the
//   next iteration starts at the destination tile, so exactly one tile is crossed. The port
//   therefore uses `delta * tile * sub / 4` (not sub+1).
//
// ---- group != 0: sub_50BAF (two-point interpolation) ----
//   `add word_EDD74, 8` -> total frames = 2 x (2+8) = 20 (+1 finishing pass), sub_1033B(0x0C) =
//   68.4ms. Interpolation is `sub_50216(unit, 7 - unk_E5A50[record])`, 8 stages, and the counter
//   advances every other frame, so it finishes in 16 frames and the rest keeps the arrival pose.
// Both routines call `sub_4559F` (= sub_454B3 map + sub_5046B camera/window) every frame, so the
// camera scroll and message window keep advancing while walking. Without that the scroll only
// catches up after the cutscene and later commands pile up in the waypoint queue.
// ---- EVENT.SYB drop/effect sprites (sub_5004F + sub_4F25F) ----
//   sub_5004F: `mov edx, offset unk_10059C; mov eax, "event.syb"; xor ebx,ebx; mov ecx, 8;
//              call sub_4F25F` loads 8 records into the unk_1005A4 table.
//   sub_4F25F: lseek(0x42) -> u16 record count (8) -> lseek(0x3C=60) -> per record
//              u32 size / u16 w (low byte) / u16 h (low byte) / s16 xo (low byte, signed) /
//              s16 yo (low byte, signed) / size bytes of RLE. Same layout as a single NC2 frame
//              (measured: EVENT.SYB rec0 w=22 h=11 xo=-10 yo=-16, and the records run
//              variable-length to exactly EOF).
//   Frames are kept as NC2Frame and drawn from the draw_list in render_sprites.
void CombatII::load_event_syb() {
    static bool g_event_syb_loaded = false;
    static std::vector<NC2Frame> g_event_syb;
    event_syb_ = &g_event_syb;
    if (g_event_syb_loaded) return;
    g_event_syb_loaded = true;
    FILE* fp = fopen(game_path("EVENT.SYB").c_str(), "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    const long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)fsz);
    const size_t got = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    if (got != buf.size() || buf.size() < 0x44) return;

    const uint16_t nrec = (uint16_t)(buf[0x42] | (buf[0x43] << 8));
    if (nrec == 0 || nrec > 64) return;
    size_t pos = 0x80;               // sub_4F25F seeks to 60 (004070) for the first record; measured 0x80
    for (uint16_t i = 0; i < nrec; ++i) {
        if (pos > buf.size() || buf.size() - pos < 12) break;
        const uint8_t* f = buf.data() + pos;
        const uint32_t dsize = (uint32_t)f[0] | ((uint32_t)f[1] << 8) |
                               ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
        NC2Frame fr;
        fr.w = f[4];                 // low byte only (same as NC2 sub_11575)
        fr.h = f[6];
        fr.x_off = (int8_t)f[8];
        fr.y_off = (int8_t)f[10];
        // Dsize is the RLE payload size after the 12-byte record header.
        const size_t data_start = pos + 12;
        if (dsize < 2 || (size_t)dsize > buf.size() - data_start ||
            fr.w <= 0 || fr.h <= 0 || (size_t)fr.w * fr.h > 0x40000) break;
        const size_t data_end = data_start + (size_t)dsize;
        fr.pixels.assign((size_t)fr.w * fr.h, 0xFF);
        // Same stream format as decode_strip in nc2_parser.cpp (mirrors the verified decoder).
        {
            const uint16_t runs = (uint16_t)((uint16_t)buf[data_start] |
                                             ((uint16_t)buf[data_start + 1] << 8));
            size_t sp = data_start + 2, out = 0;
            int cursor = 0;
            bool ok = true;
            for (uint16_t r = 0; r < runs; r++) {
                if (sp > data_end || data_end - sp < 4) { ok = false; break; }
                const uint16_t delta = (uint16_t)(buf[sp] | (buf[sp + 1] << 8));
                const int n_dw = buf[sp + 2];
                const int n_tail = buf[sp + 3];
                sp += 4;
                const size_t len = (size_t)n_dw * 4 + (size_t)n_tail;
                if (len > data_end - sp) { ok = false; break; }
                cursor += delta;
                int fill = delta;
                if (cursor >= 320) { cursor -= 320; fill = cursor; }
                out += fill;
                for (size_t b = 0; b < len; b++) {
                    if (out < fr.pixels.size()) fr.pixels[out] = buf[sp + b];
                    out++;
                }
                sp += len;
                cursor += (int)len;
            }
            if (!ok) { pos = data_end; continue; }
        }
        g_event_syb.push_back(std::move(fr));
        pos = data_end;
    }
}
