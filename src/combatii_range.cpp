#include "combatii_range.h"

#include <cstdlib>

namespace g2range {

namespace {
inline int16_t w16(int v) { return (int16_t)(uint16_t)(v & 0xFFFF); }
inline int abs16(int v) { return v < 0 ? -v : v; }
}  // namespace

// ---- Grid (E8F46) ----

void Range::clear_grid() {  // sub_13F8A: fill every tile with 0xFFFF
    e8.assign((size_t)env_.width * (size_t)env_.height, (int16_t)-1);
}

int Range::grid_read(int x, int y) const {  // sub_13CBD
    if ((uint16_t)x >= (uint16_t)env_.width || (uint16_t)y >= (uint16_t)env_.height) return -128;
    const size_t idx = (size_t)(int16_t)y * (size_t)env_.width + (size_t)(int16_t)x;
    if (idx >= e8.size()) return -128;
    return (int16_t)e8[idx];
}

void Range::grid_write(int x, int y, int v) {  // sub_13D02: x-2 < W-4, y-4 < H-8
    if ((uint16_t)(x - 2) >= (uint16_t)0xFFFF && false) return;
    if (!((int)(uint16_t)(x - 2) < env_.width - 4)) return;
    if (!((int)(uint16_t)(y - 4) < env_.height - 8)) return;
    const long idx = (long)(int16_t)y * env_.width + (int16_t)x;
    if (idx < 0 || (size_t)idx >= e8.size()) return;
    e8[(size_t)idx] = (int16_t)v;
}

int Range::tile_attr(int x, int y) const {  // sub_1093D
    const long idx = (long)(int16_t)y * env_.width + (int16_t)x;
    if (!env_.attr || idx < 0 || idx >= (long)env_.width * env_.height) return 0;
    return env_.attr[idx];
}

int Range::occupant(int x, int y) const {  // sub_11DB2 guard + CC998
    if (!((int)(uint16_t)(x - 2) < env_.width - 4)) return -1;
    if (!((int)(uint16_t)(y - 4) < env_.height - 8)) return -1;
    return env_.occupant_raw ? env_.occupant_raw(x, y) : -1;
}

void Range::step(int16_t& x, int16_t& y, int dir) {  // sub_49ABC (dir > 3 does nothing)
    switch (dir & 0xFF) {
        case 0: y = w16(y + 1); break;
        case 1: x = w16(x - 1); break;
        case 2: y = w16(y - 1); break;
        case 3: x = w16(x + 1); break;
        default: break;
    }
}

// ---- sub_1A146 ----
// Unit position -> front starting tile for a facing. The footprint w/h comes from the
// descriptor's [0]/[1].
Range::Pt Range::front_tile(const Unit& u, int facing) const {
    int16_t x = w16(u.x), y = w16(u.y);
    switch (facing & 0xFFFF) {
        case 0: y = w16(y + 1); break;
        case 1: x = w16(x - 1); y = w16(y - (u.foot_h - 1)); break;
        case 2: x = w16(x + (u.foot_w - 1)); y = w16(y - u.foot_h); break;
        case 3: x = w16(x + u.foot_w); break;
        default: break;
    }
    return {x, y};
}

// ---- sub_1A31D / sub_1A3AE ----
// Snapshot of the iterator globals (F0B92/F0B94/F0B7C(+2)/F0B78(+2)/F0B9A/F0B96/F0B9C/
// F0B98/F0B90). The original also saves F0B80, but the port has no matching field and
// 1A452/1A552/1AC08/1AC59 never read it. The LOS globals (b88/b84hi/b8e/ba4) are outside
// the original snapshot range and are not saved.
Range::IterState Range::save_iter() const {
    return {b92_, b94_, b7c_lo_, b7c_hi_, b78_lo_, b78_hi_,
            b96_, b98_, b9a_, b9c_, b90_};
}

void Range::restore_iter(const IterState& st) {
    b92_ = st.b92; b94_ = st.b94;
    b7c_lo_ = st.b7c_lo; b7c_hi_ = st.b7c_hi;
    b78_lo_ = st.b78_lo; b78_hi_ = st.b78_hi;
    b96_ = st.b96; b98_ = st.b98;
    b9a_ = st.b9a; b9c_ = st.b9c;
    b90_ = st.b90;
}

// ---- sub_1A452 / 1A552 ----
void Range::rect_init(int x, int y, int count, int facing, int range) {
    b92_ = w16(x);
    b94_ = w16(y);
    b7c_lo_ = w16(count);
    int16_t si = w16(range);
    int dir;
    if (si > 0) dir = 1;
    else { dir = -1; si = w16(-si); }
    b7c_hi_ = si;
    b78_hi_ = 0;
    b78_lo_ = 0;
    const int cm1 = w16(count - 1);
    if ((uint16_t)facing <= 3) {
        switch (facing) {
            case 0: b98_ = w16(dir); b9a_ = 1; b96_ = 0; b9c_ = w16(-cm1); break;
            case 1: b9a_ = 0; b96_ = 1; b9c_ = w16(-dir); b98_ = w16(-cm1); break;
            case 2: b9c_ = w16(cm1); b9a_ = -1; b96_ = 0; b98_ = w16(-dir); break;
            case 3: b9c_ = w16(dir); b9a_ = 0; b98_ = w16(cm1); b96_ = -1; break;
        }
    }
    b90_ = 0;
}

bool Range::rect_next(int& x, int& y) {
    if (b90_ != 0) return false;
    x = b92_;
    y = b94_;
    b78_lo_ = w16(b78_lo_ + 1);
    if (b78_lo_ != b7c_lo_) {
        b92_ = w16(b92_ + b9a_);
        b94_ = w16(b94_ + b96_);
        return true;
    }
    b78_lo_ = 0;
    b92_ = w16(b92_ + b9c_);
    b94_ = w16(b94_ + b98_);
    b78_hi_ = w16(b78_hi_ + 1);
    if (b78_hi_ == b7c_hi_) b90_ = 1;
    return true;
}

// ---- sub_1AD62 / 1AE48 ----
void Range::tri3_init(int x, int y, int facing) {
    b94_ = w16(y);
    b92_ = w16(x);
    b78_lo_ = 0;
    if ((uint16_t)facing <= 3) {
        switch (facing) {
            case 0: b98_ = 0; b9a_ = -1; b96_ = -1; b9c_ = 2; break;
            case 1: b9a_ = 1; b96_ = -1; b9c_ = 0; b98_ = 2; break;
            case 2: b98_ = 0; b9a_ = 1; b96_ = 1; b9c_ = -2; break;
            case 3: b9c_ = 0; b9a_ = -1; b96_ = 1; b98_ = -2; break;
        }
    }
    b90_ = 0;
}

bool Range::tri3_next(int& x, int& y) {
    if (b90_ != 0) return false;
    x = b92_;
    y = b94_;
    if (b78_lo_ == 0) {
        b92_ = w16(b92_ + b9a_);
        b94_ = w16(b94_ + b96_);
    } else if (b78_lo_ == 1) {
        b92_ = w16(b92_ + b9c_);
        b94_ = w16(b94_ + b98_);
    } else {
        b90_ = 1;
    }
    b78_lo_ = w16(b78_lo_ + 1);
    return true;
}

// ---- sub_1AC08 / 1AC59 ----
void Range::dia_init(int x, int y, int r) {
    b92_ = w16(x);
    b7c_hi_ = w16(r);
    b90_ = 0;
    b94_ = w16(y - r);
    b7c_lo_ = 1;
    b78_lo_ = 0;
    b78_hi_ = (r != 0 && (int16_t)w16(r) != 0) ? 2 : (int16_t)-2;
}

bool Range::dia_next(int& x, int& y) {
    if (b90_ != 0) return false;
    x = b92_;
    y = b94_;
    b78_lo_ = w16(b78_lo_ + 1);
    if (b7c_lo_ != b78_lo_) {
        b92_ = w16(b92_ + 1);
        return true;
    }
    if (b78_hi_ > 0) b92_ = w16(b92_ - b7c_lo_);
    b94_ = w16(b94_ + 1);
    const int16_t ax = b78_hi_;
    b78_lo_ = 0;
    b7c_lo_ = w16(b7c_lo_ + ax);
    if (ax < 0) b92_ = w16(b92_ - b7c_lo_);
    b7c_hi_ = w16(b7c_hi_ - 1);
    if (b7c_hi_ == 0) b78_hi_ = -2;
    if (b7c_lo_ < 0) b90_ = 1;
    return true;
}

// ---- sub_1AF22 / 1B00D ----
void Range::cone_init(int x, int y, int cx, int facing, int range) {
    b92_ = w16(x);
    b94_ = w16(y);
    b7c_lo_ = w16(cx);
    b7c_hi_ = w16(range);
    b78_lo_ = 0;
    b78_hi_ = 0;
    if ((uint16_t)facing <= 3) {
        switch (facing) {
            case 0: b96_ = 0; b9a_ = 1; b9c_ = w16(-cx); b98_ = 1; break;
            case 1: b9a_ = 0; b96_ = 1; b98_ = w16(-cx); b9c_ = -1; break;
            case 2: b96_ = 0; b9c_ = w16(cx); b9a_ = -1; b98_ = -1; break;
            case 3: b9a_ = 0; b98_ = w16(cx); b96_ = -1; b9c_ = 1; break;
        }
    }
    b90_ = 0;
}

bool Range::cone_next(int& x, int& y) {
    if (b90_ != 0) return false;
    x = b92_;
    y = b94_;
    b78_lo_ = w16(b78_lo_ + 1);
    if (b78_lo_ != b7c_lo_) {
        b92_ = w16(b92_ + b9a_);
        b94_ = w16(b94_ + b96_);
        return true;
    }
    b78_hi_ = w16(b78_hi_ + 1);
    if (b78_hi_ == b7c_hi_) {
        b90_ = 1;
        b78_lo_ = 0;
        return true;
    }
    const int ebx = b7c_hi_ - b78_hi_;          // signed word difference
    const int dx2 = w16(b96_ + b96_);
    const int ax2 = w16(b9a_ + b9a_);
    if (ebx == 1) {
        b7c_lo_ = w16(b7c_lo_ - 2);
        if (b9a_ == 0) b98_ = w16(b98_ + dx2);
        else b9c_ = w16(b9c_ + ax2);
        b92_ = w16(b92_ + b9c_);
        b94_ = w16(b94_ + b98_);
    } else {
        b7c_lo_ = w16(b7c_lo_ + 2);
        b92_ = w16(b92_ + b9c_);
        b94_ = w16(b94_ + b98_);
        if (b9a_ != 0) b9c_ = w16(b9c_ - ax2);
        else b98_ = w16(b98_ - dx2);
    }
    b78_lo_ = 0;
    return true;
}

// ---- List insertion ----
// sub_1A1F8(x, y, army): add the occupant when it is hostile to army and not listed yet.
bool Range::add_by_army(int x, int y, int army) {
    const int occ = occupant(x, y);
    if (occ < 0) return false;
    if (!env_.hostile((int8_t)army, (int8_t)env_.unit_army(occ))) return false;
    for (int v : list) if (v == occ) return false;
    list.push_back(occ);
    return true;
}

// sub_1A25F(x, y, actor): when the occupant has [+14h]&1 set, the actor must have it too.
// The hostility and duplicate checks follow.
bool Range::add_by_actor(int x, int y, const Unit& actor) {
    const int occ = occupant(x, y);
    if (occ < 0) return false;
    if ((env_.unit_flags14(occ) & 1) && !(actor.flags14 & 1)) return false;
    if (!env_.hostile((int8_t)actor.army, (int8_t)env_.unit_army(occ))) return false;
    for (int v : list) if (v == occ) return false;
    list.push_back(occ);
    return true;
}

// sub_1A2D7: F0BA4 = (attr >= 0x64 ? attr - 0x64 : attr) & 0xF0
void Range::bucket_base(int x, int y) {
    uint8_t al = (uint8_t)tile_attr(x, y);
    if (al >= 0x64) al = (uint8_t)(al - 0x64);
    ba4_ = (uint8_t)(al & 0xF0);
}

// sub_1A2FE: (uint8)(cand' - F0BA4) < 0x10
bool Range::bucket_ok(int x, int y) const {
    uint8_t al = (uint8_t)tile_attr(x, y);
    if (al >= 0x64) al = (uint8_t)(al - 0x64);
    return (uint8_t)(al - ba4_) < 0x10;
}

void Range::set_los_mode(int army, int expect, int mode) {  // sub_1B7BF
    b88_ = w16(army);
    b84hi_ = w16(expect);
    b8e_ = w16(mode);
}

int Range::los_range(const Unit& u) const {  // sub_1AEF3
    return (u.cls == 0x15 || u.cls == 0x16) ? 5 : 7;
}

// ---- Line of sight: shadow casting ----
// sub_1B61E: straight-ahead shadow; the cone widens by one tile every two rows.
void Range::shadow_rows(int x0, int y0, int facing, int n) {
    int16_t X = w16(x0), Y = w16(y0);
    int ecx = 1;
    const int16_t nn = w16(n);
    switch (facing & 0xFFFF) {
        case 0:
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sx = X;
                for (int i = 0; (int16_t)i < (int16_t)ecx; ++i) { grid_write(sx, Y, -2); sx = w16(sx + 1); }
                Y = w16(Y + 1);
                if (k & 1) { X = w16(X - 1); ecx += 2; }
            }
            break;
        case 1:
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sy = Y;
                for (int i = 0; (int16_t)i < (int16_t)ecx; ++i) { grid_write(X, sy, -2); sy = w16(sy + 1); }
                X = w16(X - 1);
                if (k & 1) { Y = w16(Y - 1); ecx += 2; }
            }
            break;
        case 2:
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sx = X;
                for (int i = 0; (int16_t)i < (int16_t)ecx; ++i) { grid_write(sx, Y, -2); sx = w16(sx + 1); }
                Y = w16(Y - 1);
                if (k & 1) { X = w16(X - 1); ecx += 2; }
            }
            break;
        case 3:
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sy = Y;
                for (int i = 0; (int16_t)i < (int16_t)ecx; ++i) { grid_write(X, sy, -2); sy = w16(sy + 1); }
                X = w16(X + 1);
                if (k & 1) { Y = w16(Y - 1); ecx += 2; }
            }
            break;
        default: break;
    }
}

