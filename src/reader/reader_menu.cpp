// The in-reader menus: table of contents, bookmarks, typography, reading
// modes and display. Every change applies immediately and keeps the reader
// on the same words, which is the whole point of adjusting type while
// reading rather than in a separate settings app.
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "app/app.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/str.h"
#include "gfx/font.h"
#include "notes/notes.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "reader/reader.h"
#include "ui/list_view.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

// Row ids for the top-level reader menu.
enum MenuId {
  kMenuToc = 1,
  kMenuBookmarks,
  kMenuAddBookmark,
  kMenuGoToPercent,
  kMenuTypography,
  kMenuReadingModes,
  kMenuDisplay,
  kMenuNewNote,
  kMenuMarkFinished,
  kMenuBookInfo,
  kMenuCloseBook,
};

// A list whose contents are rebuilt from a callback after every choice, so
// toggles and steppers show their new value at once.
class DynamicList : public ListView {
 public:
  using Builder = std::function<std::vector<Item>()>;
  using Handler = std::function<void(int id, DynamicList& list)>;

  DynamicList(std::string title, Builder builder, Handler handler)
      : ListView(std::move(title), {}, nullptr),
        builder_(std::move(builder)),
        handler_(std::move(handler)) {
    rebuild();
    on_select_ = [this](int id) {
      handler_(id, *this);
      rebuild();
    };
  }

  void rebuild() { set_items(builder_(), true); }

 private:
  Builder builder_;
  Handler handler_;
};

ListView::Item make_item(int id, std::string title, std::string value = "",
                         std::string subtitle = "") {
  ListView::Item item;
  item.id = id;
  item.row.title = std::move(title);
  item.row.trailing = std::move(value);
  item.row.subtitle = std::move(subtitle);
  return item;
}

ListView::Item make_section(std::string title) {
  ListView::Item item;
  item.separator = true;
  item.row.title = std::move(title);
  item.id = -1;
  return item;
}

const char* on_off(bool value) { return value ? "On" : "Off"; }

// Steppers: tapping the left half of a row decreases, the right half
// increases. It avoids a second screen for every numeric setting.
bool tapped_right_half(const DynamicList& list) {
  return list.last_tap_x() > list.last_tap_row_x() + list.last_tap_row_width() / 2;
}

int step_value(int value, int delta, int min_value, int max_value, int step) {
  int next = value + delta * step;
  return std::max(min_value, std::min(max_value, next));
}

}  // namespace

void ReaderScreen::open_toc() {
  std::vector<ListView::Item> items;
  const std::vector<TocEntry>& toc = book_.toc();
  if (toc.empty()) {
    // Fall back to the spine so navigation is always possible.
    for (size_t i = 0; i < book_.spine().size(); ++i) {
      items.push_back(make_item((int)i, format("Section %zu", i + 1)));
    }
  } else {
    for (size_t i = 0; i < toc.size(); ++i) {
      ListView::Item item = make_item((int)i, std::string(toc[i].depth * 2, ' ') + toc[i].title);
      item.row.bold = toc[i].spine_index == spine_index_;
      items.push_back(std::move(item));
    }
  }
  bool have_toc = !toc.empty();
  auto list = std::make_unique<ListView>("Contents", std::move(items), [this, have_toc](int id) {
    if (have_toc) {
      const TocEntry& entry = book_.toc()[(size_t)id];
      int spine = entry.spine_index >= 0 ? entry.spine_index : spine_index_;
      App::instance().pop();
      go_to_chapter(spine, book_.fragment_of(entry.href));
    } else {
      App::instance().pop();
      go_to_chapter(id);
    }
    App::instance().invalidate(Refresh::Image);
  });
  // Start the list at the chapter being read.
  for (size_t i = 0; i < toc.size(); ++i) {
    if (toc[i].spine_index == spine_index_) {
      list->reveal((int)i);
      break;
    }
  }
  App::instance().push(std::move(list));
}

