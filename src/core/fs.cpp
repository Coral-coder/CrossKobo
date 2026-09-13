#include "core/fs.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/log.h"
#include "core/str.h"

namespace ck {
namespace fs {

bool exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t file_size(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return (uint64_t)st.st_size;
}

int64_t mtime(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return (int64_t)st.st_mtime;
}

bool read_file(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rbe");
  if (!f) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  bool ok = ferror(f) == 0;
  fclose(f);
  return ok;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
  mkdir_p(dirname(path));
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wbe");
  if (!f) {
    CK_LOGE("write %s: %s", tmp.c_str(), strerror(errno));
    return false;
  }
  bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
  if (ok) ok = fflush(f) == 0;
  if (ok) ok = fsync(fileno(f)) == 0;
  fclose(f);
  if (!ok) {
    ::remove(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    CK_LOGE("rename %s: %s", path.c_str(), strerror(errno));
    ::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool append_file(const std::string& path, const std::string& data) {
  mkdir_p(dirname(path));
  FILE* f = fopen(path.c_str(), "abe");
  if (!f) return false;
  bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

bool mkdir_p(const std::string& path) {
  if (path.empty() || path == "/" || path == ".") return true;
  if (is_dir(path)) return true;
  std::string parent = dirname(path);
  if (!parent.empty() && parent != path) mkdir_p(parent);
  if (::mkdir(path.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

bool remove_file(const std::string& path) { return ::remove(path.c_str()) == 0; }

bool remove_tree(const std::string& path) {
  if (!is_dir(path)) return remove_file(path);
  for (const Entry& e : list_dir(path, true)) {
    if (e.is_dir) {
      remove_tree(e.path);
    } else {
      remove_file(e.path);
    }
  }
  return ::rmdir(path.c_str()) == 0;
}

bool rename(const std::string& from, const std::string& to) {
  mkdir_p(dirname(to));
  if (::rename(from.c_str(), to.c_str()) == 0) return true;
  // Different filesystems (onboard vs. sd card) need a copy + unlink.
  if (errno == EXDEV && copy_file(from, to)) return remove_file(from);
  return false;
}

bool copy_file(const std::string& from, const std::string& to) {
  std::string data;
  if (!read_file(from, data)) return false;
  return write_file_atomic(to, data);
}

std::vector<Entry> list_dir(const std::string& path, bool include_hidden) {
  std::vector<Entry> out;
  DIR* d = opendir(path.c_str());
  if (!d) return out;
  struct dirent* de;
  while ((de = readdir(d)) != nullptr) {
    std::string name = de->d_name;
    if (name == "." || name == "..") continue;
    if (!include_hidden && !name.empty() && name[0] == '.') continue;
    Entry e;
    e.name = name;
    e.path = join_path(path, name);
    struct stat st;
    if (::stat(e.path.c_str(), &st) == 0) {
      e.is_dir = S_ISDIR(st.st_mode);
      e.size = (uint64_t)st.st_size;
      e.mtime = (int64_t)st.st_mtime;
    } else {
      e.is_dir = de->d_type == DT_DIR;
    }
    out.push_back(std::move(e));
  }
  closedir(d);
  return out;
}

std::string dirname(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string basename(const std::string& path) {
  size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string stem(const std::string& path) {
  std::string base = basename(path);
  size_t dot = base.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
}

std::string extension(const std::string& path) {
  std::string base = basename(path);
  size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= base.size()) return "";
  return to_lower(base.substr(dot + 1));
}

std::string join_path(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (!b.empty() && b[0] == '/') return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string normalize(const std::string& path) {
  bool absolute = !path.empty() && path[0] == '/';
  std::vector<std::string> parts;
  for (const std::string& part : split(path, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (!absolute) {
        parts.push_back("..");
      }
      continue;
    }
    parts.push_back(part);
  }
  std::string out = join(parts, "/");
  return absolute ? "/" + out : out;
}

std::string sanitize_filename(const std::string& name) {
  std::string out;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|' || (unsigned char)c < 0x20) {
      out += '_';
    } else {
      out += c;
    }
  }
  out = trim(out);
  if (out.empty()) out = "untitled";
  if (out.size() > 120) out.resize(120);
  return out;
}

uint64_t free_space_bytes(const std::string& path) {
  struct statvfs st;
  if (::statvfs(path.c_str(), &st) != 0) return 0;
  return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
}

}  // namespace fs
}  // namespace ck
