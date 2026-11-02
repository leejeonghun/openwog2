// pack_status.cpp - shared base for the PACK status window and equip/class menus.
//
//   sub_32315  status_window            right-click status window (open/list/scroll/buttons/close)
//   sub_31CC8  status_draw_main         background, party list, equip icons, stats, gold, portrait
//   sub_333C7  status_draw_party_list   six-row party list on the left
//   sub_320BB  status_item_popup        right click on an equip icon -> zoom animation + item text
//   sub_3777C  status_item_stats_panel  seven item stat panels
//   sub_3205E / sub_320A4               scroll position <-> top row
//   sub_1843D                           scroll bar (draw_frame_box)
//
// Child screens live in pack_equip.cpp (sub_357C4/sub_367B0), pack_class.cpp (sub_2F789)
// and pack_itemlist.cpp (sub_34261).
//
// Data sources:
//   * Party records are the 0x88-byte blackbox.dat records in g_characters. The original
//     copies only the records whose team byte (+0x87) equals word_1A00CC into 0x19AC24 on
//     entry and writes them back through sub_230AE on exit; this port edits the roster directly.
//   * Icon table off_70A30 (0x1AE x 53), skill slot frame unk_7C784 and hand cursor 0x3115E
//     are read from PACK.EXE VAs at runtime (src/exe_resource.h).
//   * The small font is kPackFont8Width/kPackFont8Rows in pack_data.h (sub_1813C).
#include "pack.h"
#include "pack_data.h"
#include "fileutil.h"
#include "exe_resource.h"
#include "palette.h"
#include "pcx.h"
#include "game_timer.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

namespace {
// Character record fields (0x88 bytes) as used by sub_31CC8/sub_333C7/sub_367B0.
constexpr int R_NAME     = 0x02;
constexpr int R_ID       = 0x0F;
constexpr int R_FACE     = 0x10;
constexpr int R_CLASS    = 0x11;
constexpr int R_LEVEL    = 0x2B;
constexpr int R_SKILL    = 0x2E;   // +0x2E..+0x33: six skill icon bytes
constexpr int R_MAG0     = 0x38, R_MAG1 = 0x39, R_MAG2 = 0x3A, R_MAG3 = 0x3B;
constexpr int R_EXPB     = 0x3D;
constexpr int R_STAT_B0  = 0x40, R_STAT_B1 = 0x41, R_STAT_B2 = 0x42, R_STAT_B3 = 0x43;
constexpr int R_STAT_W0  = 0x44, R_STAT_W1 = 0x46;   // word
constexpr int R_HP       = 0x48, R_HPMAX = 0x4C;     // dword (displayed /100)
constexpr int R_MP       = 0x50, R_MPMAX = 0x52;     // word
constexpr int R_TOWN_SPR = 0x61;                     // word: town sprite index
constexpr int R_TEAM     = 0x87;
constexpr int kEquipOff[5] = { 0x5B, 0x5A, 0x5D, 0x5E, 0x5F };   // left to right on screen
constexpr int kEquipX[5]   = { 0xA2, 0xC0, 0xDF, 0xFC, 0x11A };
constexpr int kIconStride = 0x1AE, kIconCount = 53;
constexpr int kClassNameCount = 165;      // aKnight: 16 bytes x 165 (kPackClassNames)
constexpr int kSkillIconBase = 0x14;      // 2*value + 0x14 + 4*k

inline int rd16(const uint8_t* r, int o) { return (int)(int16_t)(r[o] | (r[o + 1] << 8)); }
inline int rd32(const uint8_t* r, int o) {
    return (int)(int32_t)(r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24));
}
}  // namespace

// ==== Party (equivalent to the 0x19AC24 selection in sub_22FED) ====
int Pack::party_count() const {
    const uint8_t team = (uint8_t)blackbox_slot_;
    int n = 0;
    for (int i = 0; i < MAX_CHARACTERS; i++)
        if (reinterpret_cast<const uint8_t*>(&g_characters[i])[R_TEAM] == team) n++;
    return n;
}

