#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ck {

// ------------------------------------------------------------------ strings
std::string trim(const std::string& s);
std::string to_lower(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Case-insensitive "does haystack contain needle", used by library search.
bool icontains(const std::string& haystack, const std::string& needle);

// --------------------------------------------------------------------- utf8
// Decodes the code point starting at `i`, advancing `i` past it. Invalid
// bytes decode to U+FFFD and advance by one so a malformed book can never
// wedge the layout engine in an infinite loop.
uint32_t utf8_next(const std::string& s, size_t& i);
size_t utf8_length(const std::string& s);
void utf8_append(std::string& out, uint32_t cp);
std::vector<uint32_t> utf8_decode(const std::string& s);
// Byte index of the start of the code point that precedes byte index `i`.
size_t utf8_prev_index(const std::string& s, size_t i);

// ----------------------------------------------------------------- entities
// Expands XML/HTML character references (&amp; &#x2014; &nbsp; ...).
std::string decode_entities(const std::string& s);

// ------------------------------------------------------------------ numbers
int to_int(const std::string& s, int fallback = 0);
double to_double(const std::string& s, double fallback = 0.0);
std::string human_size(uint64_t bytes);
// "1 h 05 m" style duration used by the reading statistics screens.
std::string human_duration(int64_t seconds);

}  // namespace ck
