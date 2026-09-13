#pragma once
#include <algorithm>

namespace ck {

struct Point {
  int x = 0;
  int y = 0;
};

struct Size {
  int w = 0;
  int h = 0;
};

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;

  Rect() = default;
  Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  int area() const { return empty() ? 0 : w * h; }
  Point center() const { return {x + w / 2, y + h / 2}; }

  bool contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
  bool contains(const Point& p) const { return contains(p.x, p.y); }

  bool intersects(const Rect& o) const {
    return !(o.x >= right() || o.right() <= x || o.y >= bottom() || o.bottom() <= y);
  }

  Rect intersect(const Rect& o) const {
    int nx = std::max(x, o.x);
    int ny = std::max(y, o.y);
    int nr = std::min(right(), o.right());
    int nb = std::min(bottom(), o.bottom());
    if (nr <= nx || nb <= ny) return Rect();
    return Rect(nx, ny, nr - nx, nb - ny);
  }

  // Smallest rectangle covering both; an empty operand is ignored so
  // accumulating dirty regions can start from Rect().
  Rect unite(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    int nx = std::min(x, o.x);
    int ny = std::min(y, o.y);
    int nr = std::max(right(), o.right());
    int nb = std::max(bottom(), o.bottom());
    return Rect(nx, ny, nr - nx, nb - ny);
  }

  Rect inset(int d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
  Rect inset(int dx, int dy) const { return Rect(x + dx, y + dy, w - 2 * dx, h - 2 * dy); }
  Rect offset(int dx, int dy) const { return Rect(x + dx, y + dy, w, h); }

  // Grows to a multiple of `align` in both axes. The MTK EPD driver is
  // happier with 8-pixel aligned update regions.
  Rect align_to(int align) const {
    if (align <= 1 || empty()) return *this;
    int nx = (x / align) * align;
    int ny = (y / align) * align;
    int nr = ((right() + align - 1) / align) * align;
    int nb = ((bottom() + align - 1) / align) * align;
    return Rect(nx, ny, nr - nx, nb - ny);
  }

  Rect clamped(int max_w, int max_h) const {
    return intersect(Rect(0, 0, max_w, max_h));
  }
};

}  // namespace ck
