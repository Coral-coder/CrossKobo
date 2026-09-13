// Notebook storage, rendering and export. The notebook file format is what
// a user's handwriting actually lives in, so a round-trip test matters more
// here than almost anywhere else.
#include <cstdio>
#include <string>

#include "app/settings.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "gfx/font.h"
#include "notes/notes.h"

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

Stroke make_stroke(int x0, int y0, int x1, int y1, Color color, int width) {
  Stroke s;
  s.color = color;
  s.width = width;
  for (int i = 0; i <= 10; ++i) {
    InkPoint p;
    p.x = x0 + (x1 - x0) * i / 10;
    p.y = y0 + (y1 - y0) * i / 10;
    p.pressure = 200 + i * 70;
    s.points.push_back(p);
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  log_init("", LogLevel::Error);
  std::string assets = argc > 1 ? argv[1] : "assets/fonts";
  FontManager::instance().scan({assets});

  std::string root = "out/notes-test";
  fs::remove_tree(root);
  Paths p;
  p.onboard = root;
  p.data = root + "/.crosskobo";
  p.notebooks = root + "/Notebooks";
  p.fonts_bundled = assets;
  set_paths(p);

  // ------------------------------------------------------------- create
  auto nb = Notebook::create("Test notebook");
  CHECK(nb != nullptr);
  if (!nb) return 1;
  CHECK(nb->page_count() == 1);
  CHECK(fs::exists(nb->path()));
  CHECK(ends_with(nb->path(), ".ckn"));

  // A second notebook with the same title must not overwrite the first.
  auto nb2 = Notebook::create("Test notebook");
  CHECK(nb2 != nullptr);
  CHECK(nb2->path() != nb->path());

  // --------------------------------------------------------------- ink
  nb->set_size(1264, 1590);
  nb->page(0).tmpl = PageTemplate::Lined;
  nb->page(0).strokes.push_back(make_stroke(100, 200, 600, 260, Color::gray(0), 4));
  nb->page(0).strokes.push_back(make_stroke(100, 400, 900, 420, Color::rgb(0xD01414), 3));
  Stroke marker = make_stroke(100, 600, 800, 600, Color::rgb(0xD8B400), 12);
  marker.highlighter = true;
  nb->page(0).strokes.push_back(marker);
  nb->add_page(PageTemplate::Grid);
  nb->page(1).strokes.push_back(make_stroke(50, 50, 500, 900, Color::rgb(0x1146C8), 6));
  CHECK(nb->page_count() == 2);
  CHECK(nb->stroke_count() == 4);
  CHECK(nb->save());

  // -------------------------------------------------------- round-trip
  std::string path = nb->path();
  auto loaded = Notebook::open(path);
  CHECK(loaded != nullptr);
  if (loaded) {
    CHECK(loaded->title() == "Test notebook");
    CHECK(loaded->page_count() == 2);
    CHECK(loaded->stroke_count() == 4);
    CHECK(loaded->width() == 1264);
    CHECK(loaded->height() == 1590);
    CHECK(loaded->page(0).tmpl == PageTemplate::Lined);
    CHECK(loaded->page(1).tmpl == PageTemplate::Grid);
    const Stroke& first = loaded->page(0).strokes[0];
    CHECK(first.points.size() == 11);
    CHECK(first.points[0].x == 100);
    CHECK(first.points[0].y == 200);
    CHECK(first.width == 4);
    CHECK(first.color == Color::gray(0));
    CHECK(loaded->page(0).strokes[1].color == Color::rgb(0xD01414));
    CHECK(loaded->page(0).strokes[2].highlighter);
    // Pressure has to survive: it is what makes the line look handwritten.
    CHECK(first.points[10].pressure == 900);
  }

  // The file must be readable JSON with a version marker, so a future
  // release can migrate it.
  Json j;
  CHECK(Json::parse_file(path, j));
  CHECK(j.get_string("format") == "crosskobo-notebook-1");
  CHECK(j.has("pages"));

  // -------------------------------------------------------------- list
  std::vector<std::string> all = Notebook::list();
  CHECK(all.size() == 2);

  // ---------------------------------------------------------- rendering
  Canvas canvas(632, 795);
  canvas.clear(Color::gray(255));
  loaded->render_page(0, canvas, Rect(0, 0, 632, 795), true);
  int painted = 0;
  bool found_red = false;
  for (int y = 0; y < canvas.height(); ++y) {
    for (int x = 0; x < canvas.width(); ++x) {
      Color c = canvas.get_pixel(x, y);
      if (c != Color::gray(255)) ++painted;
      if (c.r > 150 && c.g < 90 && c.b < 90) found_red = true;
    }
  }
  CHECK(painted > 2000);
  CHECK(found_red);   // ink stays in colour when scaled down

  // ------------------------------------------------------------ export
  std::string png = root + "/page.png";
  CHECK(loaded->export_png(0, png));
  CHECK(fs::file_size(png) > 1000);

  std::string pdf = root + "/notebook.pdf";
  CHECK(loaded->export_pdf(pdf));
  std::string pdf_data;
  CHECK(fs::read_file(pdf, pdf_data));
  CHECK(starts_with(pdf_data, "%PDF-1.4"));
  CHECK(pdf_data.find("%%EOF") != std::string::npos);
  CHECK(pdf_data.find("/Type /Catalog") != std::string::npos);
  CHECK(pdf_data.find("/Count 2") != std::string::npos);   // both pages
  CHECK(pdf_data.find("startxref") != std::string::npos);
  // Vector strokes, not a bitmap: the ink must appear as path operators.
  CHECK(pdf_data.find(" m ") != std::string::npos || pdf_data.find(" m\n") != std::string::npos);
  CHECK(pdf_data.find(" l ") != std::string::npos);
  CHECK(pdf_data.find("RG") != std::string::npos);
  // Vector export of four strokes should stay small.
  CHECK(pdf_data.size() < 200000);
  // The xref offsets must point at real objects, or readers reject the file.
  size_t xref = pdf_data.rfind("startxref");
  CHECK(xref != std::string::npos);
  if (xref != std::string::npos) {
    size_t offset = (size_t)to_int(trim(pdf_data.substr(xref + 9, 24)));
    CHECK(offset > 0 && offset < pdf_data.size());
    CHECK(pdf_data.compare(offset, 4, "xref") == 0);
  }

  std::string png_dir = root + "/pages";
  CHECK(loaded->export_all_png(png_dir));
  CHECK(fs::list_dir(png_dir).size() == 2);

  // ------------------------------------------------------- page editing
  CHECK(loaded->delete_page(1));
  CHECK(loaded->page_count() == 1);
  CHECK(!loaded->delete_page(0));    // never leave a notebook with no pages
  loaded->insert_page(0, PageTemplate::Dots);
  CHECK(loaded->page_count() == 2);
  CHECK(loaded->page(0).tmpl == PageTemplate::Dots);

  // ---------------------------------------------------------- book link
  auto linked = Notebook::for_book("The Colour of Ink", "/mnt/onboard/ink.epub");
  CHECK(linked != nullptr);
  if (linked) {
    CHECK(linked->linked_book() == "/mnt/onboard/ink.epub");
    CHECK(linked->save());
    auto again = Notebook::for_book("The Colour of Ink", "/mnt/onboard/ink.epub");
    CHECK(again != nullptr);
    // The same book must reopen the same notebook, not make another.
    if (again) CHECK(again->path() == linked->path());
  }

  // -------------------------------------------------------------- rename
  CHECK(loaded->rename("Renamed notebook"));
  CHECK(fs::exists(loaded->path()));
  CHECK(!fs::exists(path));

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
