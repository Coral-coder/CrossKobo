#include "platform/screen.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"
#include "platform/eink_ioctl.h"

namespace ck {
namespace {

int bitfield_offset(const struct fb_bitfield& f, int fallback) {
  return f.length ? (int)f.offset : fallback;
}

}  // namespace

Screen& Screen::instance() {
  static Screen s;
  return s;
}

bool Screen::open_headless(int width, int height) {
  close();
  headless_ = true;
  fb_w_ = width;
  fb_h_ = height;
  fb_bpp_ = 32;
  logical_w_ = width;
  logical_h_ = height;
  color_panel_ = true;
  dpi_ = device().dpi;
  allocate_canvas();
  CK_LOGI("screen: headless %dx%d", width, height);
  return true;
}

bool Screen::open(const std::string& fb_path) {
  close();
  fb_path_ = fb_path;
  fb_fd_ = ::open(fb_path.c_str(), O_RDWR | O_CLOEXEC);
  if (fb_fd_ < 0) {
    CK_LOGE("screen: cannot open %s: %s", fb_path.c_str(), strerror(errno));
    return false;
  }

  struct fb_var_screeninfo vinfo;
  struct fb_fix_screeninfo finfo;
  if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
      ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
    CK_LOGE("screen: FBIOGET_*SCREENINFO failed: %s", strerror(errno));
    ::close(fb_fd_);
    fb_fd_ = -1;
    return false;
  }

  const DeviceInfo& dev = device();
  is_mtk_ = dev.is_mtk;
  color_panel_ = dev.has_color_panel;
  dpi_ = dev.dpi;

  // Colour panels are only driven correctly from a 32bpp framebuffer: the
  // kernel's colour-filter-array pass expects RGB input, and the eclipse
  // (night mode) waveforms misbehave at 8bpp on MTK hardware. Ask for
  // 32bpp when we are not already there, and remember to put the original
  // mode back on exit so the stock UI is not left confused.
  if (vinfo.bits_per_pixel != 32 && (color_panel_ || is_mtk_)) {
    struct fb_var_screeninfo want = vinfo;
    want.bits_per_pixel = 32;
    want.grayscale = 0;
    want.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(fb_fd_, FBIOPUT_VSCREENINFO, &want) == 0 &&
        ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) == 0) {
      restore_vinfo_ = true;
      ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo);
      CK_LOGI("screen: switched framebuffer to 32bpp");
    } else {
      CK_LOGW("screen: could not switch to 32bpp (%s); staying at %ubpp", strerror(errno),
              vinfo.bits_per_pixel);
    }
  }

  fb_w_ = (int)vinfo.xres;
  fb_h_ = (int)vinfo.yres;
  fb_bpp_ = (int)vinfo.bits_per_pixel;
  fb_stride_ = (int)finfo.line_length;
  if (fb_stride_ <= 0) fb_stride_ = fb_w_ * (fb_bpp_ / 8);
  r_off_ = bitfield_offset(vinfo.red, 16);
  g_off_ = bitfield_offset(vinfo.green, 8);
  b_off_ = bitfield_offset(vinfo.blue, 0);
  a_off_ = bitfield_offset(vinfo.transp, 24);

  fb_len_ = (size_t)fb_stride_ * (size_t)std::max<int>(fb_h_, (int)vinfo.yres_virtual);
  if (finfo.smem_len > 0 && (size_t)finfo.smem_len > fb_len_) fb_len_ = finfo.smem_len;
  fb_mem_ = (uint8_t*)mmap(nullptr, fb_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
  if (fb_mem_ == MAP_FAILED) {
    CK_LOGE("screen: mmap failed: %s", strerror(errno));
    fb_mem_ = nullptr;
    ::close(fb_fd_);
    fb_fd_ = -1;
    return false;
  }

  logical_w_ = fb_w_;
  logical_h_ = fb_h_;
  allocate_canvas();

  // Keep the controller powered between updates for a moment so page turns
  // don't pay the power-up cost every time; -1 would never power down.
  int delay = 2000;
  if (is_mtk_) {
    ioctl(fb_fd_, HWTCON_SET_PWRDOWN_DELAY, &delay);
  } else {
    ioctl(fb_fd_, MXCFB_SET_PWRDOWN_DELAY, &delay);
  }
  if (is_mtk_ && color_panel_) {
    uint32_t mode = HWTCON_CFA_MODE_EINK_G1;
    ioctl(fb_fd_, HWTCON_SET_CFA_MODE, &mode);
  }

  CK_LOGI("screen: %s %dx%d %dbpp stride=%d rgba_off=%d/%d/%d/%d %s", fb_path.c_str(), fb_w_,
          fb_h_, fb_bpp_, fb_stride_, r_off_, g_off_, b_off_, a_off_,
          is_mtk_ ? "hwtcon" : "mxcfb");
  return true;
}

