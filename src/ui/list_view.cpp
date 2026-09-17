#include "ui/list_view.h"

#include <algorithm>

#include "app/app.h"
#include "core/str.h"
#include "platform/power.h"

namespace ck {

ListView::ListView(std::string title, std::vector<Item> items, SelectFn on_select)
    : title_(std::move(title)), items_(std::move(items)), on_select_(std::move(on_select)) {}

void ListView::set_items(std::vector<Item> items, bool keep_position) {
  items_ = std::move(items);
  if (!keep_position) first_visible_ = 0;
  if (first_visible_ >= (int)items_.size()) first_visible_ = 0;
}

void ListView::set_actions(std::vector<std::pair<std::string, int>> actions) {
  actions_ = std::move(actions);
}

void ListView::reveal(int id) {
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].id != id) continue;
    if (visible_rows_ > 0) {
      // Put the target a third of the way down, which reads better than
      // pinning it to the top.
      first_visible_ = std::max(0, (int)i - visible_rows_ / 3);
    } else {
      first_visible_ = (int)i;
    }
    return;
  }
}

void ListView::select_index(int index) {
  if (index < 0 || index >= (int)items_.size()) return;
  if (on_select_) on_select_(items_[index].id);
}

void ListView::draw(Canvas& canvas, const Rect& bounds) {
  const Theme& th = theme();
  hits_.clear();

  StatusBarInfo info;
  BatteryState battery = Power::instance().battery();
  info.battery_percent = battery.percent;
  info.charging = battery.charging;
  int back_w = show_back_ ? th.row_height : 0;
  int top = draw_top_bar(canvas, bounds, title_, info, back_w);
  if (show_back_) {
    Rect back(bounds.x + th.padding / 2, bounds.y, th.row_height, th.row_height);
    draw_icon_button(canvas, back, Icon::Back, false);
    hits_.add(back, kBackId);
  }

  Rect area(bounds.x, bounds.y + top, bounds.w, bounds.h - top);
  int header_h = draw_header(canvas, area);
  area = Rect(area.x, area.y + header_h, area.w, area.h - header_h);

  int action_h = actions_.empty() ? 0 : th.row_height + th.padding;
  int status_h = status_line_.empty() ? 0 : th.small_px + th.padding;
  Rect list_area(area.x, area.y, area.w, area.h - action_h - status_h);

  if (items_.empty()) {
    draw_centered_message(canvas, list_area, empty_message_);
  } else {
    int row_h = th.row_height;
    visible_rows_ = std::max(1, list_area.h / row_h);
    first_visible_ = std::max(0, std::min(first_visible_,
                                          std::max(0, (int)items_.size() - visible_rows_)));
    int y = list_area.y;
    for (int i = first_visible_; i < (int)items_.size() && y + row_h <= list_area.bottom(); ++i) {
      Rect row(list_area.x, y, list_area.w, row_h);
      if (items_[i].separator) {
        if (th.gradients) {
          canvas.fill_rect_gradient(row, th.section_top, th.section_bottom);
          canvas.fill_gloss(row, 0, 80);
        } else {
          canvas.fill_rect(row, th.panel);
        }
        draw_text_in(canvas, row.inset(th.padding, 0), items_[i].row.title,
                     ui_style(th.small_px, th.section_text, FontStyle::Bold), -1);
      } else {
        draw_list_row(canvas, row, items_[i].row);
        hits_.add(row, items_[i].id);
      }
      y += row_h;
    }
    draw_scroll_hint(canvas, list_area, first_visible_, visible_rows_, (int)items_.size());
  }

  if (!status_line_.empty()) {
    Rect status(bounds.x + th.padding, list_area.bottom(), bounds.w - 2 * th.padding, status_h);
    draw_text_in(canvas, status, status_line_, ui_style(th.small_px, th.muted), -1);
  }

  if (!actions_.empty()) {
    int n = (int)actions_.size();
    int gap = th.padding / 2;
    int total_w = bounds.w - 2 * th.padding;
    int button_w = (total_w - gap * (n - 1)) / n;
    int y = bounds.bottom() - th.row_height - th.padding / 2;
    for (int i = 0; i < n; ++i) {
      Rect button(bounds.x + th.padding + i * (button_w + gap), y, button_w, th.row_height);
      draw_button(canvas, button, actions_[i].first,
                  i == n - 1 ? ButtonStyle::Primary : ButtonStyle::Normal);
      hits_.add(button, actions_[i].second);
    }
  }
}

bool ListView::handle(const InputEvent& event) {
  if (handle_extra(event)) return true;
  const int page_step = std::max(1, visible_rows_);

  switch (event.type) {
    case EventType::Tap: {
      int id = hits_.hit(event.x, event.y);
      if (id == kBackId) {
        if (on_back_) {
          on_back_();
        } else {
          App::instance().pop();
        }
        return true;
      }
      if (id != -1 && on_select_) {
        last_tap_x_ = event.x;
        if (const Rect* r = hits_.rect_for(id)) {
          last_tap_row_x_ = r->x;
          last_tap_row_width_ = r->w;
        }
        on_select_(id);
        return true;
      }
      return false;
    }
    case EventType::LongPress: {
      int id = hits_.hit(event.x, event.y);
      if (id != -1 && id != kBackId && on_long_press_) {
        on_long_press_(id);
        return true;
      }
      return false;
    }
    case EventType::Swipe:
      if (event.swipe == SwipeDir::Up) {
        first_visible_ += page_step;
        return true;
      }
      if (event.swipe == SwipeDir::Down) {
        first_visible_ = std::max(0, first_visible_ - page_step);
        return true;
      }
      if (event.swipe == SwipeDir::Right) {
        if (on_back_) {
          on_back_();
        } else {
          App::instance().pop();
        }
        return true;
      }
      return false;
    case EventType::KeyDown:
      if (event.key == Key::PageForward) {
        first_visible_ += page_step;
        return true;
      }
      if (event.key == Key::PageBack) {
        first_visible_ = std::max(0, first_visible_ - page_step);
        return true;
      }
      if (event.key == Key::Home) {
        App::instance().pop_to_root();
        return true;
      }
      return false;
    default:
      return false;
  }
}

}  // namespace ck
