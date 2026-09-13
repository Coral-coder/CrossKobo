// Touch calibration that does not depend on touch being calibrated.
//
// Every other screen needs taps to land where they are drawn. This one
// works from the digitiser's own coordinates, so it can fix a panel that
// is mirrored or transposed even when nothing on screen can be hit: tap
// two marked corners, and the transform is derived from where those two
// raw points actually are. Reachable without touch at all - press both
// page-turn buttons together from any screen.
#include <algorithm>
#include <cstdlib>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/log.h"
#include "core/str.h"
#include "platform/input.h"
#include "platform/screen.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

class TouchWizard : public View {
 public:
  std::string title() const override { return "Touch calibration"; }
  Refresh refresh_hint() const override { return Refresh::Text; }
  // The raw points are what matter here; edge gestures would only get in
  // the way of a tap near a corner.
  bool edge_gestures() const override { return false; }
  const char* kind() const override { return "touch-wizard"; }

  void draw(Canvas& canvas, const Rect& bounds) override {
    const Theme& th = theme();
    paint_background(canvas);
    StatusBarInfo info;
    int top = draw_top_bar(canvas, bounds, "Touch calibration", info);

    Rect body(bounds.x + th.padding, bounds.y + top + th.padding, bounds.w - 2 * th.padding,
              bounds.h - top - 2 * th.padding);

    const char* instruction = "";
    switch (step_) {
      case 0:
        instruction =
            "Tap the circle at the TOP LEFT.\n\nYour taps do not have to land where you "
            "see them - that is what this screen is here to fix.";
        break;
      case 1:
        instruction = "Now the circle at the TOP RIGHT.";
        break;
      case 2:
        instruction = "And the circle at the BOTTOM LEFT.";
        break;
      default:
        instruction =
            "Tap anywhere to check: a dot should appear under your finger.\n\n"
            "Page-forward keeps this calibration, page-back starts again.";
        break;
    }
    std::vector<std::string> lines =
        wrap_text(instruction, ui_style(th.base_px, th.fg), body.w - 4 * th.padding);
    int line_h = text_height(ui_style(th.base_px, th.fg));
    int y = body.y + body.h / 2 - (int)lines.size() * line_h / 2;
    for (const std::string& line : lines) {
      draw_text_in(canvas, Rect(body.x + 2 * th.padding, y, body.w - 4 * th.padding, line_h),
                   line, ui_style(th.base_px, th.fg), 0);
      y += line_h;
    }

    // The targets, drawn where the tap is expected.
    int r = std::max(28, bounds.w / 18);
    int inset = r + th.padding;
    int left = bounds.x + inset, right = bounds.right() - inset;
    int upper = bounds.y + top + inset, lower = bounds.bottom() - inset;
    if (step_ == 0) draw_target(canvas, left, upper, r, true);
    if (step_ == 1) draw_target(canvas, right, upper, r, true);
    if (step_ == 2) draw_target(canvas, left, lower, r, true);
    if (step_ >= 3) {
      draw_target(canvas, left, upper, r / 2, false);
      draw_target(canvas, right, upper, r / 2, false);
      draw_target(canvas, left, lower, r / 2, false);
      if (check_x_ >= 0) {
        canvas.fill_circle(check_x_, check_y_, r / 2, theme().accent);
      }
      int bw = (body.w - th.padding) / 2;
      Rect again(body.x, body.bottom() - th.row_height, bw, th.row_height);
      Rect keep(body.right() - bw, again.y, bw, th.row_height);
      draw_button(canvas, again, "Start again", ButtonStyle::Normal);
      draw_button(canvas, keep, "Keep", ButtonStyle::Primary);
      hits_.clear();
      hits_.add(again, 1);
      hits_.add(keep, 2);
    }

    if (!note_.empty()) {
      draw_text_in(canvas, Rect(body.x, body.bottom() - th.row_height * 2, body.w,
                                th.small_px + 8),
                   note_, ui_style(th.small_px, th.muted), 0);
    }
  }

