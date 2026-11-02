#include "video.h"
#include "palette.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static double video_now_us() {
    return (double)SDL_GetPerformanceCounter() * 1e6 / (double)SDL_GetPerformanceFrequency();
}

Video::Video() = default;

Video::~Video() {
    shutdown();
}

bool Video::init() {
    if (initialized_) return true;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "VIDEO: error: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(
        "The War of Genesis II",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W * SCALE, SCREEN_H * SCALE,
        SDL_WINDOW_SHOWN
    );
    if (!window_) {
        fprintf(stderr, "VIDEO: error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // Vsync stays on (no tearing), but SDL_RenderPresent blocks the caller for a full display
    // period, so the present thread (present_loop) owns the send and the game loop only hands over
    // a copy of the newest frame - the same cadence as the original's free VRAM copy per tick.
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        // Headless/remote verification environments have no accelerated renderer;
        // G2_SOFTWARE_RENDER=1 falls back to the software renderer.
        if (getenv("G2_SOFTWARE_RENDER")) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
        }
    }
    if (!renderer_) {
        fprintf(stderr, "VIDEO: error: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_RenderSetLogicalSize(renderer_, SCREEN_W, SCREEN_H);

    // An OpenGL renderer only allows texture uploads and RenderCopy on the thread where its GL
    // context is current. A context can be current on one thread at a time, so detach it from the
    // main thread and let the present thread take it (null for D3D/software renderers).
    gl_context_ = SDL_GL_GetCurrentContext();
    if (gl_context_) SDL_GL_MakeCurrent(window_, nullptr);

    screen_texture_ = SDL_CreateTexture(
        renderer_,
        SDL_PIXELFORMAT_RGB888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H
    );
    if (!screen_texture_) {
        fprintf(stderr, "VIDEO: error: SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    backbuffer_ = new uint8_t[SCREEN_W * SCREEN_H];
    memset(backbuffer_, 0, SCREEN_W * SCREEN_H);

    // Start the present thread. If that fails, present() sends synchronously on the main thread.
    present_frame_.assign((size_t)SCREEN_W * SCREEN_H, 0);
    memset(present_pal_, 0, sizeof(present_pal_));
    present_pending_ = false;
    present_quit_ = false;
    try {
        present_thread_ = std::thread(&Video::present_loop, this);
    } catch (...) {
        present_thread_ = std::thread();
        if (gl_context_) { SDL_GL_MakeCurrent(window_, gl_context_); gl_context_ = nullptr; }
        fprintf(stderr, "VIDEO: warning: could not create the present thread - falling back to synchronous present\n");
    }

    initialized_ = true;
    return true;
}

void Video::shutdown() {
    // Stop the present thread first; only then can the renderer and texture be destroyed.
    if (present_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(present_mutex_);
            present_quit_ = true;
        }
        present_cv_.notify_all();
        present_thread_.join();
    }

    // Free the cursors before the window (no SDL cursor API after SDL_Quit).
    cursor_reset();

    delete[] backbuffer_;
    backbuffer_ = nullptr;

    if (screen_texture_) { SDL_DestroyTexture(screen_texture_); screen_texture_ = nullptr; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }

    SDL_Quit();
    initialized_ = false;
}

void Video::set_mode_13h() {
    clear();
}

void Video::set_text_mode() {
    clear();
}

void Video::clear() {
    if (backbuffer_) {
        memset(backbuffer_, 0, SCREEN_W * SCREEN_H);
    }
}

void Video::present() {
    // Rebuild the cursor image when the palette (including fades) or window scale changed.
    // Cursors are OS window resources, so they are only touched on the main thread.
    cursor_update();

    if (!backbuffer_) return;

    // On platforms where the present thread cannot take the GL context (offscreen EGL and the
    // like), send from the main thread instead - slower beats a blank screen.
    if (present_thread_.joinable() && present_gl_failed_.load()) {
        present_thread_.join();
        if (gl_context_) { SDL_GL_MakeCurrent(window_, gl_context_); gl_context_ = nullptr; }
    }

    if (!present_thread_.joinable()) {
        blit_frame(backbuffer_, &g_palette[0][0]);
        return;
    }

    // Keep only the newest frame: if the present thread is still showing the previous one, it is
    // overwritten. The next vblank shows the most recent frame, so intermediate ones can be
    // dropped.
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        std::memcpy(present_frame_.data(), backbuffer_, (size_t)SCREEN_W * SCREEN_H);
        std::memcpy(present_pal_, g_palette, sizeof(present_pal_));
        present_frame_seq_ = ++present_seq_;
        present_frame_submit_us_ = video_now_us();
        present_pending_ = true;
    }
    present_cv_.notify_one();
}

void Video::blit_backbuffer() {
    // Rebuild the cursor image when the palette (including fades) or window scale changed.
    cursor_update();
    if (!backbuffer_) return;
    blit_frame(backbuffer_, &g_palette[0][0]);
}

// Uploads one palette-index frame to the texture and presents it. With vsync on,
// SDL_RenderPresent waits for the display period; this function is only called from the present
// thread, so the game loop never pays that wait.
void Video::blit_frame(const uint8_t* frame, const uint8_t* pal) {
    if (!frame || !pal || !screen_texture_ || !renderer_) return;

    uint32_t* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(screen_texture_, nullptr, (void**)&pixels, &pitch) < 0) return;

    const int stride = pitch / 4;   // row pitch of the RGB888 texture, in pixels
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t* dst = pixels + (size_t)y * (size_t)stride;
        const uint8_t* src = frame + (size_t)y * SCREEN_W;
        for (int x = 0; x < SCREEN_W; x++) {
            const uint8_t idx = src[x];
            // Scale 6-bit (0~63) VGA DAC value to 8-bit (0~255)
            const uint8_t r = pal[(size_t)idx * 3 + 0];
            const uint8_t g = pal[(size_t)idx * 3 + 1];
            const uint8_t b = pal[(size_t)idx * 3 + 2];
            dst[x] = (0xFFu << 24) |
                     ((uint32_t)((r << 2) | (r >> 4)) << 16) |
                     ((uint32_t)((g << 2) | (g >> 4)) << 8) |
                     (uint32_t)((b << 2) | (b >> 4));
        }
    }

    SDL_UnlockTexture(screen_texture_);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, screen_texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

