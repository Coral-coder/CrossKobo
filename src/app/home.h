#pragma once
#include "ui/view.h"

namespace ck {

ViewPtr make_home_screen();
ViewPtr make_settings_screen();
ViewPtr make_stats_screen();
ViewPtr make_network_screen();
// Shown while the drive is shared with a computer, and the chooser that
// appears when the cable goes in.
ViewPtr make_usb_active_screen();
ViewPtr make_usb_prompt_screen();
ViewPtr make_about_screen();
// The quick panel: light, night mode, Wi-Fi and the way home, reached by a
// swipe down from the top edge of any screen.
ViewPtr make_quick_panel();
// Saved OPDS catalogues, and the browser that walks them.
// `name` opens that saved catalogue directly, for a menu launch.
ViewPtr make_catalogue_screen();
ViewPtr make_catalogue_screen(const std::string& name);
// Touch and stylus diagnostics: shows where input lands and lets the user
// correct mirrored or transposed digitisers without a computer.
ViewPtr make_calibration_screen();
// Two-tap calibration that works from the digitiser's raw coordinates, so
// it can fix a panel whose taps land nowhere near where they are drawn.
// Reachable with both page buttons pressed together, from any screen.
ViewPtr make_touch_wizard();
// Wireless transfer: the address and key for a browser on the same network.
ViewPtr make_transfer_screen();

}  // namespace ck
