// The handwriting editor. Ink is drawn straight onto the panel as the pen
// moves, using the controller's A2 waveform, so strokes appear under the
// nib instead of a frame later.
#include <algorithm>
#include <cmath>
#include <memory>

#include "app/app.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "notes/notes.h"
#include "platform/device.h"
#include "platform/screen.h"
#include "ui/icons.h"
#include "ui/list_view.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

enum ToolId {
  kToolBack = 1,
  kToolPen,
  kToolEraser,
  kToolHighlighter,
  kToolWidth,
  kToolColor,
  kToolUndo,
  kToolRedo,
  kToolPrevPage,
  kToolNextPage,
  kToolAddPage,
  kToolMenu,
};

// Local tool selection; distinct from ck::Tool, which describes the
// hardware pointing device that produced an event.
enum class InkTool { Pen, Eraser, Highlighter };

// How long touch input is ignored after the pen was last seen. Long enough
// to cover a hand resting between words, short enough that putting the
// stylus down and using a finger still works.
constexpr int64_t kPalmRejectMs = 900;

class NoteEditor : public View {
 public:
  NoteEditor(std::unique_ptr<Notebook> notebook, size_t page_index)
      : notebook_(std::move(notebook)), page_index_(page_index) {
    const Settings& s = settings();
    width_ = std::max(1, std::min(64, s.pen_width));
    color_index_ = std::max(0, std::min(ink_palette_size() - 1, s.pen_color_index));
  }

  void on_show() override {
    Rect page = page_rect();
    // A new notebook adopts the geometry of the screen it was created on.
    if (notebook_->stroke_count() == 0) notebook_->set_size(page.w, page.h);
  }

  void on_hide() override { save_if_dirty(); }

  bool draws_own_updates() const override { return true; }
  Refresh refresh_hint() const override { return Refresh::Image; }
  std::string title() const override { return notebook_->title(); }

  void draw(Canvas& canvas, const Rect& bounds) override;
  bool handle(const InputEvent& event) override;

 private:
  struct Action {
    enum class Kind { Add, Erase } kind = Kind::Add;
    size_t page = 0;
    std::vector<Stroke> strokes;
    std::vector<size_t> indices;  // for erase, where they were
  };

  Rect toolbar_rect() const {
    Rect b = Screen::instance().bounds();
    return Rect(b.x, b.y, b.w, theme().row_height + theme().padding / 2);
  }
  Rect page_rect() const {
    Rect b = Screen::instance().bounds();
    Rect bar = toolbar_rect();
    return Rect(b.x, bar.bottom(), b.w, b.h - bar.h);
  }

  Color current_color() const { return ink_palette()[color_index_].color; }

  // Page space is the notebook's own coordinate grid.
  InkPoint to_page(int x, int y, int pressure) const {
    Rect area = page_rect();
    double scale = std::min((double)area.w / notebook_->width(),
                            (double)area.h / notebook_->height());
    if (scale <= 0) scale = 1.0;
    InkPoint p;
    p.x = (int)std::lround((x - area.x) / scale);
    p.y = (int)std::lround((y - area.y) / scale);
    p.pressure = settings().pen_pressure ? pressure : 700;
    return p;
  }
  Rect from_page(const Rect& r) const {
    Rect area = page_rect();
    double scale = std::min((double)area.w / notebook_->width(),
                            (double)area.h / notebook_->height());
    return Rect(area.x + (int)(r.x * scale), area.y + (int)(r.y * scale),
                (int)(r.w * scale) + 2, (int)(r.h * scale) + 2);
  }

  void begin_stroke(const InputEvent& event);
  void extend_stroke(const InputEvent& event);
  void end_stroke();
  void erase_at(const InputEvent& event);
  void undo();
  void redo();
  void save_if_dirty();
  void change_page(int delta);
  void open_menu();
  void open_color_picker();
  void set_template(PageTemplate tmpl);

  std::unique_ptr<Notebook> notebook_;
  size_t page_index_ = 0;
  InkTool tool_ = InkTool::Pen;
  int width_ = 3;
  int color_index_ = 0;
  bool dirty_ = false;

  Stroke active_;
  bool drawing_ = false;
  int last_screen_x_ = 0, last_screen_y_ = 0;
  int last_pressure_ = 500;
  int64_t last_pen_ms_ = 0;

  std::vector<Action> undo_stack_;
  std::vector<Action> redo_stack_;
  HitList hits_;
};

