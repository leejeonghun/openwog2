#pragma once
#include <cstdint>
#include <string>

// File existence check
bool file_exists(const char* filename);

// Find file path resolving uppercase/lowercase differences
std::string file_find_case_insensitive(const std::string& base_dir, const std::string& filename);

// File copy (equivalent to sub_123E3)
bool file_copy(const char* src, const char* dst);

// File deletion
bool file_delete(const char* filename);

// Create empty file (flag file)
bool file_create_empty(const char* filename);

// Get file size
long file_get_size(const char* filename);

// Get file timestamp (for save slot display)
bool file_get_timestamp(const char* filename, int& year, int& month, int& day, int& hour, int& min);
// Create the 2-byte flag file the original wrote (creat + write of the DOS file handle number).
// The handle value itself is meaningless to the launcher (it only checks that the file exists),
// so a fixed value is written instead of a platform-specific fileno().
bool file_create_handle_flag(const char* filename);
