// Settings and JSON round-trips. Settings are the one file users hand-edit,
// so out-of-range and corrupt values must degrade to defaults instead of
// breaking the shell.
#include <cstdio>

#include "app/settings.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"

using namespace ck;

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

int main() {
  log_init("", LogLevel::Error);

  // ---------------------------------------------------------------- JSON
  Json j;
  std::string error;
  CHECK(Json::parse(R"({"a":1,"b":[1,2,3],"c":{"d":"x"},"e":true,"f":null,
                        "g":-1.5,"h":"é😀"})",
                    j, &error));
  CHECK(j.get_int("a") == 1);
  CHECK(j.find("b") && j.find("b")->size() == 3);
  CHECK(j.find("b")->at(2).as_int() == 3);
  CHECK(j.find("c")->get_string("d") == "x");
  CHECK(j.get_bool("e"));
  CHECK(j.find("f")->is_null());
  CHECK(j.get_double("g") == -1.5);
  // Escaped code points, including a surrogate pair, must survive as UTF-8.
  CHECK(j.get_string("h") == "\xC3\xA9\xF0\x9F\x98\x80");

  // A dump must parse back to the same values.
  Json again;
  CHECK(Json::parse(j.dump(true), again, &error));
  CHECK(again.get_int("a") == 1);
  CHECK(again.get_string("h") == j.get_string("h"));

  // Malformed input fails cleanly rather than crashing.
  Json bad;
  CHECK(!Json::parse("{\"a\":", bad, &error));
  CHECK(!error.empty());
  CHECK(!Json::parse("", bad, nullptr));
  // Missing keys fall back.
  CHECK(j.get_int("nope", 42) == 42);
  CHECK(j.get_string("nope", "d") == "d");

  // ------------------------------------------------------------ settings
  std::string root = "out/settings-test";
  fs::remove_tree(root);
  Paths p;
  p.onboard = root;
  p.data = root + "/.crosskobo";
  set_paths(p);

  Settings& s = settings();
  s.font_family = "Literata";
  s.font_size_pt = 14;
  s.line_spacing = 155;
  s.screen_margin = 56;
  s.alignment = Alignment::Left;
  s.hyphenation = false;
  s.theme = UiTheme::Dashboard;
  s.cfa_mode = CfaMode::Boost;
  s.saturation_boost = 0.3f;
  s.pen_width = 8;
  s.pen_color_index = 5;
  s.note_template = "grid";
  s.auto_page_turn_seconds = 30;
  s.touch_transform.mirror_y = true;
  s.pen_transform.swap_xy = true;
  s.usb_action = "ignore";
  s.save();
  CHECK(fs::exists(paths().settings_file()));

  Settings fresh;
  Json stored;
  CHECK(Json::parse_file(paths().settings_file(), stored));
  fresh.from_json(stored);
  CHECK(fresh.font_family == "Literata");
  CHECK(fresh.font_size_pt == 14);
  CHECK(fresh.line_spacing == 155);
  CHECK(fresh.screen_margin == 56);
  CHECK(fresh.alignment == Alignment::Left);
  CHECK(!fresh.hyphenation);
  CHECK(fresh.theme == UiTheme::Dashboard);
  CHECK(fresh.cfa_mode == CfaMode::Boost);
  CHECK(fresh.saturation_boost > 0.29f && fresh.saturation_boost < 0.31f);
  CHECK(fresh.pen_width == 8);
  CHECK(fresh.pen_color_index == 5);
  CHECK(fresh.note_template == "grid");
  CHECK(fresh.auto_page_turn_seconds == 30);
  CHECK(fresh.touch_transform.mirror_y);
  CHECK(fresh.pen_transform.swap_xy);
  CHECK(fresh.usb_action == "ignore");

  // Nonsense values must be clamped, not obeyed.
  Json hostile = Json::object();
  hostile["fontSize"] = Json(9999);
  hostile["lineSpacing"] = Json(-40);
  hostile["screenMargin"] = Json(100000);
  hostile["uiTheme"] = Json(77);
  hostile["cfaMode"] = Json(-3);
  hostile["autoPageTurnSeconds"] = Json(1);
  hostile["refreshFrequency"] = Json(9999);
  hostile["saturationBoost"] = Json(12.0);
  hostile["penWidth"] = Json(0);
  Settings clamped;
  UiTheme theme_before = clamped.theme;
  clamped.from_json(hostile);
  CHECK(clamped.font_size_pt <= 24);
  CHECK(clamped.line_spacing >= 100);
  CHECK(clamped.screen_margin <= 160);
  CHECK(clamped.theme == theme_before);       // invalid enum keeps the default
  CHECK(clamped.cfa_mode == CfaMode::Default);
  CHECK(clamped.auto_page_turn_seconds == 5);  // snapped into range
  CHECK(clamped.refresh_frequency <= 60);
  CHECK(clamped.saturation_boost <= 1.0f);
  CHECK(clamped.pen_width >= 1);

  // Point sizes must scale with panel density.
  CHECK(clamped.font_px(300) > clamped.font_px(212));

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
