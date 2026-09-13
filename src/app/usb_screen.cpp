// What CrossKobo shows while the drive is shared with a computer, and the
// choice it offers when the cable goes in.
#include <memory>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/str.h"
#include "library/library.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "platform/system.h"
#include "platform/usbms.h"
#include "reader/state.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/widgets.h"

namespace ck {
namespace {

// ------------------------------------------------------------- active view
// While the export is running the user partition is unmounted, so this
// screen deliberately touches nothing on it: no covers, no settings writes.
class UsbActiveScreen : public View {
 public:
  std::string title() const override { return "Connected to a computer"; }
  Refresh refresh_hint() const override { return Refresh::Text; }
  int tick_ms() const override { return 1000; }

  bool on_tick() override {
    UsbMs& usb = UsbMs::instance();
    if (!usb.active()) return false;
    // The cable being pulled is the usual way this ends.
    if (usb.cable_gone()) {
      finish("The cable was disconnected.");
      return true;
    }
    bool now_mounted = usb.host_connected();
    if (now_mounted != host_mounted_) {
      host_mounted_ = now_mounted;
      return true;
    }
    return false;
  }

  void draw(Canvas& canvas, const Rect& bounds) override {
    const Theme& th = theme();
    hits_.clear();
    canvas.clear(th.bg);

    StatusBarInfo info;
    BatteryState battery = Power::instance().battery();
    info.battery_percent = battery.percent;
    info.charging = battery.charging;
    int top = draw_top_bar(canvas, bounds, "Connected to a computer", info);

    Rect body(bounds.x, bounds.y + top, bounds.w, bounds.h - top - th.row_height * 2);

    // A plug, drawn large, so it reads across a room.
    int icon_size = std::min(body.w, body.h) / 3;
    Rect icon((bounds.w - icon_size) / 2, body.y + body.h / 6, icon_size, icon_size);
    draw_icon(canvas, icon, Icon::Book, th.faint, std::max(3, icon_size / 24));

    std::string headline = host_mounted_ ? "Your computer has the drive"
                                         : "Waiting for your computer";
    draw_text_in(canvas,
                 Rect(bounds.x + th.padding, icon.bottom() + th.padding,
                      bounds.w - 2 * th.padding, th.title_px + 12),
                 headline, ui_style(th.title_px, th.fg, FontStyle::Bold), 0);

    draw_centered_message(
        canvas,
        Rect(bounds.x + th.padding * 2, icon.bottom() + th.padding * 3,
             bounds.w - 4 * th.padding, th.row_height * 4),
        "Books, notebooks and everything else on the drive are available on "
        "your computer.\n\nEject the drive there, then unplug the cable - or tap "
        "Stop below.");

    Rect stop((bounds.w - bounds.w / 2) / 2, bounds.bottom() - th.row_height * 2,
              bounds.w / 2, th.row_height);
    draw_button(canvas, stop, "Stop sharing", ButtonStyle::Primary);
    hits_.add(stop, 1);
  }

  bool handle(const InputEvent& event) override {
    if (event.type == EventType::Tap && hits_.hit(event.x, event.y) == 1) {
      finish("");
      return true;
    }
    // The power button would otherwise put the device to sleep mid-copy.
    if (event.type == EventType::KeyDown && event.key == Key::Power) return true;
    return false;
  }

 private:
  void finish(const std::string& note) {
    UsbMs& usb = UsbMs::instance();
    bool ok = usb.stop();
    // Whatever the computer changed, the library and covers are stale now.
    covers::clear_memory_cache();
    Recents::instance().load();
    Recents::instance().prune();
    App::instance().pop();
    App::instance().invalidate(Refresh::Flash);
    if (!ok) {
      App::instance().show_message(
          "The drive did not come back",
          "CrossKobo could not mount the drive again:\n" + usb.last_error() +
              "\n\nRestart the device: it will mount normally on the next boot.");
    } else if (!note.empty()) {
      App::instance().show_toast(note);
    }
  }

