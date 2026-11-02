// Values and strings for the right-click status window (sub_2F09A), extracted from CHP.EXE.
// Sprites are read from CHP.EXE VAs at runtime.
#pragma once
#include <cstdint>

// flt_51D74 / flt_51D78 / flt_51D7C: scrollbar length math (6.0 = rows per screen,
// 123.0 = scrollbar track length).
static const float kChpStatusFlt51D74 = 6.0f;
static const float kChpStatusFlt51D78 = 123.0f;
static const float kChpStatusFlt51D7C = -123.0f;

// byte_7B46E / 7B46F / 7B470: palette index of the light edge, dark edge and fill used by
// sub_1C6F6 (bevel box).
static const uint8_t kChpFrameLight = 1, kChpFrameDark = 6, kChpFrameFill = 13;

// off_529CC icon table: slot n (n < 53) is an RLE stream starting at n*0x1AE inside the status
// icon buffer; three extra icons start at 0x5906 + n*0x15E.
static const int kChpStatusIconStride = 0x1ae, kChpStatusIconCount = 53;
static const int kChpStatusExtraOffset = 0x5906, kChpStatusExtraStride = 0x15e, kChpStatusExtraCount = 3;

// AKnight: class name labels (record field +0x11, 16-byte stride).
static const int kChpClassNameCount = 164;
static const char* const kChpClassNames[164] = {
    "Knight", "Paladin", "Death lord", "Dragon knight", "King", "Dragon slayer",
    "Fighter", "Assassin", "Gradiater", "Valkyrie", "Thief", "Ranger",
    "Scout", "Archeaogist", "Soldier", "Lancer", "General", "Dark General",
    "Guard", "Royal Guard ", "Earthquaker", "Archer", "Hunter", "Eagle",
    "Sniper", "Mage", "Magician", "Alchemist", "Fire Master", "Ice Master",
    "Thunder Master", "Wizard", "Dark Wizard", "Monk", "Cleric", "Priest",
    "Druid", "Bishop", "Warlock", "Necromancer", "Lich", "Sorcerer",
    "Sharman", "Exorcist", "Warrior", "Berserker", "Genoicider", "Tusa",
    "Martial Arts", "Musa", "Sword Master", "Tamer", "Savant", "Killer",
    "Cleaner", "Shadow Killer", "Shadow Master", "Merman Soldier", "Merman Armor", "Merman Lord",
    "Valtan Soldier", "Asult Valtan", "Valtan Lord", "Goblin", "Goblin Lord", "Orc",
    "Orc Lord", "Ogre", "Ogre Lord", "Skeleton", "Dark Skeleton", "DTWarrior",
    "Slime", "Dark Slime", "Ghoul", "Zombi", "Bat", "Bamp",
    "Vampire", "Vampire Lord", "Scorpion", "Wyvern", "Gargoyle", "Fire Golem",
    "Ice Golem", "Thunder Golem", "Dark Golem", "Dao", "Djinni", "Marid",
    "Salamanda", "Kraken", "Fire Dragon", "Dark Dragon", "Harpy", "Grima",
    "Brown Horse", "White Horse", "Green Horse", "Black Horse", "Blue Drake", "Dark Drake",
    "Cannon M. A.", "I. Cannon M. A.", "Arrow M. A.", "I. Arrow M. A.", "Shaft M. A. ", "I. Shaft M.A.",
    "Aero M.A.", "I. Aero M.A.", "Abarest", "Arondight", "Asura", "Dun Grnid",
    "ElGenaro", "Garius", "Ra", "Yanus", "CarrierAbarest", "Arondight",
    "Asura", "Dun Grnid", "ElGenaro", "Garius", "Ra", "Yanus",
    "Blue Drake", "Dark Drake", "F.Arondight", "F.Asura", "F.Dun Grnid", "F.Ra",
    "Asmodeus", "Infantry", "I. Infantry", "Cavalry", "I. Cavalry", "Spear",
    "I. Spear", "Bow", "I. Bow", "Magic", "I. Magic", "Canon",
    "I. Canon", "Galley Ship", "I. Galley", "Sailer", "I. Sailer", "Ironclad Ship",
    "BattleShip", "I. BattleShip", "Air Fighter", "I. Air Fighter", "Air Advanced", "I. Air Advanced",
    "Air Battler", "I, Air Battler", "Air Pirate", "Ace Pirate", "Canon", "Gate",
    "Oddy Guardian", "God",
};

// ABEviEXVJiia_0: Johab message shown by the sub_2E8FC popup when nothing more can be given.
static const char kChpStatusErrMsg[] = "\xb7\xa1\x20\xb7\xa5\xa2\x89\xb7\x65\x20\xb8\xe5\xbb\xa2\x20\xa6\x89\x88\x61\x21";
