#include "platform/device.h"

#include <cstdio>
#include <cstdlib>

#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"

namespace ck {
namespace {

DeviceInfo g_device;
bool g_probed = false;
bool g_overridden = false;

// Kobo's /mnt/onboard/.kobo/version is a comma separated line:
//   serial,unused,firmware,...,model-id
std::string read_version_field(int index) {
  std::string text;
  if (!fs::read_file("/mnt/onboard/.kobo/version", text)) return "";
  std::vector<std::string> parts = split(trim(text), ',');
  if (index < 0 || (size_t)index >= parts.size()) return "";
  return trim(parts[index]);
}

std::string run_capture(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "re");
  if (!p) return "";
  std::string out;
  char buf[256];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  pclose(p);
  return trim(out);
}

// ntx_hwconfig reads the board's hardware description block from the boot
// partition. It is how the stock scripts themselves decide whether a panel
// has a colour filter array.
bool probe_color_panel() {
  if (!fs::exists("/dev/mmcblk0p6")) return false;
  std::string out = run_capture("ntx_hwconfig -S 1 -p /dev/mmcblk0p6 EPD_Flags CFA 2>/dev/null");
  return to_lower(out).find("on") != std::string::npos;
}

std::string first_existing(const std::vector<std::string>& candidates) {
  for (const std::string& c : candidates) {
    if (fs::exists(c)) return c;
  }
  return "";
}

void probe() {
  DeviceInfo d;
  d.is_kobo = fs::exists("/mnt/onboard/.kobo") || fs::exists("/usr/local/Kobo");
  if (!d.is_kobo) {
    // Host build: pretend to be a Libra Colour so layout maths in tests
    // match the real device.
    d.codename = "monza";
    d.model = "Kobo Libra Colour (host)";
    d.has_color_panel = true;
    d.has_frontlight = true;
    d.has_natural_light = true;
    d.has_page_buttons = true;
    d.has_stylus = true;
    d.dpi = 300;
    g_device = d;
    return;
  }

  d.firmware = read_version_field(2);
  d.codename = run_capture("/bin/sh /bin/kobo_config.sh 2>/dev/null");
  if (d.codename.empty()) d.codename = run_capture("hwdetect.sh 2>/dev/null");
  if (d.codename.empty()) d.codename = "unknown";

  // Firmware 5.x replaced on-animator.sh with animator.sh plus rcS.d.
  if (fs::exists("/etc/init.d/on-animator.sh")) {
    d.layout = FirmwareLayout::V4;
  } else if (fs::is_dir("/etc/rcS.d") || fs::exists("/usr/bin/animator.sh")) {
    d.layout = FirmwareLayout::V5;
  }

  d.platform = getenv("PLATFORM") ? getenv("PLATFORM") : "";
  if (d.platform.empty()) {
    // b300-ntx is the MediaTek platform directory name.
    if (fs::exists("/etc/u-boot/b300-ntx/u-boot.mmc")) d.platform = "b300-ntx";
  }
  d.is_mtk = d.platform == "b300-ntx" || fs::exists("/dev/hwtcon") ||
             fs::exists("/proc/hwtcon") || fs::exists("/usr/local/Kobo/pickel-mtk");
  d.has_color_panel = probe_color_panel();

  d.frontlight_brightness = first_existing({
      "/sys/class/backlight/mxc_msp430.0/brightness",
      "/sys/class/backlight/mxc_msp430_fl.0/brightness",
      "/sys/class/backlight/backlight_fl/brightness",
      "/sys/class/backlight/lm3630a_led/brightness",
  });
  d.frontlight_warmth = first_existing({
      "/sys/class/backlight/lm3630a_led/color",
      "/sys/class/leds/aw99703-bl_FL1/color",
      "/sys/class/backlight/tlc5947_bl/color",
  });
  d.has_frontlight = !d.frontlight_brightness.empty();
  d.has_natural_light = !d.frontlight_warmth.empty();

  d.battery_dir = first_existing({
      "/sys/class/power_supply/bd71827_bat",
      "/sys/class/power_supply/battery",
      "/sys/class/power_supply/mc13892_bat",
  });
  d.charging_led = first_existing({"/sys/class/leds/LED/brightness",
                                   "/sys/class/leds/bd71828-green-led/brightness"});

  // Per-model traits. The codenames come from Kobo's own hardware config.
  struct Model {
    const char* codename;
    const char* name;
    int dpi;
    bool keys;
    bool stylus;
    bool gsensor;
  };
  static const Model kModels[] = {
      {"monza", "Kobo Libra Colour", 300, true, true, true},
      {"spaColour", "Kobo Clara Colour", 300, false, false, false},
      {"spaBW", "Kobo Clara BW", 300, false, false, false},
      {"condor", "Kobo Elipsa 2E", 227, false, true, true},
      {"cadmus", "Kobo Sage", 300, true, true, true},
      {"europa", "Kobo Libra 2", 300, true, false, true},
      {"io", "Kobo Elipsa", 227, false, true, true},
      {"frost", "Kobo Forma", 300, true, false, true},
      {"storm", "Kobo Libra H2O", 300, true, false, true},
      {"nova", "Kobo Clara HD", 300, false, false, false},
      {"goldfinch", "Kobo Nia", 212, false, false, false},
      {"star", "Kobo Aura 2", 212, false, false, false},
  };
  for (const Model& m : kModels) {
    if (d.codename.find(m.codename) != std::string::npos) {
      d.model = m.name;
      d.dpi = m.dpi;
      d.has_page_buttons = m.keys;
      d.has_stylus = m.stylus;
      d.has_gsensor = m.gsensor;
      break;
    }
  }
  if (d.model == "Kobo") d.model = "Kobo (" + d.codename + ")";

  g_device = d;
}

}  // namespace

std::string DeviceInfo::describe() const {
  std::string out = model;
  if (!firmware.empty()) out += " FW " + firmware;
  out += is_mtk ? " [MTK]" : " [NTX]";
  if (has_color_panel) out += " [colour]";
  if (has_stylus) out += " [stylus]";
  switch (layout) {
    case FirmwareLayout::V4: out += " [fw4]"; break;
    case FirmwareLayout::V5: out += " [fw5]"; break;
    default: break;
  }
  return out;
}

const DeviceInfo& device() {
  if (!g_probed && !g_overridden) {
    g_probed = true;
    probe();
    CK_LOGI("device: %s", g_device.describe().c_str());
  }
  return g_device;
}

void set_device(const DeviceInfo& info) {
  g_device = info;
  g_overridden = true;
  g_probed = true;
}

}  // namespace ck