void Screen::close() {
  if (fb_mem_) {
    munmap(fb_mem_, fb_len_);
    fb_mem_ = nullptr;
  }
  if (fb_fd_ >= 0) {
    ::close(fb_fd_);
    fb_fd_ = -1;
  }
  headless_ = false;
  restore_vinfo_ = false;
}

void Screen::allocate_canvas() {
  bool landscape = rotation_ == Rotation::LandscapeCW || rotation_ == Rotation::LandscapeCCW;
  logical_w_ = landscape ? fb_h_ : fb_w_;
  logical_h_ = landscape ? fb_w_ : fb_h_;
  canvas_.reset(logical_w_, logical_h_);
  canvas_.clear(Color::gray(255));
}

void Screen::set_rotation(Rotation r) {
  if (r == rotation_) return;
  rotation_ = r;
  allocate_canvas();
}

Rect Screen::to_device(const Rect& l) const {
  switch (rotation_) {
    case Rotation::Portrait: return l;
    case Rotation::PortraitFlipped:
      return Rect(fb_w_ - l.right(), fb_h_ - l.bottom(), l.w, l.h);
    case Rotation::LandscapeCW:
      // logical (x,y) -> device (fb_w-1-y, x)
      return Rect(fb_w_ - l.bottom(), l.x, l.h, l.w);
    case Rotation::LandscapeCCW:
      // logical (x,y) -> device (y, fb_h-1-x)
      return Rect(l.y, fb_h_ - l.right(), l.h, l.w);
  }
  return l;
}

void Screen::convert_and_copy(const Rect& logical) {
  if (!fb_mem_) return;
  Rect l = logical.intersect(Rect(0, 0, logical_w_, logical_h_));
  if (l.empty()) return;

  const bool invert_sw = night_mode_ && !is_mtk_;
  const float sat = saturation_;

  for (int y = l.y; y < l.bottom(); ++y) {
    const uint32_t* src = canvas_.row(y) + l.x;
    for (int x = l.x; x < l.right(); ++x) {
      Color c = Color::unpack(src[x - l.x]);
      if (sat != 0.0f) c = saturate(c, sat);
      if (invert_sw) c = c.inverted();

      int dx = x, dy = y;
      switch (rotation_) {
        case Rotation::Portrait: break;
        case Rotation::PortraitFlipped:
          dx = fb_w_ - 1 - x;
          dy = fb_h_ - 1 - y;
          break;
        case Rotation::LandscapeCW:
          dx = fb_w_ - 1 - y;
          dy = x;
          break;
        case Rotation::LandscapeCCW:
          dx = y;
          dy = fb_h_ - 1 - x;
          break;
      }
      if (dx < 0 || dy < 0 || dx >= fb_w_ || dy >= fb_h_) continue;
      uint8_t* dst = fb_mem_ + (size_t)dy * fb_stride_;

      switch (fb_bpp_) {
        case 32: {
          uint32_t px = ((uint32_t)c.r << r_off_) | ((uint32_t)c.g << g_off_) |
                        ((uint32_t)c.b << b_off_) | ((uint32_t)0xFF << a_off_);
          *(uint32_t*)(dst + (size_t)dx * 4) = px;
          break;
        }
        case 16: {
          uint16_t px = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
          *(uint16_t*)(dst + (size_t)dx * 2) = px;
          break;
        }
        case 8:
        default:
          dst[dx] = c.luma();
          break;
      }
    }
  }
}

