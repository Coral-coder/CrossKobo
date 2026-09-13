// The home screen: continue reading, a grid of recent books, and the way
// in to everything else. The Dashboard theme swaps the grid for reading
// statistics, following CrossInk's idea of a stats-forward home.
#include <algorithm>
#include <cmath>
#include <memory>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/str.h"
#include "library/library.h"
#include "notes/notes.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "reader/state.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

enum HomeId {
  kIdContinue = 1,
  kIdLibrary,
  kIdNotebooks,
  kIdStats,
  kIdSettings,
  kIdRecentBase = 100,
};

class HomeScreen : public View {
 public:
  HomeScreen() {
    Recents::instance().load();
    Recents::instance().prune();
    Stats::instance().load();
  }

  void on_show() override {
    Recents::instance().load();
    Recents::instance().prune();
  }

  std::string title() const override { return "CrossKobo"; }
  Refresh refresh_hint() const override { return Refresh::Image; }
  int tick_ms() const override { return 60000; }
  bool on_tick() override { return settings().status_bar_clock; }

  void draw(Canvas& canvas, const Rect& bounds) override;
  bool handle(const InputEvent& event) override;

 private:
  int draw_continue_card(Canvas& canvas, const Rect& area);
  int draw_recent_grid(Canvas& canvas, const Rect& area);
  int draw_dashboard(Canvas& canvas, const Rect& area);
  void draw_nav(Canvas& canvas, const Rect& area);

  HitList hits_;
};

int HomeScreen::draw_continue_card(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  const auto& entries = Recents::instance().entries();
  if (entries.empty()) return 0;
  const RecentEntry& book = entries.front();

  int card_h = std::min(area.h * 3 / 10, th.row_height * 5 / 2);
  Rect card(area.x + th.padding, area.y, area.w - 2 * th.padding, card_h);
  draw_panel(canvas, card, th.rules);

  int cover_w = card_h * 2 / 3;
  Rect cover_rect(card.x + th.padding / 2, card.y + th.padding / 2, cover_w,
                  card_h - th.padding);
  const Canvas* cover = covers::thumbnail(book.path, cover_rect.w, cover_rect.h);
  draw_cover(canvas, cover_rect, cover, book.title, book.author);

  Rect text(cover_rect.right() + th.padding, card.y + th.padding / 2,
            card.right() - cover_rect.right() - 2 * th.padding, card_h - th.padding);
  draw_text_in(canvas, Rect(text.x, text.y, text.w, th.small_px + 6), "Continue reading",
               ui_style(th.small_px, th.muted), -1);
  int y = text.y + th.small_px + 10;
  TextStyle title_st = ui_style(th.base_px + 4, th.fg, FontStyle::Bold);
  std::vector<std::string> lines = wrap_text(book.title, title_st, text.w);
  for (size_t i = 0; i < lines.size() && i < 2; ++i) {
    draw_text_in(canvas, Rect(text.x, y, text.w, text_height(title_st)), lines[i], title_st, -1);
    y += text_height(title_st);
  }
  if (!book.author.empty()) {
    draw_text_in(canvas, Rect(text.x, y, text.w, th.small_px + 6),
                 ellipsize(book.author, ui_style(th.small_px, th.muted), text.w),
                 ui_style(th.small_px, th.muted), -1);
  }

  Rect bar(text.x, text.bottom() - th.small_px - 14, text.w, 4);
  draw_progress_bar(canvas, bar, book.progress, 4);
  std::string label = format("%d%% read", (int)std::lround(book.progress * 100));
  if (book.last_read) label += " \xC2\xB7 " + relative_time(book.last_read);
  draw_text_in(canvas, Rect(text.x, bar.bottom() + 2, text.w, th.small_px + 6), label,
               ui_style(th.small_px, th.muted), -1);

  hits_.add(card, kIdContinue);
  return card_h + th.padding;
}

