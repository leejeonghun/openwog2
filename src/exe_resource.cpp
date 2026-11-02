#include "exe_resource.h"
#include "fileutil.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef GAME_DATA_DIR
#define GAME_DATA_DIR "./GENESIS2"
#endif

namespace {

uint32_t rd32(const std::vector<uint8_t>& d, size_t off) {
    return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
           ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
}

struct LeObject {
    uint32_t vsize = 0;          // virtual size
    uint32_t base = 0;           // RelocBase (VA)
    uint32_t page_map_index = 0; // 1-based page map index
    uint32_t page_count = 0;
};

// Loads one LE executable into memory and converts VA -> file offset.
// A page map entry is 4 bytes: [24-bit page number (big-endian)][flags], and the page
// number is 1-based. Data page offsets (LE+0x80) are relative to the file start.
class LeImage {
public:
    bool load(const std::string& path) {
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) {
            fprintf(stderr, "EXE: warning: cannot open %s\n", path.c_str());
            return false;
        }
        if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
        const long size = ftell(fp);
        if (size <= 0) { fclose(fp); return false; }
        rewind(fp);
        data_.resize((size_t)size);
        const size_t got = fread(data_.data(), 1, data_.size(), fp);
        fclose(fp);
        data_.resize(got);
        if (data_.size() < 0x40) return false;
        return parse();
    }

    size_t size() const { return data_.size(); }

    bool read(uint32_t va, size_t n, std::vector<uint8_t>& out) const {
        out.clear();
        if (n == 0) return false;
        out.resize(n);
        if (!read_raw(va, n, out.data())) {
            out.clear();
            return false;
        }
        return true;
    }

    // sub_1D874 / sub_2B240 RLE: u16 chunk count followed by
    // [u16 skip][u8 dword_count][u8 byte_count] + (dword_count*4+byte_count) bytes. Walk the
    // chunk headers to find the exact length.
    bool read_rle(uint32_t va, std::vector<uint8_t>& out) const {
        out.clear();
        uint8_t head[2];
        if (!read_raw(va, sizeof(head), head)) return false;
        const uint32_t chunks = (uint32_t)head[0] | ((uint32_t)head[1] << 8);
        size_t total = 2;
        for (uint32_t i = 0; i < chunks; ++i) {
            if (total > 4u * 1024u * 1024u) return false;   // guard against a corrupt chunk count
            uint8_t e[4];
            if (!read_raw(va + (uint32_t)total, sizeof(e), e)) return false;
            total += 4 + (size_t)e[2] * 4 + e[3];
        }
        out.resize(total);
        if (!read_raw(va, total, out.data())) {
            out.clear();
            return false;
        }
        return true;
    }

    // sub_2B2A1 family transparent sprite: u16 width + u16 height + width*height pixels.
    bool read_sprite(uint32_t va, std::vector<uint8_t>& out) const {
        out.clear();
        uint8_t head[4];
        if (!read_raw(va, sizeof(head), head)) return false;
        const uint32_t w = (uint32_t)head[0] | ((uint32_t)head[1] << 8);
        const uint32_t h = (uint32_t)head[2] | ((uint32_t)head[3] << 8);
        if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
        const size_t total = 4 + (size_t)w * h;
        out.resize(total);
        if (!read_raw(va, total, out.data())) {
            out.clear();
            return false;
        }
        return true;
    }

private:
    bool parse() {
        size_t le = rd32(data_, 0x3C);
        auto is_le = [this](size_t off) {
            return off + 4 <= data_.size() && data_[off] == 'L' &&
                   (data_[off + 1] == 'E' || data_[off + 1] == 'X');
        };
        if (!is_le(le)) {
            // Fallback for bound executables whose e_lfanew does not point at LE: search
            // for the "LE\0\0" signature.
            le = data_.size();
            for (size_t i = 0; i + 4 <= data_.size(); ++i) {
                if (data_[i] == 'L' && data_[i + 1] == 'E' && data_[i + 2] == 0 && data_[i + 3] == 0) {
                    le = i;
                    break;
                }
            }
            if (!is_le(le)) return false;
        }
        if (le + 0x84 > data_.size()) return false;

        page_size_ = rd32(data_, le + 0x28);
        const size_t obj_table = le + rd32(data_, le + 0x40);
        const uint32_t obj_count = rd32(data_, le + 0x44);
        page_map_offset_ = le + rd32(data_, le + 0x48);
        data_pages_offset_ = rd32(data_, le + 0x80);

        if (page_size_ == 0 || obj_count == 0 || obj_count > 64) return false;
        if (obj_table + (size_t)obj_count * 24 > data_.size()) return false;

        objects_.reserve(obj_count);
        for (uint32_t i = 0; i < obj_count; ++i) {
            const size_t e = obj_table + (size_t)i * 24;
            LeObject o;
            o.vsize = rd32(data_, e + 0);
            o.base = rd32(data_, e + 4);
            o.page_map_index = rd32(data_, e + 12);
            o.page_count = rd32(data_, e + 16);
            if (o.vsize == 0 || o.page_map_index == 0) return false;
            objects_.push_back(o);
        }
        return true;
    }