uint8_t* Pack::party_rec(int i) {
    const uint8_t team = (uint8_t)blackbox_slot_;
    for (int j = 0; j < MAX_CHARACTERS; j++) {
        uint8_t* r = reinterpret_cast<uint8_t*>(&g_characters[j]);
        if (r[R_TEAM] != team) continue;
        if (i-- == 0) return r;
    }
    return nullptr;
}

const uint8_t* Pack::party_rec(int i) const {
    return const_cast<Pack*>(this)->party_rec(i);
}

// ==== Small font (sub_18136 color, sub_1813C glyph, sub_18274 text, sub_1822D number) ====
void Pack::status_small_text(int x, int y, const char* s, uint8_t color) {
    uint8_t* bb = video_.get_backbuffer();
    if (!bb || !s) return;
    y -= 1;                                   // sub_18274: set_y(edx-1)
    for (; *s; s++) {
        const int idx = (int)(uint8_t)*s - 0x20;
        if (idx < 0 || idx >= 0x60) continue;
        const int gw = kPackFont8Width[idx];
        for (int row = 0; row < 7; row++) {
            const uint8_t bits = kPackFont8Rows[idx * 8 + row];
            for (int col = 0; col < gw; col++) {
                if (!(bits & (0x80 >> col))) continue;
                const int xx = x + col, yy = y + row;
                if (xx >= 0 && xx < SCREEN_W && yy >= 0 && yy < SCREEN_H)
                    bb[yy * SCREEN_W + xx] = color;
            }
        }
        x += gw;
    }
}

void Pack::status_small_number(int x, int y, int v, uint8_t color) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    status_small_text(x, y, buf, color);
}

// ==== Assets ====
void Pack::status_load_assets() {
    if (status_assets_loaded_) return;
    if (!exe_resource_load(ExeId::Pack, 0x70A30, ExeResLength::Fixed, status_icons_,
                           (size_t)kIconStride * kIconCount))
        fprintf(stderr, "PACK: warning: status window icons (0x70A30) load failed\n");
    if (!exe_resource_load(ExeId::Pack, 0x7C784, ExeResLength::Rle, status_slot_))
        fprintf(stderr, "PACK: warning: status window skill cells (0x7C784) load failed\n");
    if (!exe_resource_load(ExeId::Pack, 0x3115E, ExeResLength::Fixed, status_hand_, 172))
        fprintf(stderr, "PACK: warning: status window hand cursor (0x3115E) load failed\n");
    if (!exe_resource_load(ExeId::Pack, 0x7C220, ExeResLength::Rle, status_btn_a_))
        fprintf(stderr, "PACK: warning: status window buttons (0x7C220) load failed\n");
    if (!exe_resource_load(ExeId::Pack, 0x7C4D2, ExeResLength::Rle, status_btn_b_))
        fprintf(stderr, "PACK: warning: status window close button (0x7C4D2) load failed\n");
    status_bg_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    status_prev_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    status_snap_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    status_assets_loaded_ = true;
}

// sub_2E46B (status.pcx + palette) / sub_2E392 (image only)
bool Pack::status_load_pcx(const char* name, bool with_palette) {
    status_bg_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    std::string path = file_find_case_insensitive(GAME_DATA_DIR, name);
    if (path.empty() || !file_exists(path.c_str())) path = game_path(name);
    const bool ok = !path.empty() && file_exists(path.c_str()) &&
                    pcx_load_to_buffer(path, status_bg_.data(), SCREEN_W, SCREEN_H);
    if (!ok) {
        fprintf(stderr, "PACK: error: cannot read %s\n", name);
        return false;
    }
    if (with_palette) pcx_load_palette(path.c_str());
    return true;
}