void NoteEditor::draw(Canvas& canvas, const Rect& bounds) {
  const Theme& th = theme();
  hits_.clear();
  canvas.clear(th.bg);

  // ------------------------------------------------------------- toolbar
  Rect bar = toolbar_rect();
  canvas.fill_rect(bar, th.panel);
  canvas.fill_rect(Rect(bar.x, bar.bottom() - 1, bar.w, 1), th.faint);

  int pad = th.padding / 2;
  int size = bar.h - pad;
  int x = bar.x + pad;
  auto button = [&](int id, Icon icon, bool active, int w = 0) {
    Rect r(x, bar.y + pad / 2, w > 0 ? w : size, size);
    if (active) canvas.fill_round_rect(r, th.radius, th.selection);
    draw_icon(canvas, r, icon, th.fg);
    hits_.add(r, id);
    x = r.right() + pad / 2;
    return r;
  };

  button(kToolBack, Icon::Back, false);
  button(kToolPen, Icon::Pen, tool_ == InkTool::Pen);
  button(kToolHighlighter, Icon::Highlighter, tool_ == InkTool::Highlighter);
  button(kToolEraser, Icon::Eraser, tool_ == InkTool::Eraser);

  // Width: a swatch showing the actual nib size.
  {
    Rect r(x, bar.y + pad / 2, size, size);
    canvas.draw_round_rect(r, th.radius, th.faint, 1);
    canvas.fill_circle(r.center().x, r.center().y, std::max(2, width_ * 2), th.fg);
    hits_.add(r, kToolWidth);
    x = r.right() + pad / 2;
  }
  // Colour: the current ink, tap to pick another.
  {
    Rect r(x, bar.y + pad / 2, size, size);
    canvas.fill_circle(r.center().x, r.center().y, size / 2 - 4, current_color());
    canvas.draw_round_rect(r, th.radius, th.faint, 1);
    hits_.add(r, kToolColor);
    x = r.right() + pad / 2;
  }

  button(kToolUndo, Icon::Undo, false);
  button(kToolRedo, Icon::Redo, false);

  // Page navigation, right aligned.
  int right = bar.right() - pad;
  auto right_button = [&](int id, Icon icon, int w) {
    Rect r(right - w, bar.y + pad / 2, w, size);
    draw_icon(canvas, r, icon, th.fg);
    hits_.add(r, id);
    right = r.x - pad / 2;
    return r;
  };
  right_button(kToolMenu, Icon::Menu, size);
  right_button(kToolAddPage, Icon::Plus, size);
  right_button(kToolNextPage, Icon::Forward, size);
  {
    std::string label = format("%zu / %zu", page_index_ + 1, notebook_->page_count());
    TextStyle st = ui_style(th.small_px, th.muted);
    int w = text_width(label, st) + pad;
    Rect r(right - w, bar.y + pad / 2, w, size);
    draw_text_in(canvas, r, label, st, 0);
    right = r.x - pad / 2;
  }
  right_button(kToolPrevPage, Icon::Back, size);

  // ---------------------------------------------------------------- page
  Rect page = page_rect();
  notebook_->render_page(page_index_, canvas, page, true);
  if (dirty_) {
    // A small mark so it is obvious the page has unsaved changes.
    canvas.fill_circle(page.right() - 12, page.y + 12, 4, th.accent);
  }
}

void NoteEditor::begin_stroke(const InputEvent& event) {
  active_ = Stroke();
  active_.color = current_color();
  active_.width = width_;
  active_.highlighter = tool_ == InkTool::Highlighter;
  if (active_.highlighter) active_.width = std::max(8, width_ * 4);
  active_.points.push_back(to_page(event.x, event.y, event.pressure));
  drawing_ = true;
  last_screen_x_ = event.x;
  last_screen_y_ = event.y;
  last_pressure_ = event.pressure;

  // Paint the first dab immediately.
  Canvas& canvas = Screen::instance().canvas();
  Rect page = page_rect();
  canvas.push_clip(page);
  Stroke dab = active_;
  draw_stroke(canvas, dab, page, notebook_->width(), notebook_->height(),
              settings().pen_pressure);
  canvas.pop_clip();
  int r = active_.width + 4;
  Screen::instance().flush(Rect(event.x - r, event.y - r, 2 * r, 2 * r),
                           settings().notes_fast_ink ? Refresh::Pen : Refresh::Fast, false);
}

