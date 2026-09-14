// The Wi-Fi screen: what is in range, what is saved, and what CrossKobo is
// currently attached to. Connecting blocks for as long as association and
// DHCP take, so the screen says what it is doing before it starts.
#include <algorithm>
#include <memory>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/net.h"
#include "ui/keyboard.h"
#include "ui/list_view.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

constexpr int kActionToggle = -3001;
constexpr int kActionRescan = -3002;

class NetworkScreen : public ListView {
 public:
  NetworkScreen() : ListView("Wi-Fi", {}, nullptr) {
    on_select_ = [this](int id) { activate(id); };
    set_on_long_press([this](int id) { forget(id); });
    rebuild();
  }

  void on_show() override {
    Net& net = Net::instance();
    net.refresh_status();
    // Entering this screen with the radio already on should show what is in
    // range, rather than an empty list and a Rescan button.
    if (net.state() != NetState::Off && networks_.empty()) networks_ = net.scan();
    rebuild();
  }

  int tick_ms() const override { return 5000; }
  bool on_tick() override {
    Net& net = Net::instance();
    NetState before = net.state();
    net.refresh_status();
    if (net.state() != before) {
      rebuild();
      return true;
    }
    return false;
  }

 private:
  void rebuild() {
    Net& net = Net::instance();
    std::vector<Item> items;

    if (!net.available()) {
      set_empty_message(
          "CrossKobo could not find a Wi-Fi driver for this device.\n\n"
          "Details are in .crosskobo/crosskobo.log on the drive.");
      set_items({}, false);
      set_actions({});
      set_status_line("");
      return;
    }

    for (size_t i = 0; i < networks_.size(); ++i) {
      const WifiNetwork& n = networks_[i];
      Item item;
      item.id = (int)i;
      item.row.title = n.ssid;
      item.row.bold = n.current;
      std::vector<std::string> notes;
      if (n.current) notes.push_back("connected");
      if (n.known && !n.current) notes.push_back("saved");
      notes.push_back(n.secured ? "secured" : "open");
      notes.push_back(format("%d dBm", n.signal));
      item.row.subtitle = join(notes, " \xC2\xB7 ");
      item.row.trailing = format("%d%%", n.quality);
      item.row.check = n.current;
      items.push_back(std::move(item));
    }

    set_items(std::move(items), true);
    set_empty_message(net.state() == NetState::Off
                          ? "Wi-Fi is off.\n\nTurn it on to see what is in range."
                          : "Nothing in range.\n\nTap Rescan to look again.");
    std::vector<std::pair<std::string, int>> actions;
    actions.emplace_back(net.state() == NetState::Off ? "Turn on" : "Turn off", kActionToggle);
    if (net.state() != NetState::Off) actions.emplace_back("Rescan", kActionRescan);
    set_actions(std::move(actions));
    set_status_line(net.status_text());
  }

  // Draws a one-line progress message and pushes it to the panel, so the
  // user sees what is happening during a blocking operation.
  void say(const std::string& message) {
    set_status_line(message);
    App::instance().invalidate(Refresh::Fast);
    App::instance().render_now();
  }

  void activate(int id) {
    Net& net = Net::instance();
    if (id == kActionToggle) {
      if (net.state() == NetState::Off) {
        say("Powering up Wi-Fi...");
        if (net.power_on()) {
          say("Scanning...");
          networks_ = net.scan();
          // If a known network is in range, just join it.
          bool known_in_range = std::any_of(networks_.begin(), networks_.end(),
                                            [](const WifiNetwork& n) { return n.known; });
          if (known_in_range && !net.connected()) {
            say("Connecting to a saved network...");
            net.connect_saved();
            networks_ = net.scan();
          }
        }
      } else {
        say("Turning Wi-Fi off...");
        net.power_off();
        networks_.clear();
      }
      rebuild();
      App::instance().invalidate(Refresh::Flash);
      return;
    }
    if (id == kActionRescan) {
      say("Scanning...");
      networks_ = net.scan();
      rebuild();
      App::instance().invalidate(Refresh::Flash);
      return;
    }
    if (id < 0 || id >= (int)networks_.size()) return;

    WifiNetwork chosen = networks_[(size_t)id];
    if (chosen.current) {
      if (App::instance().confirm("Disconnect", "Leave " + chosen.ssid + "?", "Disconnect",
                                  "Stay")) {
        net.disconnect();
        networks_ = net.scan();
        rebuild();
      }
      return;
    }
    if (!chosen.secured || chosen.known) {
      say("Connecting to " + chosen.ssid + "...");
      bool ok = net.connect(chosen.ssid, "", chosen.known);
      if (!ok && chosen.secured) {
        // A saved password that no longer works: ask for a new one.
        ask_password(chosen);
        return;
      }
      networks_ = net.scan();
      rebuild();
      App::instance().invalidate(Refresh::Flash);
      return;
    }
    ask_password(chosen);
  }

  void ask_password(const WifiNetwork& chosen) {
    App::instance().push(std::make_unique<KeyboardView>(
        "Password for " + chosen.ssid, "", [this, chosen](const std::string& text, bool ok) {
          if (!ok) return;
          Net& net = Net::instance();
          say("Connecting to " + chosen.ssid + "...");
          if (!net.connect(chosen.ssid, text, true)) {
            App::instance().show_message("Could not connect", net.status_text());
          }
          networks_ = net.scan();
          rebuild();
          App::instance().invalidate(Refresh::Flash);
        }));
  }

  void forget(int id) {
    if (id < 0 || id >= (int)networks_.size()) return;
    const WifiNetwork& chosen = networks_[(size_t)id];
    if (!chosen.known) return;
    if (!App::instance().confirm("Forget network",
                                 "Forget " + chosen.ssid + " and its password?", "Forget",
                                 "Keep")) {
      return;
    }
    Net::instance().forget(chosen.ssid);
    networks_ = Net::instance().scan();
    rebuild();
    App::instance().invalidate(Refresh::Flash);
  }

  std::vector<WifiNetwork> networks_;
};

}  // namespace

ViewPtr make_network_screen() { return std::make_unique<NetworkScreen>(); }

}  // namespace ck
