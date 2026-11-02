#pragma once
#include <string>
#include <cstdint>
#include "video.h"
#include "input.h"

enum class CommandType {
    OTHER_OR_TOWN = 1, // byte_20539 == 1 (town, shop: pack.exe SM <cmd>)
    BATTLE_SCE    = 2  // byte_20539 == 2 (battle scenario: combatII.exe <cmd>)
};

class MainGame {
public:
    // References the single Video/Input instances owned by gene2.cpp.
    MainGame(Video& video, Input& input);
    ~MainGame();

    // Run the main_ loop from MAIN.LST.
    int run();

private:
    Video& video_;
    Input& input_;

    // sub_10014: parse the "$" file and classify the command type.
    bool read_dollar_file(std::string& out_cmd, CommandType& out_type);

    // Delete the "$" file.
    void unlink_dollar_file();

    // Check the LOAD flags (CSTLDATA/LOADED, CSTLDATA/LOAD1..5, LOAD).
    bool check_load_flag();

    // Run an external module (chp.exe, combatII.exe, pack.exe).
    int execute_combat(const std::string& arg);
    int execute_chp(const std::string& arg);
    int execute_pack(const std::string& mode, const std::string& arg);

    std::string game_path(const char* filename);
    std::string cstldata_path(const char* filename);
};