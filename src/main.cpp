#include "main.h"
#include "chp.h"
#include "pack.h"
#include "combatii.h"
#include "fileutil.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

MainGame::MainGame(Video& video, Input& input) : video_(video), input_(input) {}
MainGame::~MainGame() = default;

std::string MainGame::game_path(const char* filename) {
    return file_find_case_insensitive(GAME_DATA_DIR, filename);
}

std::string MainGame::cstldata_path(const char* filename) {
    return file_find_case_insensitive(std::string(GAME_DATA_DIR) + "/CSTLDATA", filename);
}

void MainGame::unlink_dollar_file() {
    std::string dollar = game_path("$");
    if (file_exists(dollar.c_str())) {
        file_delete(dollar.c_str());
    }
}

// ---- sub_10014: parse the "$" file and detect an SCE command (MAIN.LST 0x10014) ----
//   __STOSB(unk_2052C, 0, 0Dh)              ; clear the 13-byte buffer
//   fp = fopen("$", "rt")                   ; on failure the buffer stays empty
//   fscanf(fp, "%s", unk_2052C); fclose
//   for (edx = 0; edx < 0Dh && unk_2052C[edx] != '.'; edx++) ;  // find the first '.'
//   ext[0..2] = unk_2052C[1+edx .. 3+edx]   ; 3 chars after '.' (dword_10010=0 -> NUL)
//   byte_20539 = (ext=="sce" || ext=="SCE") ? 2 : 1
// The caller main_ only tests for "EXIT", so a missing file or blank content is a normal
// state. COMBATII leaves " " in `$` when a scenario ends (sub_506F3) and when it starts
// without an argument and contmap.dat is missing (main_+27); no content therefore means
// no command, not failure.
bool MainGame::read_dollar_file(std::string& out_cmd, CommandType& out_type) {
    out_cmd.clear();
    out_type = CommandType::OTHER_OR_TOWN;   // byte_20539 = 1 (default)

    std::string dollar_path = game_path("$");
    FILE* fp = file_exists(dollar_path.c_str()) ? fopen(dollar_path.c_str(), "rt") : nullptr;
    if (fp) {
        char buf[64] = { 0 };
        if (fscanf(fp, "%63s", buf) == 1) {
            out_cmd = buf;
        }
        fclose(fp);
    }

    // Find '.' (0x1007C: the first '.' within the first 13 bytes).
    size_t dot_pos = std::string::npos;
    for (size_t i = 0; i < out_cmd.length() && i < 13; ++i) {
        if (out_cmd[i] == '.') { dot_pos = i; break; }
    }
    if (dot_pos != std::string::npos && dot_pos + 3 < out_cmd.length()) {
        std::string ext = out_cmd.substr(dot_pos + 1, 3);
        if (ext == "sce" || ext == "SCE") {
            out_type = CommandType::BATTLE_SCE; // byte_20539 = 2
            return !out_cmd.empty();
        }
    }

    out_type = CommandType::OTHER_OR_TOWN; // byte_20539 = 1
    return !out_cmd.empty();               // diagnostic: empty (missing file/blank) still passes
}

// ---- main_ 0x10152: check the LOAD flags (MAIN.LST) ----
bool MainGame::check_load_flag() {
    std::string root_load = game_path("LOAD");
    if (file_exists(root_load.c_str())) {
        return true;
    }

    std::string loaded = cstldata_path("LOADED");
    if (file_exists(loaded.c_str())) {
        return true;
    }

    for (int i = 1; i <= 5; i++) {
        char flag_name[16];
        snprintf(flag_name, sizeof(flag_name), "LOAD%d", i);
        std::string load_i = cstldata_path(flag_name);
        if (file_exists(load_i.c_str())) {
            return true;
        }
    }

    return false;
}

// Main_ 0x1012D: unlink("$") -> spawnl(0, "chp.exe", "chp.exe", "p", NULL).
//   The child argv is {"chp.exe", "p"} -> argc=2, argv[1]="p", argv[2]=NULL.
//   No chapter number is passed: CHP opens sub_10C6D (selection screen) when "START"
//   exists and otherwise picks the chapter from contibak.tmp.
int MainGame::execute_chp(const std::string& arg) {
    fprintf(stderr, "MAIN: executing CHP module (argv[1]: %s)\n", arg.c_str());
    CHP chp(video_, input_);
    return chp.run(arg.empty() ? "p" : arg.c_str(), nullptr);
}

