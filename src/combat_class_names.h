#pragma once
// ==== Combat class names ====
// COMBATII.EXE (LE) object 2 class name table aKnight (VA 0x71118, 16-byte stride, 165 entries).
// sub_48734 reads `aKnight + [unit+11h]*16` verbatim and prints it in the info window.
// The CHP.EXE copy (chp_status_data.h) diverges from entry 93 on, so the two are not shared.
// Index 165 and above read unrelated data after the table in the original, so they return "".
// Extracted by searching COMBATII.EXE for "Knight\0" and reading 16-byte records.
static const int kCombatClassNameCount = 165;
static const char* const kCombatClassNames[165] = {
    "Knight", "Paladin", "Death lord", "Dragon knight", "King", "Dragon slayer", "Fighter",
    "Assassin", "Gradiater", "Valkyrie", "Thief", "Ranger", "Scout", "Archeaogist", "Soldier",
    "Lancer", "General", "Dark General", "Guard", "Royal Guard ", "Earthquaker", "Archer", "Hunter",
    "Eagle", "Sniper", "Mage", "Magician", "Alchemist", "Fire Master", "Ice Master",
    "Thunder Master", "Wizard", "Dark Wizard", "Monk", "Cleric", "Priest", "Druid", "Bishop",
    "Warlock", "Necromancer", "Lich", "Sorcerer", "Sharman", "Exorcist", "Warrior", "Berserker",
    "Genoicider", "Tusa", "Martial Arts", "Musa", "Sword Master", "Tamer", "Savant", "Killer",
    "Cleaner", "Shadow Killer", "Shadow Master", "Merman Soldier", "Merman Armor", "Merman Lord",
    "Valtan Soldier", "Asult Valtan", "Valtan Lord", "Goblin", "Goblin Lord", "Orc", "Orc Lord",
    "Ogre", "Ogre Lord", "Skeleton", "Dark Skeleton", "DTWarrior", "Slime", "Dark Slime", "Ghoul",
    "Zombi", "Bat", "Bamp", "Vampire", "Vampire Lord", "Scorpion", "Wyvern", "Gargoyle",
    "Fire Golem", "Ice Golem", "Thunder Golem", "Dark Golem", "Dao", "Djinni", "Marid", "Salamanda",
    "Kraken", "Fire Dragon", "Dragon Hand", "Harpy", "Grima", "Brown Horse", "White Horse",
    "Green Horse", "Black Horse", "Blue Drake", "Dark Drake", "Cannon M. A.", "I. Cannon M. A.",
    "Arrow M. A.", "I. Arrow M. A.", "Shaft M. A. ", "I. Shaft M.A.", "Aero M.A.", "I. Aero M.A.",
    "Abarest", "Arondight", "Asura", "Dun Grnid", "ElGenaro", "Garius", "Ra", "Yanus", "Carrier",
    "Abarest", "Arondight", "Asura", "Dun Grnid", "ElGenaro", "Garius", "Ra", "Yanus", "Blue Drake",
    "Dark Drake", "F.Arondight", "F.Asura", "F.Dun Grnid", "F.Ra", "Asmodeus", "Infantry",
    "I. Infantry", "Cavalry", "I. Cavalry", "Spear", "I. Spear", "Bow", "I. Bow", "Magic",
    "I. Magic", "Canon", "I. Canon", "Galley Ship", "I. Galley", "Sailer", "I. Sailer",
    "Ironclad Ship", "BattleShip", "I. BattleShip", "Air Fighter", "I. Air Fighter", "Air Advanced",
    "I. Air Advanced", "Air Battler", "I, Air Battler", "Air Pirate", "Ace Pirate", "Canon", "Gate",
    "Oddy Guardian", "God",
};

inline const char* combat_class_name(int cls) {
    return (cls >= 0 && cls < kCombatClassNameCount) ? kCombatClassNames[cls] : "";
}
