#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

namespace ck {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_path;
LogLevel g_level = LogLevel::Info;
bool g_keep_closed = false;

const char* level_tag(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info: return "INF";
    case LogLevel::Warn: return "WRN";
    case LogLevel::Error: return "ERR";
  }
  return "???";
}

// Keep the log from growing without bound on a device nobody ever cleans up.
constexpr long kMaxLogBytes = 512 * 1024;

void rotate_if_needed() {
  if (!g_file || g_path.empty()) return;
  long pos = ftell(g_file);
  if (pos < kMaxLogBytes) return;
  fclose(g_file);
  std::string old = g_path + ".1";
  ::remove(old.c_str());
  ::rename(g_path.c_str(), old.c_str());
  g_file = fopen(g_path.c_str(), "ae");
}

}  // namespace

void log_init(const std::string& file_path, LogLevel level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_level = level;
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
  g_path = file_path;
  if (!g_path.empty()) {
    g_file = fopen(g_path.c_str(), "ae");
  }
}

void log_set_level(LogLevel level) { g_level = level; }

void log_keep_closed(bool keep_closed) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_keep_closed = keep_closed;
  if (keep_closed && g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
}
LogLevel log_level() { return g_level; }

void log_write(LogLevel level, const char* fmt, ...) {
  if (level < g_level) return;

  char body[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  char stamp[32];
  time_t now = time(nullptr);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);
  strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_buf);

  std::lock_guard<std::mutex> lock(g_mutex);
  fprintf(stderr, "[%s] %s %s\n", stamp, level_tag(level), body);
  if (g_keep_closed && !g_path.empty()) {
    FILE* f = fopen(g_path.c_str(), "ae");
    if (f) {
      fprintf(f, "[%s] %s %s\n", stamp, level_tag(level), body);
      long size = ftell(f);
      fclose(f);
      if (size >= kMaxLogBytes) {
        std::string old = g_path + ".1";
        ::remove(old.c_str());
        ::rename(g_path.c_str(), old.c_str());
      }
    }
    return;
  }
  if (g_file) {
    fprintf(g_file, "[%s] %s %s\n", stamp, level_tag(level), body);
    fflush(g_file);
    rotate_if_needed();
  }
}

}  // namespace ck
