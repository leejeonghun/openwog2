#include "palette.h"
#include <cstring>
#include <SDL.h>

// Global VGA palette (6-bit per channel, 0-63)
uint8_t g_palette[256][3];

void palette_set_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    g_palette[index][0] = r;
    g_palette[index][1] = g;
    g_palette[index][2] = b;
}

void palette_get_entry(uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = g_palette[index][0];
    g = g_palette[index][1];
    b = g_palette[index][2];
}

void palette_set_range(uint8_t start, uint8_t end, const uint8_t data[][3]) {
    for (int i = start; i <= end; i++) {
        g_palette[i][0] = data[i - start][0];
        g_palette[i][1] = data[i - start][1];
        g_palette[i][2] = data[i - start][2];
    }
}

void palette_get_range(uint8_t start, uint8_t end, uint8_t data[][3]) {
    for (int i = start; i <= end; i++) {
        data[i - start][0] = g_palette[i][0];
        data[i - start][1] = g_palette[i][1];
        data[i - start][2] = g_palette[i][2];
    }
}

void palette_fade_to_black(int steps) {
    uint8_t temp[256][3];
    memcpy(temp, g_palette, sizeof(g_palette));

    for (int step = 0; step < steps; step++) {
        for (int i = 0; i < 256; i++) {
            for (int c = 0; c < 3; c++) {
                int val = g_palette[i][c] - (temp[i][c] / steps);
                g_palette[i][c] = val > 0 ? val : 0;
            }
        }
        SDL_Delay(16); // ~60fps
    }
    memset(g_palette, 0, sizeof(g_palette));
}

void palette_fade_from_black(int steps) {
    uint8_t target[256][3];
    memcpy(target, g_palette, sizeof(g_palette));
    memset(g_palette, 0, sizeof(g_palette));

    for (int step = 0; step < steps; step++) {
        for (int i = 0; i < 256; i++) {
            for (int c = 0; c < 3; c++) {
                g_palette[i][c] = target[i][c] * (step + 1) / steps;
            }
        }
        SDL_Delay(16);
    }
    memcpy(g_palette, target, sizeof(g_palette));
}

void palette_set_all_black() {
    memset(g_palette, 0, sizeof(g_palette));
}

void palette_set_all_white() {
    for (int i = 0; i < 256; i++) {
        g_palette[i][0] = 63;
        g_palette[i][1] = 63;
        g_palette[i][2] = 63;
    }
}