int MainGame::execute_combat(const std::string& arg) {
    fprintf(stderr, "MAIN: executing CombatII (scenario: %s)\n", arg.empty() ? "<empty/init>" : arg.c_str());
    CombatII combat(video_, input_);
    combat.run(arg);
    // Pass COMBATII.EXE's exit code through unchanged (the argument of sub_61CFF):
    //   0xFF = end of main_, or no argument with contmap.dat missing, or ESC
    //   0x01 = sub_52D33 (next scenario 0x12) / sub_506F3 (stage 0x01 clear)
    // main_ tests `cmp eax, 0FFh; jnz loc_100EA`, so any other code restarts combatII at
    // loc_100EA without reading '$'. On the stage-clear path contmap.dat is gone, so that
    // restart immediately writes " " to '$' and ends with 0xFF; the later '$' read then
    // carries the handoff.
    int code = combat.exit_code();
    fprintf(stderr, "MAIN: CombatII exit code: 0x%X\n", (unsigned)code);
    return code;
}

int MainGame::execute_pack(const std::string& mode, const std::string& arg) {
    fprintf(stderr, "MAIN: executing Pack (mode: %s, cmd: %s)\n", mode.c_str(), arg.c_str());
    Pack pack(video_, input_);
    return pack.run(mode, arg);
}

// ---- main_ (0x100DF): MAIN.EXE dispatch loop ----
int MainGame::run() {
    fprintf(stderr, "MAIN: === Game dispatcher started ===\n");

    std::string cmd;
    CommandType cmd_type;
    // sub_10014: fill cmd/byte_20539 from '$'. There is no failure path - a missing file
    // yields an empty string, and main_ only tests for "EXIT" (everything else continues).
    auto read_cmd = [&]() { read_dollar_file(cmd, cmd_type); };

    while (true) {
        // loc_100EA: unlink("$") -> spawnl("combatII.exe", NULL)
        unlink_dollar_file();
        int ret = execute_combat("");
        // Main_ 0x100F2..0x1010F: `cmp eax, 0FFh; jnz loc_100EA`
        //   A code other than 0xFF (scenario switch / stage clear) skips the '$' read and
        //   restarts combatII at loc_100EA. When contmap.dat is missing that restart leaves
        //   only " " in '$' and ends with 0xFF.
        if (ret != 0xFF) continue;

        // 0x10111: sub_10014 -> strcmp(unk_2052C, "EXIT"); jz -> exit
        read_cmd();
        if (cmd == "EXIT") break;

        // 0x1012D: unlink("$") -> spawnl("chp.exe", "p")
        unlink_dollar_file();
        if (execute_chp("p") != 0) break; // stop the dispatcher when the window is closed

        // 0x10152: fopen("LOAD", "rt") succeeds -> exit back to TTL
        if (check_load_flag()) {
            fprintf(stderr, "MAIN: LOAD flag detected, returning to TTL\n");
            break;
        }

        // 0x10169: sub_10014 -> exit on "EXIT", otherwise (empty included) continue
        read_cmd();
        if (cmd == "EXIT") break;

        // 0x10185: branch on byte_20539 (2 = scenario/battle, 1 = town/shop)
        if (cmd_type == CommandType::OTHER_OR_TOWN) {
            // 0x1018E: unlink("$") -> spawnl("pack.exe", "SM", cmd); then jmp loc_100EA
            unlink_dollar_file();
            execute_pack("SM", cmd);
            continue;
        }

        // loc_101BD: unlink("$") -> spawnl("combatII.exe", cmd)
        unlink_dollar_file();
        ret = execute_combat(cmd);
        if (ret != 0xFF) continue;   // 0x101E7: anything but 0FFh returns to loc_100EA

        // 0x101ED: sub_10014 -> exit on "EXIT", otherwise loc_100EA
        read_cmd();
        if (cmd == "EXIT") break;
        continue;
    }

    unlink_dollar_file();
    fprintf(stderr, "MAIN: === Game dispatcher terminated ===\n");
    return 0;
}