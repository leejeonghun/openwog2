// Data structures for the CHP and PACK modules
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

// ---- Calendar/time system ----
constexpr int DAYS_PER_YEAR = 220;
constexpr int BASE_YEAR = 1207;
constexpr int TIME_PHASES_PER_DAY = 8;

// ---- Contibak / chapter strides ----
constexpr int CONTIBAK_CHAPTER_STRIDE = 2374; // 0x946 bytes per chapter
constexpr int CONTIBAK_PATH_STRIDE    = 200;  // 0xC8 bytes per path
constexpr int MAX_CHAPTERS            = 20;
constexpr int MAX_PATHS_PER_CHAPTER   = 15;
constexpr int MAX_POINTS_PER_CHAPTER  = 30;   // 0x1E
constexpr int MAX_CHARACTERS          = 123;  // 0x7B
constexpr int MAX_EVENT_TRIGGERS      = 20;   // 0x14
constexpr int MAX_TEAMS               = 10;

// ---- Blackbox inventory slots (sub_15BD9 / sub_15DB7) ----
constexpr int MAX_BLACKBOX_SLOTS      = 100;  // 0x64
constexpr int BLACKBOX_SLOT_ITEMS     = 300;  // 300 words (600 bytes) per slot
constexpr int BLACKBOX_SLOT_STRIDE    = 600;  // 0x258 bytes of item IDs per slot

// ---- Per-chapter trailer offsets ----
constexpr int CHAPTER_TRAIL_EVENT_COUNT = 0x942; // AC1C2: event counter
constexpr int CHAPTER_TRAIL_SCENE_A     = 0x943; // AC1C3: scene marker A
constexpr int CHAPTER_TRAIL_MARK2       = 0x944; // AC1C4: secondary marker
constexpr int CHAPTER_TRAIL_SCENE_B     = 0x945; // AC1C5: scene marker B

// ---- Script VM constants ----
constexpr int VM_SCRIPT_RECORD_SIZE    = 2000;   // 0x7D0 bytes per script
constexpr int VM_SCRIPT_MEM_STRIDE     = 4000;   // 0xFA0 bytes stride in memory
constexpr int VM_INSTR_WORDS           = 25;     // 25 words (50 bytes) per instruction
constexpr int VM_INSTR_SIZE            = VM_INSTR_WORDS * 2; // 50 bytes
constexpr int VM_MAX_INSTRUCTIONS      = VM_SCRIPT_RECORD_SIZE / VM_INSTR_SIZE; // 40

constexpr int TRIGGER_STRUCT_SIZE      = 22;     // 0x16 bytes per trigger condition
constexpr int QUEST_COUNT              = 10;     // 10 quest slots (C5CE8)
constexpr int QUEST_NAME_LEN           = 13;     // 13 bytes per quest name
constexpr int QUEST_STATE_SIZE         = 148;    // 8A872 array size

// ---- VM opcodes ----
enum VMOpcode : uint16_t {
    VM_DELETE_POINT     = 1,
    VM_NOP2             = 2,
    VM_CREATE_POINT     = 3,
    VM_TEAM_SWITCH      = 4,
    VM_MESSAGE_TABLE    = 5,
    VM_MESSAGE_INLINE   = 6,
    VM_ENTER_POINT      = 7,
    VM_WARP             = 8,
    VM_EVENT_SWITCH     = 9,
    VM_SET_POINT_NAME   = 10,
    VM_NOP              = 11,
    VM_POINT_KIND       = 12,
    VM_NARRATION        = 13,
    VM_TIME_PASS        = 14,
    VM_PARTY_MERGE      = 15,
};

#pragma pack(push, 1)
struct WorldPoint {
    char display_name[20];   // +0x00: Display name (Korean)
    uint8_t active;          // +0x14: 1=visible, 0=hidden
    uint8_t kind;            // +0x15: 0=TOWN, 1=BATTLE, 2=NULL, 3=JUMP
    uint16_t unused_lo;      // +0x16
    int16_t map_x;           // +0x18
    int16_t map_y;           // +0x1A
    uint8_t data[38];        // +0x1C: id_name[13] + tail[25]

    const char* id_name() const { return reinterpret_cast<const char*>(data); }
    uint16_t& respawn_val() { return *reinterpret_cast<uint16_t*>(data + 14); }
};
static_assert(sizeof(WorldPoint) == 66, "WorldPoint must be 66 bytes");

