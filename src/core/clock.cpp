#include "core/clock.h"

#include <ctime>
#include <errno.h>

#include "core/str.h"

namespace ck {

int64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t wall_seconds() { return (int64_t)time(nullptr); }

void sleep_ms(int ms) {
  if (ms <= 0) return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

std::string format_time(int64_t unix_seconds, bool twenty_four_hour) {
  time_t t = (time_t)unix_seconds;
  struct tm tm_buf;
  localtime_r(&t, &tm_buf);
  char buf[32];
  strftime(buf, sizeof(buf), twenty_four_hour ? "%H:%M" : "%l:%M %p", &tm_buf);
  return trim(buf);
}

std::string format_date(int64_t unix_seconds) {
  time_t t = (time_t)unix_seconds;
  struct tm tm_buf;
  localtime_r(&t, &tm_buf);
  char buf[32];
  strftime(buf, sizeof(buf), "%d %b %Y", &tm_buf);
  return buf;
}

std::string format_datetime(int64_t unix_seconds, bool twenty_four_hour) {
  return format_date(unix_seconds) + " " + format_time(unix_seconds, twenty_four_hour);
}

std::string relative_time(int64_t unix_seconds) {
  if (unix_seconds <= 0) return "never";
  int64_t delta = wall_seconds() - unix_seconds;
  if (delta < 0) delta = 0;
  if (delta < 60) return "just now";
  if (delta < 3600) return format("%lld min ago", (long long)(delta / 60));
  if (delta < 86400) return format("%lld h ago", (long long)(delta / 3600));
  if (delta < 7 * 86400) {
    int64_t days = delta / 86400;
    return days == 1 ? "yesterday" : format("%lld days ago", (long long)days);
  }
  return format_date(unix_seconds);
}

}  // namespace ck