// sub_24C7F: portsmal.syf -> RLE (the original also skips the w/h header)
static bool load_small_portrait_rle(const std::string& path, int id, std::vector<uint8_t>& out) {
    out.clear();
    if (id < 0) return false;
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    do {
        if (fseek(fp, 0x80 + (long)id * 4, SEEK_SET) != 0) break;
        uint32_t off = 0;
        if (fread(&off, 4, 1, fp) != 1) break;
        if (fseek(fp, (long)off + 12, SEEK_SET) != 0) break;   // +4(w/h)+8(skip)
        out.resize(0x2710);
        const size_t n = fread(out.data(), 1, out.size(), fp);
        out.resize(n);
        ok = n > 4;
    } while (false);
    fclose(fp);
    return ok;
}

void Pack::status_gold_box_at(int x, int y) {
    load_shop_assets();
    if (!gold_box_data_.empty()) blit_sprite_rle(x, y, gold_box_data_.data());
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)team_gold_);
    const int len = (int)strlen(buf);
    status_small_number(x + 0x23 - 5 * len, y + 5, (int)team_gold_, 1);
}

// unk_9922E[...]: sub_18B2A adds [+2] += 0x10 and [+4] += 0x0C to every item.spr
// frame and sets [+6] = -[+2], so x = x_offset + 0x0C and y = -(height + 0x10).
void Pack::status_equip_icon(int x, int item1) {
    if (item1 < 1 || item1 > (int)item_sprites_.size()) return;
    const PackSprite& sp = item_sprites_[item1 - 1];
    if (sp.data.empty()) return;
    blit_sprite_rle(x + sp.x_offset + 0x0C, 0x8F - (sp.height + 0x10), sp.data.data());
}

// ==== Party list (sub_333C7) ====
void Pack::status_draw_party_list(int top, int sel) {
    const int count = party_count();
    for (int row = 0; row < 6; row++) {
        const int idx = top + row;
        const int y0 = 4 + 0x18 * row;
        if (idx == sel) draw_frame_box(6, y0, 0x86, 0x1C + 0x18 * row);
        if (idx >= count) continue;
        const uint8_t* rec = party_rec(idx);
        if (!rec) continue;
        // dword_921AE[word(+0x61)*64] -> first frame of a town sprite (16 frames per index)
        const int frame = rd16(rec, R_TOWN_SPR) * 16;
        if (frame >= 0 && frame < (int)citizen_sprites_.size() &&
            !citizen_sprites_[frame].data.empty())
            blit_sprite_rle(0x0A, y0, citizen_sprites_[frame].data.data());
        sh_text(0x26, 5 + 0x18 * row, (const char*)rec + R_NAME, 0x1000);
        const int cls = rec[R_CLASS];
        const char* cn = cls < kClassNameCount
                             ? (const char*)kPackClassNames + (size_t)cls * 16 : "";
        const int half = (int)strlen(cn) >> 1;
        status_small_text(0x4E - 6 * half, 0x16 + 0x18 * row, cn, 1);
    }
}

