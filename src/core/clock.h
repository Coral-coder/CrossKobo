#pragma once
#include <cstdint>
#include <string>

namespace ck {

// Monotonic milliseconds since an arbitrary origin; safe for timeouts.
int64_t now_ms();
int64_t wall_seconds();
// Wall-clock minutes. Views that show a clock repaint only when this
// changes: an unconditional repaint on every tick means a colour page
// flashes the whole panel for nothing.
inline int64_t wall_minutes() { return wall_seconds() / 60; }
void sleep_ms(int ms);

std::string format_time(int64_t unix_seconds, bool twenty_four_hour);
std::string format_date(int64_t unix_seconds);
std::string format_datetime(int64_t unix_seconds, bool twenty_four_hour);
// "3 days ago" / "just now", used in the library and notebook lists.
std::string relative_time(int64_t unix_seconds);

}  // namespace ck