// Present thread: whenever a new frame arrives, upload the most recent copy and present it.
// This thread alone absorbs the vsync blocking.
void Video::present_loop() {
    // Bring the GL context onto this thread; on failure fall back to main-thread sending.
    bool gl_current = false;
    if (gl_context_) {
        if (SDL_GL_MakeCurrent(window_, gl_context_) == 0) {
            gl_current = true;
        } else {
            fprintf(stderr, "VIDEO: warning: could not move the GL context to the present thread - falling back to synchronous present (%s)\n",
                    SDL_GetError());
            present_gl_failed_.store(true);
            return;
        }
    }

    std::vector<uint8_t> frame((size_t)SCREEN_W * SCREEN_H);
    std::vector<uint8_t> pal(256 * 3);
    uint64_t seq = 0;
    double submit_us = 0.0;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(present_mutex_);
            present_cv_.wait(lock, [this] { return present_quit_ || present_pending_; });
            if (present_quit_) {
                lock.unlock();
                if (gl_current) SDL_GL_MakeCurrent(window_, nullptr);   // release before exit
                return;
            }
            frame = present_frame_;                       // 64KB copy (capacity reused)
            std::memcpy(pal.data(), present_pal_, pal.size());
            seq = present_frame_seq_;
            submit_us = present_frame_submit_us_;
            present_pending_ = false;
        }
        blit_frame(frame.data(), pal.data());
        {
            std::lock_guard<std::mutex> lock(present_mutex_);
            presented_seq_ = seq;
            presented_submit_us_ = submit_us;
        }
        present_done_cv_.notify_all();
    }
}

