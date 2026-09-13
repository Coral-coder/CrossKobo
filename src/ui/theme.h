#pragma once
#include <string>

#include "app/settings.h"
#include "gfx/color.h"

namespace ck {

// Colours and metrics for the shell. Three presets mirror CrossInk's
// themes: Classic (boxes and rules), Minimal (typography only) and
// Dashboard (stats-forward). Night mode flips the palette.
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

  std::string ui_font = "Lexend Deca";
  int base_px = 26;       // body text in the shell
  int small_px = 20;
  int title_px = 34;
  int row_height = 72;
  int padding = 24;
  int radius = 12;
  bool rules = true;      // draw separators and frames
  bool shadows = false;

  static Theme make(UiTheme which, bool night, int dpi);
};

const Theme& theme();
void set_theme(const Theme& t);
void refresh_theme_from_settings(int dpi);

}  // namespace ck
