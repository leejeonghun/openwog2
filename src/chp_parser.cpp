#include "chp_parser.h"
#include "fileutil.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

CHPParser::CHPParser() = default;
CHPParser::~CHPParser() = default;

// sub_2DE6F: RLE decompression matching CHP.ASM.
bool CHPParser::decompress_rle(FILE* fp, std::vector<uint8_t>& output, int max_size) {
    output.assign(max_size, 0);

    if (fseek(fp, CHP_HEADER_SIZE, SEEK_SET) != 0) {
        error_ = "Failed to seek to RLE offset";
        return false;
    }

    int esi = 0;
    while (esi < max_size) {
        int byte_val = fgetc(fp);
        if (byte_val == EOF) break;

        uint8_t b = (uint8_t)byte_val;

        if ((b & 0xC0) == CHP_RLE_MARKER) {
            int count = b & CHP_RLE_COUNT_MASK;
            int val = fgetc(fp);
            if (val == EOF) break;
            uint8_t value = (uint8_t)val;

            for (int i = 0; i < count && esi < max_size; i++) {
                output[esi++] = value;
            }
        } else {
            output[esi++] = b;
        }
    }

    fprintf(stderr, "CHP: RLE decompressed %d bytes (target: %d)\n", esi, max_size);
    return esi > 0;
}

// Reads the 768-byte RGB palette stored in the last 768 bytes of the file and scales
// each component to the 6-bit DAC range the renderer expects.
bool CHPParser::read_palette(FILE* fp) {
    if (fseek(fp, -CHP_PALETTE_SIZE, SEEK_END) != 0) {
        error_ = "Failed to seek to palette";
        return false;
    }

    uint8_t rgb[768];
    if (fread(rgb, 1, 768, fp) != 768) {
        error_ = "Failed to read palette";
        return false;
    }

    for (int i = 0; i < 256; i++) {
        palette_[i][0] = rgb[i * 3 + 0] >> 2;
        palette_[i][1] = rgb[i * 3 + 1] >> 2;
        palette_[i][2] = rgb[i * 3 + 2] >> 2;
    }

    return true;
}

bool CHPParser::load(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
        error_ = "Cannot open file: " + filepath;
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < CHP_HEADER_SIZE) {
        error_ = "File too small";
        fclose(fp);
        return false;
    }

    if (file_size >= CHP_HEADER_SIZE + CHP_PALETTE_SIZE) {
        read_palette(fp);
    }

    // Map buffer: 960 * 700 = 672000 bytes; sub_2DE6F allocates 0xA6040 = 680000.
    constexpr int MAX_MAP_SIZE = 680000;
    if (!decompress_rle(fp, map_data_, MAX_MAP_SIZE)) {
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

bool CHPParser::load_palette_file(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) return false;

    uint8_t pal_data[768];
    if (fread(pal_data, 1, 768, fp) != 768) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    for (int i = 0; i < 256; i++) {
        palette_[i][0] = pal_data[i * 3 + 0];
        palette_[i][1] = pal_data[i * 3 + 1];
        palette_[i][2] = pal_data[i * 3 + 2];
    }
    return true;
}