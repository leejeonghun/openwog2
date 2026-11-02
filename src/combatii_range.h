#pragma once
// COMBATII.EXE range/target-list core: sub_1D124, sub_1CC09 and the iterators they use.
//
// The original keeps its state in globals (F0B78..F0B9C, F0B84/88/8E, F0BA4, the F0648
// list and the E8F46 grid). This module moves that state into a standalone class that
// reproduces the same call order and the same 16-bit wrapping, and reaches game state
// through the Env callbacks only.
//
//   front tile   sub_1A146           -> front_tile()        uses footprint w/h
//   rectangle    sub_1A452/1A552     -> rect_init/rect_next
//   three-tile   sub_1AD62/1AE48     -> tri3_init/tri3_next
//   diamond      sub_1AC08/1AC59     -> dia_init/dia_next
//   cone         sub_1AF22/1B00D     -> cone_init/cone_next
//   list insert  sub_1A1F8/1A25F     -> add_by_army/add_by_actor
//   bucket gate  sub_1A2D7/1A2FE     -> bucket_base/bucket_ok
//   straight     sub_1A625           -> line_scan           classes 0Bh/0Ch
//   LOS shadow   sub_1B7FA           -> los                 helpers 1B61E/1B1AE/1B43A
//   targets      sub_1D124           -> collect_targets
//   range grid   sub_1CC09           -> mark_range          paints E8F46
#include <cstdint>
#include <functional>
#include <vector>

namespace g2range {

// Unit summary needed for range math (BattleUnit offsets in brackets).
struct Unit {
    int x = 0, y = 0;        // [+0], [+1] signed byte
    int army = 0;            // [+0Eh] effective army, signed byte
    int cls = 0;             // [+11h] unit class
    int flags14 = 0;         // [+14h]
    int facing = 0;          // [+63h] 0 = south, 1 = west, 2 = north, 3 = east
    int weapon_class = 0;    // result of sub_174CF (0..0x13)
    int foot_w = 1, foot_h = 1;  // unk_D66A8 descriptor [0]/[1]
};

struct Env {
    int width = 0, height = 0;               // high word of dword_BA842 / word_BA840
    int map_kind = 0;                        // byte_BA84E (6 adds 1 to facings 0 and 2)
    const uint8_t* attr = nullptr;           // unk_88BD4 (width*height)
    // sub_11DB2: occupying unit id, -1 when empty. The caller applies the (x-2, y-4) guard.
    std::function<int(int, int)> occupant_raw;
    std::function<int(int)> unit_army;       // occupant's [+0Eh]
    std::function<int(int)> unit_flags14;    // occupant's [+14h]
    std::function<bool(int, int)> hostile;   // sub_1201D(a, b), including the E518D inversion
};

class Range {
public:
    explicit Range(const Env& e) : env_(e) {}

    struct Pt { int x, y; };

    // Public state (globals in the original)
    std::vector<int> list;                   // F0648; F0B9C+2 holds the count (= list.size())
    std::vector<int16_t> e8;                 // E8F46+2 (width*height, -1 = 0xFFFF)

    void clear_grid();                       // sub_13F8A
    int  grid_read(int x, int y) const;      // sub_13CBD (out of range returns -128)
    void grid_write(int x, int y, int v);    // sub_13D02 (padding guard)

    // sub_1D124(unit, x, y, facing) -> list size (the original returns AX = F0B9C+2)
    int collect_targets(const Unit& u);
    // Same as sub_1D124 with a substituted facing argument (sub_3EE2B scans 0..3).
    // The kind-6 adjustment still reads the unit's own [+63h], not the argument.
    int collect_targets_dir(const Unit& u, int dir);
    // sub_1CC09(unit): paint the range into E8F46 after clearing it with 13F8A
    void mark_range(const Unit& u);

