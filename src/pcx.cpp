#include "pcx.h"
#include "palette.h"
#include <cstdio>
#include <cstring>
#include <vector>

#pragma pack(push, 1)
struct PCXHeader {
    uint8_t id;
    uint8_t version;
    uint8_t encoding;
    uint8_t bits_per_pixel;
    uint16_t x_min, y_min, x_max, y_max;
    uint16_t h_dpi, v_dpi;
    uint8_t ega_palette[48];
    uint8_t reserved;
    uint8_t num_color_planes;
    uint16_t bytes_per_line;
    uint16_t palette_type;
    uint8_t filler[58];
};
#pragma pack(pop)

bool pcx_load(const char* filename, uint8_t* buffer, int buf_w, int buf_h) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "PCX: warning: Cannot open %s\n", filename);
        return false;
    }

    PCXHeader header;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return false;
    }

    if (header.id != 0x0A) {
        fprintf(stderr, "PCX: Not a valid PCX file (id=0x%02X)\n", header.id);
        fclose(fp);
        return false;
    }

    int width = header.x_max - header.x_min + 1;
    int height = header.y_max - header.y_min + 1;
    int bytes_per_line = header.bytes_per_line;

    std::vector<uint8_t> scanline(bytes_per_line * header.num_color_planes);

    for (int y = 0; y < height && y < buf_h; y++) {
        for (int p = 0; p < header.num_color_planes; p++) {
            int x = 0;
            while (x < bytes_per_line) {
                int byte_val = fgetc(fp);
                if (byte_val == EOF) break;

                if ((byte_val & 0xC0) == 0xC0) {
                    int count = byte_val & 0x3F;
                    int val = fgetc(fp);
                    if (val == EOF) break;
                    for (int i = 0; i < count && x < bytes_per_line; i++) {
                        scanline[p * bytes_per_line + x] = val;
                        x++;
                    }
                } else {
                    scanline[p * bytes_per_line + x] = byte_val;
                    x++;
                }
            }
        }

        if (header.num_color_planes == 1) {
            for (int x = 0; x < width && x < buf_w; x++) {
                buffer[y * buf_w + x] = scanline[x];
            }
        } else {
            for (int x = 0; x < width && x < buf_w; x++) {
                uint8_t idx = 0;
                for (int p = 0; p < header.num_color_planes; p++) {
                    idx |= scanline[p * bytes_per_line + x] << (p * header.bits_per_pixel);
                }
                buffer[y * buf_w + x] = idx;
            }
        }
    }

    // Read 256-color palette and convert 8-bit to 6-bit DAC (sub_16101: sar edx, 2)
    if (header.bits_per_pixel == 8 && header.num_color_planes == 1) {
        fseek(fp, -769, SEEK_END);
        uint8_t palette_marker = fgetc(fp);
        if (palette_marker == 0x0C) {
            uint8_t pal[256][3];
            if (fread(pal, 3, 256, fp) == 256) {
                for (int i = 0; i < 256; i++) {
                    pal[i][0] >>= 2;
                    pal[i][1] >>= 2;
                    pal[i][2] >>= 2;
                }
                palette_set_range(0, 255, pal);
            }
        }
    }

    fclose(fp);
    return true;
}

bool pcx_load_to_buffer(const std::string& filename, uint8_t* buffer, int buf_w, int buf_h) {
    return pcx_load(filename.c_str(), buffer, buf_w, buf_h);
}

bool pcx_load_palette(const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return false;

    fseek(fp, -769, SEEK_END);
    uint8_t marker = fgetc(fp);
    if (marker == 0x0C) {
        uint8_t pal[256][3];
        if (fread(pal, 3, 256, fp) == 256) {
            for (int i = 0; i < 256; i++) {
                pal[i][0] >>= 2;
                pal[i][1] >>= 2;
                pal[i][2] >>= 2;
            }
            palette_set_range(0, 255, pal);
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}