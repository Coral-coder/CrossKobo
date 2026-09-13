#include "platform/power.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"

namespace ck {

Power& Power::instance() {
  static Power p;
  return p;
}

std::string Power::read_sysfs(const std::string& path) {
  std::string out;
  if (path.empty() || !fs::read_file(path, out)) return "";
  return trim(out);
}

bool Power::write_sysfs(const std::string& path, const std::string& value) {
  if (path.empty()) return false;
  FILE* f = fopen(path.c_str(), "we");
  if (!f) {
    CK_LOGW("power: cannot write %s", path.c_str());
    return false;
  }
  fputs(value.c_str(), f);
  bool ok = fflush(f) == 0;
  fclose(f);
  return ok;
}

void Power::init() {
  if (initialised_) return;
  initialised_ = true;
  const DeviceInfo& dev = device();

  if (!dev.frontlight_brightness.empty()) {
    // The sysfs "max_brightness" sibling tells us the scale; Kobo panels
    // are usually 0..100 but some boards are 0..255.
    std::string dir = fs::dirname(dev.frontlight_brightness);
    std::string max = read_sysfs(dir + "/max_brightness");
    fl_max_ = std::max(1, to_int(max, 100));
    int raw = to_int(read_sysfs(dev.frontlight_brightness), 0);
    brightness_ = (int)std::lround((double)raw * 100.0 / (double)fl_max_);
    light_on_ = brightness_ > 0;
    if (light_on_) saved_brightness_ = brightness_;
  }
  if (!dev.frontlight_warmth.empty()) {
    // On Kobo the "color" knob counts down from warmest; the natural-light
    // scale is 0..10 on current hardware.
    int raw = to_int(read_sysfs(dev.frontlight_warmth), 0);
    warmth_ = warmth_inverted_ ? (warmth_max_ - raw) * 100 / warmth_max_
                               : raw * 100 / warmth_max_;
    warmth_ = std::max(0, std::min(100, warmth_));
  }
  CK_LOGI("power: frontlight=%s warmth=%s brightness=%d%%",
          has_frontlight() ? "yes" : "no", has_warmth() ? "yes" : "no", brightness_);
}

bool Power::has_frontlight() const { return !device().frontlight_brightness.empty(); }
bool Power::has_warmth() const { return !device().frontlight_warmth.empty(); }

void Power::set_brightness(int percent) {
  percent = std::max(0, std::min(100, percent));
  brightness_ = percent;
  if (percent > 0) saved_brightness_ = percent;
  light_on_ = percent > 0;
  int raw = (int)std::lround((double)percent * (double)fl_max_ / 100.0);
  write_sysfs(device().frontlight_brightness, format("%d", raw));
}

void Power::set_warmth(int percent) {
  percent = std::max(0, std::min(100, percent));
  warmth_ = percent;
  int raw = (int)std::lround((double)percent * (double)warmth_max_ / 100.0);
  if (warmth_inverted_) raw = warmth_max_ - raw;
  write_sysfs(device().frontlight_warmth, format("%d", raw));
}

void Power::set_frontlight_on(bool on) {
  if (on) {
    set_brightness(saved_brightness_ > 0 ? saved_brightness_ : 10);
  } else {
    int keep = brightness_;
    set_brightness(0);
    saved_brightness_ = keep > 0 ? keep : saved_brightness_;
    light_on_ = false;
  }
}

BatteryState Power::battery() {
  BatteryState st;
  const std::string& dir = device().battery_dir;
  if (dir.empty()) return st;
  st.present = true;
  st.percent = to_int(read_sysfs(dir + "/capacity"), -1);
  std::string status = to_lower(read_sysfs(dir + "/status"));
  st.charging = status == "charging";
  st.charged = status == "full";
  return st;
}

bool Power::can_suspend() {
  if (!fs::exists("/sys/power/state")) return false;
  // A MediaTek Kobo hangs in the kernel if asked to suspend while plugged
  // in, so never try. (Matches the workaround KOReader carries.)
  if (device().is_mtk) {
    BatteryState st = battery();
    if (st.charging || st.charged) return false;
  }
  return true;
}

bool Power::suspend() {
  if (!can_suspend()) {
    CK_LOGI("power: suspend refused (charging or unsupported)");
    return false;
  }
  CK_LOGI("power: suspending");
  if (!write_sysfs("/sys/power/state-extended", "1")) {
    CK_LOGW("power: kernel refused state-extended, not suspending");
    return false;
  }
  // The stock UI waits here too; suspending immediately after flipping the
  // flag is known to wedge some boards.
  sleep_ms(2000);
  sync();
  bool ok = write_sysfs("/sys/power/state", "mem");
  // Execution resumes here after wake-up.
  sleep_ms(100);
  write_sysfs("/sys/power/state-extended", "0");
  CK_LOGI("power: resumed");
  return ok;
}

void Power::reboot() {
  sync();
  if (system("reboot") != 0) CK_LOGW("power: reboot command failed");
}

void Power::power_off() {
  sync();
  if (system("poweroff") != 0) CK_LOGW("power: poweroff command failed");
}

void Power::set_charging_led(bool on) {
  write_sysfs(device().charging_led, on ? "255" : "0");
}

}  // namespace ck
