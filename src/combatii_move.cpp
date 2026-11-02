#include "combatii_move.h"

#include <cstdlib>

namespace g2move {

namespace {
inline int16_t w16(int v) { return (int16_t)(uint16_t)(v & 0xFFFF); }
constexpr size_t kQueueMax = 0xA0;   // sub_13AB1: cmp word ptr dword_EDD7E+2, 0A0h
}

PathResult trace_path(const PathEnv& env, int ux_in, int uy_in, int ufacing, int self_idx, int budget, int x_in, int y_in) {
    PathResult r;
    const int16_t x = w16(x_in), y = w16(y_in);
    const int16_t ux = (int8_t)ux_in, uy = (int8_t)uy_in;     // movsx byte ptr [ebx], [ebx+1]
    size_t qcount = 0;
    // sub_13AB1: beyond 0A0h entries clear the queue and fail.
    auto push = [&](uint8_t px, uint8_t py) -> bool {
        if (qcount >= kQueueMax) { qcount = 0; r.nodes.clear(); return false; }
        r.nodes.push_back({px, py});
        ++qcount;
        return true;
    };
    auto occupant = [&](int ox, int oy) -> int {              // sub_11DB2
        if (!((int)(uint16_t)(ox - 2) < env.width - 4)) return -1;
        if (!((int)(uint16_t)(oy - 4) < env.height - 8)) return -1;
        return env.occupant_raw ? env.occupant_raw(ox, oy) : -1;
    };

    if (x == ux && y == uy) {                                  // already there: push one node with an FFFFh marker
        r.edd76 = ufacing;                                     // movsx ax, byte ptr [ebx+63h]
        push((uint8_t)ux, (uint8_t)uy);
        r.ret = 2;                                             // 13AB1 preserves edx, so edx = 2 is the result
        return r;
    }
    {
        const int occ = occupant(x, y);
        if (occ != self_idx && occ >= 0) return r;             // cmp eax,esi; test eax,eax
    }

    const int W = env.width & 0xFF;                            // byte ptr dword_BA842+2
    const int H = env.height & 0xFF;                           // byte ptr word_BA840
    if (W == 0) return r;
    const int stride = W;
    long cell = (long)stride * (y & 0xFFFF) + x;               // imul eax,edx / add eax,edx (16-bit y is non-negative)
    auto grid = [&](long idx) -> int16_t {
        if (!env.e8 || idx < 0 || (size_t)idx >= env.e8_size) return -1;
        return env.e8[idx];
    };

    // Neighbour offsets D[0..3]: +-1 along axis k (= x==ux ? 1 : 0) and +-W along axis j (= 1-k).
    int16_t D[4] = {0, 0, 0, 0};
    const int k = (x == ux) ? 1 : 0;
    const int j = 1 - k;
    D[2 + k] = 1;
    D[k] = 1;
    if (x > ux) D[k] = -1; else D[2 + k] = -1;
    D[2 + j] = (int16_t)W;
    D[j] = (int16_t)W;
    if (uy < y) D[j] = (int16_t)-W; else D[2 + j] = (int16_t)-W;

    uint8_t cx_ = (uint8_t)x, cy_ = (uint8_t)y;                // var_1C / var_1B
    int16_t ytarget = y;                                       // word ptr [var_1A+2]
    bool first = true;                                         // var_10 (the first step fixes word_EDD76)

    for (;;) {
        const int16_t di = w16(budget);                        // word_EDD72
        const int16_t dx = grid(cell);
        if (dx == di) {
            // Reached the origin value: finish the rest as a straight x-first, then y line.
            const int16_t cxw = ux;                            // movsx cx, byte ptr [eax]
            ytarget = uy;
            for (;;) {
                if ((int)cx_ == (int)cxw && (int)cy_ == (int)(uint16_t)ytarget) { r.ret = 1; return r; }
                if (!push(cx_, cy_)) { r.ret = 0; return r; }
                if ((int)cx_ > (int)cxw) { --cx_; continue; }
                if ((int)cx_ < (int)cxw) { ++cx_; continue; }
                if ((int)cy_ > (int)ytarget) { --cy_; continue; }
                if ((int)cy_ < (int)ytarget) { ++cy_; continue; }
            }
        }
        if (!push(cx_, cy_)) { r.ret = 0; return r; }

        int16_t E[4];
        for (int s = 0; s < 4; ++s) E[s] = w16((int)(uint16_t)grid(cell + D[s]) - (int)(uint16_t)dx);

        int best = 0, besti = 0;                               // var_14 / var_4
        for (int s = 0; s < 4; ++s) {
            if (!((int16_t)best < E[s])) continue;
            const int d = D[s];
            bool valid;
            if (d % W != 0) {                                  // horizontal move
                const int nx = w16((int)cx_ + d);
                valid = !(nx < 0) && nx < W;
            } else {                                           // vertical move
                const int ny = w16((d / W) + (int)cy_);
                valid = !(ny < 0) && ny < H;
            }
            if (valid) { best = (uint16_t)E[s]; besti = s; }
        }
        const int d = D[besti];
        cell += d;
        if (d % W != 0) cx_ = (uint8_t)(cx_ + (uint8_t)d);
        else            cy_ = (uint8_t)(cy_ + (uint8_t)(d / W));
        if (!first) continue;
        if (d % W != 0) r.edd76 = (d > 0) ? 1 : 3;
        else            r.edd76 = (d > 0) ? 2 : 0;
        first = false;
    }
}

bool footprint_ok(const PathEnv& env, int self_idx, int x, int y, int w, int h) {
    auto occupant = [&](int ox, int oy) -> int {
        if (!((int)(uint16_t)(ox - 2) < env.width - 4)) return -1;
        if (!((int)(uint16_t)(oy - 4) < env.height - 8)) return -1;
        return env.occupant_raw ? env.occupant_raw(ox, oy) : -1;
    };
    const int W = env.width & 0xFF;
    for (int row = 0, yy = y; row < h; ++row, --yy) {
        for (int col = 0, xx = x; col < w; ++col, ++xx) {
            const int occ = occupant(xx, yy);
            if (occ >= 0 && occ != self_idx) return false;
            long idx = (long)(env.width) * yy + xx;            // sub_13CBD: dword_BA842 stride
            (void)W;
            int v = -128;
            if (xx >= 0 && yy >= 0 && xx < env.width && yy < env.height && env.e8 && idx >= 0 && (size_t)idx < env.e8_size)
                v = env.e8[idx];
            if (v < 0) return false;
        }
    }
    return true;
}

Box confine_box(int X, int Y) {
    return {X - 0x0A, X + 0x22, Y - 0x0A, Y + 0x1A};
}

int facing_from_pointer(int mx, int my, int X, int Y, int map_kind, int unit_cls, int edd76) {
    const int cx = w16(X + 0x0C), cy = w16(Y + 0x08);         // var_10 / var_C
    int si = w16(cx - mx); if (si < 0) si = w16(-si);          // test si,si ; jge ; neg esi
    int ax = w16(cy - my); if (ax < 0) ax = w16(-ax);
    int f;
    if ((int16_t)si > (int16_t)ax || map_kind == 6) f = (w16(mx) <= (int16_t)cx) ? 1 : 3;
    else f = (w16(my) <= (int16_t)cy) ? 2 : 0;
    if (map_kind == 5 && unit_cls != 0x7F && unit_cls != 0x80) f = edd76;
    return f;
}

// ==== sub_1620A family ====

int walk_sound_type(const WalkEnv& env, const WalkUnitInfo& u, int x, int y) {
    if (u.cls == 0x4C || u.cls == 0x4D) return -1;                    // loc_16131: word EDD7E = FFFFh
    if (env.map_kind == 4) return 4;
    if (env.map_kind == 6) return 6;
    if ((u.code_b >= 7 && u.code_b <= 0x0C) || u.code_b == 0x17) return 8;
    if (u.flags14 & 1) return 7;
    int t = 0;
    const int8_t a = (int8_t)(env.tile_attr ? env.tile_attr(x, y) : 0);   // cmp al,64h; jge (signed compare)
    if (a >= 0x64) {
        const int k = (uint8_t)(a - 0x64) & 0x0F;
        if (k == 0x0B) t = 1;
        else if (k == 1 || k == 4) t = 2;
        else if (k == 2 || k == 3) t = 3;
        else t = 0;
    }
    if (t == 0 && u.code_b >= 1 && u.code_b <= 4) t = 5;              // loc_161EA
    return t;
}

namespace {

// sub_10DA7 (north) / sub_10E2F (south) / sub_10ED1 (west) / sub_10F5C (east): one step of 4px.
// Returns 1 when it scrolled (one frame drawn), 0 when the map edge blocked it.
int scroll_step(const WalkEnv& env, WalkState& st, int dir, const WalkSink& emit) {
    constexpr int S = 4;
    switch (dir) {
    case 2: {   // sub_10DA7: fineY += S; when > 0, camY-- (restore and stop at 0), fineY -= 16
        st.cam_fy += S;
        if (st.cam_fy > 0) {
            st.cam_ty -= 1;
            if (st.cam_ty < 0) { st.cam_ty = 0; st.cam_fy -= S; return 0; }
            st.cam_fy -= 0x10;
        }
        break;
    }
    case 0: {   // sub_10E2F: fineY -= S; at camY == H-12 restore and stop; at fineY == -16, camY++, +16
        st.cam_fy -= S;
        if (st.cam_ty == env.map_h - 0x0C) { st.cam_fy += S; return 0; }
        if (st.cam_fy == -0x10) { st.cam_ty += 1; st.cam_fy += 0x10; }
        break;
    }
    case 1: {   // sub_10ED1: fineX += S; when > 12, camX-- (negative: fineX = 12, stop), fineX -= 24
        st.cam_fx += S;
        if (st.cam_fx > 0x0C) {
            st.cam_tx -= 1;
            if (st.cam_tx < 0) { st.cam_tx = 0; st.cam_fx = 0x0C; return 0; }
            st.cam_fx -= 0x18;
        }
        break;
    }
    default: {  // 3, sub_10F5C: fineX -= S; at camX == W-12 set fineX = 12, stop; fineX <= -12 -> camX++, +24
        st.cam_fx -= S;
        if (st.cam_tx == env.map_w - 0x0C) { st.cam_fx = 0x0C; return 0; }
        if (st.cam_fx <= -0x0C) { st.cam_tx += 1; st.cam_fx += 0x18; }
        break;
    }
    }
    emit(WalkEv::ScrollFrame, 0, st);   // sub_45577
    return 1;
}

struct Walker {
    const WalkEnv& env;
    const WalkUnitInfo& u;
    WalkState& st;
    const WalkSink& emit;
    const uint8_t* table;
    int counter = 3;          // byte_EDD82
    int follow = 0;           // word_EDD70
    static constexpr int kLimX = 0x0C, kLimY = 0x08;   // word_EDD78 / word_EDD7A