  bool host_mounted_ = false;
  HitList hits_;
};

// ------------------------------------------------------------- the chooser
class UsbPromptScreen : public View {
 public:
  std::string title() const override { return "USB connected"; }
  Refresh refresh_hint() const override { return Refresh::Text; }
  int tick_ms() const override { return 1000; }

  bool on_tick() override {
    // If the cable is pulled while the question is on screen, drop it.
    if (!sys::usb_plugged()) {
      App::instance().pop();
      App::instance().invalidate(Refresh::Text);
      return true;
    }
    return false;
  }

  void draw(Canvas& canvas, const Rect& bounds) override {
    const Theme& th = theme();
    hits_.clear();
    canvas.clear(th.bg);
    StatusBarInfo info;
    BatteryState battery = Power::instance().battery();
    info.battery_percent = battery.percent;
    info.charging = battery.charging;
    int top = draw_top_bar(canvas, bounds, "USB connected", info);

    Rect body(bounds.x + th.padding, bounds.y + top + th.padding, bounds.w - 2 * th.padding,
              bounds.h - top - th.padding);
    draw_text_in(canvas, Rect(body.x, body.y, body.w, th.title_px + 10), "What now?",
                 ui_style(th.title_px, th.fg, FontStyle::Bold), -1);

    struct Choice {
      const char* label;
      const char* detail;
      int id;
      ButtonStyle style;
    };
    UsbMs& usb = UsbMs::instance();
    std::string blocker = usb.blocker();
    const Choice choices[] = {
        {"Share the drive", "Your computer sees the books and notebooks. CrossKobo waits.", 1,
         ButtonStyle::Primary},
        {"Switch to the Kobo UI", "The stock software takes over until the next restart.", 2,
         ButtonStyle::Normal},
        {"Just charge", "Carry on reading.", 3, ButtonStyle::Normal},
    };

    int y = body.y + th.title_px + th.padding * 2;
    for (const Choice& choice : choices) {
      bool disabled = choice.id == 1 && !blocker.empty();
      Rect button(body.x, y, body.w, th.row_height);
      draw_button(canvas, button, choice.label,
                  disabled ? ButtonStyle::Ghost : choice.style, disabled);
      if (!disabled) hits_.add(button, choice.id);
      std::string detail = disabled ? blocker : choice.detail;
      draw_text_in(canvas, Rect(body.x + th.padding, button.bottom() + 2, body.w, th.small_px + 8),
                   detail, ui_style(th.small_px, th.muted), -1);
      y = button.bottom() + th.small_px + th.padding + 6;
    }

    draw_text_in(canvas, Rect(body.x, bounds.bottom() - th.row_height, body.w, th.small_px + 8),
                 "Settings has an option to stop asking.", ui_style(th.small_px, th.faint), -1);
  }

  bool handle(const InputEvent& event) override {
    if (event.type != EventType::Tap) return false;
    switch (hits_.hit(event.x, event.y)) {
      case 1: {
        UsbMs& usb = UsbMs::instance();
        // Settings must reach the disk before the partition goes away.
        settings().save();
        App::instance().pop();
        if (!usb.start()) {
          App::instance().show_message("Could not share the drive", usb.last_error());
          App::instance().invalidate(Refresh::Flash);
          return true;
        }
        App::instance().push(make_usb_active_screen());
        App::instance().invalidate(Refresh::Flash);
        return true;
      }
      case 2:
        App::instance().pop();
        App::instance().return_to_kobo_ui();
        return true;
      case 3:
        App::instance().pop();
        App::instance().show_toast("Charging");
        return true;
      default:
        return false;
    }
  }

 private:
  HitList hits_;
};

}  // namespace

ViewPtr make_usb_active_screen() { return std::make_unique<UsbActiveScreen>(); }
ViewPtr make_usb_prompt_screen() { return std::make_unique<UsbPromptScreen>(); }

}  // namespace ck
