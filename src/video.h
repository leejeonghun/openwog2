#pragma once
#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <SDL.h>

// VGA Mode 13h: 320x200, 256 colors
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 200;

// One mouse cursor frame: palette-index bitmap (0xFF = transparent).
// The hotspot is a pixel offset from the image's top-left in logical 320x200 units (>= 0).
struct CursorFrame {
    int width = 0;
    int height = 0;
    int hot_x = 0;
    int hot_y = 0;
    std::vector<uint8_t> pixels; // width * height
};

class Video {
public:
    Video();
    ~Video();

    bool init();
    void shutdown();

    // VGA Mode 13h equivalent
    void set_mode_13h();
    void set_text_mode();

    // Backbuffer operations (offset 0xA0000 equivalent)
    uint8_t* get_backbuffer() { return backbuffer_; }
    // Hands the latest frame to the present thread and returns immediately (non-blocking).
    // Only the present thread waits for vsync, so the 175.47 Hz game loop keeps its cadence.
    void present();
    // Waits until the frame from the previous present() has actually reached the screen
    // (returns immediately when there is none). Only for loops that must show every frame,
    // such as animations: a presented frame stays visible for one 60 Hz frame (16.7 ms), so
    // it is visible even on a renderer without vsync.
    void wait_presented();
    void clear();

    // Sends the backbuffer right now from this thread (fallback when there is no present thread).
    void blit_backbuffer();

    SDL_Renderer* get_renderer() { return renderer_; }
    SDL_Window* get_window() { return window_; }

    // Scale factor for window (320x200 -> window)
    static constexpr int SCALE = 2;

    // ---- Mouse cursor (SDL_CreateColorCursor hardware cursor) ----
    // The DOS original drew an INT 33h software cursor straight into VRAM, so scaling did not
    // exist. The port uses the OS cursor, but the image has to be built for the current window
    // scale (integer factor, nearest neighbour) and the current DAC palette so the pixels stay
    // crisp and colours match the original during fades. Both concerns live in this class;
    // modules (TTL, Pack, ...) only register and select frames.
    //
    //   cursor_register  registers a frame -> id (starting at 0). The module frees it with
    //                    cursor_reset() when it exits.
    //   cursor_select    switches the current shape to id (cheap when it is already current).
    //   cursor_set_pressed_variant
    //                    pairs a normal frame with the frame used while the button is held.
    //                    The original INT 33h handler drew the pressed sprite whenever the
    //                    button state was non-zero; cursor_update reads SDL_GetMouseState
    //                    directly (independent of Input).
    //   cursor_show      original cursor show (sub_14543) / hide (sub_14585). Visible by default.
    //   cursor_update    rebuilds the cursor when the palette or window scale changed; present()
    //                    calls it every frame, so callers normally do not need to.
    //   cursor_reset     unregisters everything, restores the system cursor and the visible
    //                    state (the original driver reset, sub_14521).
    int  cursor_register(CursorFrame frame);
    void cursor_set_pressed_variant(int normal_id, int pressed_id);
    bool cursor_select(int id);
    void cursor_show(bool show);
    void cursor_update();
    void cursor_reset();
    int  cursor_scale() const;   // integer scale for the current window size (at least 1)

    // Parses an EXE cursor buffer: variable-length sprites [width u16][height u16][pixels w*h]
    // (0xFF = transparent) stored back to back (sub_15DC0 / sub_2B2A1 family; TTL/CHP cursor
    // VAs 0x2096B / 0x80633). Hotspot is (0, 0). Stops at a malformed or truncated header and
    // returns false when no frame could be read.
    static bool cursor_parse_bin(const uint8_t* data, size_t size, std::vector<CursorFrame>& frames);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* screen_texture_ = nullptr;
    uint8_t* backbuffer_ = nullptr;
    bool initialized_ = false;

    // ---- Present thread ----
    // The original sub_5E9E8 copied the 320x200 buffer to VRAM with `rep movsd`, which was
    // effectively free, and the game presented every game tick (175.47 Hz). SDL's vsync present
    // blocks until the display refresh (usually 60 Hz), so calling it on the main loop would run
    // a `present(); wait_game_tick();` loop at one third speed. The main loop therefore hands
    // over a copy of the latest frame (64 KB memcpy) and the present thread waits for vsync and
    // shows the newest one: simulation at 175.47 Hz, display at its own rate, no tearing.
    void present_loop();
    void blit_frame(const uint8_t* frame, const uint8_t* pal);
    std::thread present_thread_;
    std::mutex present_mutex_;
    std::condition_variable present_cv_;
    std::vector<uint8_t> present_frame_;          // latest SCREEN_W*SCREEN_H palette-index copy
    uint8_t present_pal_[256][3] = {};            // DAC snapshot of that frame (consistent during fades)
    bool present_pending_ = false;
    bool present_quit_ = false;
    // Progress numbers for wait_presented: present() bumps seq and submits the frame, and the
    // present thread records the seq it actually showed in presented_seq_ before waking waiters.
    uint64_t present_seq_ = 0;
    uint64_t present_frame_seq_ = 0;
    uint64_t presented_seq_ = 0;
    double present_frame_submit_us_ = 0.0;   // when the frame was queued
    double presented_submit_us_ = 0.0;       // when the frame that is now visible was queued
    std::condition_variable present_done_cv_;
    // An OpenGL renderer binds its context to a thread: init detaches it from the main thread so
    // the present thread can take it over. If the platform refuses the move, present_gl_failed_
    // is set and present falls back to sending frames synchronously.
    SDL_GLContext gl_context_ = nullptr;
    std::atomic<bool> present_gl_failed_{false};

    struct CursorSlot {
        CursorFrame frame;
        int pressed_variant = -1;       // slot used while the button is held (-1 = none)
        SDL_Cursor* cursor = nullptr;   // SDL cursor for the current (scale, palette), lazily built
    };
    std::vector<CursorSlot> cursors_;
    int cursor_wanted_ = -1;            // slot chosen by cursor_select
    int cursor_current_ = -1;           // slot actually applied with SDL_SetCursor
    int cursor_built_scale_ = 0;
    uint8_t cursor_pal_[256][3] = {};   // palette snapshot used to build the cursor
    bool cursor_pal_valid_ = false;
    bool cursor_failed_ = false;
    bool cursor_visible_ = true;

    SDL_Cursor* cursor_build(const CursorFrame& frame, int scale) const;
    void cursor_free_all();
};
