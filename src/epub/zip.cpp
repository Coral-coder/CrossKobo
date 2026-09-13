#include "epub/zip.h"

#include "core/log.h"
#include "core/str.h"
#include "miniz.h"

namespace ck {

ZipReader::~ZipReader() { close(); }

bool ZipReader::open(const std::string& path) {
  close();
  auto* zip = new mz_zip_archive();
  memset(zip, 0, sizeof(*zip));
  if (!mz_zip_reader_init_file(zip, path.c_str(), 0)) {
    CK_LOGW("zip: cannot open %s", path.c_str());
    delete zip;
    return false;
  }
  handle_ = zip;
  path_ = path;
  mz_uint count = mz_zip_reader_get_num_files(zip);
  entries_.reserve(count);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st)) continue;
    if (st.m_is_directory) continue;
    entries_.push_back(st.m_filename);
  }
  return true;
}

void ZipReader::close() {
  if (handle_) {
    auto* zip = static_cast<mz_zip_archive*>(handle_);
    mz_zip_reader_end(zip);
    delete zip;
    handle_ = nullptr;
  }
  entries_.clear();
  path_.clear();
}

bool ZipReader::has(const std::string& name) const { return !resolve(name).empty(); }

std::string ZipReader::resolve(const std::string& name) const {
  for (const std::string& e : entries_) {
    if (e == name) return e;
  }
  std::string lower = to_lower(name);
  for (const std::string& e : entries_) {
    if (to_lower(e) == lower) return e;
  }
  // Last resort: match on the basename, which rescues books whose OPF
  // paths do not agree with the archive layout.
  size_t slash = name.find_last_of('/');
  std::string base = to_lower(slash == std::string::npos ? name : name.substr(slash + 1));
  if (base.empty()) return "";
  for (const std::string& e : entries_) {
    size_t es = e.find_last_of('/');
    if (to_lower(es == std::string::npos ? e : e.substr(es + 1)) == base) return e;
  }
  return "";
}

bool ZipReader::read(const std::string& name, std::string& out) const {
  if (!handle_) return false;
  std::string actual = resolve(name);
  if (actual.empty()) return false;
  auto* zip = static_cast<mz_zip_archive*>(handle_);
  size_t size = 0;
  void* data = mz_zip_reader_extract_file_to_heap(zip, actual.c_str(), &size, 0);
  if (!data) return false;
  out.assign((const char*)data, size);
  mz_free(data);
  return true;
}

uint64_t ZipReader::entry_size(const std::string& name) const {
  if (!handle_) return 0;
  std::string actual = resolve(name);
  if (actual.empty()) return 0;
  auto* zip = static_cast<mz_zip_archive*>(handle_);
  mz_uint32 index = 0;
  if (!mz_zip_reader_locate_file_v2(zip, actual.c_str(), nullptr, 0, &index)) return 0;
  mz_zip_archive_file_stat st;
  if (!mz_zip_reader_file_stat(zip, index, &st)) return 0;
  return (uint64_t)st.m_uncomp_size;
}

}  // namespace ck