// sub_1B1AE: side shadow for the front of the cone (B78_lo <= 1). The start tile is marked
// -2 first.
void Range::shadow_side_a(int x0, int y0, int facing, int n, int side) {
    int16_t X = w16(x0), Y = w16(y0);
    const int16_t nn = w16(n);
    grid_write(X, Y, -2);
    int16_t ecx = 2;
    switch (facing & 0xFFFF) {
        case 0: {
            const int d = side ? -1 : 1;
            int16_t y = w16(Y + 1), x = X;
            for (int16_t k = 1; k < nn; ++k) {
                int16_t sx = x;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(sx, y, -2); sx = w16(sx + d); }
                y = w16(y + 1);
                if (k & 1) x = w16(x + d); else ++ecx;
            }
            break;
        }
        case 1: {
            const int d = side ? -1 : 1;
            int16_t x = w16(X - 1), y = Y;
            for (int16_t k = 1; k < nn; ++k) {
                int16_t sy = y;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(x, sy, -2); sy = w16(sy + d); }
                x = w16(x - 1);
                if (k & 1) y = w16(y + d); else ++ecx;
            }
            break;
        }
        case 2: {
            const int d = side ? 1 : -1;
            int16_t y = w16(Y - 1), x = X;
            for (int16_t k = 1; k < nn; ++k) {
                int16_t sx = x;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(sx, y, -2); sx = w16(sx + d); }
                y = w16(y - 1);
                if (k & 1) x = w16(x + d); else ++ecx;
            }
            break;
        }
        case 3: {
            const int d = side ? 1 : -1;
            int16_t x = w16(X + 1), y = Y;
            for (int16_t k = 1; k < nn; ++k) {
                int16_t sy = y;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(x, sy, -2); sy = w16(sy + d); }
                x = w16(x + 1);
                if (k & 1) y = w16(y + d); else ++ecx;
            }
            break;
        }
        default: break;
    }
}

