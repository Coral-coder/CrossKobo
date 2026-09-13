#pragma once
#include "ui/view.h"

namespace ck {

ViewPtr make_home_screen();
ViewPtr make_settings_screen();
ViewPtr make_stats_screen();
ViewPtr make_about_screen();
// Touch and stylus diagnostics: shows where input lands and lets the user
// correct mirrored or transposed digitisers without a computer.
ViewPtr make_calibration_screen();

}  // namespace ck