// Waits until the frame from the previous present() reaches the screen. Only for loops that must
// show every single frame, such as animations (the expanding dialogue ellipse); an ordinary game
// loop is fine with present() alone.
void Video::wait_presented() {
    if (!present_thread_.joinable()) return;   // synchronous sending already finished inside present()

    double submit_us = 0.0;
    {
        std::unique_lock<std::mutex> lock(present_mutex_);
        const uint64_t target = present_seq_;
        present_done_cv_.wait(lock, [&] {
            return presented_seq_ >= target || present_gl_failed_.load() || present_quit_;
        });
        submit_us = presented_submit_us_;
    }

    // Fill 15 ms from the moment the frame was queued - one 60 Hz frame (16.7 ms) minus a margin.
    // With real vsync, present already waited for vblank, so this deadline has passed and nothing
    // waits here (the next frame goes out on the next vblank). Without vsync (software fallback,
    // offscreen) sending returns immediately and the animation frame would flash by, so 15 ms is
    // filled in to make the sequence visible.
    const double remain = (submit_us + 15000.0) - video_now_us();
    if (remain > 0.0) SDL_Delay((Uint32)((remain + 999.0) / 1000.0));
}

// ==== Mouse cursor ====

int Video::cursor_scale() const {
    int ww = 0, wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    // SDL_RenderSetLogicalSize letterboxes, so the smaller ratio is the effective scale.
    const int s = std::min(ww / SCREEN_W, wh / SCREEN_H);
    return s < 1 ? 1 : s;
}

int Video::cursor_register(CursorFrame frame) {
    const size_t need = (size_t)std::max(frame.width, 0) * (size_t)std::max(frame.height, 0);
    if (frame.pixels.size() < need) frame.pixels.resize(need, 0xFF);   // pad short data with transparent
    CursorSlot slot;
    slot.frame = std::move(frame);
    cursors_.push_back(std::move(slot));
    cursor_failed_ = false;
    return (int)cursors_.size() - 1;
}

// Frame -> SDL cursor: integer nearest-neighbour upscale with the current DAC palette (6 -> 8 bit).
SDL_Cursor* Video::cursor_build(const CursorFrame& fr, int scale) const {
    if (fr.width <= 0 || fr.height <= 0) return nullptr;
    const int sw = fr.width * scale;
    const int sh = fr.height * scale;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;

    uint32_t* px = (uint32_t*)surf->pixels;
    const int pitch32 = surf->pitch / 4;
    for (int y = 0; y < sh; y++) {
        const int sy = y / scale;
        for (int x = 0; x < sw; x++) {
            const int sx = x / scale;
            uint32_t v = 0;   // transparent
            const uint8_t ci = fr.pixels[(size_t)sy * fr.width + sx];
            if (ci != 0xFF) {
                const uint8_t r = g_palette[ci][0], g = g_palette[ci][1], b = g_palette[ci][2];
                // Same 6 -> 8 bit expansion as the blit path. SDL_PIXELFORMAT_RGBA32 is an
                // endian-dependent alias, so manual shifts would swap R and B; SDL_MapRGBA converts
                // to the surface format correctly.
                v = SDL_MapRGBA(surf->format,
                                (uint8_t)((r << 2) | (r >> 4)),
                                (uint8_t)((g << 2) | (g >> 4)),
                                (uint8_t)((b << 2) | (b >> 4)), 0xFF);
            }
            px[(size_t)y * pitch32 + x] = v;
        }
    }
    SDL_Cursor* cur = SDL_CreateColorCursor(surf, fr.hot_x * scale, fr.hot_y * scale);
    SDL_FreeSurface(surf);
    return cur;
}

void Video::cursor_free_all() {
    for (CursorSlot& s : cursors_) {
        if (s.cursor) { SDL_FreeCursor(s.cursor); s.cursor = nullptr; }
    }
    cursor_current_ = -1;
}

