#pragma once
#include <cstdint>
#include <string>

namespace ck {

// Canvas pixels are 32-bit, byte order R,G,B,A in memory (matching stb_image
// and stb_image_write). The Kaleido 3 panel is driven from a 32bpp
// framebuffer and the kernel's colour-filter-array pass turns true RGB into
// panel subpixels, so the whole pipeline stays in real colour and only the
// final blit reorders bytes for the framebuffer's layout.
struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 255;

  constexpr Color() = default;
  constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
      : r(r_), g(g_), b(b_), a(a_) {}

  static constexpr Color gray(uint8_t v, uint8_t a = 255) { return Color(v, v, v, a); }
  static constexpr Color rgb(uint32_t hex) {
    return Color((uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, 255);
  }
  static constexpr Color transparent() { return Color(0, 0, 0, 0); }

  uint32_t pack() const {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
  }
  static Color unpack(uint32_t px) {
    return Color((uint8_t)(px & 0xFF), (uint8_t)((px >> 8) & 0xFF), (uint8_t)((px >> 16) & 0xFF),
                 (uint8_t)((px >> 24) & 0xFF));
  }

  // Rec. 601 luma: what the panel shows if colour is disabled.
  uint8_t luma() const {
    return (uint8_t)((77 * (int)r + 150 * (int)g + 29 * (int)b) >> 8);
  }
  bool opaque() const { return a == 255; }
  Color with_alpha(uint8_t na) const { return Color(r, g, b, na); }
  Color inverted() const { return Color(255 - r, 255 - g, 255 - b, a); }

  bool operator==(const Color& o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
  bool operator!=(const Color& o) const { return !(*this == o); }
};

Color blend(Color dst, Color src);
Color lerp_color(Color a, Color b, float t);
// Saturation boost in HSP-ish space. Kaleido panels wash colour out, so both
// the theme and image rendering can pre-boost before the panel's own filter.
Color saturate(Color c, float amount);
// Parses "#rgb", "#rrggbb", "rgb(1,2,3)" and the CSS named colours we care
// about. Returns false when the value is not a colour at all.
bool parse_css_color(const std::string& text, Color& out);

// The e-ink safe palette used by the note editor: colours that survive the
// Kaleido colour filter array and still read as distinct on paper-white.
struct InkColor {
  const char* name;
  Color color;
};
const InkColor* ink_palette();
int ink_palette_size();

}  // namespace ck