    // sub_15C45(dir): move half a tile (12px horizontal, 8px vertical) in 4px steps.
    void half(int dir) {
        if (env.map_kind != 6) {
            st.facing = dir;
        } else if (dir == 1 || dir == 3) {
            const int a = (env.tile_attr ? env.tile_attr(st.ux, st.uy) : 0) & 0xFF;
            if (a % 100 < 0x10) st.facing = dir;
            else st.facing = (dir == 1) ? 0 : 2;
        }
        emit(WalkEv::DrawNoFlip, 0, st);        // sub_454B3
        emit(WalkEv::Timer, 5, st);             // word_71115 = 5
        int si = follow;
        const int lim = (dir == 0 || dir == 2) ? kLimY : kLimX;
        for (int cx = 0;;) {
            switch (dir) {
            case 0: st.off_y += 4; break;
            case 1: st.off_x -= 4; break;
            case 2: st.off_y -= 4; break;
            default: st.off_x += 4; break;
            }
            if (follow) si = scroll_step(env, st, dir, emit);
            if (si == 0) { emit(WalkEv::Draw, 0, st); emit(WalkEv::Draw, 0, st); }   // sub_15BFB: twice
            emit(WalkEv::Tick, 0, st);          // sub_499E4
            cx += 4;
            if (cx >= lim) {
                counter = (counter + 1) & 3;
                st.pose = table[counter];
                break;
            }
        }
    }

