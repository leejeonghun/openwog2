#include "fli.h"
#include "palette.h"
#include "audio.h"
#include "fileutil.h"
#include "game_timer.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <SDL.h>

#pragma pack(push, 1)
struct FLIHeader {
    uint32_t size;
    uint16_t type;      // 0xAF11 = FLI, 0xAF12 = FLC
    uint16_t frames;
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t flags;
    uint16_t speed;     // In 1/70th second ticks (FLI) or ms (FLC)
    uint8_t  reserved[110];
};

struct FLIFrameHeader {
    uint32_t size;
    uint16_t type;      // 0xF1FA
    uint16_t chunks;
    uint8_t  reserved[8];
};

struct FLIChunkHeader {
    uint32_t size;
    uint16_t type;
};
#pragma pack(pop)

static inline uint16_t read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- Chunk 11 (0x0B): FLI_COLOR (sub_19267) ----
void FLIPlayer::decode_color(const uint8_t* data, uint32_t size) {
    const uint8_t* ptr = data;
    uint16_t packets = read_u16(ptr); ptr += 2;
    uint8_t pal_idx = 0;

    for (uint16_t p = 0; p < packets; p++) {
        pal_idx += *ptr++;
        uint16_t count = *ptr++;
        if (count == 0) count = 256;

        for (uint16_t c = 0; c < count; c++) {
            uint8_t r = *ptr++;
            uint8_t g = *ptr++;
            uint8_t b = *ptr++;
            palette_set_entry(pal_idx++, r, g, b);
        }
    }
}

// ---- Chunk 12 (0x0C): FLI_LC (sub_19162 / sub_18F58) ----
void FLIPlayer::decode_lc(const uint8_t* data, uint8_t* buffer) {
    const uint8_t* ptr = data;
    uint16_t start_line = read_u16(ptr); ptr += 2;
    uint16_t line_count = read_u16(ptr); ptr += 2;

    for (uint16_t l = 0; l < line_count; l++) {
        int y = start_line + l;
        if (y >= SCREEN_H) break;

        uint8_t packet_count = *ptr++;
        uint8_t* dst = buffer + y * SCREEN_W;
        int x = 0;

        for (uint8_t p = 0; p < packet_count; p++) {
            uint8_t skip = *ptr++;
            x += skip;
            int8_t count = (int8_t)*ptr++;

            if (count > 0) {
                for (int k = 0; k < count && x < SCREEN_W; k++) {
                    dst[x++] = *ptr++;
                }
            } else if (count < 0) {
                int rle_count = -count;
                uint8_t val = *ptr++;
                for (int k = 0; k < rle_count && x < SCREEN_W; k++) {
                    dst[x++] = val;
                }
            }
        }
    }
}

// ---- Chunk 15 (0x0F): FLI_BRUN (sub_18D23 / sub_18B42) ----
void FLIPlayer::decode_brun(const uint8_t* data, uint8_t* buffer) {
    const uint8_t* ptr = data;

    for (int y = 0; y < SCREEN_H; y++) {
        ptr++; // Skip packet_count byte per line in standard FLI_BRUN
        uint8_t* dst = buffer + y * SCREEN_W;
        int x = 0;

        while (x < SCREEN_W) {
            int8_t count = (int8_t)*ptr++;
            if (count >= 0) {
                uint8_t val = *ptr++;
                for (int k = 0; k < count && x < SCREEN_W; k++) {
                    dst[x++] = val;
                }
            } else {
                int lit_count = -count;
                for (int k = 0; k < lit_count && x < SCREEN_W; k++) {
                    dst[x++] = *ptr++;
                }
            }
        }
    }
}

// ---- Chunk 16 (0x10): FLI_COPY (sub_18F06 / sub_18E41) ----
void FLIPlayer::decode_copy(const uint8_t* data, uint8_t* buffer) {
    memcpy(buffer, data, SCREEN_W * SCREEN_H);
}

