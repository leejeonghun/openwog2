#pragma once
#include <cstdint>
#include <functional>
#include <SDL.h>

// Mouse state (emulates INT 33h)
struct MouseState {
    int x, y;
    bool left_button;
    bool right_button;
    bool left_clicked;   // Single click event
    bool right_clicked;
};

class Input {
public:
    bool init();
    void update();
    void shutdown();

    const MouseState& get_mouse() const { return mouse_; }

    // INT 33h emulation
    bool is_mouse_installed() const { return true; }
    int get_mouse_x() const { return mouse_.x; }
    int get_mouse_y() const { return mouse_.y; }
    bool is_left_button() const { return mouse_.left_button; }
    bool is_right_button() const { return mouse_.right_button; }
    bool was_left_clicked() const { return mouse_.left_clicked; }

    bool is_key_pressed(SDL_Keycode key) const;
    // Held (level) state, matching the DOS ISR array dword_1A00F4[scancode] (make=1, break=0).
    // PACK reads dword_1A0168 (Ctrl, 0x1D), dword_1A01D4 (Alt, 0x38) and the four arrows
    // (dword_1A0214/1A0220/1A0228/1A0234) as levels, not edges.
    bool is_key_held(SDL_Keycode key) const;
    bool any_key_pressed() const { return last_key_pressed_ != SDLK_UNKNOWN; }
    // Whether the BIOS keyboard buffer holds a key for getch_ to read (sub_5D839).
    // Modifier-only input (Shift/Ctrl/Alt/Caps) never enters the BIOS buffer.
    bool any_getch_key_pressed() const {
        switch (last_key_pressed_) {
            case SDLK_UNKNOWN:
            case SDLK_LSHIFT: case SDLK_RSHIFT: case SDLK_LCTRL: case SDLK_RCTRL:
            case SDLK_LALT: case SDLK_RALT: case SDLK_LGUI: case SDLK_RGUI:
            case SDLK_CAPSLOCK: case SDLK_NUMLOCKCLEAR: case SDLK_SCROLLLOCK: case SDLK_MODE:
                return false;
            default:
                return true;
        }
    }
    bool quit_requested() const { return quit_; }
    // Test injection (G2_INPUT_SCRIPT), called by CombatII at its tick gate and unused
    // otherwise. An injected key is visible to is_key_pressed()/any_key_pressed() like a real
    // one, so keys the original reads (Esc/Space/Enter) take the same path, and it also
    // carries a test-only tag: ring-command keys absent from COMBATII.EXE (A/M/I/V/W/G/D/F/
    // N/C, 1-7, ring W/Enter) are read only through is_test_key_pressed(), so a real keyboard
    // can never trigger them.
    void test_inject_key(SDL_Keycode key) { last_key_pressed_ = key; injected_key_ = key; }
    bool is_test_key_pressed(SDL_Keycode key) const {
        return key != SDLK_UNKNOWN && injected_key_ == key;
    }
    void test_inject_click(int x, int y) {
        mouse_.x = x; mouse_.y = y;
        mouse_.left_button = true; mouse_.left_clicked = true;
    }

    // A blocking screen consumed one event. The original pops events through sub_5D22E
    // (mouse queue) and sub_61E7E + getch_ (BIOS key buffer), so the click or key that closed
    // that screen never resurfaces in the same frame's loop (battle ring, free wait, system
    // menu). The port re-raises edge flags on every update(), so clearing them here
    // reproduces the original's consumption.
    void consume_event() {
        last_key_pressed_ = SDLK_UNKNOWN;
        injected_key_ = SDLK_UNKNOWN;
        mouse_.left_clicked = false;
        mouse_.right_clicked = false;
    }

    // Consume SDL_QUIT only; other events (clicks, keys) stay queued for the next update()
    // so the delay path never loses input.
    bool poll_quit();

    // Hook called after every event pump (end of update / poll_quit), for state the original
    // refreshed per event inside its INT 33h handler, such as the pressed mouse cursor. A
    // module installs it on entry and clears it with set_poll_hook({}) on exit.
    void set_poll_hook(std::function<void()> hook) { poll_hook_ = std::move(hook); }

private:
    std::function<void()> poll_hook_;
    MouseState mouse_ = {};
    bool initialized_ = false;
    bool quit_ = false;
    SDL_Keycode last_key_pressed_ = SDLK_UNKNOWN;
    SDL_Keycode injected_key_ = SDLK_UNKNOWN; // set by test_inject_key only; real keys never touch it
    // Level state for DOS-ISR style reads (dword_1A00F4). Keyed by SDL scancode,
    // because SDL_Keycode for arrows/Ctrl/Alt is 0x40000000+, far outside any small array.
    // Updated on KEYDOWN/KEYUP in update(); survives across frames until released.
    bool held_keys_[SDL_NUM_SCANCODES] = {};
};