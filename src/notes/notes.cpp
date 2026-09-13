#include "notes/notes.h"

#include <algorithm>
#include <cmath>

#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"

namespace ck {
namespace {

constexpr const char* kExtension = "ckn";

// Stroke coordinates are written as flat integer arrays: three numbers per
// point. That keeps a page of handwriting to a few tens of kilobytes and
// still leaves the file readable and diffable.
Json stroke_to_json(const Stroke& s) {
  Json j = Json::object();
  j["color"] = Json(format("#%02X%02X%02X", s.color.r, s.color.g, s.color.b));
  j["width"] = Json(s.width);
  if (s.highlighter) j["highlighter"] = Json(true);
  Json pts = Json::array();
  for (const InkPoint& p : s.points) {
    pts.push_back(Json(p.x));
    pts.push_back(Json(p.y));
    pts.push_back(Json(p.pressure));
  }
  j["points"] = std::move(pts);
  return j;
}

Stroke stroke_from_json(const Json& j) {
  Stroke s;
  Color c;
  if (parse_css_color(j.get_string("color", "#000000"), c)) s.color = c;
  s.width = std::max(1, std::min(64, j.get_int("width", 3)));
  s.highlighter = j.get_bool("highlighter", false);
  const Json* pts = j.find("points");
  if (pts && pts->is_array()) {
    size_t count = pts->size() / 3;
    s.points.reserve(count);
    for (size_t i = 0; i + 2 < pts->size(); i += 3) {
      InkPoint p;
      p.x = pts->at(i).as_int(0);
      p.y = pts->at(i + 1).as_int(0);
      p.pressure = std::max(0, std::min(1000, pts->at(i + 2).as_int(500)));
      s.points.push_back(p);
    }
  }
  return s;
}

}  // namespace

Rect Stroke::bounds(int pad) const {
  if (points.empty()) return Rect();
  int min_x = points[0].x, max_x = points[0].x;
  int min_y = points[0].y, max_y = points[0].y;
  for (const InkPoint& p : points) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  int grow = width + pad + 2;
  return Rect(min_x - grow, min_y - grow, max_x - min_x + 2 * grow, max_y - min_y + 2 * grow);
}

const char* template_name(PageTemplate tmpl) {
  switch (tmpl) {
    case PageTemplate::Lined: return "lined";
    case PageTemplate::Grid: return "grid";
    case PageTemplate::Dots: return "dots";
    case PageTemplate::Cornell: return "cornell";
    case PageTemplate::Blank:
    default: return "blank";
  }
}

PageTemplate template_from_name(const std::string& name) {
  std::string n = to_lower(name);
  if (n == "lined") return PageTemplate::Lined;
  if (n == "grid") return PageTemplate::Grid;
  if (n == "dots") return PageTemplate::Dots;
  if (n == "cornell") return PageTemplate::Cornell;
  return PageTemplate::Blank;
}

void draw_page_template(Canvas& canvas, const Rect& area, PageTemplate tmpl) {
  // Rule spacing is derived from the area so templates look right at any
  // panel size, and light enough not to compete with the ink.
  const Color rule = Color::gray(196);
  const Color faint = Color::gray(212);
  int step = std::max(28, area.h / 44);
  switch (tmpl) {
    case PageTemplate::Lined:
      for (int y = area.y + step; y < area.bottom() - step / 2; y += step) {
        canvas.fill_rect(Rect(area.x, y, area.w, 1), rule);
      }
      break;
    case PageTemplate::Grid:
      for (int y = area.y + step; y < area.bottom(); y += step) {
        canvas.fill_rect(Rect(area.x, y, area.w, 1), faint);
      }
      for (int x = area.x + step; x < area.right(); x += step) {
        canvas.fill_rect(Rect(x, area.y, 1, area.h), faint);
      }
      break;
    case PageTemplate::Dots:
      for (int y = area.y + step; y < area.bottom(); y += step) {
        for (int x = area.x + step; x < area.right(); x += step) {
          canvas.fill_rect(Rect(x, y, 2, 2), rule);
        }
      }
      break;
    case PageTemplate::Cornell: {
      int cue = area.x + area.w / 4;
      int summary = area.bottom() - area.h / 6;
      canvas.fill_rect(Rect(cue, area.y, 1, summary - area.y), rule);
      canvas.fill_rect(Rect(area.x, summary, area.w, 1), rule);
      for (int y = area.y + step; y < summary; y += step) {
        canvas.fill_rect(Rect(cue + 1, y, area.right() - cue - 1, 1), faint);
      }
      break;
    }
    case PageTemplate::Blank:
    default:
      break;
  }
}

void draw_stroke(Canvas& canvas, const Stroke& stroke, const Rect& area, int page_w, int page_h,
                 bool pressure) {
  if (stroke.points.empty() || page_w <= 0 || page_h <= 0) return;
  double sx = (double)area.w / (double)page_w;
  double sy = (double)area.h / (double)page_h;
  double scale = std::min(sx, sy);

  auto map_x = [&](int x) { return (float)(area.x + x * scale); };
  auto map_y = [&](int y) { return (float)(area.y + y * scale); };
  auto width_at = [&](int p) {
    double w = stroke.width * scale;
    if (!pressure) return (float)w;
    // Pressure maps to 45%..130% of the nominal width: enough variation to
    // feel like a pen without the line disappearing on light strokes.
    double factor = 0.45 + 0.85 * (double)std::max(0, std::min(1000, p)) / 1000.0;
    return (float)std::max(0.6, w * factor);
  };

  Color color = stroke.color;

  if (stroke.points.size() == 1) {
    const InkPoint& p = stroke.points.front();
    int radius = std::max(1, (int)(width_at(p.pressure) / 2));
    if (stroke.highlighter) {
      canvas.fill_circle((int)map_x(p.x), (int)map_y(p.y), radius, color.with_alpha(90));
    } else {
      canvas.fill_circle((int)map_x(p.x), (int)map_y(p.y), radius, color);
    }
    return;
  }

  if (stroke.highlighter) {
    // A translucent stroke has to be composited once, not segment by
    // segment: overlapping segment ends would otherwise show as a row of
    // darker dots along the line. Build the whole stroke opaque in a
    // scratch buffer, then blend that in one pass.
    Rect box = stroke.bounds(2);
    Rect dst(area.x + (int)(box.x * scale) - 2, area.y + (int)(box.y * scale) - 2,
             (int)(box.w * scale) + 4, (int)(box.h * scale) + 4);
    dst = dst.intersect(canvas.clip());
    if (dst.empty()) return;
    Canvas scratch(dst.w, dst.h);
    scratch.clear(Color::transparent());
    for (size_t i = 1; i < stroke.points.size(); ++i) {
      const InkPoint& a = stroke.points[i - 1];
      const InkPoint& b = stroke.points[i];
      scratch.draw_thick_line_aa(map_x(a.x) - dst.x, map_y(a.y) - dst.y, map_x(b.x) - dst.x,
                                 map_y(b.y) - dst.y, width_at(a.pressure), width_at(b.pressure),
                                 color);
    }
    // Scale the accumulated coverage down to highlighter opacity.
    for (int y = 0; y < scratch.height(); ++y) {
      uint32_t* row = scratch.row(y);
      for (int x = 0; x < scratch.width(); ++x) {
        Color c = Color::unpack(row[x]);
        if (!c.a) continue;
        row[x] = c.with_alpha((uint8_t)(c.a * 90 / 255)).pack();
      }
    }
    canvas.blit(scratch, dst.x, dst.y);
    return;
  }

  for (size_t i = 1; i < stroke.points.size(); ++i) {
    const InkPoint& a = stroke.points[i - 1];
    const InkPoint& b = stroke.points[i];
    canvas.draw_thick_line_aa(map_x(a.x), map_y(a.y), map_x(b.x), map_y(b.y),
                              width_at(a.pressure), width_at(b.pressure), color);
  }
}

// ------------------------------------------------------------------ storage

std::string Notebook::directory() { return paths().notebooks; }

std::vector<std::string> Notebook::list() {
  std::vector<std::string> out;
  for (const fs::Entry& e : fs::list_dir(directory())) {
    if (!e.is_dir && fs::extension(e.path) == kExtension) out.push_back(e.path);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return fs::mtime(a) > fs::mtime(b);
  });
  return out;
}

