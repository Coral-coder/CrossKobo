#include "platform/system.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"

namespace ck {
namespace sys {
namespace {

std::string g_nickel_env;

const char* kNickelHelpers =
    "nickel hindenburg sickel fickel strickel fontickel adobehost foxitpdf iink "
    "dhcpcd-dbus dhcpcd bluealsa bluetoothd nanoclock.lua QtWebEngineProcess";

}  // namespace

int run(const std::string& command) {
  int rv = system(command.c_str());
  if (rv != 0) CK_LOGD("sys: '%s' exited %d", command.c_str(), rv);
  return rv;
}

std::string run_capture(const std::string& command) {
  FILE* p = popen(command.c_str(), "re");
  if (!p) return "";
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  pclose(p);
  return trim(out);
}

bool is_root() { return geteuid() == 0; }

bool nickel_running() { return run("pkill -0 nickel 2>/dev/null") == 0; }

void signal_ready(bool ready) {
  const char* kFlag = "/tmp/crosskobo-ready";
  if (ready) {
    fs::write_file_atomic(kFlag, "1\n");
  } else {
    fs::remove_file(kFlag);
  }
}

void stop_boot_animation() {
  // Names differ by firmware: on-animator.sh on 4.x, animator.sh on 5.x,
  // and pickel/pickel-mtk is the tool both use to push frames.
  run("killall -q -TERM on-animator.sh animator.sh 2>/dev/null");
  run("killall -q -TERM pickel pickel-mtk 2>/dev/null");
  // The scripts loop, so a TERM to the shell may land between iterations.
  sleep_ms(150);
  run("killall -q -KILL on-animator.sh animator.sh pickel pickel-mtk 2>/dev/null");
}

void clear_crash_count() { fs::remove_file("/usr/local/crosskobo/crash-count"); }

void allow_nickel() {
  fs::write_file_atomic("/tmp/crosskobo-allow-nickel", "1\n");
  fs::remove_file("/tmp/crosskobo-ready");
}

void capture_nickel_env() {
  std::string pid = run_capture("pidof -s nickel 2>/dev/null");
  if (pid.empty()) return;
  std::string environ_blob;
  if (!fs::read_file("/proc/" + pid + "/environ", environ_blob)) return;
  // /proc/<pid>/environ is NUL separated.
  std::string out;
  size_t start = 0;
  while (start < environ_blob.size()) {
    size_t end = environ_blob.find('\0', start);
    if (end == std::string::npos) end = environ_blob.size();
    std::string entry = environ_blob.substr(start, end - start);
    for (const char* key : {"PLATFORM=", "PRODUCT=", "DBUS_SESSION_BUS_ADDRESS=", "NICKEL_HOME=",
                            "WIFI_MODULE=", "INTERFACE=", "LANG="}) {
      if (starts_with(entry, key)) {
        out += "export '" + entry + "'\n";
        // Also make it available to our own children.
        size_t eq = entry.find('=');
        if (eq != std::string::npos) {
          setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), 1);
        }
        break;
      }
    }
    start = end + 1;
  }
  g_nickel_env = out;
  CK_LOGI("sys: captured %zu bytes of Nickel environment", g_nickel_env.size());
}

void stop_nickel() {
  if (!nickel_running()) return;
  CK_LOGI("sys: stopping the stock UI");
  sync();
  run(std::string("killall -q -TERM ") + kNickelHelpers + " 2>/dev/null");
  for (int i = 0; i < 40 && nickel_running(); ++i) sleep_ms(100);
  if (nickel_running()) {
    CK_LOGW("sys: nickel did not exit, sending KILL");
    run(std::string("killall -q -KILL ") + kNickelHelpers + " 2>/dev/null");
    sleep_ms(200);
  }
  // udev and the dhcp scripts write into this FIFO; with Nickel gone they
  // would block forever on open().
  fs::remove_file("/tmp/nickel-hardware-status");
}

void wifi_down() {
  const char* iface = getenv("INTERFACE");
  std::string dev = iface && *iface ? iface : "eth0";
  run("killall -q -TERM restore-wifi-async.sh enable-wifi.sh obtain-ip.sh 2>/dev/null");
  run("killall -q -TERM udhcpc dhcpcd default.script 2>/dev/null");
  run("wpa_cli -i " + dev + " terminate >/dev/null 2>&1");
  run("ifconfig " + dev + " down >/dev/null 2>&1");
}

bool start_nickel() {
  if (!device().is_kobo) {
    CK_LOGW("sys: not on a Kobo, refusing to start the stock UI");
    return false;
  }
  CK_LOGI("sys: handing back to the stock Kobo UI");
  // Stand the boot watchdog down first, or it will kill what we start.
  allow_nickel();
  wifi_down();
  sync();

  if (device().layout == FirmwareLayout::V5 ||
      fs::exists("/etc/init.d/z-nickel-hardware-status")) {
    // Firmware 5 keeps its own start-up scripts; re-running them brings up
    // the whole stack (including the pieces rc.local owns).
    run("unset LD_LIBRARY_PATH; /etc/init.d/z-nickel-hardware-status");
    sync();
    run("/etc/rc.local");
    return true;
  }

  // Firmware 4: recreate the status FIFO, then launch the UI the way rcS
  // does, with the environment we siphoned from the original process.
  fs::remove_file("/tmp/nickel-hardware-status");
  run("mkfifo /tmp/nickel-hardware-status 2>/dev/null");
  std::string script =
      g_nickel_env +
      "export LD_LIBRARY_PATH=/usr/local/Kobo\n"
      "export QT_GSTREAMER_PLAYBIN_AUDIOSINK=alsasink\n"
      "cd /\n"
      "/usr/local/Kobo/hindenburg >/dev/null 2>&1 &\n"
      "LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad "
      ">/dev/null 2>&1 &\n";
  if (device().platform != "freescale") script += "udevadm trigger >/dev/null 2>&1 &\n";
  // Restart the boot animation script too: the established launchers rely on
  // Nickel killing it, and it is what re-arms KFMon if the user has it.
  if (fs::exists("/etc/init.d/on-animator.sh")) {
    script += "(/etc/init.d/on-animator.sh >/dev/null 2>&1) &\n";
  }
  std::string path = "/tmp/crosskobo-start-nickel.sh";
  if (!fs::write_file_atomic(path, script)) return false;
  run("chmod +x " + path);
  return run("/bin/sh " + path) == 0;
}

bool usb_plugged() {
  // Kobo exposes the cable through one of a few power-supply nodes
  // depending on the PMIC generation.
  static const char* kCandidates[] = {
      "/sys/class/power_supply/usb/online",
      "/sys/class/power_supply/bd71827_ac/online",
      "/sys/class/power_supply/ac/online",
      "/sys/class/power_supply/mc13892_charger/online",
  };
  for (const char* path : kCandidates) {
    std::string value;
    if (fs::read_file(path, value)) return to_int(trim(value), 0) != 0;
  }
  // Fall back to the USB device controller state.
  for (const fs::Entry& e : fs::list_dir("/sys/class/udc")) {
    std::string state;
    if (fs::read_file(e.path + "/state", state)) {
      state = trim(state);
      return state == "configured" || state == "addressed";
    }
  }
  return false;
}

}  // namespace sys
}  // namespace ck