// ==== sub_1948B / sub_1882C: full FLI video + VOC audio player ====
bool FLIPlayer::play(const std::string& fli_path, const std::string& voc_path,
                     Video& video, Input& input) {
    FILE* fp = fopen(fli_path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "FLI: warning: Cannot open %s\n", fli_path.c_str());
        return false;
    }

    FLIHeader header;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    if (header.type != 0xAF11 && header.type != 0xAF12) {
        fprintf(stderr, "FLI: warning: Invalid magic 0x%04X in %s\n", header.type, fli_path.c_str());
        fclose(fp);
        return false;
    }

    // Calculate frame delay (FLI speed is in 1/70s ticks)
    uint32_t frame_delay_ms = header.speed ? (header.speed * 1000 / 70) : 71;
    if (header.type == 0xAF12 && header.speed > 0) {
        frame_delay_ms = header.speed; // FLC speed is in milliseconds
    }
    if (frame_delay_ms < 10) frame_delay_ms = 71;

    // Start VOC audio playback if file exists
    if (!voc_path.empty() && file_exists(voc_path.c_str())) {
        Audio::play_voc(voc_path);
    }

    uint8_t* backbuf = video.get_backbuffer();

    // Read full remaining file content for fast decoding
    fseek(fp, 128, SEEK_SET);
    std::vector<uint8_t> file_data;
    fseek(fp, 0, SEEK_END);
    long total_size = ftell(fp);
    fseek(fp, 128, SEEK_SET);

    file_data.resize(total_size - 128);
    fread(file_data.data(), 1, file_data.size(), fp);
    fclose(fp);

    const uint8_t* ptr = file_data.data();
    const uint8_t* end_ptr = file_data.data() + file_data.size();

    bool skipped = false;

    GameTimer::FramePacer pacer(frame_delay_ms > 0 ? 1000.0 / frame_delay_ms : 14.0);
    for (uint16_t f = 0; f < header.frames && !skipped; f++) {
        if (ptr + sizeof(FLIFrameHeader) > end_ptr) break;

        FLIFrameHeader frame_hdr;
        memcpy(&frame_hdr, ptr, sizeof(frame_hdr));

        if (frame_hdr.type != 0xF1FA) {
            ptr += frame_hdr.size;
            continue;
        }

        const uint8_t* chunk_ptr = ptr + sizeof(FLIFrameHeader);
        const uint8_t* frame_end = ptr + frame_hdr.size;

        for (uint16_t c = 0; c < frame_hdr.chunks; c++) {
            if (chunk_ptr + sizeof(FLIChunkHeader) > frame_end) break;

            FLIChunkHeader chunk_hdr;
            memcpy(&chunk_hdr, chunk_ptr, sizeof(chunk_hdr));

            const uint8_t* chunk_data = chunk_ptr + sizeof(FLIChunkHeader);
            uint32_t data_size = chunk_hdr.size - sizeof(FLIChunkHeader);

            switch (chunk_hdr.type) {
                case 11: // 0x0B: FLI_COLOR
                case 4:  // 0x04: FLI_COLOR_256
                    decode_color(chunk_data, data_size);
                    break;
                case 12: // 0x0C: FLI_LC
                    decode_lc(chunk_data, backbuf);
                    break;
                case 13: // 0x0D: FLI_BLACK
                    memset(backbuf, 0, SCREEN_W * SCREEN_H);
                    break;
                case 15: // 0x0F: FLI_BRUN
                    decode_brun(chunk_data, backbuf);
                    break;
                case 16: // 0x10: FLI_COPY
                    decode_copy(chunk_data, backbuf);
                    break;
                default:
                    break;
            }

            chunk_ptr += chunk_hdr.size;
        }

        video.present();
        ptr = frame_end;

        // Check user input to skip intro (ESC, Space, Enter, Left Click, Quit)
        input.update();
        if (input.quit_requested()) {
            Audio::stop();
            return false;
        }

        if (input.was_left_clicked() || input.is_key_pressed(SDLK_ESCAPE) ||
            input.is_key_pressed(SDLK_SPACE) || input.is_key_pressed(SDLK_RETURN)) {
            skipped = true;
            break;
        }

        // Frame timing (false when a quit was requested, which aborts playback)
        if (!pacer.pace(input)) {
            Audio::stop();
            return false;
        }
    }

    Audio::stop();
    return true;
}