    const LeObject* find_object(uint32_t va, size_t n) const {
        for (const LeObject& o : objects_) {
            if (va < o.base) continue;
            const uint64_t off = (uint64_t)va - o.base;
            if (off >= o.vsize) continue;
            if (off + n > o.vsize) return nullptr;   // reject requests that cross the object end
            return &o;
        }
        return nullptr;
    }

    bool read_raw(uint32_t va, size_t n, uint8_t* dst) const {
        const LeObject* obj = find_object(va, n);
        if (!obj) return false;

        size_t done = 0;
        while (done < n) {
            const uint64_t rel = (uint64_t)va - obj->base + done;
            const uint64_t page_index = rel / page_size_;
            if (page_index >= obj->page_count) return false;   // BSS (not present in the file)

            const size_t entry = page_map_offset_ +
                                 ((size_t)obj->page_map_index - 1 + (size_t)page_index) * 4;
            if (entry + 4 > data_.size()) return false;
            const uint32_t page_no = ((uint32_t)data_[entry] << 16) |
                                     ((uint32_t)data_[entry + 1] << 8) |
                                     (uint32_t)data_[entry + 2];
            if (page_no == 0) return false;

            const size_t in_page = (size_t)(rel % page_size_);
            const size_t file_off = (size_t)data_pages_offset_ +
                                    ((size_t)page_no - 1) * page_size_ + in_page;
            const size_t take = std::min(n - done, (size_t)page_size_ - in_page);
            if (file_off + take > data_.size()) return false;

            memcpy(dst + done, data_.data() + file_off, take);
            done += take;
        }
        return true;
    }

    std::vector<uint8_t> data_;
    uint32_t page_size_ = 0;
    uint32_t data_pages_offset_ = 0;
    size_t page_map_offset_ = 0;
    std::vector<LeObject> objects_;
};

struct ExeCache {
    bool tried = false;
    std::unique_ptr<LeImage> image;
};

LeImage* get_image(ExeId exe) {
    static ExeCache cache[3];
    const int idx = (int)exe;
    if (idx < 0 || idx > 2) return nullptr;

    ExeCache& c = cache[idx];
    if (!c.tried) {
        c.tried = true;
        const char* name = exe_resource_file_name(exe);
        const std::string path = file_find_case_insensitive(GAME_DATA_DIR, name);
        auto image = std::make_unique<LeImage>();
        if (image->load(path)) {
            fprintf(stderr, "EXE: reading resources directly from %s (%zu bytes)\n",
                    path.c_str(), image->size());
            c.image = std::move(image);
        }
    }
    return c.image.get();
}

}  // namespace

const char* exe_resource_file_name(ExeId exe) {
    switch (exe) {
        case ExeId::Pack: return "PACK.EXE";
        case ExeId::Chp:  return "CHP.EXE";
        case ExeId::Ttl:  return "TTL.EXE";
    }
    return "?";
}

bool exe_resource_load(ExeId exe, uint32_t va, ExeResLength len,
                       std::vector<uint8_t>& out, size_t fixed_size) {
    out.clear();
    LeImage* image = get_image(exe);
    if (!image) return false;

    bool ok = false;
    switch (len) {
        case ExeResLength::Fixed:
            if (fixed_size > 0) ok = image->read(va, fixed_size, out);
            break;
        case ExeResLength::Rle:
            ok = image->read_rle(va, out);
            break;
        case ExeResLength::Sprite:
            ok = image->read_sprite(va, out);
            break;
    }
    if (!ok) {
        fprintf(stderr, "EXE: warning: could not read %s VA 0x%X resource\n",
                exe_resource_file_name(exe), (unsigned)va);
    }
    return ok;
}
