#include "platform/power.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"
#include "platform/input.h"

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

  // Let the keys come up first. The release of the power button is itself a
  // wake event, so suspending while it is still in flight turns the sleep
  // into an instant resume - which is exactly what the device did before
  // this: every suspend came back after two or three seconds.
  Input& input = Input::instance();
  for (int i = 0; i < 40 && input.any_key_held(); ++i) sleep_ms(50);
  input.drain();

  if (!write_sysfs("/sys/power/state-extended", "1")) {
    CK_LOGW("power: kernel refused state-extended, not suspending");
    return false;
  }
  // The stock UI waits here too; suspending immediately after flipping the
  // flag is known to wedge some boards.
  sleep_ms(2000);
  sync();

  bool ok = false;
  int64_t slept_ms = 0;
  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
    // The wakeup_count protocol: hand the kernel the count we last saw and
    // it refuses the suspend if anything has happened since. Without this a
    // pending event is not an error, it is an immediate wake.
    std::string count;
    if (fs::read_file("/sys/power/wakeup_count", count)) {
      count = trim(count);
      if (!count.empty() && !write_sysfs("/sys/power/wakeup_count", count)) {
        CK_LOGI("power: wake event pending at count %s, retrying", count.c_str());
        sleep_ms(250);
        continue;
      }
    }
    // "mem" is what every Kobo generation has used; "freeze" is the
    // fallback for a kernel built without suspend-to-RAM.
    const char* kStates[] = {"mem", "freeze"};
    for (const char* state : kStates) {
      int64_t before = now_ms();
      errno = 0;
      if (write_sysfs("/sys/power/state", state)) {
        // Execution resumes here after wake-up.
        slept_ms = now_ms() - before;
        CK_LOGI("power: back from %s after %lld ms", state, (long long)slept_ms);
        ok = true;
        break;
      }
      CK_LOGW("power: /sys/power/state=%s refused (%s)", state, strerror(errno));
    }
  }
  if (ok && slept_ms < 1000) {
    // The write returned at once: something woke the device straight away,
    // or the kernel never froze. Worth saying so, because it looks to the
    // user like sleep simply does not work.
    CK_LOGW("power: the kernel returned from suspend immediately");
  }
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

void Power::stop_boot_led() {
  int found = 0;
  for (const fs::Entry& e : fs::list_dir("/sys/class/leds")) {
    // Order matters: a hardware blink keeps running until its trigger is
    // detached, and only then does brightness stick.
    write_sysfs(e.path + "/trigger", "none");
    write_sysfs(e.path + "/delay_on", "0");
    write_sysfs(e.path + "/delay_off", "0");
    write_sysfs(e.path + "/blink", "0");
    write_sysfs(e.path + "/brightness", "0");
    ++found;
    CK_LOGI("power: led %s off", e.name.c_str());
  }
  // Older NTX boards drive the indicator through one node, with a channel
  // per colour, instead of the LED class.
  const char* kNtx = "/sys/devices/platform/ntx_led/lit";
  if (fs::exists(kNtx)) {
    for (int ch = 3; ch <= 4; ++ch) {
      write_sysfs(kNtx, format("ch %d cur 0", ch));
    }
    ++found;
  }
  if (found == 0) CK_LOGI("power: no LED nodes to quieten");
}

void Power::set_charging_led(bool on) {
  write_sysfs(device().charging_led, on ? "255" : "0");
}

}  // namespace ck
