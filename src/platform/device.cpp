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

  // PLATFORM is exported by the firmware's rcS *after* our boot hook runs,
  // so when CrossKobo starts the whole show it has to work this out for
  // itself. The driver directory is named after the platform, which is the
  // most reliable source: /drivers/mt8113t-ntx, /drivers/mx6sll-ntx, ...
  d.platform = getenv("PLATFORM") ? getenv("PLATFORM") : "";
  if (d.platform.empty()) {
    std::vector<fs::Entry> drivers = fs::list_dir("/drivers");
    for (const fs::Entry& e : drivers) {
      if (!e.is_dir) continue;
      // Skip the generic directories some firmware ships alongside.
      if (e.name == "common" || e.name == "wifi") continue;
      d.platform = e.name;
      if (e.name.find("-ntx") != std::string::npos) break;
    }
  }
  if (!d.platform.empty()) setenv("PLATFORM", d.platform.c_str(), 0);
  if (!d.codename.empty()) setenv("PRODUCT", d.codename.c_str(), 0);

  d.is_mtk = starts_with(d.platform, "mt") || fs::exists("/dev/wmtWifi") ||
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

  // ------------------------------------------------------------------ Wi-Fi
  // The module name is not guessable, so look for the .ko the firmware
  // ships. MediaTek boards keep theirs in an mt66xx subdirectory.
  const char* kWifiModules[] = {"wlan_drv_gen4m", "moal",   "8821cs", "8189fs",
                                "8192ee",         "dhd",    "wifi_drv"};
  std::vector<std::string> wifi_dirs = {"/drivers/" + d.platform + "/mt66xx",
                                        "/drivers/" + d.platform + "/wifi",
                                        "/drivers/" + d.platform};
  for (const std::string& dir : wifi_dirs) {
    if (!fs::is_dir(dir)) continue;
    for (const char* mod : kWifiModules) {
      if (!fs::exists(dir + "/" + mod + ".ko")) continue;
      d.wifi_module = mod;
      d.wifi_module_dir = dir;
      break;
    }
    if (!d.wifi_module.empty()) break;
  }
  // The interface name appears once the module is loaded; if it is already
  // up we can read it straight from sysfs.
  for (const fs::Entry& e : fs::list_dir("/sys/class/net")) {
    if (e.name == "lo" || starts_with(e.name, "usb") || starts_with(e.name, "rndis")) continue;
    if (fs::exists(e.path + "/wireless") || fs::exists(e.path + "/phy80211") ||
        starts_with(e.name, "wlan")) {
      d.wifi_interface = e.name;
      break;
    }
  }
  if (d.wifi_interface.empty()) {
    const char* iface = getenv("INTERFACE");
    d.wifi_interface = iface && *iface ? iface : (d.is_mtk ? "wlan0" : "eth0");
  }

  // -------------------------------------------------------------------- USB
  // The user partition is the twelfth on MediaTek boards and the third on
  // the older NXP ones; check before trusting either.
  for (const char* candidate : {"/dev/mmcblk0p12", "/dev/mmcblk0p3", "/dev/mmcblk0p4"}) {
    if (fs::exists(candidate)) {
      d.user_partition = candidate;
      break;
    }
  }
  // Prefer whatever is actually mounted at onboard, which is authoritative.
  std::string mounts;
  if (fs::read_file("/proc/mounts", mounts)) {
    for (const std::string& line : split(mounts, '\n')) {
      std::vector<std::string> fields = split(line, ' ');
      if (fields.size() < 2) continue;
      if (fields[1] == "/mnt/onboard") d.user_partition = fields[0];
      if (fields[1] == "/mnt/sd") d.sd_partition = fields[0];
    }
  }
  if (d.sd_partition.empty() && fs::exists("/dev/mmcblk1p1")) d.sd_partition = "/dev/mmcblk1p1";
  // Firmware 5 renamed the gadget; the init script only exists there.
  d.usb_gadget_name = fs::exists("/etc/init.d/usb-gadget") ? "kobo" : "g1";
  for (const fs::Entry& e : fs::list_dir("/sys/class/udc")) {
    d.usb_udc = e.name;
    break;
  }
  d.serial = read_version_field(0);

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
  if (!platform.empty()) out += " platform=" + platform;
  if (!wifi_module.empty()) out += " wifi=" + wifi_module + "/" + wifi_interface;
  if (!user_partition.empty()) out += " onboard=" + user_partition;
  if (!usb_udc.empty()) out += " udc=" + usb_udc;
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