    // sub_15EB5: advance one tile toward the next node.
    void step(PathNode n) {
        follow = (st.ux - st.cam_tx == 6 || st.uy - st.cam_ty == 6) ? 1 : 0;
        if (st.uy > (int)n.y) {
            half(2); st.uy -= 1; st.off_y = 8; half(2);
        } else if (st.uy < (int)n.y) {
            half(0); st.uy += 1; st.off_y = -8; half(0);
        } else if (st.ux > (int)n.x) {
            half(1); st.ux -= 1; st.off_x = 12; half(1);
        } else if (st.ux < (int)n.x) {
            half(3); st.ux += 1; st.off_x = -12; half(3);
        }
        // Same tile: the original printf's "Ehh?\n" and returns without changing the screen.
    }
};

}  // namespace

void walk_path(const WalkEnv& env, const WalkUnitInfo& u, WalkState& st,
               std::vector<PathNode> stack, int facing, const WalkSink& emit) {
    static const uint8_t kTableA[4] = {2, 3, 4, 3};   // unk_7110D
    static const uint8_t kTableB[4] = {3, 4, 3, 4};   // unk_71111
    const bool wide = (u.code_b >= 0x0F && u.code_b <= 0x1F);
    if (wide) {
        emit(WalkEv::Timer, 0x10, st);
        st.pose = 2;
        emit(WalkEv::Draw, 0, st);
        emit(WalkEv::Tick, 0, st);
    } else {
        st.pose = 3;
    }
    Walker w{env, u, st, emit, wide ? kTableB : kTableA};
    w.counter = 3;
    st.off_x = st.off_y = 0;
    while (!stack.empty()) {
        const PathNode n = stack.back();
        stack.pop_back();
        const int t = walk_sound_type(env, u, n.x, n.y);
        if (t > -1) emit(WalkEv::Sfx, t + 0x0B, st);
        w.step(n);
    }
    if (facing >= 0) st.facing = facing & 0xFF;
    if (u.code_b != 0) {
        emit(WalkEv::Timer, 0x18, st);
        st.pose = 2; emit(WalkEv::Draw, 0, st); emit(WalkEv::Tick, 0, st);
        st.pose = 1; emit(WalkEv::Draw, 0, st); emit(WalkEv::Tick, 0, st);
    }
}

}  // namespace g2move
