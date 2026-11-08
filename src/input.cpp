#include "input.h"
#include <cstdio>
#include <cstdlib>

bool Input::init() {
    if (initialized_) return true;

    if (SDL_InitSubSystem(SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "INPUT: error: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return false;
    }

    initialized_ = true;
    return true;
}

void Input::update() {
    mouse_.left_clicked = false;
    mouse_.right_clicked = false;
    last_key_pressed_ = SDLK_UNKNOWN;
    injected_key_ = SDLK_UNKNOWN;
    // G2_MOUSE_TRACE=1: trace mouse coordinates (motion is logged once per 60 events).
    static const bool mtrace = (std::getenv("G2_MOUSE_TRACE") != nullptr);
    static int mmotion = 0;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_MOUSEMOTION:
                mouse_.x = event.motion.x;
                mouse_.y = event.motion.y;
                if (mtrace && (mmotion++ % 60 == 0))
                    std::fprintf(stderr, "INPUT: motion=(%d,%d)\n",
                                 event.motion.x, event.motion.y);
                break;

            case SDL_MOUSEBUTTONDOWN:
                mouse_.x = event.button.x;
                mouse_.y = event.button.y;
                if (mtrace)
                    std::fprintf(stderr, "INPUT: button=(%d,%d) btn=%d\n",
                                 event.button.x, event.button.y,
                                 event.button.button);
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_.left_button = true;
                    mouse_.left_clicked = true;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    mouse_.right_button = true;
                    mouse_.right_clicked = true;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_.left_button = false;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    mouse_.right_button = false;
                }
                break;

            case SDL_KEYDOWN: {
                last_key_pressed_ = event.key.keysym.sym;
                const SDL_Scancode sc = event.key.keysym.scancode;
                if (sc >= 0 && sc < SDL_NUM_SCANCODES) held_keys_[sc] = true;
                break;
            }
            case SDL_KEYUP: {
                const SDL_Scancode sc = event.key.keysym.scancode;
                if (sc >= 0 && sc < SDL_NUM_SCANCODES) held_keys_[sc] = false;
                break;
            }
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    for (int i = 0; i < SDL_NUM_SCANCODES; i++) held_keys_[i] = false;
                }
                break;

            case SDL_QUIT:
                quit_ = true;
                break;
        }
    }
    if (poll_hook_) poll_hook_();
}

bool Input::is_key_pressed(SDL_Keycode key) const {
    return last_key_pressed_ == key;
}

bool Input::is_key_held(SDL_Keycode key) const {
    const SDL_Scancode sc = SDL_GetScancodeFromKey(key);
    if (sc >= SDL_NUM_SCANCODES) return false;
    return held_keys_[sc];
}

bool Input::poll_quit() {
    SDL_Event ev;
    if (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) {
        quit_ = true;
    }
    if (poll_hook_) poll_hook_();
    return quit_;
}

void Input::shutdown() {
    initialized_ = false;
}