    // sub_1F738, the class 11h/12h confirm step: clear via 13F8A and empty the list, then
    // for every 1AC08(x,y,r) diamond tile write 13D02(x,y,r) and add hostile units through
    // 1A1F8 (army based).
    void area_diamond(const Unit& u, int x, int y, int r);
    // sub_1F466 click: empty the list, then 1A452(cx, cy, count 2, facing 0, range -2) and
    // 1A1F8 for every tile.
    void collect_block2x2(const Unit& u, int cx, int cy);
    // sub_1EE23 (weapon class 4): the 1AD62/1AE48 three-tile order one tile ahead.
    std::vector<Pt> tri3_tiles(int x, int y, int facing);

    // ---- Sub-routines (public for tests) ----
    Pt front_tile(const Unit& u, int facing) const;               // sub_1A1DE + sub_1A146
    int tile_attr(int x, int y) const;                            // sub_1093D
    // sub_1B7BF + sub_13F8A + sub_1B7FA: set the LOS mode (F0B8E) and expected army (F0B86),
    // clear the grid, then start shadow casting from the actor's front tile.
    // mode 0 = paint the grid with 1, mode 1 = collect into list.
    void los_from_actor(const Unit& u, int expect, int mode, int range);
    void los(int x, int y, int facing, int cx, int range);        // sub_1B7FA (front tile base)

    // ---- Raw iterator API (reproduces the call order of sub_43CF0 and friends) ----
    //   sub_1A452/1A552 rectangle, sub_1AC08/1AC59 diamond, sub_1A1F8 list insertion,
    //   sub_11DB2 occupancy, sub_1A31D/1A3AE snapshot (F0B92..F0B9A words).
    struct IterState {
        int16_t b92, b94, b7c_lo, b7c_hi, b78_lo, b78_hi;
        int16_t b96, b98, b9a, b9c, b90;
    };
    IterState save_iter() const;                                  // sub_1A31D
    void restore_iter(const IterState& st);                       // sub_1A3AE
    void rect_init(int x, int y, int count, int facing, int range); // sub_1A452
    bool rect_next(int& x, int& y);                               // sub_1A552
    void dia_init(int x, int y, int r);                           // sub_1AC08
    bool dia_next(int& x, int& y);                                // sub_1AC59
    bool add_by_army(int x, int y, int army);                     // sub_1A1F8
    bool add_by_actor(int x, int y, const Unit& actor);           // sub_1A25F
    int occupant_at(int x, int y) const { return occupant(x, y); } // sub_11DB2 (guard included)

private:
    const Env& env_;
    int collect_targets_impl(const Unit& u, int dir, int kind6_facing);
    // Iterator globals
    int16_t b92_ = 0, b94_ = 0, b7c_lo_ = 0, b7c_hi_ = 0, b78_lo_ = 0, b78_hi_ = 0;
    int16_t b96_ = 0, b98_ = 0, b9a_ = 0, b9c_ = 0, b90_ = 0;
    int16_t b88_ = 0, b84hi_ = 0, b8e_ = 0;
    uint8_t ba4_ = 0;

    int occupant(int x, int y) const;                             // sub_11DB2 (guard included)
    void tri3_init(int x, int y, int facing);
    bool tri3_next(int& x, int& y);
    void cone_init(int x, int y, int cx, int facing, int range);
    bool cone_next(int& x, int& y);
    void bucket_base(int x, int y);
    bool bucket_ok(int x, int y) const;
    void line_scan(int x, int y, int facing, int count, int range);
    void shadow_rows(int x, int y, int facing, int n);            // sub_1B61E
    void shadow_side_a(int x, int y, int facing, int n, int side);// sub_1B1AE
    void shadow_side_b(int x, int y, int facing, int n, int side);// sub_1B43A
    void set_los_mode(int army, int expect, int mode);            // sub_1B7BF
    static void step(int16_t& x, int16_t& y, int dir);            // sub_49ABC
    int los_range(const Unit& u) const;                           // sub_1AEF3
};

}  // namespace g2range