void NoteEditor::extend_stroke(const InputEvent& event) {
  if (!drawing_) return;
  InkPoint p = to_page(event.x, event.y, event.pressure);
  // Drop samples that land on the same pixel: they add file size and
  // nothing visible.
  if (!active_.points.empty()) {
    const InkPoint& last = active_.points.back();
    if (std::abs(last.x - p.x) < 1 && std::abs(last.y - p.y) < 1) return;
  }
  active_.points.push_back(p);

  Canvas& canvas = Screen::instance().canvas();
  Rect page = page_rect();
  double scale = std::min((double)page.w / notebook_->width(),
                          (double)page.h / notebook_->height());
  float w0 = (float)std::max(1.0, active_.width * scale *
                                      (settings().pen_pressure
                                           ? 0.45 + 0.85 * last_pressure_ / 1000.0
                                           : 1.0));
  float w1 = (float)std::max(1.0, active_.width * scale *
                                      (settings().pen_pressure
                                           ? 0.45 + 0.85 * event.pressure / 1000.0
                                           : 1.0));
  Color ink = active_.highlighter ? current_color().with_alpha(90) : current_color();
  canvas.push_clip(page);
  canvas.draw_thick_line_aa((float)last_screen_x_, (float)last_screen_y_, (float)event.x,
                            (float)event.y, w0, w1, ink);
  canvas.pop_clip();

  int pad = (int)std::ceil(std::max(w0, w1)) + 3;
  Rect dirty(std::min(last_screen_x_, event.x) - pad, std::min(last_screen_y_, event.y) - pad,
             std::abs(event.x - last_screen_x_) + 2 * pad,
             std::abs(event.y - last_screen_y_) + 2 * pad);
  Screen::instance().flush(dirty.intersect(page),
                           settings().notes_fast_ink ? Refresh::Pen : Refresh::Fast, false);

  last_screen_x_ = event.x;
  last_screen_y_ = event.y;
  last_pressure_ = event.pressure;
}

void NoteEditor::end_stroke() {
  if (!drawing_) return;
  drawing_ = false;
  if (active_.points.empty()) return;
  NotePage& page = notebook_->page(page_index_);
  page.strokes.push_back(active_);
  Action action;
  action.kind = Action::Kind::Add;
  action.page = page_index_;
  action.strokes.push_back(active_);
  undo_stack_.push_back(std::move(action));
  redo_stack_.clear();
  if (undo_stack_.size() > 80) undo_stack_.erase(undo_stack_.begin());
  active_ = Stroke();
  dirty_ = true;
}

void NoteEditor::erase_at(const InputEvent& event) {
  NotePage& page = notebook_->page(page_index_);
  InkPoint p = to_page(event.x, event.y, 0);
  int reach = std::max(6, width_ * 3);
  Action action;
  action.kind = Action::Kind::Erase;
  action.page = page_index_;
  Rect dirty;
  // Whole-stroke erase: what note apps on e-ink almost always do, and it
  // stays fast and undoable.
  for (size_t i = page.strokes.size(); i > 0; --i) {
    const Stroke& s = page.strokes[i - 1];
    if (!s.bounds(reach).contains(p.x, p.y)) continue;
    bool hit = false;
    for (const InkPoint& q : s.points) {
      int dx = q.x - p.x, dy = q.y - p.y;
      if (dx * dx + dy * dy <= reach * reach) {
        hit = true;
        break;
      }
    }
    if (!hit) continue;
    dirty = dirty.unite(from_page(s.bounds(2)));
    action.strokes.push_back(s);
    action.indices.push_back(i - 1);
    page.strokes.erase(page.strokes.begin() + (long)(i - 1));
  }
  if (action.strokes.empty()) return;
  undo_stack_.push_back(std::move(action));
  redo_stack_.clear();
  dirty_ = true;

  // Repaint the affected area: erasing needs the background back.
  Canvas& canvas = Screen::instance().canvas();
  Rect page_area = page_rect();
  Rect repaint = dirty.intersect(page_area);
  canvas.push_clip(repaint);
  notebook_->render_page(page_index_, canvas, page_area, true);
  canvas.pop_clip();
  Screen::instance().flush(repaint, Refresh::Fast, false);
}

void NoteEditor::undo() {
  if (undo_stack_.empty()) {
    App::instance().show_toast("Nothing to undo");
    return;
  }
  Action action = undo_stack_.back();
  undo_stack_.pop_back();
  NotePage& page = notebook_->page(action.page);
  if (action.kind == Action::Kind::Add) {
    if (!page.strokes.empty()) page.strokes.pop_back();
  } else {
    // Put erased strokes back where they were.
    for (size_t i = action.strokes.size(); i > 0; --i) {
      size_t at = std::min(action.indices[i - 1], page.strokes.size());
      page.strokes.insert(page.strokes.begin() + (long)at, action.strokes[i - 1]);
    }
  }
  redo_stack_.push_back(std::move(action));
  dirty_ = true;
  page_index_ = action.page;
  App::instance().invalidate(Refresh::Image);
}

