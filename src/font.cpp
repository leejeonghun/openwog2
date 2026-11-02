#include "font.h"
#include <cstdio>
#include <cstring>

// ---- Lookup tables read directly from the TTL.LST data section ----
const uint8_t Font::cho_map_[32] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 17, 18, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const uint8_t Font::jung_map_[32] = {
    0, 0, 0, 1, 2, 3, 4, 5, 0, 0, 6, 7, 8, 9, 10,
    11, 0, 0, 12, 13, 14, 15, 16, 17, 0, 0, 18, 19, 20, 21, 0, 0
};

const uint8_t Font::jong_map_[32] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 0, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 0, 0
};

const uint8_t Font::cho_jung_group_nojong_[20] = {
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1
};

const uint8_t Font::cho_jung_group_jong_[20] = {
    0, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 3, 3, 3
};

const uint8_t Font::jung_cho_group_nojong_[22] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 3, 3, 1, 2, 4, 4, 4, 2, 1, 3, 0
};

const uint8_t Font::jung_cho_group_jong_[22] = {
    0, 5, 5, 5, 5, 5, 5, 5, 5, 6, 7, 7, 7, 6, 6, 7, 7, 7, 6, 6, 7, 5
};

const uint8_t Font::jung_jong_group_map_[22] = {
    0, 0, 2, 0, 2, 1, 2, 1, 2, 3, 0, 2, 1, 3, 3, 1, 2, 1, 3, 3, 1, 1
};

constexpr int CHO_GROUP_STRIDE  = 0x280;
constexpr int JUNG_GROUP_STRIDE = 0x2C0;
constexpr int JONG_GROUP_STRIDE = 0x380;
constexpr int GLYPH_STRIDE      = 0x20;

// Compatibility jamo tables for the Johab -> UTF-8 helper (1-based glyph index -> UTF-8).
// The indices match the value ranges of cho_map_/jung_map_/jong_map_ (cho 1..19,
// jung 1..21, jong 0..27).
namespace {
const char* const kChoCompat[20] = {
    "", "ㄱ", "ㄲ", "ㄴ", "ㄷ", "ㄸ", "ㄹ", "ㅁ", "ㅂ", "ㅃ",
    "ㅅ", "ㅆ", "ㅇ", "ㅈ", "ㅉ", "ㅊ", "ㅋ", "ㅌ", "ㅍ", "ㅎ"
};
const char* const kJungCompat[22] = {
    "", "ㅏ", "ㅐ", "ㅑ", "ㅒ", "ㅓ", "ㅔ", "ㅕ", "ㅖ", "ㅗ", "ㅘ",
    "ㅙ", "ㅚ", "ㅛ", "ㅜ", "ㅝ", "ㅞ", "ㅟ", "ㅠ", "ㅡ", "ㅢ", "ㅣ"
};
const char* const kJongCompat[28] = {
    "", "ㄱ", "ㄲ", "ㄳ", "ㄴ", "ㄵ", "ㄶ", "ㄷ", "ㄹ", "ㄺ", "ㄻ", "ㄼ", "ㄽ", "ㄾ",
    "ㄿ", "ㅀ", "ㅁ", "ㅂ", "ㅄ", "ㅅ", "ㅆ", "ㅇ", "ㅈ", "ㅊ", "ㅋ", "ㅌ", "ㅍ", "ㅎ"
};

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}
} // namespace

// Johab 2-byte -> UTF-8. The decomposition uses the same bit fields as draw_string
// (b0 = 0xxxxx x0: cho 5 bits / jung 3 bits; b1 = jung 2 bits + jong 5 bits) and
// composes the precomposed syllable directly with the Unicode formula.
std::string Font::johab_to_utf8(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 2);
    for (size_t i = 0; i < text.size();) {
        const uint8_t b0 = (uint8_t)text[i];
        if (b0 == 0) break;                              // the original string ends at NUL too
        if (b0 == '\n') { out += "\\n"; i += 1; continue; } // keep the log on one line
        if (b0 == '\r') { i += 1; continue; }
        if (b0 < 0x80) { out.push_back((char)b0); i += 1; continue; }
        if (i + 1 >= text.size()) { out += "?"; break; } // truncated 2-byte pair
        const uint8_t b1 = (uint8_t)text[i + 1];
        i += 2;
        const int cho_code  = (b0 & 0x7C) >> 2;
        const int jung_code = ((b0 & 0x03) << 3) | ((b1 & 0xE0) >> 5);
        const int jong_code = b1 & 0x1F;
        const int cho_idx  = (cho_code  < 32) ? cho_map_[cho_code]  : 0;
        const int jung_idx = (jung_code < 32) ? jung_map_[jung_code] : 0;
        const int jong_idx = (jong_code < 32) ? jong_map_[jong_code] : 0;
        if (cho_idx == 0 && jung_idx == 0 && jong_idx == 0) { out += "?"; continue; }
        if (cho_idx > 0 && jung_idx > 0) {
            const uint32_t cp = 0xAC00u
                + (uint32_t)((cho_idx - 1) * 21 + (jung_idx - 1)) * 28u
                + (uint32_t)jong_idx;
            append_utf8(out, cp);
            continue;
        }
        // Jamo-only pairs (no cho or no jung) cannot compose: emit compatibility jamo.
        if (cho_idx > 0 && cho_idx < 20) out += kChoCompat[cho_idx];
        if (jung_idx > 0 && jung_idx < 22) out += kJungCompat[jung_idx];
        if (jong_idx > 0 && jong_idx < 28) out += kJongCompat[jong_idx];
    }
    return out;
}