void ReaderScreen::open_bookmarks() {
  auto build = [this]() {
    std::vector<ListView::Item> items;
    for (size_t i = 0; i < state_.bookmarks.size(); ++i) {
      const Bookmark& b = state_.bookmarks[i];
      ListView::Item item = make_item((int)i, b.snippet.empty() ? "Bookmark" : b.snippet);
      item.row.subtitle = format("Section %d \xC2\xB7 %s", b.spine + 1,
                                 relative_time(b.created).c_str());
      items.push_back(std::move(item));
    }
    return items;
  };
  auto list = std::make_unique<ListView>("Bookmarks", build(), [this](int id) {
    if (id < 0 || id >= (int)state_.bookmarks.size()) return;
    Bookmark b = state_.bookmarks[(size_t)id];
    App::instance().pop();
    go_to_position(b.spine, b.block, b.frag, b.offset);
    App::instance().invalidate(Refresh::Image);
  });
  list->set_empty_message("No bookmarks yet.\nLong-press a page to add one.");
  ListView* raw = list.get();
  raw->set_on_long_press([this, raw, build](int id) {
    if (id < 0 || id >= (int)state_.bookmarks.size()) return;
    if (App::instance().confirm("Remove bookmark", state_.bookmarks[(size_t)id].snippet,
                                "Remove", "Keep")) {
      state_.bookmarks.erase(state_.bookmarks.begin() + id);
      state_.save();
      raw->set_items(build(), true);
    }
  });
  App::instance().push(std::move(list));
}

namespace {

void push_font_picker(ReaderScreen* reader) {
  std::vector<std::string> families = FontManager::instance().family_names();
  std::vector<ListView::Item> items;
  for (size_t i = 0; i < families.size(); ++i) {
    ListView::Item item = make_item((int)i, families[i]);
    item.row.bold = families[i] == settings().font_family;
    if (item.row.bold) item.row.check = true;
    items.push_back(std::move(item));
  }
  auto list = std::make_unique<ListView>(
      "Reading font", std::move(items), [reader, families](int id) {
        if (id < 0 || id >= (int)families.size()) return;
        settings().font_family = families[(size_t)id];
        settings().save();
        reader->relayout();
        App::instance().pop();
        App::instance().invalidate(Refresh::Flash);
      });
  App::instance().push(std::move(list));
}

void push_typography_menu(ReaderScreen* reader) {
  enum {
    kFont = 1,
    kSize,
    kSpacing,
    kMargin,
    kAlign,
    kHyphen,
    kEmbedded,
    kIndent,
    kParaSpace,
  };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    items.push_back(make_section("Type"));
    items.push_back(make_item(kFont, "Font", s.font_family));
    items.push_back(make_item(kSize, "Size", format("%d pt", s.font_size_pt),
                              "Tap left or right to change"));
    items.push_back(make_item(kSpacing, "Line spacing", format("%d%%", s.line_spacing)));
    items.push_back(make_item(kMargin, "Margins", format("%d px", s.screen_margin)));
    items.push_back(make_section("Paragraphs"));
    items.push_back(make_item(kAlign, "Alignment",
                              s.alignment == Alignment::Justify ? "Justified" : "Ragged"));
    items.push_back(make_item(kHyphen, "Hyphenation", on_off(s.hyphenation)));
    items.push_back(make_item(kIndent, "Force indents", on_off(s.force_paragraph_indent)));
    items.push_back(
        make_item(kParaSpace, "Extra spacing", on_off(s.extra_paragraph_spacing)));
    items.push_back(make_item(kEmbedded, "Book's own styles", on_off(s.embedded_style)));
    return items;
  };
  auto handler = [reader](int id, DynamicList& list) {
    Settings& s = settings();
    int delta = tapped_right_half(list) ? 1 : -1;
    switch (id) {
      case kFont:
        push_font_picker(reader);
        return;
      case kSize:
        s.font_size_pt = step_value(s.font_size_pt, delta, 8, 24, 1);
        break;
      case kSpacing:
        s.line_spacing = step_value(s.line_spacing, delta, 100, 200, 5);
        break;
      case kMargin:
        s.screen_margin = step_value(s.screen_margin, delta, 8, 160, 8);
        break;
      case kAlign:
        s.alignment = s.alignment == Alignment::Justify ? Alignment::Left : Alignment::Justify;
        break;
      case kHyphen:
        s.hyphenation = !s.hyphenation;
        break;
      case kIndent:
        s.force_paragraph_indent = !s.force_paragraph_indent;
        break;
      case kParaSpace:
        s.extra_paragraph_spacing = !s.extra_paragraph_spacing;
        break;
      case kEmbedded:
        s.embedded_style = !s.embedded_style;
        // The book's CSS is applied while parsing, so this needs a reparse.
        reader->go_to_position(reader->state().spine, reader->state().block,
                               reader->state().frag, reader->state().offset);
        break;
      default:
        return;
    }
    s.save();
    reader->relayout();
  };
  App::instance().push(std::make_unique<DynamicList>("Typography", build, handler));
}

