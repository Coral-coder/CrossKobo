#include "ui/theme.h"

#include <algorithm>
#include <cmath>

namespace ck {
namespace {
Theme g_theme;
}

Theme Theme::make(UiTheme which, bool night, int dpi) {
  Theme t;
  // Scale type and spacing with the panel's real DPI so the shell looks the
  // same physical size on a 212 dpi Aura and a 300 dpi Libra Colour.
  double s = (double)dpi / 300.0;
  auto px = [&](int at300) { return std::max(10, (int)std::lround(at300 * s)); };

  t.base_px = px(26);
  t.small_px = px(20);
  t.title_px = px(34);
  t.row_height = px(74);
  t.padding = px(24);
  t.radius = px(12);

  switch (which) {
    case UiTheme::Minimal:
      t.rules = false;
      t.panel = Color::gray(255);
      t.selection = Color::gray(235);
      t.border = Color::gray(140);
      break;
    case UiTheme::Dashboard:
      t.rules = true;
      t.panel = Color::gray(243);
      t.accent = Color::rgb(0x0F8A2E);
      t.accent_soft = Color::rgb(0xE0F2E4);
      break;
    case UiTheme::Classic:
    default:
      break;
  }

  if (night) {
    // Kaleido panels look muddy with pure black, so night mode uses a very
    // dark grey ground and pulls the accents up in brightness.
    t.bg = Color::gray(18);
    t.fg = Color::gray(235);
    t.muted = Color::gray(150);
    t.faint = Color::gray(70);
    t.border = Color::gray(120);
    t.panel = Color::gray(34);
    t.selection = Color::gray(60);
    t.accent = Color::rgb(0x7FA3FF);
    t.accent_soft = Color::gray(44);
  }
  return t;
}

const Theme& theme() { return g_theme; }
void set_theme(const Theme& t) { g_theme = t; }

void refresh_theme_from_settings(int dpi) {
  g_theme = Theme::make(settings().theme, settings().night_mode, dpi);
}

}  // namespace ck
