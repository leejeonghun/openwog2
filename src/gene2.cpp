#include <cstdio>
#include <string>
#include <cctype>
#include "ttl.h"
#include "chp.h"
#include "pack.h"
#include "combatii.h"
#include "main.h"
#include "fileutil.h"
#include "video.h"
#include "input.h"
#include "audio.h"
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    fprintf(stderr, "MAIN: === The War of Genesis II - C++/SDL2 Port ===\n\n");

    // Single Video/Input instances shared by reference by every module: the window is
    // created once and closed once at exit, so switching modules never flickers.
    Video video;
    if (!video.init()) {
        fprintf(stderr, "MAIN: error: video init failed\n");
        return -1;
    }
    video.set_mode_13h();

    Input input;
    input.init();
    {
        // Log the driver and window size once at startup. Mouse events are already in
        // logical units; scaling them again double-divides the coordinate.
        int ww = 0, wh = 0;
        SDL_Window* w = video.get_window();
        if (w) SDL_GetWindowSize(w, &ww, &wh);
        fprintf(stderr, "VIDEO: driver=%s window=%dx%d\n",
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
                ww, wh);
    }

    int exit_code = 0;

    // With a command-line argument, run that module directly.
    if (argc >= 2) {
        std::string arg1 = argv[1];
        if (arg1 == "chp" || arg1 == "CHP") {
            // Original argv model: chp.exe <argv1> [argv2]
            //   argv1: 'p' = enter after battle / continue from MAIN, 's' = start a chapter
            //          file, 't' = generator tool, a number = 's <number>' (port convenience)
            //   argv2: chapter number read from \chapdata\<argv2>.chp
            const char* a1 = (argc >= 3) ? argv[2] : "p";
            const char* a2 = (argc >= 4) ? argv[3] : nullptr;
            CHP chp(video, input);
            exit_code = chp.run(a1, a2);
        } else if (arg1 == "main" || arg1 == "MAIN") {
            MainGame main_game(video, input);
            exit_code = main_game.run();
        }
        else if (arg1 == "pack" || arg1 == "PACK") {
            // Original PACK.EXE argv model: pack.exe <SM|TOOL> <number>
            //   argv[1]: "SM" (town) or "TOOL" (map tool); case-sensitive, anything else
            //            exits 0xFF
            //   argv[2]: numeric string; MAIN passes the '$' contents verbatim and the
            //            original runs atoi, stores it in a byte, and exits when it is 0xFF
            // Passed through unchanged, as for COMBATII, for development and test entry.
            const char* m = (argc >= 3) ? argv[2] : "SM";
            const char* t = (argc >= 4) ? argv[3] : "1";
            Pack pack(video, input);
            exit_code = pack.run(m, t ? t : "1");
        } else if (arg1 == "combat" || arg1 == "combatii" || arg1 == "COMBATII") {
            CombatII combat(video, input);
            std::string param = (argc >= 3) ? argv[2] : "";
            exit_code = combat.run(param);
        }
        video.shutdown();
        Audio::shutdown();
        return exit_code;
    }

    // Default path: GENE2.COM / TTL -> MAIN.EXE game loop.
    while (true) {
        TTL ttl(video, input);
        int result = ttl.run();

        // TTL returns 1 when the player chose New Game, Load Game or Continue Battle.
        if (result == 1) {
            MainGame main_game(video, input);
            main_game.run();

            // MAIN exited on a LOAD flag: loop back to TTL to load the slot.
            std::string dollar = file_find_case_insensitive(GAME_DATA_DIR, "$");
            std::string start_flag = file_find_case_insensitive(GAME_DATA_DIR, "START");
            
            // A plain EXIT leaves neither file behind: quit for good.
            if (!file_exists(dollar.c_str()) && !file_exists(start_flag.c_str())) {
                break;
            }
        } else {
            // To DOS (0) or an error (-1).
            break;
        }
    }

    video.shutdown();
    Audio::shutdown();
    return 0;
}