#pragma once
#include <cstdint>
#include <string>

namespace ck {

// Clock synchronisation. CrossKobo replaces the software that normally sets
// the clock, so without this the time in the status bar drifts with the
// RTC and reading statistics land on the wrong day. SNTP over UDP is a
// 48-byte request and a 48-byte answer, which is why it is worth doing
// ourselves rather than shelling out.

// Decodes a 48-byte SNTP answer into a Unix time, or 0 with a reason. Split
// out from the query so the byte arithmetic can be tested without a
// network.
int64_t sntp_decode(const unsigned char reply[48], std::string& error);

// Asks a time server what the time is. Returns the Unix time, or 0 on
// failure with the reason in `error`.
int64_t sntp_query(const std::string& host, int timeout_ms, std::string& error);

// Queries a server and sets the system clock. Needs root, which CrossKobo
// has on the device. `drift_seconds` reports how far off the clock was.
bool sync_clock(std::string& error, int64_t* drift_seconds = nullptr);

}  // namespace ck