void Video::cursor_set_pressed_variant(int normal_id, int pressed_id) {
    if (normal_id < 0 || normal_id >= (int)cursors_.size()) return;
    if (pressed_id < -1 || pressed_id >= (int)cursors_.size()) return;
    cursors_[(size_t)normal_id].pressed_variant = pressed_id;
}

void Video::cursor_update() {
    if (!initialized_ || !window_ || cursors_.empty()) return;

    // Slot to show now: the selected one, or its pressed pair while the button is held
    int effective = cursor_wanted_;
    if (effective >= 0 && effective < (int)cursors_.size()) {
        const int pv = cursors_[(size_t)effective].pressed_variant;
        if (pv >= 0 && SDL_GetMouseState(nullptr, nullptr) != 0) effective = pv;
    }

    const int scale = cursor_scale();
    bool rebuild = !cursor_pal_valid_ || scale != cursor_built_scale_ ||
                   std::memcmp(cursor_pal_, g_palette, sizeof(cursor_pal_)) != 0;

    // Free stale cursors only after the new one is set, so the default arrow never flashes.
    std::vector<SDL_Cursor*> stale;
    if (rebuild) {
        for (CursorSlot& s : cursors_) {
            if (s.cursor) { stale.push_back(s.cursor); s.cursor = nullptr; }
        }
        cursor_current_ = -1;
        std::memcpy(cursor_pal_, g_palette, sizeof(cursor_pal_));
        cursor_built_scale_ = scale;
        cursor_pal_valid_ = true;
        cursor_failed_ = false;
    }

    if (!cursor_failed_ && effective >= 0 && effective < (int)cursors_.size()) {
        CursorSlot& s = cursors_[(size_t)effective];
        if (!s.cursor) {
            s.cursor = cursor_build(s.frame, scale);
            if (!s.cursor) {
                fprintf(stderr, "VIDEO: warning: SDL_CreateColorCursor failed: %s (using the default cursor)\n", SDL_GetError());
                cursor_failed_ = true;   // do not retry until the palette/scale changes or a new frame is registered
            }
        }
        if (s.cursor && cursor_current_ != effective) {
            SDL_SetCursor(s.cursor);
            cursor_current_ = effective;
            SDL_ShowCursor(cursor_visible_ ? SDL_ENABLE : SDL_DISABLE);
        }
    }

    for (SDL_Cursor* c : stale) SDL_FreeCursor(c);
}

bool Video::cursor_select(int id) {
    if (id < 0 || id >= (int)cursors_.size()) return false;
    cursor_wanted_ = id;
    cursor_update();
    return !cursor_failed_ && cursor_current_ >= 0;
}

void Video::cursor_show(bool show) {
    cursor_visible_ = show;
    if (initialized_) SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

void Video::cursor_reset() {
    if (initialized_) {
        SDL_SetCursor(SDL_GetDefaultCursor());   // restore the default before freeing
        cursor_free_all();
        SDL_ShowCursor(SDL_ENABLE);
    }
    cursors_.clear();
    cursor_wanted_ = -1;
    cursor_current_ = -1;
    cursor_pal_valid_ = false;
    cursor_failed_ = false;
    cursor_visible_ = true;
}

bool Video::cursor_parse_bin(const uint8_t* data, size_t size, std::vector<CursorFrame>& frames) {
    frames.clear();
    size_t off = 0;
    while (data && off + 4 <= size) {
        const int w = data[off] | (data[off + 1] << 8);
        const int h = data[off + 2] | (data[off + 3] << 8);
        const size_t n = (size_t)w * (size_t)h;
        if (w <= 0 || h <= 0 || w > 64 || h > 64 || off + 4 + n > size) break;
        CursorFrame f;
        f.width = w;
        f.height = h;
        f.hot_x = 0;   // no hotspot: the sprite's top-left is (x, y)
        f.hot_y = 0;
        f.pixels.assign(data + off + 4, data + off + 4 + n);
        frames.push_back(std::move(f));
        off += 4 + n;
    }
    return !frames.empty();
}
