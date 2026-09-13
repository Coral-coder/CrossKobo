#pragma once
#include <string>

#include "app/settings.h"
#include "gfx/color.h"

namespace ck {

// Colours and metrics for the shell. Four presets: Classic (boxes and
// rules), Minimal (typography only), Dashboard (stats-forward) and Aero -
// the glossy, sky-blue, water-droplet look of the late 2000s, which the
// colour panel is finally able to render. Night mode flips the palette.
struct Theme {
  Color bg = Color::gray(255);
  Color fg = Color::gray(0);
  Color muted = Color::gray(110);
  Color faint = Color::gray(190);
  Color border = Color::gray(60);
  Color accent = Color::rgb(0x1146C8);
  Color accent_soft = Color::rgb(0xE3E9FA);
  Color selection = Color::gray(225);
  Color panel = Color::gray(247);

  // Aero chrome: gradients, a glossy wash, soft shadows and droplets.
  bool gradients = false;
  bool bubbles = false;
  bool shadows = false;
  bool dark = false;      // night palette: wash tints toward the ground
  // Page ground. Flat themes set both to bg; Aero washes white down to sky.
  Color bg_top = Color::gray(255);
  Color bg_bottom = Color::gray(255);
  Color bar_top = Color::gray(255);
  Color bar_bottom = Color::gray(255);
  Color bar_text = Color::gray(0);
  Color bar_muted = Color::gray(110);
  Color button_top = Color::gray(247);
  Color button_bottom = Color::gray(232);
  Color button_text = Color::gray(0);
  Color panel_top = Color::gray(250);
  Color panel_bottom = Color::gray(240);
  Color section_top = Color::gray(247);
  Color section_bottom = Color::gray(240);
  Color section_text = Color::gray(110);
  // Four hues for things that want to be told apart: library, notebooks,
  // statistics, settings.
  Color accents4[4] = {Color::rgb(0x1146C8), Color::rgb(0x0F8A2E), Color::rgb(0x00757D),
                       Color::rgb(0xE06A00)};

  std::string ui_font = "Lexend Deca";
  int base_px = 26;       // body text in the shell
  int small_px = 20;
  int title_px = 34;
  int row_height = 72;
  int padding = 24;
  int radius = 12;
  bool rules = true;      // draw separators and frames

  static Theme make(UiTheme which, bool night, int dpi);
};

// Washes a hue toward the page ground - white by day, deep water at
// night - so tinted glass stays legible under either palette. `amount` is
// how far to wash, 0 for the pure hue and 1 for the ground.
Color wash(Color hue, float amount);

const Theme& theme();
void set_theme(const Theme& t);
void refresh_theme_from_settings(int dpi);

}  // namespace ck
