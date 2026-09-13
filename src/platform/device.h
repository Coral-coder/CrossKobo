#pragma once
#include <string>

namespace ck {

// Firmware layout. Kobo changed both the boot scripts and the update
// mechanism with firmware 5.x, and CrossKobo has to install itself and hand
// control back to the stock UI differently on each.
enum class FirmwareLayout { Unknown, V4, V5 };

struct DeviceInfo {
  std::string codename = "unknown";  // "monza" = Libra Colour
  std::string model = "Kobo";
  std::string firmware;              // e.g. "4.46.23836"
  std::string platform;              // e.g. "b300-ntx" (MTK) or "mx6sll-ntx"
  FirmwareLayout layout = FirmwareLayout::Unknown;

  bool is_kobo = false;
  bool is_mtk = false;        // MediaTek: hwtcon EPD interface
  bool has_color_panel = false;  // Kaleido 3 colour filter array
  bool has_frontlight = false;
  bool has_natural_light = false;  // adjustable warmth
  bool has_page_buttons = false;
  bool has_stylus = false;
  bool has_gsensor = false;

  int dpi = 300;

  // sysfs knobs, probed at startup; empty when unsupported.
  std::string frontlight_brightness;
  std::string frontlight_warmth;
  std::string battery_dir;
  std::string charging_led;

  // Human-readable summary for the About screen and the log header.
  std::string describe() const;
};

// Probes the device once and caches the result.
const DeviceInfo& device();
// Overrides detection (used by the simulator and by tests).
void set_device(const DeviceInfo& info);

}  // namespace ck
