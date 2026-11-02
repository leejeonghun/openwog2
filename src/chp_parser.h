#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "data_structures.h"

constexpr int CHP_HEADER_SIZE = 128;         // 0x80
constexpr int CHP_PALETTE_SIZE = 768;        // 256 * 3
constexpr int CHP_RLE_MARKER = 0xC0;
constexpr int CHP_RLE_COUNT_MASK = 0x3F;

constexpr int MAP_BUFFER_WIDTH = 960;        // 0x3C0
constexpr int MAP_BUFFER_HEIGHT = 700;
constexpr int MAP_BUFFER_SIZE = MAP_BUFFER_WIDTH * MAP_BUFFER_HEIGHT;

class CHPParser {
public:
    CHPParser();
    ~CHPParser();

    bool load(const std::string& filepath);
    bool load_palette_file(const std::string& filepath);

    const uint8_t* get_map_data() const { return map_data_.data(); }
    int get_map_size() const { return (int)map_data_.size(); }

    auto get_palette() const -> const uint8_t (*)[3] { return palette_; }
    const std::string& get_error() const { return error_; }

private:
    bool decompress_rle(FILE* fp, std::vector<uint8_t>& output, int max_size);
    bool read_palette(FILE* fp);

    std::vector<uint8_t> map_data_;
    uint8_t palette_[256][3] = {};
    std::string error_;
};