// sub_1B43A: side shadow for the back of the cone (B78_lo > 1). No start-tile mark, and the
// width grows by one on every odd row.
void Range::shadow_side_b(int x0, int y0, int facing, int n, int side) {
    int16_t X = w16(x0), Y = w16(y0);
    const int16_t nn = w16(n);
    int16_t ecx = 1;
    switch (facing & 0xFFFF) {
        case 0: {
            const int d = side ? -1 : 1;
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sx = X;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(sx, Y, -2); sx = w16(sx + d); }
                Y = w16(Y + 1);
                if (k & 1) ++ecx;
            }
            break;
        }
        case 1: {
            const int d = side ? -1 : 1;
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sy = Y;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(X, sy, -2); sy = w16(sy + d); }
                X = w16(X - 1);
                if (k & 1) ++ecx;
            }
            break;
        }
        case 2: {
            const int d = side ? 1 : -1;
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sx = X;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(sx, Y, -2); sx = w16(sx + d); }
                Y = w16(Y - 1);
                if (k & 1) ++ecx;
            }
            break;
        }
        case 3: {
            const int d = side ? 1 : -1;
            for (int16_t k = 0; k < nn; ++k) {
                int16_t sy = Y;
                for (int16_t i = 0; i < ecx; ++i) { grid_write(X, sy, -2); sy = w16(sy + d); }
                X = w16(X + 1);
                if (k & 1) ++ecx;
            }
            break;
        }
        default: break;
    }
}