Font::Font() = default;

Font::~Font() {
    delete[] cho_data_;
    delete[] jung_data_;
    delete[] jong_data_;
    delete[] ascii_data_;
}

bool Font::load_hangul(const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "FONT: warning: Cannot open %s\n", filename);
        return false;
    }

    delete[] cho_data_;
    delete[] jung_data_;
    delete[] jong_data_;

    cho_data_  = new uint8_t[0x1400];
    jung_data_ = new uint8_t[0xB00];
    jong_data_ = new uint8_t[0xE00];

    fread(cho_data_, 1, 0x1400, fp);
    fread(jung_data_, 1, 0xB00, fp);
    fread(jong_data_, 1, 0xE00, fp);
    fclose(fp);

    return true;
}

bool Font::load_ascii(const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "FONT: warning: Cannot open %s\n", filename);
        return false;
    }

    delete[] ascii_data_;
    ascii_data_ = new uint8_t[0x1000];
    fread(ascii_data_, 1, 0x1000, fp);
    fclose(fp);

    loaded_ = true;
    return true;
}

// sub_14F3F: set the text mode and colour.
void Font::set_text_mode(int mode, uint16_t color_param, uint8_t bg_color) {
    text_mode_ = mode;
    if (mode == 1) {
        text_color_param_ = 0x00FF;
        bg_color_ = (uint8_t)color_param;
    } else if (mode == 2) {
        text_color_param_ = color_param;
        bg_color_ = 0xFF;
    } else {
        text_color_param_ = color_param;
        bg_color_ = bg_color;
    }
}

// sub_14EFE: OR one glyph into the 32-byte composition buffer.
static void or_glyph(uint8_t* dst, const uint8_t* src, int count_bytes) {
    for (int i = 0; i < count_bytes; i++) {
        dst[i] |= src[i];
    }
}

// sub_12009: low-level 1bpp glyph rasterizer.
static void render_glyph_raw(uint8_t* buffer, int buf_w, int buf_h,
                             int x, int y, const uint8_t* glyph_bitmap,
                             int width, int height, uint8_t fg_color, uint8_t bg_color) {
    int byte_idx = 0;
    for (int r = 0; r < height; r++) {
        int dy = y + r;
        uint8_t bit_mask = 0x80;
        uint8_t cur_byte = glyph_bitmap[byte_idx++];

        for (int c = 0; c < width; c++) {
            int dx = x + c;
            bool bit_set = (cur_byte & bit_mask) != 0;

            if (bit_set) {
                if (fg_color != 0xFF && dx >= 0 && dx < buf_w && dy >= 0 && dy < buf_h) {
                    buffer[dy * buf_w + dx] = fg_color;
                }
            } else {
                if (bg_color != 0xFF && dx >= 0 && dx < buf_w && dy >= 0 && dy < buf_h) {
                    buffer[dy * buf_w + dx] = bg_color;
                }
            }

            bit_mask >>= 1;
            if (bit_mask == 0) {
                bit_mask = 0x80;
                if (c + 1 < width) {
                    cur_byte = glyph_bitmap[byte_idx++];
                }
            }
        }
    }
}

// sub_14F9F: dispatch glyph rendering according to text_mode_.
void Font::draw_glyph_mode(uint8_t* buffer, int buf_w, int buf_h,
                           int x, int y, const uint8_t* glyph_bitmap,
                           int width, int height) {
    uint8_t outline_color = (uint8_t)(text_color_param_ & 0xFF);
    uint8_t fg_color      = (uint8_t)((text_color_param_ >> 8) & 0xFF);

    switch (text_mode_) {
        case 0:
        case 1:
        case 2:
            render_glyph_raw(buffer, buf_w, buf_h, x, y, glyph_bitmap, width, height,
                             (uint8_t)text_color_param_, bg_color_);
            break;

        case 4: // Outline Mode (4-way outline + main text)
            if (bg_color_ != 0xFF) {
                render_glyph_raw(buffer, buf_w, buf_h, x, y, glyph_bitmap, width, height, fg_color, bg_color_);
            }
            render_glyph_raw(buffer, buf_w, buf_h, x - 1, y - 1, glyph_bitmap, width, height, outline_color, 0xFF);
            render_glyph_raw(buffer, buf_w, buf_h, x + 1, y + 1, glyph_bitmap, width, height, outline_color, 0xFF);
            render_glyph_raw(buffer, buf_w, buf_h, x - 1, y + 1, glyph_bitmap, width, height, outline_color, 0xFF);
            render_glyph_raw(buffer, buf_w, buf_h, x + 1, y - 1, glyph_bitmap, width, height, outline_color, 0xFF);
            render_glyph_raw(buffer, buf_w, buf_h, x, y, glyph_bitmap, width, height, fg_color, 0xFF);
            break;

        case 5: // Shadow Mode
            if (bg_color_ != 0xFF) {
                render_glyph_raw(buffer, buf_w, buf_h, x, y, glyph_bitmap, width, height, fg_color, bg_color_);
            }
            render_glyph_raw(buffer, buf_w, buf_h, x + 1, y + 1, glyph_bitmap, width, height, outline_color, 0xFF);
            render_glyph_raw(buffer, buf_w, buf_h, x, y, glyph_bitmap, width, height, fg_color, 0xFF);
            break;

        default:
            render_glyph_raw(buffer, buf_w, buf_h, x, y, glyph_bitmap, width, height, fg_color, bg_color_);
            break;
    }
}

