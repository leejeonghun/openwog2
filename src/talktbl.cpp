#include "talktbl.h"
#include <cstdio>
#include <cstring>

// sub_24D5C: (al & 0xFF) >> 7 == 1, i.e. a byte >= 0x80 is the lead byte of a
// 2-byte Hangul character.
static inline bool talktbl_is_lead_byte(uint8_t c) {
    return (c >> 7) == 1;
}

// sub_24D70: walks buf[0, len) byte by byte (1 byte for ASCII, 2 for Hangul) and
// reports whether len lands in the middle of a 2-byte character.
static bool talktbl_splits_hangul(int len, const uint8_t* buf) {
    int pos = 0;
    while (pos < len) {
        if (talktbl_is_lead_byte(buf[pos])) pos += 2;
        else pos += 1;
    }
    return pos == len + 1;
}

// sub_25DD3: fills the conversion buffer (unk_196C14, 0xA28 bytes, zeroed) and
// returns the length in ebx (bytes written, including the '}' overwritten with NUL).
bool talktbl_load_buffer(const std::string& path, int talk_id, std::vector<uint8_t>& out_buf, int& out_len) {
    out_buf.clear();
    out_len = 0;
    if (talk_id < 0) return false;

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "TALK: warning: cannot open %s\n", path.c_str());
        return false;
    }

    // ---- 1) skip the preceding talk_id records up to "}}" (loc_25E1E..loc_25E52) ----
    // var_4 (the previously read character) is zeroed after fopen and then kept
    // across record boundaries - the original never resets it between blocks.
    int prev_char = 0;
    int block_count = 0;
    while (block_count < talk_id) {
        int c;
        do {
            c = fgetc(fp);
            if (c == EOF) { fclose(fp); return false; }
        } while (c != '{');

        for (;;) {
            c = fgetc(fp);
            if (c == EOF) { fclose(fp); return false; }
            if (prev_char == '}' && c == '}') break; // "}}" ends the record
            prev_char = c;
        }
        block_count++;
    }

    // ---- 2) find the opening '{' of the target record (loc_25E52) ----
    {
        int c;
        do {
            c = fgetc(fp);
            if (c == EOF) { fclose(fp); return false; }
        } while (c != '{');
    }

    // ---- 3) convert and copy the target record body (loc_25E5E..loc_25F07) ----
    static constexpr int BUF_SIZE = 0xA28; // unk_196C14 size (2600 bytes, STOSB)
    std::vector<uint8_t> buf(BUF_SIZE, 0);
    int wpos = 0; // ebx: next write position (= bytes written so far)
    int c;        // ecx/edi: character being processed

read_next_char:
    c = fgetc(fp);
    if (c == EOF) { fclose(fp); return false; }

    // The original opens the file in text mode ("rt"), where the DOS CRT strips
    // '\r' from CRLF at the stream level. The port opens it "rb", so filter here.
    if (c == '\r') goto read_next_char;

    // End test: the last byte written is '}' and the character just read is '}'.
    if (wpos > 0 && buf[wpos - 1] == '}' && c == '}') {
        buf[wpos - 1] = 0; // overwrite the final '}' with NUL to terminate the string
        goto done;
    }

    if (c == '\n') {
        int nc;
        do {
            nc = fgetc(fp);
            if (nc == EOF) { fclose(fp); return false; }
            if (nc == '\r') continue; // strip '\r' as text mode would
        } while (nc == ' ' || nc == '\t' || nc == '\r');   // loc_25E80: only spaces/tabs are skipped (a second '\n' of a blank line survives)
        // loc_25E80: write a single space, then continue with the next character.
        if (wpos < BUF_SIZE) buf[wpos] = ' ';
        wpos++;
        c = nc;
        // The original continues at loc_25E9C without re-testing for the terminator,
        // so a '}' that is the second byte of "}}" is first stored as a normal byte
        // and the end is detected on the next pass. after_newline re-filters '\r'.
        goto after_newline;
    }

    goto after_newline;

