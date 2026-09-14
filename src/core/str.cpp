#include "core/str.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ck {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (unsigned char)s[b] <= ' ') ++b;
  while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
  return s.substr(b, e - b);
}

std::string to_lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = (char)tolower((unsigned char)c);
  return out;
}

std::string to_upper(const std::string& s) {
  std::string out = s;
  for (char& ch : out) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
  }
  return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

bool icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  std::string h = to_lower(haystack), n = to_lower(needle);
  return h.find(n) != std::string::npos;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

std::string format(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < (int)sizeof(buf)) return std::string(buf, n < 0 ? 0 : n);

  std::string big((size_t)n + 1, '\0');
  va_start(ap, fmt);
  vsnprintf(&big[0], big.size(), fmt, ap);
  va_end(ap);
  big.resize((size_t)n);
  return big;
}

uint32_t utf8_next(const std::string& s, size_t& i) {
  if (i >= s.size()) return 0;
  unsigned char c = (unsigned char)s[i];
  auto cont = [&](size_t k) {
    return i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80;
  };
  if (c < 0x80) {
    ++i;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && cont(1)) {
    uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
    i += 2;
    return cp < 0x80 ? 0xFFFD : cp;
  }
  if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 6) |
                  ((unsigned char)s[i + 2] & 0x3F);
    i += 3;
    return cp < 0x800 ? 0xFFFD : cp;
  }
  if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 12) |
                  ((uint32_t)((unsigned char)s[i + 2] & 0x3F) << 6) |
                  ((unsigned char)s[i + 3] & 0x3F);
    i += 4;
    return (cp < 0x10000 || cp > 0x10FFFF) ? 0xFFFD : cp;
  }
  ++i;
  return 0xFFFD;
}

size_t utf8_length(const std::string& s) {
  size_t i = 0, n = 0;
  while (i < s.size()) {
    utf8_next(s, i);
    ++n;
  }
  return n;
}

void utf8_append(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

std::vector<uint32_t> utf8_decode(const std::string& s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) out.push_back(utf8_next(s, i));
  return out;
}

size_t utf8_prev_index(const std::string& s, size_t i) {
  if (i == 0) return 0;
  size_t j = i - 1;
  while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) --j;
  return j;
}

namespace {

struct NamedEntity {
  const char* name;
  uint32_t cp;
};

// The named entities that actually turn up in EPUBs, plus the XML five.
const NamedEntity kEntities[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},
    {"apos", '\''},    {"nbsp", 0x00A0},   {"shy", 0x00AD},    {"ndash", 0x2013},
    {"mdash", 0x2014}, {"lsquo", 0x2018},  {"rsquo", 0x2019},  {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"sbquo", 0x201A},  {"bdquo", 0x201E},  {"dagger", 0x2020},
    {"Dagger", 0x2021},{"bull", 0x2022},   {"hellip", 0x2026}, {"prime", 0x2032},
    {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"oline", 0x203E},
    {"frasl", 0x2044}, {"euro", 0x20AC},   {"trade", 0x2122},  {"copy", 0x00A9},
    {"reg", 0x00AE},   {"deg", 0x00B0},    {"plusmn", 0x00B1}, {"sup2", 0x00B2},
    {"sup3", 0x00B3},  {"micro", 0x00B5},  {"para", 0x00B6},   {"middot", 0x00B7},
    {"frac14", 0x00BC},{"frac12", 0x00BD}, {"frac34", 0x00BE}, {"iquest", 0x00BF},
    {"times", 0x00D7}, {"divide", 0x00F7}, {"pound", 0x00A3},  {"yen", 0x00A5},
    {"cent", 0x00A2},  {"sect", 0x00A7},   {"laquo", 0x00AB},  {"raquo", 0x00BB},
    {"iexcl", 0x00A1}, {"szlig", 0x00DF},  {"agrave", 0x00E0}, {"aacute", 0x00E1},
    {"acirc", 0x00E2}, {"atilde", 0x00E3}, {"auml", 0x00E4},   {"aring", 0x00E5},
    {"aelig", 0x00E6}, {"ccedil", 0x00E7}, {"egrave", 0x00E8}, {"eacute", 0x00E9},
    {"ecirc", 0x00EA}, {"euml", 0x00EB},   {"igrave", 0x00EC}, {"iacute", 0x00ED},
    {"icirc", 0x00EE}, {"iuml", 0x00EF},   {"ntilde", 0x00F1}, {"ograve", 0x00F2},
    {"oacute", 0x00F3},{"ocirc", 0x00F4},  {"otilde", 0x00F5}, {"ouml", 0x00F6},
    {"oslash", 0x00F8},{"ugrave", 0x00F9}, {"uacute", 0x00FA}, {"ucirc", 0x00FB},
    {"uuml", 0x00FC},  {"yacute", 0x00FD}, {"yuml", 0x00FF},   {"ensp", 0x2002},
    {"emsp", 0x2003},  {"thinsp", 0x2009}, {"zwnj", 0x200C},   {"zwj", 0x200D},
    {"lrm", 0x200E},   {"rlm", 0x200F},    {"larr", 0x2190},   {"rarr", 0x2192},
    {"harr", 0x2194},  {"spades", 0x2660}, {"clubs", 0x2663},  {"hearts", 0x2665},
    {"diams", 0x2666},
};

}  // namespace

std::string decode_entities(const std::string& s) {
  if (s.find('&') == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') {
      out += s[i++];
      continue;
    }
    size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {
      out += s[i++];
      continue;
    }
    std::string name = s.substr(i + 1, semi - i - 1);
    bool handled = false;
    if (!name.empty() && name[0] == '#') {
      uint32_t cp = 0;
      if (name.size() > 2 && (name[1] == 'x' || name[1] == 'X')) {
        cp = (uint32_t)strtoul(name.c_str() + 2, nullptr, 16);
      } else {
        cp = (uint32_t)strtoul(name.c_str() + 1, nullptr, 10);
      }
      if (cp > 0 && cp <= 0x10FFFF) {
        utf8_append(out, cp);
        handled = true;
      }
    } else {
      for (const auto& e : kEntities) {
        if (name == e.name) {
          utf8_append(out, e.cp);
          handled = true;
          break;
        }
      }
    }
    if (handled) {
      i = semi + 1;
    } else {
      out += s[i++];
    }
  }
  return out;
}

int to_int(const std::string& s, int fallback) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  long v = strtol(s.c_str(), &end, 10);
  if (end == s.c_str()) return fallback;
  return (int)v;
}

double to_double(const std::string& s, double fallback) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str()) return fallback;
  return v;
}

std::string human_size(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB"};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 3) {
    v /= 1024.0;
    ++u;
  }
  return format(u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

std::string human_duration(int64_t seconds) {
  if (seconds < 60) return format("%llds", (long long)seconds);
  int64_t minutes = seconds / 60;
  if (minutes < 60) return format("%lldm", (long long)minutes);
  int64_t hours = minutes / 60;
  return format("%lldh %02lldm", (long long)hours, (long long)(minutes % 60));
}

}  // namespace ck