// sub_15134: main string rendering routine (ASCII 8x16 and Johab 16x16).
void Font::draw_string(uint8_t* buffer, int buf_w, int buf_h,
                       int x, int y, const char* str) {
    if (!str || (!cho_data_ && !ascii_data_)) return;

    int cursor_x = x;
    int start_x = x;
    int cursor_y = y;
    int len = (int)strlen(str);

    for (int i = 0; i < len; i++) {
        uint8_t b0 = (uint8_t)str[i];

        if (b0 == 0x0A) {
            cursor_x = start_x;
            cursor_y += HAN_FONT_H;
            continue;
        }

        // A TALK.TBL parsing bug can leave '\r' (0x0D) in the buffer; skip it so ASCII
        // glyph 13 is never drawn (the original reads in "rt" mode, which strips it).
        if (b0 == 0x0D) {
            continue;
        }

        if (b0 < 0x80) {
            // ASCII character (8x16)
            if (b0 == 0x20) {
                cursor_x += ASC_FONT_W;
            } else if (ascii_data_) {
                const uint8_t* glyph = &ascii_data_[b0 * ASC_FONT_H];
                draw_glyph_mode(buffer, buf_w, buf_h, cursor_x, cursor_y, glyph, ASC_FONT_W, ASC_FONT_H);
                cursor_x += ASC_FONT_W;
            }
            continue;
        }

        // Korean Johab character (2 bytes)
        if (i + 1 >= len) break;
        uint8_t b1 = (uint8_t)str[i + 1];
        i++;

        int cho_code  = (b0 & 0x7C) >> 2;
        int jung_code = ((b0 & 0x03) << 3) | ((b1 & 0xE0) >> 5);
        int jong_code = b1 & 0x1F;

        int cho_idx  = (cho_code < 32) ? cho_map_[cho_code] : 0;
        int jung_idx = (jung_code < 32) ? jung_map_[jung_code] : 0;
        int jong_idx = (jong_code < 32) ? jong_map_[jong_code] : 0;

        int cho_group = 0;
        int jung_group = 0;
        int jong_group = 0;

        if (jong_idx == 0) {
            cho_group  = (jung_idx < 22) ? jung_cho_group_nojong_[jung_idx] : 0;
            jung_group = (cho_idx < 20) ? cho_jung_group_nojong_[cho_idx] : 0;
        } else {
            cho_group  = (jung_idx < 22) ? jung_cho_group_jong_[jung_idx] : 0;
            if (cho_idx != 0) {
                jung_group = (cho_idx < 20) ? cho_jung_group_jong_[cho_idx] : 0;
            } else {
                jung_group = 2;
            }
            jong_group = (jung_idx < 22) ? jung_jong_group_map_[jung_idx] : 0;
        }

        memset(glyph_buf_, 0, GLYPH_BYTES);

        if (cho_idx != 0 && cho_data_) {
            int offset = cho_group * CHO_GROUP_STRIDE + cho_idx * GLYPH_STRIDE;
            if (offset + GLYPH_STRIDE <= 0x1400) {
                or_glyph(glyph_buf_, cho_data_ + offset, GLYPH_BYTES);
            }
        }

        if (jung_idx != 0 && jung_data_) {
            int offset = jung_group * JUNG_GROUP_STRIDE + jung_idx * GLYPH_STRIDE;
            if (offset + GLYPH_STRIDE <= 0xB00) {
                or_glyph(glyph_buf_, jung_data_ + offset, GLYPH_BYTES);
            }
        }

        if (jong_idx != 0 && jong_data_) {
            int offset = jong_group * JONG_GROUP_STRIDE + jong_idx * GLYPH_STRIDE;
            if (offset + GLYPH_STRIDE <= 0xE00) {
                or_glyph(glyph_buf_, jong_data_ + offset, GLYPH_BYTES);
            }
        }

        draw_glyph_mode(buffer, buf_w, buf_h, cursor_x, cursor_y, glyph_buf_, HAN_FONT_W, HAN_FONT_H);
        cursor_x += HAN_FONT_W;
    }
}