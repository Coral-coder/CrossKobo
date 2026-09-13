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
#include "core/paths.h"
#include "core/str.h"
#include "library/library.h"
#include "notes/notes.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "reader/state.h"
#include "ui/icons.h"
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
    refresh_shelf();
  }

  void on_show() override {
    Recents::instance().load();
    Recents::instance().prune();
    refresh_shelf();
  }

  std::string title() const override { return "CrossKobo"; }
  Refresh refresh_hint() const override { return Refresh::Image; }
  int tick_ms() const override { return 60000; }
  bool on_tick() override { return settings().status_bar_clock; }

  void draw(Canvas& canvas, const Rect& bounds) override;
  bool handle(const InputEvent& event) override;

 private:
  // What the grid shows: reading history first, then anything else in the
  // library, newest first. A fresh install has no history at all, and
  // saying "your library is empty" in that case was simply wrong.
  void refresh_shelf();

  int draw_continue_card(Canvas& canvas, const Rect& area);
  int draw_recent_grid(Canvas& canvas, const Rect& area);
  // A row of small glass tiles pinned above the navigation buttons.
  int draw_glance(Canvas& canvas, const Rect& area);
  int glance_height() const;
  int draw_dashboard(Canvas& canvas, const Rect& area);
  void draw_nav(Canvas& canvas, const Rect& area);

  struct ShelfEntry {
    std::string path;
    std::string title;
    std::string author;
    double progress = -1.0;   // -1 = never opened
    bool finished = false;
  };

  std::vector<ShelfEntry> shelf_;
  bool has_history_ = false;
  bool library_empty_ = true;
  HitList hits_;
};

void HomeScreen::refresh_shelf() {
  shelf_.clear();
  const std::vector<RecentEntry>& recents = Recents::instance().entries();
  has_history_ = !recents.empty();
  for (const RecentEntry& e : recents) {
    ShelfEntry entry;
    entry.path = e.path;
    entry.title = e.title;
    entry.author = e.author;
    entry.progress = e.progress;
    entry.finished = e.finished;
    shelf_.push_back(std::move(entry));
  }

  // Top up from the library so the grid is useful before anything has been
  // read. Scanned once per visit to this screen, not per frame.
  const size_t kWanted = 10;
  std::vector<LibraryEntry> found = scan_library(paths().onboard, false);
  std::vector<LibraryEntry> books;
  for (const LibraryEntry& e : found) {
    if (!e.is_dir) books.push_back(e);
  }
  // One level down too: most people keep books in folders.
  for (const LibraryEntry& e : found) {
    if (!e.is_dir || books.size() >= kWanted * 2) continue;
    for (const LibraryEntry& nested : scan_library(e.path, false)) {
      if (!nested.is_dir) books.push_back(nested);
    }
  }
  library_empty_ = books.empty() && !has_history_;
  std::sort(books.begin(), books.end(),
            [](const LibraryEntry& a, const LibraryEntry& b) { return a.mtime > b.mtime; });
  for (const LibraryEntry& book : books) {
    if (shelf_.size() >= kWanted) break;
    bool already = std::any_of(shelf_.begin(), shelf_.end(), [&](const ShelfEntry& e) {
      return e.path == book.path;
    });
    if (already) continue;
    ShelfEntry entry;
    entry.path = book.path;
    entry.title = book.name;
    entry.author = book.author;
    entry.progress = book.progress;
    entry.finished = book.finished;
    shelf_.push_back(std::move(entry));
  }
}

int HomeScreen::draw_continue_card(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  // Only offer to continue something actually being read.
  if (!has_history_ || shelf_.empty()) return 0;
  const ShelfEntry& book = shelf_.front();

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
  draw_progress_bar(canvas, bar, std::max(0.0, book.progress), 4);
  std::string label = format("%d%% read", (int)std::lround(std::max(0.0, book.progress) * 100));
  draw_text_in(canvas, Rect(text.x, bar.bottom() + 2, text.w, th.small_px + 6), label,
               ui_style(th.small_px, th.muted), -1);

  hits_.add(card, kIdContinue);
  return card_h + th.padding;
}