void NoteEditor::redo() {
  if (redo_stack_.empty()) {
    App::instance().show_toast("Nothing to redo");
    return;
  }
  Action action = redo_stack_.back();
  redo_stack_.pop_back();
  NotePage& page = notebook_->page(action.page);
  if (action.kind == Action::Kind::Add) {
    for (const Stroke& s : action.strokes) page.strokes.push_back(s);
  } else {
    for (size_t index : action.indices) {
      if (index < page.strokes.size()) page.strokes.erase(page.strokes.begin() + (long)index);
    }
  }
  undo_stack_.push_back(std::move(action));
  dirty_ = true;
  App::instance().invalidate(Refresh::Image);
}

void NoteEditor::save_if_dirty() {
  if (!dirty_) return;
  if (notebook_->save()) {
    dirty_ = false;
  } else {
    App::instance().show_toast("Could not save the notebook");
  }
}

void NoteEditor::change_page(int delta) {
  save_if_dirty();
  int next = (int)page_index_ + delta;
  if (next < 0 || next >= (int)notebook_->page_count()) return;
  page_index_ = (size_t)next;
  App::instance().invalidate(Refresh::Flash);
}

void NoteEditor::set_template(PageTemplate tmpl) {
  notebook_->page(page_index_).tmpl = tmpl;
  settings().note_template = template_name(tmpl);
  settings().save();
  dirty_ = true;
  App::instance().invalidate(Refresh::Flash);
}

void NoteEditor::open_color_picker() {
  std::vector<ListView::Item> items;
  for (int i = 0; i < ink_palette_size(); ++i) {
    ListView::Item item;
    item.id = i;
    item.row.title = ink_palette()[i].name;
    item.row.swatch = ink_palette()[i].color;
    if (i == color_index_) item.row.check = true;
    items.push_back(std::move(item));
  }
  auto list = std::make_unique<ListView>("Ink colour", std::move(items), [this](int id) {
    color_index_ = std::max(0, std::min(ink_palette_size() - 1, id));
    settings().pen_color_index = color_index_;
    settings().save();
    App::instance().pop();
    App::instance().invalidate(Refresh::Image);
  });
  App::instance().push(std::move(list));
}