std::string Notebook::path_for_title(const std::string& title) {
  return fs::join_path(directory(), fs::sanitize_filename(title) + "." + kExtension);
}

std::unique_ptr<Notebook> Notebook::create(const std::string& title,
                                           const std::string& linked_book) {
  fs::mkdir_p(directory());
  auto nb = std::unique_ptr<Notebook>(new Notebook());
  nb->title_ = title.empty() ? "Notebook" : title;
  nb->path_ = path_for_title(nb->title_);
  // Never silently overwrite an existing notebook.
  int suffix = 2;
  while (fs::exists(nb->path_)) {
    nb->title_ = format("%s %d", title.c_str(), suffix++);
    nb->path_ = path_for_title(nb->title_);
  }
  nb->linked_book_ = linked_book;
  nb->created_ = wall_seconds();
  nb->modified_ = nb->created_;
  nb->add_page(template_from_name(settings().note_template));
  if (!nb->save()) return nullptr;
  return nb;
}

std::unique_ptr<Notebook> Notebook::open(const std::string& path) {
  auto nb = std::unique_ptr<Notebook>(new Notebook());
  nb->path_ = path;
  if (!nb->load()) return nullptr;
  return nb;
}

std::unique_ptr<Notebook> Notebook::for_book(const std::string& book_title,
                                             const std::string& book_path) {
  for (const std::string& path : list()) {
    auto nb = open(path);
    if (nb && nb->linked_book() == book_path) return nb;
  }
  std::string title = book_title.empty() ? fs::stem(book_path) : book_title;
  return create("Notes on " + title, book_path);
}

