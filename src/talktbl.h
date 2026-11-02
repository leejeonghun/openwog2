#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ==== TALK.TBL reader (PACK.ASM sub_25DD3) and choice splitter (sub_24FD5) ====
// CHP reads the same talk.tbl format, so this lives outside the Pack class.
//
// Record format: "{ ... }}" - a record ends only when two '}' bytes follow each
// other. The first '}' is later overwritten with NUL, so it also ends the text.
//
// To reach record N (0-based) the reader skips N records by scanning for '{' and
// then discarding bytes until "}}", finds the next '{' and converts the body into a
// 0xA28-byte buffer (unk_196C14, zero filled):
//   '\n' : skip the following spaces/tabs, write one space, then process the next
//          character directly (loc_25E80 -> loc_25E9C). The "}}" test is not repeated,
//          so a terminator right after a newline is first stored as a normal byte.
//   '#'  : move the write position to the next line boundary (25, 51, 77, 103 - the
//          last index of each 26-byte line), then store '#' like any other byte.
//   other: if the write position is exactly a line boundary, the byte is a Hangul
//          lead byte (>= 0x80) and the boundary does not fall inside a 2-byte
//          character, write a space there and push the character to the next line.
//   '\t' : dropped; every other byte is copied as is.
//
// Choices: the body carries '@' followed by "[choice1] [choice2] ..." (an empty slot
// is "[]"). sub_24FD5 skips from just after '@' to the next '[' and collects up to
// the matching ']', four times. '@' itself is never drawn; it only marks choice mode.

// Reads record talk_id (0-based), applying the sub_25DD3 conversion to out_text.
// Keeps the '@' marker and the "[..]" slots; line boundaries are space padded.
// Returns false when talk.tbl cannot be opened or the record does not exist.
bool talktbl_load_raw(const std::string& path, int talk_id, std::string& out_text);

// sub_25DD3 as written: returns the 0xA28-byte buffer plus the length (the position
// where the trailing '}' was overwritten with NUL).
bool talktbl_load_buffer(const std::string& path, int talk_id, std::vector<uint8_t>& out_buf, int& out_len);

// Splits text at '@' into the dialogue body and up to four "[..]" choices (sub_24FD5).
// Without '@' the body is the whole text and out_choices stays empty. Empty slots
// ("[]") become empty strings but still occupy an index.
void talktbl_split_choices(const std::string& text, std::string& out_body,
                           std::vector<std::string>& out_choices);

// Splits the already line-wrapped body into pages of lines_per_page lines of
// line_width bytes each (the last page may be shorter). Lines are joined with '\n',
// which Font::draw_string treats as a line break.
std::vector<std::string> talktbl_paginate(const std::string& body, int line_width = 26, int lines_per_page = 4);

// NPC dialogue paging (sub_25F21): the first page holds 3 lines because row 0 of the
// four-row window is taken by the NPC name; later pages hold 4 lines.
//   var_8 (1 = name not drawn yet) makes the first frame draw the name at
//   Y = text_base_y - 48 and jump to loc_267FC without advancing var_30, the body
//   cursor, so page 1 = name row + the first three 26-byte body lines.
//   Once var_8 is 0 every page re-enters at loc_26384 and fills all four rows.
struct TalkPage {
    std::vector<std::string> lines; // dialogue lines already split at 26-byte boundaries
};

std::vector<TalkPage> talktbl_paginate_npc(const std::string& body, int line_width = 26);