int HomeScreen::draw_recent_grid(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  // The continue card already shows the first entry when there is history.
  size_t first = has_history_ ? 1 : 0;
  if (shelf_.size() <= first) {
    draw_centered_message(
        canvas, area,
        library_empty_
            ? "No books yet.\n\nConnect the device to a computer and copy EPUB, TXT or CBZ "
              "files onto the drive, or send them over Wi-Fi from Settings."
            : "Nothing opened yet.\n\nTap Library to pick something.");
    return area.h;
  }

  draw_text_in(canvas, Rect(area.x + th.padding, area.y, area.w, th.small_px + 8),
               has_history_ ? "Recent" : "In your library",
               ui_style(th.small_px, th.muted, FontStyle::Bold), -1);
  Rect grid(area.x + th.padding, area.y + th.small_px + 12, area.w - 2 * th.padding,
            area.h - th.small_px - 12);

  // Three columns and up to three rows: the 3x3 grid CrossInk uses. Cells
  // keep a book-shaped 2:3 cover, and the grid shrinks the cells rather
  // than stretching them when space is tight.
  const int cols = 3;
  int gap = th.padding;
  int label_h = th.small_px + 8;
  int cell_w = (grid.w - gap * (cols - 1)) / cols;
  int natural_h = cell_w * 3 / 2 + label_h;
  int items = (int)(shelf_.size() - first);
  int wanted_rows = (items + cols - 1) / cols;
  int fit_rows = std::max(1, (grid.h + gap) / (natural_h + gap));
  int rows = std::max(1, std::min({3, wanted_rows, fit_rows}));
  int cell_h = natural_h;
  if (rows * (natural_h + gap) - gap > grid.h) {
    cell_h = (grid.h - gap * (rows - 1)) / rows;
    cell_w = std::min(cell_w, (cell_h - label_h) * 2 / 3);
  }
  // A single row of full-height covers looks stranded on a tall page, so
  // cap the cover at a third of the space and let the glance strip use
  // what is left. With a full shelf the rows fill the page anyway.
  if (rows == 1 && cell_h > grid.h / 2) {
    cell_h = std::max(th.row_height * 3, grid.h / 2);
    cell_w = std::min(cell_w, (cell_h - label_h) * 2 / 3);
  }
  int col_pitch = cell_w + gap;
  // Centre the columns: shrunken cells would otherwise huddle on the left.
  int shown_cols = std::min(cols, std::max(1, items));
  int grid_x = grid.x + std::max(0, (grid.w - (shown_cols * col_pitch - gap)) / 2);
  int header_h = th.small_px + 12;
  int used_h = header_h + rows * (cell_h + gap) - gap;

  size_t index = first;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      if (index >= shelf_.size()) return used_h;
      const ShelfEntry& e = shelf_[index];
      Rect cell(grid_x + c * col_pitch, grid.y + r * (cell_h + gap), cell_w, cell_h);
      Rect cover_rect(cell.x, cell.y, cell.w, cell_h - label_h);
      const Canvas* cover = covers::thumbnail(e.path, cover_rect.w, cover_rect.h);
      draw_cover(canvas, cover_rect, cover, e.title, e.author);
      std::string label = e.finished
                              ? "Read"
                              : (e.progress > 0.0
                                     ? format("%d%%", (int)std::lround(e.progress * 100))
                                     : "New");
      draw_text_in(canvas, Rect(cell.x, cover_rect.bottom() + 2, cell.w, th.small_px + 6),
                   ellipsize(e.title, ui_style(th.small_px - 2, th.fg), cell.w - 30),
                   ui_style(th.small_px - 2, th.fg), -1);
      draw_text_in(canvas, Rect(cell.x, cover_rect.bottom() + 2, cell.w, th.small_px + 6), label,
                   ui_style(th.small_px - 2, th.muted), 1);
      hits_.add(cell, kIdRecentBase + (int)index);
      ++index;
    }
  }
  return used_h;
}

int HomeScreen::glance_height() const {
  const Theme& th = theme();
  return th.small_px + 12 + th.row_height + th.small_px + th.padding / 2;
}

