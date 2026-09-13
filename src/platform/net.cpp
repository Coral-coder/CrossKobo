#include "platform/net.h"

#include <algorithm>
#include <cstdlib>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"
#include "platform/system.h"

namespace ck {
namespace {

constexpr int kAssociationTimeoutMs = 25000;
constexpr int kDhcpTimeoutMs = 30000;

// wpa_supplicant's control socket directory, where both the stock software
// and wpa_cli expect to find it.
constexpr const char* kCtrlDir = "/var/run/wpa_supplicant";

std::string supplicant_conf() {
  // Firmware 5 moved the config onto the user partition. Prefer whichever
  // exists so saved networks are shared with the stock software.
  if (fs::exists("/mnt/onboard/.kobo/wpa_supplicant.conf")) {
    return "/mnt/onboard/.kobo/wpa_supplicant.conf";
  }
  return "/etc/wpa_supplicant/wpa_supplicant.conf";
}

bool module_loaded(const std::string& name) {
  std::string modules;
  if (!fs::read_file("/proc/modules", modules)) return false;
  return modules.find(name + " ") != std::string::npos;
}

void insmod_if_needed(const std::string& dir, const std::string& module,
                      const std::string& params = "") {
  if (module_loaded(module)) return;
  std::string path = dir + "/" + module + ".ko";
  if (!fs::exists(path)) {
    // Some firmware builds these in; that is not an error.
    CK_LOGD("net: %s not found, assuming it is built in", path.c_str());
    return;
  }
  sys::run("insmod " + path + (params.empty() ? "" : " " + params) + " 2>/dev/null");
  sleep_ms(250);
}

// The regulatory domain the stock software was told to use. Getting this
// wrong can make channels invisible.
std::string country_code() {
  std::string conf;
  if (!fs::read_file("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf", conf)) return "";
  for (const std::string& line : split(conf, '\n')) {
    if (starts_with(line, "WifiRegulatoryDomain=")) return trim(line.substr(21));
  }
  return "";
}

int quality_from_dbm(int dbm) {
  // The usual rule of thumb: -50 dBm and up is full strength, -100 is none.
  if (dbm >= -50) return 100;
  if (dbm <= -100) return 0;
  return (dbm + 100) * 2;
}

}  // namespace

Net& Net::instance() {
  static Net n;
  return n;
}

bool Net::available() const {
  if (simulated_) return true;
  const DeviceInfo& dev = device();
  if (!dev.is_kobo) return false;
  return !dev.wifi_module.empty() || fs::exists("/sys/class/net/" + dev.wifi_interface);
}

std::string Net::wpa_cli(const std::string& command) const {
  const DeviceInfo& dev = device();
  std::string cmd = format("wpa_cli -p %s -i %s %s 2>/dev/null", kCtrlDir,
                           dev.wifi_interface.c_str(), command.c_str());
  return sys::run_capture(cmd);
}

bool Net::load_modules() {
  const DeviceInfo& dev = device();
  if (dev.wifi_module.empty()) {
    status_ = "No Wi-Fi driver found for this device";
    return false;
  }
  const std::string& dir = dev.wifi_module_dir;

  if (dev.wifi_module == "wlan_drv_gen4m") {
    // MediaTek: the connectivity stack goes up first, then a debug-channel
    // handshake, then the chip is powered through /dev/wmtWifi. The stock
    // software does exactly this, sleeps included.
    insmod_if_needed(dir, "wmt_drv");
    insmod_if_needed(dir, "wmt_chrdev_wifi");
    insmod_if_needed(dir, "wmt_cdev_bt");
    insmod_if_needed(dir, dev.wifi_module);

    sys::run("echo \"0xDB9DB9\" > /proc/driver/wmt_dbg 2>/dev/null");
    sys::run("echo \"7 9 0\" > /proc/driver/wmt_dbg 2>/dev/null");
    sleep_ms(1000);
    sys::run("echo \"0xDB9DB9\" > /proc/driver/wmt_dbg 2>/dev/null");
    sys::run("echo \"7 9 1\" > /proc/driver/wmt_dbg 2>/dev/null");
    sys::run("echo 1 > /dev/wmtWifi 2>/dev/null");
    sleep_ms(2000);
  } else if (dev.wifi_module == "moal") {
    // NXP 88W8987: a submodule first, and the driver needs to be told which
    // mode to come up in.
    insmod_if_needed(dir, "mlan");
    std::string params = "mod_para=nxp/wifi_mod_para_sd8987.conf";
    std::string cc = country_code();
    if (!cc.empty()) params += " reg_alpha2=" + cc;
    insmod_if_needed(dir, dev.wifi_module, params);
    sleep_ms(2000);
  } else {
    // The older NTX boards: a power-control module, then the driver.
    insmod_if_needed(dir, "sdio_wifi_pwr");
    std::string params;
    std::string cc = country_code();
    if (!cc.empty() && dev.wifi_module == "8821cs") params = "rtw_country_code=" + cc;
    insmod_if_needed(dir, dev.wifi_module, params);
    sleep_ms(1000);
  }

  modules_loaded_ = true;

  // The interface only exists once the driver is up, so re-read its name.
  std::string iface = dev.wifi_interface;
  if (!fs::exists("/sys/class/net/" + iface)) {
    for (const fs::Entry& e : fs::list_dir("/sys/class/net")) {
      if (e.name == "lo") continue;
      if (starts_with(e.name, "wlan") || fs::exists(e.path + "/wireless")) {
        DeviceInfo updated = dev;
        updated.wifi_interface = e.name;
        set_device(updated);
        CK_LOGI("net: interface is %s", e.name.c_str());
        break;
      }
    }
  }

  sys::run("ifconfig " + device().wifi_interface + " up 2>/dev/null");
  if (device().wifi_module == "dhd") {
    sys::run("wlarm_le -i " + device().wifi_interface + " up 2>/dev/null");
  }
  return true;
}

void Net::unload_modules() {
  const DeviceInfo& dev = device();
  sys::run("ifconfig " + dev.wifi_interface + " down 2>/dev/null");
  if (dev.wifi_module == "wlan_drv_gen4m") {
    // The MediaTek modules are loaded once and stay; powering the chip down
    // through wmtWifi is what actually saves the battery.
    sys::run("echo 0 > /dev/wmtWifi 2>/dev/null");
  } else if (dev.wifi_module == "moal") {
    sys::run("rmmod moal 2>/dev/null");
    sleep_ms(250);
    sys::run("rmmod mlan 2>/dev/null");
  } else if (!dev.wifi_module.empty()) {
    sys::run("rmmod " + dev.wifi_module + " 2>/dev/null");
    sleep_ms(250);
    sys::run("rmmod sdio_wifi_pwr 2>/dev/null");
  }
  modules_loaded_ = false;
}

bool Net::start_supplicant() {
  const DeviceInfo& dev = device();
  if (sys::run("pkill -0 wpa_supplicant 2>/dev/null") == 0) return true;

  std::string conf = supplicant_conf();
  if (!fs::exists(conf)) {
    // Create a minimal config so a first-ever connection can be saved.
    fs::mkdir_p(fs::dirname(conf));
    fs::write_file_atomic(conf,
                          "ctrl_interface=/var/run/wpa_supplicant\n"
                          "update_config=1\n"
                          "ap_scan=1\n");
  }
  // nl80211 for the modern chips, wext for the old Realtek/Broadcom ones.
  const char* driver =
      (dev.wifi_module == "wlan_drv_gen4m" || dev.wifi_module == "moal") ? "nl80211" : "wext";
  std::string cmd = format("wpa_supplicant -D %s -i %s -c %s -C %s -B >/dev/null 2>&1", driver,
                           dev.wifi_interface.c_str(), conf.c_str(), kCtrlDir);
  sys::run(cmd);
  for (int i = 0; i < 40; ++i) {
    if (sys::run("pkill -0 wpa_supplicant 2>/dev/null") == 0) {
      sleep_ms(500);
      return true;
    }
    sleep_ms(250);
  }
  status_ = "wpa_supplicant would not start";
  return false;
}

void Net::stop_supplicant() {
  const DeviceInfo& dev = device();
  sys::run("wpa_cli -p " + std::string(kCtrlDir) + " -i " + dev.wifi_interface +
           " terminate >/dev/null 2>&1");
  sleep_ms(250);
  sys::run("killall -q -TERM wpa_supplicant 2>/dev/null");
}

bool Net::run_dhcp() {
  const DeviceInfo& dev = device();
  release_dhcp();
  // dhcpcd is what the stock software uses; udhcpc is the busybox fallback.
  std::string cmd;
  if (fs::exists("/sbin/dhcpcd")) {
    cmd = format("dhcpcd -d -t %d -w %s >/dev/null 2>&1", kDhcpTimeoutMs / 1000,
                 dev.wifi_interface.c_str());
  } else {
    cmd = format(
        "udhcpc -S -i %s -s /etc/udhcpc.d/default.script -x hostname:kobo -b -q >/dev/null 2>&1",
        dev.wifi_interface.c_str());
  }
  sys::run(cmd);
  for (int i = 0; i < kDhcpTimeoutMs / 500; ++i) {
    if (!ip_address().empty()) return true;
    sleep_ms(500);
  }
  status_ = "No address from DHCP";
  return false;
}

void Net::release_dhcp() {
  const DeviceInfo& dev = device();
  // Keep the resolver configuration: some DHCP clients wipe it on release.
  sys::run("cp -a /etc/resolv.conf /tmp/resolv.crosskobo 2>/dev/null");
  if (fs::exists("/sbin/dhcpcd")) {
    sys::run("dhcpcd -d -k " + dev.wifi_interface + " >/dev/null 2>&1");
  }
  sys::run("killall -q -TERM udhcpc default.script 2>/dev/null");
  std::string resolv;
  if (fs::read_file("/etc/resolv.conf", resolv) && trim(resolv).empty()) {
    sys::run("mv -f /tmp/resolv.crosskobo /etc/resolv.conf 2>/dev/null");
  } else {
    fs::remove_file("/tmp/resolv.crosskobo");
  }
}

std::string Net::ip_address() const {
  if (simulated_) return state_ == NetState::Connected ? "192.168.1.42" : "";
  // Reading it back from the interface is the only honest answer.
  std::string out = sys::run_capture("ip -4 addr show " + device().wifi_interface +
                                     " 2>/dev/null | grep -o 'inet [0-9.]*' | head -1");
  if (!out.empty()) return trim(out.substr(5));
  out = sys::run_capture("ifconfig " + device().wifi_interface +
                         " 2>/dev/null | grep -o 'inet addr:[0-9.]*' | head -1");
  if (!out.empty()) return trim(out.substr(10));
  return "";
}

std::string Net::current_ssid() const {
  if (simulated_) return state_ == NetState::Connected ? simulated_ssid_ : "";
  std::string status = wpa_cli("status");
  for (const std::string& line : split(status, '\n')) {
    if (starts_with(line, "ssid=")) return trim(line.substr(5));
  }
  return "";
}

int Net::current_quality() const {
  if (simulated_) return state_ == NetState::Connected ? 78 : 0;
  std::string signal = wpa_cli("signal_poll");
  for (const std::string& line : split(signal, '\n')) {
    if (starts_with(line, "RSSI=")) return quality_from_dbm(to_int(trim(line.substr(5))));
  }
  return 0;
}

bool Net::power_on() {
  if (state_ == NetState::Connected || state_ == NetState::Connecting) return true;
  if (simulated_) {
    state_ = NetState::Starting;
    status_ = "Wi-Fi ready";
    return true;
  }
  if (!available()) {
    status_ = "This device has no Wi-Fi CrossKobo can drive";
    state_ = NetState::Failed;
    return false;
  }
  state_ = NetState::Starting;
  status_ = "Powering up Wi-Fi";
  CK_LOGI("net: powering up (%s on %s)", device().wifi_module.c_str(),
          device().wifi_interface.c_str());
  if (!load_modules()) {
    state_ = NetState::Failed;
    return false;
  }
  if (!start_supplicant()) {
    state_ = NetState::Failed;
    return false;
  }
  status_ = "Wi-Fi ready";
  return true;
}

void Net::power_off() {
  if (simulated_) {
    state_ = NetState::Off;
    status_ = "Wi-Fi off";
    return;
  }
  CK_LOGI("net: powering down");
  release_dhcp();
  stop_supplicant();
  unload_modules();
  state_ = NetState::Off;
  status_ = "Wi-Fi off";
}

std::vector<WifiNetwork> Net::scan() {
  std::vector<WifiNetwork> out;
  if (state_ == NetState::Off && !power_on()) return out;

  if (simulated_) {
    struct Fake {
      const char* ssid;
      int signal;
      bool secured;
      bool known;
    };
    static const Fake kFakes[] = {
        {"Reading Room", -44, true, true},   {"Kitchen", -58, true, false},
        {"Library Guest", -67, false, false}, {"BT-HH-8821", -74, true, false},
        {"Neighbour's Wi-Fi", -85, true, false},
    };
    for (const Fake& f : kFakes) {
      WifiNetwork net;
      net.ssid = f.ssid;
      net.signal = f.signal;
      net.quality = quality_from_dbm(f.signal);
      net.secured = f.secured;
      net.known = f.known;
      net.current = state_ == NetState::Connected && simulated_ssid_ == f.ssid;
      out.push_back(std::move(net));
    }
    status_ = format("%zu networks found", out.size());
    return out;
  }

  state_ = NetState::Scanning;
  status_ = "Scanning";
  wpa_cli("scan");
  // Scans take a couple of seconds; the results list grows as they arrive.
  sleep_ms(2500);
  std::string results = wpa_cli("scan_results");
  std::vector<std::string> known = saved_networks();
  std::string current = current_ssid();

  bool first_line = true;
  for (const std::string& line : split(results, '\n')) {
    if (first_line) {   // header: bssid / frequency / signal level / flags / ssid
      first_line = false;
      continue;
    }
    if (trim(line).empty()) continue;
    std::vector<std::string> fields = split(line, '\t');
    if (fields.size() < 5) continue;
    WifiNetwork net;
    net.bssid = fields[0];
    net.signal = to_int(fields[2]);
    net.quality = quality_from_dbm(net.signal);
    std::string flags = fields[3];
    net.secured = flags.find("WPA") != std::string::npos ||
                  flags.find("WEP") != std::string::npos;
    net.ssid = fields[4];
    if (net.ssid.empty()) continue;   // hidden network
    net.known = std::find(known.begin(), known.end(), net.ssid) != known.end();
    net.current = !current.empty() && net.ssid == current;
    // Keep only the strongest sighting of each name.
    auto existing = std::find_if(out.begin(), out.end(),
                                 [&](const WifiNetwork& n) { return n.ssid == net.ssid; });
    if (existing != out.end()) {
      if (net.signal > existing->signal) *existing = net;
      continue;
    }
    out.push_back(std::move(net));
  }
  std::sort(out.begin(), out.end(),
            [](const WifiNetwork& a, const WifiNetwork& b) { return a.signal > b.signal; });
  state_ = connected() ? NetState::Connected : NetState::Starting;
  status_ = format("%zu networks found", out.size());
  return out;
}

bool Net::wait_for_association(int timeout_ms) {
  int64_t deadline = now_ms() + timeout_ms;
  while (now_ms() < deadline) {
    std::string status = wpa_cli("status");
    for (const std::string& line : split(status, '\n')) {
      if (!starts_with(line, "wpa_state=")) continue;
      std::string wpa_state = trim(line.substr(10));
      if (wpa_state == "COMPLETED") return true;
      if (wpa_state == "DISCONNECTED" || wpa_state == "INACTIVE") {
        // Keep waiting: the supplicant cycles through these while trying.
      }
    }
    sleep_ms(500);
  }
  return false;
}

bool Net::connect(const std::string& ssid, const std::string& psk, bool save) {
  if (ssid.empty()) return false;
  if (state_ == NetState::Off && !power_on()) return false;
  if (simulated_) {
    simulated_ssid_ = ssid;
    state_ = NetState::Connected;
    status_ = ssid + " \xC2\xB7 " + ip_address();
    return true;
  }
  state_ = NetState::Connecting;
  status_ = "Connecting to " + ssid;
  CK_LOGI("net: connecting to %s", ssid.c_str());

  // Reuse the saved entry when there is one, so we do not accumulate
  // duplicates in wpa_supplicant's config.
  int id = -1;
  std::string list = wpa_cli("list_networks");
  bool header = true;
  for (const std::string& line : split(list, '\n')) {
    if (header) {
      header = false;
      continue;
    }
    std::vector<std::string> fields = split(line, '\t');
    if (fields.size() >= 2 && fields[1] == ssid) {
      id = to_int(fields[0], -1);
      break;
    }
  }
  if (id < 0) {
    id = to_int(trim(wpa_cli("add_network")), -1);
    if (id < 0) {
      status_ = "wpa_supplicant refused a new network";
      state_ = NetState::Failed;
      return false;
    }
  }

  auto set = [&](const std::string& key, const std::string& value) {
    std::string reply = wpa_cli(format("set_network %d %s %s", id, key.c_str(), value.c_str()));
    return reply.find("OK") != std::string::npos;
  };
  // Quoting matters here: wpa_cli wants the SSID and passphrase quoted, and
  // key_mgmt unquoted.
  set("ssid", "'\"" + ssid + "\"'");
  if (psk.empty()) {
    set("key_mgmt", "NONE");
  } else {
    set("psk", "'\"" + psk + "\"'");
    set("key_mgmt", "WPA-PSK");
  }
  set("scan_ssid", "1");
  wpa_cli(format("enable_network %d", id));
  wpa_cli(format("select_network %d", id));

  if (!wait_for_association(kAssociationTimeoutMs)) {
    status_ = "Could not join " + ssid;
    state_ = NetState::Failed;
    wpa_cli(format("disable_network %d", id));
    return false;
  }
  if (save) wpa_cli("save_config");

  status_ = "Getting an address";
  if (!run_dhcp()) {
    state_ = NetState::Failed;
    return false;
  }
  state_ = NetState::Connected;
  status_ = ssid + " \xC2\xB7 " + ip_address();
  CK_LOGI("net: connected to %s as %s", ssid.c_str(), ip_address().c_str());
  return true;
}

bool Net::connect_saved() {
  if (state_ == NetState::Off && !power_on()) return false;
  if (simulated_) return connect("Reading Room", "", false);
  if (saved_networks().empty()) {
    status_ = "No saved networks";
    return false;
  }
  state_ = NetState::Connecting;
  status_ = "Looking for a known network";
  wpa_cli("reconnect");
  if (!wait_for_association(kAssociationTimeoutMs)) {
    status_ = "No known network in range";
    state_ = NetState::Failed;
    return false;
  }
  if (!run_dhcp()) {
    state_ = NetState::Failed;
    return false;
  }
  state_ = NetState::Connected;
  status_ = current_ssid() + " \xC2\xB7 " + ip_address();
  return true;
}

void Net::disconnect() {
  if (simulated_) {
    state_ = NetState::Starting;
    status_ = "Disconnected";
    simulated_ssid_.clear();
    return;
  }
  wpa_cli("disconnect");
  release_dhcp();
  if (state_ == NetState::Connected) state_ = NetState::Starting;
  status_ = "Disconnected";
}

bool Net::forget(const std::string& ssid) {
  if (simulated_) return true;
  std::string list = wpa_cli("list_networks");
  bool header = true;
  for (const std::string& line : split(list, '\n')) {
    if (header) {
      header = false;
      continue;
    }
    std::vector<std::string> fields = split(line, '\t');
    if (fields.size() >= 2 && fields[1] == ssid) {
      wpa_cli(format("remove_network %s", fields[0].c_str()));
      wpa_cli("save_config");
      return true;
    }
  }
  return false;
}

std::vector<std::string> Net::saved_networks() {
  std::vector<std::string> out;
  if (simulated_) {
    out.push_back("Reading Room");
    return out;
  }
  std::string list = wpa_cli("list_networks");
  bool header = true;
  for (const std::string& line : split(list, '\n')) {
    if (header) {
      header = false;
      continue;
    }
    std::vector<std::string> fields = split(line, '\t');
    if (fields.size() >= 2 && !fields[1].empty()) out.push_back(fields[1]);
  }
  return out;
}

bool Net::ensure_connected() {
  refresh_status();
  if (connected()) return true;
  if (!power_on()) return false;
  return connect_saved();
}

void Net::refresh_status() {
  if (simulated_) return;
  if (!available()) {
    state_ = NetState::Off;
    return;
  }
  if (sys::run("pkill -0 wpa_supplicant 2>/dev/null") != 0) {
    state_ = NetState::Off;
    status_ = "Wi-Fi off";
    return;
  }
  std::string ip = ip_address();
  std::string ssid = current_ssid();
  if (!ip.empty() && !ssid.empty()) {
    state_ = NetState::Connected;
    status_ = ssid + " \xC2\xB7 " + ip;
  } else if (state_ == NetState::Connected) {
    state_ = NetState::Starting;
    status_ = "Wi-Fi ready";
  }
}

}  // namespace ck
