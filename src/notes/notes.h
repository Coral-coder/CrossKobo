#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "ui/view.h"

namespace ck {

// One sampled point of a pen stroke. Coordinates are stored in page space
// (the same pixel grid the notebook was drawn on) so a notebook re-opens
// identically after a rotation or a firmware change.
struct InkPoint {
  int x = 0;
  int y = 0;
  int pressure = 500;  // 0..1000
};

struct Stroke {
  std::vector<InkPoint> points;
  Color color = Color::gray(0);
  int width = 3;        // nominal width in pixels at full pressure
  bool highlighter = false;

  Rect bounds(int pad = 0) const;
};

enum class PageTemplate { Blank, Lined, Grid, Dots, Cornell };

struct NotePage {
  std::vector<Stroke> strokes;
  PageTemplate tmpl = PageTemplate::Blank;
};

// A notebook is a single JSON file on the user-visible partition, so it can
// be copied off the device over USB without any export step.
class Notebook {
 public:
  static std::string directory();
  static std::vector<std::string> list();          // full paths
  static std::string path_for_title(const std::string& title);
  static std::unique_ptr<Notebook> create(const std::string& title,
                                          const std::string& linked_book = "");
  static std::unique_ptr<Notebook> open(const std::string& path);
  // Finds the notebook already linked to a book, or creates one.
  static std::unique_ptr<Notebook> for_book(const std::string& book_title,
                                            const std::string& book_path);

  bool save();
  bool rename(const std::string& new_title);
  bool remove();

  const std::string& title() const { return title_; }
  const std::string& path() const { return path_; }
  const std::string& linked_book() const { return linked_book_; }
  int64_t modified() const { return modified_; }
  int64_t created() const { return created_; }

  size_t page_count() const { return pages_.size(); }
  NotePage& page(size_t index);
  const NotePage& page(size_t index) const;
  void add_page(PageTemplate tmpl);
  void insert_page(size_t index, PageTemplate tmpl);
  bool delete_page(size_t index);
  int stroke_count() const;

  int width() const { return width_; }
  int height() const { return height_; }
  void set_size(int w, int h);

  // Exports. PNG writes one file per page; PDF writes a single vector
  // document, which stays small and prints cleanly.
  bool export_png(size_t index, const std::string& path) const;
  bool export_all_png(const std::string& dir) const;
  bool export_pdf(const std::string& path) const;

  // Renders a page (template plus ink) onto a canvas scaled into `dst`.
  void render_page(size_t index, Canvas& canvas, const Rect& dst, bool with_template = true) const;

 private:
  bool load();

  std::string path_;
  std::string title_;
  std::string linked_book_;
  int64_t created_ = 0;
  int64_t modified_ = 0;
  int width_ = 1264;
  int height_ = 1680;
  std::vector<NotePage> pages_;
};

// Draws a page background for the given template.
void draw_page_template(Canvas& canvas, const Rect& area, PageTemplate tmpl);
// Draws one stroke, honouring pressure when enabled.
void draw_stroke(Canvas& canvas, const Stroke& stroke, const Rect& area, int page_w, int page_h,
                 bool pressure);
const char* template_name(PageTemplate tmpl);
PageTemplate template_from_name(const std::string& name);

// Entry points used by the shell.
ViewPtr make_notes_browser();
// Opens (or creates) the notebook attached to a book and pushes the editor.
void open_note_for_book(const std::string& book_title, const std::string& book_path);
ViewPtr make_note_editor(std::unique_ptr<Notebook> notebook, size_t page_index = 0);

}  // namespace ck
