#include "platform/usbms.h"

#include <cerrno>
#include <cstring>
#include <sys/mount.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"
#include "platform/system.h"

namespace ck {
namespace {

constexpr const char* kOnboard = "/mnt/onboard";
constexpr const char* kSdCard = "/mnt/sd";
constexpr const char* kConfigFsRoot = "/sys/kernel/config/usb_gadget";

// Kobo's own USB vendor id. Hosts (and Calibre) recognise the device by
// this and by the serial number from the firmware's version tag.
constexpr const char* kVendorId = "0x2237";
constexpr const char* kProductId = "0x4163";

bool write_sysfs(const std::string& path, const std::string& value) {
  FILE* f = fopen(path.c_str(), "we");
  if (!f) {
    CK_LOGW("usbms: cannot write %s: %s", path.c_str(), strerror(errno));
    return false;
  }
  fputs(value.c_str(), f);
  bool ok = fflush(f) == 0;
  fclose(f);
  return ok;
}

std::string read_sysfs(const std::string& path) {
  std::string out;
  if (!fs::read_file(path, out)) return "";
  return trim(out);
}

bool is_mounted(const std::string& mountpoint) {
  std::string mounts;
  if (!fs::read_file("/proc/mounts", mounts)) return false;
  return mounts.find(" " + mountpoint + " ") != std::string::npos;
}

std::string firmware_version() {
  std::string text;
  if (!fs::read_file("/mnt/onboard/.kobo/version", text)) return "4.0.0";
  std::vector<std::string> parts = split(trim(text), ',');
  return parts.size() > 2 ? parts[2] : "4.0.0";
}

}  // namespace

UsbMs& UsbMs::instance() {
  static UsbMs u;
  return u;
}

std::string UsbMs::gadget_path() const {
  return std::string(kConfigFsRoot) + "/" + device().usb_gadget_name;
}

bool UsbMs::supported() const {
  const DeviceInfo& dev = device();
  if (!dev.is_kobo || dev.user_partition.empty()) return false;
  if (fs::is_dir(kConfigFsRoot)) return !dev.usb_udc.empty();
  // The legacy path needs the gadget modules the firmware ships.
  return fs::is_dir("/drivers/" + dev.platform);
}

std::string UsbMs::blocker() const {
  const DeviceInfo& dev = device();
  if (!supported()) return "CrossKobo cannot export this device's storage";

  std::string modules;
  fs::read_file("/proc/modules", modules);
  if (modules.find("g_ether ") != std::string::npos) {
    return "USBNet is enabled; turn it off first";
  }
  if (modules.find("g_serial ") != std::string::npos) {
    return "USBSerial is enabled; turn it off first";
  }
  if (!active_ &&
      (modules.find("g_mass_storage ") != std::string::npos ||
       modules.find("g_file_storage ") != std::string::npos)) {
    return "Storage is already exported by something else";
  }
  if (!dev.usb_udc.empty() && !active_) {
    std::string state = read_sysfs("/sys/class/udc/" + dev.usb_udc + "/state");
    if (!state.empty() && state != "not attached") {
      return "Another USB gadget is attached (" + state + ")";
    }
  }
  return "";
}

bool UsbMs::host_connected() const {
  const DeviceInfo& dev = device();
  if (dev.usb_udc.empty()) return active_;
  std::string state = read_sysfs("/sys/class/udc/" + dev.usb_udc + "/state");
  return state == "configured";
}

bool UsbMs::cable_gone() const { return !sys::usb_plugged(); }

bool UsbMs::unmount_storage() {
  sync();
  // Dropping caches first makes the unmount far more likely to succeed.
  write_sysfs("/proc/sys/vm/drop_caches", "3");

  had_sd_ = is_mounted(kSdCard);
  if (had_sd_ && umount(kSdCard) != 0) {
    CK_LOGW("usbms: could not unmount the SD card: %s", strerror(errno));
    had_sd_ = false;   // carry on with just the internal storage
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!is_mounted(kOnboard)) return true;
    if (umount(kOnboard) == 0) return true;
    if (errno != EBUSY) break;
    // Something still has a file open. Name it in the log: it is almost
    // always a leftover of ours, and that is worth knowing about.
    std::string busy = sys::run_capture("fuser -m /mnt/onboard 2>/dev/null");
    CK_LOGW("usbms: /mnt/onboard is busy (%s), retrying", busy.c_str());
    sync();
    sleep_ms(400);
  }
  error_ = format("Could not unmount the drive: %s", strerror(errno));
  CK_LOGE("usbms: %s", error_.c_str());
  return false;
}

bool UsbMs::remount_storage() {
  const DeviceInfo& dev = device();
  bool ok = true;
  if (!is_mounted(kOnboard)) {
    fs::mkdir_p(kOnboard);
    // The stock firmware mounts onboard with these options; keeping them
    // identical means the stock software is happy afterwards too.
    std::string opts = "noatime,nodiratime,shortname=mixed,utf8";
    if (mount(dev.user_partition.c_str(), kOnboard, "vfat", MS_NOATIME | MS_NODIRATIME,
              opts.c_str()) != 0) {
      CK_LOGW("usbms: mount(2) failed (%s), falling back to the mount command",
              strerror(errno));
      ok = sys::run("mount -t vfat -o noatime,nodiratime,shortname=mixed,utf8 " +
                    dev.user_partition + " " + kOnboard + " 2>/dev/null") == 0;
    }
  }
  if (had_sd_ && !is_mounted(kSdCard) && !dev.sd_partition.empty()) {
    fs::mkdir_p(kSdCard);
    sys::run("mount -t vfat -o noatime,nodiratime,shortname=mixed,utf8 " + dev.sd_partition +
             " " + kSdCard + " 2>/dev/null");
  }
  if (!ok) {
    error_ = "The drive could not be mounted again";
    CK_LOGE("usbms: %s", error_.c_str());
  }
  return ok;
}

