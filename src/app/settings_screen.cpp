// The settings tree, the reading statistics screen, the About screen and
// the input calibration screen.
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "core/version.h"
#include "gfx/font.h"
#include "library/library.h"
#include "platform/device.h"
#include "platform/net.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "platform/system.h"
#include "reader/state.h"
#include "ui/list_view.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

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
  void on_show() override { rebuild(); }
  void rebuild() { set_items(builder_(), true); }

 private:
  Builder builder_;
  Handler handler_;
};

ListView::Item row(int id, std::string title, std::string value = "",
                   std::string subtitle = "") {
  ListView::Item item;
  item.id = id;
  item.row.title = std::move(title);
  item.row.trailing = std::move(value);
  item.row.subtitle = std::move(subtitle);
  return item;
}

ListView::Item section(std::string title) {
  ListView::Item item;
  item.separator = true;
  item.id = -1;
  item.row.title = std::move(title);
  return item;
}

const char* on_off(bool v) { return v ? "On" : "Off"; }

bool right_half(const DynamicList& list) {
  return list.last_tap_x() > list.last_tap_row_x() + list.last_tap_row_width() / 2;
}

int step(int value, int delta, int lo, int hi, int by) {
  return std::max(lo, std::min(hi, value + delta * by));
}

// ------------------------------------------------------------ sub-screens

