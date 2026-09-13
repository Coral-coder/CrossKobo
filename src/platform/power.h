#pragma once
#include <string>

namespace ck {

struct BatteryState {
  int percent = -1;
  bool charging = false;
  bool charged = false;
  bool present = false;
};

// Frontlight, battery and suspend. All of it is sysfs poking that fails
// softly: a missing knob just means the feature is unavailable.
class Power {
 public:
  static Power& instance();

  void init();

  // ------------------------------------------------------------ frontlight
  bool has_frontlight() const;
  bool has_warmth() const;
  int brightness() const { return brightness_; }   // 0..100
  int warmth() const { return warmth_; }           // 0..100
  void set_brightness(int percent);
  void set_warmth(int percent);
  void set_frontlight_on(bool on);
  bool frontlight_on() const { return light_on_; }

  // --------------------------------------------------------------- battery
  BatteryState battery();

  // --------------------------------------------------------------- suspend
  // Kobo's kernel wants /sys/power/state-extended flipped first, a short
  // pause, then the usual write to /sys/power/state. Suspending while
  // charging hangs MediaTek devices, so that case is refused.
  bool suspend();
  bool can_suspend();
  void reboot();
  void power_off();

  // Charging LED, where the hardware has one.
  void set_charging_led(bool on);

 private:
  Power() = default;
  bool write_sysfs(const std::string& path, const std::string& value);
  std::string read_sysfs(const std::string& path);

  int brightness_ = 0;
  int warmth_ = 0;
  bool light_on_ = false;
  int saved_brightness_ = 10;
  int fl_max_ = 100;
  bool warmth_inverted_ = true;
  int warmth_max_ = 10;
  bool initialised_ = false;
};

}  // namespace ck
