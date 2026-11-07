// pack_class.cpp: class change screen (cc.pcx), reached from the status window's
// lower-left button.
//
// Original (dasm/PACK_code.ASM):
//   sub_2F789  status_class_screen  screen body (entry transition + event loop)  40380..42405
//   sub_2E392  image-only PCX loader (no palette)                                38497
//   sub_2E5CE  calc_shortfall       class requirement shortfall                 38917
//   sub_2E979  req_ok               requirement check (dims unmet grid cells)    39067
//   sub_2EC68  draw_party_list      six-row party list on the right              39333
//   sub_2EDC9  compose_base         cc.pcx + party list + left requirement icons 39475
//   sub_2F113  draw_grid            4x4 class cells + selected cell name label   39736
//   sub_2F435  class_tab_of         character -> tab number                      40019
//   sub_2F5B0  class_sprite_row     row that holds the town sprite               40199
//   sub_2F602  compose_all          compose_base + grid + thumb + tab buttons    40239
//   sub_2F6AF  class_can_change     whether the class change is allowed          40291
//   sub_2F70E  class_recalc_stats   recompute derived stats after the change     40335
//
// The numeric tables are read at runtime from PACK.EXE VAs (src/exe_resource.h):
//   byte_768AE  63 x 18  class requirements (10 stats + 6 skills + prev class + level)
//   unk_76D1C   15 x 16 words  the 4x4 class cells of each tab (class+1)
//   off_76EFC   48 x 63 words  town sprite number per row
//   dword_7869A[cls*2] >> 16 = word at 0x7869C + cls*8 = tab number unless overridden
//   word_537D8[cls*8] (+0 attack, +2 speed byte, +4 defense/magic)
//   unk_70830[cls*3] weapon group (0 or 6 doubles the attack)
//   off_76336 three tab buttons (0x15E apart), off_75FDA/off_76188 previous-class icons,
//   unk_75E2C shortfall background, off_75774/off_75922 level icons, off_76750 OK button
//
// Note: the original edits a copy at 0x19AC24 and writes it back with sub_230AE on exit;
// the port edits the g_characters record directly, so no write-back is needed.
#include "pack.h"
#include "pack_data.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "audio.h"
#include "palette.h"
#include "game_timer.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <initializer_list>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

namespace {

// Character record (+0x88) offsets, confirmed in sub_2E5CE/sub_2F113/sub_2F70E.
constexpr int R_NAME  = 0x02;
constexpr int R_PREV  = 0x0D;   // byte_19AC31: previous class
constexpr int R_ID    = 0x0F;
constexpr int R_CLASS = 0x11;
constexpr int R_LEVEL = 0x2B;
constexpr int R_SKILL = 0x2E;
constexpr int R_MAG   = 0x38;
constexpr int R_STAT  = 0x40;
constexpr int R_STATW = 0x44;
constexpr int R_HPMAX = 0x4C;
constexpr int R_MPMAX = 0x52;
constexpr int R_ATS   = 0x54;
constexpr int R_DEF   = 0x59;
constexpr int R_TOWN  = 0x61;
constexpr int EQUIP_OFF[5] = { 0x5B, 0x5A, 0x5D, 0x5E, 0x5F };

constexpr int CLASS_COUNT = 63;
constexpr int TAB_COUNT   = 15;
constexpr int SPR_ROWS    = 48;
constexpr int SPR_COLS    = 63;

inline int rd16(const uint8_t* r, int o) { return (int)(int16_t)(r[o] | (r[o + 1] << 8)); }
inline int rd32(const uint8_t* r, int o) {
    return (int)(int32_t)(r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24));
}
inline void put16(uint8_t* r, int o, int v) { r[o] = (uint8_t)v; r[o + 1] = (uint8_t)(v >> 8); }
inline int halfv(int v) { return (v - (v >> 31)) >> 1; }   // (v - sign) >> 1

struct ClassAssets {
    bool tried = false, ok = false;
    std::vector<uint8_t> req, grid, spr_rows, tab_of, rec8, wgroup;
    std::vector<uint8_t> tab_btn[3], prev_a, prev_b, tip_bg, lvl_ok, lvl_bad, ok_btn;
};
ClassAssets g_cls;