// sub_1B7BF + sub_13F8A + sub_1B7FA: the nine call sites (sub_1CC09/1D124/28540/41715/...)
// always run these three steps in this order, so they are combined here. The base tile is the
// actor's front tile (sub_1A1DE).
void Range::los_from_actor(const Unit& u, int expect, int mode, int range) {
    set_los_mode(u.army, expect, mode);
    clear_grid();
    const Pt f = front_tile(u, u.facing);
    los(f.x, f.y, u.facing, 1, range);
}

// sub_1B7FA(x, y, facing, cx, range)
void Range::los(int x_in, int y_in, int facing, int cx, int range) {
    const int16_t xorig = w16(x_in), yorig = w16(y_in);
    int16_t X = xorig, Y = yorig;
    step(X, Y, (facing + 2) % 4);                           // step back to the base tile
    int a = tile_attr(X, Y);
    if (a > 0x64) a -= 0x64;
    uint8_t T = (uint8_t)(a & 0xF0);

    cone_init(xorig, yorig, cx, facing, range);
    int cxx, cyy;
    cone_next(cxx, cyy);                                    // first tile = front tile
    X = w16(cxx);
    Y = w16(cyy);
    a = tile_attr(X, Y);
    T = (uint8_t)(T + 0x10);
    if (a > 0x64) a -= 0x64;
    if ((uint8_t)a == 0) return;
    if ((uint8_t)a > T) return;

    while (cone_next(cxx, cyy)) {
        X = w16(cxx);
        Y = w16(cyy);
        if ((int16_t)grid_read(X, Y) == -2) continue;       // already shadowed
        a = tile_attr(X, Y);
        if (a > 0x64) a -= 0x64;
        if ((uint8_t)a > T && ((uint8_t)a & 0x0F) == 8) a = 0;
        if ((uint8_t)a != 0) {                              // open tile: mark or collect
            if (b8e_ == 0) {
                grid_write(X, Y, 1);
            } else if (b8e_ == 1) {
                const int occ = occupant(X, Y);
                if (occ >= 0 && env_.hostile((int8_t)b88_, (int8_t)env_.unit_army(occ)) == (b84hi_ != 0))
                    list.push_back(occ);
            }
            continue;
        }
        // Blocked tile: cast the shadow for the remaining row count n.
        int n = w16((int)b7c_hi_ - (int)b78_hi_);
        if (b78_lo_ == 0) n = w16(n + 1);
        if (facing == 0 || facing == 2) {
            if (X == xorig) {
                shadow_rows(X, Y, facing, n);
            } else {
                const int side = (facing == 0) ? (X < xorig) : (X > xorig);
                if (b78_lo_ > 1) shadow_side_b(X, Y, facing, n, side);
                else shadow_side_a(X, Y, facing, n, side);
            }
        } else if (facing == 1 || facing == 3) {
            if (Y == yorig) {
                shadow_rows(X, Y, facing, n);
            } else {
                const int side = (facing == 1) ? (yorig > Y) : (yorig < Y);
                if (b78_lo_ > 1) shadow_side_b(X, Y, facing, n, side);
                else shadow_side_a(X, Y, facing, n, side);
            }
        }
    }
}