int HomeScreen::draw_recent_grid(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  const auto& entries = Recents::instance().entries();
  if (entries.size() <= 1) {
    if (entries.empty()) {
      draw_centered_message(canvas, area,
                            "Your library is empty.\n\nConnect the device to a computer and "
                            "copy EPUB, TXT or CBZ files onto it.");
    }
    return area.h;
  }

  draw_text_in(canvas, Rect(area.x + th.padding, area.y, area.w, th.small_px + 8), "Recent",
               ui_style(th.small_px, th.muted, FontStyle::Bold), -1);
  Rect grid(area.x + th.padding, area.y + th.small_px + 12, area.w - 2 * th.padding,
            area.h - th.small_px - 12);

  // Three columns and up to three rows: the 3x3 recents grid CrossInk
  // uses. Cells keep a book-shaped 2:3 cover, and the grid shrinks the
  // cells rather than stretching them when space is tight.
  const int cols = 3;
  int gap = th.padding;
  int label_h = th.small_px + 8;
  int cell_w = (grid.w - gap * (cols - 1)) / cols;
  int natural_h = cell_w * 3 / 2 + label_h;
  int items = (int)entries.size() - 1;
  int wanted_rows = (items + cols - 1) / cols;
  int fit_rows = std::max(1, (grid.h + gap) / (natural_h + gap));
  int rows = std::max(1, std::min({3, wanted_rows, fit_rows}));
  int cell_h = natural_h;
  if (rows * (natural_h + gap) - gap > grid.h) {
    cell_h = (grid.h - gap * (rows - 1)) / rows;
    cell_w = std::min(cell_w, (cell_h - label_h) * 2 / 3);
  }
  int col_pitch = cell_w + gap;

  size_t index = 1;  // entry 0 is the continue card
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      if (index >= entries.size()) return grid.h;
      const RecentEntry& e = entries[index];
      Rect cell(grid.x + c * col_pitch, grid.y + r * (cell_h + gap), cell_w, cell_h);
      Rect cover_rect(cell.x, cell.y, cell.w, cell_h - label_h);
      const Canvas* cover = covers::thumbnail(e.path, cover_rect.w, cover_rect.h);
      draw_cover(canvas, cover_rect, cover, e.title, e.author);
      std::string label = e.finished ? "Read" : format("%d%%", (int)std::lround(e.progress * 100));
      draw_text_in(canvas, Rect(cell.x, cover_rect.bottom() + 2, cell.w, th.small_px + 6),
                   ellipsize(e.title, ui_style(th.small_px - 2, th.fg), cell.w - 30),
                   ui_style(th.small_px - 2, th.fg), -1);
      draw_text_in(canvas, Rect(cell.x, cover_rect.bottom() + 2, cell.w, th.small_px + 6), label,
                   ui_style(th.small_px - 2, th.muted), 1);
      hits_.add(cell, kIdRecentBase + (int)index);
      ++index;
    }
  }
  return grid.h;
}

int HomeScreen::draw_dashboard(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  const Stats& stats = Stats::instance();
  draw_text_in(canvas, Rect(area.x + th.padding, area.y, area.w, th.small_px + 8),
               "Reading statistics", ui_style(th.small_px, th.muted, FontStyle::Bold), -1);

  struct Tile {
    std::string label;
    std::string value;
  };
  std::vector<Tile> tiles = {
      {"Books finished", format("%d", stats.books_finished)},
      {"Total time", human_duration(stats.total_seconds)},
      {"Sessions", format("%d", stats.sessions)},
      {"Pages turned", format("%d", stats.pages_turned)},
      {"Average session", human_duration(stats.average_session())},
      {"Pages per minute", format("%.1f", stats.pages_per_minute())},
      {"Today", human_duration(stats.seconds_today())},
      {"Streak", format("%d day%s", stats.streak_days(), stats.streak_days() == 1 ? "" : "s")},
  };

  Rect grid(area.x + th.padding, area.y + th.small_px + 12, area.w - 2 * th.padding,
            area.h - th.small_px - 12);
  const int cols = 2;
  int gap = th.padding / 2;
  int cell_w = (grid.w - gap) / cols;
  int rows = ((int)tiles.size() + cols - 1) / cols;
  int cell_h = std::min(th.row_height * 3, (grid.h - gap * (rows - 1)) / std::max(1, rows));
  for (size_t i = 0; i < tiles.size(); ++i) {
    int r = (int)i / cols, c = (int)i % cols;
    Rect cell(grid.x + c * (cell_w + gap), grid.y + r * (cell_h + gap), cell_w, cell_h);
    if (cell.bottom() > grid.bottom()) break;
    draw_panel(canvas, cell, th.rules);
    draw_text_in(canvas, Rect(cell.x + th.padding / 2, cell.y + 6, cell.w - th.padding,
                              th.small_px + 4),
                 tiles[i].label, ui_style(th.small_px - 2, th.muted), -1);
    draw_text_in(canvas,
                 Rect(cell.x + th.padding / 2, cell.y + th.small_px + 6, cell.w - th.padding,
                      cell.h - th.small_px - 10),
                 tiles[i].value, ui_style(th.title_px - 4, th.fg, FontStyle::Bold), -1);
  }
  hits_.add(grid, kIdStats);
  return grid.h;
}

