// Drives the application through synthetic input the way the event loop
// does: home screen, library, reader, reader menu, typography changes,
// notebook drawing and export, settings. It asserts the screen actually
// changes and that nothing falls over - run under ASan/UBSan this is what
// catches the "a row's callback popped the view that owns it" class of bug.
#include <cstdio>
#include <string>
#include <vector>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "gfx/font.h"
#include "library/library.h"
#include "notes/notes.h"
#include "platform/input.h"
#include "platform/screen.h"
#include "ui/theme.h"

using namespace ck;

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

namespace {

const int kWidth = 1264;
const int kHeight = 1680;

InputEvent tap(int x, int y) {
  InputEvent e;
  e.type = EventType::Tap;
  e.x = x;
  e.y = y;
  e.time_ms = now_ms();
  return e;
}

InputEvent key(Key k) {
  InputEvent e;
  e.type = EventType::KeyDown;
  e.key = k;
  e.time_ms = now_ms();
  return e;
}

InputEvent swipe(SwipeDir dir) {
  InputEvent e;
  e.type = EventType::Swipe;
  e.swipe = dir;
  e.x = kWidth / 2;
  e.y = kHeight / 2;
  e.time_ms = now_ms();
  return e;
}

// An edge swipe: `from_y` is where the finger started, which is what the
// app tests against, so dy carries the rest.
InputEvent edge_swipe(SwipeDir dir, int from_y, int to_y) {
  InputEvent e;
  e.type = EventType::Swipe;
  e.swipe = dir;
  e.x = kWidth / 2;
  e.y = to_y;
  e.dy = to_y - from_y;
  e.time_ms = now_ms();
  return e;
}

InputEvent pen(EventType type, int x, int y, int pressure) {
  InputEvent e;
  e.type = type;
  e.x = x;
  e.y = y;
  e.pressure = pressure;
  e.tool = Tool::Pen;
  e.time_ms = now_ms();
  return e;
}

// A cheap fingerprint of what is on screen, to tell whether an action did
// anything at all.
uint32_t screen_hash() {
  const Canvas& c = Screen::instance().canvas();
  uint32_t hash = 2166136261u;
  for (int y = 0; y < c.height(); y += 7) {
    const uint32_t* row = c.row(y);
    for (int x = 0; x < c.width(); x += 5) {
      hash ^= row[x];
      hash *= 16777619u;
    }
  }
  return hash;
}

// The y of a list row, counting from the top bar.
int row_y(int index) {
  const Theme& th = theme();
  return th.row_height + index * th.row_height + th.row_height / 2;
}

// Centre of the nth button in a ListView action bar of `count` buttons.
Point action_button(int index, int count) {
  const Theme& th = theme();
  int gap = th.padding / 2;
  int total = kWidth - 2 * th.padding;
  int w = (total - gap * (count - 1)) / count;
  int x = th.padding + index * (w + gap) + w / 2;
  int y = kHeight - th.row_height / 2 - th.padding / 2;
  return {x, y};
}

void write_fixture_books(const std::string& root) {
  fs::mkdir_p(root);
  std::string body;
  for (int i = 0; i < 40; ++i) {
    body += "Paragraph " + format("%d", i) +
            ". The path turned east at the old wall, where the light was already going, "
            "and she counted the steps out of habit rather than need.\n\n";
  }
  fs::write_file_atomic(fs::join_path(root, "Interaction Test.txt"), body);
  fs::write_file_atomic(fs::join_path(root, "Second Book.txt"), body);
}

}  // namespace

