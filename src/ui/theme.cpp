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

  // Flat defaults; the Aero preset below turns the chrome on.
  t.bar_top = t.bg;
  t.bar_bottom = t.bg;
  t.bar_text = t.fg;
  t.bar_muted = t.muted;
  t.button_top = t.panel;
  t.button_bottom = t.panel;
  t.button_text = t.fg;
  t.panel_top = t.panel;
  t.panel_bottom = t.panel;
  t.section_top = t.panel;
  t.section_bottom = t.panel;
  t.section_text = t.muted;

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
    case UiTheme::Aero: {
      // Sky blues, aqua and fresh green; glass over everything; a dark
      // navy for text rather than black. Saturated on purpose: the Kaleido
      // filter array halves effective saturation, so gentle tints wash out
      // to grey on the panel.
      t.bg = Color::rgb(0xF2F8FF);
      t.fg = Color::rgb(0x102438);
      t.muted = Color::rgb(0x4A6A86);
      t.faint = Color::rgb(0xBBD6EE);
      t.border = Color::rgb(0x6FA5D6);
      t.panel = Color::rgb(0xEAF4FE);
      t.selection = Color::rgb(0xCFE6FA);
      t.accent = Color::rgb(0x1273D4);
      t.accent_soft = Color::rgb(0xD7EBFC);

      t.gradients = true;
      t.bubbles = true;
      t.shadows = true;
      t.rules = true;
      t.radius = px(16);

      t.bar_top = Color::rgb(0x3AA0E8);
      t.bar_bottom = Color::rgb(0x0F63B8);
      t.bar_text = Color::rgb(0xFFFFFF);
      t.bar_muted = Color::rgb(0xD8ECFF);

      t.button_top = Color::rgb(0xFBFDFF);
      t.button_bottom = Color::rgb(0xC9E2F7);
      t.button_text = Color::rgb(0x0E3A63);

      t.panel_top = Color::rgb(0xFFFFFF);
      t.panel_bottom = Color::rgb(0xDCEDFB);

      t.section_top = Color::rgb(0x4FB0EE);
      t.section_bottom = Color::rgb(0x1B79C8);
      t.section_text = Color::rgb(0xFFFFFF);

      t.bg_top = Color::rgb(0xFDFEFF);
      t.bg_bottom = Color::rgb(0xC4E1F8);

      t.accents4[0] = Color::rgb(0x1273D4);   // library: sky
      t.accents4[1] = Color::rgb(0x2FA23C);   // notebooks: grass
      t.accents4[2] = Color::rgb(0x00A4B4);   // statistics: aqua
      t.accents4[3] = Color::rgb(0xF0891B);   // settings: sun
      break;
    }
    case UiTheme::Classic:
    default:
      break;
  }

  if (night && which == UiTheme::Aero) {
    // Aero after dark: deep water rather than plain black.
    t.bg = Color::rgb(0x0A1420);
    t.fg = Color::rgb(0xDCEBFA);
    t.muted = Color::rgb(0x7C9AB4);
    t.faint = Color::rgb(0x24384C);
    t.border = Color::rgb(0x3C6FA0);
    t.panel = Color::rgb(0x132234);
    t.selection = Color::rgb(0x1E3C58);
    t.accent = Color::rgb(0x4FA8F0);
    t.accent_soft = Color::rgb(0x17293D);
    t.bar_top = Color::rgb(0x1B4670);
    t.bar_bottom = Color::rgb(0x0A2240);
    t.bar_text = Color::rgb(0xE6F2FF);
    t.bar_muted = Color::rgb(0x9EBDD8);
    t.button_top = Color::rgb(0x1B3350);
    t.button_bottom = Color::rgb(0x0F2338);
    t.button_text = Color::rgb(0xDCEBFA);
    t.panel_top = Color::rgb(0x17293D);
    t.panel_bottom = Color::rgb(0x0E1C2C);
    t.section_top = Color::rgb(0x235A8C);
    t.section_bottom = Color::rgb(0x123C63);
    t.section_text = Color::rgb(0xE6F2FF);
    t.bg_top = Color::rgb(0x13273E);
    t.bg_bottom = Color::rgb(0x05090F);
  } else if (night) {
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
  t.dark = night;
  if (!t.gradients) {
    t.bg_top = t.bg;
    t.bg_bottom = t.bg;
  }
  return t;
}

Color wash(Color hue, float amount) {
  const Theme& th = g_theme;
  // Night keeps a little more of the hue: a dark tint that washes as far
  // as the daylight one would go turns into featureless grey.
  return th.dark ? lerp_color(hue, th.bg, amount * 0.85f)
                 : lerp_color(hue, Color::gray(255), amount);
}

const Theme& theme() { return g_theme; }
void set_theme(const Theme& t) { g_theme = t; }

void refresh_theme_from_settings(int dpi) {
  g_theme = Theme::make(settings().theme, settings().night_mode, dpi);
}

}  // namespace ck
