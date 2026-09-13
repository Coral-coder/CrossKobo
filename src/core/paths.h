#pragma once
#include <string>

namespace ck {

// Every path CrossKobo touches, in one place. On the device the program
// lives on the rootfs (which survives a firmware update only if reinstalled)
// while all user data lives on the FAT32 "onboard" partition the user sees
// over USB, so notebooks and settings are reachable from a PC.
struct Paths {
  std::string onboard = "/mnt/onboard";           // user-visible storage
  std::string sdcard = "/mnt/sd";                 // second card, if present
  std::string install = "/usr/local/crosskobo";   // binary + assets
  std::string data = "/mnt/onboard/.crosskobo";   // settings, state, caches
  std::string notebooks = "/mnt/onboard/Notebooks";
  std::string fonts_user = "/mnt/onboard/fonts";
  std::string fonts_bundled = "/usr/local/crosskobo/fonts";
  std::string fonts_nickel = "/usr/local/Kobo/fonts";

  std::string settings_file() const { return data + "/settings.json"; }
  std::string state_file() const { return data + "/state.json"; }
  std::string stats_file() const { return data + "/stats.json"; }
  std::string recents_file() const { return data + "/recent.json"; }
  std::string cache_dir() const { return data + "/cache"; }
  std::string log_file() const { return data + "/crosskobo.log"; }
  std::string screenshots() const { return onboard + "/Screenshots"; }
  // Presence of this file tells the boot hook to stand down and let the
  // stock Kobo UI start instead. It is the user's escape hatch over USB.
  std::string disable_flag() const { return data + "/DISABLE"; }
};

// The global instance, adjusted by main() when running under the simulator.
Paths& paths();
void set_paths(const Paths& p);

}  // namespace ck
