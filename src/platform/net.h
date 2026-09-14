#pragma once
#include <string>
#include <vector>

namespace ck {

struct WifiNetwork {
  std::string ssid;
  std::string bssid;
  int signal = 0;        // dBm as reported by the driver
  int quality = 0;       // 0..100, derived from the signal
  bool secured = true;
  bool known = false;    // already in wpa_supplicant's config
  bool current = false;
};

enum class NetState { Off, Starting, Scanning, Connecting, Connected, Failed };

// Wi-Fi, driven the way the stock software drives it: power the chip,
// insert the driver modules, run wpa_supplicant, then a DHCP client. The
// sequences differ between MediaTek and NXP boards and are documented in
// docs/HARDWARE.md.
class Net {
 public:
  static Net& instance();

  bool available() const;        // hardware and drivers present
  NetState state() const { return state_; }
  const std::string& status_text() const { return status_; }
  bool connected() const { return state_ == NetState::Connected; }
  std::string ip_address() const;
  std::string current_ssid() const;
  int current_quality() const;

  // Powers everything up and starts wpa_supplicant. Returns false if the
  // hardware or the driver modules are missing.
  bool power_on();
  // Tears the whole stack down, which is what saves the battery.
  void power_off();

  // Scans and returns what is in range, strongest first. Takes a few
  // seconds; runs synchronously.
  std::vector<WifiNetwork> scan();

  // Joins a network, optionally saving it for next time. `psk` is empty for
  // an open network. Blocks until DHCP succeeds or the timeout expires.
  bool connect(const std::string& ssid, const std::string& psk, bool save = true);
  // Reconnects to whichever saved network is in range.
  bool connect_saved();
  void disconnect();
  bool forget(const std::string& ssid);
  std::vector<std::string> saved_networks();

  // Brings the network up if it is not already, for a feature that needs
  // it. Returns false when there is nothing to connect to.
  bool ensure_connected();

  void refresh_status();

  // Simulation, for the host simulator and the test suite: canned scan
  // results and instant connections, with no hardware touched.
  void set_simulated(bool simulated) { simulated_ = simulated; }
  bool simulated() const { return simulated_; }

 private:
  Net() = default;
  bool load_modules();
  void unload_modules();
  bool start_supplicant();
  void stop_supplicant();
  bool run_dhcp();
  void release_dhcp();
  std::string wpa_cli(const std::string& command) const;
  bool wait_for_association(int timeout_ms);

  NetState state_ = NetState::Off;
  std::string status_;
  bool modules_loaded_ = false;
  bool simulated_ = false;
  std::string simulated_ssid_;
};

}  // namespace ck
