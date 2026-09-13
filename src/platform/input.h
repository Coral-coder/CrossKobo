#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/geometry.h"

namespace ck {

enum class Key {
  None,
  PageForward,
  PageBack,
  Power,
  Home,
  Light,
  Menu,
};

enum class SwipeDir { None, Left, Right, Up, Down };
enum class Tool { Finger, Pen, Eraser };

enum class EventType {
  None,
  TouchDown,
  TouchMove,
  TouchUp,
  Tap,
  LongPress,
  Swipe,
  PenDown,
  PenMove,
  PenUp,
  KeyDown,
  KeyUp,
  SleepCoverClosed,
  SleepCoverOpened,
  UsbPlugged,
  UsbUnplugged,
  Timeout,
};

struct InputEvent {
  EventType type = EventType::None;
  int x = 0;
  int y = 0;
  int pressure = 0;        // 0..1000 normalised
  Tool tool = Tool::Finger;
  Key key = Key::None;
  SwipeDir swipe = SwipeDir::None;
  int dx = 0, dy = 0;      // movement since the previous event
  int64_t time_ms = 0;

  bool is_touch() const {
    return type == EventType::TouchDown || type == EventType::TouchMove ||
           type == EventType::TouchUp || type == EventType::Tap ||
           type == EventType::LongPress || type == EventType::Swipe;
  }
  bool is_pen() const {
    return type == EventType::PenDown || type == EventType::PenMove || type == EventType::PenUp;
  }
};

// How raw digitiser coordinates map onto the logical screen. Both the touch
// panel and the stylus digitiser can be mirrored or transposed relative to
// the display, and it varies by model, so the values live in settings and
// there is a calibration screen that lets the user fix them by hand.
struct TouchTransform {
  bool swap_xy = false;
  bool mirror_x = false;
  bool mirror_y = false;
};

// What a device's capability bits say it is. A panel can be both: the Libra
// Colour's Elan controller reports finger contacts and a stylus on one
// node, and treating it as only a digitiser leaves touch dead.
struct InputCaps {
  bool touch = false;
  bool pen = false;
};
InputCaps classify_caps(bool has_pen_tool, bool has_mt, bool has_abs_xy, bool has_btn_touch);

class Input {
 public:
  static Input& instance();

  // Opens every input device that looks useful. Safe to call twice.
  bool open();
  void close();
  // Re-scans for devices; the stylus only appears when it is first used on
  // some firmware, and USB plug/unplug can re-enumerate the touch panel.
  void rescan();

  void set_touch_transform(const TouchTransform& t) { touch_tf_ = t; }
  void set_pen_transform(const TouchTransform& t) { pen_tf_ = t; }
  TouchTransform touch_transform() const { return touch_tf_; }
  TouchTransform pen_transform() const { return pen_tf_; }
  void set_screen_size(int w, int h);
  // Rotation applied on top of the transform, so the UI can rotate without
  // the platform layer re-opening anything.
  void set_rotation(int quarter_turns) { rotation_ = ((quarter_turns % 4) + 4) % 4; }

  // Blocks for up to `timeout_ms` and returns the next gesture-level event.
  // Returns a Timeout event when nothing happened.
  InputEvent next(int timeout_ms);
  // Discards anything queued; used after a long operation so a stack of
  // stale taps does not fire at once.
  void drain();
  bool has_pen() const { return pen_fd_count_ > 0; }

  // Diagnostics for the calibration screen.
  std::string describe_devices() const;

  // Test hook: pushes a synthetic event onto the queue. Used by the host
  // simulator and the test suite, which have no /dev/input.
  void inject(const InputEvent& event) { queue_.push_back(event); }

 private:
  Input() = default;

  struct Device {
    int fd = -1;
    std::string path;
    std::string name;
    bool touch = false;
    bool pen = false;
    bool keys = false;
    bool cover = false;
    // The Libra Colour's Elan panel is one device for both finger and
    // stylus, so `touch` and `pen` are not exclusive: the tool events say
    // which one is on the glass right now.
    bool pen_active = false;
    // Raw axis ranges, used to normalise to screen coordinates. The stylus
    // can report on different axes to the finger, so both are kept.
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0, max_pressure = 0;
    int pen_min_x = 0, pen_max_x = 0, pen_min_y = 0, pen_max_y = 0;
    // Coordinates seen so far in the current SYN_REPORT frame, per device.
    int pending_x = -1, pending_y = -1;

    // True when this frame's coordinates belong to the stylus.
    bool as_pen() const { return pen && (!touch || pen_active); }
  };

  bool classify(Device& d);
  void read_device(Device& d, std::vector<InputEvent>& out);
  void map_point(const Device& d, int raw_x, int raw_y, bool is_pen, int& out_x, int& out_y) const;
  void flush_touch_gesture(std::vector<InputEvent>& out, int64_t now);

  // Logs the first few mapped points, so a device whose panel geometry
  // nobody has checked can be diagnosed from the log alone.
  mutable int map_log_left_ = 6;

  std::vector<Device> devices_;
  std::vector<InputEvent> queue_;
  int pen_fd_count_ = 0;

  TouchTransform touch_tf_;
  TouchTransform pen_tf_;
  int screen_w_ = 0, screen_h_ = 0;
  int rotation_ = 0;

  // Touch gesture state for the primary contact.
  bool touching_ = false;
  bool moved_ = false;
  bool long_press_sent_ = false;
  int start_x_ = 0, start_y_ = 0;
  int last_x_ = 0, last_y_ = 0;
  int64_t start_time_ = 0;
  int active_slot_ = 0;
  int current_slot_ = 0;

  bool pen_down_ = false;
  int pen_x_ = 0, pen_y_ = 0, pen_pressure_ = 0;
  Tool pen_tool_ = Tool::Pen;
};

}  // namespace ck
