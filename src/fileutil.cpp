#include "fileutil.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <string>
#include <cctype>

namespace fs = std::filesystem;

bool file_exists(const char* filename) {
    if (!filename) return false;
    std::error_code ec;
    return fs::exists(filename, ec);
}

// ---- Case-insensitive path resolver that walks the directory tree ----
static fs::path resolve_path_case_recursive(const fs::path& target) {
    std::error_code ec;
    if (fs::exists(target, ec)) {
        return target;
    }

    fs::path current;
    auto it = target.begin();
    auto end = target.end();

    // Root directory/drive (e.g. "C:\", "/")
    if (target.has_root_name()) {
        current = *it++;
        if (it != end && target.has_root_directory() && (*it == "/" || *it == "\\")) {
            current /= *it++;
        }
    }
    else if (target.has_root_directory()) {
        current = *it++;
    }

    for (; it != end; ++it) {
        const auto& part = *it;
        if (part == "." || part == "..") {
            current = current.empty() ? part : (current / part);
            continue;
        }

        fs::path search_dir = current.empty() ? fs::path(".") : current;
        if (!fs::exists(search_dir, ec) || !fs::is_directory(search_dir, ec)) {
            current = current.empty() ? part : (current / part);
            continue;
        }

        std::string part_str = part.string();
        bool found = false;

        for (const auto& entry : fs::directory_iterator(search_dir, fs::directory_options::skip_permission_denied, ec)) {
            std::string entry_name = entry.path().filename().string();
            if (entry_name.length() == part_str.length() &&
                std::equal(entry_name.begin(), entry_name.end(), part_str.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                        std::tolower(static_cast<unsigned char>(b));
                    })) {
                current = current.empty() ? entry.path().filename() : (current / entry.path().filename());
                found = true;
                break;
            }
        }

        if (!found) {
            current = current.empty() ? part : (current / part);
        }
    }

    return current;
}

std::string file_find_case_insensitive(const std::string& base_dir, const std::string& filename) {
    fs::path full_path = base_dir.empty() ? fs::path(filename) : (fs::path(base_dir) / filename);
    std::error_code ec;
    if (fs::exists(full_path, ec)) return full_path.string();

    fs::path resolved = resolve_path_case_recursive(full_path);
    if (fs::exists(resolved, ec)) return resolved.string();

    // Fallback attempts
    std::string upper_name = filename;
    std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), ::toupper);
    fs::path upper_path = base_dir.empty() ? fs::path(upper_name) : (fs::path(base_dir) / upper_name);
    if (fs::exists(upper_path, ec)) return upper_path.string();

    std::string lower_name = filename;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    fs::path lower_path = base_dir.empty() ? fs::path(lower_name) : (fs::path(base_dir) / lower_name);
    if (fs::exists(lower_path, ec)) return lower_path.string();

    return full_path.string();
}

bool file_copy(const char* src, const char* dst) {
    if (!src || !dst) return false;
    std::error_code ec;
    return fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}

bool file_delete(const char* filename) {
    if (!filename) return false;
    std::error_code ec;
    return fs::remove(filename, ec);
}

bool file_create_empty(const char* filename) {
    if (!filename) return false;
    std::ofstream ofs(filename, std::ios::binary | std::ios::trunc);
    return ofs.is_open();
}

long file_get_size(const char* filename) {
    if (!filename) return -1;
    std::error_code ec;
    auto size = fs::file_size(filename, ec);
    if (ec) return -1;
    return static_cast<long>(size);
}

bool file_get_timestamp(const char* filename, int& year, int& month, int& day, int& hour, int& min) {
    if (!filename) return false;
    std::error_code ec;
    auto ftime = fs::last_write_time(filename, ec);
    if (ec) return false;

    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
    struct tm* t = std::localtime(&cftime);
    if (!t) return false;

    year = t->tm_year + 1900;
    month = t->tm_mon + 1;
    day = t->tm_mday;
    hour = t->tm_hour;
    min = t->tm_min;
    return true;
}

bool file_create_handle_flag(const char* filename) {
    if (!filename) return false;
    FILE* fp = std::fopen(filename, "wb");
    if (!fp) return false;
    const unsigned char handle[2] = { 5, 0 };   // first DOS handle after stdin/stdout/stderr/aux/prn
    std::fwrite(handle, 1, 2, fp);
    std::fclose(fp);
    return true;
}