// ==== Status window body (sub_31CC8, calls sub_333C7) ====
void Pack::status_draw_main(int sel, int top) {
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    memcpy(bb, status_bg_.data(), status_bg_.size());
    status_draw_party_list(top, sel);
    const int count = party_count();
    if (sel < 0 || sel >= count) return;
    const uint8_t* rec = party_rec(sel);
    if (!rec) return;

    for (int k = 0; k < 5; k++) {
        const int item = rec[kEquipOff[k]];
        if (item) status_equip_icon(kEquipX[k], item);
    }

    char buf[48];
    status_small_text(0xE1, 0x21, "HP:", 0x1B);
    status_small_text(0xE1, 0x2A, "MP:", 0x21);
    status_small_text(0xDC, 0x33, "LV:     EXP:", 0x11);
    // Original format strings aDD_0/aDD_1: "%d /\t%d" and "%d\t   %d". '\t' has no glyph
    // and advances by 0 (sub_1813C), so the literal bytes must be kept for spacing.
    snprintf(buf, sizeof(buf), "%d /\t%d", rd32(rec, R_HP) / 100, rd32(rec, R_HPMAX) / 100);
    status_small_text(0xF8, 0x21, buf, 1);
    snprintf(buf, sizeof(buf), "%d /\t%d", rd16(rec, R_MP), rd16(rec, R_MPMAX));
    status_small_text(0xF8, 0x2A, buf, 1);
    snprintf(buf, sizeof(buf), "%d\t   %d", (int)rec[R_LEVEL], (int)rec[R_EXPB]);
    status_small_text(0xF3, 0x33, buf, 1);

    sh_text(0xDC, 0x0B, (const char*)rec + R_NAME, 0x1000);
    status_small_number(0xBC, 0x43, rd16(rec, R_STAT_W0), 1);
    status_small_number(0xEC, 0x43, rec[R_STAT_B0], 1);
    status_small_number(0x11A, 0x43, rec[R_STAT_B1], 1);
    status_small_number(0xBC, 0x57, rd16(rec, R_STAT_W1), 1);
    status_small_number(0xEC, 0x57, rec[R_STAT_B2], 1);
    status_small_number(0x11A, 0x57, rec[R_STAT_B3], 1);

    status_gold_box_at(0xBD, 0x86);

    // sub_24C7F + sub_2B240(x=0xA2, y=9)
    std::vector<uint8_t> rle;
    std::string ppath = file_find_case_insensitive(GAME_DATA_DIR, "portsmal.syf");
    if (ppath.empty() || !file_exists(ppath.c_str())) ppath = game_path("portsmal.syf");
    if (load_small_portrait_rle(ppath, rec[R_FACE], rle))
        blit_sprite_rle(0xA2, 9, rle.data());
}

// ==== Skill/magic grid (sub_32315 loc_32603..loc_32AAD) ====
static const uint8_t* icon_ptr(const std::vector<uint8_t>& icons, int slot) {
    if (slot < 0 || slot >= (int)(icons.size() / 0x1AE)) return nullptr;
    return icons.data() + (size_t)slot * 0x1AE;
}

void Pack::status_draw_skill_grid(const uint8_t* rec, int x0) {
    if (!rec) return;
    if (!status_slot_.empty())
        for (int i = 0; i < 6; i++)
            blit_sprite_rle(x0 + 48 * (i % 3), 0x3D + 21 * (i / 3), status_slot_.data());
    int n = 0;
    for (int k = 0; k < 6; k++) {
        const int b = rec[R_SKILL + k];
        if (b <= 0) continue;
        const uint8_t* ic = icon_ptr(status_icons_, 2 * b + kSkillIconBase + 4 * k);
        if (ic) blit_sprite_rle(x0 + 2 + 48 * (n % 3), 0x3E + 21 * (n / 3), ic);
        n++;
    }
    // Element icon: first non-zero of +0x38 (12), +0x3A (14), +0x39 (16), +0x3B (18)
    const int bx = x0 + 2 + 48 * (n % 3), by = 0x3E + 21 * (n / 3);
    const int nx = x0 + 0x1C + 48 * (n % 3), ny = 0x44 + 21 * (n / 3);
    static const int kMagOff[4]  = { R_MAG0, R_MAG2, R_MAG1, R_MAG3 };
    static const int kMagSlot[4] = { 12, 14, 16, 18 };
    for (int m = 0; m < 4; m++) {
        const int v = rec[kMagOff[m]];
        if (v <= 0) continue;
        const uint8_t* ic = icon_ptr(status_icons_, kMagSlot[m]);
        if (ic) blit_sprite_rle(bx, by, ic);
        status_small_number(nx, ny, v, 1);
        break;
    }
}

