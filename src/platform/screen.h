#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include <linux/fb.h>

#include "gfx/canvas.h"
#include "gfx/geometry.h"

namespace ck {

// How a region should be pushed to the panel. The mapping to hardware
// waveforms lives in screen.cpp and differs between the MediaTek and i.MX
// display controllers.
enum class Refresh {
  Auto,       // let the driver decide
  Fast,       // DU: two-tone, fastest, for menus and highlights
  Text,       // GL16: 16 grey levels, no flash - the reading default
  Image,      // GC16 / GCC16 on colour panels: full tonal range
  Color,      // GCC16 + saturation: images and colour-rich pages
  Highlight,  // GLRC16: colour over text without flashing
  Pen,        // A2 + forced A2 output: lowest latency, for ink
  Flash,      // full flashing refresh, clears ghosting
  Init,       // panel init
};

// Colour filter array processing on Kaleido panels.
enum class CfaMode { Default, Standard, Boost, Desaturate, Vivid, Mono };

enum class Rotation { Portrait = 0, LandscapeCW = 1, PortraitFlipped = 2, LandscapeCCW = 3 };

// Owns the framebuffer and the logical drawing canvas. Everything above the
// platform layer works in logical coordinates; Screen applies rotation and
// the framebuffer's pixel layout during flush.
class Screen {
 public:
  static Screen& instance();

  bool open(const std::string& fb_path = "/dev/fb0");
  void close();
  bool is_open() const { return fb_fd_ >= 0 || headless_; }

  // Headless mode backs the canvas with plain memory: used by the host
  // simulator, the test suite and screenshot rendering.
  bool open_headless(int width, int height);

  int width() const { return logical_w_; }
  int height() const { return logical_h_; }
  Rect bounds() const { return Rect(0, 0, logical_w_, logical_h_); }
  Canvas& canvas() { return canvas_; }
  int dpi() const { return dpi_; }
  bool color() const { return color_panel_; }

  void set_rotation(Rotation r);
  Rotation rotation() const { return rotation_; }

  // Pushes `r` (logical coordinates) to the panel. `wait` blocks until the
  // panel has finished, which the reader uses before sleeping so the last
  // page is actually on screen.
  void flush(const Rect& r, Refresh mode = Refresh::Auto, bool wait = false);
  void flush_all(Refresh mode = Refresh::Flash, bool wait = false);

  // Night mode inverts the panel through the eclipse waveforms where the
  // hardware supports it, and in software otherwise.
  void set_night_mode(bool on);
  bool night_mode() const { return night_mode_; }

  void set_cfa_mode(CfaMode mode);
  CfaMode cfa_mode() const { return cfa_mode_; }
  // Extra saturation applied in software before the panel's own filter.
  void set_saturation_boost(float amount) { saturation_ = amount; }
  float saturation_boost() const { return saturation_; }

  // Number of non-flashing refreshes before one flashing refresh is forced,
  // to clear accumulated ghosting. 0 disables.
  void set_flash_interval(int pages) { flash_interval_ = pages; }
  void note_page_turn();
  bool flash_due() const;
  void reset_flash_counter() { since_flash_ = 0; }

  void wait_for_complete();
  bool save_screenshot(const std::string& path);

 private:
  Screen() = default;
  void allocate_canvas();
  void convert_and_copy(const Rect& logical);
  void send_update(const Rect& device_rect, Refresh mode, bool wait);
  Rect to_device(const Rect& logical) const;

  int fb_fd_ = -1;
  bool headless_ = false;
  uint8_t* fb_mem_ = nullptr;
  size_t fb_len_ = 0;
  int fb_w_ = 0, fb_h_ = 0;          // framebuffer pixel dimensions
  int fb_bpp_ = 32;
  int fb_stride_ = 0;                // bytes per line
  int r_off_ = 16, g_off_ = 8, b_off_ = 0, a_off_ = 24;  // bit offsets
  bool restore_vinfo_ = false;
  struct fb_var_screeninfo* saved_vinfo_ = nullptr;  // original mode, if changed
  std::string fb_path_;

  int logical_w_ = 0, logical_h_ = 0;
  int dpi_ = 300;
  bool color_panel_ = false;
  bool is_mtk_ = false;
  Rotation rotation_ = Rotation::Portrait;
  Canvas canvas_;

  bool night_mode_ = false;
  CfaMode cfa_mode_ = CfaMode::Default;
  float saturation_ = 0.0f;
  uint32_t marker_ = 1;
  uint32_t last_marker_ = 0;
  int flash_interval_ = 0;
  int since_flash_ = 0;
};

}  // namespace ck