bool load_class_assets() {
    if (g_cls.tried) return g_cls.ok;
    g_cls.tried = true;
    bool ok = true;
    ok &= exe_resource_load(ExeId::Pack, 0x768AE, ExeResLength::Fixed, g_cls.req, (size_t)CLASS_COUNT * 18);
    ok &= exe_resource_load(ExeId::Pack, 0x76D1C, ExeResLength::Fixed, g_cls.grid, (size_t)TAB_COUNT * 32);
    ok &= exe_resource_load(ExeId::Pack, 0x76EFC, ExeResLength::Fixed, g_cls.spr_rows, (size_t)SPR_ROWS * SPR_COLS * 2);
    ok &= exe_resource_load(ExeId::Pack, 0x7869A, ExeResLength::Fixed, g_cls.tab_of, (size_t)CLASS_COUNT * 8 + 4);
    ok &= exe_resource_load(ExeId::Pack, 0x537D8, ExeResLength::Fixed, g_cls.rec8, (size_t)CLASS_COUNT * 8);
    ok &= exe_resource_load(ExeId::Pack, 0x70830, ExeResLength::Fixed, g_cls.wgroup, (size_t)CLASS_COUNT * 3);
    for (int i = 0; i < 3; i++)
        ok &= exe_resource_load(ExeId::Pack, (uint32_t)(0x76336 + i * 0x15E), ExeResLength::Rle, g_cls.tab_btn[i]);
    ok &= exe_resource_load(ExeId::Pack, 0x75FDA, ExeResLength::Rle, g_cls.prev_a);
    ok &= exe_resource_load(ExeId::Pack, 0x76188, ExeResLength::Rle, g_cls.prev_b);
    ok &= exe_resource_load(ExeId::Pack, 0x75E2C, ExeResLength::Rle, g_cls.tip_bg);
    ok &= exe_resource_load(ExeId::Pack, 0x75774, ExeResLength::Rle, g_cls.lvl_ok);
    ok &= exe_resource_load(ExeId::Pack, 0x75922, ExeResLength::Rle, g_cls.lvl_bad);
    ok &= exe_resource_load(ExeId::Pack, 0x76750, ExeResLength::Rle, g_cls.ok_btn);
    if (!ok) fprintf(stderr, "PACK: warning: class change screen data load failed (PACK.EXE VA)\n");
    g_cls.ok = ok;
    return ok;
}

inline int req_at(int cls, int k) {
    const size_t i = (size_t)cls * 18 + (size_t)k;
    return i < g_cls.req.size() ? g_cls.req[i] : 0;
}
inline int grid_class(int tab, int cell) {
    const size_t i = (size_t)tab * 32 + (size_t)cell * 2;
    if (i + 1 >= g_cls.grid.size()) return -1;
    return (int)(int16_t)(g_cls.grid[i] | (g_cls.grid[i + 1] << 8)) - 1;
}
inline int spr_of(int row, int cls) {
    if (row < 0 || row >= SPR_ROWS || cls < 0 || cls >= SPR_COLS) return -1;
    const size_t i = ((size_t)row * SPR_COLS + (size_t)cls) * 2;
    if (i + 1 >= g_cls.spr_rows.size()) return -1;
    return (int)(uint16_t)(g_cls.spr_rows[i] | (g_cls.spr_rows[i + 1] << 8));
}
inline int tab_of_table(int cls) {
    const size_t i = (size_t)cls * 8 + 2;
    if (i + 1 >= g_cls.tab_of.size()) return 0;
    return (int)(int16_t)(g_cls.tab_of[i] | (g_cls.tab_of[i + 1] << 8));
}
inline int rec8_word(int cls, int off) {
    const size_t i = (size_t)cls * 8 + (size_t)off;
    if (i + 1 >= g_cls.rec8.size()) return 0;
    return (int)(uint16_t)(g_cls.rec8[i] | (g_cls.rec8[i + 1] << 8));
}
inline int rec8_byte(int cls, int off) {
    const size_t i = (size_t)cls * 8 + (size_t)off;
    return i < g_cls.rec8.size() ? g_cls.rec8[i] : 0;
}
inline int wgroup_of(int cls) {
    const size_t i = (size_t)cls * 3;
    return i < g_cls.wgroup.size() ? g_cls.wgroup[i] : 0;
}

}  // namespace