// ==== Item stat panel (sub_3777C) ====
//   icons 0/2/4/6 -> (0x46/0x78/0xAA/0xDC, 0x55), item.spr 9/84 -> (0x46/0x78, 0x69),
//   icon 10 -> (0xAA, 0x69); the seven values are signed bytes at ITEM.DAT +0x28..+0x2E
void Pack::status_item_stats_panel(int item1) {
    load_item_data();
    int v[7] = {0, 0, 0, 0, 0, 0, 0};
    const int id0 = item1 - 1;
    if (id0 >= 0 && id0 < MAX_ITEMS)
        for (int i = 0; i < 7; i++) v[i] = (int8_t)item_data_[id0][0x28 + i];

    static const int kIcX[4] = { 0x46, 0x78, 0xAA, 0xDC };
    static const int kIcSlot[4] = { 0, 2, 4, 6 };
    for (int i = 0; i < 4; i++)
        if (const uint8_t* ic = icon_ptr(status_icons_, kIcSlot[i]))
            blit_sprite_rle(kIcX[i], 0x55, ic);
    if (item_sprites_.size() > 9 && !item_sprites_[9].data.empty())
        blit_sprite_rle(0x46, 0x69, item_sprites_[9].data.data());
    if (item_sprites_.size() > 84 && !item_sprites_[84].data.empty())
        blit_sprite_rle(0x78, 0x69, item_sprites_[84].data.data());
    if (const uint8_t* ic = icon_ptr(status_icons_, 10))
        blit_sprite_rle(0xAA, 0x69, ic);

    static const int kNumX[4] = { 0x5F, 0x91, 0xC3, 0xF5 };
    for (int i = 0; i < 4; i++) status_small_number(kNumX[i], 0x5A, v[i], 1);
    for (int i = 0; i < 3; i++) status_small_number(kNumX[i], 0x6E, v[4 + i], 1);
}

// ==== Item description popup (sub_320BB) ====
//   Grows a box from the mouse position to (0x24,0x3B)-(0x118,0x82) in 10 steps, then draws
//   the dimming, frame, name and stats.
void Pack::status_item_popup(int item1) {
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    int st = 0, mx = input_.get_mouse_x(), my = input_.get_mouse_y();
    int x1 = mx, y1 = my, x2 = mx, y2 = my;
    const int dx1 = (0x24 - x1) / 10, dy1 = (0x3B - y1) / 10;
    const int dx2 = (0x118 - x1) / 10, dy2 = (0x82 - y1) / 10;
    memcpy(status_prev_.data(), bb, status_prev_.size());
    for (int i = 0; i < 10; i++) {
        if (i > 0) {   // sub_18303: rectangle outline
            sh_hline(x1, y1, x2 - x1, 1); sh_hline(x1, y2, x2 - x1, 1);
            sh_vline(x1, y1, y2 - y1, 1); sh_vline(x2, y1, y2 - y1, 1);
        }
        video_.present();
        GameTimer::delay_with_pump(input_, 16);
        x1 += dx1; y1 += dy1; x2 += dx2; y2 += dy2;
    }
    memcpy(bb, status_prev_.data(), status_prev_.size());
    render_dialog_dimming(0x24, 0x3B);
    if (!dialog_frame_data_.empty()) blit_sprite_rle(0x1E, 0x32, dialog_frame_data_.data());
    if (item1 >= 1 && item1 <= MAX_ITEMS) {
        const char* name = (const char*)item_data_[item1 - 1] + 1;
        const int len = (int)strlen(name);
        sh_text(0xA0 - (len / 2) * 8, 0x41, name, 0x1000);
    }
    status_item_stats_panel(item1);
    video_.present();
    // sub_2E6CF: wait for left release -> next left press -> release
    do { if (!poll_mouse(st, mx, my)) return; } while (st == 1);
    do { if (!poll_mouse(st, mx, my)) return; } while (st != 1);
    do { if (!poll_mouse(st, mx, my)) return; } while (st == 1);
    memcpy(bb, status_prev_.data(), status_prev_.size());
    video_.present();
}

// ==== Scroll math (sub_3205E / sub_320A4) and bar ====
float Pack::status_thumb_height(int count) {
    if (count < 6) return 123.0f;
    return (float)((long double)123.0 / ((long double)count / (long double)6.0));
}

int Pack::status_scroll_to_top(int off, int h, int count) {
    if (h == 0x7B) return 0;
    const long double q = (long double)(0x7B - h) / (long double)off;   // off==0 -> +inf -> 0
    return (int)((long double)(count - 5) / q);
}

