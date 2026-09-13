#pragma once
#include <functional>
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "gfx/font.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace ck {

// Immediate-mode drawing helpers plus a hit-test list. Screens rebuild both
// on every draw, which suits e-ink: there is no animation to preserve and
// no retained widget tree to keep in sync.
class HitList {
 public:
  void clear() { zones_.clear(); }
  void add(const Rect& r, int id) { zones_.push_back({r, id}); }
  // Returns the id of the topmost zone containing the point, or -1.
  int hit(int x, int y) const;
  const Rect* rect_for(int id) const;
  bool empty() const { return zones_.empty(); }

 private:
  struct Zone {
    Rect rect;
    int id;
  };
  std::vector<Zone> zones_;
};

TextStyle ui_style(int px, Color color, FontStyle style = FontStyle::Regular);

struct StatusBarInfo {
  std::string left;
  std::string centre;
  std::string right;
  int battery_percent = -1;
  bool charging = false;
  bool show_clock = true;
  bool show_battery = true;
};

// Top bar used by the shell screens. Returns the height it consumed.
// `left_inset` reserves room at the left for a back button drawn by the
// caller, so the title never overlaps it.
int draw_top_bar(Canvas& c, const Rect& area, const std::string& title,
                 const StatusBarInfo& info, int left_inset = 0);
void draw_battery_icon(Canvas& c, const Rect& r, int percent, bool charging);

enum class ButtonStyle { Normal, Primary, Ghost, Danger };
void draw_button(Canvas& c, const Rect& r, const std::string& label, ButtonStyle style,
                 bool selected = false);
void draw_icon_button(Canvas& c, const Rect& r, Icon icon, bool selected = false);

struct ListRow {
  std::string title;
  std::string subtitle;
  std::string trailing;
  bool selected = false;
  bool bold = false;
  bool is_folder = false;
  int progress_percent = -1;   // draws a slim progress bar when >= 0
  Color swatch = Color::transparent();
  bool check = false;          // draws a tick at the trailing edge
};
void draw_list_row(Canvas& c, const Rect& r, const ListRow& row);

void draw_progress_bar(Canvas& c, const Rect& r, double fraction, int thickness);
void draw_slider(Canvas& c, const Rect& r, const std::string& label, int value, int min_value,
                 int max_value);
void draw_toggle(Canvas& c, const Rect& r, const std::string& label, bool on);
void draw_panel(Canvas& c, const Rect& r, bool outlined = true);
// Modal frame: dims the background, draws a titled card, returns the body.
Rect draw_dialog(Canvas& c, const Rect& screen, const std::string& title, int body_height);
void draw_centered_message(Canvas& c, const Rect& area, const std::string& text, int px = 0);
void draw_scroll_hint(Canvas& c, const Rect& area, int first_visible, int visible, int total);
// A book cover, or a generated placeholder when the book has none.
void draw_cover(Canvas& c, const Rect& r, const Canvas* cover, const std::string& title,
                const std::string& author);

}  // namespace ck
