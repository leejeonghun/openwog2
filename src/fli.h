#pragma once
#include <string>
#include <cstdint>
#include "video.h"
#include "input.h"

// Autodesk FLI/FLC Animation Player (sub_186DD / sub_1882C / sub_1948B)
class FLIPlayer {
public:
    static bool play(const std::string& fli_path, const std::string& voc_path,
                     Video& video, Input& input);

private:
    static void decode_color(const uint8_t* data, uint32_t size);
    static void decode_lc(const uint8_t* data, uint8_t* buffer);
    static void decode_brun(const uint8_t* data, uint8_t* buffer);
    static void decode_copy(const uint8_t* data, uint8_t* buffer);
};