bool Notebook::load() {
  Json j;
  if (!Json::parse_file(path_, j) || !j.is_object()) {
    CK_LOGW("notes: cannot read %s", path_.c_str());
    return false;
  }
  title_ = j.get_string("title", fs::stem(path_));
  linked_book_ = j.get_string("book");
  created_ = j.get_int64("created", 0);
  modified_ = j.get_int64("modified", created_);
  width_ = std::max(100, j.get_int("width", 1264));
  height_ = std::max(100, j.get_int("height", 1680));
  pages_.clear();
  if (const Json* pages = j.find("pages")) {
    for (const Json& pj : pages->items()) {
      NotePage page;
      page.tmpl = template_from_name(pj.get_string("template", "blank"));
      if (const Json* strokes = pj.find("strokes")) {
        for (const Json& sj : strokes->items()) page.strokes.push_back(stroke_from_json(sj));
      }
      pages_.push_back(std::move(page));
    }
  }
  if (pages_.empty()) pages_.push_back(NotePage());
  return true;
}

bool Notebook::save() {
  modified_ = wall_seconds();
  Json j = Json::object();
  j["format"] = Json("crosskobo-notebook-1");
  j["title"] = Json(title_);
  if (!linked_book_.empty()) j["book"] = Json(linked_book_);
  j["created"] = Json(created_);
  j["modified"] = Json(modified_);
  j["width"] = Json(width_);
  j["height"] = Json(height_);
  Json pages = Json::array();
  for (const NotePage& page : pages_) {
    Json pj = Json::object();
    pj["template"] = Json(std::string(template_name(page.tmpl)));
    Json strokes = Json::array();
    for (const Stroke& s : page.strokes) strokes.push_back(stroke_to_json(s));
    pj["strokes"] = std::move(strokes);
    pages.push_back(std::move(pj));
  }
  j["pages"] = std::move(pages);
  fs::mkdir_p(fs::dirname(path_));
  if (!j.save_file(path_, false)) {
    CK_LOGE("notes: failed to save %s", path_.c_str());
    return false;
  }
  return true;
}