// ---- sub_1A625: straight scan ----
void Range::line_scan(int x_in, int y_in, int facing, int count, int range) {
    int16_t X = w16(x_in), Y = w16(y_in);
    step(X, Y, (facing + 2) % 4);
    int a = tile_attr(X, Y);
    if (a > 0x64) a -= 0x64;
    const uint8_t var4 = (uint8_t)((a & 0xF0) + 0x18);
    // The second sub_49ABC call moves the saved original (x, y) (var_10/var_14) one more
    // tile forward.
    int16_t sx = w16(x_in), sy = w16(y_in);
    step(sx, sy, facing);
    rect_init(sx, sy, count, facing, range - 1);
    int cx, cy;
    while (rect_next(cx, cy)) {
        a = tile_attr(cx, cy);
        if (a > 0x64) a -= 0x64;
        if ((uint8_t)a == 0 || (uint8_t)a == var4) return;
        if (b8e_ == 0) {
            grid_write(cx, cy, 1);
        } else {
            const int occ = occupant(cx, cy);
            if (occ < 0) continue;
            if ((int)env_.hostile((int8_t)b88_, (int8_t)env_.unit_army(occ)) != (int)b84hi_) continue;
            list.push_back(occ);                            // no duplicate check
        }
    }
}

// ---- sub_1D124 ----
int Range::collect_targets(const Unit& u) {
    return collect_targets_impl(u, u.facing, u.facing);
}

