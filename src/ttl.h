#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "video.h"
#include "palette.h"
#include "font.h"
#include "input.h"
#include "audio.h"

struct GameConfig {
    uint8_t sound_card_type;
    uint8_t irq;
    uint8_t dma;
    uint16_t io_port;
    uint8_t midi_type;
    uint8_t extra[5];
};

enum class IPCFlag : int {
    NONE = 0,
    MENU = 3,
    LOAD1 = 4,
    LOAD2 = 5,
    LOAD3 = 6,
    LOAD4 = 7,
    LOAD5 = 8,
};

class TTL {
public:
    // References the single Video/Input instances owned by gene2.cpp, so switching
    // modules never closes and recreates the window.
    TTL(Video& video, Input& input);
    ~TTL();

    int run();

private:
    Video& video_;
    Input& input_;
    Font font_;

    GameConfig config_ = {};
    IPCFlag ipc_flag_ = IPCFlag::NONE;
    bool has_continue_data_ = false;   // blackbox.dat exists -> draw the "Continue Battle" item (sub_137B6)

    // Cursor frame id from Video::cursor_register; Video handles the pressed variant as a pair.
    int cursor_normal_ = -1;

    // Exit control
    bool should_exit_ = false;
    int return_code_ = 0;

    void load_config();
    IPCFlag check_ipc_flags();
    bool copy_file(const char* src, const char* dst);
    // sub_123E3: on failure prints "%s read/write error!" and exits with 0xFF.
    bool copy_or_die(const std::string& src, const std::string& dst);

    void play_intro(bool with_hicom);
    bool play_fli_with_audio(const std::string& fli_name, const std::string& voc_name);

    // Main menu (sub_138A8 / sub_137B6)
    void handle_menu();
    void render_menu();
    void draw_menu_item(int item, bool highlight);   // item 1..4
    int  menu_hit(int mx, int my, bool can_continue) const;
    void new_game();

    // Load screen (sub_13038 / sub_1257C)
    void handle_save_load();
    std::string slot_info_text(int slot);            // slot 1..5
    void draw_slot_row(int row, const bool* exists, bool highlight);   // row 0..4 = slot, 5 = back
    // Start a LOAD slot: copy blackbox.svN / contibak.svN to the live save and hand over to MAIN.
    void start_loaded_game(int slot, bool from_ipc_flag);

    void cleanup_and_exit(int code);

    // Mouse (sub_141B4..sub_14600, user handler loc_143AA)
    bool load_cursor_frames(std::vector<CursorFrame>& frames);   // TTL.EXE VA 0x2096B
    void init_mouse();
    bool left_only() const;       // original button state == 1 (left only, still true while held)
    void wait_left_release();     // sub_1232D

    void draw_text(int x, int y, const char* text);

    std::string game_path(const char* filename);
    std::string cstldata_path(const char* filename);
    std::string chapdata_path(const char* filename);
};
