#include "ui/widgets.h"

#include "ui/icons.h"

#include <algorithm>
#include <cmath>

#include "core/clock.h"
#include "core/str.h"
#include "platform/net.h"

namespace ck {

int HitList::hit(int x, int y) const {
  for (auto it = zones_.rbegin(); it != zones_.rend(); ++it) {
    if (it->rect.contains(x, y)) return it->id;
  }
  return -1;
}

const Rect* HitList::rect_for(int id) const {
  for (const Zone& z : zones_) {
    if (z.id == id) return &z.rect;
  }
  return nullptr;
}

TextStyle ui_style(int px, Color color, FontStyle style) {
  TextStyle st;
  st.family = theme().ui_font;
  st.px = px;
  st.color = color;
  st.style = style;
  return st;
}

void draw_battery_icon(Canvas& c, const Rect& r, int percent, bool charging) {
  const Theme& th = theme();
  int body_w = r.w - std::max(2, r.w / 8);
  Rect body(r.x, r.y, body_w, r.h);
  c.draw_rect(body, th.fg, 2);
  Rect cap(body.right() + 1, r.y + r.h / 4, r.w - body_w - 1, r.h / 2);
  c.fill_rect(cap, th.fg);
  if (percent >= 0) {
    Rect inner = body.inset(4);
    int w = (int)std::lround(inner.w * std::max(0, std::min(100, percent)) / 100.0);
    Color fill = percent <= 15 ? Color::rgb(0xD01414) : th.fg;
    c.fill_rect(Rect(inner.x, inner.y, w, inner.h), fill);
  }
  if (charging) {
    // A small bolt over the body.
    int cx = body.x + body.w / 2;
    int cy = body.y + body.h / 2;
    c.draw_line(cx + 3, body.y + 2, cx - 2, cy, th.accent, 2);
    c.draw_line(cx - 2, cy, cx + 2, body.bottom() - 2, th.accent, 2);
  }
}

int draw_top_bar(Canvas& c, const Rect& area, const std::string& title,
                 const StatusBarInfo& info, int left_inset) {
  const Theme& th = theme();
  int h = th.row_height;
  Rect bar(area.x, area.y, area.w, h);
  c.fill_rect(bar, th.bg);

  TextStyle title_st = ui_style(th.base_px, th.fg, FontStyle::Bold);
  int right_edge = bar.right() - th.padding;

  if (info.show_battery && info.battery_percent >= 0) {
    int icon_w = th.base_px + 10;
    int icon_h = th.base_px - 4;
    Rect icon(right_edge - icon_w, bar.y + (h - icon_h) / 2, icon_w, icon_h);
    draw_battery_icon(c, icon, info.battery_percent, info.charging);
    right_edge = icon.x - 10;
    if (!settings().hide_battery_percentage) {
      std::string pct = format("%d%%", info.battery_percent);
      TextStyle st = ui_style(th.small_px, th.muted);
      int w = text_width(pct, st);
      draw_text_in(c, Rect(right_edge - w, bar.y, w, h), pct, st, 1);
      right_edge -= w + 12;
    }
  }
  // Wi-Fi, when it is on: connected devices show the fan, a powered but
  // unconnected radio shows it struck through.
  {
    Net& net = Net::instance();
    if (net.state() != NetState::Off) {
      int size = th.base_px + 2;
      Rect icon(right_edge - size, bar.y + (h - size) / 2, size, size);
      draw_icon(c, icon, net.connected() ? Icon::Wifi : Icon::WifiOff,
                net.connected() ? th.fg : th.muted);
      right_edge = icon.x - 10;
    }
  }
  if (info.show_clock) {
    std::string clock = format_time(wall_seconds(), settings().clock_24h);
    TextStyle st = ui_style(th.small_px, th.muted);
    int w = text_width(clock, st);
    draw_text_in(c, Rect(right_edge - w, bar.y, w, h), clock, st, 1);
    right_edge -= w + 16;
  }
  if (!info.right.empty()) {
    TextStyle st = ui_style(th.small_px, th.muted);
    int w = text_width(info.right, st);
    draw_text_in(c, Rect(right_edge - w, bar.y, w, h), info.right, st, 1);
    right_edge -= w + 16;
  }

  Rect title_box(bar.x + th.padding + left_inset, bar.y,
                 right_edge - bar.x - th.padding - left_inset, h);
  if (!title.empty()) draw_text_in(c, title_box, title, title_st, -1);

  if (th.rules) c.fill_rect(Rect(bar.x, bar.bottom() - 1, bar.w, 1), th.faint);
  return h;
}

void draw_button(Canvas& c, const Rect& r, const std::string& label, ButtonStyle style,
                 bool selected) {
  const Theme& th = theme();
  Color bg = th.bg, fg = th.fg, border = th.border;
  switch (style) {
    case ButtonStyle::Primary:
      bg = selected ? th.fg : th.accent;
      fg = Color::gray(255);
      border = bg;
      break;
    case ButtonStyle::Danger:
      bg = selected ? Color::rgb(0x8B0000) : Color::rgb(0xD01414);
      fg = Color::gray(255);
      border = bg;
      break;
    case ButtonStyle::Ghost:
      bg = selected ? th.selection : th.bg;
      border = Color::transparent();
      break;
    case ButtonStyle::Normal:
    default:
      bg = selected ? th.selection : th.panel;
      break;
  }
  if (bg.a) c.fill_round_rect(r, th.radius, bg);
  if (border.a) c.draw_round_rect(r, th.radius, border, 2);
  draw_text_in(c, r.inset(th.padding / 2, 0), label, ui_style(th.base_px, fg), 0);
}

void draw_icon_button(Canvas& c, const Rect& r, Icon icon, bool selected) {
  const Theme& th = theme();
  if (selected) c.fill_round_rect(r, th.radius, th.selection);
  draw_icon(c, r, icon, th.fg);
}

void draw_list_row(Canvas& c, const Rect& r, const ListRow& row) {
  const Theme& th = theme();
  if (row.selected) c.fill_rect(r, th.selection);
  int x = r.x + th.padding;
  int avail = r.w - 2 * th.padding;

  if (row.swatch.a) {
    int size = th.base_px;
    c.fill_circle(x + size / 2, r.y + r.h / 2, size / 2, row.swatch);
    x += size + th.padding / 2;
    avail -= size + th.padding / 2;
  }
  if (row.is_folder) {
    // A simple folder mark: cheap to draw and unmistakable on e-ink.
    int size = th.base_px;
    Rect f(x, r.y + (r.h - size) / 2, size + size / 3, size);
    c.draw_rect(f, th.muted, 2);
    c.fill_rect(Rect(f.x + 3, f.y - 4, size / 2, 5), th.muted);
    x += f.w + th.padding / 2;
    avail -= f.w + th.padding / 2;
  }

  // Trailing text first, then the tick to its left: both used to be drawn
  // hard against the right edge and overlapped each other.
  int right_edge = r.right() - th.padding;
  if (!row.trailing.empty()) {
    TextStyle st = ui_style(th.small_px, th.muted);
    int w = text_width(row.trailing, st);
    draw_text_in(c, Rect(right_edge - w, r.y, w, r.h), row.trailing, st, 1);
    right_edge -= w + th.padding / 2;
    avail -= w + th.padding;
  }
  if (row.check) {
    int size = th.base_px;
    Rect tick(right_edge - size, r.y + (r.h - size) / 2, size, size);
    draw_icon(c, tick, Icon::Check, th.accent);
    right_edge = tick.x - th.padding / 2;
    avail -= size + th.padding / 2;
  }

  bool two_line = !row.subtitle.empty();
  TextStyle title_st =
      ui_style(th.base_px, th.fg, row.bold ? FontStyle::Bold : FontStyle::Regular);
  if (two_line) {
    int top_h = r.h * 55 / 100;
    draw_text_in(c, Rect(x, r.y + 2, avail, top_h), row.title, title_st, -1);
    TextStyle sub = ui_style(th.small_px, th.muted);
    draw_text_in(c, Rect(x, r.y + top_h - 4, avail, r.h - top_h), row.subtitle, sub, -1);
  } else {
    draw_text_in(c, Rect(x, r.y, avail, r.h), row.title, title_st, -1);
  }

  if (row.progress_percent >= 0) {
    int bar_h = 3;
    Rect bar(x, r.bottom() - bar_h - 6, std::max(40, avail / 3), bar_h);
    draw_progress_bar(c, bar, row.progress_percent / 100.0, bar_h);
  }
  if (th.rules) c.fill_rect(Rect(r.x + th.padding, r.bottom() - 1, r.w - 2 * th.padding, 1),
                            th.faint);
}

void draw_progress_bar(Canvas& c, const Rect& r, double fraction, int thickness) {
  const Theme& th = theme();
  int t = std::max(1, thickness);
  Rect track(r.x, r.y + (r.h - t) / 2, r.w, t);
  c.fill_rect(track, th.faint);
  int w = (int)std::lround(track.w * std::max(0.0, std::min(1.0, fraction)));
  c.fill_rect(Rect(track.x, track.y, w, t), th.fg);
}

void draw_slider(Canvas& c, const Rect& r, const std::string& label, int value, int min_value,
                 int max_value) {
  const Theme& th = theme();
  TextStyle st = ui_style(th.small_px, th.muted);
  int label_h = th.small_px + 8;
  if (!label.empty()) {
    draw_text_in(c, Rect(r.x, r.y, r.w - 80, label_h), label, st, -1);
    draw_text_in(c, Rect(r.right() - 80, r.y, 80, label_h), format("%d", value), st, 1);
  }
  int span = std::max(1, max_value - min_value);
  double frac = (double)(value - min_value) / (double)span;
  Rect track(r.x, r.y + label_h + (r.h - label_h) / 2 - 3, r.w, 6);
  c.fill_round_rect(track, 3, th.faint);
  c.fill_round_rect(Rect(track.x, track.y, (int)std::lround(track.w * frac), track.h), 3, th.fg);
  int knob_x = track.x + (int)std::lround(track.w * frac);
  c.fill_circle(knob_x, track.y + track.h / 2, th.base_px / 2, th.fg);
  c.fill_circle(knob_x, track.y + track.h / 2, th.base_px / 2 - 3, th.bg);
}

void draw_toggle(Canvas& c, const Rect& r, const std::string& label, bool on) {
  const Theme& th = theme();
  int track_w = th.base_px * 2 + 8;
  int track_h = th.base_px + 4;
  Rect track(r.right() - th.padding - track_w, r.y + (r.h - track_h) / 2, track_w, track_h);
  draw_text_in(c, Rect(r.x + th.padding, r.y, track.x - r.x - 2 * th.padding, r.h), label,
               ui_style(th.base_px, th.fg), -1);
  c.fill_round_rect(track, track_h / 2, on ? th.fg : th.faint);
  int knob_r = track_h / 2 - 3;
  int knob_x = on ? track.right() - knob_r - 3 : track.x + knob_r + 3;
  c.fill_circle(knob_x, track.y + track_h / 2, knob_r, th.bg);
  if (th.rules) c.fill_rect(Rect(r.x + th.padding, r.bottom() - 1, r.w - 2 * th.padding, 1),
                            th.faint);
}

void draw_panel(Canvas& c, const Rect& r, bool outlined) {
  const Theme& th = theme();
  c.fill_round_rect(r, th.radius, th.panel);
  if (outlined) c.draw_round_rect(r, th.radius, th.faint, 2);
}

Rect draw_dialog(Canvas& c, const Rect& screen, const std::string& title, int body_height) {
  const Theme& th = theme();
  // A light scrim: full black would cost a flashing refresh to clear.
  c.fill_rect_blend(screen, Color(0, 0, 0, 40));
  int w = std::min(screen.w - 4 * th.padding, (int)(screen.w * 0.86));
  int title_h = title.empty() ? 0 : th.row_height;
  int h = std::min(screen.h - 4 * th.padding, title_h + body_height + 2 * th.padding);
  Rect card((screen.w - w) / 2 + screen.x, (screen.h - h) / 2 + screen.y, w, h);
  c.fill_round_rect(card, th.radius, th.bg);
  c.draw_round_rect(card, th.radius, th.border, 2);
  if (title_h) {
    draw_text_in(c, Rect(card.x + th.padding, card.y, card.w - 2 * th.padding, title_h), title,
                 ui_style(th.base_px + 2, th.fg, FontStyle::Bold), -1);
    c.fill_rect(Rect(card.x + th.padding, card.y + title_h - 1, card.w - 2 * th.padding, 1),
                th.faint);
  }
  return Rect(card.x + th.padding, card.y + title_h + th.padding / 2, card.w - 2 * th.padding,
              card.h - title_h - th.padding);
}

void draw_centered_message(Canvas& c, const Rect& area, const std::string& text, int px) {
  const Theme& th = theme();
  TextStyle st = ui_style(px > 0 ? px : th.base_px, th.muted);
  std::vector<std::string> lines = wrap_text(text, st, area.w - 2 * th.padding);
  int line_h = text_height(st);
  int total = (int)lines.size() * line_h;
  int y = area.y + (area.h - total) / 2;
  for (const std::string& line : lines) {
    draw_text_in(c, Rect(area.x, y, area.w, line_h), line, st, 0);
    y += line_h;
  }
}

void draw_scroll_hint(Canvas& c, const Rect& area, int first_visible, int visible, int total) {
  if (total <= visible || visible <= 0) return;
  const Theme& th = theme();
  int track_w = 6;
  Rect track(area.right() - track_w - 4, area.y, track_w, area.h);
  c.fill_round_rect(track, track_w / 2, th.faint);
  double frac = (double)visible / (double)total;
  int thumb_h = std::max(24, (int)std::lround(area.h * frac));
  double pos = (double)first_visible / (double)std::max(1, total - visible);
  int y = area.y + (int)std::lround((area.h - thumb_h) * std::max(0.0, std::min(1.0, pos)));
  c.fill_round_rect(Rect(track.x, y, track_w, thumb_h), track_w / 2, th.muted);
}

void draw_cover(Canvas& c, const Rect& r, const Canvas* cover, const std::string& title,
                const std::string& author) {
  const Theme& th = theme();
  if (cover && cover->valid()) {
    // Fit inside the slot, preserving aspect ratio.
    double sx = (double)r.w / cover->width();
    double sy = (double)r.h / cover->height();
    double scale = std::min(sx, sy);
    int w = std::max(1, (int)std::lround(cover->width() * scale));
    int h = std::max(1, (int)std::lround(cover->height() * scale));
    Rect dst(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h);
    c.blit_scaled(*cover, dst);
    c.draw_rect(dst, th.faint, 1);
    return;
  }
  // Placeholder: a tinted card with the title set like a cover, which is
  // far more useful than a generic book icon when a library has no
  // embedded cover images.
  c.fill_round_rect(r, th.radius, th.panel);
  c.draw_round_rect(r, th.radius, th.faint, 2);
  c.fill_rect(Rect(r.x + 5, r.y + th.radius, 5, r.h - 2 * th.radius), th.accent);

  int inner_w = r.w - 30;
  int px = std::max(13, std::min(th.base_px, r.w / 9));
  TextStyle st = ui_style(px, th.fg, FontStyle::Bold);
  std::vector<std::string> lines = wrap_text(title, st, inner_w);
  int line_h = text_height(st);
  int author_h = author.empty() ? 0 : line_h;
  int max_lines = std::max(1, (r.h - 2 * th.padding - author_h) / line_h);
  if ((int)lines.size() > max_lines) {
    lines.resize(max_lines);
    lines.back() = ellipsize(lines.back() + "\xE2\x80\xA6", st, inner_w);
  }
  int block_h = (int)lines.size() * line_h + author_h;
  int y = r.y + std::max(th.padding / 2, (r.h - block_h) / 2);
  for (const std::string& line : lines) {
    draw_text_in(c, Rect(r.x + 18, y, inner_w, line_h), line, st, -1);
    y += line_h;
  }
  if (!author.empty()) {
    TextStyle as = ui_style(std::max(11, px - 4), th.muted);
    draw_text_in(c, Rect(r.x + 18, y + 2, inner_w, line_h), ellipsize(author, as, inner_w), as,
                 -1);
  }
}

}  // namespace ck
