#pragma once
#include <cstdint>
#include <string>

// Font sizes
constexpr int HAN_FONT_W = 16;
constexpr int HAN_FONT_H = 16;
constexpr int ASC_FONT_W = 8;
constexpr int ASC_FONT_H = 16;
constexpr int GLYPH_BYTES = 32; // 16x16 bitmap = 32 bytes

class Font {
public:
    Font();
    ~Font();

    bool load_hangul(const char* filename);
    bool load_ascii(const char* filename);

    // sub_14F3F: set the text mode and colour parameters.
    // mode 4: outline (high byte = foreground, low byte = outline colour);
    // mode 5: shadow.
    void set_text_mode(int mode, uint16_t color_param, uint8_t bg_color = 0xFF);

    // sub_15134 / sub_15421: Draw string using current text mode
    void draw_string(uint8_t* buffer, int buf_w, int buf_h,
                     int x, int y, const char* str);

    // Johab Korean -> UTF-8, for log and debug output only; screen rendering still uses
    // draw_string's glyph composition below. The indices from cho_map_/jung_map_/jong_map_
    // follow Unicode jamo order (cho 1..19, jung 1..21, jong 0..27), so a precomposed syllable
    // is U+AC00 + (cho-1)*21*28 + (jung-1)*28 + jong. Jamo-only pairs fall back to
    // compatibility jamo, and '\n' becomes the two characters "\n" to keep a log on one line.
    static std::string johab_to_utf8(const std::string& text);

    bool is_loaded() const { return loaded_; }

private:
    uint8_t* cho_data_ = nullptr;   // 0x1400 bytes (8 groups * 20 glyphs * 32)
    uint8_t* jung_data_ = nullptr;  // 0xB00 bytes  (4 groups * 22 glyphs * 32)
    uint8_t* jong_data_ = nullptr;  // 0xE00 bytes  (4 groups * 28 glyphs * 32)
    uint8_t* ascii_data_ = nullptr; // 0x1000 bytes (256 * 16)

    uint8_t glyph_buf_[GLYPH_BYTES];

    // Text rendering state (sub_14F3F / dword_3E914, dword_3E918, dword_3E920)
    int text_mode_ = 4;
    uint16_t text_color_param_ = 0x1000; // Default: Yellow(16) + Black(0) outline
    uint8_t bg_color_ = 0xFF;            // 0xFF = transparent

    // sub_14F9F: Render a glyph using current mode (outline, shadow, normal)
    void draw_glyph_mode(uint8_t* buffer, int buf_w, int buf_h,
                         int x, int y, const uint8_t* glyph_bitmap, int width, int height);

    // ---- Lookup tables read from the TTL.LST data section ----
    static const uint8_t cho_map_[32];
    static const uint8_t jung_map_[32];
    static const uint8_t jong_map_[32];
    static const uint8_t cho_jung_group_nojong_[20];
    static const uint8_t cho_jung_group_jong_[20];
    static const uint8_t jung_cho_group_nojong_[22];
    static const uint8_t jung_cho_group_jong_[22];
    static const uint8_t jung_jong_group_map_[22];

    bool loaded_ = false;
};