void push_reading_settings() {
  enum { kFont = 1, kSize, kSpacing, kMargin, kAlign, kHyphen, kIndent, kSpace, kCss, kImages };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    items.push_back(row(kFont, "Reading font", s.font_family));
    items.push_back(row(kSize, "Font size", format("%d pt", s.font_size_pt)));
    items.push_back(row(kSpacing, "Line spacing", format("%d%%", s.line_spacing)));
    items.push_back(row(kMargin, "Page margins", format("%d px", s.screen_margin)));
    items.push_back(row(kAlign, "Alignment",
                        s.alignment == Alignment::Justify ? "Justified" : "Ragged right"));
    items.push_back(row(kHyphen, "Hyphenation", on_off(s.hyphenation)));
    items.push_back(row(kIndent, "Force paragraph indents", on_off(s.force_paragraph_indent)));
    items.push_back(row(kSpace, "Extra paragraph spacing", on_off(s.extra_paragraph_spacing)));
    items.push_back(row(kCss, "Use the book's own styles", on_off(s.embedded_style)));
    const char* modes[] = {"Greyscale", "Colour", "Colour, boosted"};
    items.push_back(row(kImages, "Images", modes[(int)s.image_rendering],
                        "How pictures inside books are rendered"));
    return items;
  };
  auto handler = [](int id, DynamicList& list) {
    Settings& s = settings();
    int delta = right_half(list) ? 1 : -1;
    switch (id) {
      case kFont: {
        std::vector<std::string> families = FontManager::instance().family_names();
        std::vector<ListView::Item> options;
        for (size_t i = 0; i < families.size(); ++i) {
          ListView::Item item = row((int)i, families[i]);
          if (families[i] == s.font_family) item.row.check = true;
          options.push_back(std::move(item));
        }
        App::instance().push(std::make_unique<ListView>(
            "Reading font", std::move(options), [families](int choice) {
              if (choice >= 0 && choice < (int)families.size()) {
                settings().font_family = families[(size_t)choice];
                settings().save();
              }
              App::instance().pop();
            }));
        return;
      }
      case kSize: s.font_size_pt = step(s.font_size_pt, delta, 8, 24, 1); break;
      case kSpacing: s.line_spacing = step(s.line_spacing, delta, 100, 200, 5); break;
      case kMargin: s.screen_margin = step(s.screen_margin, delta, 8, 160, 8); break;
      case kAlign:
        s.alignment = s.alignment == Alignment::Justify ? Alignment::Left : Alignment::Justify;
        break;
      case kHyphen: s.hyphenation = !s.hyphenation; break;
      case kIndent: s.force_paragraph_indent = !s.force_paragraph_indent; break;
      case kSpace: s.extra_paragraph_spacing = !s.extra_paragraph_spacing; break;
      case kCss: s.embedded_style = !s.embedded_style; break;
      case kImages:
        s.image_rendering = (ImageRendering)(((int)s.image_rendering + 1) % 3);
        break;
      default: return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Reading", build, handler));
}

void push_display_settings() {
  enum { kTheme = 1, kNight, kColour, kSaturation, kRotation, kRefresh, kBrightness, kWarmth };
  auto build = []() {
    Settings& s = settings();
    Power& power = Power::instance();
    std::vector<ListView::Item> items;
    const char* themes[] = {"Classic", "Minimal", "Dashboard"};
    items.push_back(row(kTheme, "Interface theme", themes[(int)s.theme]));
    items.push_back(row(kNight, "Night mode", on_off(s.night_mode)));
    if (Screen::instance().color()) {
      items.push_back(section("Colour"));
      const char* modes[] = {"Panel default", "Standard", "Boosted", "Muted", "Vivid",
                             "Greyscale"};
      items.push_back(row(kColour, "Colour rendering", modes[(int)s.cfa_mode],
                          "Drives the Kaleido colour filter"));
      items.push_back(row(kSaturation, "Extra saturation",
                          format("%+d%%", (int)std::lround(s.saturation_boost * 100)),
                          "Applied before the panel filter"));
    }
    items.push_back(section("Panel"));
    items.push_back(row(kRotation, "Rotation", format("%d\xC2\xB0", s.rotation * 90)));
    items.push_back(row(kRefresh, "Full refresh every",
                        s.refresh_frequency ? format("%d pages", s.refresh_frequency) : "Never",
                        "Clears ghosting"));
    if (power.has_frontlight()) {
      items.push_back(section("Front light"));
      items.push_back(row(kBrightness, "Brightness", format("%d%%", power.brightness())));
      if (power.has_warmth()) {
        items.push_back(row(kWarmth, "Warmth", format("%d%%", power.warmth())));
      }
    }
    return items;
  };
  auto handler = [](int id, DynamicList& list) {
    Settings& s = settings();
    Power& power = Power::instance();
    int delta = right_half(list) ? 1 : -1;
    switch (id) {
      case kTheme:
        s.theme = (UiTheme)(((int)s.theme + 1) % 3);
        refresh_theme_from_settings(Screen::instance().dpi());
        App::instance().invalidate(Refresh::Flash);
        break;
      case kNight:
        s.night_mode = !s.night_mode;
        Screen::instance().set_night_mode(s.night_mode);
        refresh_theme_from_settings(Screen::instance().dpi());
        App::instance().invalidate(Refresh::Flash);
        break;
      case kColour:
        s.cfa_mode = (CfaMode)(((int)s.cfa_mode + 1) % 6);
        Screen::instance().set_cfa_mode(s.cfa_mode);
        App::instance().invalidate(Refresh::Color);
        break;
      case kSaturation: {
        int value = (int)std::lround(s.saturation_boost * 100);
        value = step(value, delta, -50, 80, 10);
        s.saturation_boost = value / 100.0f;
        Screen::instance().set_saturation_boost(s.saturation_boost);
        App::instance().invalidate(Refresh::Color);
        break;
      }
      case kRotation:
        s.rotation = (s.rotation + (delta > 0 ? 1 : 3)) % 4;
        Screen::instance().set_rotation((Rotation)s.rotation);
        Input::instance().set_rotation(s.rotation);
        Input::instance().set_screen_size(Screen::instance().width(),
                                          Screen::instance().height());
        refresh_theme_from_settings(Screen::instance().dpi());
        App::instance().invalidate(Refresh::Flash);
        break;
      case kRefresh:
        s.refresh_frequency = step(s.refresh_frequency, delta, 0, 30, 1);
        Screen::instance().set_flash_interval(s.refresh_frequency);
        break;
      case kBrightness:
        power.set_brightness(step(power.brightness(), delta, 0, 100, 5));
        s.frontlight_brightness = power.brightness();
        s.frontlight_on = power.brightness() > 0;
        break;
      case kWarmth:
        power.set_warmth(step(power.warmth(), delta, 0, 100, 10));
        s.frontlight_warmth = power.warmth();
        break;
      default: return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Display and light", build, handler));
}

void push_notes_settings() {
  enum { kWidth = 1, kColour, kPressure, kPalm, kTemplate, kFastInk };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    items.push_back(row(kWidth, "Pen width", format("%d px", s.pen_width)));
    ListView::Item colour = row(kColour, "Ink colour",
                                ink_palette()[std::max(0, std::min(ink_palette_size() - 1,
                                                                   s.pen_color_index))]
                                    .name);
    colour.row.swatch =
        ink_palette()[std::max(0, std::min(ink_palette_size() - 1, s.pen_color_index))].color;
    items.push_back(std::move(colour));
    items.push_back(row(kPressure, "Pressure sensitivity", on_off(s.pen_pressure)));
    items.push_back(row(kPalm, "Palm rejection", on_off(s.palm_rejection),
                        "Ignore touch just after the pen is used"));
    items.push_back(row(kTemplate, "Default page template", s.note_template));
    items.push_back(row(kFastInk, "Fast ink refresh", on_off(s.notes_fast_ink),
                        "A2 waveform while drawing: fastest, slightly grainy"));
    return items;
  };
  auto handler = [](int id, DynamicList& list) {
    Settings& s = settings();
    int delta = right_half(list) ? 1 : -1;
    switch (id) {
      case kWidth: s.pen_width = step(s.pen_width, delta, 1, 12, 1); break;
      case kColour:
        s.pen_color_index = (s.pen_color_index + (delta > 0 ? 1 : ink_palette_size() - 1)) %
                            ink_palette_size();
        break;
      case kPressure: s.pen_pressure = !s.pen_pressure; break;
      case kPalm: s.palm_rejection = !s.palm_rejection; break;
      case kTemplate: {
        static const char* kTemplates[] = {"blank", "lined", "grid", "dots", "cornell"};
        int index = 0;
        for (int i = 0; i < 5; ++i) {
          if (s.note_template == kTemplates[i]) index = i;
        }
        s.note_template = kTemplates[(index + 1) % 5];
        break;
      }
      case kFastInk: s.notes_fast_ink = !s.notes_fast_ink; break;
      default: return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Notes", build, handler));
}

void push_power_settings() {
  enum { kSleep = 1, kPowerOff, kSleepScreen, kRestoreLight, kSleepNow };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    items.push_back(row(kSleep, "Sleep after",
                        s.sleep_timeout_minutes ? format("%d min", s.sleep_timeout_minutes)
                                                : "Never"));
    items.push_back(row(kPowerOff, "Power off after",
                        s.power_off_hours ? format("%d h asleep", s.power_off_hours) : "Never",
                        "Saves the last of the battery on a device left in a bag"));
    const char* screens[] = {"Book cover", "Reading progress", "Statistics", "Custom image",
                             "Blank"};
    items.push_back(row(kSleepScreen, "Sleep screen", screens[(int)s.sleep_screen]));
    items.push_back(row(kRestoreLight, "Restore light on wake",
                        on_off(s.frontlight_restore_on_wake)));
    items.push_back(row(kSleepNow, "Sleep now"));
    return items;
  };
  auto handler = [](int id, DynamicList& list) {
    Settings& s = settings();
    int delta = right_half(list) ? 1 : -1;
    switch (id) {
      case kSleep: s.sleep_timeout_minutes = step(s.sleep_timeout_minutes, delta, 0, 120, 5); break;
      case kPowerOff: s.power_off_hours = step(s.power_off_hours, delta, 0, 48, 1); break;
      case kSleepScreen: s.sleep_screen = (SleepScreen)(((int)s.sleep_screen + 1) % 5); break;
      case kRestoreLight:
        s.frontlight_restore_on_wake = !s.frontlight_restore_on_wake;
        break;
      case kSleepNow:
        s.save();
        App::instance().sleep_now();
        return;
      default: return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Power", build, handler));
}

void push_controls_settings() {
  enum { kSwap, kFollow, kTapZones, kTapMenu, kUsb, kCalibrate };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    if (device().has_page_buttons) {
      items.push_back(row(kSwap, "Swap page buttons", on_off(s.buttons_swapped)));
      items.push_back(row(kFollow, "Buttons follow rotation", on_off(s.buttons_follow_rotation)));
    }
    const char* zones[] = {"Standard", "Inverted", "Off"};
    items.push_back(row(kTapZones, "Tap zones", zones[(int)s.tap_zones]));
    items.push_back(row(kTapMenu, "Centre tap opens menu", on_off(s.tap_for_reader_menu)));
    items.push_back(row(kUsb, "When USB is connected",
                        s.usb_action == "ask" ? "Ask"
                                              : (s.usb_action == "handover" ? "Switch to Kobo UI"
                                                                            : "Ignore")));
    items.push_back(row(kCalibrate, "Touch and stylus test",
                        "", "Check where taps land and fix mirrored axes"));
    return items;
  };
  auto handler = [](int id, DynamicList& list) {
    Settings& s = settings();
    switch (id) {
      case kSwap: s.buttons_swapped = !s.buttons_swapped; break;
      case kFollow: s.buttons_follow_rotation = !s.buttons_follow_rotation; break;
      case kTapZones: s.tap_zones = (TapZones)(((int)s.tap_zones + 1) % 3); break;
      case kTapMenu: s.tap_for_reader_menu = !s.tap_for_reader_menu; break;
      case kUsb:
        s.usb_action = s.usb_action == "ask" ? "handover"
                                             : (s.usb_action == "handover" ? "ignore" : "ask");
        break;
      case kCalibrate:
        App::instance().push(make_calibration_screen());
        return;
      default: return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Controls", build, handler));
}

void push_library_settings() {
  enum { kHidden = 1, kMoveRead, kDropRead, kSort, kClearCache };
  auto build = []() {
    Settings& s = settings();
    std::vector<ListView::Item> items;
    items.push_back(row(kHidden, "Show hidden files", on_off(s.show_hidden_files)));
    items.push_back(row(kMoveRead, "Move finished books to \"Read\"",
                        on_off(s.move_finished_to_read_folder)));
    items.push_back(row(kDropRead, "Drop finished books from Recent",
                        on_off(s.remove_read_books_from_recents)));
    items.push_back(row(kSort, "Sort by", s.library_sort));
    items.push_back(row(kClearCache, "Clear cover cache", "",
                        "Frees space; covers are rebuilt when needed"));
    return items;
  };
  auto handler = [](int id, DynamicList& list) {
    Settings& s = settings();
    switch (id) {
      case kHidden: s.show_hidden_files = !s.show_hidden_files; break;
      case kMoveRead:
        s.move_finished_to_read_folder = !s.move_finished_to_read_folder;
        break;
      case kDropRead:
        s.remove_read_books_from_recents = !s.remove_read_books_from_recents;
        break;
      case kSort:
        s.library_sort = s.library_sort == "recent"
                             ? "title"
                             : (s.library_sort == "title" ? "author" : "recent");
        break;
      case kClearCache:
        fs::remove_tree(paths().cache_dir());
        covers::clear_memory_cache();
        App::instance().show_toast("Cover cache cleared");
        return;
      default: return;
    }
    s.save();
  };
  App::instance().push(std::make_unique<DynamicList>("Library", build, handler));
}

}  // namespace

// ----------------------------------------------------------- settings root

ViewPtr make_settings_screen() {
  enum {
    kReading = 1,
    kDisplay,
    kNotes,
    kPower,
    kControls,
    kLibrary,
    kNetwork,
    kAbout,
    kReturnToKobo,
    kRestart,
  };
  std::vector<ListView::Item> items;
  items.push_back(row(kReading, "Reading",
                      format("%s %dpt", settings().font_family.c_str(),
                             settings().font_size_pt)));
  items.push_back(row(kDisplay, "Display and light"));
  items.push_back(row(kNotes, "Notes and stylus"));
  items.push_back(row(kPower, "Power and sleep"));
  items.push_back(row(kControls, "Controls"));
  items.push_back(row(kLibrary, "Library"));
  {
    Net& net = Net::instance();
    net.refresh_status();
    items.push_back(row(kNetwork, "Wi-Fi",
                        net.connected() ? net.current_ssid()
                                        : (net.available() ? "Off" : "Unavailable"),
                        net.connected() ? net.status_text() : ""));
  }
  items.push_back(section("System"));
  items.push_back(row(kAbout, "About CrossKobo"));
  items.push_back(row(kRestart, "Restart CrossKobo"));
  items.push_back(row(kReturnToKobo, "Return to the Kobo UI", "",
                      "Starts the stock software until the next restart"));

  auto list = std::make_unique<ListView>("Settings", std::move(items), [](int id) {
    switch (id) {
      case kReading: push_reading_settings(); break;
      case kDisplay: push_display_settings(); break;
      case kNotes: push_notes_settings(); break;
      case kPower: push_power_settings(); break;
      case kControls: push_controls_settings(); break;
      case kLibrary: push_library_settings(); break;
      case kNetwork: App::instance().push(make_network_screen()); break;
      case kAbout: App::instance().push(make_about_screen()); break;
      case kRestart:
        settings().save();
        App::instance().quit(kExitRestart);
        break;
      case kReturnToKobo:
        if (App::instance().confirm(
                "Return to the Kobo UI",
                "The stock Kobo software will start now.\n\nCrossKobo comes back the next "
                "time the device is powered on.",
                "Switch", "Stay")) {
          App::instance().return_to_kobo_ui();
        }
        break;
      default: break;
    }
  });
  return list;
}

// ------------------------------------------------------------------- stats

ViewPtr make_stats_screen() {
  Stats& stats = Stats::instance();
  stats.load();
  std::vector<ListView::Item> items;
  items.push_back(section("All time"));
  items.push_back(row(0, "Books finished", format("%d", stats.books_finished)));
  items.push_back(row(0, "Time reading", human_duration(stats.total_seconds)));
  items.push_back(row(0, "Sessions", format("%d", stats.sessions)));
  items.push_back(row(0, "Pages turned", format("%d", stats.pages_turned)));
  items.push_back(row(0, "Average session", human_duration(stats.average_session())));
  items.push_back(row(0, "Longest session", human_duration(stats.longest_session)));
  items.push_back(row(0, "Pages per minute", format("%.1f", stats.pages_per_minute())));
  items.push_back(section("Recently"));
  items.push_back(row(0, "Today", human_duration(stats.seconds_today())));
  items.push_back(row(0, "Streak",
                      format("%d day%s", stats.streak_days(),
                             stats.streak_days() == 1 ? "" : "s")));
  if (stats.first_use) {
    items.push_back(row(0, "Reading since", format_date(stats.first_use)));
  }
  items.push_back(section("Per book"));
  for (const RecentEntry& e : Recents::instance().entries()) {
    items.push_back(row(0, e.title,
                        e.finished ? "Read" : format("%d%%", (int)std::lround(e.progress * 100)),
                        e.author));
  }
  return std::make_unique<ListView>("Statistics", std::move(items), nullptr);
}

// ------------------------------------------------------------------- about

ViewPtr make_about_screen() {
  const DeviceInfo& dev = device();
  Screen& screen = Screen::instance();
  std::vector<ListView::Item> items;
  items.push_back(row(0, "CrossKobo", kVersion));
  items.push_back(section("Device"));
  items.push_back(row(0, "Model", dev.model));
  items.push_back(row(0, "Codename", dev.codename));
  if (!dev.firmware.empty()) items.push_back(row(0, "Kobo firmware", dev.firmware));
  items.push_back(row(0, "Display controller", dev.is_mtk ? "MediaTek hwtcon" : "i.MX EPDC"));
  items.push_back(row(0, "Panel", format("%dx%d @ %d dpi", screen.width(), screen.height(),
                                         screen.dpi())));
  items.push_back(row(0, "Colour panel", screen.color() ? "Yes (Kaleido)" : "No"));
  items.push_back(row(0, "Stylus", Input::instance().has_pen() ? "Detected" : "Not detected"));
  items.push_back(row(0, "Front light", Power::instance().has_frontlight() ? "Yes" : "No"));
  items.push_back(row(0, "Warmth control", Power::instance().has_warmth() ? "Yes" : "No"));
  items.push_back(section("Storage"));
  items.push_back(row(0, "Books", paths().onboard));
  items.push_back(row(0, "Notebooks", paths().notebooks));
  items.push_back(row(0, "Settings", paths().data));
  items.push_back(row(0, "Free space", human_size(fs::free_space_bytes(paths().onboard))));
  items.push_back(section("Credits"));
  items.push_back(row(0, "Design lineage", "CrossPoint Reader / CrossInk"));
  items.push_back(row(0, "Hardware notes", "FBInk and KOReader (documentation)"));
  items.push_back(row(0, "Fonts", "Lexend Deca, Literata (OFL)"));
  items.push_back(row(0, "Licence", "MIT"));
  return std::make_unique<ListView>("About", std::move(items), nullptr);
}

// ------------------------------------------------------------- calibration

namespace {

// Shows raw input as it arrives. With no way to test this build on every
// Kobo model, this screen is how a user diagnoses a mirrored touch panel or
// stylus and fixes it themselves.
class CalibrationScreen : public View {
 public:
  std::string title() const override { return "Touch and stylus test"; }
  Refresh refresh_hint() const override { return Refresh::Fast; }

  void draw(Canvas& canvas, const Rect& bounds) override {
    const Theme& th = theme();
    hits_.clear();
    canvas.clear(th.bg);
    StatusBarInfo info;
    int top = draw_top_bar(canvas, bounds, "Touch and stylus test", info);

    // Lay the bottom out first: two rows of toggles and a row of buttons,
    // so the drawing area gets whatever is left rather than overlapping them.
    int button_row_h = th.row_height + th.padding / 2;
    int toggle_rows_h = 2 * (th.row_height + 6);
    Rect body(bounds.x + th.padding, bounds.y + top, bounds.w - 2 * th.padding,
              bounds.h - top - toggle_rows_h - button_row_h - th.padding);
    draw_panel(canvas, body, true);

    // Targets in the corners: tap them and see whether the marks land in
    // the same place.
    const int r = 26;
    Color target = th.faint;
    canvas.draw_round_rect(Rect(body.x + 8, body.y + 8, 2 * r, 2 * r), r, target, 2);
    canvas.draw_round_rect(Rect(body.right() - 8 - 2 * r, body.y + 8, 2 * r, 2 * r), r, target, 2);
    canvas.draw_round_rect(Rect(body.x + 8, body.bottom() - 8 - 2 * r, 2 * r, 2 * r), r, target,
                           2);
    canvas.draw_round_rect(Rect(body.right() - 8 - 2 * r, body.bottom() - 8 - 2 * r, 2 * r, 2 * r),
                           r, target, 2);
    draw_text_in(canvas, Rect(body.x, body.y + body.h / 2 - th.row_height, body.w, th.row_height),
                 "Touch the circles, then draw with the stylus",
                 ui_style(th.small_px, th.muted), 0);

    for (const Mark& m : marks_) {
      Color c = m.pen ? Color::rgb(0x1146C8) : Color::rgb(0xD01414);
      canvas.fill_circle(m.x, m.y, m.pen ? 4 + m.pressure / 200 : 8, c);
    }
    if (!marks_.empty()) {
      const Mark& last = marks_.back();
      draw_text_in(canvas,
                   Rect(body.x + th.padding, body.bottom() - th.row_height, body.w, th.row_height),
                   format("last: %s at %d,%d pressure %d", last.pen ? "pen" : "touch", last.x,
                          last.y, last.pressure),
                   ui_style(th.small_px, th.fg), -1);
    }

    // Transform toggles.
    int y = body.bottom() + th.padding / 2;
    const Settings& s = settings();
    struct Toggle {
      const char* label;
      bool value;
      int id;
    };
    const Toggle toggles[] = {
        {"Touch: swap X/Y", s.touch_transform.swap_xy, 1},
        {"Touch: mirror X", s.touch_transform.mirror_x, 2},
        {"Touch: mirror Y", s.touch_transform.mirror_y, 3},
        {"Pen: swap X/Y", s.pen_transform.swap_xy, 4},
        {"Pen: mirror X", s.pen_transform.mirror_x, 5},
        {"Pen: mirror Y", s.pen_transform.mirror_y, 6},
    };
    int cols = 3;
    int w = (bounds.w - 2 * th.padding - 2 * (th.padding / 2)) / cols;
    for (int i = 0; i < 6; ++i) {
      int c = i % cols, r2 = i / cols;
      Rect button(bounds.x + th.padding + c * (w + th.padding / 2),
                  y + r2 * (th.row_height + 6), w, th.row_height);
      draw_button(canvas, button, format("%s: %s", toggles[i].label, on_off(toggles[i].value)),
                  toggles[i].value ? ButtonStyle::Primary : ButtonStyle::Normal);
      hits_.add(button, toggles[i].id);
    }

    Rect clear(bounds.x + th.padding, bounds.bottom() - th.row_height - 4,
               (bounds.w - 3 * th.padding) / 2, th.row_height);
    Rect done(clear.right() + th.padding, clear.y, clear.w, th.row_height);
    draw_button(canvas, clear, "Clear marks", ButtonStyle::Normal);
    draw_button(canvas, done, "Done", ButtonStyle::Primary);
    hits_.add(clear, 7);
    hits_.add(done, 8);
  }

  bool handle(const InputEvent& event) override {
    if (event.is_pen()) {
      if (event.type != EventType::PenUp) add_mark(event.x, event.y, event.pressure, true);
      return true;
    }
    if (event.type == EventType::TouchDown || event.type == EventType::TouchMove) {
      add_mark(event.x, event.y, event.pressure, false);
      return true;
    }
    if (event.type != EventType::Tap) return false;
    Settings& s = settings();
    switch (hits_.hit(event.x, event.y)) {
      case 1: s.touch_transform.swap_xy = !s.touch_transform.swap_xy; break;
      case 2: s.touch_transform.mirror_x = !s.touch_transform.mirror_x; break;
      case 3: s.touch_transform.mirror_y = !s.touch_transform.mirror_y; break;
      case 4: s.pen_transform.swap_xy = !s.pen_transform.swap_xy; break;
      case 5: s.pen_transform.mirror_x = !s.pen_transform.mirror_x; break;
      case 6: s.pen_transform.mirror_y = !s.pen_transform.mirror_y; break;
      case 7: marks_.clear(); return true;
      case 8:
        s.save();
        App::instance().pop();
        return true;
      default: return false;
    }
    s.save();
    Input::instance().set_touch_transform(s.touch_transform);
    Input::instance().set_pen_transform(s.pen_transform);
    marks_.clear();
    return true;
  }

 private:
  struct Mark {
    int x, y, pressure;
    bool pen;
  };
  void add_mark(int x, int y, int pressure, bool pen) {
    marks_.push_back({x, y, pressure, pen});
    if (marks_.size() > 400) marks_.erase(marks_.begin());
  }

  std::vector<Mark> marks_;
  HitList hits_;
};

}  // namespace

ViewPtr make_calibration_screen() { return std::make_unique<CalibrationScreen>(); }

}  // namespace ck