int main(int argc, char** argv) {
  log_init("", LogLevel::Error);
  std::string assets = argc > 1 ? argv[1] : "assets/fonts";

  std::string root = "out/interaction";
  fs::remove_tree(root);
  write_fixture_books(root);

  Paths p;
  p.onboard = root;
  p.data = root + "/.crosskobo";
  p.notebooks = root + "/Notebooks";
  p.fonts_user = root + "/fonts";
  p.fonts_bundled = assets;
  p.install = ".";
  set_paths(p);
  fs::mkdir_p(paths().data + "/books");
  fs::mkdir_p(paths().cache_dir());
  fs::mkdir_p(paths().notebooks);

  App& app = App::instance();
  CHECK(app.init(true, kWidth, kHeight));
  app.push(make_home_screen());
  app.render_now();
  CHECK(app.depth() == 1);

  const Theme& th = theme();

  // ------------------------------------------------------------- library
  // The nav bar sits at the bottom of the home screen: Library is first.
  Point library_button = {th.padding + (kWidth - 2 * th.padding - 3 * (th.padding / 2)) / 8,
                          kHeight - (th.row_height + th.padding) / 2};
  uint32_t before = screen_hash();
  app.pump(tap(library_button.x, library_button.y));
  CHECK(app.depth() == 2);
  CHECK(screen_hash() != before);

  // Open a book. Row 0 is the Notebooks folder (folders sort first), so
  // the first book is row 1.
  before = screen_hash();
  app.pump(tap(kWidth / 2, row_y(1)));
  CHECK(app.depth() == 3);
  CHECK(screen_hash() != before);

  // ------------------------------------------------------------- reading
  // Page forward and back with the hardware buttons and with taps.
  for (int i = 0; i < 4; ++i) app.pump(key(Key::PageForward));
  uint32_t after_pages = screen_hash();
  app.pump(key(Key::PageBack));
  CHECK(screen_hash() != after_pages);
  app.pump(tap(kWidth - 60, kHeight / 2));   // right edge: next page
  app.pump(tap(60, kHeight / 2));            // left edge: previous page

  // A long press bookmarks the page, and doing it again removes it.
  InputEvent long_press;
  long_press.type = EventType::LongPress;
  long_press.x = kWidth / 2;
  long_press.y = kHeight / 2;
  app.pump(long_press);
  app.pump(long_press);

  // The reader menu, then the typography screen, then a size change.
  app.pump(swipe(SwipeDir::Up));
  CHECK(app.depth() == 4);
  before = screen_hash();
  app.pump(tap(kWidth / 2, row_y(5)));        // "Typography"
  CHECK(app.depth() == 5);
  CHECK(screen_hash() != before);
  int size_before = settings().font_size_pt;
  // Tap the right half of the size row to increase it.
  app.pump(tap(kWidth - 200, row_y(2)));
  CHECK(settings().font_size_pt >= size_before);
  app.pump(tap(200, row_y(2)));               // and the left half to decrease
  app.pump(swipe(SwipeDir::Right));           // back out of typography
  CHECK(app.depth() == 4);

  // Contents, from the menu. The fixture is a text book, so this is the
  // synthetic section list.
  before = screen_hash();
  app.pump(tap(kWidth / 2, row_y(0)));        // "Contents"
  CHECK(app.depth() == 4);                    // menu replaced by the TOC
  app.pump(tap(kWidth / 2, row_y(0)));        // jump to the first section
  CHECK(app.depth() == 3);                    // back in the reader

  // Closing the book from the menu must not take the reader's own callback
  // down with it: this is the regression that motivated deferred popping.
  app.pump(swipe(SwipeDir::Up));
  CHECK(app.depth() == 4);
  app.pump(tap(kWidth / 2, row_y(12)));       // "Close book"
  CHECK(app.depth() <= 2);

  // --------------------------------------------------------------- notes
  app.pop_to_root();
  app.render_now();
  app.push(make_notes_browser());
  app.render_now();
  CHECK(app.depth() == 2);
  // "New notebook" is the only action button.
  Point new_notebook = action_button(0, 1);
  app.pump(tap(new_notebook.x, new_notebook.y));
  CHECK(app.depth() == 3);
  CHECK(Notebook::list().size() == 1);

  // Draw with the stylus.
  int page_top = th.row_height + th.padding / 2;
  app.pump(pen(EventType::PenDown, 200, page_top + 200, 400));
  for (int i = 1; i <= 40; ++i) {
    app.pump(pen(EventType::PenMove, 200 + i * 15, page_top + 200 + (i % 7) * 9, 300 + i * 12));
  }
  app.pump(pen(EventType::PenUp, 800, page_top + 240, 0));
  app.pump(pen(EventType::PenDown, 200, page_top + 500, 700));
  for (int i = 1; i <= 20; ++i) {
    app.pump(pen(EventType::PenMove, 200 + i * 25, page_top + 500 + i * 3, 700));
  }
  app.pump(pen(EventType::PenUp, 700, page_top + 560, 0));

  // Toolbar: cycle the width, pick a colour, undo, redo, add a page.
  int tool_y = th.row_height / 2;
  int size = th.row_height;
  int pad = th.padding / 2;
  auto toolbar_slot = [&](int index) { return pad + index * (size + pad / 2) + size / 2; };
  app.pump(tap(toolbar_slot(4), tool_y));     // width
  app.pump(tap(toolbar_slot(5), tool_y));     // colour: opens the picker
  CHECK(app.depth() == 4);
  app.pump(tap(kWidth / 2, row_y(3)));        // choose an ink
  CHECK(app.depth() == 3);
  CHECK(settings().pen_color_index == 3);
  app.pump(tap(toolbar_slot(6), tool_y));     // undo
  app.pump(tap(toolbar_slot(7), tool_y));     // redo

  // The notebook menu, and an export.
  app.pump(tap(kWidth - pad - size / 2, tool_y));
  CHECK(app.depth() == 4);
  app.pump(tap(kWidth / 2, row_y(5)));        // "Export as PDF"
  std::vector<std::string> notebooks = Notebook::list();
  CHECK(notebooks.size() == 1);
  if (!notebooks.empty()) {
    std::string pdf = fs::join_path(Notebook::directory(),
                                    fs::stem(notebooks[0]) + ".pdf");
    CHECK(fs::exists(pdf));
    CHECK(fs::file_size(pdf) > 500);
  }
  app.pop_to_root();

  // ------------------------------------------------------------ settings
  app.push(make_settings_screen());
  app.render_now();
  CHECK(app.depth() == 2);
  app.pump(tap(kWidth / 2, row_y(1)));        // "Display and light"
  CHECK(app.depth() == 3);
  bool night_before = settings().night_mode;
  app.pump(tap(kWidth / 2, row_y(1)));        // night mode
  CHECK(settings().night_mode != night_before);
  app.pump(tap(kWidth / 2, row_y(1)));        // and back
  app.pump(swipe(SwipeDir::Right));
  CHECK(app.depth() == 2);

  // Statistics and the calibration screen, which handles raw input.
  app.push(make_stats_screen());
  app.render_now();
  app.pop();
  app.push(make_calibration_screen());
  app.render_now();
  app.pump(tap(kWidth / 2, kHeight / 3));
  app.pump(pen(EventType::PenDown, 300, 400, 500));
  app.pump(pen(EventType::PenMove, 400, 500, 600));
  app.pump(pen(EventType::PenUp, 400, 500, 0));
  app.render_now();
  app.pop_to_root();

  // Edge gestures: there is no hardware back button, so a swipe up from the
  // bottom edge has to be the way out of any screen, and a swipe down from
  // the top has to reach the quick panel.
  app.pop_to_root();
  app.push(make_library_screen(root));
  CHECK(app.depth() == 2);
  app.pump(edge_swipe(SwipeDir::Up, kHeight - 10, kHeight / 2));
  CHECK(app.depth() == 1);                       // back out of the library
  app.pump(edge_swipe(SwipeDir::Up, kHeight - 10, kHeight / 2));
  CHECK(app.depth() == 1);                       // and the root stays put
  app.pump(edge_swipe(SwipeDir::Down, 8, kHeight / 3));
  CHECK(app.depth() == 2);                       // quick panel
  app.render_now();
  app.pump(edge_swipe(SwipeDir::Up, kHeight - 10, kHeight / 2));
  CHECK(app.depth() == 1);
  // A swipe that starts in the middle is the view's own business: in the
  // reader those turn pages.
  app.push(make_library_screen(root));
  app.pump(edge_swipe(SwipeDir::Up, kHeight / 2, kHeight / 3));
  CHECK(app.depth() == 2);
  app.pop_to_root();

  // The catalogue list draws its empty state, and adding one walks two
  // keyboards; the browser itself needs a server, so it is not pushed here.
  app.push(make_catalogue_screen());
  app.render_now();
  CHECK(app.depth() == 2);
  app.pop();

  // Touch calibration. The wizard derives the mapping from three taps in
  // the panel's own coordinates, which is the only thing that works when
  // taps land nowhere near where they are drawn.
  {
    const int kW = 1264, kH = 1680;
    // A panel like the Libra Colour's: its X axis runs down the screen and
    // its Y axis runs right to left.
    auto to_raw = [&](int sx, int sy, int out[2]) {
      out[0] = sy;
      out[1] = kW - sx;
    };
    int tl[2], tr[2], bl[2];
    to_raw(0, 0, tl);
    to_raw(kW - 1, 0, tr);
    to_raw(0, kH - 1, bl);
    TouchTransform tf;
    CHECK(derive_touch_transform(tl, tr, bl, tf));
    CHECK(tf.swap_xy);
    CHECK(tf.mirror_x);
    CHECK(!tf.mirror_y);

    // A panel that needs nothing done to it.
    auto identity = [&](int sx, int sy, int out[2]) {
      out[0] = sx;
      out[1] = sy;
    };
    identity(0, 0, tl);
    identity(kW - 1, 0, tr);
    identity(0, kH - 1, bl);
    CHECK(derive_touch_transform(tl, tr, bl, tf));
    CHECK(!tf.swap_xy && !tf.mirror_x && !tf.mirror_y);

    // And one mounted upside down.
    auto rotated = [&](int sx, int sy, int out[2]) {
      out[0] = kW - sx;
      out[1] = kH - sy;
    };
    rotated(0, 0, tl);
    rotated(kW - 1, 0, tr);
    rotated(0, kH - 1, bl);
    CHECK(derive_touch_transform(tl, tr, bl, tf));
    CHECK(!tf.swap_xy && tf.mirror_x && tf.mirror_y);

    // Three taps in the same place say nothing.
    int same[2] = {100, 100};
    CHECK(!derive_touch_transform(same, same, same, tf));
  }

  // The wizard itself draws, and both page buttons together reach it.
  app.pop_to_root();
  app.push(make_touch_wizard());
  app.render_now();
  CHECK(app.depth() == 2);
  app.pop();

  // Device classification. The Libra Colour's panel reports a stylus tool
  // AND multitouch on one node; classifying it as a digitiser alone is what
  // left finger taps landing on stale coordinates.
  {
    InputCaps elan = classify_caps(true, true, true, true);
    CHECK(elan.touch && elan.pen);
    InputCaps panel = classify_caps(false, true, true, true);
    CHECK(panel.touch && !panel.pen);
    InputCaps wacom = classify_caps(true, false, true, false);
    CHECK(wacom.pen && !wacom.touch);
    InputCaps single_touch = classify_caps(false, false, true, true);
    CHECK(single_touch.touch && !single_touch.pen);
    InputCaps buttons = classify_caps(false, false, false, false);
    CHECK(!buttons.touch && !buttons.pen);
  }

  // Everything still standing, and the home screen still draws.
  app.render_now();
  CHECK(app.depth() == 1);
  CHECK(screen_hash() != 0);
  app.shutdown();

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
