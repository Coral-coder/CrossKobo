// The quick panel: a swipe down from the top edge of any screen. It carries
// the handful of things worth reaching without walking into Settings -
// light, night mode, Wi-Fi, sharing the drive, and the way home - because
// on an e-reader every extra screen costs a full refresh.
#include <algorithm>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/str.h"
#include "platform/net.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "platform/system.h"
#include "platform/usbms.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

enum QuickId {
  kLightDown = 1,
  kLightUp,
  kLightToggle,
  kNight,
  kWifi,
  kShare,
  kHome,
  kSettings,
  kSleep,
  kDismiss,
};

class QuickPanel : public View {
 public:
  std::string title() const override { return "Quick settings"; }
  Refresh refresh_hint() const override { return Refresh::Text; }
  bool opaque() const override { return false; }  // composites over the view below
  // The panel is reached by an edge gesture; it should not be dismissed by
  // one, or the same swipe would close it again.
  bool edge_gestures() const override { return false; }

  void draw(Canvas& canvas, const Rect& bounds) override {
    const Theme& th = theme();
    hits_.clear();
    Power& power = Power::instance();
    Net& net = Net::instance();

    int rows = 3;
    int hint_h = th.small_px + 10;
    int body_h = rows * (th.row_height + th.padding) + th.padding + hint_h;
    Rect card(bounds.x + th.padding, bounds.y + th.padding, bounds.w - 2 * th.padding, body_h);
    if (th.shadows) canvas.draw_soft_shadow(card, th.radius, 5, 44);
    if (th.gradients) {
      canvas.fill_round_rect_gradient(card, th.radius, th.panel_top, th.panel_bottom);
      canvas.fill_gloss(card, th.radius, 80);
      canvas.draw_round_rect(card, th.radius, th.border, 2);
    } else {
      draw_panel(canvas, card, true);
    }

    // Row 1: the front light, which is the reason most people open this.
    Rect row(card.x + th.padding, card.y + th.padding, card.w - 2 * th.padding, th.row_height);
    int btn = th.row_height + th.padding;
    Rect minus(row.x, row.y, btn, row.h);
    Rect plus(row.right() - btn, row.y, btn, row.h);
    Rect middle(minus.right() + th.padding / 2, row.y,
                plus.x - minus.right() - th.padding, row.h);
    draw_button(canvas, minus, "-", ButtonStyle::Normal);
    draw_button(canvas, plus, "+", ButtonStyle::Normal);
    std::string light = power.frontlight_on()
                            ? format("Light %d%%", power.brightness())
                            : std::string("Light off");
    draw_button(canvas, middle, light,
                power.frontlight_on() ? ButtonStyle::Primary : ButtonStyle::Normal);
    hits_.add(minus, kLightDown);
    hits_.add(plus, kLightUp);
    hits_.add(middle, kLightToggle);

    // Row 2: night mode and Wi-Fi.
    row.y += btn;
    int half = (row.w - th.padding) / 2;
    Rect night(row.x, row.y, half, row.h);
    Rect wifi(row.right() - half, row.y, half, row.h);
    draw_button(canvas, night, settings().night_mode ? "Night on" : "Night off",
                settings().night_mode ? ButtonStyle::Primary : ButtonStyle::Normal);
    std::string wifi_label = net.connected() ? "Wi-Fi: " + net.current_ssid()
                             : net.state() == NetState::Off ? "Wi-Fi off"
                                                            : "Wi-Fi on";
    draw_button(canvas, wifi, ellipsize(wifi_label, ui_style(th.base_px, th.button_text),
                                        wifi.w - th.padding),
                net.connected() ? ButtonStyle::Primary : ButtonStyle::Normal);
    hits_.add(night, kNight);
    hits_.add(wifi, kWifi);

    // Row 3: where to go, and the drive.
    row.y += btn;
    int third = (row.w - 2 * th.padding) / 3;
    Rect home(row.x, row.y, third, row.h);
    Rect settings_btn(home.right() + th.padding, row.y, third, row.h);
    Rect share(row.right() - third, row.y, third, row.h);
    draw_button(canvas, home, "Home", ButtonStyle::Normal);
    draw_button(canvas, settings_btn, "Settings", ButtonStyle::Normal);
    bool can_share = UsbMs::instance().supported() && sys::usb_plugged();
    draw_button(canvas, share, "Share drive", ButtonStyle::Normal, !can_share);
    hits_.add(home, kHome);
    hits_.add(settings_btn, kSettings);
    if (can_share) hits_.add(share, kShare);

    draw_text_in(canvas, Rect(card.x, card.bottom() - hint_h, card.w, hint_h),
                 "Swipe up from the bottom to go back", ui_style(th.small_px, th.muted), 0);

    // Anything below the card dismisses.
    hits_.add(Rect(bounds.x, card.bottom() + th.padding / 2, bounds.w,
                   bounds.h - card.bottom() - th.padding / 2),
              kDismiss);
  }

  bool handle(const InputEvent& event) override {
    if (event.type == EventType::KeyDown) {
      if (event.key == Key::PageBack || event.key == Key::PageForward) {
        App::instance().pop();
        return true;
      }
      return false;
    }
    if (event.type == EventType::Swipe && event.swipe == SwipeDir::Up) {
      App::instance().pop();
      return true;
    }
    if (event.type != EventType::Tap) return false;
    Power& power = Power::instance();
    switch (hits_.hit(event.x, event.y)) {
      case kLightDown:
      case kLightUp: {
        int step = 10;
        int value = std::max(1, std::min(100, power.brightness() +
                                                  (hits_.hit(event.x, event.y) == kLightUp
                                                       ? step
                                                       : -step)));
        power.set_frontlight_on(true);
        power.set_brightness(value);
        settings().frontlight_on = true;
        settings().frontlight_brightness = value;
        settings().save();
        return true;
      }
      case kLightToggle:
        power.set_frontlight_on(!power.frontlight_on());
        settings().frontlight_on = power.frontlight_on();
        settings().save();
        return true;
      case kNight: {
        Settings& s = settings();
        s.night_mode = !s.night_mode;
        Screen::instance().set_night_mode(s.night_mode);
        refresh_theme_from_settings(Screen::instance().dpi());
        s.save();
        App::instance().invalidate(Refresh::Flash);
        return true;
      }
      case kWifi:
        App::instance().pop();
        App::instance().push(make_network_screen());
        return true;
      case kShare:
        App::instance().pop();
        App::instance().push(make_usb_prompt_screen());
        return true;
      case kHome:
        App::instance().pop();
        App::instance().pop_to_root();
        return true;
      case kSettings:
        App::instance().pop();
        App::instance().push(make_settings_screen());
        return true;
      case kDismiss:
        App::instance().pop();
        return true;
      default:
        return false;
    }
  }

 private:
  HitList hits_;
};

}  // namespace

ViewPtr make_quick_panel() { return ViewPtr(new QuickPanel()); }

}  // namespace ck