bool UsbMs::attach_gadget() {
  const DeviceInfo& dev = device();
  std::string partitions = dev.user_partition;
  if (!dev.sd_partition.empty()) partitions += "," + dev.sd_partition;
  std::string serial = dev.serial.empty() ? "N000000000000" : dev.serial;
  std::string product = "eReader-" + firmware_version();

  if (fs::is_dir(kConfigFsRoot)) {
    // configfs (MediaTek): build the gadget, point a mass storage LUN at
    // the partition, then bind it to the USB device controller.
    const std::string g = gadget_path();
    fs::mkdir_p(g);
    fs::mkdir_p(g + "/strings/0x409");
    write_sysfs(g + "/idVendor", kVendorId);
    write_sysfs(g + "/idProduct", kProductId);
    write_sysfs(g + "/strings/0x409/serialnumber", serial);
    write_sysfs(g + "/strings/0x409/manufacturer", "Kobo");
    write_sysfs(g + "/strings/0x409/product", product);
    fs::mkdir_p(g + "/configs/c.1/strings/0x409");
    write_sysfs(g + "/configs/c.1/strings/0x409/configuration", "KOBOeReader");
    fs::mkdir_p(g + "/functions/mass_storage.0/lun.0");
    write_sysfs(g + "/functions/mass_storage.0/lun.0/removable", "1");
    if (!write_sysfs(g + "/functions/mass_storage.0/lun.0/file", dev.user_partition)) {
      error_ = "The kernel would not take the storage partition";
      return false;
    }
    // Nickel leaves its own gadget behind, so only link when needed.
    if (!fs::exists(g + "/configs/c.1/mass_storage.0")) {
      sys::run("ln -s " + g + "/functions/mass_storage.0 " + g + "/configs/c.1 2>/dev/null");
    }
    if (!write_sysfs(g + "/UDC", dev.usb_udc)) {
      error_ = "The USB controller refused the gadget";
      return false;
    }
    return true;
  }

  // Legacy NXP boards: one module, with the partitions as a parameter.
  std::string dir = "/drivers/" + dev.platform;
  std::string params =
      format("idVendor=%s idProduct=%s iManufacturer=Kobo iProduct=%s iSerialNumber=%s",
             kVendorId, kProductId, product.c_str(), serial.c_str());
  if (fs::exists(dir + "/g_mass_storage.ko")) {
    sys::run(format("insmod %s/g_mass_storage.ko file=%s stall=0 removable=1 %s 2>/dev/null",
                    dir.c_str(), partitions.c_str(), params.c_str()));
  } else {
    std::string gadgets = dir + "/usb/gadget";
    sys::run("insmod " + gadgets + "/configfs.ko 2>/dev/null");
    sys::run("insmod " + gadgets + "/libcomposite.ko 2>/dev/null");
    sys::run("insmod " + gadgets + "/usb_f_mass_storage.ko 2>/dev/null");
    sys::run(format("insmod %s/g_file_storage.ko file=%s stall=0 removable=1 %s 2>/dev/null",
                    gadgets.c_str(), partitions.c_str(), params.c_str()));
  }
  sleep_ms(1000);
  std::string modules;
  fs::read_file("/proc/modules", modules);
  if (modules.find("g_mass_storage ") == std::string::npos &&
      modules.find("g_file_storage ") == std::string::npos) {
    error_ = "The USB storage driver would not load";
    return false;
  }
  return true;
}

bool UsbMs::detach_gadget() {
  const DeviceInfo& dev = device();
  if (fs::is_dir(kConfigFsRoot) && fs::is_dir(gadget_path())) {
    const std::string g = gadget_path();
    write_sysfs(g + "/UDC", "\n");
    sleep_ms(200);
    fs::remove_file(g + "/configs/c.1/mass_storage.0");
    sys::run("rmdir " + g + "/configs/c.1/strings/0x409 2>/dev/null");
    sys::run("rmdir " + g + "/configs/c.1 2>/dev/null");
    sys::run("rmdir " + g + "/functions/mass_storage.0 2>/dev/null");
    sys::run("rmdir " + g + "/strings/0x409 2>/dev/null");
    sys::run("rmdir " + g + " 2>/dev/null");
    // Firmware 5 pre-configures a gadget for the stock software; put it
    // back so USB still works if the user returns to the Kobo UI.
    if (fs::exists("/etc/init.d/usb-gadget")) sys::run("/etc/init.d/usb-gadget 2>/dev/null");
    return true;
  }
  std::string modules;
  fs::read_file("/proc/modules", modules);
  if (modules.find("g_mass_storage ") != std::string::npos) {
    sys::run("rmmod g_mass_storage 2>/dev/null");
  } else if (modules.find("g_file_storage ") != std::string::npos) {
    sys::run("rmmod g_file_storage 2>/dev/null");
  }
  sleep_ms(500);
  (void)dev;
  return true;
}

bool UsbMs::start() {
  error_.clear();
  if (active_) return true;
  std::string why = blocker();
  if (!why.empty()) {
    error_ = why;
    return false;
  }
  CK_LOGI("usbms: exporting %s", device().user_partition.c_str());
  if (!unmount_storage()) return false;
  if (!attach_gadget()) {
    // Never leave the user without their drive.
    detach_gadget();
    remount_storage();
    return false;
  }
  active_ = true;
  return true;
}

bool UsbMs::stop() {
  if (!active_) return true;
  CK_LOGI("usbms: ending the export");
  detach_gadget();
  active_ = false;
  bool ok = remount_storage();
  sync();
  return ok;
}

}  // namespace ck
