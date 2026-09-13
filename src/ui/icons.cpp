#include "ui/icons.h"

#include <algorithm>
#include <cmath>

namespace ck {
namespace {

// All icons are drawn inside a square inset of the box so different icons
// line up optically when they sit next to each other in a toolbar.
Rect icon_box(const Rect& box) {
  int size = std::min(box.w, box.h);
  size = std::max(8, (int)(size * 0.62));
  return Rect(box.x + (box.w - size) / 2, box.y + (box.h - size) / 2, size, size);
}

void chevron(Canvas& c, const Rect& r, Color color, int weight, int dir) {
  // dir: 0 left, 1 right, 2 up, 3 down
  int cx = r.center().x, cy = r.center().y;
  int arm = r.w / 3;
  switch (dir) {
    case 0:
      c.draw_line(cx + arm / 2, cy - arm, cx - arm / 2, cy, color, weight);
      c.draw_line(cx - arm / 2, cy, cx + arm / 2, cy + arm, color, weight);
      break;
    case 1:
      c.draw_line(cx - arm / 2, cy - arm, cx + arm / 2, cy, color, weight);
      c.draw_line(cx + arm / 2, cy, cx - arm / 2, cy + arm, color, weight);
      break;
    case 2:
      c.draw_line(cx - arm, cy + arm / 2, cx, cy - arm / 2, color, weight);
      c.draw_line(cx, cy - arm / 2, cx + arm, cy + arm / 2, color, weight);
      break;
    default:
      c.draw_line(cx - arm, cy - arm / 2, cx, cy + arm / 2, color, weight);
      c.draw_line(cx, cy + arm / 2, cx + arm, cy - arm / 2, color, weight);
      break;
  }
}

void arc(Canvas& c, int cx, int cy, int radius, float start_deg, float end_deg, Color color,
         int weight) {
  const int steps = std::max(8, radius);
  float prev_x = 0, prev_y = 0;
  for (int i = 0; i <= steps; ++i) {
    float t = (float)i / (float)steps;
    float angle = (start_deg + (end_deg - start_deg) * t) * 3.14159265f / 180.0f;
    float x = cx + std::cos(angle) * radius;
    float y = cy + std::sin(angle) * radius;
    if (i > 0) c.draw_thick_line_aa(prev_x, prev_y, x, y, (float)weight, (float)weight, color);
    prev_x = x;
    prev_y = y;
  }
}

}  // namespace

void draw_icon(Canvas& canvas, const Rect& box, Icon icon, Color color, int weight) {
  Rect r = icon_box(box);
  if (weight <= 0) weight = std::max(2, r.w / 9);
  int cx = r.center().x, cy = r.center().y;

  switch (icon) {
    case Icon::Back: chevron(canvas, r, color, weight, 0); break;
    case Icon::Forward: chevron(canvas, r, color, weight, 1); break;
    case Icon::ChevronUp: chevron(canvas, r, color, weight, 2); break;
    case Icon::ChevronDown: chevron(canvas, r, color, weight, 3); break;

    case Icon::Home: {
      int half = r.w / 2;
      canvas.draw_line(cx - half, cy, cx, cy - half, color, weight);
      canvas.draw_line(cx, cy - half, cx + half, cy, color, weight);
      canvas.draw_rect(Rect(cx - half * 2 / 3, cy, half * 4 / 3, half), color, weight);
      break;
    }
    case Icon::Menu: {
      int dot = std::max(2, weight);
      for (int i = -1; i <= 1; ++i) {
        canvas.fill_circle(cx, cy + i * (r.h / 3), dot, color);
      }
      break;
    }
    case Icon::Plus:
      canvas.fill_rect(Rect(cx - r.w / 2, cy - weight / 2, r.w, std::max(1, weight)), color);
      canvas.fill_rect(Rect(cx - weight / 2, cy - r.h / 2, std::max(1, weight), r.h), color);
      break;
    case Icon::Close:
      canvas.draw_line(r.x, r.y, r.right(), r.bottom(), color, weight);
      canvas.draw_line(r.right(), r.y, r.x, r.bottom(), color, weight);
      break;
    case Icon::Check:
      canvas.draw_line(r.x, cy, cx - r.w / 8, r.bottom() - weight, color, weight);
      canvas.draw_line(cx - r.w / 8, r.bottom() - weight, r.right(), r.y, color, weight);
      break;
    case Icon::Star: {
      // A five-pointed star, filled: reads clearly even when tiny.
      const int points = 5;
      float outer = (float)r.w / 2.0f;
      float inner = outer * 0.45f;
      float prev_x = 0, prev_y = 0, first_x = 0, first_y = 0;
      for (int i = 0; i <= points * 2; ++i) {
        float radius = (i % 2 == 0) ? outer : inner;
        float angle = (-90.0f + i * 36.0f) * 3.14159265f / 180.0f;
        float x = cx + std::cos(angle) * radius;
        float y = cy + std::sin(angle) * radius;
        if (i == 0) {
          first_x = x;
          first_y = y;
        } else {
          canvas.draw_thick_line_aa(prev_x, prev_y, x, y, (float)weight, (float)weight, color);
        }
        prev_x = x;
        prev_y = y;
      }
      canvas.draw_thick_line_aa(prev_x, prev_y, first_x, first_y, (float)weight, (float)weight,
                                color);
      break;
    }
    case Icon::Pen: {
      // A nib: a long body tapering to a point at the lower left.
      canvas.draw_thick_line_aa((float)r.right(), (float)r.y, (float)(r.x + r.w / 4),
                                (float)(r.bottom() - r.h / 4), (float)weight * 2.0f,
                                (float)weight * 2.0f, color);
      canvas.draw_thick_line_aa((float)(r.x + r.w / 4), (float)(r.bottom() - r.h / 4),
                                (float)r.x, (float)r.bottom(), (float)weight * 1.6f, 1.0f,
                                color);
      break;
    }
    case Icon::Highlighter: {
      canvas.fill_rect(Rect(r.x, cy - r.h / 4, r.w, r.h / 3), color.with_alpha(110));
      canvas.fill_rect(Rect(r.x, cy + r.h / 4, r.w, std::max(2, weight)), color);
      break;
    }
    case Icon::Eraser: {
      // A slab, tilted, with a wiped baseline.
      Rect slab(r.x, r.y + r.h / 4, r.w * 3 / 4, r.h / 2);
      canvas.draw_rect(slab, color, weight);
      canvas.fill_rect(Rect(r.x, r.bottom() - weight, r.w, std::max(1, weight)), color);
      break;
    }
    case Icon::Undo:
    case Icon::Redo: {
      bool redo = icon == Icon::Redo;
      int radius = r.w / 2 - weight;
      arc(canvas, cx, cy + radius / 3, radius, redo ? 200.0f : 340.0f, redo ? 340.0f : 200.0f,
          color, weight);
      // Arrow head at the end of the sweep.
      int hx = redo ? cx + radius : cx - radius;
      int hy = cy + radius / 3;
      int s = std::max(3, r.w / 5);
      canvas.draw_line(hx, hy, hx + (redo ? -s : s), hy - s, color, weight);
      canvas.draw_line(hx, hy, hx + (redo ? -s : s), hy + s, color, weight);
      break;
    }
    case Icon::Search: {
      int radius = r.w * 2 / 5;
      arc(canvas, cx - radius / 3, cy - radius / 3, radius, 0.0f, 360.0f, color, weight);
      canvas.draw_line(cx + radius / 3, cy + radius / 3, r.right(), r.bottom(), color, weight);
      break;
    }
    case Icon::Sort: {
      for (int i = 0; i < 3; ++i) {
        int w = r.w - i * r.w / 3;
        canvas.fill_rect(Rect(r.x, r.y + i * r.h / 3, w, std::max(1, weight)), color);
      }
      break;
    }
    case Icon::Light: {
      arc(canvas, cx, cy, r.w / 4, 0.0f, 360.0f, color, weight);
      for (int i = 0; i < 8; ++i) {
        float angle = i * 45.0f * 3.14159265f / 180.0f;
        float x0 = cx + std::cos(angle) * (r.w / 3.0f);
        float y0 = cy + std::sin(angle) * (r.w / 3.0f);
        float x1 = cx + std::cos(angle) * (r.w / 2.0f);
        float y1 = cy + std::sin(angle) * (r.w / 2.0f);
        canvas.draw_thick_line_aa(x0, y0, x1, y1, (float)weight, (float)weight, color);
      }
      break;
    }
    case Icon::Note: {
      canvas.draw_rect(r, color, weight);
      for (int i = 1; i <= 3; ++i) {
        canvas.fill_rect(Rect(r.x + r.w / 6, r.y + i * r.h / 4, r.w * 2 / 3, std::max(1, weight - 1)),
                         color);
      }
      break;
    }
    case Icon::Book: {
      canvas.draw_rect(Rect(r.x, r.y, r.w / 2, r.h), color, weight);
      canvas.draw_rect(Rect(cx, r.y, r.w / 2, r.h), color, weight);
      break;
    }
    case Icon::Settings: {
      arc(canvas, cx, cy, r.w / 3, 0.0f, 360.0f, color, weight);
      for (int i = 0; i < 6; ++i) {
        float angle = i * 60.0f * 3.14159265f / 180.0f;
        float x0 = cx + std::cos(angle) * (r.w / 3.0f);
        float y0 = cy + std::sin(angle) * (r.w / 3.0f);
        float x1 = cx + std::cos(angle) * (r.w / 2.0f);
        float y1 = cy + std::sin(angle) * (r.w / 2.0f);
        canvas.draw_thick_line_aa(x0, y0, x1, y1, (float)weight * 1.4f, (float)weight * 1.4f,
                                  color);
      }
      break;
    }
    case Icon::Shift: {
      int half = r.w / 2;
      canvas.draw_line(cx - half, cy, cx, cy - half, color, weight);
      canvas.draw_line(cx, cy - half, cx + half, cy, color, weight);
      canvas.draw_rect(Rect(cx - half / 2, cy, half, half), color, weight);
      break;
    }
    case Icon::Backspace: {
      canvas.draw_line(r.x, cy, r.x + r.w / 3, r.y, color, weight);
      canvas.draw_line(r.x, cy, r.x + r.w / 3, r.bottom(), color, weight);
      canvas.fill_rect(Rect(r.x + r.w / 3, r.y, r.w * 2 / 3, r.h), Color::transparent());
      canvas.draw_rect(Rect(r.x + r.w / 3, r.y, r.w * 2 / 3, r.h), color, weight);
      canvas.draw_line(r.x + r.w / 2, cy - r.h / 5, r.x + r.w * 5 / 6, cy + r.h / 5, color,
                       std::max(1, weight - 1));
      canvas.draw_line(r.x + r.w * 5 / 6, cy - r.h / 5, r.x + r.w / 2, cy + r.h / 5, color,
                       std::max(1, weight - 1));
      break;
    }
  }
}

}  // namespace ck