void push_reading_modes_menu(ReaderScreen* reader) {
  enum { kFocus = 1, kGuide, kAutoTurn, kTapZones, kTapMenu, kRefresh };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    items.push_back(make_item(kFocus, "Focus reading", on_off(s.focus_reading),
                              "Hide the status bar while reading"));
    items.push_back(make_item(kGuide, "Guide dots", on_off(s.guide_dots),
                              "A dotted rule under each line"));
    items.push_back(make_item(kAutoTurn, "Auto page turn",
                              s.auto_page_turn_seconds ? format("%d s", s.auto_page_turn_seconds)
                                                       : "Off"));
    const char* zones = s.tap_zones == TapZones::Standard
                            ? "Standard"
                            : (s.tap_zones == TapZones::Inverted ? "Inverted" : "Off");
    items.push_back(make_item(kTapZones, "Tap zones", zones));
    items.push_back(make_item(kTapMenu, "Centre tap opens menu", on_off(s.tap_for_reader_menu)));
    items.push_back(make_item(kRefresh, "Full refresh every",
                              s.refresh_frequency ? format("%d pages", s.refresh_frequency)
                                                  : "Never"));
    return items;
  };
  auto handler = [reader](int id, DynamicList& list) {
    Settings& s = settings();
    int delta = tapped_right_half(list) ? 1 : -1;
    switch (id) {
      case kFocus:
        s.focus_reading = !s.focus_reading;
        reader->relayout();
        break;
      case kGuide:
        s.guide_dots = !s.guide_dots;
        break;
      case kAutoTurn: {
        static const int kSteps[] = {0, 5, 10, 15, 20, 30, 45, 60, 90, 120};
        int count = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
        int index = 0;
        for (int i = 0; i < count; ++i) {
          if (kSteps[i] == s.auto_page_turn_seconds) index = i;
        }
        index = std::max(0, std::min(count - 1, index + delta));
        s.auto_page_turn_seconds = kSteps[index];
        break;
      }
      case kTapZones:
        s.tap_zones = (TapZones)(((int)s.tap_zones + 1) % 3);
        break;
      case kTapMenu:
        s.tap_for_reader_menu = !s.tap_for_reader_menu;
        break;
      case kRefresh:
        s.refresh_frequency = step_value(s.refresh_frequency, delta, 0, 30, 1);
        Screen::instance().set_flash_interval(s.refresh_frequency);
        break;
      default:
        return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Reading modes", build, handler));
}

void push_display_menu(ReaderScreen* reader) {
  enum { kBrightness = 1, kWarmth, kNight, kColour, kSaturation, kRotate };
  auto build = []() {
    Settings& s = settings();
    Power& power = Power::instance();
    std::vector<ListView::Item> items;
    if (power.has_frontlight()) {
      items.push_back(make_item(kBrightness, "Front light", format("%d%%", power.brightness()),
                                "Tap left or right to change"));
    }
    if (power.has_warmth()) {
      items.push_back(make_item(kWarmth, "Warmth", format("%d%%", power.warmth())));
    }
    items.push_back(make_item(kNight, "Night mode", on_off(s.night_mode)));
    if (Screen::instance().color()) {
      const char* modes[] = {"Panel default", "Standard", "Boosted", "Muted", "Vivid",
                             "Greyscale"};
      items.push_back(make_item(kColour, "Colour rendering", modes[(int)s.cfa_mode],
                                "How the Kaleido filter is driven"));
      items.push_back(make_item(kSaturation, "Extra saturation",
                                format("%+d%%", (int)std::lround(s.saturation_boost * 100))));
    }
    items.push_back(make_item(kRotate, "Rotation", format("%d\xC2\xB0", s.rotation * 90)));
    return items;
  };
  auto handler = [reader](int id, DynamicList& list) {
    Settings& s = settings();
    Power& power = Power::instance();
    int delta = tapped_right_half(list) ? 1 : -1;
    switch (id) {
      case kBrightness:
        power.set_brightness(step_value(power.brightness(), delta, 0, 100, 5));
        s.frontlight_brightness = power.brightness();
        s.frontlight_on = power.brightness() > 0;
        break;
      case kWarmth:
        power.set_warmth(step_value(power.warmth(), delta, 0, 100, 10));
        s.frontlight_warmth = power.warmth();
        break;
      case kNight:
        s.night_mode = !s.night_mode;
        Screen::instance().set_night_mode(s.night_mode);
        refresh_theme_from_settings(Screen::instance().dpi());
        reader->relayout();
        App::instance().invalidate(Refresh::Flash);
        break;
      case kColour:
        s.cfa_mode = (CfaMode)(((int)s.cfa_mode + 1) % 6);
        Screen::instance().set_cfa_mode(s.cfa_mode);
        App::instance().invalidate(Refresh::Color);
        break;
      case kSaturation: {
        int value = (int)std::lround(s.saturation_boost * 100);
        value = step_value(value, delta, -50, 80, 10);
        s.saturation_boost = value / 100.0f;
        Screen::instance().set_saturation_boost(s.saturation_boost);
        App::instance().invalidate(Refresh::Color);
        break;
      }
      case kRotate:
        s.rotation = (s.rotation + (delta > 0 ? 1 : 3)) % 4;
        Screen::instance().set_rotation((Rotation)s.rotation);
        Input::instance().set_rotation(s.rotation);
        Input::instance().set_screen_size(Screen::instance().width(),
                                          Screen::instance().height());
        refresh_theme_from_settings(Screen::instance().dpi());
        reader->relayout();
        App::instance().invalidate(Refresh::Flash);
        break;
      default:
        return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Display", build, handler));
}

void push_percent_menu(ReaderScreen* reader) {
  std::vector<ListView::Item> items;
  for (int pct = 0; pct <= 100; pct += 5) {
    items.push_back(make_item(pct, format("%d%%", pct)));
  }
  auto list = std::make_unique<ListView>("Go to", std::move(items), [reader](int id) {
    App::instance().pop();
    reader->go_to_percent(id / 100.0);
    App::instance().invalidate(Refresh::Image);
  });
  list->reveal((int)std::lround(reader->progress() * 20.0) * 5);
  App::instance().push(std::move(list));
}

void push_book_info(ReaderScreen* reader) {
  const BookMetadata& meta = reader->book().metadata();
  std::vector<ListView::Item> items;
  items.push_back(make_item(0, "Title", "", meta.title));
  if (!meta.author.empty()) items.push_back(make_item(0, "Author", "", meta.author));
  if (!meta.series.empty()) {
    items.push_back(make_item(0, "Series", "",
                              meta.series_index
                                  ? format("%s, book %d", meta.series.c_str(), meta.series_index)
                                  : meta.series));
  }
  if (!meta.publisher.empty()) items.push_back(make_item(0, "Publisher", "", meta.publisher));
  if (!meta.language.empty()) items.push_back(make_item(0, "Language", "", meta.language));
  items.push_back(make_item(0, "Sections", format("%d", (int)reader->book().spine().size())));
  BookState& st = reader->state();
  items.push_back(make_item(0, "Progress", format("%d%%", (int)std::lround(st.progress * 100))));
  items.push_back(make_item(0, "Time spent", human_duration(st.total_seconds)));
  items.push_back(make_item(0, "Sessions", format("%d", st.sessions)));
  items.push_back(make_item(0, "Pages turned", format("%d", st.pages_turned)));
  items.push_back(make_item(0, "File", "", reader->book().path()));
  App::instance().push(std::make_unique<ListView>("Book details", std::move(items), nullptr));
}

}  // namespace

void ReaderScreen::open_menu() {
  auto build = [this]() {
    std::vector<ListView::Item> items;
    items.push_back(make_item(kMenuToc, "Contents",
                              format("%d sections", (int)book_.spine().size())));
    items.push_back(make_item(kMenuGoToPercent, "Go to",
                              format("%d%%", (int)std::lround(state_.progress * 100))));
    items.push_back(make_item(kMenuBookmarks, "Bookmarks",
                              format("%d", (int)state_.bookmarks.size())));
    bool marked = !pages_.empty() &&
                  state_.has_bookmark_at(spine_index_, pages_[page_index_].start_block);
    items.push_back(make_item(kMenuAddBookmark,
                              marked ? "Remove this bookmark" : "Bookmark this page"));
    items.push_back(make_section("Adjust"));
    items.push_back(make_item(kMenuTypography, "Typography",
                              format("%s %dpt", settings().font_family.c_str(),
                                     settings().font_size_pt)));
    items.push_back(make_item(kMenuReadingModes, "Reading modes"));
    items.push_back(make_item(kMenuDisplay, "Display and light"));
    items.push_back(make_section("This book"));
    items.push_back(make_item(kMenuNewNote, "New note for this book"));
    items.push_back(make_item(kMenuMarkFinished,
                              state_.finished ? "Mark as unread" : "Mark as finished"));
    items.push_back(make_item(kMenuBookInfo, "Book details"));
    items.push_back(make_item(kMenuCloseBook, "Close book"));
    return items;
  };

  auto handler = [this](int id, DynamicList& list) {
    switch (id) {
      case kMenuToc:
        App::instance().pop();
        open_toc();
        break;
      case kMenuBookmarks:
        App::instance().pop();
        open_bookmarks();
        break;
      case kMenuAddBookmark:
        toggle_bookmark();
        break;
      case kMenuGoToPercent:
        push_percent_menu(this);
        break;
      case kMenuTypography:
        push_typography_menu(this);
        break;
      case kMenuReadingModes:
        push_reading_modes_menu(this);
        break;
      case kMenuDisplay:
        push_display_menu(this);
        break;
      case kMenuNewNote:
        open_note_for_book(book_.metadata().title, book_.path());
        break;
      case kMenuMarkFinished:
        mark_finished(!state_.finished);
        App::instance().show_toast(state_.finished ? "Marked as finished" : "Marked as unread");
        break;
      case kMenuBookInfo:
        push_book_info(this);
        break;
      case kMenuCloseBook:
        App::instance().pop();   // the menu
        App::instance().pop();   // the reader
        break;
      default:
        break;
    }
  };

  auto list = std::make_unique<DynamicList>(book_.metadata().title, build, handler);
  list->set_status_line(format("%d%% \xC2\xB7 page %d of %d in this section \xC2\xB7 %s read",
                               (int)std::lround(state_.progress * 100), page_index_ + 1,
                               (int)pages_.size(), human_duration(state_.total_seconds).c_str()));
  App::instance().push(std::move(list));
}

}  // namespace ck
