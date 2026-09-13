#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/view.h"
#include "ui/widgets.h"

namespace ck {

// A scrolling list of rows, used for the library, the table of contents,
// bookmarks, notebooks and every settings page. Pages rather than scrolls:
// on e-ink a full-page step is both faster and easier to follow than
// smooth scrolling.
class ListView : public View {
 public:
  struct Item {
    ListRow row;
    int id = 0;
    bool separator = false;   // a non-selectable section heading
  };

  using SelectFn = std::function<void(int id)>;
  using LongPressFn = std::function<void(int id)>;

  ListView(std::string title, std::vector<Item> items, SelectFn on_select);

  void set_items(std::vector<Item> items, bool keep_position = false);
  void set_title(std::string title) { title_ = std::move(title); }
  void set_on_long_press(LongPressFn fn) { on_long_press_ = std::move(fn); }
  void set_on_back(std::function<void()> fn) { on_back_ = std::move(fn); }
  // A row of buttons along the bottom; ids are passed to on_select.
  void set_actions(std::vector<std::pair<std::string, int>> actions);
  void set_empty_message(std::string text) { empty_message_ = std::move(text); }
  void set_show_back(bool show) { show_back_ = show; }
  void set_status_line(std::string text) { status_line_ = std::move(text); }
  // Scrolls so that the row with this id is visible.
  void reveal(int id);
  void select_index(int index);

  void draw(Canvas& canvas, const Rect& bounds) override;
  bool handle(const InputEvent& event) override;
  Refresh refresh_hint() const override { return Refresh::Text; }
  std::string title() const override { return title_; }

  int item_count() const { return (int)items_.size(); }
  // Where the last tap landed inside the selected row, so a row can act as
  // a stepper (tap the left half to decrease, the right half to increase).
  int last_tap_x() const { return last_tap_x_; }
  int last_tap_row_x() const { return last_tap_row_x_; }
  int last_tap_row_width() const { return last_tap_row_width_; }

 protected:
  // Hook for subclasses that want to paint above the list.
  virtual int draw_header(Canvas& canvas, const Rect& area) { return 0; }
  virtual bool handle_extra(const InputEvent& event) { return false; }

  std::string title_;
  std::vector<Item> items_;
  SelectFn on_select_;
  LongPressFn on_long_press_;
  std::function<void()> on_back_;
  std::vector<std::pair<std::string, int>> actions_;
  std::string empty_message_ = "Nothing here";
  std::string status_line_;
  bool show_back_ = true;

  int first_visible_ = 0;
  int visible_rows_ = 0;
  HitList hits_;
  int last_tap_x_ = 0;
  int last_tap_row_x_ = 0;
  int last_tap_row_width_ = 0;
  static constexpr int kBackId = -1000;
};

}  // namespace ck
