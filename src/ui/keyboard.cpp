#include "ui/keyboard.h"

#include <algorithm>

#include "app/app.h"
#include "core/str.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace ck {
namespace {

const char* kRowsLower[4] = {"qwertyuiop", "asdfghjkl", "zxcvbnm", ""};
const char* kRowsUpper[4] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", ""};
const char* kRowsSymbols[4] = {"1234567890", "-_/:;()&@\"", ".,?!'#%+*=", ""};

constexpr int kIdShift = -10;
constexpr int kIdSymbols = -11;
constexpr int kIdBackspace = -12;
constexpr int kIdSpace = -13;
constexpr int kIdDone = -14;
constexpr int kIdCancel = -15;
constexpr int kIdClear = -16;

}  // namespace

KeyboardView::KeyboardView(std::string title, std::string initial, DoneFn on_done)
    : title_(std::move(title)), text_(std::move(initial)), on_done_(std::move(on_done)) {}

void KeyboardView::draw(Canvas& canvas, const Rect& bounds) {
  const Theme& th = theme();
  hits_.clear();
  keys_.clear();
  canvas.clear(th.bg);

  StatusBarInfo info;
  int top = draw_top_bar(canvas, bounds, title_, info);

  // Text field.
  Rect field(bounds.x + th.padding, bounds.y + top + th.padding, bounds.w - 2 * th.padding,
             th.row_height);
  canvas.fill_round_rect(field, th.radius, th.panel);
  canvas.draw_round_rect(field, th.radius, th.border, 2);
  std::string shown = text_.empty() ? "" : text_;
  draw_text_in(canvas, field.inset(th.padding, 0), shown + "\xE2\x96\x8F",
               ui_style(th.base_px, th.fg), -1);
  Rect clear(field.right() - th.row_height, field.y, th.row_height, th.row_height);
  if (!text_.empty()) {
    draw_icon(canvas, clear, Icon::Close, th.muted);
    hits_.add(clear, kIdClear);
    keys_.push_back("");
  }

  // Keyboard occupies the bottom half or so.
  int rows = 4;
  int key_h = std::min(th.row_height + 12, (bounds.h - field.bottom() - 3 * th.padding) / rows);
  int kb_h = key_h * rows;
  int kb_top = bounds.bottom() - kb_h - th.padding;
  const char** layout = symbols_ ? kRowsSymbols : (shift_ ? kRowsUpper : kRowsLower);

  for (int r = 0; r < 3; ++r) {
    std::string row = layout[r];
    if (row.empty()) continue;
    // Count code points rather than bytes so an accented layout still fits.
    std::vector<std::string> chars;
    size_t i = 0;
    while (i < row.size()) {
      size_t start = i;
      utf8_next(row, i);
      chars.push_back(row.substr(start, i - start));
    }
    int n = (int)chars.size();
    int key_w = (bounds.w - 2 * th.padding) / std::max(1, n);
    int x = bounds.x + th.padding + ((bounds.w - 2 * th.padding) - key_w * n) / 2;
    int y = kb_top + r * key_h;
    for (int c = 0; c < n; ++c) {
      Rect key(x + c * key_w + 2, y + 2, key_w - 4, key_h - 4);
      canvas.fill_round_rect(key, th.radius, th.panel);
      canvas.draw_round_rect(key, th.radius, th.faint, 1);
      draw_text_in(canvas, key, chars[c], ui_style(th.base_px, th.fg), 0);
      hits_.add(key, (int)keys_.size());
      keys_.push_back(chars[c]);
    }
  }

  // Bottom row: shift, symbols, space, backspace, done.
  int y = kb_top + 3 * key_h;
  int unit = (bounds.w - 2 * th.padding) / 10;
  int x = bounds.x + th.padding;
  auto special = [&](int id, const std::string& label, int units, ButtonStyle style,
                     Icon* icon = nullptr) {
    Rect key(x + 2, y + 2, unit * units - 4, key_h - 4);
    draw_button(canvas, key, icon ? "" : label, style);
    if (icon) {
      draw_icon(canvas, key, *icon,
                style == ButtonStyle::Primary ? Color::gray(255) : theme().fg);
    }
    hits_.add(key, id);
    x += unit * units;
  };
  Icon shift_icon = Icon::Shift;
  Icon back_icon = Icon::Backspace;
  special(kIdShift, "", 1, shift_ ? ButtonStyle::Primary : ButtonStyle::Normal, &shift_icon);
  special(kIdSymbols, symbols_ ? "abc" : "?123", 2,
          symbols_ ? ButtonStyle::Primary : ButtonStyle::Normal);
  special(kIdSpace, "space", 3, ButtonStyle::Normal);
  special(kIdBackspace, "", 2, ButtonStyle::Normal, &back_icon);
  special(kIdDone, "Done", 2, ButtonStyle::Primary);

  Rect cancel(bounds.x + th.padding, field.bottom() + th.padding / 2, bounds.w / 3,
              th.row_height);
  draw_button(canvas, cancel, "Cancel", ButtonStyle::Ghost);
  hits_.add(cancel, kIdCancel);
}

void KeyboardView::press(const std::string& key) {
  text_ += key;
  if (shift_) shift_ = false;
}

bool KeyboardView::handle(const InputEvent& event) {
  if (event.type == EventType::KeyDown && event.key == Key::PageBack) {
    if (!text_.empty()) text_.erase(utf8_prev_index(text_, text_.size()));
    return true;
  }
  if (event.type != EventType::Tap) return false;
  int id = hits_.hit(event.x, event.y);
  if (id == -1) return false;

  switch (id) {
    case kIdShift:
      shift_ = !shift_;
      return true;
    case kIdSymbols:
      symbols_ = !symbols_;
      shift_ = false;
      return true;
    case kIdSpace:
      text_ += ' ';
      return true;
    case kIdBackspace:
      if (!text_.empty()) text_.erase(utf8_prev_index(text_, text_.size()));
      return true;
    case kIdClear:
      text_.clear();
      return true;
    case kIdDone: {
      DoneFn done = on_done_;
      std::string text = text_;
      App::instance().pop();
      if (done) done(text, true);
      return true;
    }
    case kIdCancel: {
      DoneFn done = on_done_;
      App::instance().pop();
      if (done) done("", false);
      return true;
    }
    default:
      if (id >= 0 && id < (int)keys_.size()) {
        press(keys_[(size_t)id]);
        return true;
      }
      return false;
  }
}

}  // namespace ck