bool Notebook::rename(const std::string& new_title) {
  if (new_title.empty()) return false;
  std::string target = path_for_title(new_title);
  if (target != path_ && fs::exists(target)) return false;
  std::string old = path_;
  title_ = new_title;
  path_ = target;
  if (!save()) {
    path_ = old;
    return false;
  }
  if (old != path_) fs::remove_file(old);
  return true;
}

bool Notebook::remove() { return fs::remove_file(path_); }

NotePage& Notebook::page(size_t index) {
  if (pages_.empty()) pages_.push_back(NotePage());
  return pages_[std::min(index, pages_.size() - 1)];
}

const NotePage& Notebook::page(size_t index) const {
  static const NotePage empty;
  if (pages_.empty()) return empty;
  return pages_[std::min(index, pages_.size() - 1)];
}

void Notebook::add_page(PageTemplate tmpl) {
  NotePage page;
  page.tmpl = tmpl;
  pages_.push_back(std::move(page));
}

void Notebook::insert_page(size_t index, PageTemplate tmpl) {
  NotePage page;
  page.tmpl = tmpl;
  pages_.insert(pages_.begin() + (long)std::min(index, pages_.size()), std::move(page));
}

bool Notebook::delete_page(size_t index) {
  if (pages_.size() <= 1 || index >= pages_.size()) return false;
  pages_.erase(pages_.begin() + (long)index);
  return true;
}

int Notebook::stroke_count() const {
  int total = 0;
  for (const NotePage& p : pages_) total += (int)p.strokes.size();
  return total;
}

void Notebook::set_size(int w, int h) {
  width_ = std::max(100, w);
  height_ = std::max(100, h);
}

void Notebook::render_page(size_t index, Canvas& canvas, const Rect& dst,
                           bool with_template) const {
  const NotePage& p = page(index);
  canvas.push_clip(dst);
  canvas.fill_rect(dst, Color::gray(255));
  if (with_template) draw_page_template(canvas, dst, p.tmpl);
  for (const Stroke& s : p.strokes) {
    draw_stroke(canvas, s, dst, width_, height_, settings().pen_pressure);
  }
  canvas.pop_clip();
}

bool Notebook::export_png(size_t index, const std::string& path) const {
  Canvas canvas(width_, height_);
  canvas.clear(Color::gray(255));
  render_page(index, canvas, Rect(0, 0, width_, height_), true);
  return canvas.save_png(path);
}

bool Notebook::export_all_png(const std::string& dir) const {
  fs::mkdir_p(dir);
  bool ok = true;
  for (size_t i = 0; i < pages_.size(); ++i) {
    std::string path = fs::join_path(dir, format("%s-%02zu.png",
                                                 fs::sanitize_filename(title_).c_str(), i + 1));
    ok = export_png(i, path) && ok;
  }
  return ok;
}