void Screen::send_update(const Rect& dr, Refresh mode, bool wait) {
  if (fb_fd_ < 0 || dr.empty()) return;

  Rect r = dr.align_to(8).clamped(fb_w_, fb_h_);
  uint32_t marker = marker_++;
  if (marker_ == 0) marker_ = 1;
  last_marker_ = marker;

  if (is_mtk_) {
    struct hwtcon_update_data u;
    memset(&u, 0, sizeof(u));
    u.update_region.top = (uint32_t)r.y;
    u.update_region.left = (uint32_t)r.x;
    u.update_region.width = (uint32_t)r.w;
    u.update_region.height = (uint32_t)r.h;
    u.update_marker = marker;
    u.update_mode = CK_UPDATE_MODE_PARTIAL;
    u.flags = 0;
    u.dither_mode = 0;

    switch (mode) {
      case Refresh::Fast:
        u.waveform_mode = HWTCON_WAVEFORM_MODE_DU;
        break;
      case Refresh::Pen:
        // A2 with forced A2 output is the low-latency ink path: the driver
        // clamps output to two tones and skips the slower transitions.
        u.waveform_mode = HWTCON_WAVEFORM_MODE_A2;
        u.flags |= HWTCON_FLAG_FORCE_A2_OUTPUT;
        break;
      case Refresh::Text:
        u.waveform_mode = night_mode_ ? HWTCON_WAVEFORM_MODE_GCK16 : HWTCON_WAVEFORM_MODE_GL16;
        break;
      case Refresh::Image:
        u.waveform_mode = HWTCON_WAVEFORM_MODE_GC16;
        break;
      case Refresh::Color:
        // Kaleido-tuned waveforms must be paired with a FULL update.
        u.waveform_mode = HWTCON_WAVEFORM_MODE_GCC16;
        u.update_mode = CK_UPDATE_MODE_FULL;
        break;
      case Refresh::Highlight:
        u.waveform_mode =
            night_mode_ ? HWTCON_WAVEFORM_MODE_GLKW16 : HWTCON_WAVEFORM_MODE_GLRC16;
        u.update_mode = CK_UPDATE_MODE_FULL;
        break;
      case Refresh::Flash:
        u.waveform_mode = HWTCON_WAVEFORM_MODE_GC16;
        u.update_mode = CK_UPDATE_MODE_FULL;
        break;
      case Refresh::Init:
        u.waveform_mode = HWTCON_WAVEFORM_MODE_INIT;
        u.update_mode = CK_UPDATE_MODE_FULL;
        break;
      case Refresh::Auto:
      default:
        u.waveform_mode = HWTCON_WAVEFORM_MODE_AUTO;
        break;
    }

    if (color_panel_ && (mode == Refresh::Color || mode == Refresh::Highlight)) {
      switch (cfa_mode_) {
        case CfaMode::Boost: u.flags |= HWTCON_FLAG_CFA_EINK_G2; break;
        case CfaMode::Desaturate: u.flags |= HWTCON_FLAG_CFA_EINK_G0; break;
        case CfaMode::Vivid: u.flags |= HWTCON_FLAG_CFA_EINK_AIE_S7; break;
        case CfaMode::Mono: u.flags |= HWTCON_FLAG_CFA_SKIP; break;
        case CfaMode::Standard: u.flags |= HWTCON_FLAG_CFA_EINK_G1; break;
        case CfaMode::Default: break;
      }
    } else if (color_panel_ && cfa_mode_ == CfaMode::Mono) {
      u.flags |= HWTCON_FLAG_CFA_SKIP;
    }

    if (ioctl(fb_fd_, HWTCON_SEND_UPDATE, &u) < 0) {
      CK_LOGW("screen: HWTCON_SEND_UPDATE (%d,%d %dx%d wfm=%u): %s", r.x, r.y, r.w, r.h,
              u.waveform_mode, strerror(errno));
      return;
    }
  } else {
    struct mxcfb_update_data_ntx u;
    memset(&u, 0, sizeof(u));
    u.update_region.top = (uint32_t)r.y;
    u.update_region.left = (uint32_t)r.x;
    u.update_region.width = (uint32_t)r.w;
    u.update_region.height = (uint32_t)r.h;
    u.update_marker = marker;
    u.temp = CK_TEMP_USE_AMBIENT;
    u.update_mode = CK_UPDATE_MODE_PARTIAL;

    switch (mode) {
      case Refresh::Fast:
      case Refresh::Pen:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_A2;
        break;
      case Refresh::Text:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_GL16;
        break;
      case Refresh::Image:
      case Refresh::Color:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_GC16;
        break;
      case Refresh::Highlight:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_REAGL;
        break;
      case Refresh::Flash:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_GC16;
        u.update_mode = CK_UPDATE_MODE_FULL;
        break;
      case Refresh::Init:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_INIT;
        u.update_mode = CK_UPDATE_MODE_FULL;
        break;
      case Refresh::Auto:
      default:
        u.waveform_mode = MXCFB_WAVEFORM_MODE_AUTO;
        break;
    }
    if (ioctl(fb_fd_, MXCFB_SEND_UPDATE, &u) < 0) {
      CK_LOGW("screen: MXCFB_SEND_UPDATE: %s", strerror(errno));
      return;
    }
  }

  if (wait) wait_for_complete();
}

