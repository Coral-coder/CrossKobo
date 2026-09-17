#pragma once
#include <string>

namespace ck {

// Exports the device's storage over USB from inside CrossKobo, so copying
// books on and off no longer means handing the device back to the stock
// software.
//
// The sequence is: flush and unmount the user partition, hand the block
// device to the USB gadget, wait for the host to finish, tear the gadget
// down, and mount the partition again. Both gadget generations are
// supported: configfs on MediaTek boards and the g_mass_storage module on
// the older NXP ones.
class UsbMs {
 public:
  static UsbMs& instance();

  // True when this device looks like something we know how to export.
  bool supported() const;
  // Anything that would make an export unsafe (a custom gadget already
  // attached, USBNet in use, storage busy). Empty when it is safe to start.
  std::string blocker() const;

  bool active() const { return active_; }

  // Unmounts storage and attaches the gadget. On failure nothing is left
  // half-done: storage is remounted before returning.
  bool start();
  // Detaches the gadget and remounts storage. Safe to call when inactive.
  bool stop();

  // True while the host has the drive mounted (the UDC reports a
  // configured connection).
  bool host_connected() const;
  // Set when the cable is gone, which is the cue to stop.
  bool cable_gone() const;

  const std::string& last_error() const { return error_; }

 private:
  UsbMs() = default;
  bool unmount_storage();
  bool remount_storage();
  bool attach_gadget();
  bool detach_gadget();
  std::string gadget_path() const;

  bool active_ = false;
  bool had_sd_ = false;
  std::string error_;
};

}  // namespace ck
