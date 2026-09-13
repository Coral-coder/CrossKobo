#include "gfx/color.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/str.h"

namespace ck {

Color blend(Color dst, Color src) {
  if (src.a == 255) return src;
  if (src.a == 0) return dst;
  int sa = src.a;
  int da = 255 - sa;
  auto mix = [&](int s, int d) { return (uint8_t)((s * sa + d * da + 127) / 255); };
  uint8_t out_a = (uint8_t)std::min(255, sa + dst.a * da / 255);
  return Color(mix(src.r, dst.r), mix(src.g, dst.g), mix(src.b, dst.b), out_a);
}

Color lerp_color(Color a, Color b, float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  auto mix = [&](uint8_t x, uint8_t y) {
    return (uint8_t)std::lround(x + (y - x) * t);
  };
  return Color(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a));
}

Color saturate(Color c, float amount) {
  if (amount == 0.0f) return c;
  float l = (float)c.luma();
  auto push = [&](uint8_t v) {
    float out = l + (v - l) * (1.0f + amount);
    return (uint8_t)std::max(0.0f, std::min(255.0f, out));
  };
  return Color(push(c.r), push(c.g), push(c.b), c.a);
}

namespace {

struct NamedColor {
  const char* name;
  uint32_t hex;
};

const NamedColor kNamed[] = {
    {"black", 0x000000},   {"white", 0xFFFFFF},   {"red", 0xFF0000},    {"green", 0x008000},
    {"blue", 0x0000FF},    {"yellow", 0xFFFF00},  {"gray", 0x808080},   {"grey", 0x808080},
    {"silver", 0xC0C0C0},  {"maroon", 0x800000},  {"olive", 0x808000},  {"lime", 0x00FF00},
    {"aqua", 0x00FFFF},    {"cyan", 0x00FFFF},    {"teal", 0x008080},   {"navy", 0x000080},
    {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"purple", 0x800080}, {"orange", 0xFFA500},
    {"brown", 0xA52A2A},   {"pink", 0xFFC0CB},    {"gold", 0xFFD700},   {"indigo", 0x4B0082},
    {"darkred", 0x8B0000}, {"darkblue", 0x00008B},{"darkgreen", 0x006400},
    {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"dimgray", 0x696969},
    {"transparent", 0xFFFFFF},
};

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool parse_css_color(const std::string& text, Color& out) {
  std::string s = to_lower(trim(text));
  if (s.empty()) return false;

  if (s[0] == '#') {
    std::string hex = s.substr(1);
    if (hex.size() == 3 || hex.size() == 4) {
      int v[4] = {0, 0, 0, 15};
      for (size_t i = 0; i < hex.size(); ++i) {
        int d = hex_digit(hex[i]);
        if (d < 0) return false;
        v[i] = d;
      }
      out = Color((uint8_t)(v[0] * 17), (uint8_t)(v[1] * 17), (uint8_t)(v[2] * 17),
                  (uint8_t)(v[3] * 17));
      return true;
    }
    if (hex.size() == 6 || hex.size() == 8) {
      int v[4] = {0, 0, 0, 255};
      for (size_t i = 0; i * 2 + 1 < hex.size(); ++i) {
        int hi = hex_digit(hex[i * 2]), lo = hex_digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        v[i] = hi * 16 + lo;
      }
      out = Color((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]);
      return true;
    }
    return false;
  }

  if (starts_with(s, "rgb")) {
    size_t open = s.find('(');
    size_t close = s.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) return false;
    std::vector<std::string> parts = split(s.substr(open + 1, close - open - 1), ',');
    if (parts.size() < 3) return false;
    auto channel = [](const std::string& p) {
      std::string t = trim(p);
      if (!t.empty() && t.back() == '%') {
        return (uint8_t)std::max(0.0, std::min(255.0, to_double(t.substr(0, t.size() - 1)) * 2.55));
      }
      return (uint8_t)std::max(0, std::min(255, to_int(t)));
    };
    uint8_t a = 255;
    if (parts.size() >= 4) {
      double av = to_double(trim(parts[3]), 1.0);
      a = (uint8_t)std::max(0.0, std::min(255.0, av * 255.0));
    }
    out = Color(channel(parts[0]), channel(parts[1]), channel(parts[2]), a);
    return true;
  }

  if (s == "transparent") {
    out = Color::transparent();
    return true;
  }
  for (const auto& n : kNamed) {
    if (s == n.name) {
      out = Color::rgb(n.hex);
      return true;
    }
  }
  return false;
}

namespace {
// Deep, fully saturated inks: the Kaleido filter array halves effective
// saturation, so pastel pen colours end up indistinguishable from grey.
const InkColor kInk[] = {
    {"Black", Color::rgb(0x000000)},  {"Graphite", Color::rgb(0x4A4A4A)},
    {"Red", Color::rgb(0xD01414)},    {"Orange", Color::rgb(0xE06A00)},
    {"Yellow", Color::rgb(0xD8B400)}, {"Green", Color::rgb(0x0F8A2E)},
    {"Teal", Color::rgb(0x00757D)},   {"Blue", Color::rgb(0x1146C8)},
    {"Violet", Color::rgb(0x6B21B0)}, {"Magenta", Color::rgb(0xC01080)},
};
}  // namespace

const InkColor* ink_palette() { return kInk; }
int ink_palette_size() { return (int)(sizeof(kInk) / sizeof(kInk[0])); }

}  // namespace ck
