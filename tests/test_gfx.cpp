// Renders text and shapes on the host and checks the output is not blank.
#include <cstdio>
#include <cstdlib>

#include "core/fs.h"
#include "core/log.h"
#include "gfx/canvas.h"
#include "gfx/font.h"

using namespace ck;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

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

  std::string out = "out/test_gfx.png";
  CHECK(c.save_png(out));
  CHECK(fs::file_size(out) > 1000);

  printf("%s (painted=%d)\n", failures ? "FAILED" : "ok", painted);
  return failures ? 1 : 0;
}