int HomeScreen::draw_glance(Canvas& canvas, const Rect& area) {
  const Theme& th = theme();
  int tile_h = th.row_height + th.small_px + th.padding / 2;
  if (area.h < glance_height()) return 0;

  const Stats& stats = Stats::instance();
  int notebooks = (int)Notebook::list().size();
  struct Tile {
    std::string label;
    std::string value;
    int id;
  };
  Tile tiles[4] = {
      {"Read today", human_duration(stats.seconds_today()), kIdStats},
      {"Streak", format("%d day%s", stats.streak_days(), stats.streak_days() == 1 ? "" : "s"),
       kIdStats},
      {"Finished", format("%d book%s", stats.books_finished,
                          stats.books_finished == 1 ? "" : "s"),
       kIdStats},
      {"Notebooks", format("%d", notebooks), kIdNotebooks},
  };

  draw_text_in(canvas, Rect(area.x + th.padding, area.y, area.w, th.small_px + 8), "At a glance",
               ui_style(th.small_px, th.muted, FontStyle::Bold), -1);
  int gap = th.padding / 2;
  int strip_w = area.w - 2 * th.padding;
  int tile_w = (strip_w - gap * 3) / 4;
  int y = area.y + th.small_px + 12;
  for (int i = 0; i < 4; ++i) {
    Rect cell(area.x + th.padding + i * (tile_w + gap), y, tile_w, tile_h);
    if (th.gradients) {
      Color hue = th.accents4[i % 4];
      if (th.shadows) canvas.draw_soft_shadow(cell, th.radius, 3, 26);
      canvas.fill_round_rect_gradient(cell, th.radius, wash(hue, 0.88f), wash(hue, 0.55f));
      canvas.fill_gloss(cell, th.radius, 90);
      canvas.draw_round_rect(cell, th.radius, wash(hue, 0.3f), 2);
    } else {
      draw_panel(canvas, cell, th.rules);
    }
    draw_text_in(canvas,
                 Rect(cell.x + th.padding / 2, cell.y + 6, cell.w - th.padding, th.small_px + 4),
                 tiles[i].label, ui_style(th.small_px - 2, th.muted), -1);
    draw_text_in(canvas,
                 Rect(cell.x + th.padding / 2, cell.y + th.small_px + 6, cell.w - th.padding,
                      cell.h - th.small_px - 10),
                 tiles[i].value, ui_style(th.base_px + 2, th.fg, FontStyle::Bold), -1);
    hits_.add(cell, tiles[i].id);
  }
  return th.small_px + 12 + tile_h;
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
    if (th.gradients) {
      Color hue = th.accents4[i % 4];
      if (th.shadows) canvas.draw_soft_shadow(cell, th.radius, 3, 28);
      canvas.fill_round_rect_gradient(cell, th.radius, wash(hue, 0.86f), wash(hue, 0.58f));
      canvas.fill_gloss(cell, th.radius, 90);
      canvas.draw_round_rect(cell, th.radius, wash(hue, 0.35f), 2);
    } else {
      draw_panel(canvas, cell, th.rules);
    }
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
    Icon icon;
  };
  const Nav navs[] = {{"Library", kIdLibrary, Icon::Book},
                      {"Notebooks", kIdNotebooks, Icon::Note},
                      {"Statistics", kIdStats, Icon::Sort},
                      {"Settings", kIdSettings, Icon::Settings}};
  int n = 4;
  int gap = th.padding / 2;
  int w = (area.w - 2 * th.padding - gap * (n - 1)) / n;
  for (int i = 0; i < n; ++i) {
    Rect button(area.x + th.padding + i * (w + gap), area.y, w, th.row_height);
    if (th.gradients) {
      // Each destination gets its own hue, glossed: the four-colour
      // signature of the era, and genuinely easier to hit by memory.
      Color hue = th.accents4[i % 4];
      if (th.shadows) canvas.draw_soft_shadow(button, th.radius, 3, 34);
      canvas.fill_round_rect_gradient(button, th.radius,
                                      lerp_color(hue, Color::gray(255), 0.32f),
                                      lerp_color(hue, Color::gray(0), 0.18f));
      canvas.fill_gloss(button, th.radius, 110);
      canvas.draw_round_rect(button, th.radius, lerp_color(hue, Color::gray(0), 0.4f), 2);
      int icon_size = th.base_px + 4;
      Rect icon(button.x + th.padding / 2, button.y + (button.h - icon_size) / 2, icon_size,
                icon_size);
      draw_icon(canvas, icon, navs[i].icon, Color::gray(255));
      draw_text_in(canvas,
                   Rect(icon.right(), button.y, button.w - icon.w - th.padding, button.h),
                   navs[i].label, ui_style(th.base_px, Color::gray(255)), 0);
    } else {
      draw_button(canvas, button, navs[i].label, ButtonStyle::Normal);
    }
    hits_.add(button, navs[i].id);
  }
}

void HomeScreen::draw(Canvas& canvas, const Rect& bounds) {
  const Theme& th = theme();
  hits_.clear();
  paint_background(canvas);

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
    // The glance strip sits above the buttons, so the page has weight at
    // the bottom even when the shelf is short. It gives way when a full
    // three-row shelf needs the room.
    int glance_h = glance_height();
    bool show_glance = rest.h >= glance_h + th.row_height * 6;
    Rect shelf_area = show_glance
                          ? Rect(rest.x, rest.y, rest.w, rest.h - glance_h - th.padding / 2)
                          : rest;
    draw_recent_grid(canvas, shelf_area);
    if (show_glance) {
      draw_glance(canvas, Rect(rest.x, rest.bottom() - glance_h, rest.w, glance_h));
    }
  }
  draw_nav(canvas, Rect(bounds.x, bounds.bottom() - nav_h, bounds.w, nav_h));
}

bool HomeScreen::handle(const InputEvent& event) {
  if (event.type == EventType::KeyDown) {
    if (event.key == Key::PageForward || event.key == Key::PageBack) {
      // The page buttons open the last book: the most common thing to want.
      if (!shelf_.empty()) {
        open_book(shelf_.front().path);
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
  switch (id) {
    case kIdContinue:
      if (!shelf_.empty()) open_book(shelf_.front().path);
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
        if (index < shelf_.size()) {
          open_book(shelf_[index].path);
          return true;
        }
      }
      return false;
  }
}

}  // namespace

ViewPtr make_home_screen() { return std::make_unique<HomeScreen>(); }

}  // namespace ck
