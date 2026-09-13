#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ck {
namespace fs {

struct Entry {
  std::string name;   // basename
  std::string path;   // full path
  bool is_dir = false;
  uint64_t size = 0;
  int64_t mtime = 0;
};

bool exists(const std::string& path);
bool is_dir(const std::string& path);
uint64_t file_size(const std::string& path);
int64_t mtime(const std::string& path);

bool read_file(const std::string& path, std::string& out);
// Writes via a temporary file + rename so a power loss mid-write cannot
// truncate settings, reading state or a notebook.
bool write_file_atomic(const std::string& path, const std::string& data);
bool append_file(const std::string& path, const std::string& data);
bool mkdir_p(const std::string& path);
bool remove_file(const std::string& path);
bool remove_tree(const std::string& path);
bool rename(const std::string& from, const std::string& to);
bool copy_file(const std::string& from, const std::string& to);

std::vector<Entry> list_dir(const std::string& path, bool include_hidden = false);

std::string dirname(const std::string& path);
std::string basename(const std::string& path);
std::string stem(const std::string& path);          // basename without extension
std::string extension(const std::string& path);      // lowercase, no dot
std::string join_path(const std::string& a, const std::string& b);
// Resolves ".." / "." and collapses slashes; used for EPUB-internal hrefs.
std::string normalize(const std::string& path);
std::string sanitize_filename(const std::string& name);

uint64_t free_space_bytes(const std::string& path);

}  // namespace fs
}  // namespace ck