void NoteEditor::open_menu() {
  enum {
    kExportPdf = 1,
    kExportPng,
    kTemplate,
    kDeletePage,
    kInsertPage,
    kRenameNotebook,
    kDeleteNotebook,
    kPressure,
    kFastInk,
    kInfo,
  };
  std::vector<ListView::Item> items;
  auto add = [&](int id, std::string title, std::string trailing = "",
                 std::string subtitle = "") {
    ListView::Item item;
    item.id = id;
    item.row.title = std::move(title);
    item.row.trailing = std::move(trailing);
    item.row.subtitle = std::move(subtitle);
    items.push_back(std::move(item));
  };
  add(kTemplate, "Page template", template_name(notebook_->page(page_index_).tmpl));
  add(kInsertPage, "Insert page after this one");
  add(kDeletePage, "Delete this page");
  add(kPressure, "Pressure sensitivity", settings().pen_pressure ? "On" : "Off");
  add(kFastInk, "Fast ink refresh", settings().notes_fast_ink ? "On" : "Off",
      "Uses the A2 waveform while drawing");
  add(kExportPdf, "Export as PDF", "", "Vector, one file for the notebook");
  add(kExportPng, "Export pages as PNG");
  add(kRenameNotebook, "Rename notebook", notebook_->title());
  add(kDeleteNotebook, "Delete notebook");
  add(kInfo, "Notebook details",
      format("%zu pages, %d strokes", notebook_->page_count(), notebook_->stroke_count()));

  auto list = std::make_unique<ListView>("Notebook", std::move(items), [this](int id) {
    switch (id) {
      case kTemplate: {
        std::vector<ListView::Item> options;
        static const PageTemplate kAll[] = {PageTemplate::Blank, PageTemplate::Lined,
                                            PageTemplate::Grid, PageTemplate::Dots,
                                            PageTemplate::Cornell};
        for (int i = 0; i < 5; ++i) {
          ListView::Item item;
          item.id = i;
          item.row.title = template_name(kAll[i]);
          options.push_back(std::move(item));
        }
        App::instance().push(std::make_unique<ListView>(
            "Template", std::move(options), [this](int choice) {
              static const PageTemplate kAll[] = {PageTemplate::Blank, PageTemplate::Lined,
                                                  PageTemplate::Grid, PageTemplate::Dots,
                                                  PageTemplate::Cornell};
              set_template(kAll[std::max(0, std::min(4, choice))]);
              App::instance().pop();
              App::instance().pop();
            }));
        break;
      }
      case kInsertPage:
        notebook_->insert_page(page_index_ + 1,
                               template_from_name(settings().note_template));
        ++page_index_;
        dirty_ = true;
        save_if_dirty();
        App::instance().pop();
        App::instance().invalidate(Refresh::Flash);
        break;
      case kDeletePage:
        if (notebook_->page_count() <= 1) {
          App::instance().show_toast("A notebook needs at least one page");
          break;
        }
        if (App::instance().confirm("Delete page",
                                    format("Delete page %zu of %zu?", page_index_ + 1,
                                           notebook_->page_count()),
                                    "Delete", "Keep")) {
          notebook_->delete_page(page_index_);
          if (page_index_ >= notebook_->page_count()) page_index_ = notebook_->page_count() - 1;
          dirty_ = true;
          save_if_dirty();
          App::instance().pop();
          App::instance().invalidate(Refresh::Flash);
        }
        break;
      case kPressure:
        settings().pen_pressure = !settings().pen_pressure;
        settings().save();
        App::instance().pop();
        App::instance().invalidate(Refresh::Image);
        break;
      case kFastInk:
        settings().notes_fast_ink = !settings().notes_fast_ink;
        settings().save();
        App::instance().pop();
        break;
      case kExportPdf: {
        save_if_dirty();
        std::string out = fs::join_path(Notebook::directory(),
                                        fs::sanitize_filename(notebook_->title()) + ".pdf");
        if (notebook_->export_pdf(out)) {
          App::instance().show_message("Exported", "Saved to\n" + out);
        } else {
          App::instance().show_message("Export failed", "Could not write\n" + out);
        }
        break;
      }
      case kExportPng: {
        save_if_dirty();
        std::string dir = fs::join_path(Notebook::directory(),
                                        fs::sanitize_filename(notebook_->title()) + " pages");
        if (notebook_->export_all_png(dir)) {
          App::instance().show_message("Exported", format("%zu pages saved to\n%s",
                                                          notebook_->page_count(), dir.c_str()));
        } else {
          App::instance().show_message("Export failed", "Could not write to\n" + dir);
        }
        break;
      }
      case kRenameNotebook:
        App::instance().show_message(
            "Rename notebook",
            "Notebooks are plain files in\n" + Notebook::directory() +
                "\n\nRename the .ckn file from your computer over USB and the new name "
                "appears here.");
        break;
      case kDeleteNotebook:
        if (App::instance().confirm("Delete notebook",
                                    "Delete \"" + notebook_->title() + "\" and all its pages?",
                                    "Delete", "Keep")) {
          notebook_->remove();
          App::instance().pop();  // menu
          App::instance().pop();  // editor
        }
        break;
      case kInfo:
        App::instance().show_message(
            notebook_->title(),
            format("%zu pages\n%d strokes\nCreated %s\nModified %s\n\nFile:\n%s",
                   notebook_->page_count(), notebook_->stroke_count(),
                   format_date(notebook_->created()).c_str(),
                   relative_time(notebook_->modified()).c_str(), notebook_->path().c_str()));
        break;
      default:
        break;
    }
  });
  App::instance().push(std::move(list));
}

