// combatii_equip_data.h: data for the ally equipment screen (sub_48E90), extracted from
// COMBATII.EXE.
// Strings are Johab byte sequences.
#pragma once
#include <cstdint>

// VA 0x76F37: "You cannot equip this here."
static const char kEquipMsgNoSlot[] = "\xb5\x61\x8b\xa1\xb5\x41\x20\xb8\x77\xc0\x62\xd0\x69\x20\xae\x81\x20\xb4\xf4\xaf\x73\x93\xa1\x94\x61\x2e";

// VA 0x76F52: "This class cannot equip weapons."
static const char kEquipMsgNoWeapon[] = "\xa2\x81\x8b\xa1\x9f\x69\x20\xb8\x77\xc0\x62\xd0\x69\x20\xae\x81\x20\xb4\xf4\x93\x65\x20\xbb\xa2\xb4\xf3\xb7\xb3\x93\xa1\x94\x61\x2e\x0a";

// VA 0x76F75: "This weapon does not match your class."
static const char kEquipMsgWrongClass[] = "\xbb\xa2\xb4\xf3\xb5\x41\x20\xa0\x78\xbb\xa1\xb4\x67\x93\x65\x20\xa2\x81\x8b\xa1\xb7\xb3\x93\xa1\x94\x61\x2e\x0a";

// VA 0x76F92: "You cannot hold a two-handed weapon now."
static const char kEquipMsgTwoHand[] = "\xb4\xb7\xad\xa5\xa2\x81\x8b\xa1\x9f\x69\x20\xba\xe9\xae\x81\x20\xb4\xf4\x93\x65\x20\xac\x77\xc8\x81\xb7\xb3\x93\xa1\x94\x61\x2e\x0a";

// VA 0x76FB4: "You are already holding a two-handed weapon."
static const char kEquipMsgHoldTwoHand[] = "\xb4\xb7\xad\xa5\x20\xa2\x81\x8b\xa1\x9f\x69\x20\xba\xe1\x89\xa1\x20\xb7\xb6\x93\x65\x20\xac\x77\xc8\x81\xb7\xb3\x93\xa1\x94\x61\x2e\x0a";

// VA 0x76FD7: "This class cannot use shields."
static const char kEquipMsgNoShield[] = "\xa4\x77\xcc\x81\x20\xac\x61\xb6\x77\xa6\x89\x88\x61\xb7\xa5\x20\xbb\xa2\xb4\xf3\xb7\xb3\x93\xa1\x94\x61\x2e\x0a";

// VA 0x76FF4: "This class cannot use evasion cloaks."
static const char kEquipMsgNoCloak[] = "\xd2\x41\xcf\xa1\xb6\x77\x20\xa0\x77\xc9\xa1\x20\xac\x61\xb6\x77\xa6\x89\x88\x61\xb7\xa5\x20\xbb\xa2\xb4\xf3\xb7\xb3\x93\xa1\x94\x61\x2e\x0a";

// VA 0x77018: "This is not an accessory."
static const char kEquipMsgNotAccessory[] = "\xa5\xa1\xb9\xa1\x8a\x81\x88\x61\x20\xb4\x61\x93\xb3\x93\xa1\x94\x61\x2e\x0a";

// unk_74124[weapon class]: equippable item category, compared with [item+0].
// -1 = the class cannot hold weapons.
static const int8_t kEquipWeaponReq[16] = {-1, 1, 5, 3, 2, 4, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0};

// Word ptr dword_71B6C[class*8] (+0): [54h] = max(0x3C, [42h] + this value), applied at the end of
// sub_181A6 / sub_18262. Entries past the table proper belong to the adjacent data.
static const int16_t kEquipTpBase[256] = {
    70, 85, 90, 95, 85, 100, 40, 50, 55, 60, 20, 30, 40, 45, 50, 60,
    70, 80, 35, 50, 55, 20, 30, 40, 40, 20, 20, 25, 25, 25, 25, 30,
    50, 20, 20, 25, 30, 30, 35, 45, 55, 20, 25, 35, 50, 60, 75, 70,
    80, 30, 60, 65, 35, 25, 30, 35, 40, 55, 65, 75, 35, 45, 55, 50,
    50, 40, 60, 40, 40, 45, 55, 70, 30, 50, 45, 40, 20, 30, 45, 60,
    65, 80, 45, 45, 45, 45, 50, 20, 55, 65, 45, 40, 60, 60, 40, 100,
    30, 50, 30, 50, 40, 40, 30, 30, 40, 40, 20, 20, 70, 70, 70, 60,
    55, 65, 50, 70, 80, 60, 30, 70, 60, 55, 65, 50, 70, 80, 60, 40,
    40, 80, 75, 60, 80, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 100, 0, 770, 259, 769, 0, 3, 770, 259, 515, 0, 8, 1283,
    772, 260, 0, 12, 1284, 1285, 770, 10, 0, 1281, 517, 1027, 10, 0, 515, 1284,
    772, 2048, 0, 1284, 1281, 1029, 0, 0, 261, 1025, 1029, 0, 0, 261, 1026, 1283,
    0, 0, 261, 514, 1029, 0, 0, 261, 1281, 771, 0, 0, 1281, 773, 1025, 0,
    0, 1281, 1285, 515, 0, 0, 260, 1285, 257, 1280, 0, 1028, 1028, 770, 6, 0,
    1027, 772, 516, 5, 0, 0, 0, 515, 514, 771, 0, 0, 0, 0, 0, 0,
};