// PACK.ASM reads character_id at byte_19AC33, which is unk_19AC24 + 0x0F, so name is 15 bytes
// (not 16) and character_id follows at +0x0F. Keeping name at 15 bytes and padding at 87 keeps
// every later field - including town_sprite_id at +0x61, read by sub_2117F - at the offset the
// original uses, with the same 136-byte total.
struct CharacterData {
    char name[15];           // +0x00
    uint8_t character_id;    // +0x0F
    uint8_t class_id;        // +0x10
    uint8_t portrait_id;     // +0x11
    uint8_t level;           // +0x12
    uint32_t current_hp;     // +0x13
    uint32_t max_hp;         // +0x17
    uint16_t current_mp;     // +0x1B
    uint16_t max_mp;         // +0x1D
    uint32_t exp;            // +0x1F
    uint8_t str;             // +0x23
    uint8_t dex;             // +0x24
    int8_t  int_;            // +0x25
    uint8_t equip_weapon_r;  // +0x26
    uint8_t equip_weapon_l;  // +0x27
    uint8_t equip_armor;     // +0x28
    uint8_t equip_helmet;    // +0x29
    uint8_t equip_accessory; // +0x2A
    uint8_t party_team_id;   // +0x2B
    uint8_t team_field;      // +0x2C (unused by CHP; the team byte is at +0x87, see chr_team())
    uint8_t equip_stat_attack;  // +0x2D
    uint8_t equip_stat_defense; // +0x2E
    uint8_t equip_stat_magic_def; // +0x2F
    uint8_t equip_stat_speed;   // +0x30
    // +0x31: the town_sprite_id word lives at padding[0x61-0x31] = padding[48]
    // (PACK.ASM sub_2117F: `mov ax, word_19AC85[eax*8]`; see Pack::init_player_sprite_base())
    uint8_t padding[87];        // +0x31
};
static_assert(sizeof(CharacterData) == 136, "CharacterData must be 136 bytes");

// CHP.ASM: byte_8FFE3[idx*0x88] = unk_8FF5C + 0x87 - the party team byte of a character record
// (0x63 = waiting roster). sub_15A7F / sub_15AE5 / sub_15B40 and the VM party operations all use
// it.
constexpr int CHR_TEAM_OFFSET = 0x87;
inline uint8_t& chr_team(CharacterData& c) { return reinterpret_cast<uint8_t*>(&c)[CHR_TEAM_OFFSET]; }
inline uint8_t  chr_team(const CharacterData& c) { return reinterpret_cast<const uint8_t*>(&c)[CHR_TEAM_OFFSET]; }
static_assert(offsetof(CharacterData, character_id) == 0x0F,
              "character_id must sit at +0x0F (PACK.ASM byte_19AC33 cross-check)");

struct EventTriggerCondition {
    int16_t chapter;
    int16_t point_id;
    int16_t state_check;
    int16_t flag_idx1;
    int16_t flag_idx2;
    int16_t flag_idx3;
    int16_t flag_val1;
    int16_t flag_val2;
    int16_t flag_val3;
    int16_t pass_days;
    int16_t exec_class;
};
static_assert(sizeof(EventTriggerCondition) == 22, "EventTriggerCondition must be 22 bytes");

struct QuestObjective {
    char name[QUEST_NAME_LEN];
};
static_assert(sizeof(QuestObjective) == 13, "QuestObjective must be 13 bytes");
#pragma pack(pop)

// ---- State shared by CHP, PACK and the other modules ----
extern uint8_t g_event_flags[20000];
extern WorldPoint g_world_points[MAX_POINTS_PER_CHAPTER];
extern CharacterData g_characters[MAX_CHARACTERS];
extern uint32_t g_slot_gold[MAX_BLACKBOX_SLOTS];
extern uint16_t g_slot_item_count[MAX_BLACKBOX_SLOTS];
extern uint16_t g_slot_items[MAX_BLACKBOX_SLOTS][BLACKBOX_SLOT_ITEMS];

struct GameState {
    uint8_t  time_of_day;
    uint8_t  current_point;
    uint8_t  current_scene;
    uint8_t  current_day;
    uint16_t current_year;

    uint8_t  chapter_id;
    uint16_t chapter_day_count;

    uint16_t limit_year;
    uint8_t  limit_day;
    uint8_t  limit_extra;

    int32_t  camera_x;
    int32_t  camera_y;

    uint8_t event_flags[20000];

    uint32_t team_gold;
    bool     in_point;
    uint8_t  team_active;
    uint8_t  display_mode;
    uint8_t  config_data[10];
    uint8_t  travel_style;   // word_C5DD9+1 (CONTIBAK state block [8]): 0 none / 1 bigong1 / 2 bigong2 / 3 seal

    uint16_t party_count;

    uint8_t  quest_state[QUEST_STATE_SIZE];
    uint8_t  chapter_clear;
    uint8_t  secondary_state;
    uint16_t quest_counter;
    uint8_t  chapter_scene_marker;
    uint8_t  chapter_scene_target;
    uint16_t chapter_start_year;
    uint8_t  chapter_start_day;
    uint8_t  chapter_started_flag[MAX_CHAPTERS];


    char     pending_command[64];
    bool     transition_pending;

    void clear() {
        memset(this, 0, sizeof(*this));
    }
};

namespace IPC {
    constexpr const char* LOAD1 = "LOAD1";
    constexpr const char* LOAD2 = "LOAD2";
    constexpr const char* LOAD3 = "LOAD3";
    constexpr const char* LOAD4 = "LOAD4";
    constexpr const char* LOAD5 = "LOAD5";
    constexpr const char* LOADED = "LOADED";
    constexpr const char* MENU = "MENU";
    constexpr const char* EXIT = "EXIT";
    constexpr const char* PASS = "PASS";
    constexpr const char* START = "START";

    constexpr const char* BLACKBOX_DAT = "blackbox.dat";
    constexpr const char* BLACKBOX_DEF = "blackbox.def";
    constexpr const char* CONTIBAK_TMP = "contibak.tmp";
    constexpr const char* CONTMAP_DAT = "contmap.dat";
    constexpr const char* GAME_CFG = "game.cfg";
    constexpr const char* DOLLAR = "$";
}