after_newline:
    // A '\n' read right after a newline (blank line) is stored as 0x0A; only '\r'
    // was removed by text mode in the original.
    if (c == '\r') goto read_next_char;

    if (c == '#') {
        static const int kLineBoundaries[4] = {0x19, 0x33, 0x4D, 0x67}; // 25,51,77,103
        for (int b : kLineBoundaries) {
            if (wpos <= b) { wpos = b; break; }
        }
        // Only wpos moves; no byte is written, which matches the original's zeroed
        // buffer (the gap is effectively NUL padding).
    }

    // Keep a 2-byte Hangul character from being split at a line end (sub_24D5C/sub_24D70)
    for (int edi = 0x19; edi <= wpos; edi += 0x1A) {
        if (wpos != edi) continue;
        if (edi == 0) continue;
        if (!talktbl_is_lead_byte((uint8_t)c)) continue;
        if (talktbl_splits_hangul(edi, buf.data())) continue;
        if (wpos < BUF_SIZE) buf[wpos] = ' ';
        wpos = edi + 1;
        break;
    }

    if (c == '\t') goto read_next_char; // tabs are dropped

    if (wpos < BUF_SIZE) buf[wpos] = (uint8_t)c;
    wpos++;
    goto read_next_char;

done:
    fclose(fp);
    out_buf = buf;
    out_len = wpos;
    return true;
}

bool talktbl_load_raw(const std::string& path, int talk_id, std::string& out_text) {
    out_text.clear();
    std::vector<uint8_t> buf;
    int len = 0;
    if (!talktbl_load_buffer(path, talk_id, buf, len)) return false;
    out_text.assign(reinterpret_cast<const char*>(buf.data()), (size_t)(len > 0 ? len - 1 : 0));
    return true;
}

void talktbl_split_choices(const std::string& text, std::string& out_body,
                           std::vector<std::string>& out_choices) {
    out_choices.clear();

    size_t at_pos = text.find('@');
    if (at_pos == std::string::npos) {
        out_body = text;
        return;
    }

    out_body = text.substr(0, at_pos);

    // sub_24FD5: skip from just after '@' to the next '[' and collect up to ']',
    // repeated at most four times.
    size_t pos = at_pos + 1;
    for (int i = 0; i < 4; i++) {
        size_t lb = text.find('[', pos);
        if (lb == std::string::npos) break;
        size_t rb = text.find(']', lb + 1);
        if (rb == std::string::npos) break;
        out_choices.push_back(text.substr(lb + 1, rb - lb - 1));
        pos = rb + 1;
    }
}

std::vector<std::string> talktbl_paginate(const std::string& body, int line_width, int lines_per_page) {
    std::vector<std::string> lines;
    for (size_t i = 0; i < body.size(); i += (size_t)line_width) {
        size_t len = std::min((size_t)line_width, body.size() - i);
        lines.push_back(body.substr(i, len));
    }

    std::vector<std::string> pages;
    for (size_t i = 0; i < lines.size(); i += (size_t)lines_per_page) {
        std::string page;
        size_t end = std::min(i + (size_t)lines_per_page, lines.size());
        for (size_t j = i; j < end; j++) {
            if (j > i) page += '\n';
            page += lines[j];
        }
        pages.push_back(page);
    }
    if (pages.empty()) pages.push_back(""); // always return at least one page
    return pages;
}

std::vector<TalkPage> talktbl_paginate_npc(const std::string& body, int line_width) {
    std::vector<std::string> raw_lines;
    for (size_t i = 0; i < body.size(); i += (size_t)line_width) {
        size_t len = std::min((size_t)line_width, body.size() - i);
        std::string s = body.substr(i, len);
        // sub_25F21 loc_266A5: a NUL inside a 26-byte line ends that line, which
        // handles the zero padding left by the '#' handling.
        size_t nul_pos = s.find('\0');
        if (nul_pos != std::string::npos) {
            s.resize(nul_pos);
        }
        raw_lines.push_back(s);
    }

    std::vector<TalkPage> pages;
    size_t line_idx = 0;

    // Page 1: at most 3 lines (row 0 holds the NPC name, so the body starts at row 1)
    {
        TalkPage p;
        for (int j = 0; j < 3 && line_idx < raw_lines.size(); j++, line_idx++) {
            p.lines.push_back(raw_lines[line_idx]);
        }
        pages.push_back(p); // page 1 always exists, even for a name with no body
    }

    // Pages 2 and later: up to 4 lines each
    while (line_idx < raw_lines.size()) {
        TalkPage p;
        for (int j = 0; j < 4 && line_idx < raw_lines.size(); j++, line_idx++) {
            p.lines.push_back(raw_lines[line_idx]);
        }
        pages.push_back(p);
    }

    return pages;
}
