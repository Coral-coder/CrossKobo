// Renders text and shapes on the host and checks the output is not blank.
#include <cstdio>
#include <cstdlib>

#include "core/fs.h"
#include "core/log.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using namespace ck;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

// The Aero chrome: gradients, gloss, droplets and the page ground. These
// are cheap to get subtly wrong (a flipped gradient, a wash that turns to
// mud at night), so check the pixels rather than the intent.
static void test_aero_chrome() {
  Canvas c(120, 200);
  c.clear(Color::gray(255));
  c.fill_rect_gradient(Rect(0, 0, 120, 200), Color::rgb(0x000000), Color::rgb(0xFFFFFF));
  CHECK(c.get_pixel(60, 1).r < 12);
  CHECK(c.get_pixel(60, 198).r > 243);
  CHECK(c.get_pixel(60, 60).r < c.get_pixel(60, 140).r);

  // Gloss brightens the top of a shape and leaves the bottom alone.
  c.clear(Color::gray(128));
  c.fill_gloss(Rect(0, 0, 120, 200), 0, 120);
  CHECK(c.get_pixel(60, 4).r > 140);
  CHECK(c.get_pixel(60, 196).r <= 130);

  // A droplet stays inside its radius.
  c.clear(Color::gray(0));
  c.draw_bubble(60, 100, 20, Color(255, 255, 255, 90));
  CHECK(c.get_pixel(60, 100).r > 0);
  CHECK(c.get_pixel(60, 160) == Color::gray(0));
  // Asked for faintly, the rim and highlight come down with the body.
  c.clear(Color::gray(0));
  c.draw_bubble(60, 100, 20, Color(255, 255, 255, 12));
  CHECK(c.get_pixel(60, 100).r < 40);
  CHECK(c.get_pixel(53, 93).r < 60);

  Theme day = Theme::make(UiTheme::Aero, false, 265);
  CHECK(day.gradients && day.bubbles && day.shadows);
  CHECK(!day.dark);
  CHECK(day.bg_top != day.bg_bottom);
  Theme flat = Theme::make(UiTheme::Classic, false, 265);
  CHECK(!flat.gradients);
  CHECK(flat.bg_top == flat.bg && flat.bg_bottom == flat.bg);
  Theme dark = Theme::make(UiTheme::Aero, true, 265);
  CHECK(dark.dark);
  CHECK(dark.bg_top.r < 64);

  // wash() washes toward white by day and toward the dark ground at night,
  // so tinted glass keeps contrast against the text either way.
  Color hue = Color::rgb(0x2FA23C);
  set_theme(day);
  Color light_tint = wash(hue, 0.9f);
  CHECK(light_tint.r > 200 && light_tint.g > 200 && light_tint.b > 200);
  set_theme(dark);
  Color night_tint = wash(hue, 0.9f);
  CHECK(night_tint.g < 120);
  CHECK(night_tint.g > dark.bg.g);

  // The page ground follows the theme: a wash under Aero, flat otherwise.
  Canvas page(200, 400);
  set_theme(day);
  paint_background(page);
  CHECK(page.get_pixel(100, 2) != page.get_pixel(100, 397));
  set_theme(flat);
  paint_background(page);
  CHECK(page.get_pixel(100, 2) == page.get_pixel(100, 397));
  CHECK(page.get_pixel(100, 2) == flat.bg);
}

int main(int argc, char** argv) {
  log_init("", LogLevel::Warn);
  std::string assets = argc > 1 ? argv[1] : "assets/fonts";
  FontManager::instance().scan({assets});
  CHECK(!FontManager::instance().family_names().empty());

  Canvas c(600, 400);
  c.clear(Color::gray(255));
  c.fill_round_rect(Rect(20, 20, 560, 120), 16, Color::rgb(0xE8E8E8));
  c.draw_round_rect(Rect(20, 20, 560, 120), 16, Color::gray(0), 2);

  TextStyle st;
  st.family = "Lexend Deca";
  st.px = 36;
  st.color = Color::gray(0);
  int w = text_width("CrossKobo", st);
  CHECK(w > 60);
  draw_text(c, 40, 90, "CrossKobo colour e-ink shell", st);

  st.px = 22;
  st.color = Color::rgb(0xD01414);
  draw_text(c, 40, 200, "Red ink on Kaleido 3", st);
  st.color = Color::rgb(0x1146C8);
  draw_text(c, 40, 240, "Blue ink, pressure strokes", st);

  c.draw_thick_line_aa(40, 300, 560, 360, 2, 14, Color::rgb(0x0F8A2E));

  // Count non-white pixels: catches a silently broken glyph pipeline.
  int painted = 0;
  for (int y = 0; y < c.height(); ++y) {
    for (int x = 0; x < c.width(); ++x) {
      if (c.get_pixel(x, y) != Color::gray(255)) ++painted;
    }
  }
  CHECK(painted > 5000);

  test_aero_chrome();

  std::string out = "out/test_gfx.png";
  CHECK(c.save_png(out));
  CHECK(fs::file_size(out) > 1000);

  printf("%s (painted=%d)\n", failures ? "FAILED" : "ok", painted);
  return failures ? 1 : 0;
}