int Pack::status_top_to_scroll(int top, int count) {
    if (top == 0) return 0;
    const long double q = (long double)count / (long double)top;
    return (int)((long double)0x7B / q);
}

void Pack::status_draw_thumb() {
    const int h = (int)status_thumb_h_;
    const int off = (int)status_thumb_off_;
    draw_frame_box(0x87, 0x0F + off, 0x96, h + 0x0F + off);
}

void Pack::status_hand_cursor() {
    if (status_hand_.empty()) return;
    sh_raw_blit(input_.get_mouse_x(), input_.get_mouse_y(), status_hand_);
}

// ==== Return transition from a child screen (sub_37F18) ====
// Same convention as CHP sub_2F09A: the class screen (left button) slides in from the
// right, the equip screen (right button) from the left.
void Pack::status_slide_back(int sel, bool from_item_list) {
    uint8_t* bb = video_.get_backbuffer();
    if (!bb) return;
    status_load_pcx("status.pcx", false);
    status_draw_main(sel, status_top_);
    status_draw_thumb();
    memcpy(status_snap_.data(), bb, status_snap_.size());   // unk_172224 <- new status window
    const size_t total = (size_t)SCREEN_W * SCREEN_H;

    // sub_37F18(src, w, h, dst): rectangle copy with a 0x140 stride. The original trusts
    // w/h and copies blindly; this port clips to the screen.
    auto rect_copy = [&](const uint8_t* src, int src_x, int dst_x, int w, int h) {
        if (w <= 0 || h <= 0) return;
        int x0 = std::max(0, -dst_x);              // columns to skip on the left
        int x1 = std::min(w, SCREEN_W - dst_x);
        if (x0 >= x1) return;
        h = std::min(h, SCREEN_H);
        const int sx = std::max(0, src_x + x0);
        const int dx = dst_x + x0;
        const int n = std::min(x1 - x0, SCREEN_W - sx);
        if (n <= 0) return;
        for (int y = 0; y < h; y++)
            memcpy(bb + (size_t)y * SCREEN_W + dx, src + (size_t)y * SCREEN_W + sx, (size_t)n);
    };

    if (from_item_list) {
        // loc_3305F (return from the class screen):
        //   sub_37F18(unk_183394, 0xA0, 0x94, backbuffer)         ; left half of the old screen
        //   sub_37F18(unk_172224, (esi%4)+ebp, 0x94, backbuffer+0x9E-edi)
        //   esi=-2, ebp=0x9E, edi=0, each +2 per step while edi <= 0xA0.
        int esi = -2, ebp = 0x9E, edi = 0;
        do {
            memcpy(bb, status_prev_.data(), total);
            rect_copy(status_prev_.data(), 0, 0, 0xA0, 0x94);       // left half of the old screen
            const int rem = (esi < 0) ? -2 : (esi & 3);              // idiv remainder (-2 when negative)
            rect_copy(status_snap_.data(), 0, 0x9E - edi, ebp + rem, 0x94);
            esi += 2; ebp += 2; edi += 2;
            video_.present();
            GameTimer::delay_with_pump(input_, 16);
        } while (edi <= 0xA0);
    } else {
        // loc_331FD (return from the equip screen):
        //   sub_37F18(unk_183434 = old screen + 0xA0, 0xA0, 0x94, backbuffer + 0xA0)
        //   sub_37F18(unk_1722C4 = new screen + 0xA0, (esi%4)+ebp, 0x94, backbuffer)
        //   esi=0, ebp=0xA0, edi=new screen + 0xA0; +2 / -2 per step while esi <= 0xA0.
        int esi = 0, ebp = 0xA0;
        do {
            memcpy(bb, status_prev_.data(), total);
            rect_copy(status_prev_.data(), 0xA0, 0xA0, 0xA0, 0x94);  // keep the right half of the old screen
            // Src = unk_1722C4 - 2k = new screen x 0xA0-esi, width = ebp + esi%4
            rect_copy(status_snap_.data(), 0xA0 - esi, 0, ebp + (esi & 3), 0x94);
            esi += 2; ebp += 2;
            video_.present();
            GameTimer::delay_with_pump(input_, 16);
        } while (esi <= 0xA0);
    }
    status_load_pcx("status.pcx", false);   // sub_2E392 (status.pcx -> unk_172224)
}

