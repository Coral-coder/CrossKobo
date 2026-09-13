#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ck {

// Read-only ZIP access over miniz. EPUBs are ZIPs, and so are the .cbz
// comic archives the library can open.
class ZipReader {
 public:
  ZipReader() = default;
  ~ZipReader();
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool open(const std::string& path);
  void close();
  bool is_open() const { return handle_ != nullptr; }

  const std::vector<std::string>& entries() const { return entries_; }
  bool has(const std::string& name) const;
  // Case-insensitive lookup, because some books disagree with themselves
  // about the case of their own hrefs.
  std::string resolve(const std::string& name) const;
  bool read(const std::string& name, std::string& out) const;
  uint64_t entry_size(const std::string& name) const;

 private:
  void* handle_ = nullptr;          // mz_zip_archive*
  std::string path_;
  std::vector<std::string> entries_;
};

}  // namespace ck
