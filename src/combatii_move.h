#pragma once
// Pure path/facing math for the COMBATII.EXE move command: the decision logic of
// sub_15523 / sub_165C2 / sub_16357, called from sub_16834.
//
//   sub_15523: trace the destination -> origin path (gradient descent over the E8F46
//              diffusion grid). Returns rejected(0) / found(1) / already-there(2) plus the
//              new word_EDD76 (last move direction) and the nodes pushed onto the
//              sub_13AB1 queue (E8CC0).
//   sub_165C2: is the whole destination footprint free and is E8F46 >= 0.
//   sub_16357: facing computation for the facing-select loop, plus the mouse confine box.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace g2move {

struct PathEnv {
    int width = 0, height = 0;                // dword_BA842+2 / word_BA840, full values for the sub_11DB2 guard
    const int16_t* e8 = nullptr;              // E8F46+2. Stride is (width & 0xFF); the original reads only that byte
    size_t e8_size = 0;                       // element count; out-of-range reads yield -1
    std::function<int(int, int)> occupant_raw;// occupying unit id (-1 = none); the sub_11DB2 guard applies here
};

struct PathNode { uint8_t x, y; };

struct PathResult {
    int ret = 0;                // sub_15523 result: 0 rejected, 1 path found, 2 destination is the current tile
    int edd76 = -1;             // new word_EDD76 when it changed (-1 = unchanged)
    std::vector<PathNode> nodes;// nodes this call pushed onto the sub_13AB1 queue (E8CC0)
};

// sub_15523(x, y, unit): unit at (ux,uy) facing ufacing; self_idx = unit id, budget = word_EDD72.
PathResult trace_path(const PathEnv& env, int ux, int uy, int ufacing, int self_idx, int budget, int x, int y);

// sub_165C2(x, y, facing, unit): is the whole w x h footprint free (or self) with E8F46 >= 0.
// The original scans columns as +x and rows as -y starting at (x, y).
bool footprint_ok(const PathEnv& env, int self_idx, int x, int y, int w, int h);

// Mouse confine box of sub_16357 (INT 33h AX=7/8); X,Y = screen top-left pixel of the target tile.
struct Box { int x0, x1, y0, y1; };
Box confine_box(int X, int Y);

// sub_16357 loc_1642C: pick the facing from the mouse on event 1/6
// (0 south, 1 west, 2 north, 3 east). map_kind 6 tests the horizontal axis only; map_kind 5
// pins to word_EDD76 unless the class is 7Fh/80h.
int facing_from_pointer(int mx, int my, int X, int Y, int map_kind, int unit_cls, int edd76);

// ==== sub_1620A: player move walk animation ====
// sub_15EB5 / sub_15C45 / sub_16123 / sub_10DA7 / sub_10E2F / sub_10ED1 / sub_10F5C.
// Drawing, timers and sound effects are emitted as events only; the caller applies each state
// snapshot to the live units and camera and draws it.
struct WalkState {
    int ux = 0, uy = 0;          // unit [00h]/[01h] (signed bytes)
    int facing = 0;              // [63h]
    int pose = 0;                // [64h]
    int off_x = 0, off_y = 0;    // [65h]/[66h]
    int cam_tx = 0, cam_ty = 0;  // low/high word of dword_BA83C (camera tile X/Y)
    int cam_fx = 0, cam_fy = 0;  // low word of dword_7001C (fine X), BA838 (fine Y)
};
struct WalkUnitInfo { int code_b = 0; int cls = 0; int flags14 = 0; };   // [5Ch] / [11h] / [14h]
struct WalkEnv {
    int map_w = 0, map_h = 0;                 // word_BA844 / word_BA840
    int map_kind = 0;                         // byte_BA84E
    std::function<int(int, int)> tile_attr;   // sub_1093D: byte unk_88BD4[y*w+x]
};
enum class WalkEv {
    Draw,          // sub_45559 (= sub_454B3 + sub_5E9E8): draw and present
    DrawNoFlip,    // sub_454B3 only (no present)
    ScrollFrame,   // sub_45577: draw one scroll step and present
    Tick,          // sub_499E4: wait one timer tick
    Timer,         // sub_1033B(arg)
    Sfx,           // sub_4F4CD(arg)
};
using WalkSink = std::function<void(WalkEv, int, const WalkState&)>;

// sub_16123: step sound type (-1 = none); (x,y) = the next node tile.
int walk_sound_type(const WalkEnv& env, const WalkUnitInfo& u, int x, int y);

// sub_1620A body. stack = nodes pushed by sub_15523, popped from the back (sub_13AFA).
// facing < 0 leaves [63h] untouched (`test si,si; jl`).
void walk_path(const WalkEnv& env, const WalkUnitInfo& u, WalkState& st,
               std::vector<PathNode> stack, int facing, const WalkSink& emit);

}  // namespace g2move