// ==== sub_32315: right-click status window ====
void Pack::status_window() {
    load_item_data();
    load_shop_assets();
    status_load_assets();
    if (item_sprites_.empty()) load_standard_sprites(game_path("item.spr"), item_sprites_);

    status_sel_ = 0;
    status_top_ = 0;
    status_thumb_off_ = 0.0f;
    status_thumb_h_ = status_thumb_height(party_count());

    // sub_319DB: wait until the right button is released
    wait_right_released();
    if (!status_load_pcx("status.pcx", true)) return;

    status_draw_main(status_sel_, status_top_);
    status_draw_thumb();
    video_.present();

    int flag = 0;
    auto count = [&]() { return party_count(); };
    auto in = [&](int x0, int x1, int y0, int y1, int mx, int my) {
        return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
    };
    auto redraw = [&]() {
        status_draw_main(status_sel_, status_top_);
        status_draw_thumb();
        video_.present();
    };
    auto wait_left_release = [&]() {
        int st, mx, my;
        do { if (!poll_mouse(st, mx, my)) return; } while (st == 1);
    };

    for (;;) {
        int st = 0, mx = 0, my = 0;
        if (!poll_mouse(st, mx, my)) break;
        // loc_33310: left click on the close button (0xFC..0x135, 0xBB..0xC5) exits
        if (st == 1 && in(0xFC, 0x135, 0xBB, 0xC5, mx, my)) break;

        if (st == 2) {                                   // right click: equip description
            if (in(0xA2, 0x135, 0x67, 0x81, mx, my)) {
                const int idx = (mx - 0xA2) / 0x1E;
                const uint8_t* rec = (status_sel_ >= 0 && status_sel_ < count()) ? party_rec(status_sel_) : nullptr;
                if (idx <= 4 && rec) {
                    const int item = rec[kEquipOff[idx]];
                    if (item) status_item_popup(item);
                }
            }
            redraw();
            wait_left_release();
        }

        if (st != 1 && flag == 0) continue;   // loc_332EB
        flag = 0;

        if (in(8, 0x88, 5, 0x93, mx, my)) {              // party list selection
            const int t = status_top_ + (my - 4) / 0x18;
            status_sel_ = t;
            if (count() <= t) status_sel_ = count() - 1;
        }
        if (in(0xFC, 0x135, 0x87, 0x91, mx, my)) {       // skill/magic grid (while held)
            const uint8_t* rec = (status_sel_ >= 0 && status_sel_ < count()) ? party_rec(status_sel_) : nullptr;
            if (rec) status_draw_skill_grid(rec, 0xA6);
            if (!status_btn_a_.empty()) blit_sprite_rle(0xFC, 0x87, status_btn_a_.data());
            video_.present();
            int s2, x2, y2;
            do { if (!poll_mouse(s2, x2, y2)) return; } while (s2 == 1);
            redraw();
            wait_left_release();
            continue;
        }
        if (mx >= 0x87 && mx <= 0x98) {                   // scroll bar
            const int yy = 0x0F + (int)status_thumb_off_;
            if (yy <= my && !((long double)my > (long double)yy + (long double)status_thumb_h_)) {
                int grab = my;
                for (;;) {
                    status_draw_main(status_sel_, status_top_);
                    status_draw_thumb();
                    status_hand_cursor();
                    video_.present();
                    int s2, x2, y2;
                    if (!poll_mouse(s2, x2, y2)) return;
                    if (s2 != 1) break;
                    if (grab < y2) {                      // down
                        const int d = y2 - grab;
                        status_thumb_off_ += (float)d;
                        grab += d;
                        const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
                        if (fv > 123.0L) {
                            grab = (int)((long double)grab - ((long double)(float)fv - 123.0L));
                            status_thumb_off_ = (float)(123.0L - (long double)status_thumb_h_);
                        }
                        status_top_ = status_scroll_to_top((int)status_thumb_off_, (int)status_thumb_h_, count());
                        if (count() == 9 && status_top_ == 4) status_top_ = 3;
                    } else if (grab > y2) {               // up
                        const int d = grab - y2;
                        status_thumb_off_ -= (float)d;
                        grab -= d;
                        if (status_thumb_off_ < 0.0f) status_thumb_off_ = 0.0f;
                        status_top_ = status_scroll_to_top((int)status_thumb_off_, (int)status_thumb_h_, count());
                    }
                }
                redraw();
                wait_left_release();
                continue;
            }
        }
        if (in(0x87, 0x98, 5, 0x0F, mx, my)) {            // up arrow
            if (status_top_ > 0) status_top_--;
            status_thumb_off_ = (float)status_top_to_scroll(status_top_, count());
            redraw(); wait_left_release(); continue;
        }
        if (in(0x87, 0x98, 0x8A, 0x93, mx, my)) {         // down arrow
            if (count() - 6 > status_top_) status_top_++;
            status_thumb_off_ = (float)status_top_to_scroll(status_top_, count());
            redraw(); wait_left_release(); continue;
        }
        if (in(0x87, 0x98, 0x0F, 0x8A, mx, my)) {         // bar track
            status_thumb_off_ = (float)(my - 0x0F);
            const long double fv = (long double)status_thumb_off_ + (long double)status_thumb_h_;
            if (fv > 123.0L) status_thumb_off_ = (float)(123.0L - (long double)status_thumb_h_);
            status_top_ = status_scroll_to_top((int)status_thumb_off_, (int)status_thumb_h_, count());
            redraw(); wait_left_release(); continue;
        }
        // Deviation: sub_32315 reopens a child screen from the coordinates alone when
        // var_18 (the return value) is non-zero, because it does not poll mouse events during
        // the animation and reuses the previous frame's position. Reopening straight from the
        // return animation would let the same screen pop back up, so entry is limited to a
        // frame where the left button is actually pressed (st == 1).
        if (st == 1 && in(0, 0x4E, 0x94, 0xA3, mx, my)) {  // bottom left: class screen (sub_2F789)
            video_.present();
            memcpy(status_prev_.data(), video_.get_backbuffer(), status_prev_.size());
            const int r = status_class_screen(status_sel_);
            flag = r;
            if (r == 0) break;
            if (r == -1) {
                // PACK.EXE 0x520F3 aBEviEXVJiia_0: "cannot change class" (Johab)
                show_message_box("\xB7\xA1 \xB7\xA5\xA2\x89\xB7\x65 \xB8\xE5\xBB\xA2 \xA6\x89\x88\x61!");
                redraw();
                wait_left_release();
                continue;
            }
            status_slide_back(status_sel_, true);
            continue;
        }
        if (st == 1 && in(0xEE, 0x13F, 0x94, 0xA3, mx, my)) {   // bottom right: equip screen (sub_357C4)
            video_.present();
            memcpy(status_prev_.data(), video_.get_backbuffer(), status_prev_.size());
            const int r = status_equipment_screen(status_sel_);
            flag = r;
            if (r == 0) break;
            status_slide_back(status_sel_, false);
            continue;
        }
        redraw();
        wait_left_release();
    }

    // loc_33359: show the pressed close button, then clean up
    if (!status_btn_b_.empty()) blit_sprite_rle(0xFC, 0xBB, status_btn_b_.data());
    video_.present();
    wait_left_release();
    uint8_t* bb = video_.get_backbuffer();
    memset(bb, 0, (size_t)SCREEN_W * SCREEN_H);
    video_.present();
    palette_set_range(0, 255, town_palette_);   // sub_20118: restore the town palette
}