// sub_3EE2B: 1D124(actor, x, y, dir) with only the facing argument replaced. The kind-6
// adjustment reads the unit's [+63h] as in the original, so kind6_facing is passed apart.
int Range::collect_targets_dir(const Unit& u, int dir) {
    return collect_targets_impl(u, dir & 3, u.facing);
}

int Range::collect_targets_impl(const Unit& u, int facing_arg, int kind6_facing) {
    list.clear();
    int facing2;
    if (env_.map_kind == 6) {
        facing2 = (kind6_facing == 0 || kind6_facing == 2) ? (uint8_t)(facing_arg + 1) : facing_arg;
    } else {
        facing2 = facing_arg;
    }
    const Pt ft = front_tile(u, facing2);
    int16_t x = (int16_t)ft.x, y = (int16_t)ft.y;
    const int bl = u.weapon_class & 0xFF;
    bucket_base(u.x, u.y);                                  // sub_1A2D7(unit position)

    int cx_, cy_;
    auto fin = [&]() { return (int)list.size(); };

    switch (bl) {
        case 0: case 1: case 2:
            rect_init(x, y, 1, facing2, 1);
            rect_next(cx_, cy_);
            add_by_actor(cx_, cy_, u);
            return fin();
        case 3:
            rect_init(x, y, 1, facing2, 2);
            while (rect_next(cx_, cy_)) {
                if (!bucket_ok(cx_, cy_)) continue;
                add_by_actor(cx_, cy_, u);
            }
            return fin();
        case 4:
            tri3_init(x, y, facing2);
            while (tri3_next(cx_, cy_)) add_by_actor(cx_, cy_, u);
            return fin();
        case 5:
            set_los_mode(u.army, 1, 1);
            clear_grid();
            los(x, y, facing_arg, 1, los_range(u));
            return fin();
        case 6: case 7: case 8: case 9: case 0x0D:
            rect_init(x, y, 2, facing_arg, (bl == 8 || bl == 0x0D) ? 3 : 2);
            while (rect_next(cx_, cy_)) add_by_army(cx_, cy_, u.army);
            return fin();
        case 0x0B: case 0x0C:
            set_los_mode(u.army, 1, 1);
            line_scan(x, y, facing_arg, 2, bl == 0x0B ? 9 : 7);
            return fin();
        case 0x0E:
            rect_init(x, y, 2, u.facing, 2);
            while (rect_next(cx_, cy_)) add_by_army(cx_, cy_, u.army);
            return fin();
        case 0x0F: case 0x10: {
            int cxp, cyp, r;
            if (bl == 0x10) { cxp = u.x; cyp = u.y; r = 3; }
            else if (u.cls == 0x8C || u.cls == 0x8D) { cxp = x; cyp = y; r = 4; }
            else { cxp = u.x; cyp = u.y; r = 4; }
            dia_init(cxp, cyp, r);
            while (dia_next(cx_, cy_)) {
                const int d = abs16((int16_t)w16(u.x - cx_)) + abs16((int16_t)w16(u.y - cy_));  // sub_49B01
                if (d <= 1) continue;
                add_by_army(cx_, cy_, u.army);
            }
            return fin();
        }
        case 0x11: {
            int16_t px = x, py = y;
            step(px, py, u.facing);
            rect_init(px, py, 3, u.facing, 7);
            while (rect_next(cx_, cy_)) add_by_army(cx_, cy_, u.army);
            return fin();
        }
        case 0x12: {
            for (int pass = 0; pass < 2; ++pass) {
                int16_t px = w16(u.x), py = w16(u.y);
                const int dir = (pass == 0) ? ((u.facing + 1) & 3) : ((u.facing + 3) & 3);
                step(px, py, dir);
                rect_init(px, py, 1, dir, 5);
                rect_next(cx_, cy_);                        // skip the first two tiles
                rect_next(cx_, cy_);
                while (rect_next(cx_, cy_)) add_by_army(cx_, cy_, u.army);
            }
            return fin();
        }
        case 0x13: {
            int16_t px = w16(u.x), py = w16(u.y);
            step(px, py, facing2);
            rect_init(px, py, 1, facing2, 5);
            while (rect_next(cx_, cy_)) add_by_army(cx_, cy_, u.army);
            return fin();
        }
        default:
            return fin();                                    // 0Ah and 14h+: no targets
    }
}