// ---------------------------------------------------------------- PDF export
// A minimal PDF writer. Strokes are emitted as vector paths, so exported
// notes stay sharp at any zoom and a whole notebook is a few tens of
// kilobytes rather than a stack of bitmaps.
bool Notebook::export_pdf(const std::string& path) const {
  // PDF user space is 72 units per inch; map the page to A4-ish
  // proportions based on the notebook's own aspect ratio.
  const double page_w = 595.0;
  const double page_h = page_w * (double)height_ / (double)width_;
  const double scale = page_w / (double)width_;

  std::vector<std::string> contents;
  contents.reserve(pages_.size());
  for (const NotePage& page : pages_) {
    std::string body;
    body += "1 J 1 j\n";  // round caps and joins
    // Template rules, drawn light grey.
    int step = std::max(28, height_ / 44);
    auto line = [&](double x0, double y0, double x1, double y1, double w, double grey) {
      body += format("%.3f %.3f %.3f RG %.2f w %.2f %.2f m %.2f %.2f l S\n", grey, grey, grey, w,
                     x0 * scale, page_h - y0 * scale, x1 * scale, page_h - y1 * scale);
    };
    switch (page.tmpl) {
      case PageTemplate::Lined:
        for (int y = step; y < height_ - step / 2; y += step) {
          line(0, y, width_, y, 0.5, 0.78);
        }
        break;
      case PageTemplate::Grid:
        for (int y = step; y < height_; y += step) line(0, y, width_, y, 0.4, 0.84);
        for (int x = step; x < width_; x += step) line(x, 0, x, height_, 0.4, 0.84);
        break;
      case PageTemplate::Dots:
        for (int y = step; y < height_; y += step) {
          for (int x = step; x < width_; x += step) line(x, y, x + 1, y, 1.2, 0.7);
        }
        break;
      case PageTemplate::Cornell: {
        double cue = width_ / 4.0;
        double summary = height_ - height_ / 6.0;
        line(cue, 0, cue, summary, 0.6, 0.7);
        line(0, summary, width_, summary, 0.6, 0.7);
        break;
      }
      case PageTemplate::Blank:
      default:
        break;
    }

    for (const Stroke& s : page.strokes) {
      if (s.points.empty()) continue;
      double r = s.color.r / 255.0, g = s.color.g / 255.0, b = s.color.b / 255.0;
      if (s.highlighter) {
        // No transparency groups: lighten the colour instead.
        r = r * 0.4 + 0.6;
        g = g * 0.4 + 0.6;
        b = b * 0.4 + 0.6;
      }
      // Pressure is approximated per segment by splitting the polyline
      // wherever the width changes noticeably.
      size_t start = 0;
      while (start + 1 < s.points.size() || (start == 0 && s.points.size() == 1)) {
        double width_units = std::max(0.3, s.width * scale *
                                               (0.45 + 0.85 * s.points[start].pressure / 1000.0));
        body += format("%.3f %.3f %.3f RG %.2f w %.2f %.2f m", r, g, b, width_units,
                       s.points[start].x * scale, page_h - s.points[start].y * scale);
        size_t i = start + 1;
        for (; i < s.points.size(); ++i) {
          body += format(" %.2f %.2f l", s.points[i].x * scale, page_h - s.points[i].y * scale);
          double next_w = std::max(0.3, s.width * scale *
                                            (0.45 + 0.85 * s.points[i].pressure / 1000.0));
          if (std::fabs(next_w - width_units) > 0.35) break;
        }
        body += " S\n";
        if (i >= s.points.size()) break;
        start = i;
      }
      if (s.points.size() == 1) {
        double w = std::max(0.5, s.width * scale);
        body += format("%.3f %.3f %.3f rg %.2f %.2f %.2f %.2f re f\n", r, g, b,
                       s.points[0].x * scale - w / 2, page_h - s.points[0].y * scale - w / 2, w,
                       w);
      }
    }
    contents.push_back(std::move(body));
  }

  // Object layout: 1 catalog, 2 pages tree, then per page an object pair.
  const size_t page_count = std::max<size_t>(1, contents.size());
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  auto add_object = [&](const std::string& body) {
    offsets.push_back(out.size());
    out += format("%zu 0 obj\n", offsets.size());
    out += body;
    out += "endobj\n";
  };

  std::string kids;
  for (size_t i = 0; i < page_count; ++i) kids += format("%zu 0 R ", 3 + i * 2);
  add_object("<< /Type /Catalog /Pages 2 0 R >>\n");
  add_object(format("<< /Type /Pages /Count %zu /Kids [%s] >>\n", page_count, kids.c_str()));
  for (size_t i = 0; i < page_count; ++i) {
    add_object(format("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
                      "/Contents %zu 0 R /Resources << >> >>\n",
                      page_w, page_h, 4 + i * 2));
    const std::string& body = i < contents.size() ? contents[i] : std::string();
    add_object(format("<< /Length %zu >>\nstream\n%sendstream\n", body.size(), body.c_str()));
  }

  size_t xref = out.size();
  out += format("xref\n0 %zu\n0000000000 65535 f \n", offsets.size() + 1);
  for (size_t off : offsets) out += format("%010zu 00000 n \n", off);
  out += format("trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
                offsets.size() + 1, xref);

  fs::mkdir_p(fs::dirname(path));
  return fs::write_file_atomic(path, out);
}

}  // namespace ck