// ==== sub_2F789: class change screen ====
// Returns -1 = class change not allowed (the caller shows a message), 0 = close the whole
// status window, 1 = return to the status window.
int Pack::status_class_screen(int sel) {
    load_item_data();
    status_load_assets();
    if (item_sprites_.empty()) load_standard_sprites(game_path("item.spr"), item_sprites_);
    if (!load_class_assets()) return -1;

    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return -1;
    const size_t screen_bytes = (size_t)SCREEN_W * SCREEN_H;

    auto party_n = [&]() { return party_count(); };
    int ebp = sel;
    if (party_n() <= 0) return -1;
    if (ebp < 0) ebp = 0;
    if (ebp >= party_n()) ebp = party_n() - 1;
    // sub_2F789 entry saves *edx (the status window's top row, dword_1A4F10) into dword_1A40E0
    // and restores it on return 1; the port saves and restores status_top_ instead.
    const int saved_status_top = status_top_;

    // Screen state (original dword_1A40D4/D8/DC/E0/E4).
    int spr_row = 0;
    int tab_cur = 0;
    int cell_cur = 0;
    int sel_class = -1;
    int short_[18] = {0};

    const uint8_t* icons = status_icons_.data();
    const size_t icon_count = status_icons_.size() / 0x1AE;
    auto icon_at = [&](int slot) -> const uint8_t* {
        if (slot < 0 || (size_t)slot >= icon_count) return nullptr;
        return icons + (size_t)slot * 0x1AE;
    };

    // ---- sub_2F6AF: whether the class change is allowed ----
    auto can_change = [&](int idx) {
        const uint8_t* r = party_rec(idx);
        if (!r) return false;
        switch (r[R_CLASS] + 1) {
            case 3: case 4: case 5: case 6: case 0x0A: case 0x12:
            case 0x21: case 0x34: case 0x35: case 0x47: case 0x48:
                return false;
            default: break;
        }
        return r[R_ID] != 0x48;
    };
    if (!can_change(ebp)) return -1;

    // ---- sub_2F435: tab number ----
    auto tab_of = [&](int idx) {
        const uint8_t* r = party_rec(idx);
        if (!r) return 3;
        const int id = r[R_ID];
        auto in = [&](std::initializer_list<int> l) {
            for (int v : l) if (v == id) return true;
            return false;
        };
        if (in({0x45, 0x2E})) return 3;
        if (in({0x66, 0x68, 0x6C})) return 4;
        if (in({0x3F, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62})) return 5;
        if (in({0x3E, 0x2C})) return 6;
        if (in({0x14, 0x15, 0x32, 0x3B, 0x3C, 0x3D, 0x6D, 0x6E})) return 7;
        if (in({0x03, 0x04, 0x0C, 0x48, 0x64, 0x65, 0x6A, 0x6B, 0x70, 0x71, 0x73, 0x77})) return 8;
        if (in({0x16, 0x50})) return 9;
        if (in({0x4B})) return 0x0A;
        if (in({0x08, 0x4F, 0x67})) return 0x0B;
        if (in({0x42, 0x41})) return 0x0C;
        if (in({0x43, 0x44})) return 0x0D;
        if (in({0x11, 0x6F})) return 0x0E;
        return tab_of_table(r[R_CLASS]);
    };
    // ---- sub_2F5B0 ----
    auto sprite_row = [&](int idx) {
        const uint8_t* r = party_rec(idx);
        if (!r) return 0x1869F;
        const int spr = rd16(r, R_TOWN);
        for (int row = 0; row < SPR_ROWS; row++)
            for (int c = 0; c < SPR_COLS; c++)
                if (spr_of(row, c) == spr) return row;
        return 0x1869F;
    };
    // ---- sub_2E979 ----
    auto req_ok = [&](const uint8_t* r, int cls) {
        if (!r || cls < 0 || cls >= CLASS_COUNT) return false;
        bool ok = true;
        if (r[R_STAT + 0] < req_at(cls, 0)) ok = false;
        if (req_at(cls, 1) > r[R_STAT + 1]) ok = false;
        if (req_at(cls, 2) > r[R_STAT + 2]) ok = false;
        if (req_at(cls, 3) > r[R_STAT + 3]) ok = false;
        if (rd32(r, R_HPMAX) / 100 < req_at(cls, 4)) ok = false;
        if (req_at(cls, 5) > (int16_t)(uint16_t)rd16(r, R_MPMAX)) ok = false;
        if (req_at(cls, 6) > r[R_MAG + 0]) ok = false;
        if (req_at(cls, 7) > r[R_MAG + 2]) ok = false;
        if (req_at(cls, 8) > r[R_MAG + 1]) ok = false;
        if (req_at(cls, 9) > r[R_MAG + 3]) ok = false;
        for (int k = 10; k < 16; k++)
            if (req_at(cls, k) > r[R_SKILL + (k - 10)]) ok = false;
        if (r[R_PREV] + 1 != req_at(cls, 16) && req_at(cls, 16) != 0) ok = false;
        if (r[R_LEVEL] < req_at(cls, 17)) ok = false;
        return ok;
    };
    // ---- sub_2E5CE ----
    auto calc_short = [&](const uint8_t* r, int cls) {
        for (int i = 0; i < 18; i++) short_[i] = 0;
        if (!r || cls < 0 || cls >= CLASS_COUNT) return false;
        bool ok = true;
        auto bcmp = [&](int k, int have) {
            const int q = req_at(cls, k);
            if (have < q) { short_[k] = (uint8_t)(q - have); ok = false; }
        };
        bcmp(0, r[R_STAT + 0]);
        bcmp(1, r[R_STAT + 1]);
        bcmp(2, r[R_STAT + 2]);
        bcmp(3, r[R_STAT + 3]);
        {
            const int hp = rd32(r, R_HPMAX) / 100;
            const int q = req_at(cls, 4);
            if (hp < q) { short_[4] = (uint8_t)(q - hp); ok = false; }
        }
        {
            const int mp = (int16_t)(uint16_t)rd16(r, R_MPMAX);
            const int q = req_at(cls, 5);
            if (mp < q) { short_[5] = (uint8_t)(q - mp); ok = false; }
        }
        bcmp(6, r[R_MAG + 0]);
        bcmp(7, r[R_MAG + 2]);
        bcmp(8, r[R_MAG + 1]);
        bcmp(9, r[R_MAG + 3]);
        for (int k = 10; k < 16; k++) bcmp(k, r[R_SKILL + (k - 10)]);
        {
            const int q = req_at(cls, 16);
            if (r[R_PREV] + 1 != q && q != 0) { short_[16] = 1; ok = false; }
        }
        {
            const int q = req_at(cls, 17);
            if (r[R_LEVEL] < q) { short_[17] = (uint8_t)(q - r[R_LEVEL]); ok = false; }
        }
        return ok;
    };
    // ---- sub_188BC: dim a rectangle (palette 0x00..0x60 -> 7-(R+G+B)/0x25) ----
    auto dim_rect = [&](int x1, int y1, int x2, int y2) {
        for (int y = y1; y < y2; y++) {
            if (y < 0 || y >= SCREEN_H) continue;
            for (int x = x1; x < x2; x++) {
                if (x < 0 || x >= SCREEN_W) continue;
                uint8_t& p = bb[y * SCREEN_W + x];
                if (p > 0x60) continue;
                uint8_t rr, gg, bbv;
                palette_get_entry(p, rr, gg, bbv);
                p = (uint8_t)(7 - ((int)rr + gg + bbv) / 0x25);
            }
        }
    };

    // ---- sub_2EC68: party list on the right (6 rows, 0x18 apart) ----
    auto draw_party_list = [&](int top, int selsel) {
        const int n = party_n();
        for (int row = 0; row < 6; row++) {
            const int idx = top + row;
            const int y0 = 4 + 0x18 * row;
            if (idx == selsel) draw_frame_box(0xA9, y0, 0x129, 0x1C + 0x18 * row);
            if (idx >= n) continue;
            const uint8_t* r = party_rec(idx);
            if (!r) continue;
            const int spr = rd16(r, R_TOWN) * 16;
            if (spr >= 0 && spr < (int)citizen_sprites_.size() && !citizen_sprites_[spr].data.empty())
                blit_sprite_rle(0xAD, y0, citizen_sprites_[spr].data.data());
            sh_text(0xC9, 5 + 0x18 * row, (const char*)r + R_NAME, 0x1000);
            const int cls = r[R_CLASS];
            const char* cn = cls < 165 ? (const char*)kPackClassNames + (size_t)cls * 16 : "";
            const int half = (int)strlen(cn) >> 1;
            status_small_text(0xF1 - 6 * half, 0x16 + 0x18 * row, cn, 1);
        }
    };

    // ---- Tail of sub_2EDC9: requirement icon strip of the selected cell ----
    auto draw_req_panel = [&](int selsel, int tab, int cell) {
        const uint8_t* r = party_rec(selsel);
        const int cls = grid_class(tab, cell);
        if (!r || cls < 0) return;
        calc_short(r, cls);
        int row = 0;
        auto put_icon = [&](int slot) {
            if (const uint8_t* p = icon_at(slot)) blit_sprite_rle(4, 9 + row * 0x11, p);
            row++;
        };
        for (int i = 0; i < 10; i++)
            if (req_at(cls, i)) put_icon(2 * i + (short_[i] ? 1 : 0));
        for (int j = 10; j < 16; j++) {
            const int v = req_at(cls, j);
            if (v) put_icon(v * 2 - 2 + (0x14 + 4 * (j - 10)) + (short_[j] ? 1 : 0));
        }
        const int pre = req_at(cls, 16);
        if (pre) put_icon((pre == 2 ? 0x30 : 0x2C) + (short_[16] ? 1 : 0));
        if (req_at(cls, 17)) {
            const std::vector<uint8_t>& s2 = short_[17] == 0 ? g_cls.lvl_ok : g_cls.lvl_bad;
            if (!s2.empty()) blit_sprite_rle(4, 9 + row * 0x11, s2.data());
        }
    };

    // ---- sub_2EDC9 + sub_2F113 ----
    auto compose = [&](int selsel, int tab, int cell) {
        memcpy(bb, status_bg_.data(), screen_bytes);
        draw_party_list(status_top_, selsel);
        draw_req_panel(selsel, tab, cell);

        const uint8_t* r = party_rec(selsel);
        const int srow = sprite_row(selsel);
        int last_w = 0, last_h = 0;
        for (int row = 0; row < 4; row++) {
            for (int c = 0; c < 4; c++) {
                const int cls = grid_class(tab, row * 4 + c);
                if (cls < 0) continue;
                const int spr = spr_of(srow, cls);
                if (spr < 0) continue;
                const int frame = spr * 16;
                if (frame < 0 || frame >= (int)citizen_sprites_.size()) continue;
                const PackSprite& sp = citizen_sprites_[frame];
                if (sp.data.empty()) continue;
                const int w = (int16_t)sp.width, h = (int16_t)sp.height;
                last_w = w; last_h = h;
                const int x = (0x2A + 0x20 * c) - halfv(w);
                const int y = (0x11 + 0x1F * row) - halfv(h);
                blit_sprite_rle(x, y, sp.data.data());
                if (r && !req_ok(r, cls)) dim_rect(x, y, x + w, y + h);
            }
        }
        // Tail of sub_2F113: name label of the selected cell. The original uses the size of
        // the last sprite the loop drew.
        if (r) {
            const int cls = grid_class(tab, cell);
            if (cls < 0) return;
            const char* name = cls < 165 ? (const char*)kPackClassNames + (size_t)cls * 16 : "";
            const int len = (int)strlen(name);
            int x = (0x32 + 0x20 * (cell % 4)) - halfv(last_w) - 5 * (len >> 1);
            const int y = (0x11 + 0x1F * (cell / 4)) - halfv(last_h);
            if (x < 0x1A) x = 0x1A;
            draw_frame_box(x - 2, y - 2, len * 6 + x, y + 5, 4);
            const uint8_t col = tab == 0 ? 0x2D : (tab == 1 ? 0x25 : (tab == 2 ? 0x35 : 1));
            status_small_text(x, y, name, col);
            sel_class = cls;
        }
    };

    // ---- sub_1843D(0x12A, 0x0F+off, 0x139, 0x0F+off+h): scroll thumb of this screen ----
    // The status window's thumb is at x 0x87..0x96; this screen puts it at 0x12A..0x139,
    // beside the party list.
    auto draw_thumb_cc = [&]() {
        const int h = (int)status_thumb_h_;
        const int off = (int)status_thumb_off_;
        draw_frame_box(0x12A, 0x0F + off, 0x139, h + 0x0F + off);
    };

    // ---- sub_2F602 ----
    auto draw_all = [&](int selsel) {
        compose(selsel, tab_cur, cell_cur);
        draw_thumb_cc();
        if (tab_cur >= 0 && tab_cur < 3 && !g_cls.tab_btn[tab_cur].empty())
            blit_sprite_rle(0x1F + tab_cur * 0x20, 0x84, g_cls.tab_btn[tab_cur].data());
        video_.present();
    };

    // ---- sub_2F70E ----
    auto recalc = [&](uint8_t* r) {
        if (!r) return;
        const int cls = r[R_CLASS];
        if (cls < 0 || cls >= CLASS_COUNT) return;
        int w44 = r[R_STAT + 0];
        const int grp = wgroup_of(cls);
        if (grp == 0 || grp == 6) w44 *= 2;
        put16(r, R_STATW + 0, w44);
        put16(r, R_STATW + 2, rec8_word(cls, 0));
        int v54 = r[R_STAT + 2] + rec8_word(cls, 4);
        if ((int16_t)(uint16_t)v54 < 0x3C) v54 = 0x3C;
        put16(r, R_ATS, v54);
        r[R_DEF] = (uint8_t)rec8_byte(cls, 2);
    };

    // sub_2F789 L40502 (entry, id 1) and L41183 (OK success, id 2): sub_394D1 with
    // volume byte_82732*10. sub_18BC5 loads id 1 = menu.VOC and id 2 = sell.VOC.
    auto play_sfx = [&](int idx) { play_pack_sfx(idx); };

    spr_row = sprite_row(ebp);
    tab_cur = tab_of(ebp);
    {
        const int g = grid_class(tab_cur, 0);
        sel_class = g >= 0 ? g : -1;
    }
    if (party_n() < 6) status_thumb_h_ = 123.0f;
    else status_thumb_h_ = (float)((long double)123.0 / ((long double)party_n() / 6.0L));
    status_thumb_off_ = (float)status_top_to_scroll(status_top_, party_n());

    auto load_page = [&]() { return status_load_pcx("cc.pcx", false); };
    if (!load_page()) return 0;
    compose(ebp, tab_cur, cell_cur);
    draw_thumb_cc();
    if (tab_cur >= 0 && tab_cur < 3 && !g_cls.tab_btn[tab_cur].empty())
        blit_sprite_rle(0x1F + tab_cur * 0x20, 0x84, g_cls.tab_btn[tab_cur].data());
    memcpy(status_snap_.data(), bb, screen_bytes);   // sub_1DABB: the finished new screen
    play_sfx(1);

    // loc_2F939: the old screen slides right while the new one is revealed from the left
    // (sub_37F18).
    {
        // sub_37F18(src, w, 0x94, dst) copies a w x 0x94 block from source x=0 to dst_x
        // row by row; it is not one contiguous memcpy.
        auto lincopy = [&](const uint8_t* src, int dst_x, int w) {
            if (w <= 0) return;
            if (dst_x < 0) { w += dst_x; src += -dst_x; dst_x = 0; }
            if (w <= 0 || dst_x >= SCREEN_W) return;
            if (dst_x + w > SCREEN_W) w = SCREEN_W - dst_x;
            const int h = std::min(0x94, SCREEN_H);
            for (int y = 0; y < h; y++)
                memcpy(bb + (size_t)y * SCREEN_W + dst_x, src + (size_t)y * SCREEN_W, (size_t)w);
        };
        int s = 0, e = 0x13E;
        do {
            lincopy(status_bg_.data(), 0, s + s % 4);   // unk_172224 = cc.pcx background
            lincopy(status_prev_.data(), s, e + e % 4);
            s += 2; e -= 2;
            video_.present();
            GameTimer::delay_with_pump(input_, 16);
        } while (s <= 0xA0);
    }

L_restart:
    spr_row = sprite_row(ebp);
    if (!load_page()) return 0;
    memcpy(bb, status_snap_.data(), screen_bytes);
    if (party_n() < 6) status_thumb_h_ = 123.0f;
    else status_thumb_h_ = (float)((long double)123.0 / ((long double)party_n() / 6.0L));
    tab_cur = tab_of(ebp);
    compose(ebp, tab_cur, cell_cur);
    draw_thumb_cc();
    if (tab_cur >= 0 && tab_cur < 3 && !g_cls.tab_btn[tab_cur].empty())
        blit_sprite_rle(0x1F + tab_cur * 0x20, 0x84, g_cls.tab_btn[tab_cur].data());
    video_.present();
    {
        int s2, x2, y2;
        do { if (!poll_mouse(s2, x2, y2)) return 0; } while (s2 == 1);
    }

    int var4 = 1;
    for (;;) {
        int ev = 0, mx = 0, my = 0;
        if (!poll_mouse(ev, mx, my)) return 0;
        if (should_exit_) return 0;
        auto in = [&](int x0, int x1, int y0, int y1) {
            return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
        };

        // loc_30482: close button (0xFC..0x135, 0xBB..0xC5).
        if (ev == 1 && in(0xFC, 0x135, 0xBB, 0xC5)) {
            if (!status_btn_b_.empty()) blit_sprite_rle(0xFC, 0xBB, status_btn_b_.data());
            video_.present();
            int s2, x2, y2;
            do { if (!poll_mouse(s2, x2, y2)) return 0; } while (s2 == 1);
            return 0;
        }

        // ---- 4x4 class cell click ----
        if (ev == 1) {
            bool hit = false;
            for (int row = 0; row < 4 && !hit; row++) {
                for (int c = 0; c < 4; c++) {
                    if (in(0x1D + 0x1E * c, 0x3A + 0x1E * c, row * 0x1F + 2, row * 0x1F + 0x20)) {
                        cell_cur = row * 4 + c;
                        for (int k = 0; k < 18; k++) short_[k] = 0;
                        draw_all(ebp);
                        var4 = 0;
                        hit = true;
                        break;
                    }
                }
            }
        }

        // ---- Hover tooltip on the left requirement strip ----
        if (in(4, 0x18, 9, 0x8F)) {
            const int row = (my - 9) / 0x11;
            const int cls = grid_class(tab_cur, cell_cur);
            int edi = 0, edx = 0;
            do {
                const int q = req_at(cls, edi);
                edi++;
                if (q) edx++;
            } while (edx <= row && edi < 0x14);
            const int bh = (edi >= 1 && edi <= 18) ? short_[edi - 1] : 0;
            edi--;
            if (bh > 0 && edi < 0x14) {
                bool shown = true;
                const int yy = row * 0x11 + 9;
                if (bh == 1 && edi == 0x10) {
                    const int dl = req_at(cls, 0x10);
                    if (dl < bh) shown = false;
                    else if (dl == 1) { if (!g_cls.prev_a.empty()) blit_sprite_rle(4, yy, g_cls.prev_a.data()); else shown = false; }
                    else if (dl == 2) { if (!g_cls.prev_b.empty()) blit_sprite_rle(4, yy, g_cls.prev_b.data()); else shown = false; }
                    else shown = false;
                } else {
                    if (!g_cls.tip_bg.empty()) blit_sprite_rle(4, yy, g_cls.tip_bg.data());
                    status_small_number(8, row * 0x11 + 0x0F, short_[edi], 0x14);
                }
                if (shown) {
                    video_.present();
                    do {
                        int s2, x2, y2;
                        if (!poll_mouse(s2, x2, y2)) return 0;
                        mx = x2;
                        my = y2;
                    } while (!should_exit_ && mx >= 4 && mx <= 0x18 && my >= yy && my <= yy + 0x11);
                    draw_all(ebp);
                }
            }
        }
        if (var4 != 1) { draw_all(ebp); var4 = 1; }
        if (ev != 1) continue;

        // ---- Party list on the right ----
        if (in(0xAB, 0x12B, 8, 0x97)) {
            const int idx = status_top_ + (my - 8) / 0x18;
            if (can_change(idx)) {
                ebp = idx < party_n() ? idx : party_n() - 1;
                for (int k = 0; k < 18; k++) short_[k] = 0;
                goto L_restart;
            }
            // PACK.EXE 0x52073 aBEviEXVJiia: "This character cannot change class!" (Johab bytes)
            show_message_box("\xB7\xA1 \xB7\xA5\xA2\x89\xB7\x65 \xB8\xE5\xBB\xA2 \xA6\x89\x88\x61!");
            draw_all(ebp);
        }
        // ---- Scrollbar ----
        // sub_2F789 loc_2FE12 -> loc_2FFFC: the thumb test covers x and y together, so a
        // click on the bar outside the thumb falls through to the arrow/track tests below.
        else if (mx >= 0x12A && mx <= 0x139 &&
                 (0x0F + (int)status_thumb_off_) <= my &&
                 !((long double)my > (long double)(0x0F + (int)status_thumb_off_) + (long double)status_thumb_h_)) {
            int grab = my;
            for (;;) {
                draw_all(ebp);
                int s2, x2, y2;
                if (!poll_mouse(s2, x2, y2)) return 0;
                if (s2 != 1) break;
                if (grab < y2) {
                    const int d = y2 - grab;
                    status_thumb_off_ += (float)d;
                    grab += d;
                    const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
                    if (fv > 123.0L) {
                        grab = (int)((long double)grab - ((long double)(float)fv - 123.0L));
                        status_thumb_off_ = (float)(123.0L - (long double)status_thumb_h_);
                    }
                    status_top_ = status_scroll_to_top((int)status_thumb_off_, (int)status_thumb_h_, party_n());
                    if (party_n() == 9 && status_top_ == 4) status_top_ = 3;
                } else if (grab > y2) {
                    const int d = grab - y2;
                    status_thumb_off_ -= (float)d;
                    grab -= d;
                    if (status_thumb_off_ < 0.0f) status_thumb_off_ = 0.0f;
                    status_top_ = status_scroll_to_top((int)status_thumb_off_, (int)status_thumb_h_, party_n());
                }
            }
            draw_all(ebp);
        }
        else if (in(0x12A, 0x139, 5, 0x0F)) {
            if (status_top_ > 0) status_top_--;
            status_thumb_off_ = (float)status_top_to_scroll(status_top_, party_n());
            draw_all(ebp);
        }
        else if (in(0x12A, 0x139, 0x8A, 0x93)) {
            if (party_n() - 6 > status_top_) status_top_++;
            status_thumb_off_ = (float)status_top_to_scroll(status_top_, party_n());
            draw_all(ebp);
        }
        else if (in(0x12A, 0x139, 0x0F, 0x8A)) {
            status_thumb_off_ = (float)(my - 0x0F);
            const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
            if (fv > 123.0L) status_thumb_off_ = (float)(123.0L - (long double)status_thumb_h_);
            status_top_ = status_scroll_to_top((int)status_thumb_off_, (int)status_thumb_h_, party_n());
            draw_all(ebp);
        }
        // ---- Tab buttons 0..2 ----
        else if (tab_cur < 3 &&
                 (in(0x1F, 0x34, 0x84, 0x90) || in(0x3F, 0x54, 0x84, 0x90) || in(0x5F, 0x74, 0x84, 0x90))) {
            if (in(0x1F, 0x34, 0x84, 0x90)) tab_cur = 0;
            else if (in(0x3F, 0x54, 0x84, 0x90)) tab_cur = 1;
            else tab_cur = 2;
            for (int k = 0; k < 18; k++) short_[k] = 0;
            draw_all(ebp);
        }
        // ---- OK button ----
        else if (in(0x7F, 0x94, 0x84, 0x90)) {
            if (!g_cls.ok_btn.empty()) blit_sprite_rle(0x7F, 0x84, g_cls.ok_btn.data());
            video_.present();
            GameTimer::delay_with_pump(input_, 33 * 16);   // sub_1199E
            uint8_t* r = party_rec(ebp);
            if (r && sel_class >= 0 && req_ok(r, sel_class)) {
                play_sfx(2);
                r[R_CLASS] = (uint8_t)sel_class;
                const int spr = spr_of(spr_row, sel_class);
                if (spr >= 0) put16(r, R_TOWN, spr);
                // The original tests sub_376D8's result with test eax,eax / jnz (1 = success,
                // -1 = failure). Both are non-zero, so that branch never opens the item list;
                // the port opens the discard screen on failure (-1), matching what EVE
                // loc_130A7 does when the inventory is full.
                for (int k = 0; k < 3; k++) {              // sub_376D8 / sub_37E5E
                    const int off = EQUIP_OFF[k];
                    if (r[off] == 0) continue;
                    if (inv_add_front((int)r[off] - 1) < 0) status_inventory_list();
                    status_equip_strip(r, k + 1);
                }
                if (r[0x5E] != 0) {
                    if (inv_add_front((int)r[0x5E] - 1) < 0) status_inventory_list();
                    r[0x5E] = 0;
                }
                if (r[0x5F] != 0) {
                    if (inv_add_front((int)r[0x5F] - 1) < 0) status_inventory_list();
                    r[0x5F] = 0;
                }
            }
            recalc(r);                                     // always runs (original)
            draw_all(ebp);
        }
        // ---- Bottom row: return to the status window (loc_3042E) ----
        // sub_2B433(backbuffer -> unk_183394) stores this screen as the "previous screen",
        // sets *edx (status top row) = dword_1A40E0 (saved on entry) and *ebx (status_sel_)
        // = ebp, then returns 1. The return transition (sub_32315 loc_3305F) leaves this
        // screen on the left.
        else if (in(0, 0x13F, 0x94, 0xA3)) {
            memcpy(status_prev_.data(), bb, screen_bytes);
            status_sel_ = ebp;            // mov [eax], ebp  (ebx = &status sel)
            status_top_ = saved_status_top;   // mov [edx], eax  (edx = &status top)
            return 1;
        }

        // loc_3047D: wait for the button release, then handle the next event.
        {
            int s2, x2, y2;
            do { if (!poll_mouse(s2, x2, y2)) return 0; } while (s2 == 1);
        }
    }
}
