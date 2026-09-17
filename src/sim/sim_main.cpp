// Host-side simulator. It runs the real application against an in-memory
// framebuffer and writes each screen to a PNG, which is how the interface
// is reviewed and regression-checked without a Kobo on the desk.
#include <algorithm>
#include <cstdio>
#include <memory>
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
#include "core/version.h"
#include "gfx/font.h"
#include "library/library.h"
#include "notes/notes.h"
#include "platform/device.h"
#include "platform/input.h"
#include "platform/net.h"
#include "platform/screen.h"
#include "reader/reader.h"
#include "reader/state.h"
#include "ui/theme.h"

using namespace ck;

namespace {

std::string g_out = "out/sim";
int g_shots = 0;
// When set, every shot is also written scaled to this width under
// <out>/doc, which is what the documentation gallery embeds.
int g_doc_width = 0;

void shot(const std::string& name) {
  // Screenshots should show the screen, not a toast left over from setup.
  App::instance().clear_toast();
  App::instance().render_now();
  std::string path = format("%s/%02d-%s.png", g_out.c_str(), ++g_shots, name.c_str());
  const Canvas& src = Screen::instance().canvas();
  if (src.save_png(path)) {
    printf("  %s\n", path.c_str());
  } else {
    printf("  FAILED to write %s\n", path.c_str());
  }
  if (g_doc_width > 0 && src.width() > 0) {
    int h = std::max(1, src.height() * g_doc_width / src.width());
    Canvas small(g_doc_width, h);
    small.clear(Color::gray(255));
    small.blit_scaled(src, small.bounds());
    std::string dir = g_out + "/doc";
    fs::mkdir_p(dir);
    small.save_png(dir + "/" + name + ".png");
  }
}

void show(ViewPtr view, const std::string& name) {
  if (!view) {
    printf("  (no view for %s)\n", name.c_str());
    return;
  }
  App::instance().push(std::move(view));
  shot(name);
}

// A handwriting sample so the notes screenshots show real ink rather than
// an empty page.
void draw_sample_notes(Notebook& notebook) {
  auto add = [&](std::vector<std::pair<int, int>> pts, Color color, int width,
                 bool highlighter = false) {
    Stroke s;
    s.color = color;
    s.width = width;
    s.highlighter = highlighter;
    for (size_t i = 0; i < pts.size(); ++i) {
      InkPoint p;
      p.x = pts[i].first;
      p.y = pts[i].second;
      // Taper the ends, the way a real pen stroke reads.
      double t = (double)i / (double)std::max<size_t>(1, pts.size() - 1);
      p.pressure = (int)(400 + 600 * (1.0 - std::abs(0.5 - t) * 2.0));
      s.points.push_back(p);
    }
    notebook.page(0).strokes.push_back(std::move(s));
  };

  // "Hello" written in strokes, plus a highlight and a red annotation.
  add({{120, 300}, {120, 460}}, Color::gray(0), 4);
  add({{120, 380}, {200, 380}}, Color::gray(0), 4);
  add({{200, 300}, {200, 460}}, Color::gray(0), 4);
  add({{250, 340}, {250, 460}}, Color::gray(0), 4);
  add({{250, 340}, {300, 330}, {320, 370}, {280, 400}, {250, 395}}, Color::gray(0), 4);
  add({{360, 300}, {360, 460}}, Color::gray(0), 4);
  add({{410, 300}, {410, 460}}, Color::gray(0), 4);
  add({{470, 340}, {520, 330}, {545, 380}, {520, 430}, {470, 420}, {455, 380}, {470, 340}},
      Color::gray(0), 4);
  add({{110, 520}, {620, 520}}, Color::rgb(0xD8B400), 10, true);
  add({{120, 620}, {400, 600}}, Color::rgb(0xD01414), 3);
  add({{120, 660}, {520, 645}}, Color::rgb(0x1146C8), 3);
  add({{700, 300}, {760, 420}, {820, 300}, {880, 420}}, Color::rgb(0x0F8A2E), 6);
}

// A small library so the home and library screens have something in them.
// The EPUB the test suite builds is copied in when it is around, because a
// real book with covers, styling and colour plates makes for far more
// representative screenshots than a text file.
void make_fixture_library(const std::string& root) {
  fs::mkdir_p(root);
  for (const char* candidate : {"out/fixture.epub", "../out/fixture.epub"}) {
    if (!fs::exists(candidate)) continue;
    std::string target = fs::join_path(root, "The Colour of Ink.epub");
    if (!fs::exists(target)) fs::copy_file(candidate, target);
    break;
  }
  const char* kFiles[] = {"A Short Walk.txt", "Notes on Colour.txt", "The Kaleido Papers.txt"};
  const char* kBodies[] = {
      "A Short Walk\n\nThe path turned east at the old wall, where the light was already "
      "going.\n\nShe counted the steps out of habit, not need.\n",
      "Notes on Colour\n\nKaleido panels place a colour filter over the same greyscale "
      "ink, which is why colour resolves at half the linear resolution.\n\nThe practical "
      "consequence is that colour wants larger shapes.\n",
      "The Kaleido Papers\n\nKaleido 3 is a colour filter array laid over the same "
      "microcapsule ink that renders black and white, which is why colour resolves at "
      "half the linear resolution and why saturation is inherently gentle.\n\n"
      "The practical consequence for a reader is that colour wants larger shapes: a "
      "cover, a plate, a highlighter stroke. Fine coloured detail reads as noise.\n"};
  for (int i = 0; i < 3; ++i) {
    std::string path = fs::join_path(root, kFiles[i]);
    if (!fs::exists(path)) fs::write_file_atomic(path, kBodies[i]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string root = "out/sim-root";
  std::string book;
  int width = 1264, height = 1680;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--root" && i + 1 < argc) {
      root = argv[++i];
    } else if (arg == "--book" && i + 1 < argc) {
      book = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      g_out = argv[++i];
    } else if (arg == "--doc-width" && i + 1 < argc) {
      g_doc_width = to_int(argv[++i], 0);
    } else if (arg == "--size" && i + 1 < argc) {
      std::vector<std::string> parts = split(argv[++i], 'x');
      if (parts.size() == 2) {
        width = to_int(parts[0], width);
        height = to_int(parts[1], height);
      }
    }
  }

  fs::mkdir_p(g_out);
  make_fixture_library(root);

  Paths p;
  p.onboard = root;
  p.data = root + "/.crosskobo";
  p.notebooks = root + "/Notebooks";
  p.fonts_user = root + "/fonts";
  p.fonts_bundled = "assets/fonts";
  p.install = ".";
  set_paths(p);
  fs::mkdir_p(paths().data + "/books");
  fs::mkdir_p(paths().cache_dir());
  fs::mkdir_p(paths().notebooks);
  log_init("", LogLevel::Warn);

  App& app = App::instance();
  if (!app.init(true, width, height)) {
    fprintf(stderr, "simulator: could not initialise\n");
    return 1;
  }
  printf("CrossKobo simulator %dx%d, writing to %s/\n", width, height, g_out.c_str());

  // Give the fixture library some reading history so the home screen shows
  // a continue card and a populated grid.
  Recents::instance().load();
  for (const LibraryEntry& e : scan_library(root, false)) {
    if (e.is_dir) continue;
    Book b;
    if (!b.open(e.path)) continue;
    BookState st = BookState::load(b);
    st.progress = 0.12 + 0.3 * (double)(g_shots % 3);
    st.last_read = wall_seconds() - 3600;
    st.total_seconds = 4200;
    st.sessions = 6;
    st.pages_turned = 128;
    st.save();
    Recents::instance().touch(st);
  }
  Stats::instance().load();
  Stats::instance().add_reading(3600, 240, true);

  app.push(make_home_screen());
  shot("home-classic");

  settings().theme = UiTheme::Dashboard;
  refresh_theme_from_settings(Screen::instance().dpi());
  shot("home-dashboard");

  settings().theme = UiTheme::Aero;
  refresh_theme_from_settings(Screen::instance().dpi());
  shot("home-aero");
  show(make_quick_panel(), "quick-panel-aero");
  App::instance().pop();
  show(make_library_screen(root), "library-aero");
  App::instance().pop();
  show(make_transfer_screen(), "transfer-aero");
  App::instance().pop();
  show(make_catalogue_screen(), "catalogues-aero");
  App::instance().pop();
  show(make_settings_screen(), "settings-aero");
  App::instance().pop();

  settings().theme = UiTheme::Classic;
  refresh_theme_from_settings(Screen::instance().dpi());

  show(make_library_screen(root), "library");
  App::instance().pop();

  // Reader, on the richest book available.
  std::string reading = book;
  if (reading.empty()) {
    for (const LibraryEntry& e : scan_library(root, false)) {
      if (!e.is_dir) {
        reading = e.path;
        break;
      }
    }
  }
  if (!reading.empty()) {
    auto reader = std::make_unique<ReaderScreen>(reading);
    if (reader->ok()) {
      ReaderScreen* raw = reader.get();
      app.push(std::move(reader));
      shot("reader-page1");
      raw->next_page();
      shot("reader-page2");
      settings().focus_reading = true;
      shot("reader-focus");
      settings().focus_reading = false;
      raw->open_menu();
      shot("reader-menu");
      app.pop();
      app.pop();
    }
  }

  // Notes: a notebook with sample ink, in the editor.
  auto notebook = Notebook::open(fs::join_path(paths().notebooks, "Sample notes.ckn"));
  if (!notebook) {
    notebook = Notebook::create("Sample notes");
    if (notebook) {
      notebook->set_size(width, height - 90);
      notebook->page(0).tmpl = PageTemplate::Lined;
      draw_sample_notes(*notebook);
      notebook->add_page(PageTemplate::Grid);
      notebook->save();
    }
  }
  if (notebook) {
    std::string pdf = fs::join_path(g_out, "sample-notes.pdf");
    if (notebook->export_pdf(pdf)) printf("  %s (%s)\n", pdf.c_str(),
                                          human_size(fs::file_size(pdf)).c_str());
    std::string png = fs::join_path(g_out, "sample-notes-page1.png");
    notebook->export_png(0, png);
    show(make_note_editor(std::move(notebook), 0), "notes-editor");
    app.pop();
  }
  show(make_notes_browser(), "notes-browser");
  app.pop();

  // The network screen, with a simulated radio so it can be reviewed.
  Net::instance().set_simulated(true);
  Net::instance().power_on();
  show(make_network_screen(), "wifi");
  app.pop();
  Net::instance().connect("Reading Room", "", false);
  show(make_network_screen(), "wifi-connected");
  app.pop();

  // The USB chooser, which is what a cable going in looks like.
  show(make_usb_prompt_screen(), "usb-prompt");
  app.pop();

  show(make_settings_screen(), "settings");
  app.pop();
  show(make_stats_screen(), "statistics");
  app.pop();
  show(make_about_screen(), "about");
  app.pop();
  show(make_calibration_screen(), "calibration");
  app.pop();

  // Night mode, to check the palette flips cleanly.
  settings().night_mode = true;
  refresh_theme_from_settings(Screen::instance().dpi());
  Screen::instance().set_night_mode(true);
  shot("home-night");
  settings().theme = UiTheme::Aero;
  refresh_theme_from_settings(Screen::instance().dpi());
  shot("home-aero-night");
  settings().theme = UiTheme::Classic;
  settings().night_mode = false;
  refresh_theme_from_settings(Screen::instance().dpi());

  app.shutdown();
  printf("%d screenshots written\n", g_shots);
  return 0;
}
