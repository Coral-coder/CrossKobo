#include "platform/input.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/device.h"

namespace ck {
namespace {

constexpr int kTapSlopPx = 18;
constexpr int kSwipeMinPx = 70;
constexpr int kLongPressMs = 650;
constexpr int kMaxTapMs = 500;

bool test_bit(const unsigned long* bits, int nr) {
  return (bits[nr / (8 * sizeof(long))] >> (nr % (8 * sizeof(long)))) & 1UL;
}

Key key_from_code(int code) {
  // Codes confirmed against Kobo hardware: 193/194 are the page-turn
  // buttons on the Libra/Forma/Sage bodies, 102 is Home, 90 the light
  // button, 116 the power slider.
  switch (code) {
    case 193: return Key::PageBack;
    case 194: return Key::PageForward;
    case KEY_PAGEUP: return Key::PageBack;
    case KEY_PAGEDOWN: return Key::PageForward;
    case KEY_LEFT: return Key::PageBack;
    case KEY_RIGHT: return Key::PageForward;
    case 102: return Key::Home;
    case 90: return Key::Light;
    case KEY_POWER: return Key::Power;
    case KEY_MENU: return Key::Menu;
    default: return Key::None;
  }
}

}  // namespace

Input& Input::instance() {
  static Input in;
  return in;
}

void Input::set_screen_size(int w, int h) {
  screen_w_ = w;
  screen_h_ = h;
}

bool Input::classify(Device& d) {
  unsigned long ev_bits[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
  if (ioctl(d.fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return false;

  char name[256] = {0};
  if (ioctl(d.fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) d.name = name;

  unsigned long key_bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
  unsigned long abs_bits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
  unsigned long sw_bits[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
  if (test_bit(ev_bits, EV_KEY)) ioctl(d.fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
  if (test_bit(ev_bits, EV_ABS)) ioctl(d.fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
  if (test_bit(ev_bits, EV_SW)) ioctl(d.fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits);

  bool has_pen_tool = test_bit(key_bits, BTN_TOOL_PEN) || test_bit(key_bits, BTN_TOOL_RUBBER);
  bool has_mt = test_bit(abs_bits, ABS_MT_POSITION_X);
  bool has_abs_xy = test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y);

  // A digitiser reports a pen tool; a touch panel reports multitouch slots
  // or a plain BTN_TOUCH with absolute axes.
  if (has_pen_tool && has_abs_xy) {
    d.pen = true;
  } else if (has_mt || (has_abs_xy && test_bit(key_bits, BTN_TOUCH))) {
    d.touch = true;
  }

  if (test_bit(key_bits, KEY_POWER) || test_bit(key_bits, 193) || test_bit(key_bits, 194) ||
      test_bit(key_bits, KEY_PAGEUP) || test_bit(key_bits, KEY_PAGEDOWN) ||
      test_bit(key_bits, 102) || test_bit(key_bits, 90)) {
    d.keys = true;
  }
  if (test_bit(sw_bits, SW_LID) || test_bit(key_bits, 59) || test_bit(key_bits, 35)) {
    d.cover = true;
  }

  struct input_absinfo abs;
  int ax = d.pen || !has_mt ? ABS_X : ABS_MT_POSITION_X;
  int ay = d.pen || !has_mt ? ABS_Y : ABS_MT_POSITION_Y;
  if (ioctl(d.fd, EVIOCGABS(ax), &abs) == 0) {
    d.min_x = abs.minimum;
    d.max_x = abs.maximum;
  }
  if (ioctl(d.fd, EVIOCGABS(ay), &abs) == 0) {
    d.min_y = abs.minimum;
    d.max_y = abs.maximum;
  }
  if (ioctl(d.fd, EVIOCGABS(ABS_PRESSURE), &abs) == 0 && abs.maximum > 0) {
    d.max_pressure = abs.maximum;
  } else if (ioctl(d.fd, EVIOCGABS(ABS_MT_PRESSURE), &abs) == 0) {
    d.max_pressure = abs.maximum;
  }

  return d.touch || d.pen || d.keys || d.cover;
}

bool Input::open() {
  if (!devices_.empty()) return true;
  rescan();
  return !devices_.empty();
}

void Input::rescan() {
  close();
  const DeviceInfo& dev = device();
  // Sensible defaults per model; the calibration screen can override them
  // and the values are then remembered in settings.
  if (dev.codename.find("monza") != std::string::npos) {
    touch_tf_.mirror_y = true;
  }

  for (const fs::Entry& e : fs::list_dir("/dev/input", true)) {
    if (!starts_with(e.name, "event")) continue;
    Device d;
    d.path = e.path;
    d.fd = ::open(e.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (d.fd < 0) continue;
    if (!classify(d)) {
      ::close(d.fd);
      continue;
    }
    CK_LOGI("input: %s \"%s\"%s%s%s%s range=%d..%d/%d..%d pmax=%d", d.path.c_str(),
            d.name.c_str(), d.touch ? " touch" : "", d.pen ? " pen" : "", d.keys ? " keys" : "",
            d.cover ? " cover" : "", d.min_x, d.max_x, d.min_y, d.max_y, d.max_pressure);
    if (d.pen) ++pen_fd_count_;
    devices_.push_back(d);
  }
  if (devices_.empty()) CK_LOGW("input: no usable input devices found");
}

void Input::close() {
  for (Device& d : devices_) {
    if (d.fd >= 0) ::close(d.fd);
  }
  devices_.clear();
  pen_fd_count_ = 0;
  queue_.clear();
  pen_samples_.clear();
  touching_ = false;
  pen_down_ = false;
}

void Input::map_point(const Device& d, int raw_x, int raw_y, bool is_pen, int& out_x,
                      int& out_y) const {
  const TouchTransform& tf = is_pen ? pen_tf_ : touch_tf_;
  int span_x = d.max_x > d.min_x ? d.max_x - d.min_x : 0;
  int span_y = d.max_y > d.min_y ? d.max_y - d.min_y : 0;

  // Normalise to 0..1 in digitiser space, then place on the panel. Doing it
  // in normalised space means a digitiser with a different resolution to
  // the panel (common for stylus layers) still lands in the right place.
  float nx = span_x ? (float)(raw_x - d.min_x) / (float)span_x : 0.0f;
  float ny = span_y ? (float)(raw_y - d.min_y) / (float)span_y : 0.0f;

  if (tf.swap_xy) std::swap(nx, ny);
  if (tf.mirror_x) nx = 1.0f - nx;
  if (tf.mirror_y) ny = 1.0f - ny;

  // The screen may be rotated relative to the panel.
  for (int i = 0; i < rotation_; ++i) {
    float t = nx;
    nx = 1.0f - ny;
    ny = t;
  }

  int w = screen_w_ > 0 ? screen_w_ : 1;
  int h = screen_h_ > 0 ? screen_h_ : 1;
  out_x = std::max(0, std::min(w - 1, (int)(nx * (float)w)));
  out_y = std::max(0, std::min(h - 1, (int)(ny * (float)h)));
}

void Input::flush_touch_gesture(std::vector<InputEvent>& out, int64_t now) {
  if (!touching_) return;
  InputEvent up;
  up.type = EventType::TouchUp;
  up.x = last_x_;
  up.y = last_y_;
  up.time_ms = now;
  out.push_back(up);

  int dx = last_x_ - start_x_;
  int dy = last_y_ - start_y_;
  int adx = std::abs(dx), ady = std::abs(dy);
  int64_t dt = now - start_time_;

  if (!moved_ && !long_press_sent_ && dt <= kMaxTapMs) {
    InputEvent tap;
    tap.type = EventType::Tap;
    tap.x = start_x_;
    tap.y = start_y_;
    tap.time_ms = now;
    out.push_back(tap);
  } else if (std::max(adx, ady) >= kSwipeMinPx) {
    InputEvent sw;
    sw.type = EventType::Swipe;
    sw.x = start_x_;
    sw.y = start_y_;
    sw.dx = dx;
    sw.dy = dy;
    sw.time_ms = now;
    if (adx > ady) {
      sw.swipe = dx < 0 ? SwipeDir::Left : SwipeDir::Right;
    } else {
      sw.swipe = dy < 0 ? SwipeDir::Up : SwipeDir::Down;
    }
    out.push_back(sw);
  }

  touching_ = false;
  moved_ = false;
  long_press_sent_ = false;
}

void Input::read_device(Device& d, std::vector<InputEvent>& out) {
  struct input_event evs[64];
  ssize_t n;
  int& raw_x = d.pending_x;  // pending values within one SYN frame
  int& raw_y = d.pending_y;
  while ((n = read(d.fd, evs, sizeof(evs))) > 0) {
    int count = (int)(n / (ssize_t)sizeof(struct input_event));
    for (int i = 0; i < count; ++i) {
      const struct input_event& ev = evs[i];
      int64_t now = now_ms();

      if (ev.type == EV_KEY) {
        if (d.pen && (ev.code == BTN_TOOL_PEN || ev.code == BTN_TOOL_RUBBER)) {
          pen_tool_ = ev.code == BTN_TOOL_RUBBER ? Tool::Eraser : Tool::Pen;
          if (!ev.value && pen_down_) {
            InputEvent e;
            e.type = EventType::PenUp;
            e.x = pen_x_;
            e.y = pen_y_;
            e.tool = pen_tool_;
            e.time_ms = now;
            out.push_back(e);
            pen_samples_.push_back(e);
            pen_down_ = false;
          }
          continue;
        }
        if (d.pen && ev.code == BTN_TOUCH) {
          if (ev.value) {
            pen_down_ = true;
            InputEvent e;
            e.type = EventType::PenDown;
            e.x = pen_x_;
            e.y = pen_y_;
            e.pressure = pen_pressure_;
            e.tool = pen_tool_;
            e.time_ms = now;
            out.push_back(e);
            pen_samples_.push_back(e);
          } else if (pen_down_) {
            InputEvent e;
            e.type = EventType::PenUp;
            e.x = pen_x_;
            e.y = pen_y_;
            e.tool = pen_tool_;
            e.time_ms = now;
            out.push_back(e);
            pen_samples_.push_back(e);
            pen_down_ = false;
          }
          continue;
        }
        if (ev.code == BTN_TOUCH && d.touch) {
          if (ev.value && !touching_) {
            touching_ = true;
            moved_ = false;
            long_press_sent_ = false;
            start_time_ = now;
            start_x_ = last_x_;
            start_y_ = last_y_;
            InputEvent e;
            e.type = EventType::TouchDown;
            e.x = last_x_;
            e.y = last_y_;
            e.time_ms = now;
            out.push_back(e);
          } else if (!ev.value) {
            flush_touch_gesture(out, now);
          }
          continue;
        }
        if (d.cover && (ev.code == 59 || ev.code == 35)) {
          InputEvent e;
          e.type = ev.value ? EventType::SleepCoverClosed : EventType::SleepCoverOpened;
          e.time_ms = now;
          out.push_back(e);
          continue;
        }
        Key k = key_from_code(ev.code);
        if (k != Key::None && ev.value != 2) {
          InputEvent e;
          e.type = ev.value ? EventType::KeyDown : EventType::KeyUp;
          e.key = k;
          e.time_ms = now;
          out.push_back(e);
        }
        continue;
      }

      if (ev.type == EV_ABS) {
        switch (ev.code) {
          case ABS_MT_SLOT:
            current_slot_ = ev.value;
            break;
          case ABS_MT_TRACKING_ID:
            if (ev.value == -1) {
              if (current_slot_ == active_slot_) flush_touch_gesture(out, now);
            } else if (!touching_ && current_slot_ >= 0) {
              active_slot_ = current_slot_;
              touching_ = true;
              moved_ = false;
              long_press_sent_ = false;
              start_time_ = now;
              // Position arrives in the same frame; start_* is fixed up on
              // the first move below.
              start_x_ = last_x_;
              start_y_ = last_y_;
              InputEvent e;
              e.type = EventType::TouchDown;
              e.x = last_x_;
              e.y = last_y_;
              e.time_ms = now;
              out.push_back(e);
            }
            break;
          case ABS_MT_POSITION_X:
          case ABS_X:
            raw_x = ev.value;
            break;
          case ABS_MT_POSITION_Y:
          case ABS_Y:
            raw_y = ev.value;
            break;
          case ABS_PRESSURE:
          case ABS_MT_PRESSURE:
            if (d.pen) {
              pen_pressure_ = d.max_pressure > 0
                                  ? (int)((int64_t)ev.value * 1000 / d.max_pressure)
                                  : 500;
            }
            break;
          default:
            break;
        }
        continue;
      }

      if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (raw_x < 0 && raw_y < 0) continue;
        int mx = 0, my = 0;
        map_point(d, raw_x < 0 ? 0 : raw_x, raw_y < 0 ? 0 : raw_y, d.pen, mx, my);
        if (d.pen) {
          pen_x_ = mx;
          pen_y_ = my;
          if (pen_down_) {
            InputEvent e;
            e.type = EventType::PenMove;
            e.x = mx;
            e.y = my;
            e.pressure = pen_pressure_;
            e.tool = pen_tool_;
            e.time_ms = now;
            pen_samples_.push_back(e);
            out.push_back(e);
          }
        } else {
          bool first = touching_ && !moved_ && start_x_ == last_x_ && start_y_ == last_y_ &&
                       (last_x_ == 0 && last_y_ == 0);
          last_x_ = mx;
          last_y_ = my;
          if (first) {
            start_x_ = mx;
            start_y_ = my;
          }
          if (touching_) {
            int dx = mx - start_x_;
            int dy = my - start_y_;
            if (std::abs(dx) > kTapSlopPx || std::abs(dy) > kTapSlopPx) moved_ = true;
            InputEvent e;
            e.type = EventType::TouchMove;
            e.x = mx;
            e.y = my;
            e.dx = dx;
            e.dy = dy;
            e.time_ms = now;
            out.push_back(e);
          }
        }
        raw_x = raw_y = -1;
      }
    }
  }
}

InputEvent Input::next(int timeout_ms) {
  if (!queue_.empty()) {
    InputEvent e = queue_.front();
    queue_.erase(queue_.begin());
    return e;
  }

  int64_t deadline = now_ms() + std::max(0, timeout_ms);
  while (true) {
    // A held finger has to produce a long-press without any new hardware
    // event, so the poll timeout shrinks while a touch is active.
    int wait = (int)std::max<int64_t>(0, deadline - now_ms());
    if (touching_ && !long_press_sent_) {
      int64_t remaining = kLongPressMs - (now_ms() - start_time_);
      wait = (int)std::max<int64_t>(0, std::min<int64_t>(wait, remaining));
    }

    std::vector<struct pollfd> pfds;
    pfds.reserve(devices_.size());
    for (const Device& d : devices_) {
      struct pollfd p;
      p.fd = d.fd;
      p.events = POLLIN;
      p.revents = 0;
      pfds.push_back(p);
    }

    int rv = pfds.empty() ? 0 : poll(pfds.data(), pfds.size(), wait);
    if (rv < 0) {
      if (errno == EINTR) continue;
      CK_LOGW("input: poll: %s", strerror(errno));
      sleep_ms(50);
    } else if (rv > 0) {
      for (size_t i = 0; i < pfds.size(); ++i) {
        if (pfds[i].revents & POLLIN) read_device(devices_[i], queue_);
      }
    } else if (pfds.empty()) {
      sleep_ms(std::min(wait, 50));
    }

    if (touching_ && !long_press_sent_ && now_ms() - start_time_ >= kLongPressMs && !moved_) {
      long_press_sent_ = true;
      InputEvent e;
      e.type = EventType::LongPress;
      e.x = last_x_;
      e.y = last_y_;
      e.time_ms = now_ms();
      queue_.push_back(e);
    }

    if (!queue_.empty()) {
      InputEvent e = queue_.front();
      queue_.erase(queue_.begin());
      return e;
    }
    if (now_ms() >= deadline) {
      InputEvent e;
      e.type = EventType::Timeout;
      e.time_ms = now_ms();
      return e;
    }
  }
}

void Input::drain() {
  std::vector<InputEvent> scratch;
  for (Device& d : devices_) read_device(d, scratch);
  queue_.clear();
  pen_samples_.clear();
}

std::vector<InputEvent> Input::take_pen_samples() {
  std::vector<InputEvent> out;
  out.swap(pen_samples_);
  return out;
}

std::string Input::describe_devices() const {
  std::string out;
  for (const Device& d : devices_) {
    out += format("%s \"%s\"%s%s%s%s x:%d-%d y:%d-%d p:%d\n", d.path.c_str(), d.name.c_str(),
                  d.touch ? " touch" : "", d.pen ? " pen" : "", d.keys ? " keys" : "",
                  d.cover ? " cover" : "", d.min_x, d.max_x, d.min_y, d.max_y, d.max_pressure);
  }
  if (out.empty()) out = "no input devices\n";
  return out;
}

}  // namespace ck