  bool handle(const InputEvent& event) override {
    if (event.type == EventType::KeyDown) {
      if (event.key == Key::PageBack) {
        restart();
        return true;
      }
      if (event.key == Key::PageForward) {
        if (step_ >= 3) {
          finish();
        } else {
          App::instance().pop();
        }
        return true;
      }
      return false;
    }
    if (event.type != EventType::Tap && event.type != EventType::TouchUp) return false;
    if (event.raw_x < 0 || event.raw_y < 0) {
      note_ = "That tap carried no raw coordinates; use the page buttons to leave.";
      return true;
    }
    if (step_ < 3) {
      raw_[step_][0] = event.raw_x;
      raw_[step_][1] = event.raw_y;
      note_ = format("read as %d,%d", event.raw_x, event.raw_y);
      if (++step_ < 3) return true;
      if (!derive()) {
        note_ = "Those taps were too close together. Start again at the top left.";
        step_ = 0;
      }
      return true;
    }
    // Verification: show where the tap landed under the new transform, and
    // let the buttons decide.
    int id = hits_.hit(event.x, event.y);
    if (id == 1) {
      restart();
      return true;
    }
    if (id == 2) {
      finish();
      return true;
    }
    check_x_ = event.x;
    check_y_ = event.y;
    return true;
  }

 private:
  static void draw_target(Canvas& canvas, int cx, int cy, int r, bool strong) {
    const Theme& th = theme();
    // A ring, drawn as two filled circles: the canvas has no circle
    // outline, and a ring is what reads as a target.
    canvas.fill_circle(cx, cy, r, th.accent);
    canvas.fill_circle(cx, cy, r - (strong ? 5 : 3), strong ? th.accent_soft : th.panel);
    canvas.fill_circle(cx, cy, std::max(3, r / 5), th.accent);
  }

  bool derive() {
    TouchTransform tf;
    if (!derive_touch_transform(raw_[0], raw_[1], raw_[2], tf)) return false;

    // From here the user's answer is the whole mapping; the orientation
    // guessed from the axis ranges steps aside.
    Settings& s = settings();
    s.touch_transform = tf;
    s.touch_calibrated = true;
    Input& input = Input::instance();
    input.set_touch_transform(tf);
    input.set_auto_transpose(false);
    note_ = format("swap %s, mirror x %s, mirror y %s", tf.swap_xy ? "on" : "off",
                   tf.mirror_x ? "on" : "off", tf.mirror_y ? "on" : "off");
    CK_LOGI("calibration: %d,%d / %d,%d / %d,%d gives %s", raw_[0][0], raw_[0][1], raw_[1][0],
            raw_[1][1], raw_[2][0], raw_[2][1], note_.c_str());
    check_x_ = -1;
    return true;
  }

  void restart() {
    step_ = 0;
    check_x_ = -1;
    note_.clear();
    // Put the old state back while the user tries again.
    Input::instance().set_touch_transform(saved_);
    Input::instance().set_auto_transpose(saved_auto_);
    settings().touch_transform = saved_;
    settings().touch_calibrated = saved_calibrated_;
    App::instance().invalidate(Refresh::Flash);
  }

  void finish() {
    settings().save();
    App::instance().show_toast("Touch calibration saved");
    App::instance().pop();
  }

  int step_ = 0;
  int raw_[3][2] = {{0, 0}, {0, 0}, {0, 0}};
  int check_x_ = -1, check_y_ = -1;
  std::string note_;
  TouchTransform saved_ = settings().touch_transform;
  bool saved_calibrated_ = settings().touch_calibrated;
  bool saved_auto_ = Input::instance().auto_transpose();
  HitList hits_;
};

}  // namespace

ViewPtr make_touch_wizard() { return ViewPtr(new TouchWizard()); }

}  // namespace ck
