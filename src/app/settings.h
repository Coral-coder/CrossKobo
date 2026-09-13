#pragma once
#include <string>
#include <vector>

#include "core/json.h"
#include "platform/input.h"
#include "platform/screen.h"

namespace ck {

enum class UiTheme { Classic, Minimal, Dashboard, Aero };
enum class SleepScreen { Cover, BookProgress, Dashboard, Custom, Blank };
enum class Alignment { Left, Justify };
enum class ImageRendering { Grayscale, Color, ColorBoost };
enum class PageAnimation { None, Fade };
enum class TapZones { Standard, Inverted, Off };

// Every user-visible knob. The layout mirrors CrossInk's settings taxonomy
// so the two feel like relatives, with additions for colour and the stylus.
struct Settings {
  // -------------------------------------------------------------- reading
  std::string font_family = "Literata";
  int font_size_pt = 12;             // 10, 12, 14, 16 like CrossInk
  int line_spacing = 140;            // percent of the font's natural leading
  int screen_margin = 40;            // pixels
  Alignment alignment = Alignment::Justify;
  bool hyphenation = true;
  bool embedded_style = true;        // honour the book's own CSS
  bool force_paragraph_indent = false;
  bool extra_paragraph_spacing = false;
  ImageRendering image_rendering = ImageRendering::Color;

  // ----------------------------------------------------------- reader ui
  bool focus_reading = false;        // hide everything but the text
  bool guide_dots = false;           // dotted rule under the current line
  int auto_page_turn_seconds = 0;    // 0 = off, else 5..120
  bool status_bar_title = true;
  bool status_bar_clock = true;
  // Looks for a new release once a day, and only when Wi-Fi is already
  // connected. Never turns the radio on by itself.
  bool auto_update_check = true;
  // Sets the clock from a time server when Wi-Fi is connected. CrossKobo
  // replaces the software that normally does this, so without it the time
  // drifts and reading statistics land on the wrong day.
  bool clock_sync = true;
  bool status_bar_battery = true;
  bool status_bar_progress_bar = true;
  int status_bar_progress_thickness = 3;
  bool status_bar_chapter_pages = true;
  bool status_bar_percentage = true;
  bool hide_battery_percentage = false;
  TapZones tap_zones = TapZones::Standard;
  bool tap_for_reader_menu = true;   // centre tap opens the menu
  int refresh_frequency = 8;         // full refresh every N page turns, 0 = never

  // -------------------------------------------------------------- library
  bool show_hidden_files = false;
  bool move_finished_to_read_folder = false;
  bool remove_read_books_from_recents = true;
  std::string library_sort = "recent";  // recent|title|author|added

  // ---------------------------------------------------------------- notes
  int pen_width = 3;                 // 1..12
  int pen_color_index = 0;           // index into the ink palette
  bool pen_pressure = true;
  bool palm_rejection = true;        // ignore touch while the pen is near
  std::string note_template = "blank";  // blank|lined|grid|dots
  bool notes_fast_ink = true;        // A2 refresh while drawing

  // ---------------------------------------------------------------- power
  int sleep_timeout_minutes = 15;
  int power_off_hours = 0;           // hours asleep before powering off; 0 = never
  SleepScreen sleep_screen = SleepScreen::Cover;
  std::string sleep_custom_image;
  bool frontlight_restore_on_wake = true;
  int frontlight_brightness = 20;
  int frontlight_warmth = 0;
  bool frontlight_on = true;

  // --------------------------------------------------------------- device
  UiTheme theme = UiTheme::Classic;
  bool night_mode = false;
  CfaMode cfa_mode = CfaMode::Default;
  float saturation_boost = 0.0f;
  int rotation = 0;                  // quarter turns
  bool clock_24h = true;
  bool buttons_swapped = false;      // swap the page-turn buttons
  bool buttons_follow_rotation = true;
  std::string usb_action = "ask";    // ask|handover|ignore
  TouchTransform touch_transform;
  // Set once the calibration wizard has run. Until then the platform layer
  // is free to guess the panel's orientation from its axis ranges; after
  // it, the user's answer is the whole story.
  bool touch_calibrated = false;
  TouchTransform pen_transform;
  bool log_debug = false;

  // ----------------------------------------------------------- catalogues
  // Saved OPDS catalogues: Shelfmark, Calibre-Web, Kavita, Komga, BookLore
  // and the public ones all speak the same format. Credentials are stored
  // in the settings file on the user partition, which is visible over USB,
  // so this is Basic authentication for a home server - not a secret store.
  struct Catalogue {
    std::string name;
    std::string url;
    std::string user;
    std::string password;
  };
  std::vector<Catalogue> catalogues;
  // Where books fetched from a catalogue are written, under the drive root.
  std::string catalogue_folder = "Downloads";

  // ------------------------------------------------------------ interface
  void load();
  // Seeds settings that depend on which device this is. Called when there
  // is no settings file yet, so a calibration the user makes later sticks.
  void apply_device_defaults();
  // Just the touch mapping part, re-applied on every load until the user
  // calibrates, so an old build's guess does not stick forever.
  void apply_touch_defaults();
  void save() const;
  Json to_json() const;
  void from_json(const Json& j);

  // Convenience: the font size in pixels for this device's DPI.
  int font_px(int dpi) const;
};

Settings& settings();

}  // namespace ck
