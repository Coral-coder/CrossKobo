#include "app/settings.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/paths.h"
#include "platform/device.h"

namespace ck {
namespace {

Settings g_settings;

template <typename E>
E enum_from(const Json& j, const std::string& key, E fallback, int count) {
  int v = j.get_int(key, (int)fallback);
  if (v < 0 || v >= count) return fallback;
  return (E)v;
}

Json transform_to_json(const TouchTransform& t) {
  Json j = Json::object();
  j["swap_xy"] = Json(t.swap_xy);
  j["mirror_x"] = Json(t.mirror_x);
  j["mirror_y"] = Json(t.mirror_y);
  return j;
}

TouchTransform transform_from_json(const Json* j, const TouchTransform& fallback) {
  if (!j || !j->is_object()) return fallback;
  TouchTransform t;
  t.swap_xy = j->get_bool("swap_xy", fallback.swap_xy);
  t.mirror_x = j->get_bool("mirror_x", fallback.mirror_x);
  t.mirror_y = j->get_bool("mirror_y", fallback.mirror_y);
  return t;
}

}  // namespace

Settings& settings() { return g_settings; }

int Settings::font_px(int dpi) const {
  // A "point" is 1/72 inch; e-ink panels report a real DPI, so this gives
  // physically consistent type across the 212..300 dpi Kobo range.
  double px = (double)font_size_pt * (double)dpi / 72.0;
  return std::max(12, (int)std::lround(px));
}

Json Settings::to_json() const {
  Json j = Json::object();
  j["fontFamily"] = Json(font_family);
  j["fontSize"] = Json(font_size_pt);
  j["lineSpacing"] = Json(line_spacing);
  j["screenMargin"] = Json(screen_margin);
  j["paragraphAlignment"] = Json((int)alignment);
  j["hyphenationEnabled"] = Json(hyphenation);
  j["embeddedStyle"] = Json(embedded_style);
  j["forceParagraphIndents"] = Json(force_paragraph_indent);
  j["extraParagraphSpacing"] = Json(extra_paragraph_spacing);
  j["imageRendering"] = Json((int)image_rendering);

  j["focusReadingEnabled"] = Json(focus_reading);
  j["guideDots"] = Json(guide_dots);
  j["autoPageTurnSeconds"] = Json(auto_page_turn_seconds);
  j["statusBarTitle"] = Json(status_bar_title);
  j["statusBarClock"] = Json(status_bar_clock);
  j["autoUpdateCheck"] = Json(auto_update_check);
  j["touchCalibrated"] = Json(touch_calibrated);
  {
    Json list = Json::array();
    for (const Catalogue& c : catalogues) {
      Json entry = Json::object();
      entry["name"] = Json(c.name);
      entry["url"] = Json(c.url);
      if (!c.user.empty()) entry["user"] = Json(c.user);
      if (!c.password.empty()) entry["password"] = Json(c.password);
      list.push_back(entry);
    }
    j["catalogues"] = list;
    j["catalogueFolder"] = Json(catalogue_folder);
  }
  j["statusBarBattery"] = Json(status_bar_battery);
  j["statusBarProgressBar"] = Json(status_bar_progress_bar);
  j["statusBarProgressBarThickness"] = Json(status_bar_progress_thickness);
  j["statusBarChapterPageCount"] = Json(status_bar_chapter_pages);
  j["statusBarBookProgressPercentage"] = Json(status_bar_percentage);
  j["hideBatteryPercentage"] = Json(hide_battery_percentage);
  j["touchReaderControls"] = Json((int)tap_zones);
  j["tapForReaderMenu"] = Json(tap_for_reader_menu);
  j["refreshFrequency"] = Json(refresh_frequency);

  j["showHiddenFiles"] = Json(show_hidden_files);
  j["moveFinishedToReadFolder"] = Json(move_finished_to_read_folder);
  j["removeReadBooksFromRecents"] = Json(remove_read_books_from_recents);
  j["librarySort"] = Json(library_sort);

  j["penWidth"] = Json(pen_width);
  j["penColorIndex"] = Json(pen_color_index);
  j["penPressure"] = Json(pen_pressure);
  j["palmRejection"] = Json(palm_rejection);
  j["noteTemplate"] = Json(note_template);
  j["notesFastInk"] = Json(notes_fast_ink);

  j["sleepTimeoutMinutes"] = Json(sleep_timeout_minutes);
  j["powerOffHours"] = Json(power_off_hours);
  j["sleepScreen"] = Json((int)sleep_screen);
  j["sleepCustomImage"] = Json(sleep_custom_image);
  j["frontlightRestoreOnWake"] = Json(frontlight_restore_on_wake);
  j["frontlightBrightness"] = Json(frontlight_brightness);
  j["frontlightWarmth"] = Json(frontlight_warmth);
  j["frontlightOn"] = Json(frontlight_on);

  j["uiTheme"] = Json((int)theme);
  j["screenInverted"] = Json(night_mode);
  j["cfaMode"] = Json((int)cfa_mode);
  j["saturationBoost"] = Json((double)saturation_boost);
  j["orientation"] = Json(rotation);
  j["clockFormat24h"] = Json(clock_24h);
  j["buttonsSwapped"] = Json(buttons_swapped);
  j["frontButtonFollowOrientation"] = Json(buttons_follow_rotation);
  j["usbAction"] = Json(usb_action);
  j["touchTransform"] = transform_to_json(touch_transform);
  j["penTransform"] = transform_to_json(pen_transform);
  j["logDebug"] = Json(log_debug);
  return j;
}

void Settings::from_json(const Json& j) {
  if (!j.is_object()) return;
  font_family = j.get_string("fontFamily", font_family);
  // Keep the accepted range identical to what the settings screen
  // offers, so a hand-edited file cannot reach a size the UI then refuses
  // to show.
  font_size_pt = std::max(8, std::min(24, j.get_int("fontSize", font_size_pt)));
  line_spacing = std::max(100, std::min(220, j.get_int("lineSpacing", line_spacing)));
  screen_margin = std::max(0, std::min(160, j.get_int("screenMargin", screen_margin)));
  alignment = enum_from(j, "paragraphAlignment", alignment, 2);
  hyphenation = j.get_bool("hyphenationEnabled", hyphenation);
  embedded_style = j.get_bool("embeddedStyle", embedded_style);
  force_paragraph_indent = j.get_bool("forceParagraphIndents", force_paragraph_indent);
  extra_paragraph_spacing = j.get_bool("extraParagraphSpacing", extra_paragraph_spacing);
  image_rendering = enum_from(j, "imageRendering", image_rendering, 3);

  focus_reading = j.get_bool("focusReadingEnabled", focus_reading);
  guide_dots = j.get_bool("guideDots", guide_dots);
  auto_page_turn_seconds = j.get_int("autoPageTurnSeconds", auto_page_turn_seconds);
  if (auto_page_turn_seconds != 0) {
    auto_page_turn_seconds = std::max(5, std::min(120, auto_page_turn_seconds));
  }
  status_bar_title = j.get_bool("statusBarTitle", status_bar_title);
  status_bar_clock = j.get_bool("statusBarClock", status_bar_clock);
  auto_update_check = j.get_bool("autoUpdateCheck", auto_update_check);
  touch_calibrated = j.get_bool("touchCalibrated", touch_calibrated);
  catalogue_folder = j.get_string("catalogueFolder", catalogue_folder);
  if (const Json* list = j.find("catalogues")) {
    catalogues.clear();
    for (const Json& entry : list->items()) {
      Catalogue c;
      c.name = entry.get_string("name");
      c.url = entry.get_string("url");
      c.user = entry.get_string("user");
      c.password = entry.get_string("password");
      if (!c.url.empty()) catalogues.push_back(c);
    }
  }
  status_bar_battery = j.get_bool("statusBarBattery", status_bar_battery);
  status_bar_progress_bar = j.get_bool("statusBarProgressBar", status_bar_progress_bar);
  status_bar_progress_thickness =
      std::max(1, std::min(10, j.get_int("statusBarProgressBarThickness",
                                        status_bar_progress_thickness)));
  status_bar_chapter_pages = j.get_bool("statusBarChapterPageCount", status_bar_chapter_pages);
  status_bar_percentage = j.get_bool("statusBarBookProgressPercentage", status_bar_percentage);
  hide_battery_percentage = j.get_bool("hideBatteryPercentage", hide_battery_percentage);
  tap_zones = enum_from(j, "touchReaderControls", tap_zones, 3);
  tap_for_reader_menu = j.get_bool("tapForReaderMenu", tap_for_reader_menu);
  refresh_frequency = std::max(0, std::min(60, j.get_int("refreshFrequency", refresh_frequency)));

  show_hidden_files = j.get_bool("showHiddenFiles", show_hidden_files);
  move_finished_to_read_folder =
      j.get_bool("moveFinishedToReadFolder", move_finished_to_read_folder);
  remove_read_books_from_recents =
      j.get_bool("removeReadBooksFromRecents", remove_read_books_from_recents);
  library_sort = j.get_string("librarySort", library_sort);

  pen_width = std::max(1, std::min(12, j.get_int("penWidth", pen_width)));
  pen_color_index = j.get_int("penColorIndex", pen_color_index);
  pen_pressure = j.get_bool("penPressure", pen_pressure);
  palm_rejection = j.get_bool("palmRejection", palm_rejection);
  note_template = j.get_string("noteTemplate", note_template);
  notes_fast_ink = j.get_bool("notesFastInk", notes_fast_ink);

  sleep_timeout_minutes =
      std::max(0, std::min(240, j.get_int("sleepTimeoutMinutes", sleep_timeout_minutes)));
  power_off_hours = std::max(0, std::min(72, j.get_int("powerOffHours", power_off_hours)));
  sleep_screen = enum_from(j, "sleepScreen", sleep_screen, 5);
  sleep_custom_image = j.get_string("sleepCustomImage", sleep_custom_image);
  frontlight_restore_on_wake =
      j.get_bool("frontlightRestoreOnWake", frontlight_restore_on_wake);
  frontlight_brightness =
      std::max(0, std::min(100, j.get_int("frontlightBrightness", frontlight_brightness)));
  frontlight_warmth = std::max(0, std::min(100, j.get_int("frontlightWarmth", frontlight_warmth)));
  frontlight_on = j.get_bool("frontlightOn", frontlight_on);

  theme = enum_from(j, "uiTheme", theme, 4);
  night_mode = j.get_bool("screenInverted", night_mode);
  cfa_mode = enum_from(j, "cfaMode", cfa_mode, 6);
  saturation_boost = (float)std::max(-1.0, std::min(1.0, j.get_double("saturationBoost",
                                                                      saturation_boost)));
  rotation = ((j.get_int("orientation", rotation) % 4) + 4) % 4;
  clock_24h = j.get_bool("clockFormat24h", clock_24h);
  buttons_swapped = j.get_bool("buttonsSwapped", buttons_swapped);
  buttons_follow_rotation =
      j.get_bool("frontButtonFollowOrientation", buttons_follow_rotation);
  usb_action = j.get_string("usbAction", usb_action);
  touch_transform = transform_from_json(j.find("touchTransform"), touch_transform);
  pen_transform = transform_from_json(j.find("penTransform"), pen_transform);
  log_debug = j.get_bool("logDebug", log_debug);
}

void Settings::apply_touch_defaults() {
  const DeviceInfo& dev = device();
  touch_transform = TouchTransform();
  // A starting guess only. The panel's transposition is worked out from the
  // kernel's own axis ranges, so this is just which way each axis runs;
  // "Calibrate touch" settles it properly in three taps, and once it has,
  // this is never applied again.
  if (dev.codename.find("monza") != std::string::npos) {
    touch_transform.mirror_x = true;
  }
}

void Settings::apply_device_defaults() {
  const DeviceInfo& dev = device();
  apply_touch_defaults();
  if (!dev.has_frontlight) frontlight_on = false;
}

void Settings::load() {
  Json j;
  if (!Json::parse_file(paths().settings_file(), j)) {
    CK_LOGI("settings: no settings file yet, using defaults for %s",
            device().codename.c_str());
    apply_device_defaults();
    return;
  }
  from_json(j);
  // Until the user has calibrated, the touch transform is ours to guess -
  // and an earlier build's guess should not outlive it.
  if (!touch_calibrated) apply_touch_defaults();
  CK_LOGI("settings: loaded from %s", paths().settings_file().c_str());
}

void Settings::save() const {
  if (!to_json().save_file(paths().settings_file(), true)) {
    CK_LOGW("settings: could not write %s", paths().settings_file().c_str());
  }
}

}  // namespace ck