void Screen::wait_for_complete() {
  if (fb_fd_ < 0 || last_marker_ == 0) return;
  if (is_mtk_) {
    struct hwtcon_update_marker_data m;
    memset(&m, 0, sizeof(m));
    m.update_marker = last_marker_;
    ioctl(fb_fd_, HWTCON_WAIT_FOR_UPDATE_COMPLETE, &m);
  } else {
    uint32_t marker = last_marker_;
    ioctl(fb_fd_, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &marker);
  }
  last_marker_ = 0;
}

void Screen::flush(const Rect& r, Refresh mode, bool wait) {
  Rect l = r.intersect(Rect(0, 0, logical_w_, logical_h_));
  if (l.empty()) return;

  // Colour content needs a colour waveform; a plain Auto/Text refresh of a
  // colour region renders it as grey. Auto therefore inspects the region.
  if (mode == Refresh::Auto && color_panel_) {
    bool has_color = false;
    for (int y = l.y; y < l.bottom() && !has_color; y += 3) {
      const uint32_t* row = canvas_.row(y);
      for (int x = l.x; x < l.right(); x += 3) {
        Color c = Color::unpack(row[x]);
        int mx = std::max(c.r, std::max(c.g, c.b));
        int mn = std::min(c.r, std::min(c.g, c.b));
        if (mx - mn > 24) {
          has_color = true;
          break;
        }
      }
    }
    mode = has_color ? Refresh::Color : Refresh::Text;
  }

  convert_and_copy(l);
  if (headless_) return;
  send_update(to_device(l), mode, wait);
}

void Screen::flush_all(Refresh mode, bool wait) {
  flush(Rect(0, 0, logical_w_, logical_h_), mode, wait);
  since_flash_ = 0;
}

void Screen::set_night_mode(bool on) {
  if (night_mode_ == on) return;
  night_mode_ = on;
  // On MTK the eclipse waveforms (GCK16/GLKW16) handle inversion in the
  // controller; elsewhere convert_and_copy() inverts in software.
}

void Screen::set_cfa_mode(CfaMode mode) {
  cfa_mode_ = mode;
  if (fb_fd_ < 0 || !is_mtk_ || !color_panel_) return;
  uint32_t kernel_mode = HWTCON_CFA_MODE_EINK_G1;
  switch (mode) {
    case CfaMode::Boost:
    case CfaMode::Vivid: kernel_mode = HWTCON_CFA_MODE_EINK_G2; break;
    case CfaMode::Mono: kernel_mode = HWTCON_CFA_MODE_NONE; break;
    default: kernel_mode = HWTCON_CFA_MODE_EINK_G1; break;
  }
  ioctl(fb_fd_, HWTCON_SET_CFA_MODE, &kernel_mode);
}

void Screen::note_page_turn() { ++since_flash_; }

bool Screen::flash_due() const {
  return flash_interval_ > 0 && since_flash_ >= flash_interval_;
}

bool Screen::save_screenshot(const std::string& path) { return canvas_.save_png(path); }

}  // namespace ck