bool NoteEditor::handle(const InputEvent& event) {
  Rect page = page_rect();

  // ------------------------------------------------------------ pen input
  if (event.is_pen()) {
    last_pen_ms_ = now_ms();
    bool in_page = page.contains(event.x, event.y);
    // Kobo styluses report their eraser end as a separate tool; honour it
    // whatever is selected in the toolbar.
    InkTool effective = event.tool == ck::Tool::Eraser ? InkTool::Eraser : tool_;

    if (event.type == EventType::PenDown) {
      if (!in_page) return false;
      if (effective == InkTool::Eraser) {
        erase_at(event);
      } else {
        begin_stroke(event);
      }
      return false;  // we painted directly
    }
    if (event.type == EventType::PenMove) {
      if (effective == InkTool::Eraser) {
        if (in_page) erase_at(event);
      } else if (drawing_ && in_page) {
        extend_stroke(event);
      } else if (drawing_ && !in_page) {
        end_stroke();
      }
      return false;
    }
    if (event.type == EventType::PenUp) {
      end_stroke();
      // Settle the ink with a proper waveform: A2 leaves light grey edges.
      if (settings().notes_fast_ink) {
        Screen::instance().flush(page, Refresh::Text, false);
      }
      return false;
    }
  }

  // ---------------------------------------------------------- touch input
  bool pen_recently_used = now_ms() - last_pen_ms_ < kPalmRejectMs;
  bool touch_draws = !Input::instance().has_pen();

  switch (event.type) {
    case EventType::Tap: {
      int id = hits_.hit(event.x, event.y);
      if (id > 0) {
        switch (id) {
          case kToolBack:
            save_if_dirty();
            App::instance().pop();
            return true;
          case kToolPen:
            tool_ = InkTool::Pen;
            return true;
          case kToolEraser:
            tool_ = InkTool::Eraser;
            return true;
          case kToolHighlighter:
            tool_ = InkTool::Highlighter;
            return true;
          case kToolWidth: {
            static const int kWidths[] = {1, 2, 3, 5, 8, 12};
            int index = 0;
            for (int i = 0; i < 6; ++i) {
              if (kWidths[i] == width_) index = i;
            }
            width_ = kWidths[(index + 1) % 6];
            settings().pen_width = width_;
            settings().save();
            return true;
          }
          case kToolColor:
            open_color_picker();
            return true;
          case kToolUndo:
            undo();
            return true;
          case kToolRedo:
            redo();
            return true;
          case kToolPrevPage:
            change_page(-1);
            return true;
          case kToolNextPage:
            change_page(1);
            return true;
          case kToolAddPage:
            save_if_dirty();
            notebook_->add_page(template_from_name(settings().note_template));
            page_index_ = notebook_->page_count() - 1;
            dirty_ = true;
            save_if_dirty();
            App::instance().invalidate(Refresh::Flash);
            return true;
          case kToolMenu:
            open_menu();
            return true;
          default:
            break;
        }
      }
      return false;
    }
    case EventType::TouchDown:
      if (touch_draws && page.contains(event.x, event.y) &&
          !(settings().palm_rejection && pen_recently_used)) {
        InputEvent e = event;
        e.pressure = 700;
        if (tool_ == InkTool::Eraser) {
          erase_at(e);
        } else {
          begin_stroke(e);
        }
      }
      return false;
    case EventType::TouchMove:
      if (touch_draws && drawing_ && page.contains(event.x, event.y)) {
        InputEvent e = event;
        e.pressure = 700;
        extend_stroke(e);
      } else if (touch_draws && tool_ == InkTool::Eraser && page.contains(event.x, event.y) &&
                 !pen_recently_used) {
        erase_at(event);
      }
      return false;
    case EventType::TouchUp:
      if (drawing_) {
        end_stroke();
        if (settings().notes_fast_ink) Screen::instance().flush(page, Refresh::Text, false);
      }
      return false;
    case EventType::Swipe:
      if (event.swipe == SwipeDir::Left) {
        change_page(1);
        return true;
      }
      if (event.swipe == SwipeDir::Right) {
        // Only outside the page area, so a swipe over the page can draw.
        if (!page.contains(event.x, event.y) || !touch_draws) {
          change_page(-1);
          return true;
        }
      }
      return false;
    case EventType::KeyDown:
      if (event.key == Key::PageForward) {
        change_page(1);
        return true;
      }
      if (event.key == Key::PageBack) {
        change_page(-1);
        return true;
      }
      if (event.key == Key::Home) {
        save_if_dirty();
        App::instance().pop_to_root();
        return true;
      }
      return false;
    default:
      return false;
  }
}

}  // namespace

ViewPtr make_note_editor(std::unique_ptr<Notebook> notebook, size_t page_index) {
  if (!notebook) return nullptr;
  return std::make_unique<NoteEditor>(std::move(notebook), page_index);
}

void open_note_for_book(const std::string& book_title, const std::string& book_path) {
  auto notebook = Notebook::for_book(book_title, book_path);
  if (!notebook) {
    App::instance().show_message("Notes", "Could not create a notebook in\n" +
                                              Notebook::directory());
    return;
  }
  App::instance().push(make_note_editor(std::move(notebook), 0));
}

}  // namespace ck