// ---- sub_1CC09 ----
void Range::mark_range(const Unit& u) {
    const int facing_arg = u.facing;
    int facing2;
    if (env_.map_kind == 6) facing2 = (u.facing == 0 || u.facing == 2) ? (uint8_t)(facing_arg + 1) : facing_arg;
    else facing2 = facing_arg;
    const Pt ft = front_tile(u, facing2);
    int16_t x = (int16_t)ft.x, y = (int16_t)ft.y;
    const int dl = u.weapon_class & 0xFF;
    clear_grid();
    int cx_, cy_;
    auto mark_all_rect = [&]() { while (rect_next(cx_, cy_)) grid_write(cx_, cy_, 1); };

    if (dl >= 0x14) return;
    if (dl <= 3) {
        rect_init(x, y, 1, facing2, dl == 3 ? 2 : 1);
        mark_all_rect();
    } else if (dl == 4) {
        tri3_init(x, y, facing2);
        while (tri3_next(cx_, cy_)) grid_write(cx_, cy_, 1);
    } else if (dl == 5) {
        clear_grid();
        set_los_mode(u.army, 1, 0);
        los(x, y, facing_arg, 1, los_range(u));
    } else if (dl <= 9 || dl == 0x0D) {
        rect_init(x, y, 2, facing_arg, (dl == 8 || dl == 0x0D) ? 3 : 2);
        mark_all_rect();
    } else if (dl == 0x0A) {
        // Nothing to paint
    } else if (dl == 0x0B || dl == 0x0C) {
        clear_grid();
        set_los_mode(u.army, 1, 0);
        line_scan(x, y, facing_arg, 2, dl == 0x0B ? 9 : 7);
    } else if (dl == 0x0E) {
        rect_init(x, y, 2, facing_arg, 2);
        mark_all_rect();
    } else if (dl == 0x0F || dl == 0x10) {
        int cxp, cyp, r;
        if (dl == 0x10) { cxp = u.x; cyp = u.y; r = 3; }
        else if (u.cls == 0x8C || u.cls == 0x8D) { cxp = x; cyp = y; r = 4; }
        else { cxp = u.x; cyp = u.y; r = 4; }
        dia_init(cxp, cyp, r);
        while (dia_next(cx_, cy_)) grid_write(cx_, cy_, 1);
        if (dl != 0x10) {
            dia_init(u.x, u.y, 1);
            while (dia_next(cx_, cy_)) grid_write(cx_, cy_, -1);
        }
    } else if (dl == 0x11) {
        int16_t px = x, py = y;
        step(px, py, u.facing);
        rect_init(px, py, 1, u.facing, 7);
        mark_all_rect();
    } else if (dl == 0x12) {
        for (int pass = 0; pass < 2; ++pass) {
            int16_t px = w16(u.x), py = w16(u.y);
            const int dir = (pass == 0) ? ((u.facing + 1) & 3) : ((u.facing + 3) & 3);
            step(px, py, dir);
            rect_init(px, py, 1, dir, 5);
            rect_next(cx_, cy_);
            rect_next(cx_, cy_);
            mark_all_rect();
        }
    } else if (dl == 0x13) {
        int16_t px = w16(u.x), py = w16(u.y);
        step(px, py, facing2);
        rect_init(px, py, 1, facing2, 5);
        mark_all_rect();
    }
}

}  // namespace g2range

namespace g2range {

void Range::area_diamond(const Unit& u, int x, int y, int r) {
    clear_grid();
    list.clear();
    dia_init(x, y, r);
    int cx, cy;
    while (dia_next(cx, cy)) {
        grid_write(cx, cy, r);
        add_by_army(cx, cy, u.army);
    }
}

void Range::collect_block2x2(const Unit& u, int cx, int cy) {
    list.clear();
    rect_init(cx, cy, 2, 0, -2);
    int x, y;
    while (rect_next(x, y)) add_by_army(x, y, u.army);
}

std::vector<Range::Pt> Range::tri3_tiles(int x, int y, int facing) {
    std::vector<Pt> out;
    tri3_init(x, y, facing);
    int cx, cy;
    for (int i = 0; i < 3 && tri3_next(cx, cy); ++i) out.push_back({cx, cy});
    return out;
}

}  // namespace g2range