void HomeScreen::draw_nav(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  struct Nav {
    const char* label;
    int id;
  };
  const Nav navs[] = {{"Library", kIdLibrary},
                      {"Notebooks", kIdNotebooks},
                      {"Statistics", kIdStats},
                      {"Settings", kIdSettings}};
  int n = 4;
  int gap = th.padding / 2;
  int w = (area.w - 2 * th.padding - gap * (n - 1)) / n;
  for (int i = 0; i < n; ++i) {
    Rect button(area.x + th.padding + i * (w + gap), area.y, w, th.row_height);
    draw_button(canvas, button, navs[i].label, ButtonStyle::Normal);
    hits_.add(button, navs[i].id);
  }
}

void HomeScreen::draw(Canvas& canvas, const Rect& bounds) {
  const Theme& th = theme();
  hits_.clear();
  canvas.clear(th.bg);

  StatusBarInfo info;
  BatteryState battery = Power::instance().battery();
  info.battery_percent = battery.percent;
  info.charging = battery.charging;
  int top = draw_top_bar(canvas, bounds, "CrossKobo", info);

  int nav_h = th.row_height + th.padding;
  Rect body(bounds.x, bounds.y + top + th.padding / 2, bounds.w,
            bounds.h - top - nav_h - th.padding);

  int used = draw_continue_card(canvas, body);
  Rect rest(body.x, body.y + used, body.w, body.h - used);
  if (settings().theme == UiTheme::Dashboard) {
    draw_dashboard(canvas, rest);
  } else {
    draw_recent_grid(canvas, rest);
  }
  draw_nav(canvas, Rect(bounds.x, bounds.bottom() - nav_h, bounds.w, nav_h));
}

bool HomeScreen::handle(const InputEvent& event) {
  if (event.type == EventType::KeyDown) {
    if (event.key == Key::PageForward || event.key == Key::PageBack) {
      // The page buttons open the last book: the most common thing to want.
      const auto& entries = Recents::instance().entries();
      if (!entries.empty()) {
        open_book(entries.front().path);
        return true;
      }
      App::instance().push(make_library_screen());
      return true;
    }
    return false;
  }
  if (event.type == EventType::Swipe && event.swipe == SwipeDir::Left) {
    App::instance().push(make_library_screen());
    return true;
  }
  if (event.type != EventType::Tap) return false;

  int id = hits_.hit(event.x, event.y);
  const auto& entries = Recents::instance().entries();
  switch (id) {
    case kIdContinue:
      if (!entries.empty()) open_book(entries.front().path);
      return true;
    case kIdLibrary:
      App::instance().push(make_library_screen());
      return true;
    case kIdNotebooks:
      App::instance().push(make_notes_browser());
      return true;
    case kIdStats:
      App::instance().push(make_stats_screen());
      return true;
    case kIdSettings:
      App::instance().push(make_settings_screen());
      return true;
    default:
      if (id >= kIdRecentBase) {
        size_t index = (size_t)(id - kIdRecentBase);
        if (index < entries.size()) {
          open_book(entries[index].path);
          return true;
        }
      }
      return false;
  }
}

}  // namespace

ViewPtr make_home_screen() { return std::make_unique<HomeScreen>(); }

}  // namespace ck
